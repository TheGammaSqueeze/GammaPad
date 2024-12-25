#include "gammapad_capture.h"
#include <sys/epoll.h>
#include <linux/input.h>
#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

/* We'll rely on the global 'controllerFd' declared in gammapad_main.c. */
extern int controllerFd;

/*
 * We'll store:
 *   - The identified driver path (e.g. "/sys/bus/platform/drivers/retrogame_joypad")
 *   - The real device name (e.g. "singleadc-joypad")
 *   - Whether we successfully identified them => gHasDriver=1
 */
static char g_driverPath[256];
static char g_deviceName[256];
static int  gHasDriver = 0; // Whether we identified a driver

/*
 * We'll store discovered scancodes for EV_KEY and EV_ABS, plus min/max.
 * 
 * NOTE: These arrays previously only handled a single device, 
 * but we want to merge multiple devices. We'll handle that logic
 * by calling discoverKeys/Axes for each new device, while respecting
 * the "first device wins" collisions.
 */
int g_discoveredKeys[KEY_MAX+1];
int g_discoveredAxes[ABS_MAX+1];
static int g_physicalAbsMin[ABS_MAX+1];
static int g_physicalAbsMax[ABS_MAX+1];

/*
 * scancode => final code if .kl says so; fallback is same scancode if not mapped.
 */
int g_keyMap[KEY_MAX+1];
int g_absMap[ABS_MAX+1];

/*
 * We'll also store the device path in g_physicalDevicePath
 * so we can remove it in the destructor if it's the primary device.
 */
static char g_physicalDevicePath[256];

/*
 * Accessors used by gammapad_controller.c => get raw min/max
 */
int getPhysicalAbsMin(int scancode)
{
    if (scancode < 0 || scancode > ABS_MAX) return -32768;
    return g_physicalAbsMin[scancode];
}

int getPhysicalAbsMax(int scancode)
{
    if (scancode < 0 || scancode > ABS_MAX) return 32767;
    return g_physicalAbsMax[scancode];
}

/****************************************************************************
 * readLinkFully:
 *   Runs "readlink -f <somePath>", capturing the fully resolved path into
 *   outBuf. If successful, outBuf might look like:
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
 *   an absolute path under /sys/... if needed.
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
 *   Then store that as outDriverPath => "/sys/bus/platform/drivers/retrogame_joypad"
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
    char* p = strstr(resolved, "/input/input");
    if (!p) return;
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
        // fallback => device/device/driver
        if (tryLsDriverPath(evBase, "device/", outDriverPath, dpSize) < 0) {
            return -1;
        }
    }

    /* Step 2: readlink -f => e.g. "/sys/class/input/<evBase>/device" */
    char deviceSymlink[512];
    snprintf(deviceSymlink, sizeof(deviceSymlink),
             "/sys/class/input/%s/device", evBase);

    char resolved[512];
    if (readLinkFully(deviceSymlink, resolved, sizeof(resolved)) < 0) {
        return -1;
    }

    climbUpIfInInputSubdir(resolved);

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
 * We'll handle collisions here so that scancodes from the first device 
 * overshadow scancodes from any subsequent devices if they try to claim 
 * the same scancode or final axis code. 
 * 
 * Because the user wants "the first device wins" approach, we add logic 
 * to skip discovered scancodes from secondary devices if they're already 
 * discovered by the primary device.
 *
 * We keep the same approach for triggers overshadowing normal axes, 
 * but we do it for multi-device.
 */

/* [ADDED for multi-dev aggregator]
   We'll define a function 'merge_discovered_keys_and_axes' that merges 
   scancodes from a new device into the global arrays g_discoveredKeys / g_discoveredAxes,
   respecting the "first device wins" collision logic.
*/
static void merge_discovered_keys(int newKeys[KEY_MAX+1], int newKeyMap[KEY_MAX+1])
{
    for (int sc=0; sc<=KEY_MAX; sc++){
        if (newKeys[sc]) {
            // If global array already discovered sc => skip
            if (g_discoveredKeys[sc]) {
                // The first device discovered it => do nothing
            } else {
                // Not discovered yet => adopt
                g_discoveredKeys[sc] = 1;
                g_keyMap[sc]         = newKeyMap[sc];
            }
        }
    }
}

static void merge_discovered_axes(int newAxes[ABS_MAX+1], int newAbsMap[ABS_MAX+1],
                                  int newAbsMin[ABS_MAX+1], int newAbsMax[ABS_MAX+1])
{
    for (int sc=0; sc<=ABS_MAX; sc++){
        if (newAxes[sc]) {
            if (g_discoveredAxes[sc]) {
                // Already discovered => skip
            } else {
                // Not discovered => adopt
                g_discoveredAxes[sc]   = 1;
                g_absMap[sc]           = newAbsMap[sc];
                g_physicalAbsMin[sc]   = newAbsMin[sc];
                g_physicalAbsMax[sc]   = newAbsMax[sc];
            }
        }
    }
}

/*
 * We'll do the original single-device approach inside discoverKeys/discoverAxes,
 * but for each new device, we'll store them in local arrays, then call 
 * merge_discovered_keys/axes to unify them globally. 
 */

/*
 * We'll keep the original single-device discover logic, but rename them 
 * to discoverSingleDeviceKeys/Axes, then a wrapper discoverKeys/Axes 
 * that merges them.
 */
static void discoverSingleDeviceKeys(int fd, int outKeys[KEY_MAX+1], int outKeyMap[KEY_MAX+1])
{
    unsigned long keyBits[(KEY_MAX+1)/(8*sizeof(long))];
    memset(keyBits, 0, sizeof(keyBits));
    memset(outKeys, 0, sizeof(int)*(KEY_MAX+1));

    for (int i=0; i<=KEY_MAX; i++){
        outKeyMap[i] = i; // fallback
    }

    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits) < 0) {
        fprintf(stderr, "[GammaPadCapture] discoverSingleDeviceKeys: EVIOCGBIT(EV_KEY) => %s\n",
                strerror(errno));
        return;
    }
    int countFound=0;
    for (int code=0; code<=KEY_MAX; code++){
        int bitSet = (keyBits[code/(8*sizeof(long))] >> (code%(8*sizeof(long)))) & 1;
        if (bitSet) {
            outKeys[code] = 1;
            countFound++;
        }
    }
    fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceKeys => found %d key scancodes.\n", countFound);
}

/* same for axes */
static void discoverSingleDeviceAxes(int fd, int outAxes[ABS_MAX+1], int outAbsMap[ABS_MAX+1],
                                     int outAbsMin[ABS_MAX+1], int outAbsMax[ABS_MAX+1])
{
    unsigned long absBits[(ABS_MAX+1)/(8*sizeof(long))];
    memset(absBits, 0, sizeof(absBits));
    memset(outAxes, 0, sizeof(int)*(ABS_MAX+1));

    for (int i=0; i<=ABS_MAX; i++){
        outAbsMap[i] = i; // fallback
        outAbsMin[i] = -32768;
        outAbsMax[i] = 32767;
    }

    if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits) < 0) {
        fprintf(stderr, "[GammaPadCapture] discoverSingleDeviceAxes: EVIOCGBIT(EV_ABS) => %s\n",
                strerror(errno));
        return;
    }
    int countFound=0;
    for (int code=0; code<=ABS_MAX; code++){
        int bitSet = (absBits[code/(8*sizeof(long))] >> (code % (8*sizeof(long)))) & 1;
        if (!bitSet) {
            continue;
        }
        outAxes[code] = 1;
        countFound++;
        struct input_absinfo info;
        if (ioctl(fd, EVIOCGABS(code), &info) == 0) {
            outAbsMin[code] = info.minimum;
            outAbsMax[code] = info.maximum;
            fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceAxes: scancode=%d => min=%d, max=%d\n",
                code, info.minimum, info.maximum);
        } else {
            fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceAxes: EVIOCGABS(%d) => fail %s\n",
                code, strerror(errno));
        }
    }
    fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceAxes => found %d axis scancodes.\n", countFound);
}

/*
 * We'll keep parse_android_keylayout_file_if_needed and parseKeyLayoutLine 
 * the same, but rename discoverKeys, discoverAxes to reflect 
 * the aggregator approach.
 */

/* We keep the same #ifdef logic, no changes. */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd)
{
    fprintf(stderr, "[GammaPadCapture] parse_android_keylayout_file_if_needed: Attempting .kl parse...\n");
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) == 0) {
        fprintf(stderr,"[GammaPadCapture] Vendor=0x%04x Product=0x%04x\n",
                id.vendor, id.product);

        char klPath[256];
        snprintf(klPath, sizeof(klPath),
            "/system/usr/keylayout/Vendor_%04x_Product_%04x.kl",
            id.vendor, id.product);

        FILE* f = fopen(klPath, "r");
        if (!f) {
            fprintf(stderr,"[KL] No .kl found at %s\n", klPath);
            return;
        }
        fprintf(stderr,"[KL] Found .kl => %s, parsing...\n", klPath);

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line,'\n');
            if (nl) *nl=0;
            parseKeyLayoutLine(line);
        }
        fclose(f);
    }
}
#else
void parse_android_keylayout_file_if_needed(int fd)
{
    (void)fd;
}
#endif

/*
 * parseKeyLayoutLine => unchanged
 */
void parseKeyLayoutLine(const char* line)
{
    char type[32], sCode[32], name[32], rest[128];
    memset(type,0,sizeof(type));
    memset(sCode,0,sizeof(sCode));
    memset(name,0,sizeof(name));
    memset(rest,0,sizeof(rest));

    int parts = sscanf(line, "%31s %31s %31s %127[^\n]", type, sCode, name, rest);
    if (parts < 3) {
        return;
    }
    if (!strcasecmp(type, "key")) {
        int scancode = 0;
        if (!strncasecmp(sCode,"0x",2)) {
            scancode = (int)strtol(sCode,NULL,16);
        } else {
            scancode = atoi(sCode);
        }
        if (scancode>=0 && scancode<=KEY_MAX) {
            if (!strcasecmp(name,"BUTTON_A"))      g_keyMap[scancode]= BTN_A;
            else if (!strcasecmp(name,"BUTTON_B")) g_keyMap[scancode]= BTN_B;
            else if (!strcasecmp(name,"BUTTON_X")) g_keyMap[scancode]= BTN_X;
            else if (!strcasecmp(name,"BUTTON_Y")) g_keyMap[scancode]= BTN_Y;
            /* fallback => scancode=>scancode if not recognized */

            fprintf(stderr,"[KL] 'key %s %s' => scancode=%d => final=%d\n",
                sCode,name,scancode,g_keyMap[scancode]);
        }
    }
    else if (!strcasecmp(type, "axis")) {
        int scancode=0;
        if (!strncasecmp(sCode,"0x",2)) {
            scancode= (int)strtol(sCode,NULL,16);
        } else {
            scancode= atoi(sCode);
        }
        if (scancode>=0 && scancode<=ABS_MAX) {
            if (!strcasecmp(name,"X"))         g_absMap[scancode] = ABS_X;
            else if (!strcasecmp(name,"Y"))    g_absMap[scancode] = ABS_Y;
            else if (!strcasecmp(name,"Z"))    g_absMap[scancode] = ABS_Z;
            else if (!strcasecmp(name,"RZ"))   g_absMap[scancode] = ABS_RZ;
            else if (!strcasecmp(name,"LTRIGGER")) g_absMap[scancode] = ABS_BRAKE;  // no swap
            else if (!strcasecmp(name,"RTRIGGER")) g_absMap[scancode] = ABS_GAS;    // no swap
            else if (!strcasecmp(name,"HAT_X"))     g_absMap[scancode] = ABS_HAT0X;
            else if (!strcasecmp(name,"HAT_Y"))     g_absMap[scancode] = ABS_HAT0Y;
            /* fallback => scancode => scancode */

            fprintf(stderr,"[KL] 'axis %s %s' => scancode=%d => finalAbs=%d\n",
                sCode,name,scancode,g_absMap[scancode]);
        }
    }
}

/*
 * discoverKeys => aggregator that calls discoverSingleDeviceKeys, merges in.
 */
void discoverKeys(int fd)
{
    int singleKeys[KEY_MAX+1];
    int singleKeyMap[KEY_MAX+1];
    discoverSingleDeviceKeys(fd, singleKeys, singleKeyMap);

    // Merge them
    merge_discovered_keys(singleKeys, singleKeyMap);
}

/*
 * discoverAxes => aggregator that calls discoverSingleDeviceAxes, merges in.
 */
void discoverAxes(int fd)
{
    int singleAxes[ABS_MAX+1];
    int singleAbsMap[ABS_MAX+1];
    int singleAbsMin[ABS_MAX+1];
    int singleAbsMax[ABS_MAX+1];

    discoverSingleDeviceAxes(fd, singleAxes, singleAbsMap, singleAbsMin, singleAbsMax);

    // Merge them
    merge_discovered_axes(singleAxes, singleAbsMap, singleAbsMin, singleAbsMax);
}

/*
 * open_physical_device => 
 *   1) parse the driver path & device name from sysfs
 *   2) open + grab the device
 *   3) parse .kl + discover scancodes (merging them if we already had from another device)
 *   4) store the path => so destructor can remove it at exit (only if first device)
 *
 * For multi-device logic, we add a param "isPrimary" to decide if we remove node or not,
 * but to keep the user’s original function signature, we store an internal static array 
 * or use an approach from gammapad_main. 
 *
 * For minimal intrusion, we keep the function signature but assume the FIRST TIME 
 * we call it is primary; subsequent times are secondary.
 */

// [ADDED for multi-dev]
static int g_hasPrimaryDevice = 0;

int open_physical_device(const char* device_path)
{
    if (!device_path) return -1;

    int fd = open(device_path, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[GammaPadCapture] Failed to open %s: %s\n",
                device_path, strerror(errno));
        return -1;
    }

    // Check if we have a primary device yet
    int isPrimary = 0;
    if (!g_hasPrimaryDevice) {
        // Mark this one as primary
        isPrimary = 1;
        g_hasPrimaryDevice = 1;
    }

    // If it's primary => parse out driver path
    if (isPrimary) {
        if (identifyDriverAndDevice(
                device_path,
                g_driverPath, sizeof(g_driverPath),
                g_deviceName, sizeof(g_deviceName))==0)
        {
            gHasDriver = 1;
        } else {
            fprintf(stderr,
                "[GammaPadCapture] Could not identify driver/device from sysfs for '%s', skipping unbind.\n",
                device_path);
            gHasDriver = 0;
        }

        memset(g_physicalDevicePath,0,sizeof(g_physicalDevicePath));
        strncpy(g_physicalDevicePath, device_path, sizeof(g_physicalDevicePath)-1);
    }

#ifdef __ANDROID__
    parse_android_keylayout_file_if_needed(fd);
#endif

    // aggregator discover
    discoverKeys(fd);
    discoverAxes(fd);
    // collision resolution is done in gammapad_controller.c => not needed here.

    if (ioctl(fd, EVIOCGRAB, 1) < 0) {
        fprintf(stderr, "[GammaPadCapture] EVIOCGRAB on %s failed: %s\n",
                device_path, strerror(errno));
    }

    fprintf(stderr,"[GammaPadCapture] open_physical_device => '%s' (fd=%d). isPrimary=%d\n",
        device_path, fd, isPrimary);
    return fd;
}

/*
 * forward_physical_event:
 *   Forwards EV_KEY/EV_ABS to the global 'controllerFd'.
 *   We do scancode => final code transform if .kl says so.
 */
void forward_physical_event(const struct input_event* ev)
{
    if (!ev) return;
    if (controllerFd < 0) return;

    if (ev->type == EV_KEY) {
        int orig = ev->code;
        if (orig < 0 || orig > KEY_MAX) return;
        int mapped = g_keyMap[orig];

        fprintf(stderr,"[FWD] KEY scancode=%d => final=%d, value=%d\n",
            orig, mapped, ev->value);

        struct input_event out[2];
        memset(&out, 0, sizeof(out));

        out[0].type  = EV_KEY;
        out[0].code  = mapped;
        out[0].value = ev->value;
        out[1].type  = EV_SYN;
        out[1].code  = SYN_REPORT;
        out[1].value = 0;
        write(controllerFd, &out, sizeof(out));
    }
    else if (ev->type == EV_ABS) {
        int orig = ev->code;
        if (orig < 0 || orig > ABS_MAX) return;
        int mapped = g_absMap[orig];

        fprintf(stderr,"[FWD] ABS scancode=%d => final=%d, value=%d\n",
            orig, mapped, ev->value);

        if (mapped < 0) {
            // pruned from collision => do nothing
            return;
        }

        struct input_event out[2];
        memset(&out, 0, sizeof(out));

        out[0].type  = EV_ABS;
        out[0].code  = mapped;
        out[0].value = ev->value;
        out[1].type  = EV_SYN;
        out[1].code  = SYN_REPORT;
        out[1].value = 0;
        write(controllerFd, &out, sizeof(out));
    }
}

/*
 * destructor => remove node + unbind/rebind at program exit if isPrimary device
 */
__attribute__((destructor))
static void onFinish(void)
{
    fprintf(stderr, "[GammaPadCapture] onFinish() => removing node + unbind/rebind for primary device.\n");

    // If we had a primary device
    if (g_physicalDevicePath[0]) {
        char rmCmd[300];
        snprintf(rmCmd, sizeof(rmCmd), "rm -f '%s'", g_physicalDevicePath);
        fprintf(stderr, "[GammaPadCapture] destructor => remove node => %s\n", rmCmd);
        system(rmCmd);
    }

    unbindAndRebind();
}
