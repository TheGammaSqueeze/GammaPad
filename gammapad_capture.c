#include "gammapad_capture.h"
#include <sys/epoll.h>
#include <linux/input.h>
#include <errno.h>
#include <ctype.h>

/* We'll rely on the global 'controllerFd' declared in gammapad_main.c. */
extern int controllerFd;

/*
 * We'll store:
 *   - The identified driver path (e.g. "/sys/bus/platform/drivers/retrogame_joypad")
 *   - The real device name (e.g. "singleadc-joypad")
 */
static char g_driverPath[256];
static char g_deviceName[256];
static int gHasDriver = 0; // Whether we successfully identified driverPath/deviceName

/****************************************************************************
 * readLinkFully:
 *   Runs "readlink -f <somePath>", capturing the fully resolved path into
 *   outBuf.  If successful, outBuf might look like:
 *       "/sys/devices/platform/singleadc-joypad/input/input183"
 * Return 0 on success, -1 on failure.
 ****************************************************************************/
static int readLinkFully(const char* path, char* outBuf, size_t outSize)
{
    if (!path || !outBuf || outSize < 2) return -1;

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "readlink -f '%s' 2>/dev/null", path);

    FILE* fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    // strip newline
    char* nl = strchr(line, '\n');
    if (nl) *nl = 0;

    if (!line[0]) return -1;
    snprintf(outBuf, outSize, "%s", line);
    return 0;
}

/****************************************************************************
 * skipDotSlashes:
 *   Helper to skip leading "../" or "./" from a path so we can build
 *   absolute path under /sys/... if needed.
 ****************************************************************************/
static const char* skipDotSlashes(const char* str)
{
    while (str[0] == '.') {
        if (str[1] == '/') {
            str += 2; // skip "./"
        } else if (str[1] == '.' && str[2] == '/') {
            str += 3; // skip "../"
        } else {
            break;
        }
    }
    while (*str == '/') {
        ++str; // skip any leading slashes
    }
    return str;
}

/****************************************************************************
 * tryLsDriverPath:
 *   We'll do "ls -l /sys/class/input/<evName>/device/<subdir>driver"
 *   to parse if it points to e.g. "../../bus/platform/drivers/retrogame_joypad".
 *   Then store that as outDriverPath: "/sys/bus/platform/drivers/retrogame_joypad"
 ****************************************************************************/
static int tryLsDriverPath(const char* evName,
                           const char* subdir,
                           char* outDriverPath, size_t dpSize)
{
    if (!evName || !outDriverPath) return -1;

    char driverLink[512];
    snprintf(driverLink, sizeof(driverLink),
             "/sys/class/input/%s/device/%sdriver", evName, subdir);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ls -l '%s' 2>/dev/null", driverLink);

    FILE* fp = popen(cmd, "r");
    if (!fp) return -1;

    char line[1024];
    if (!fgets(line, sizeof(line), fp)) {
        pclose(fp);
        return -1;
    }
    pclose(fp);

    /* Example:
       "lrwxrwxrwx 1 root root 0 2023-12-01 12:00 driver -> ../../bus/platform/drivers/retrogame_joypad"
    */
    const char* arrow = strstr(line, "-> ");
    if (!arrow) return -1;
    arrow += 3; // skip "-> "

    const char* pathPart = skipDotSlashes(arrow);
    // e.g. "bus/platform/drivers/retrogame_joypad"
    snprintf(outDriverPath, dpSize, "/sys/%s", pathPart);

    char* nl = strchr(outDriverPath, '\n');
    if (nl) *nl = 0;

    return 0;
}

/****************************************************************************
 * climbUpIfInInputSubdir:
 *   If resolved is something like:
 *       "/sys/devices/platform/singleadc-joypad/input/input183"
 *   and we see that it ends with "input/input<number>", we'll cut that
 *   portion so that we revert to "/sys/devices/platform/singleadc-joypad".
 ****************************************************************************/
static void climbUpIfInInputSubdir(char* resolved)
{
    if (!resolved || !resolved[0]) return;
    /* find "/input/input" in the path if it exists */
    char* p = strstr(resolved, "/input/input");
    if (!p) return;

    /* 
     * p might point to the first slash in "/input/input183".
     * We'll cut *before* that slash, effectively removing "/input/input183".
     *
     * e.g.
     *   resolved = "/sys/devices/platform/singleadc-joypad/input/input183"
     *   p = ".../singleadc-joypad/input/input183" 
     * We'll do *p = '\0', so resolved => "/sys/devices/platform/singleadc-joypad"
     */
    *p = '\0';
}

/****************************************************************************
 * identifyDriverAndDevice:
 *   eventNode might be "/dev/input/event4" => evBase = "event4"
 *   1) We parse out the driver path from device/driver or device/device/driver
 *   2) We readlink -f /sys/class/input/<evBase>/device => e.g.
 *       "/sys/devices/platform/singleadc-joypad/input/input183"
 *      Then we detect the "/input/input" suffix -> cut it => 
 *       "/sys/devices/platform/singleadc-joypad"
 *      Then parse final slash => "singleadc-joypad"
 ****************************************************************************/
static int identifyDriverAndDevice(const char* eventNode,
                                   char* outDriverPath, size_t dpSize,
                                   char* outDeviceName, size_t dnSize)
{
    if (!eventNode || !outDriverPath || !outDeviceName) return -1;

    const char* evBase = strrchr(eventNode, '/');
    if (!evBase) evBase = eventNode; 
    else evBase++;

    /* Step 1: Try to get the driver path. */
    if (tryLsDriverPath(evBase, "", outDriverPath, dpSize) < 0) {
        // fallback: device/device/driver
        if (tryLsDriverPath(evBase, "device/", outDriverPath, dpSize) < 0) {
            // none
            return -1;
        }
    }

    /* Step 2: readlink -f /sys/class/input/<evBase>/device => e.g. /sys/devices/platform/singleadc-joypad/input/input183 */
    char deviceSymlink[512];
    snprintf(deviceSymlink, sizeof(deviceSymlink),
             "/sys/class/input/%s/device", evBase);

    char resolved[512];
    if (readLinkFully(deviceSymlink, resolved, sizeof(resolved)) < 0) {
        return -1;
    }

    /* 
     * Possibly it's "/sys/devices/platform/singleadc-joypad/input/input183".
     * We'll cut off the "/input/input183" portion if present.
     */
    climbUpIfInInputSubdir(resolved); 
    // Now resolved might be "/sys/devices/platform/singleadc-joypad".

    // parse final slash => outDeviceName
    const char* lastSlash = strrchr(resolved, '/');
    if (!lastSlash) return -1;
    lastSlash++;
    if (!*lastSlash) return -1;

    snprintf(outDeviceName, dnSize, "%s", lastSlash);

    fprintf(stderr,
        "[GammaPadCapture] driverPath='%s', deviceName='%s'\n",
        outDriverPath, outDeviceName);

    return 0;
}

/*
 * We'll do unbind 3 times, then bind 3 times, each step 1 second apart,
 * writing directly to /sys/bus/platform/drivers/<driver>/unbind etc.
 */
static void unbindAndRebind(void)
{
    if (!gHasDriver) {
        fprintf(stderr, "[GammaPadCapture] No valid driver to unbind.\n");
        return;
    }
    if (!g_driverPath[0] || !g_deviceName[0]) {
        fprintf(stderr, "[GammaPadCapture] Missing driver/device for unbind.\n");
        return;
    }

    fprintf(stderr,
        "[GammaPadCapture] We'll unbind 3 times, then bind 3 times, each step 1 sec apart.\n"
        " driverPath='%s', deviceName='%s'\n",
        g_driverPath, g_deviceName);

    // 1) Unbind 3 times
    for (int i = 1; i <= 3; i++) {
        char unbindPath[512];
        snprintf(unbindPath, sizeof(unbindPath), "%s/unbind", g_driverPath);

        fprintf(stderr, "[GammaPadCapture] [unbind #%d] Opening '%s' for write.\n", i, unbindPath);

        FILE* fUnbind = fopen(unbindPath, "w");
        if (!fUnbind) {
            fprintf(stderr, "[GammaPadCapture] [unbind #%d] Failed to open '%s': %s\n",
                    i, unbindPath, strerror(errno));
        } else {
            fprintf(stderr, "[GammaPadCapture] [unbind #%d] Writing '%s'...\n", i, g_deviceName);
            fprintf(fUnbind, "%s\n", g_deviceName);
            fclose(fUnbind);
        }
        sleep(1);
    }

    // 2) Bind 3 times
    for (int i = 1; i <= 3; i++) {
        char bindPath[512];
        snprintf(bindPath, sizeof(bindPath), "%s/bind", g_driverPath);

        fprintf(stderr, "[GammaPadCapture] [bind #%d] Opening '%s' for write.\n", i, bindPath);

        FILE* fBind = fopen(bindPath, "w");
        if (!fBind) {
            fprintf(stderr, "[GammaPadCapture] [bind #%d] Failed to open '%s': %s\n",
                    i, bindPath, strerror(errno));
        } else {
            fprintf(stderr, "[GammaPadCapture] [bind #%d] Writing '%s'...\n", i, g_deviceName);
            fprintf(fBind, "%s\n", g_deviceName);
            fclose(fBind);
        }
        sleep(1);
    }

    fprintf(stderr, "[GammaPadCapture] Done unbind/rebind cycles.\n");
}

/*
 * open_physical_device:
 *   1) parse the driver path & device name
 *   2) open + grab the device
 *   3) remove the node
 */
int open_physical_device(const char* device_path)
{
    if (!device_path) return -1;

    /* Attempt to identify the driver path and device name from sysfs. */
    int rc = identifyDriverAndDevice(device_path,
                                     g_driverPath, sizeof(g_driverPath),
                                     g_deviceName, sizeof(g_deviceName));
    if (rc == 0) {
        gHasDriver = 1;
    } else {
        fprintf(stderr,
            "[GammaPadCapture] Could not identify driver/device from sysfs for '%s', skipping unbind.\n",
            device_path);
        gHasDriver = 0;
    }

    /* Attempt to open the device. */
    int fd = open(device_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[GammaPadCapture] Failed to open %s: %s\n",
                device_path, strerror(errno));
        return -1;
    }

    /* Attempt to EVIOCGRAB. Even if it fails, we continue. */
    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "[GammaPadCapture] EVIOCGRAB on %s failed: %s\n",
                device_path, strerror(errno));
    }

    /* Remove the node from /dev/input/... so nothing else sees it. */
    char rmCmd[300];
    snprintf(rmCmd, sizeof(rmCmd), "rm -f '%s'", device_path);
    fprintf(stderr, "[GammaPadCapture] Removing node with: %s\n", rmCmd);
    system(rmCmd);
    fprintf(stderr, "[GammaPadCapture] Removed node: %s\n", device_path);

    return fd;
}

/*
 * forward_physical_event:
 *   Forwards EV_KEY/EV_ABS to the global 'controllerFd'.
 */
void forward_physical_event(const struct input_event* ev)
{
    if (!ev) return;
    if (ev->type == EV_KEY || ev->type == EV_ABS) {
        if (controllerFd < 0) return;
        struct input_event out[2];
        memset(&out, 0, sizeof(out));

        out[0].type  = ev->type;
        out[0].code  = ev->code;
        out[0].value = ev->value;
        out[1].type  = EV_SYN;
        out[1].code  = SYN_REPORT;
        out[1].value = 0;

        write(controllerFd, &out, sizeof(out));
    }
}

/*
 * We'll call unbindAndRebind() automatically at program exit.
 */
__attribute__((destructor))
static void onFinish(void)
{
    fprintf(stderr, "[GammaPadCapture] onFinish() => unbind/rebind attempt.\n");
    unbindAndRebind();
}
