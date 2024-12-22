#include "retrogamepad2xbox.h"
#include <sys/ioctl.h>

void enable_mouse_mode(int fd) {
    mouse_mode = 1;
    fprintf(stderr, "Mouse mode enabled\n");
    send_shell_command(
        "su -lp 2000 -c \"am start -a android.intent.action.MAIN "
        "-e toasttext 'Mouse mode enabled. Hold down Select and R1 to disable.' "
        "-n bellavita.toast/.MainActivity\""
    );

    // Clear all button states
    struct input_event ev[33];
    memset(&ev, 0, sizeof ev);

    ev[0].type = EV_KEY;   ev[0].code = BTN_A;           ev[0].value = 0;
    ev[1].type = EV_KEY;   ev[1].code = BTN_B;           ev[1].value = 0;
    ev[2].type = EV_KEY;   ev[2].code = BTN_X;           ev[2].value = 0;
    ev[3].type = EV_KEY;   ev[3].code = BTN_Y;           ev[3].value = 0;
    ev[4].type = EV_KEY;   ev[4].code = BTN_TL;          ev[4].value = 0;
    ev[5].type = EV_KEY;   ev[5].code = BTN_TR;          ev[5].value = 0;
    ev[6].type = EV_KEY;   ev[6].code = BTN_TL2;         ev[6].value = 0;
    ev[7].type = EV_KEY;   ev[7].code = BTN_TR2;         ev[7].value = 0;
    ev[8].type = EV_KEY;   ev[8].code = BTN_SELECT;      ev[8].value = 0;
    ev[9].type = EV_KEY;   ev[9].code = BTN_START;       ev[9].value = 0;
    ev[10].type = EV_KEY;  ev[10].code = BTN_THUMBL;     ev[10].value = 0;
    ev[11].type = EV_KEY;  ev[11].code = BTN_THUMBR;     ev[11].value = 0;
    ev[12].type = EV_KEY;  ev[12].code = BTN_DPAD_UP;    ev[12].value = 0;
    ev[13].type = EV_KEY;  ev[13].code = BTN_DPAD_DOWN;  ev[13].value = 0;
    ev[14].type = EV_KEY;  ev[14].code = BTN_DPAD_LEFT;  ev[14].value = 0;
    ev[15].type = EV_KEY;  ev[15].code = BTN_DPAD_RIGHT; ev[15].value = 0;
    ev[16].type = EV_KEY;  ev[16].code = BTN_BACK;       ev[16].value = 0;
    ev[17].type = EV_KEY;  ev[17].code = BTN_MODE;       ev[17].value = 0;
    ev[18].type = EV_KEY;  ev[18].code = BTN_GAMEPAD;    ev[18].value = 0;
    ev[19].type = EV_KEY;  ev[19].code = KEY_VOLUMEDOWN; ev[19].value = 0;
    ev[20].type = EV_KEY;  ev[20].code = KEY_VOLUMEUP;   ev[20].value = 0;
    ev[21].type = EV_KEY;  ev[21].code = KEY_POWER;      ev[21].value = 0;
    ev[22].type = EV_ABS;  ev[22].code = ABS_X;          ev[22].value = 0;
    ev[23].type = EV_ABS;  ev[23].code = ABS_Y;          ev[23].value = 0;
    ev[24].type = EV_ABS;  ev[24].code = ABS_Z;          ev[24].value = 0;
    ev[25].type = EV_ABS;  ev[25].code = ABS_RZ;         ev[25].value = 0;
    ev[26].type = EV_ABS;  ev[26].code = ABS_GAS;        ev[26].value = 0;
    ev[27].type = EV_ABS;  ev[27].code = ABS_BRAKE;      ev[27].value = 0;
    ev[28].type = EV_ABS;  ev[28].code = ABS_HAT0X;      ev[28].value = 0;
    ev[29].type = EV_ABS;  ev[29].code = ABS_HAT0Y;      ev[29].value = 0;
    ev[30].type = EV_KEY;  ev[30].code = BTN_1;          ev[30].value = 0;
    ev[31].type = EV_KEY;  ev[31].code = BTN_2;          ev[31].value = 0;

    ev[32].type = EV_SYN;  ev[32].code = SYN_REPORT;     ev[32].value = 0;

    if (write(fd, &ev, sizeof ev) < 0) {
        perror("write");
    }
}

void disable_mouse_mode() {
    mouse_mode = 0;
    fprintf(stderr, "Mouse mode disabled\n");
}
