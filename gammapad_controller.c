#include "gammapad.h"
#include "gammapad_inputdefs.h"
#include <errno.h>
#include <string.h>

/* We'll rely on these externs from gammapad_capture.c */
extern int g_discoveredKeys[KEY_MAX+1];
extern int g_discoveredAxes[ABS_MAX+1];
extern int g_keyMap[KEY_MAX+1];
extern int g_absMap[ABS_MAX+1];
extern int getPhysicalAbsMin(int scancode);
extern int getPhysicalAbsMax(int scancode);

/* We'll read from g_physicalFd if it's open. */
extern int g_physicalFd;

/*
 * setAbsRange => fallback approach if axis wasn't discovered
 */
static void setAbsRange(struct uinput_user_dev *uidev,
                        int axis, int defMin, int defMax)
{
    /* We'll only do fallback if not discovered. We'll scan for scancodes that map to 'axis'. */
    for(int sc=0; sc<=ABS_MAX; sc++){
        if(g_discoveredAxes[sc]){
            int finalAxis= g_absMap[sc];
            if(finalAxis==axis){
                // This axis is discovered => skip fallback
                return;
            }
        }
    }
    // If we get here => axis not discovered => fallback
    uidev->absmin[axis]= defMin;
    uidev->absmax[axis]= defMax;
    LOG_FF("setAbsRange: fallback axis=%d => min=%d, max=%d\n", axis, defMin, defMax);
}

/*
 * enableDiscoveredKeys => scancode => final = g_keyMap[scancode],
 * then UI_SET_KEYBIT(final).
 */
static void enableDiscoveredKeys(int fd)
{
    int countFound=0;
    for(int sc=0; sc<=KEY_MAX; sc++){
        if(g_discoveredKeys[sc]){
            int finalKey= g_keyMap[sc];
            if(ioctl(fd, UI_SET_KEYBIT, finalKey)<0){
                LOG_FF("enableDiscoveredKeys: UI_SET_KEYBIT(%d) => %s\n",
                       finalKey, strerror(errno));
            } else {
                LOG_FF("enableDiscoveredKeys: scancode=%d => final=%d\n",
                       sc, finalKey);
                countFound++;
            }
        }
    }
    if(!countFound){
        LOG_FF("enableDiscoveredKeys: none => fallback array.\n");
        gp_enable_gamepad_buttons(fd);
    }
}

/*
 * enableDiscoveredAxes => scancode => final= g_absMap[scancode],
 * then UI_SET_ABSBIT(final).
 */
static void enableDiscoveredAxes(int fd)
{
    int countFound=0;
    for(int sc=0; sc<=ABS_MAX; sc++){
        if(g_discoveredAxes[sc]){
            int finalAxis= g_absMap[sc];
            if(ioctl(fd, UI_SET_ABSBIT, finalAxis)<0){
                LOG_FF("enableDiscoveredAxes: UI_SET_ABSBIT(%d) => %s\n",
                       finalAxis,strerror(errno));
            } else {
                LOG_FF("enableDiscoveredAxes: scancode=%d => finalAxis=%d\n",
                       sc, finalAxis);
                countFound++;
            }
        }
    }
    if(!countFound){
        LOG_FF("enableDiscoveredAxes: none => fallback array.\n");
        gp_enable_gamepad_abs(fd);
    }
}

int create_virtual_controller(int* fd_out)
{
    if(!fd_out)return -1;

    int fd= open("/dev/uinput", O_RDWR|O_NONBLOCK);
    if(fd<0){
        LOG_FF("create_virtual_controller: open => %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);

    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_PERIODIC);
    ioctl(fd, UI_SET_FFBIT, FF_CONSTANT);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    ioctl(fd, UI_SET_FFBIT, FF_RAMP);
    ioctl(fd, UI_SET_FFBIT, FF_SPRING);
    ioctl(fd, UI_SET_FFBIT, FF_DAMPER);
    ioctl(fd, UI_SET_FFBIT, FF_INERTIA);

    /* Dynamically discovered scancodes => final codes. */
    enableDiscoveredKeys(fd);
    enableDiscoveredAxes(fd);

    struct uinput_user_dev uidev;
    memset(&uidev,0,sizeof(uidev));

    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "GammaPad Virtual Controller");
    uidev.id.bustype= BUS_USB;
    uidev.id.vendor= 0x045e;
    uidev.id.product=0x02fd;
    uidev.id.version=0x0003;
    uidev.ff_effects_max= 32;

    /*
     * fallback setAbsRange for typical axes => only if not discovered
     */
    setAbsRange(&uidev, ABS_X,   -1800, 1800);
    setAbsRange(&uidev, ABS_Y,   -1800, 1800);
    setAbsRange(&uidev, ABS_Z,   -1800, 1800);
    setAbsRange(&uidev, ABS_RZ,  -1800, 1800);
    setAbsRange(&uidev, ABS_GAS,   0,   255);
    setAbsRange(&uidev, ABS_BRAKE, 0,   255);
    setAbsRange(&uidev, ABS_HAT0X, -1,  1);
    setAbsRange(&uidev, ABS_HAT0Y, -1,  1);

    /*
     * Now override discovered scancodes => finalAxis with real min/max
     */
    for(int sc=0; sc<=ABS_MAX; sc++){
        if(g_discoveredAxes[sc]){
            int finalAxis= g_absMap[sc];
            int minVal= getPhysicalAbsMin(sc);
            int maxVal= getPhysicalAbsMax(sc);
            LOG_FF("create_virtual_controller: scancode=%d => finalAxis=%d => min=%d, max=%d\n",
                sc, finalAxis, minVal, maxVal);
            uidev.absmin[finalAxis]= minVal;
            uidev.absmax[finalAxis]= maxVal;
        }
    }

    if(write(fd, &uidev,sizeof(uidev))<0){
        LOG_FF("create_virtual_controller: write => %s\n",strerror(errno));
        close(fd);
        return -1;
    }
    if(ioctl(fd, UI_DEV_CREATE)<0){
        LOG_FF("create_virtual_controller: UI_DEV_CREATE => %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_FF("create_virtual_controller: success => fd=%d\n", fd);
    *fd_out= fd;
    return 0;
}

/*
 * create_virtual_mouse => same as before
 */
int create_virtual_mouse(int* fd_out)
{
    if(!fd_out)return -1;

    int fd= open("/dev/uinput", O_WRONLY|O_NONBLOCK);
    if(fd<0){
        LOG_FF("create_virtual_mouse: open => %s\n", strerror(errno));
        return -1;
    }

    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    if(gp_enable_mouse_buttons(fd)<0){
        LOG_FF("create_virtual_mouse: gp_enable_mouse_buttons => fail\n");
        close(fd);
        return -1;
    }
    if(gp_enable_mouse_relaxes(fd)<0){
        LOG_FF("create_virtual_mouse: gp_enable_mouse_relaxes => fail\n");
        close(fd);
        return -1;
    }

    struct uinput_user_dev uidev;
    memset(&uidev,0,sizeof(uidev));

    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "GammaPad Virtual Mouse");
    uidev.id.bustype= BUS_USB;
    uidev.id.vendor= 0x045e;
    uidev.id.product=0x02ff;
    uidev.id.version=0x0003;

    if(write(fd,&uidev,sizeof(uidev))<0){
        LOG_FF("create_virtual_mouse: write => %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if(ioctl(fd, UI_DEV_CREATE)<0){
        LOG_FF("create_virtual_mouse: UI_DEV_CREATE => %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    LOG_FF("create_virtual_mouse: success => fd=%d\n", fd);
    *fd_out= fd;
    return 0;
}

void destroy_virtual_device(int fd)
{
    if(fd<0)return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
