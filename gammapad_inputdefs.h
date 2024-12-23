#ifndef GAMMAPAD_INPUTDEFS_H
#define GAMMAPAD_INPUTDEFS_H

#include "gammapad.h"

/* Gamepad buttons that might be used on typical controllers (Xbox-like layout). */
static const unsigned int GAMMAPAD_BUTTON_CODES[] = {
    BTN_A, BTN_B, BTN_C,
    BTN_X, BTN_Y, BTN_Z,
    BTN_TL, BTN_TR,
    BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START,
    BTN_THUMBL, BTN_THUMBR,
    BTN_DPAD_UP, BTN_DPAD_DOWN,
    BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
    BTN_BACK, BTN_MODE,
    BTN_GAMEPAD,
    KEY_VOLUMEDOWN, KEY_VOLUMEUP, KEY_POWER,
    BTN_1, BTN_2
};

/* Common axes for analog sticks or triggers. */
static const unsigned int GAMMAPAD_ABS_CODES[] = {
    ABS_X, ABS_Y,
    ABS_Z, ABS_RZ,
    ABS_GAS, ABS_BRAKE,
    ABS_HAT0X, ABS_HAT0Y
};

/* Mouse buttons (including middle button). */
static const unsigned int GAMMAPAD_MOUSE_BUTTONS[] = {
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
};

/* Mouse relative movement and scrolling. */
static const unsigned int GAMMAPAD_MOUSE_REL_AXES[] = {
    REL_X, REL_Y, REL_WHEEL, REL_HWHEEL
};

/* Prototypes for enabling these codes on a /dev/uinput device. */
int gp_enable_gamepad_buttons(int fd);
int gp_enable_gamepad_abs(int fd);
int gp_enable_mouse_buttons(int fd);
int gp_enable_mouse_relaxes(int fd);

#endif /* GAMMAPAD_INPUTDEFS_H */
