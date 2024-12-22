#include "retrogamepad2xbox.h"
#include <sys/ioctl.h> // In case needed for ioctl

void setup_abs(int fd, unsigned chan, int min, int max) {
    if (ioctl(fd, UI_SET_ABSBIT, chan))
        perror("UI_SET_ABSBIT");

    struct uinput_abs_setup s = {
        .code = chan,
        .absinfo = {
            .minimum = min,
            .maximum = max
        },
    };

    if (ioctl(fd, UI_ABS_SETUP, &s))
        perror("UI_ABS_SETUP");
}
