#include "gammapad.h"
#include "gammapad_inputdefs.h"

/* External function to schedule events (declared in gammapad_main.c). */
extern void scheduleEvent(int code, int isKey, int value, unsigned long long durationMs);

/*
 * parseCommand:
 * -------------
 * Parses user commands from the console and issues corresponding events.
 */
void parseCommand(const char* line)
{
    char cmd[32], arg1[32], arg2[32], arg3[32];
    memset(cmd, 0, sizeof(cmd));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));
    memset(arg3, 0, sizeof(arg3));

    int parts = sscanf(line, "%31s %31s %31s %31s", cmd, arg1, arg2, arg3);
    if (parts < 1) {
        return;
    }
    if (!strcasecmp(cmd, "exit")) {
        return;
    }
    if (!strcasecmp(cmd, "press") && parts >= 2) {
        unsigned long long dur = 3000; // default
        if (parts >= 3) {
            unsigned long long tmp = strtoull(arg2, NULL, 10);
            if (tmp > 0) dur = tmp;
        }
        // Map button names to codes
        if (!strcasecmp(arg1, "select")) {
            scheduleEvent(BTN_SELECT, 1, 1, dur);
        } else if (!strcasecmp(arg1, "start")) {
            scheduleEvent(BTN_START, 1, 1, dur);
        } else if (!strcasecmp(arg1, "up")) {
            scheduleEvent(ABS_HAT0Y, 0, -1, dur);
        } else if (!strcasecmp(arg1, "down")) {
            scheduleEvent(ABS_HAT0Y, 0, 1, dur);
        } else if (!strcasecmp(arg1, "left")) {
            scheduleEvent(ABS_HAT0X, 0, -1, dur);
        } else if (!strcasecmp(arg1, "right")) {
            scheduleEvent(ABS_HAT0X, 0, 1, dur);
        } else if (!strcasecmp(arg1, "z")) {
            scheduleEvent(BTN_Z, 1, 1, dur);
        } else if (!strcasecmp(arg1, "c")) {
            scheduleEvent(BTN_C, 1, 1, dur);
        } else if (!strcasecmp(arg1, "a")) {
            scheduleEvent(BTN_A, 1, 1, dur);
        } else if (!strcasecmp(arg1, "b")) {
            scheduleEvent(BTN_B, 1, 1, dur);
        } else if (!strcasecmp(arg1, "x")) {
            scheduleEvent(BTN_X, 1, 1, dur);
        } else if (!strcasecmp(arg1, "y")) {
            scheduleEvent(BTN_Y, 1, 1, dur);
        } else if (!strcasecmp(arg1, "l1")) {
            scheduleEvent(BTN_TL, 1, 1, dur);
        } else if (!strcasecmp(arg1, "l2")) {
            scheduleEvent(BTN_TL2, 1, 1, dur);
        } else if (!strcasecmp(arg1, "l3")) {
            scheduleEvent(BTN_THUMBL, 1, 1, dur);
        } else if (!strcasecmp(arg1, "r1")) {
            scheduleEvent(BTN_TR, 1, 1, dur);
        } else if (!strcasecmp(arg1, "r2")) {
            scheduleEvent(BTN_TR2, 1, 1, dur);
        } else if (!strcasecmp(arg1, "r3")) {
            scheduleEvent(BTN_THUMBR, 1, 1, dur);
        } else if (!strcasecmp(arg1, "back")) {
            scheduleEvent(BTN_BACK, 1, 1, dur);
        } else if (!strcasecmp(arg1, "mode")) {
            scheduleEvent(BTN_MODE, 1, 1, dur);
        } else if (!strcasecmp(arg1, "gamepad")) {
            scheduleEvent(BTN_GAMEPAD, 1, 1, dur);
        } else if (!strcasecmp(arg1, "volumedown")) {
            scheduleEvent(KEY_VOLUMEDOWN, 1, 1, dur);
        } else if (!strcasecmp(arg1, "volumeup")) {
            scheduleEvent(KEY_VOLUMEUP, 1, 1, dur);
        } else if (!strcasecmp(arg1, "power")) {
            scheduleEvent(KEY_POWER, 1, 1, dur);
        } else if (!strcasecmp(arg1, "1")) {
            scheduleEvent(BTN_1, 1, 1, dur);
        } else if (!strcasecmp(arg1, "2")) {
            scheduleEvent(BTN_2, 1, 1, dur);
        }
    }
    else if (!strcasecmp(cmd, "push") && parts >= 3) {
        int val = atoi(arg2);
        unsigned long long dur = 3000; // default
        if (parts >= 4) {
            unsigned long long tmp = strtoull(arg3, NULL, 10);
            if (tmp > 0) dur = tmp;
        }
        if (!strcasecmp(arg1, "abs_x")) {
            scheduleEvent(ABS_X, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_y")) {
            scheduleEvent(ABS_Y, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_z")) {
            scheduleEvent(ABS_Z, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_rz")) {
            scheduleEvent(ABS_RZ, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_gas")) {
            scheduleEvent(ABS_GAS, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_brake")) {
            scheduleEvent(ABS_BRAKE, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_hat0x")) {
            scheduleEvent(ABS_HAT0X, 0, val, dur);
        } else if (!strcasecmp(arg1, "abs_hat0y")) {
            scheduleEvent(ABS_HAT0Y, 0, val, dur);
        }
    }
    // else: unknown or incomplete command, ignore quietly
}
