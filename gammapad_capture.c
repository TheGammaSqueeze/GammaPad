#include "gammapad_capture.h"
#include <linux/input.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/* from gammapad_main.c */
extern int controllerFd;

/*
 * The scancode arrays are declared externally so other files can see them.
 */
int g_discoveredKeys[KEY_MAX+1];
int g_discoveredAxes[ABS_MAX+1];
static int g_physicalAbsMin[ABS_MAX+1];
static int g_physicalAbsMax[ABS_MAX+1];

/* scancode => final code arrays from .kl logic. */
int g_keyMap[KEY_MAX+1];
int g_absMap[ABS_MAX+1];

/*
 * Accessors => used by gammapad_controller.c
 */
int getPhysicalAbsMin(int scancode)
{
    if(scancode<0||scancode>ABS_MAX) return -32768;
    return g_physicalAbsMin[scancode];
}
int getPhysicalAbsMax(int scancode)
{
    if(scancode<0||scancode>ABS_MAX) return 32767;
    return g_physicalAbsMax[scancode];
}

/*
 * forward declarations
 */
void parse_android_keylayout_file_if_needed(int fd);
void discoverKeys(int fd);
void discoverAxes(int fd);
void parseKeyLayoutLine(const char* line);

int open_physical_device(const char* device_path)
{
    if(!device_path) return -1;

    int fd= open(device_path, O_RDWR|O_NONBLOCK);
    if(fd<0){
        fprintf(stderr,"[Capture] open_physical_device: open '%s' => %s\n",
            device_path, strerror(errno));
        return -1;
    }

    /* Initialize arrays => identity transform by default. */
    for(int i=0;i<=KEY_MAX;i++){
        g_keyMap[i]= i;
        g_discoveredKeys[i]=0;
    }
    for(int i=0;i<=ABS_MAX;i++){
        g_absMap[i]= i;
        g_discoveredAxes[i]=0;
        g_physicalAbsMin[i]=0;
        g_physicalAbsMax[i]=0;
    }

#ifdef __ANDROID__
    parse_android_keylayout_file_if_needed(fd);
#endif

    discoverKeys(fd);
    discoverAxes(fd);

    /* If you want exclusive: */
    if(ioctl(fd, EVIOCGRAB,1)<0){
        fprintf(stderr,"[Capture] EVIOCGRAB => %s\n", strerror(errno));
    }

    fprintf(stderr,"[Capture] open_physical_device => '%s' opened.\n", device_path);
    return fd; /* we do NOT remove node here, let main() do it after create_virtual_pad. */
}

/*
 * forward_physical_event => scancode => final code => write to controllerFd
 */
void forward_physical_event(const struct input_event* ev)
{
    if(!ev) return;
    if(controllerFd<0) return;

    if(ev->type==EV_KEY){
        int orig= ev->code;
        if(orig<0||orig>KEY_MAX) return;
        int mapped= g_keyMap[orig];

        /* Add debug logs => see the transform. */
        fprintf(stderr,"[FWD] KEY scancode=%d => final=%d, value=%d\n",
            orig, mapped, ev->value);

        struct input_event out[2];
        memset(&out,0,sizeof(out));
        out[0].type= EV_KEY;
        out[0].code= mapped;
        out[0].value= ev->value;
        out[1].type= EV_SYN;
        out[1].code= SYN_REPORT;
        out[1].value=0;
        write(controllerFd,&out,sizeof(out));
    }
    else if(ev->type==EV_ABS){
        int orig= ev->code;
        if(orig<0||orig>ABS_MAX) return;
        int mapped= g_absMap[orig];

        /* Add debug logs => see the transform. */
        fprintf(stderr,"[FWD] ABS scancode=%d => final=%d, value=%d\n",
            orig, mapped, ev->value);

        struct input_event out[2];
        memset(&out,0,sizeof(out));
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
 * destructor => we do not remove the node here.
 */
__attribute__((destructor))
static void onFinish(void)
{
    fprintf(stderr,"[Capture] onFinish => no node removal, that is done in main.\n");
}

/*
 * parse_android_keylayout_file_if_needed => read .kl
 */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd)
{
    fprintf(stderr,"[Capture] parse_android_keylayout_file_if_needed => started.\n");
    struct input_id id;
    if(ioctl(fd,EVIOCGID,&id)==0){
        fprintf(stderr,"[Capture] Vendor=0x%04x, Product=0x%04x\n",
            id.vendor,id.product);

        char klPath[256];
        snprintf(klPath,sizeof(klPath),
            "/system/usr/keylayout/Vendor_%04x_Product_%04x.kl",
            id.vendor,id.product);

        FILE* f= fopen(klPath,"r");
        if(!f){
            fprintf(stderr,"[KL] no .kl => %s\n", klPath);
            return;
        }
        fprintf(stderr,"[KL] found => %s\n",klPath);

        char line[256];
        while(fgets(line,sizeof(line),f)){
            char*nl= strchr(line,'\n');
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
 * parseKeyLayoutLine => transform scancode => final code if we see "key" or "axis"
 *   Because we removed 'static', it must match the forward decl: void parseKeyLayoutLine(const char*);
 */
void parseKeyLayoutLine(const char* line)
{
    fprintf(stderr,"[Capture] parseKeyLayoutLine => '%s'\n", line);
    char type[32], sCode[32], name[32], rest[128];
    memset(type,0,sizeof(type));
    memset(sCode,0,sizeof(sCode));
    memset(name,0,sizeof(name));
    memset(rest,0,sizeof(rest));

    int parts= sscanf(line, "%31s %31s %31s %127[^\n]", type, sCode, name, rest);
    if(parts<3) return;

    if(!strcasecmp(type,"key")){
        int scancode=0;
        if(!strncasecmp(sCode,"0x",2)) scancode= (int)strtol(sCode,NULL,16);
        else scancode= atoi(sCode);
        if(scancode>=0 && scancode<=KEY_MAX){
            if(!strcasecmp(name,"BUTTON_A"))   g_keyMap[scancode]= BTN_A;
            else if(!strcasecmp(name,"BUTTON_B")) g_keyMap[scancode]= BTN_B;
            else if(!strcasecmp(name,"BUTTON_X")) g_keyMap[scancode]= BTN_X;
            else if(!strcasecmp(name,"BUTTON_Y")) g_keyMap[scancode]= BTN_Y;
            /* etc. fallback => scancode => scancode if not recognized */

            fprintf(stderr,"[KL] 'key %s %s' => scancode=%d => final=%d\n",
                sCode,name,scancode, g_keyMap[scancode]);
        }
    }
    else if(!strcasecmp(type,"axis")){
        int scancode=0;
        if(!strncasecmp(sCode,"0x",2)) scancode= (int)strtol(sCode,NULL,16);
        else scancode= atoi(sCode);
        if(scancode>=0 && scancode<=ABS_MAX){
            if(!strcasecmp(name,"X"))        g_absMap[scancode]= ABS_X;
            else if(!strcasecmp(name,"Y"))   g_absMap[scancode]= ABS_Y;
            else if(!strcasecmp(name,"Z"))   g_absMap[scancode]= ABS_Z;
            else if(!strcasecmp(name,"RZ"))  g_absMap[scancode]= ABS_RZ;
            else if(!strcasecmp(name,"LTRIGGER")) g_absMap[scancode]= ABS_GAS;
            else if(!strcasecmp(name,"RTRIGGER")) g_absMap[scancode]= ABS_BRAKE;
            else if(!strcasecmp(name,"HAT_X"))    g_absMap[scancode]= ABS_HAT0X;
            else if(!strcasecmp(name,"HAT_Y"))    g_absMap[scancode]= ABS_HAT0Y;
            /* etc. fallback => scancode => scancode */

            fprintf(stderr,"[KL] 'axis %s %s' => scancode=%d => finalAbs=%d\n",
                sCode,name,scancode, g_absMap[scancode]);
        }
    }
}

/*
 * discoverKeys => set g_discoveredKeys[scancode]=1 if device has that scancode
 */
void discoverKeys(int fd)
{
    unsigned long bits[(KEY_MAX+1)/(8*sizeof(long))];
    memset(bits,0,sizeof(bits));
    int rc= ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits);
    if(rc<0){
        fprintf(stderr,"[Capture] discoverKeys => EVIOCGBIT(EV_KEY)=> %s\n",
            strerror(errno));
        return;
    }
    int countFound=0;
    for(int c=0;c<=KEY_MAX;c++){
        int isSet= (bits[c/(8*sizeof(long))] >> (c%(8*sizeof(long)))) &1;
        if(isSet){
            g_discoveredKeys[c]=1;
            countFound++;
        }
    }
    fprintf(stderr,"[Capture] discoverKeys => found %d key scancodes.\n", countFound);
}

/*
 * discoverAxes => set g_discoveredAxes[scancode]=1 if device has that scancode
 * also store min/max in g_physicalAbsMin/Max[scancode].
 */
void discoverAxes(int fd)
{
    unsigned long bits[(ABS_MAX+1)/(8*sizeof(long))];
    memset(bits,0,sizeof(bits));
    int rc= ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(bits)), bits);
    if(rc<0){
        fprintf(stderr,"[Capture] discoverAxes => EVIOCGBIT(EV_ABS)=> %s\n",strerror(errno));
        return;
    }
    int countFound=0;
    for(int c=0; c<=ABS_MAX; c++){
        int isSet= (bits[c/(8*sizeof(long))] >> (c%(8*sizeof(long)))) &1;
        if(!isSet){
            g_discoveredAxes[c]=0;
            g_physicalAbsMin[c]=0;
            g_physicalAbsMax[c]=0;
            continue;
        }
        g_discoveredAxes[c]=1;
        countFound++;
        struct input_absinfo info;
        if(ioctl(fd, EVIOCGABS(c), &info)==0){
            g_physicalAbsMin[c]= info.minimum;
            g_physicalAbsMax[c]= info.maximum;
            fprintf(stderr,"[Capture] discoverAxes => scancode=%d => min=%d, max=%d\n",
                c, info.minimum, info.maximum);
        } else {
            g_physicalAbsMin[c]=-32768;
            g_physicalAbsMax[c]=32767;
        }
    }
    fprintf(stderr,"[Capture] discoverAxes => found %d axis scancodes.\n", countFound);
}
