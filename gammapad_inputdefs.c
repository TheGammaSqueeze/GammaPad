#include "gammapad_inputdefs.h"

int gp_enable_gamepad_buttons(int fd)
{
    size_t count = sizeof(GAMMAPAD_BUTTON_CODES) / sizeof(GAMMAPAD_BUTTON_CODES[0]);
    for (size_t i = 0; i < count; i++) {
        if (ioctl(fd, UI_SET_KEYBIT, GAMMAPAD_BUTTON_CODES[i]) < 0) {
            LOG_FF("[FF] gp_enable_gamepad_buttons failed code=%u\n", GAMMAPAD_BUTTON_CODES[i]);
            return -1;
        }
    }
    return 0;
}

int gp_enable_gamepad_abs(int fd)
{
    size_t count = sizeof(GAMMAPAD_ABS_CODES) / sizeof(GAMMAPAD_ABS_CODES[0]);
    for (size_t i = 0; i < count; i++) {
        if (ioctl(fd, UI_SET_ABSBIT, GAMMAPAD_ABS_CODES[i]) < 0) {
            LOG_FF("[FF] gp_enable_gamepad_abs failed code=%u\n", GAMMAPAD_ABS_CODES[i]);
            return -1;
        }
    }
    return 0;
}

int gp_enable_mouse_buttons(int fd)
{
    size_t count = sizeof(GAMMAPAD_MOUSE_BUTTONS) / sizeof(GAMMAPAD_MOUSE_BUTTONS[0]);
    for (size_t i = 0; i < count; i++) {
        if (ioctl(fd, UI_SET_KEYBIT, GAMMAPAD_MOUSE_BUTTONS[i]) < 0) {
            LOG_FF("[FF] gp_enable_mouse_buttons failed code=%u\n", GAMMAPAD_MOUSE_BUTTONS[i]);
            return -1;
        }
    }
    return 0;
}

int gp_enable_mouse_relaxes(int fd)
{
    size_t count = sizeof(GAMMAPAD_MOUSE_REL_AXES) / sizeof(GAMMAPAD_MOUSE_REL_AXES[0]);
    for (size_t i = 0; i < count; i++) {
        if (ioctl(fd, UI_SET_RELBIT, GAMMAPAD_MOUSE_REL_AXES[i]) < 0) {
            LOG_FF("[FF] gp_enable_mouse_relaxes failed code=%u\n", GAMMAPAD_MOUSE_REL_AXES[i]);
            return -1;
        }
    }
    return 0;
}
