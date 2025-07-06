/*****************************************************
 * gammapad_controller.c
 *
 * Creates the virtual controller (via /dev/uinput)
 * using all discovered scancodes from aggregator
 * logic in gammapad_capture.c. Also sets up force-
 * feedback bits (advertise FF_RUMBLE, FF_CONSTANT, etc.).
 *
 * When a physical FF device is present (via --ffdev),
 * we query its supported FF effect bits and only enable those
 * in the virtual controller. This way the virtual device
 * is an accurate reflection of the physical motor.
 *****************************************************/

#include "gammapad.h"
#include "gammapad_inputdefs.h"
#include <errno.h>
#include <string.h>
#include "input-event-codes.h"   /* for KEY_MAX */


/* We'll rely on these externs from gammapad_capture.c */
extern int g_discoveredKeys[KEY_MAX+1];
extern int g_discoveredAxes[ABS_MAX+1];
extern int g_keyMap[KEY_MAX+1];
extern int g_absMap[ABS_MAX+1];
extern int getPhysicalAbsMin(int scancode);
extern int getPhysicalAbsMax(int scancode);
extern int g_customKeyMap[KEY_MAX + 1];

/* We'll read from g_physicalFd if it's open. (legacy leftover) */
extern int g_physicalFd;

/*
 * setAbsRange => fallback approach if axis wasn't discovered
 */
static void setAbsRange(struct uinput_user_dev *uidev,
                        int axis, int defMin, int defMax)
{
    /* We'll only do fallback if aggregator never discovered that axis. */
    for (int sc = 0; sc <= ABS_MAX; sc++){
        if (g_discoveredAxes[sc]){
            if (g_absMap[sc] == axis){
                return;
            }
        }
    }
    uidev->absmin[axis] = defMin;
    uidev->absmax[axis] = defMax;
    LOG_FF("setAbsRange: fallback axis=%d => min=%d, max=%d\n", axis, defMin, defMax);
}

/*
 * enableDiscoveredKeys => for each discovered key scancode, call UI_SET_KEYBIT
 */
static void enableDiscoveredKeys(int fd)
{
    int countFound = 0;
    for (int sc = 0; sc <= KEY_MAX; sc++){
        if (g_discoveredKeys[sc]){
            int finalKey = g_keyMap[sc];
            if (ioctl(fd, UI_SET_KEYBIT, finalKey) < 0){
                LOG_FF("enableDiscoveredKeys: UI_SET_KEYBIT(%d) => %s\n",
                       finalKey, strerror(errno));
            } else {
                LOG_FF("enableDiscoveredKeys: scancode=%d => final=%d\n",
                       sc, finalKey);
                countFound++;
            }
        }
    }
    if (!countFound){
        LOG_FF("enableDiscoveredKeys: none => fallback array.\n");
        gp_enable_gamepad_buttons(fd);
    }
}

/*
 * enableDiscoveredAxes => for each discovered axis scancode, call UI_SET_ABSBIT
 */
static void enableDiscoveredAxes(int fd)
{
    int countFound = 0;
    for (int sc = 0; sc <= ABS_MAX; sc++){
        if (g_discoveredAxes[sc]){
            int finalAxis = g_absMap[sc];
            if (ioctl(fd, UI_SET_ABSBIT, finalAxis) < 0){
                LOG_FF("enableDiscoveredAxes: UI_SET_ABSBIT(%d) => %s\n",
                       finalAxis, strerror(errno));
            } else {
                LOG_FF("enableDiscoveredAxes: scancode=%d => finalAxis=%d\n",
                       sc, finalAxis);
                countFound++;
            }
        }
    }
    if (!countFound){
        LOG_FF("enableDiscoveredAxes: none => fallback array.\n");
        gp_enable_gamepad_abs(fd);
    }
}

/*
 * create_virtual_controller => creates the virtual pad.
 * New change: if a physical FF device is present, query its supported
 * FF effects via EVIOCGBIT and enable exactly those bits on the virtual
 * controller so that it reflects the physical motor.
 */
int create_virtual_controller(int* fd_out) {
    if (!fd_out) return -1;
    int fd = open("/dev/uinput", O_RDWR|O_NONBLOCK);
    if (fd < 0) {
        LOG_FF("create_virtual_controller: open => %s\n", strerror(errno));
        return -1;
    }

    /* Core bits */
    if (ioctl(fd, UI_SET_EVBIT, EV_KEY)  < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_ABS)  < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_SYN)  < 0 ||
        ioctl(fd, UI_SET_EVBIT, EV_FF)   < 0) {
        LOG_FF("create_virtual_controller: UI_SET_EVBIT => %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0) {
        LOG_FF("create_virtual_controller: UI_SET_PROPBIT => %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    extern int g_ffPhysicalFd;
    extern int g_hasPhysicalFF;
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        /* Query the physical device for supported FF effect bits */
        unsigned long ffBits[2] = {0};
        if (ioctl(g_ffPhysicalFd, EVIOCGBIT(EV_FF, sizeof(ffBits)), ffBits) < 0) {
            LOG_FF("create_virtual_controller: EVIOCGBIT on physical FF device failed: %s\n", strerror(errno));
            /* Fall back to a minimal set (e.g., FF_RUMBLE only) */
            ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
        } else {
            /* Iterate over a reasonable range (e.g., 0 to 127) and enable only supported bits */
            for (unsigned int i = 0; i < 128; i++) {
                if (ffBits[i / (8 * sizeof(unsigned long))] & (1UL << (i % (8 * sizeof(unsigned long))))) {
                    ioctl(fd, UI_SET_FFBIT, i);
                    LOG_FF("create_virtual_controller: enabling FF effect %u\n", i);
                }
            }
        }
    } else {
        /* No physical FF device: enable a default set */
        ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
        ioctl(fd, UI_SET_FFBIT, FF_PERIODIC);
        ioctl(fd, UI_SET_FFBIT, FF_CONSTANT);
        ioctl(fd, UI_SET_FFBIT, FF_GAIN);
        ioctl(fd, UI_SET_FFBIT, FF_RAMP);
        ioctl(fd, UI_SET_FFBIT, FF_SPRING);
        ioctl(fd, UI_SET_FFBIT, FF_DAMPER);
        ioctl(fd, UI_SET_FFBIT, FF_INERTIA);
    }

    /* Enable keys and axes based on discovered scancodes */
    enableDiscoveredKeys(fd);
    enableDiscoveredAxes(fd);

    /* Advertise every user-mapped destination code */
    for (int sc = 0; sc <= KEY_MAX; sc++) {
        int dst = g_customKeyMap[sc];
        if (dst >= 0) {
            ioctl(fd, UI_SET_KEYBIT, dst);
        }
    }

    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "%s", g_uiname);
    uidev.id.bustype = g_uibus;
    uidev.id.vendor  = g_uivid;
    uidev.id.product = g_uiproduct;
    uidev.id.version = g_uiversion;
    uidev.ff_effects_max = 32;

    /* GAS/BRAKE emulation: always advertise ABS_BRAKE (sc 2) & ABS_GAS (sc 5) */
    if (g_gas_brake_emulation) {
        ioctl(fd, UI_SET_ABSBIT, ABS_BRAKE);
        ioctl(fd, UI_SET_ABSBIT, ABS_GAS);

        /* if not discovered, give them a 0..16384 range */
        if (!g_discoveredAxes[ABS_BRAKE]) {
            uidev.absmin[ABS_BRAKE] = 0;
            uidev.absmax[ABS_BRAKE] = 16384;
        }
        if (!g_discoveredAxes[ABS_GAS]) {
            uidev.absmin[ABS_GAS] = 0;
            uidev.absmax[ABS_GAS] = 16384;
        }
        LOG_FF("create_virtual_controller: emulated ABS_BRAKE/ABS_GAS\n");
    }

    /* fallback ranges */
    setAbsRange(&uidev, ABS_X,     -1800, 1800);
    setAbsRange(&uidev, ABS_Y,     -1800, 1800);
    setAbsRange(&uidev, ABS_Z,     -1800, 1800);
    setAbsRange(&uidev, ABS_RX,    -1800, 1800);
    setAbsRange(&uidev, ABS_RY,    -1800, 1800);
    setAbsRange(&uidev, ABS_BRAKE,     0, 255);
    setAbsRange(&uidev, ABS_GAS,       0, 255);
    setAbsRange(&uidev, ABS_HAT0X,    -1,    1);
    setAbsRange(&uidev, ABS_HAT0Y,    -1,    1);

    /* override with real min/max */
    for (int sc = 0; sc <= ABS_MAX; sc++) {
        if (g_discoveredAxes[sc]) {
            int axis = g_absMap[sc];
            int mn   = getPhysicalAbsMin(sc);
            int mx   = getPhysicalAbsMax(sc);
            uidev.absmin[axis] = mn;
            uidev.absmax[axis] = mx;
            LOG_FF("create_virtual_controller: sc=%d→axis=%d→[%d..%d]\n",
                   sc, axis, mn, mx);
        }
    }

    if (write(fd, &uidev, sizeof(uidev)) < 0 ||
        ioctl(fd, UI_DEV_CREATE)    < 0)
    {
        LOG_FF("create_virtual_controller: UI_DEV_CREATE => %s\n",
               strerror(errno));
        close(fd);
        return -1;
    }

    LOG_FF("create_virtual_controller: success fd=%d\n", fd);
    *fd_out = fd;
    return 0;
}

/*
 * create_virtual_mouse => same as before.
 */
int create_virtual_mouse(int* fd_out)
{
    if (!fd_out)
        return -1;
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0){
        LOG_FF("create_virtual_mouse: open => %s\n", strerror(errno));
        return -1;
    }
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);
    if (gp_enable_mouse_buttons(fd) < 0){
        LOG_FF("create_virtual_mouse: gp_enable_mouse_buttons => fail\n");
        close(fd);
        return -1;
    }
    if (gp_enable_mouse_relaxes(fd) < 0){
        LOG_FF("create_virtual_mouse: gp_enable_mouse_relaxes => fail\n");
        close(fd);
        return -1;
    }
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "GammaPad Virtual Mouse");
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor  = 0x045e;
    uidev.id.product = 0x02ff;
    uidev.id.version = 0x0003;
    if (write(fd, &uidev, sizeof(uidev)) < 0){
        LOG_FF("create_virtual_mouse: write => %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    if (ioctl(fd, UI_DEV_CREATE) < 0){
        LOG_FF("create_virtual_mouse: UI_DEV_CREATE => %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    LOG_FF("create_virtual_mouse: success => fd=%d\n", fd);
    *fd_out = fd;
    return 0;
}

void destroy_virtual_device(int fd)
{
    if (fd < 0)
        return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
