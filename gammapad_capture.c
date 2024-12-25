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
 */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd)
{
    fprintf(stderr,"[GammaPadCapture] parse_android_keylayout_file_if_needed => .kl\n");
    struct input_id id;
    if(ioctl(fd, EVIOCGID, &id)==0){
        fprintf(stderr,"[KL] vendor=0x%04x product=0x%04x\n", id.vendor, id.product);

        char klPath[256];
        snprintf(klPath,sizeof(klPath),
                 "/system/usr/keylayout/Vendor_%04x_Product_%04x.kl",
                 id.vendor, id.product);

        FILE* f= fopen(klPath,"r");
        if(!f){
            fprintf(stderr,"[KL] no .kl => %s\n", klPath);
            return;
        }
        fprintf(stderr,"[KL] found => %s\n", klPath);

        char line[256];
        while(fgets(line,sizeof(line),f)){
            char*nl=strchr(line,'\n');
            if(nl)*nl=0;
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
        int final= sc; // fallback
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
        int final= sc; // fallback
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

#ifdef __ANDROID__
    parse_android_keylayout_file_if_needed(fd);
#endif

    discoverKeys(fd, isPrimary);
    discoverAxes(fd);
    resolveAxisCollisions();

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
    if(!ev) return;
    if(controllerFd<0) return;

    if(ev->type==EV_KEY){
        int sc= ev->code;
        if(sc<0||sc>KEY_MAX) return;
        int mapped= g_keyMap[sc];
        fprintf(stderr,"[FWD] KEY sc=%d => final=%d => val=%d\n",
                sc,mapped,ev->value);

        struct input_event out[2];
        memset(out,0,sizeof(out));
        out[0].type= EV_KEY;
        out[0].code= mapped;
        out[0].value= ev->value;
        out[1].type= EV_SYN;
        out[1].code= SYN_REPORT;
        out[1].value=0;
        write(controllerFd,&out,sizeof(out));
    }
    else if(ev->type==EV_ABS){
        int sc= ev->code;
        if(sc<0||sc>ABS_MAX) return;
        int mapped= g_absMap[sc];
        fprintf(stderr,"[FWD] ABS sc=%d => final=%d => val=%d\n",
                sc,mapped,ev->value);

        if(mapped<0) return; // overshadowed => skip

        struct input_event out[2];
        memset(out,0,sizeof(out));
        out[0].type= EV_ABS;
        out[0].code= mapped;
        out[0].value= ev->value;
        out[1].type= EV_SYN;
        out[1].code= SYN_REPORT;
        out[1].value=0;
        write(controllerFd,&out,sizeof(out));
    }
}

/*
 * destructor => unbind/bind at exit (no node removal here)
 */
__attribute__((destructor))
static void onFinish(void)
{
    fprintf(stderr,"[GammaPadCapture] onFinish() => unbind/rebind x3 for primary device.\n");
    unbindAndRebind();
}
