#include "gammapad.h"
#include "gammapad_inputdefs.h"

int create_virtual_controller(int* fd_out)
{
    if (!fd_out) return -1;

    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    /* Enable events: EV_KEY, EV_ABS, EV_FF for a rumble-capable gamepad. */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_ABS);
    ioctl(fd, UI_SET_EVBIT, EV_FF);

    /* Advertise some FF effect types. */
    ioctl(fd, UI_SET_FFBIT, FF_RUMBLE);
    ioctl(fd, UI_SET_FFBIT, FF_PERIODIC);
    ioctl(fd, UI_SET_FFBIT, FF_CONSTANT);
    ioctl(fd, UI_SET_FFBIT, FF_GAIN);
    ioctl(fd, UI_SET_FFBIT, FF_RAMP);
    ioctl(fd, UI_SET_FFBIT, FF_SPRING);
    ioctl(fd, UI_SET_FFBIT, FF_DAMPER);
    ioctl(fd, UI_SET_FFBIT, FF_INERTIA);

    if (gp_enable_gamepad_buttons(fd) < 0) {
        close(fd);
        return -1;
    }
    if (gp_enable_gamepad_abs(fd) < 0) {
        close(fd);
        return -1;
    }

    /* Fill out a uinput_user_dev with IDs and effect count. */
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));

    snprintf(uidev.name, UINPUT_MAX_NAME_SIZE, "GammaPad Virtual Controller");
    uidev.id.bustype = BUS_USB;
    /* Example IDs, can be changed. */
    uidev.id.vendor  = 0x045e; /* “Microsoft-ish” vendor ID. */
    uidev.id.product = 0x02fd; /* Some product code. */
    uidev.id.version = 0x0003;
    uidev.ff_effects_max = 32; /* how many effects we can handle at once. */

    /* Some default ABS ranges for typical sticks/triggers. */
    uidev.absmin[ABS_X] = -32768;   uidev.absmax[ABS_X] = 32767;
    uidev.absmin[ABS_Y] = -32768;   uidev.absmax[ABS_Y] = 32767;
    uidev.absmin[ABS_Z]  = -32768;  uidev.absmax[ABS_Z]  = 32767;
    uidev.absmin[ABS_RZ] = -32768;  uidev.absmax[ABS_RZ] = 32767;
    /* For triggers. */
    uidev.absmin[ABS_GAS]   = 0;    uidev.absmax[ABS_GAS]   = 255;
    uidev.absmin[ABS_BRAKE] = 0;    uidev.absmax[ABS_BRAKE] = 255;
    /* D-pad as hat. */
    uidev.absmin[ABS_HAT0X] = -1;   uidev.absmax[ABS_HAT0X] = 1;
    uidev.absmin[ABS_HAT0Y] = -1;   uidev.absmax[ABS_HAT0Y] = 1;

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    *fd_out = fd;
    return 0;
}

int create_virtual_mouse(int* fd_out)
{
    if (!fd_out) return -1;

    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    /* Mouse needs EV_KEY (buttons) and EV_REL (rel movement). */
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_REL);

    if (gp_enable_mouse_buttons(fd) < 0) {
        close(fd);
        return -1;
    }
    if (gp_enable_mouse_relaxes(fd) < 0) {
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

    if (write(fd, &uidev, sizeof(uidev)) < 0) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, UI_DEV_CREATE) < 0) {
        close(fd);
        return -1;
    }

    *fd_out = fd;
    return 0;
}

void destroy_virtual_device(int fd)
{
    if (fd < 0) return;
    ioctl(fd, UI_DEV_DESTROY);
    close(fd);
}
