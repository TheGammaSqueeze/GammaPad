/*****************************************************
 * gammapad_capture.c
 *
 * Dynamically captures inputs from multiple physical
 * controllers, merges their scancodes into global
 * aggregator arrays, resolves collisions (e.g. triggers
 * overshadow normal axes), parses .kl for each device,
 * then forwards events to the single virtual controller.
 *
 * We remove /dev/input/event* of the primary device
 * after the virtual pad is fully created (not in the
 * destructor). The destructor only does unbind x3 +
 * bind x3 at exit (but we also provide unbindAndRebind()
 * as a public function if main wants to call it directly
 * on exit).
 *****************************************************/

#include "gammapad_capture.h"
#include "gammapad_calibration.h"
#include "gammapad_config.h"
#include "gammapad.h"
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
 *   - The identified driver path for the primary device
 *   - The real device name for the primary device
 *   - Whether we identified them => gHasDriver=1
 */
static char g_driverPath[256];
static char g_deviceName[256];
static int  gHasDriver = 0; // Whether we identified a valid driver for the primary

/*
 * We'll store discovered scancodes for EV_KEY and EV_ABS from *all*
 * opened physical devices, plus min/max for axes. aggregator approach.
 */
int g_discoveredKeys[KEY_MAX+1];
int g_discoveredAxes[ABS_MAX+1];
static int g_physicalAbsMin[ABS_MAX+1];
static int g_physicalAbsMax[ABS_MAX+1];

/*
 * scancode => final code if .kl says so; fallback => same scancode if not mapped.
 * aggregator approach with collisions favoring the primary device.
 */
int g_keyMap[KEY_MAX+1];
int g_absMap[ABS_MAX+1];

/*
 * We'll store the path of the primary device so we can remove it
 * after the virtual pad is up, and do unbind/bind at exit.
 */
static char g_physicalDevicePath[256];

/*
 * Whether we've assigned a "primary" device yet. The first non-`--ffdev`
 * device is considered primary => special handling.
 */
static int g_hasPrimaryDevice = 0;

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

/* Circular deadzone support: track last processed ABS values */
static int last_processed_abs[ABS_MAX+1] = {0};

/*****************************************************************************
 * readLinkFully => "readlink -f <somePath>"
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

/*****************************************************************************
 * skipDotSlashes => skip leading "../" or "./"
 ****************************************************************************/
static const char* skipDotSlashes(const char* str)
{
    while (str[0] == '.') {
        if (str[1] == '/') {
            str += 2; 
        } else if (str[1] == '.' && str[2] == '/') {
            str += 3; 
        } else {
            break;
        }
    }
    while (*str == '/') {
        ++str;
    }
    return str;
}

/*****************************************************************************
 * tryLsDriverPath => "ls -l /sys/class/input/<evBase>/device/<subdir>driver"
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

    const char* arrow = strstr(line, "-> ");
    if (!arrow) return -1;
    arrow += 3; // skip "-> "

    const char* pathPart = skipDotSlashes(arrow);
    snprintf(outDriverPath, dpSize, "/sys/%s", pathPart);

    char* nl = strchr(outDriverPath, '\n');
    if (nl) *nl = 0;

    return 0;
}

/*****************************************************************************
 * climbUpIfInInputSubdir => e.g. cut "/input/inputNN" suffix
 ****************************************************************************/
static void climbUpIfInInputSubdir(char* resolved)
{
    if (!resolved || !resolved[0]) return;
    char* p = strstr(resolved, "/input/input");
    if (!p) return;
    *p = '\0';
}

/*****************************************************************************
 * identifyDriverAndDevice => parse the driver path + device name from sysfs
 ****************************************************************************/
static int identifyDriverAndDevice(const char* eventNode,
                                   char* outDriverPath, size_t dpSize,
                                   char* outDeviceName, size_t dnSize)
{
    if (!eventNode || !outDriverPath || !outDeviceName) return -1;

    const char* evBase = strrchr(eventNode, '/');
    if (!evBase) evBase = eventNode;
    else evBase++;

    // 1) try <evBase>/device/driver
    if (tryLsDriverPath(evBase, "", outDriverPath, dpSize) < 0) {
        // fallback => device/device/driver
        if (tryLsDriverPath(evBase, "device/", outDriverPath, dpSize) < 0) {
            return -1;
        }
    }

    // 2) readlink -f => e.g. "/sys/class/input/<evBase>/device"
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
 * unbindAndRebind => do unbind x3 + bind x3 for the primary device
 */
void unbindAndRebind(void)
{
    if (g_noSourceRebind) {
        fprintf(stderr,"[GammaPadCapture] --no-source-rebind: skipping unbind/rebind.\n");
        return;
    }

    if(!gHasDriver){
        fprintf(stderr,"[GammaPadCapture] No valid driver => skip unbind.\n");
        return;
    }
    if(!g_driverPath[0] || !g_deviceName[0]){
        fprintf(stderr,"[GammaPadCapture] Missing driver/device => skip unbind.\n");
        return;
    }

    fprintf(stderr,"[GammaPadCapture] We'll unbind 3x + bind 3x for driver.\n");
    // unbind x3
    for(int i=1;i<=3;i++){
        char unbindPath[512];
        snprintf(unbindPath,sizeof(unbindPath),"%s/unbind", g_driverPath);
        FILE* fUnbind= fopen(unbindPath,"w");
        if(!fUnbind){
            fprintf(stderr,"[GammaPadCapture] unbind #%d => open fail => %s\n", i,strerror(errno));
        } else {
            fprintf(stderr,"[GammaPadCapture] unbind #%d => writing '%s'\n",i,g_deviceName);
            fprintf(fUnbind,"%s\n",g_deviceName);
            fclose(fUnbind);
        }
        sleep(1);
    }
    // bind x3
    for(int i=1;i<=3;i++){
        char bindPath[512];
        snprintf(bindPath,sizeof(bindPath),"%s/bind", g_driverPath);
        FILE* fBind= fopen(bindPath,"w");
        if(!fBind){
            fprintf(stderr,"[GammaPadCapture] bind #%d => open fail => %s\n", i,strerror(errno));
        } else {
            fprintf(stderr,"[GammaPadCapture] bind #%d => writing '%s'\n",i,g_deviceName);
            fprintf(fBind,"%s\n", g_deviceName);
            fclose(fBind);
        }
        sleep(1);
    }
    fprintf(stderr,"[GammaPadCapture] done unbind/rebind cycles.\n");
}

/*
 * unbindPrimaryDriver:
 *   Write g_deviceName into <driverPath>/unbind three times.
 */
void unbindPrimaryDriver(void)
{
    if (g_noSourceRebind) {
        fprintf(stderr, "[GammaPadCapture] --no-source-rebind: skipping unbindPrimaryDriver.\n");
        return;
    }

    if (!gHasDriver || !g_driverPath[0] || !g_deviceName[0]) {
        fprintf(stderr, "[GammaPadCapture] unbindPrimaryDriver: no driver info, skipping.\n");
        return;
    }
    fprintf(stderr, "[GammaPadCapture] unbindPrimaryDriver => unbinding driver 3×.\n");
    for (int i = 1; i <= 3; i++) {
        char unbindPath[512];
        snprintf(unbindPath, sizeof(unbindPath), "%s/unbind", g_driverPath);
        FILE* f = fopen(unbindPath, "w");
        if (!f) {
            fprintf(stderr, "[GammaPadCapture] unbind #%d => open fail => %s\n", i, strerror(errno));
        } else {
            fprintf(stderr, "[GammaPadCapture] unbind #%d => writing '%s'\n", i, g_deviceName);
            fprintf(f, "%s\n", g_deviceName);
            fclose(f);
        }
        sleep(1);
    }
}

/*
 * bindPrimaryDriver:
 *   Write g_deviceName into <driverPath>/bind three times.
 */
void bindPrimaryDriver(void)
{
    if (g_noSourceRebind) {
        fprintf(stderr, "[GammaPadCapture] --no-source-rebind: skipping bindPrimaryDriver.\n");
        return;
    }

    if (!gHasDriver || !g_driverPath[0] || !g_deviceName[0]) {
        fprintf(stderr, "[GammaPadCapture] bindPrimaryDriver: no driver info, skipping.\n");
        return;
    }
    fprintf(stderr, "[GammaPadCapture] bindPrimaryDriver => binding driver 3×.\n");
    for (int i = 1; i <= 3; i++) {
        char bindPath[512];
        snprintf(bindPath, sizeof(bindPath), "%s/bind", g_driverPath);
        FILE* f = fopen(bindPath, "w");
        if (!f) {
            fprintf(stderr, "[GammaPadCapture] bind #%d => open fail => %s\n", i, strerror(errno));
        } else {
            fprintf(stderr, "[GammaPadCapture] bind #%d => writing '%s'\n", i, g_deviceName);
            fprintf(f, "%s\n", g_deviceName);
            fclose(f);
        }
        sleep(1);
    }
}

/*
 * We'll do collision resolution so triggers overshadow normal axes,
 * or bigger-range scancodes overshadow smaller-range if mapped to same axis.
 */
static void resolveAxisCollisions(void)
{
    struct {
        int scancode;
        int range;
    } finalUsed[ABS_MAX+1];
    memset(finalUsed,0,sizeof(finalUsed));
    for(int i=0;i<=ABS_MAX;i++){
        finalUsed[i].scancode= -1;
        finalUsed[i].range=0;
    }

    for(int sc=0; sc<=ABS_MAX; sc++){
        if(!g_discoveredAxes[sc]) continue;
        int finalAxis= g_absMap[sc];
        if(finalAxis<0 || finalAxis>ABS_MAX) continue;

        int range= g_physicalAbsMax[sc] - g_physicalAbsMin[sc];
        if(range<0) range= -range;

        if(finalUsed[finalAxis].scancode<0){
            finalUsed[finalAxis].scancode= sc;
            finalUsed[finalAxis].range= range;
        } else {
            int oldSc= finalUsed[finalAxis].scancode;
            int oldRange= finalUsed[finalAxis].range;

            /* define "trigger" scancodes => sc=2 or sc=5 overshadow normal axes */
            int isNewTrigger= ((sc==2)||(sc==5));
            int isOldTrigger= ((oldSc==2)||(oldSc==5));

            if(!isOldTrigger && isNewTrigger){
                finalUsed[finalAxis].scancode= sc;
                finalUsed[finalAxis].range= range;
                g_discoveredAxes[oldSc]=0;
                g_absMap[oldSc]= -1;
                fprintf(stderr,"[Collision] finalAxis=%d oldSc=%d overshadowed by new trigger sc=%d\n",
                        finalAxis,oldSc,sc);
            }
            else if(isOldTrigger && !isNewTrigger){
                g_discoveredAxes[sc]=0;
                g_absMap[sc]= -1;
                fprintf(stderr,"[Collision] finalAxis=%d sc=%d overshadowed by old trigger sc=%d\n",
                        finalAxis,sc,oldSc);
            }
            else {
                // both triggers or both not => bigger range wins
                if(range>oldRange){
                    finalUsed[finalAxis].scancode= sc;
                    finalUsed[finalAxis].range= range;
                    g_discoveredAxes[oldSc]=0;
                    g_absMap[oldSc]= -1;
                    fprintf(stderr,"[Collision] finalAxis=%d oldSc=%d replaced by sc=%d(bigger)\n",
                            finalAxis, oldSc, sc);
                } else {
                    g_discoveredAxes[sc]=0;
                    g_absMap[sc]= -1;
                    fprintf(stderr,"[Collision] finalAxis=%d sc=%d overshadowed by oldSc=%d(bigger)\n",
                            finalAxis, sc, oldSc);
                }
            }
        }
    }
}

/*
 * aggregator approach => merges discovered scancodes from each device
 * For keys => if the scancode is *not* discovered, adopt it. If it
 * *is* discovered, we only override if the new device is primary
 * and the old one wasn't.
 */
static void mergeKeysIntoGlobal(const int inKeys[KEY_MAX+1], int isPrimary)
{
    for(int sc=0; sc<=KEY_MAX; sc++){
        if(inKeys[sc]){
            if(!g_discoveredKeys[sc]){
                // not discovered => adopt
                g_discoveredKeys[sc]=1;
            } else {
                // discovered => if *this* device is primary => we might
                // let its .kl overshadow
                if(isPrimary){
                    // do nothing special here, parseKeyLayoutLine might set g_keyMap
                }
            }
        }
    }
}

/*
 * aggregator => merges axis scancodes
 */
static void mergeAxesIntoGlobal(const int inAxes[ABS_MAX+1],
                                const int inMin[ABS_MAX+1],
                                const int inMax[ABS_MAX+1])
{
    for(int sc=0; sc<=ABS_MAX; sc++){
        if(inAxes[sc]){
            g_discoveredAxes[sc]=1;
            g_physicalAbsMin[sc]= inMin[sc];
            g_physicalAbsMax[sc]= inMax[sc];
        }
    }
}

/*
 * discoverSingleDeviceKeys => gather from EVIOCGBIT(EV_KEY,...)
 */
static void discoverSingleDeviceKeys(int fd, int outKeys[KEY_MAX+1])
{
    unsigned long keyBits[(KEY_MAX+1)/(8*sizeof(long))];
    memset(keyBits,0,sizeof(keyBits));
    memset(outKeys,0,sizeof(int)*(KEY_MAX+1));

    if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits)<0){
        fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceKeys => %s\n", strerror(errno));
        return;
    }
    int count=0;
    for(int code=0; code<=KEY_MAX; code++){
        int bitSet= (keyBits[code/(8*sizeof(long))] >> (code%(8*sizeof(long)))) &1;
        if(bitSet){
            outKeys[code]=1;
            count++;
        }
    }
    fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceKeys => found %d key scancodes.\n", count);
}

/*
 * discoverSingleDeviceAxes => gather from EVIOCGBIT(EV_ABS,...)
 */
static void discoverSingleDeviceAxes(int fd,
                                     int outAxes[ABS_MAX+1],
                                     int outMin[ABS_MAX+1],
                                     int outMax[ABS_MAX+1])
{
    unsigned long absBits[(ABS_MAX+1)/(8*sizeof(long))];
    memset(absBits,0,sizeof(absBits));
    memset(outAxes,0,sizeof(int)*(ABS_MAX+1));

    for(int i=0; i<=ABS_MAX;i++){
        outMin[i]= -32768;
        outMax[i]=  32767;
    }

    if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits)<0){
        fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceAxes => %s\n", strerror(errno));
        return;
    }
    int count=0;
    for(int code=0; code<=ABS_MAX; code++){
        int bitSet= (absBits[code/(8*sizeof(long))] >> (code%(8*sizeof(long)))) &1;
        if(bitSet){
            outAxes[code]=1;
            count++;
            struct input_absinfo info;
            if(ioctl(fd, EVIOCGABS(code), &info)==0){
                outMin[code]= info.minimum;
                outMax[code]= info.maximum;
                fprintf(stderr,"[GammaPadCapture] singleDeviceAxes => sc=%d => min=%d, max=%d\n",
                        code, info.minimum, info.maximum);
            }
        }
    }
    fprintf(stderr,"[GammaPadCapture] discoverSingleDeviceAxes => found %d axis scancodes.\n", count);
}

/*
 * aggregator discover => merges
 */
void discoverKeys(int fd, int isPrimary)
{
    int singleKeys[KEY_MAX+1];
    memset(singleKeys,0,sizeof(singleKeys));

    discoverSingleDeviceKeys(fd, singleKeys);
    mergeKeysIntoGlobal(singleKeys, isPrimary);
}

void discoverAxes(int fd)
{
    int singleAxes[ABS_MAX+1];
    int singleMin[ABS_MAX+1];
    int singleMax[ABS_MAX+1];
    memset(singleAxes,0,sizeof(singleAxes));
    memset(singleMin,0,sizeof(singleMin));
    memset(singleMax,0,sizeof(singleMax));

    discoverSingleDeviceAxes(fd, singleAxes, singleMin, singleMax);
    mergeAxesIntoGlobal(singleAxes, singleMin, singleMax);
}

/*
 * parse_android_keylayout_file_if_needed => called once per device
 * If .kl is not found or empty, scancode => scancode is kept as fallback.
 */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd)
{
    fprintf(stderr, "[GammaPadCapture] parse_android_keylayout_file_if_needed => .kl\n");
    struct input_id id;
    if (ioctl(fd, EVIOCGID, &id) == 0) {
        fprintf(stderr, "[KL] vendor=0x%04x product=0x%04x\n", id.vendor, id.product);

        /* Try both system and vendor partitions */
        const char* base_dirs[] = {
            "/system/usr/keylayout",
            "/vendor/usr/keylayout"
        };
        char klPath[256];
        FILE* f = NULL;

        for (size_t i = 0; i < sizeof(base_dirs)/sizeof(base_dirs[0]); i++) {
            snprintf(klPath, sizeof(klPath),
                     "%s/Vendor_%04x_Product_%04x.kl",
                     base_dirs[i], id.vendor, id.product);

            f = fopen(klPath, "r");
            if (f) {
                fprintf(stderr, "[KL] found => %s => parsing lines...\n", klPath);
                break;
            } else {
                fprintf(stderr, "[KL] no .kl => %s => skipping.\n", klPath);
            }
        }

        if (!f) {
            /* None of the paths had a matching .kl */
            return;
        }

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strchr(line, '\n');
            if (nl) *nl = '\0';
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
 * parseKeyLayoutLine => includes more button scancodes so we can map them
 */
void parseKeyLayoutLine(const char* line)
{
    char type[32], sCode[64], name[64], rest[128];
    memset(type,0,sizeof(type));
    memset(sCode,0,sizeof(sCode));
    memset(name,0,sizeof(name));
    memset(rest,0,sizeof(rest));

    int parts= sscanf(line, "%31s %63s %63s %127[^\n]", type, sCode, name, rest);
    if(parts<3) return;

    // parse scancode
    int sc=0;
    if(!strncasecmp(sCode,"0x",2)){
        sc= (int)strtol(sCode,NULL,16);
    } else {
        sc= atoi(sCode);
    }
    if(sc<0) return; // invalid sc

    if(!strcasecmp(type,"key")){
        if(sc>KEY_MAX) return;
        int final= sc; // fallback => sc => sc
        // Additional recognized names => final codes:
        if(!strcasecmp(name,"BUTTON_A"))        final= BTN_A;
        else if(!strcasecmp(name,"BUTTON_B"))   final= BTN_B;
        else if(!strcasecmp(name,"BUTTON_X"))   final= BTN_X;
        else if(!strcasecmp(name,"BUTTON_Y"))   final= BTN_Y;
        else if(!strcasecmp(name,"BUTTON_L1"))  final= BTN_TL;
        else if(!strcasecmp(name,"BUTTON_R1"))  final= BTN_TR;
        else if(!strcasecmp(name,"BUTTON_L2"))  final= BTN_TL2;
        else if(!strcasecmp(name,"BUTTON_R2"))  final= BTN_TR2;
        else if(!strcasecmp(name,"BUTTON_SELECT")) final= BTN_SELECT;
        else if(!strcasecmp(name,"BUTTON_START"))  final= BTN_START;
        else if(!strcasecmp(name,"BUTTON_Z"))   final= BTN_Z;
        else if(!strcasecmp(name,"BUTTON_C"))   final= BTN_C;
        else if(!strcasecmp(name,"BUTTON_MODE"))   final= BTN_MODE;
        else if(!strcasecmp(name,"BUTTON_BACK"))   final= BTN_BACK;
        else if(!strcasecmp(name,"BUTTON_THUMBL")) final= BTN_THUMBL;
        else if(!strcasecmp(name,"BUTTON_THUMBR")) final= BTN_THUMBR;
        else if(!strcasecmp(name,"BUTTON_GAMEPAD")) final= BTN_GAMEPAD;
        else if(!strcasecmp(name,"BUTTON_1"))   final= BTN_1;
        else if(!strcasecmp(name,"BUTTON_2"))   final= BTN_2;
        else if(!strcasecmp(name,"BUTTON_3"))   final= BTN_3;
        else if(!strcasecmp(name,"BUTTON_4"))   final= BTN_4;
        else if(!strcasecmp(name,"BACK"))       final= KEY_BACK;
        else if(!strcasecmp(name,"F10"))        final= KEY_F10;
        // fallback => final= sc

        g_keyMap[sc]= final;

        fprintf(stderr,"[KL] 'key %s %s' => sc=%d => finalKey=%d\n",
                sCode,name,sc, g_keyMap[sc]);
    }
    else if(!strcasecmp(type,"axis")){
        if(sc>ABS_MAX) return;
        int final= sc; // fallback => sc => sc
        if(!strcasecmp(name,"X"))           final= ABS_X;
        else if(!strcasecmp(name,"Y"))      final= ABS_Y;
        else if(!strcasecmp(name,"Z"))      final= ABS_Z;
        else if(!strcasecmp(name,"RZ"))     final= ABS_RZ;
        else if(!strcasecmp(name,"LTRIGGER")) final= ABS_BRAKE;
        else if(!strcasecmp(name,"RTRIGGER")) final= ABS_GAS;
        else if(!strcasecmp(name,"HAT_X"))  final= ABS_HAT0X;
        else if(!strcasecmp(name,"HAT_Y"))  final= ABS_HAT0Y;
        // fallback => final= sc

        g_absMap[sc]= final;

        fprintf(stderr,"[KL] 'axis %s %s' => sc=%d => finalAxis=%d\n",
                sCode,name,sc,g_absMap[sc]);
    }
}

/*
 * removePrimaryPhysicalNode => called in gammapad_main.c AFTER we create the virtual pad
 */
void removePrimaryPhysicalNode(void)
{
    if(!g_physicalDevicePath[0]) return;
    char rmCmd[512];
    snprintf(rmCmd,sizeof(rmCmd),"rm -f '%s'", g_physicalDevicePath);
    fprintf(stderr,"[GammaPadCapture] removePrimaryPhysicalNode => %s\n", rmCmd);
    system(rmCmd);

    g_physicalDevicePath[0]= '\0'; // avoid double removal
}

/*
 * open_physical_device => aggregator approach
 *   - We also initialize g_keyMap[sc] = sc and g_absMap[sc] = sc for fallback
 *     so if no .kl is found, the scancode remains sc => sc.
 */
int open_physical_device(const char* device_path)
{
    if(!device_path) return -1;

    int fd= open(device_path, O_RDWR|O_NONBLOCK);
    if(fd<0){
        fprintf(stderr,"[GammaPadCapture] Failed to open %s => %s\n", device_path, strerror(errno));
        return -1;
    }

    int isPrimary=0;
    if(!g_hasPrimaryDevice){
        isPrimary=1;
        g_hasPrimaryDevice=1;

        /* parse driver => store => unbind/bind at exit */
        if(identifyDriverAndDevice(device_path,
                                   g_driverPath,sizeof(g_driverPath),
                                   g_deviceName,sizeof(g_deviceName))==0){
            gHasDriver=1;
        } else {
            fprintf(stderr,"[GammaPadCapture] No driver => skip unbind.\n");
            gHasDriver=0;
        }

        memset(g_physicalDevicePath,0,sizeof(g_physicalDevicePath));
        strncpy(g_physicalDevicePath, device_path, sizeof(g_physicalDevicePath)-1);
    }

    /* Initialize fallback sc => sc for all possible scancodes,
       so if we find no .kl or no lines for a sc, it remains sc => sc. */
    for(int k=0; k<=KEY_MAX; k++){
        if(!g_discoveredKeys[k]){ 
            g_keyMap[k] = k; 
        }
    }
    for(int a=0; a<=ABS_MAX; a++){
        if(!g_discoveredAxes[a]){
            g_absMap[a] = a;
        }
    }

#ifdef __ANDROID__
    parse_android_keylayout_file_if_needed(fd);
#endif

    discoverKeys(fd, isPrimary);
    discoverAxes(fd);
    resolveAxisCollisions();

    /* Re-apply fallback sc => sc for any scancodes that .kl didn't map. 
       We'll do this AFTER collisions so that overshadowed scancodes can remain -1 if needed. */
    for(int sc=0; sc<=KEY_MAX; sc++){
        if(g_discoveredKeys[sc]){
            /* If g_keyMap[sc] <= 0 => not assigned => fallback => sc
               except sc=0 can be a valid code => e.g. KEY_RESERVED => let's skip that. 
               We'll skip if parseKeyLayoutLine set it to something > 0. */
            if(g_keyMap[sc] <= 0){
                g_keyMap[sc] = sc;
            }
        }
    }
    for(int sc=0; sc<=ABS_MAX; sc++){
        if(g_discoveredAxes[sc]){
            if(g_absMap[sc] < 0){
                /* overshadowed => keep -1 => means "ignore" */
            } else if(g_absMap[sc] == 0 && sc!=0){
                /* if it's 0 but sc!=0 => fallback => sc. 
                   (sc=0 is often ABS_X => that's valid. ) */
                g_absMap[sc] = sc;
            }
        }
    }

    if(ioctl(fd, EVIOCGRAB,1)<0){
        fprintf(stderr,"[GammaPadCapture] EVIOCGRAB => %s\n", strerror(errno));
    }

    fprintf(stderr,"[GammaPadCapture] open_physical_device => '%s' (fd=%d). isPrimary=%d\n",
            device_path, fd, isPrimary);
    return fd;
}

/*
 * forward_physical_event => transform scancode => final => forward to controllerFd
 */
void forward_physical_event(const struct input_event* ev)
{
    if (!ev || controllerFd < 0) return;

    if (ev->type == EV_KEY) {
        int sc     = ev->code;
        if (sc < 0 || sc > KEY_MAX) return;
        int mapped = g_keyMap[sc];

        /* ABXY swap */
        if (g_abxy_layout) {
            if      (mapped == BTN_A) mapped = BTN_B;
            else if (mapped == BTN_B) mapped = BTN_A;
            else if (mapped == BTN_X) mapped = BTN_Y;
            else if (mapped == BTN_Y) mapped = BTN_X;
        }

        /* Custom user mappings: override if set */
        if (mapped >= 0 && mapped <= KEY_MAX) {
            int cm = g_customKeyMap[mapped];
            if (cm >= 0) {
                mapped = cm;
            }
        }

        /* 1) Forward the KEY event */
        LOG_FF("[FWD] KEY sc=%d => final=%d => val=%d\n",
               sc, mapped, ev->value);
        struct input_event outKey[2] = {};
        outKey[0].type  = EV_KEY;
        outKey[0].code  = mapped;
        outKey[0].value = ev->value;
        outKey[1].type  = EV_SYN;
        outKey[1].code  = SYN_REPORT;
        outKey[1].value = 0;
        write(controllerFd, outKey, sizeof(outKey));

        /* 2) GAS/BRAKE emulation: L2/R2 keys → ABS_BRAKE/ABS_GAS */
        if (g_gas_brake_emulation) {
            int axisSc   = -1;
            int axisCode = -1;
            if (mapped == BTN_TL2)      { axisSc = ABS_BRAKE; axisCode = ABS_BRAKE; }
            else if (mapped == BTN_TR2) { axisSc = ABS_GAS;   axisCode = ABS_GAS;   }
            if (axisSc >= 0) {
                int full = (g_discoveredAxes[axisSc]
                            ? getPhysicalAbsMax(axisSc)
                            : 255);
                struct input_event outAbs[2] = {};
                outAbs[0].type  = EV_ABS;
                outAbs[0].code  = axisCode;
                outAbs[0].value = ev->value ? full : 0;
                outAbs[1].type  = EV_SYN;
                outAbs[1].code  = SYN_REPORT;
                outAbs[1].value = 0;
                write(controllerFd, outAbs, sizeof(outAbs));
            }
        }

        return;
    }
    else if (ev->type == EV_ABS) {
        int sc = ev->code;
        if (sc < 0 || sc > ABS_MAX) return;

        /* 0) Record raw value and apply calibration *before* any inversion/sensitivity */
        recordRawValue(sc, ev->value);
        int ev_val = applyCalibration(sc, ev->value);

        /* 1) Inversion */
        if ((sc == ABS_X || sc == ABS_Y) && g_left_stick_invert) {
            int mn = getPhysicalAbsMin(sc);
            int mx = getPhysicalAbsMax(sc);
            ev_val = mn + mx - ev_val;
        }
       else if ((sc == ABS_Z || sc == ABS_RZ) && g_right_stick_invert_z_rz) {
            int mn = getPhysicalAbsMin(sc);
            int mx = getPhysicalAbsMax(sc);
            ev_val = mn + mx - ev_val;
        }
        else if ((sc == ABS_RX || sc == ABS_RY) && g_right_stick_invert) {
            int mn = getPhysicalAbsMin(sc);
            int mx = getPhysicalAbsMax(sc);
            ev_val = mn + mx - ev_val;
        }

        /* 2) Sensitivity scaling (sticks only: X, Y, RX, RY) */
        if (sc == ABS_X  || sc == ABS_Y  ||
            sc == ABS_RX || sc == ABS_RY ||
            sc == ABS_Z  || sc == ABS_RZ )

        {
            int sens = g_analog_sensitivity;
            if (sens != 0) {
                int mn     = getPhysicalAbsMin(sc);
                int mx     = getPhysicalAbsMax(sc);
                int center = (mn + mx) / 2;
                int delta  = ev_val - center;
                int num;
                switch (sens) {
                    case -3: num =  50; break;
                    case -2: num =  75; break;
                    case -1: num =  90; break;
                    case  1: num = 110; break;
                    case  2: num = 125; break;
                    case  3: num = 150; break;
                    default: num = 100; break;
                }
                delta  = (delta * num) / 100;
                ev_val = center + delta;
                if (ev_val < mn) ev_val = mn;
                if (ev_val > mx) ev_val = mx;
            }
        }

        /* 2.5) Circular deadzone (sticks only: ABS_X, ABS_Y, ABS_RX, ABS_RY) */
       if (g_deadzone > 0 &&
           (sc == ABS_X  || sc == ABS_Y  ||
            sc == ABS_RX || sc == ABS_RY ||
            sc == ABS_Z  || sc == ABS_RZ))
       {
           /* circular dead-zone: works on each stick pair */
           int mn     = getPhysicalAbsMin(sc);
           int mx     = getPhysicalAbsMax(sc);
           int center = (mn + mx) / 2;
           int half   = (mx - mn) / 2;
           int dz     = (half * g_deadzone) / 100;

           /* pick the “other” axis in the same stick */
           int other_sc;
           if      (sc == ABS_X ) other_sc = ABS_Y;
           else if (sc == ABS_Y ) other_sc = ABS_X;
           else if (sc == ABS_RX) other_sc = ABS_RY;
           else if (sc == ABS_RY) other_sc = ABS_RX;
           else if (sc == ABS_Z ) other_sc = ABS_RZ;
           else                    other_sc = ABS_Z;   // sc == ABS_RZ

           int dx = ev_val - center;
           int dy = last_processed_abs[other_sc] - center;
           if ((dx*dx + dy*dy) <= dz*dz) {
               ev_val = center;
           }
       }

        /* 3) DPAD ↔ Left-Stick swap */
        if (g_dpad_analog_swap) {
            int hatx = g_absMap[ABS_HAT0X];
            int haty = g_absMap[ABS_HAT0Y];
            int lsx  = g_absMap[ABS_X];
            int lsy  = g_absMap[ABS_Y];

            if (hatx >= 0 && haty >= 0 && lsx >= 0 && lsy >= 0) {
                /* DPAD → Analog */
                if (sc == ABS_HAT0X || sc == ABS_HAT0Y) {
                    int physMin, physMax, physCenter, mapped;
                    if (sc == ABS_HAT0X) {
                        physMin = getPhysicalAbsMin(ABS_X);
                        physMax = getPhysicalAbsMax(ABS_X);
                        mapped  = lsx;
                    } else {
                        physMin = getPhysicalAbsMin(ABS_Y);
                        physMax = getPhysicalAbsMax(ABS_Y);
                        mapped  = lsy;
                    }
                    physCenter = (physMin + physMax) / 2;

                    struct input_event out[2] = {};
                    out[0].type  = EV_ABS;
                    out[0].code  = mapped;
                    out[0].value = (ev_val < 0 ? physMin
                                      : ev_val > 0 ? physMax
                                      : physCenter);
                    out[1].type  = EV_SYN;
                    out[1].code  = SYN_REPORT;
                    out[1].value = 0;
                    write(controllerFd, out, sizeof(out));
                    return;
                }
                /* Analog → DPAD */
                if (sc == ABS_X || sc == ABS_Y) {
                    int physMin    = getPhysicalAbsMin(sc);
                    int physMax    = getPhysicalAbsMax(sc);
                    int physCenter = (physMin + physMax) / 2;
                    int halfRange  = (physMax - physMin) / 2;
                    int thresh     = halfRange * 60 / 100;

                    int delta  = ev_val - physCenter;
                    int hatVal = (delta >  thresh ? +1
                                   : delta < -thresh ? -1
                                   : 0);

                    int mapped = (sc == ABS_X ? hatx : haty);
                    struct input_event out[2] = {};
                    out[0].type  = EV_ABS;
                    out[0].code  = mapped;
                    out[0].value = hatVal;
                    out[1].type  = EV_SYN;
                    out[1].code  = SYN_REPORT;
                    out[1].value = 0;
                    write(controllerFd, out, sizeof(out));
                    return;
                }
            }
        }

        /* 3.5) GAS/BRAKE → L2/R2 emulation (axis→button) when no physical L2/R2 exists */
        if (g_gas_brake_emulation && controllerFd >= 0) {
            /* We only synthesize if the source does NOT have real L2/R2 keys */
            static int tl2_down = 0, tr2_down = 0;
            const int have_l2 = (g_discoveredKeys[BTN_TL2] != 0);
            const int have_r2 = (g_discoveredKeys[BTN_TR2] != 0);

            /* thresholds computed from physical min/max of the axis */
            if (sc == ABS_BRAKE && !have_l2) {
                int mn = getPhysicalAbsMin(ABS_BRAKE);
                int mx = getPhysicalAbsMax(ABS_BRAKE);
                int thr_on  = mn + (int)((long long)(mx - mn) * 80 / 100); /* 80% press */
                int thr_off = mn + (int)((long long)(mx - mn) * 60 / 100); /* 60% release */
                int want_down = (ev_val >= thr_on) ? 1 : ((ev_val <= thr_off) ? 0 : tl2_down);
                if (want_down != tl2_down) {
                    struct input_event k[2] = {};
                    k[0].type = EV_KEY; k[0].code = BTN_TL2; k[0].value = want_down;
                    k[1].type = EV_SYN; k[1].code = SYN_REPORT; k[1].value = 0;
                    write(controllerFd, k, sizeof(k));
                    tl2_down = want_down;
                    LOG_FF("[EMU] ABS_BRAKE=%d → BTN_TL2=%d (on=%d%% off=%d%%)\n",
                           ev_val, tl2_down, 80, 60);
                }
            } else if (sc == ABS_GAS && !have_r2) {
                int mn = getPhysicalAbsMin(ABS_GAS);
                int mx = getPhysicalAbsMax(ABS_GAS);
                int thr_on  = mn + (int)((long long)(mx - mn) * 80 / 100); /* 80% press */
                int thr_off = mn + (int)((long long)(mx - mn) * 60 / 100); /* 60% release */
                int want_down = (ev_val >= thr_on) ? 1 : ((ev_val <= thr_off) ? 0 : tr2_down);
                if (want_down != tr2_down) {
                    struct input_event k[2] = {};
                    k[0].type = EV_KEY; k[0].code = BTN_TR2; k[0].value = want_down;
                    k[1].type = EV_SYN; k[1].code = SYN_REPORT; k[1].value = 0;
                    write(controllerFd, k, sizeof(k));
                    tr2_down = want_down;
                    LOG_FF("[EMU] ABS_GAS=%d → BTN_TR2=%d (on=%d%% off=%d%%)\n",
                           ev_val, tr2_down, 80, 60);
                }
            }
        }

        /* 4) Normal ABS mapping */
        int mapped = g_absMap[sc];
        LOG_FF("[FWD] ABS sc=%d => final=%d => val=%d\n",
               sc, mapped, ev_val);
        if (mapped < 0) return;

        struct input_event out[2] = {};
        out[0].type  = EV_ABS;
        out[0].code  = mapped;
        out[0].value = ev_val;
        out[1].type  = EV_SYN;
        out[1].code  = SYN_REPORT;
        out[1].value = 0;
        write(controllerFd, out, sizeof(out));

        /* 5) Record for next deadzone calc */
        last_processed_abs[sc] = ev_val;
    }
}

/*
 * destructor => unbind/bind at exit (no node removal here)
 */
__attribute__((destructor))
static void onFinish(void)
{
    if (g_noSourceRebind) {
        fprintf(stderr,"[GammaPadCapture] --no-source-rebind: skipping onFinish unbind/rebind.\n");
        return;
    }

    fprintf(stderr,"[GammaPadCapture] onFinish() => unbind/rebind x3 for primary device.\n");
    unbindAndRebind();
}
