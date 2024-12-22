#include "retrogamepad2xbox.h"
#include <sys/ioctl.h>

int main(void) {
    // Set up the signal handler for SIGBUS
    struct sigaction sa;
    sa.sa_handler = bus_error_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    if (sigaction(SIGBUS, &sa, NULL) == -1) {
        perror("Error setting signal handler for SIGBUS");
        exit(EXIT_FAILURE);
    }

    setupMaps();
    createAnalogSensitvityCSV();
    fprintf(stderr, "Open /dev/uinput...\n");
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (fd < 0) {
        perror("open /dev/uinput");
        return 1;
    }

    fprintf(stderr, "Open /dev/uinput for mouse...\n");
    int mouse_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

    if (mouse_fd < 0) {
        perror("open /dev/uinput for mouse");
        return 1;
    }

    fprintf(stderr, "Set up virtual controller keys and analog options...\n");
    ioctl(fd, UI_SET_EVBIT, EV_KEY); // enable button/key handling

    ioctl(fd, UI_SET_KEYBIT, BTN_A);
    ioctl(fd, UI_SET_KEYBIT, BTN_B);
    ioctl(fd, UI_SET_KEYBIT, BTN_C);
    ioctl(fd, UI_SET_KEYBIT, BTN_X);
    ioctl(fd, UI_SET_KEYBIT, BTN_Y);
    ioctl(fd, UI_SET_KEYBIT, BTN_Z);
    ioctl(fd, UI_SET_KEYBIT, BTN_1);
    ioctl(fd, UI_SET_KEYBIT, BTN_2);
    ioctl(fd, UI_SET_KEYBIT, BTN_TL);
    ioctl(fd, UI_SET_KEYBIT, BTN_TR);
    ioctl(fd, UI_SET_KEYBIT, BTN_TL2);
    ioctl(fd, UI_SET_KEYBIT, BTN_TR2);
    ioctl(fd, UI_SET_KEYBIT, BTN_START);
    ioctl(fd, UI_SET_KEYBIT, BTN_SELECT);
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMBL);
    ioctl(fd, UI_SET_KEYBIT, BTN_THUMBR);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_UP);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_DOWN);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_LEFT);
    ioctl(fd, UI_SET_KEYBIT, BTN_DPAD_RIGHT);
    ioctl(fd, UI_SET_KEYBIT, BTN_BACK);
    ioctl(fd, UI_SET_KEYBIT, BTN_MODE);
    ioctl(fd, UI_SET_KEYBIT, BTN_GAMEPAD);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEDOWN);
    ioctl(fd, UI_SET_KEYBIT, KEY_VOLUMEUP);
    ioctl(fd, UI_SET_KEYBIT, KEY_POWER);

    ioctl(fd, UI_SET_EVBIT, EV_ABS); // enable analog absolute position handling

    setup_abs(fd, ABS_X, -1800, 1800);
    setup_abs(fd, ABS_Y, -1800, 1800);
    setup_abs(fd, ABS_Z, -1800, 1800);
    setup_abs(fd, ABS_RZ, -1800, 1800);
    setup_abs(fd, ABS_GAS, 0, 1);
    setup_abs(fd, ABS_BRAKE, 0, 1);
    setup_abs(fd, ABS_HAT0X, -1, 1);
    setup_abs(fd, ABS_HAT0Y, -1, 1);

    fprintf(stderr, "Set up virtual mouse keys and relative options...\n");
    ioctl(mouse_fd, UI_SET_EVBIT, EV_KEY); // enable button/key handling for mouse
    ioctl(mouse_fd, UI_SET_KEYBIT, BTN_LEFT);
    ioctl(mouse_fd, UI_SET_KEYBIT, BTN_RIGHT);
    ioctl(mouse_fd, UI_SET_RELBIT, REL_WHEEL);
    ioctl(mouse_fd, UI_SET_RELBIT, REL_HWHEEL);

    ioctl(mouse_fd, UI_SET_EVBIT, EV_REL); // enable relative position handling for mouse
    ioctl(mouse_fd, UI_SET_RELBIT, REL_X);
    ioctl(mouse_fd, UI_SET_RELBIT, REL_Y);

    fprintf(stderr, "Assign virtual controller as Microsoft X-Box One S Controller...\n");
    struct uinput_setup setup = {
        .name = "Xbox Wireless Controller",
        .id = {
            .bustype = BUS_USB,
            .vendor = 0x045e,
            .product = 0x02fd,
            .version = 0x0003,
        }
    };

    fprintf(stderr, "Finalize virtual controller configuration...\n");
    if (ioctl(fd, UI_DEV_SETUP, &setup)) {
        perror("UI_DEV_SETUP");
        return 1;
    }

    fprintf(stderr, "Assign virtual mouse as Generic Mouse...\n");
    struct uinput_setup mouse_setup = {
        .name = "Generic Mouse",
        .id = {
            .bustype = BUS_USB,
            .vendor = 0x045e,
            .product = 0x02ff,
            .version = 0x0003,
        }
    };

    fprintf(stderr, "Finalize virtual mouse configuration...\n");
    if (ioctl(mouse_fd, UI_DEV_SETUP, &mouse_setup)) {
        perror("UI_DEV_SETUP for mouse");
        return 1;
    }

    // Unbind retrogame_joypad and rebind
    fprintf(stderr, "Unbinding retrogame_joypad...\n");
    send_shell_command("echo singleadc-joypad > /sys/bus/platform/drivers/singleadc-joypad/unbind");
    send_shell_command("echo singleadc-joypad > /sys/bus/platform/drivers/singleadc-joypad/unbind");
    sleep(3);

    fprintf(stderr, "Create virtual controller uinput device...\n");
    if (ioctl(fd, UI_DEV_CREATE)) {
        perror("UI_DEV_CREATE");
        return 1;
    }

    fprintf(stderr, "Create virtual mouse uinput device...\n");
    if (ioctl(mouse_fd, UI_DEV_CREATE)) {
        perror("UI_DEV_CREATE for mouse");
        return 1;
    }

    fprintf(stderr, "Rebinding retrogame_joypad, force a failure\n");
    send_shell_command("echo singleadc-joypad > /sys/bus/platform/drivers/singleadc-joypad/bind");
    sleep(1);
    fprintf(stderr, "Clean up any left over retrogame_joypad files\n");
    send_shell_command("rm -rf /sys/devices/platform/singleadc-joypad");
    send_shell_command("rm -rf /sys/devices/platform/singleadc-joypad");
    sleep(1);
    fprintf(stderr, "Finally bind the physical retrogame_joypad again\n");
    send_shell_command("echo singleadc-joypad > /sys/bus/platform/drivers/singleadc-joypad/bind");
    sleep(1);

    // Create /dev/input/event# string by using grep to get physical retrogame_joypad event number
    char openrgp[1000] = "/dev/input/";
    strcat(openrgp, send_shell_command(
        "grep -E 'Name|Handlers|Phys=' /proc/bus/input/devices | "
        "grep -A1 retrogame_joypad/ | grep -Eo 'event[0-9]+'"
    ));
    fprintf(stderr, "Physical retrogame_joypad: %s\nReady.\n", openrgp);

    int physical_retrogame_joypad = open(openrgp, O_RDWR | O_NONBLOCK, S_IRUSR | S_IWUSR);
    ioctl(physical_retrogame_joypad, EVIOCGRAB, 1);

    char rgpremove[1000] = "rm ";
    strcat(rgpremove, openrgp);
    send_shell_command(rgpremove);

    char tjpremove[1000] = "rm /dev/input/";
    strcat(tjpremove, send_shell_command(
        "grep -E 'Name|Handlers|Phys=' /proc/bus/input/devices | "
        "grep -A1 input/touch_joypad | grep -Eo 'event[0-9]+'"
    ));
    send_shell_command(tjpremove);

    // Define data structure to capture physical inputs
    struct input_event ie;

    // Create /dev/input/event# string by using grep to get physical gpio-keys event number
    char opengpio[1000] = "/dev/input/";
    strcat(opengpio, send_shell_command(
        "grep -E 'Name|Handlers|Phys=' /proc/bus/input/devices | "
        "grep -A1 gpio-keys/ | grep -Eo 'event[0-9]+'"
    ));
    fprintf(stderr, "Physical gpio_keys: %s\nReady.\n", opengpio);

    int physical_gpio_keys = open(opengpio, O_RDWR | O_NONBLOCK, S_IRUSR | S_IWUSR);
    ioctl(physical_gpio_keys, EVIOCGRAB, 1);
    struct input_event gpioie;

    // Create /dev/input/event# string by using grep to get physical adc-keys event number
    char openadckeys[1000] = "/dev/input/";
    strcat(openadckeys, send_shell_command(
        "grep -E 'Name|Handlers|Phys=' /proc/bus/input/devices | "
        "grep -A1 adc-keys/ | grep -Eo 'event[0-9]+'"
    ));
    fprintf(stderr, "Physical gpio_keys: %s\nReady.\n", openadckeys);

    int physical_adc_keys = open(openadckeys, O_RDWR | O_NONBLOCK, S_IRUSR | S_IWUSR);
    ioctl(physical_adc_keys, EVIOCGRAB, 1);
    struct input_event adckeysie;

    unsigned count = 0;

    int PHYSICAL_BTN_A = 0;
    int PHYSICAL_BTN_B = 0;
    int PHYSICAL_BTN_C = 0;
    int PHYSICAL_BTN_X = 0;
    int PHYSICAL_BTN_Y = 0;
    int PHYSICAL_BTN_Z = 0;
    int PHYSICAL_BTN_TL = 0;
    int PHYSICAL_BTN_TR = 0;
    int PHYSICAL_BTN_TL2 = 0;
    int PHYSICAL_BTN_TR2 = 0;
    int PHYSICAL_BTN_START = 0;
    int PHYSICAL_BTN_SELECT = 0;
    int PHYSICAL_BTN_THUMBL = 0;
    int PHYSICAL_BTN_THUMBR = 0;
    int PHYSICAL_BTN_DPAD_UP = 0;
    int PHYSICAL_BTN_DPAD_DOWN = 0;
    int PHYSICAL_BTN_DPAD_LEFT = 0;
    int PHYSICAL_BTN_DPAD_RIGHT = 0;
    int PHYSICAL_BTN_BACK = 0;
    int PHYSICAL_BTN_HOME = 0;
    int PHYSICAL_BTN_VOLUMEDOWN = 0;
    int PHYSICAL_BTN_VOLUMEUP = 0;
    int PHYSICAL_BTN_POWER = 0;
    int VIRTUAL_BTN_MODE = 0;
    int VIRTUAL_BTN_1 = 0;
    int VIRTUAL_BTN_2 = 0;

    int PHYSICAL_HAT_X = 0;
    int PHYSICAL_HAT_Y = 0;
    int PHYSICAL_ABS_Y = 0;
    int PHYSICAL_ABS_X = 0;
    int PHYSICAL_ABS_Z = 0;
    int PHYSICAL_ABS_RZ = 0;

    int homepressed = 0;
    int backpressed = 0;
    int backcount = 0;
    int backpresscomplete = 0;
    int homepresscomplete = 0;
    int bpresscomplete = 0;

    int screenison = 1;
    int isadjustingbrightness = 0;
    int menutoggleactivated = 0;
    int menutogglecompleted = 0;
    int isfanturnedoffduringsleep = 0;
    int fanison = 0;

    set_performance_mode();
    if (*analog_sensitivity != 0) {
        setAnalogSensitvityTable(*analog_sensitivity);
    }

    while (1) {
        // Update screen status and MMAP variables
        if (count % 250 == 0) {
            updateMapVars();
        }

        if (analog_sensitivity_isupdated_local == 1) {
            *analog_sensitivity_isupdated = 0;
            analog_sensitivity_isupdated_local = 0;
            if (*analog_sensitivity != 0) {
                setAnalogSensitvityTable(*analog_sensitivity);
            }
        }

        read(physical_gpio_keys, &gpioie, sizeof(struct input_event));
        if (gpioie.type == 1) {
            screenison = 1;
            if (debug_messages_enabled == 1) {
                fprintf(stderr, "time:%ld.%06ld\ttype:%u\tcode:%u\tvalue:%d\n",
                        gpioie.time.tv_sec, gpioie.time.tv_usec,
                        gpioie.type, gpioie.code, gpioie.value);
            }
            if (gpioie.code == 114) {
                PHYSICAL_BTN_VOLUMEDOWN = gpioie.value;
            }
            if (gpioie.code == 115) {
                PHYSICAL_BTN_VOLUMEUP = gpioie.value;
            }
            if (gpioie.code == 116) {
                PHYSICAL_BTN_POWER = gpioie.value;
            }
        }

        read(physical_adc_keys, &adckeysie, sizeof(struct input_event));
        if (adckeysie.type == 1 && adckeysie.code == 158) {
            if (debug_messages_enabled == 1) {
                fprintf(stderr, "ADCKEYS time:%ld.%06ld\ttype:%u\tcode:%u\tvalue:%d\n",
                    adckeysie.time.tv_sec, adckeysie.time.tv_usec,
                    adckeysie.type, adckeysie.code, adckeysie.value);
            }
        }

        struct input_event ev[33];
        memset(&ev, 0, sizeof ev);
        read(physical_retrogame_joypad, &ie, sizeof(struct input_event));

        if (ie.type != 0) {
            if (debug_messages_enabled == 1 && ie.value != 127) {
                fprintf(stderr, "time:%ld.%06ld\ttype:%u\tcode:%u\tvalue:%d\n",
                        ie.time.tv_sec, ie.time.tv_usec, ie.type, ie.code, ie.value);
            }
            // L1
            if (ie.code == 310) PHYSICAL_BTN_TL = ie.value;
            // L2
            if (ie.code == 312) PHYSICAL_BTN_TL2 = ie.value;
            // L3
            if (ie.code == 317) PHYSICAL_BTN_THUMBL = ie.value;
            // R1
            if (ie.code == 311) PHYSICAL_BTN_TR = ie.value;
            // R2
            if (ie.code == 313) PHYSICAL_BTN_TR2 = ie.value;
            // R3
            if (ie.code == 318) PHYSICAL_BTN_THUMBR = ie.value;
            // A
            if (ie.code == 304) PHYSICAL_BTN_A = ie.value;
            // B
            if (ie.code == 305) PHYSICAL_BTN_B = ie.value;
            // C
            if (ie.code == 306) PHYSICAL_BTN_C = ie.value;
            // X
            if (ie.code == 307) PHYSICAL_BTN_X = ie.value;
            // Y
            if (ie.code == 308) PHYSICAL_BTN_Y = ie.value;
            // Z
            if (ie.code == 309) PHYSICAL_BTN_Z = ie.value;
            // SELECT
            if (ie.code == 314) PHYSICAL_BTN_SELECT = ie.value;
            // START
            if (ie.code == 315) PHYSICAL_BTN_START = ie.value;
            // BACK
            if (ie.code == 158) PHYSICAL_BTN_BACK = ie.value;
            // HOME
            if (ie.code == 68) PHYSICAL_BTN_HOME = ie.value;
            // MODE - RG503
            if (ie.code == 316) PHYSICAL_BTN_BACK = ie.value;
            // MODE - HYBRID USAGE WITH BACK BUTTON
            if (VIRTUAL_BTN_MODE == 1) {
                PHYSICAL_BTN_BACK = 0;
            }

            // DPAD UP/DOWN
            if (ie.code == 17 || ie.code == 544 || ie.code == 545) {
                if (*dpad_analog_swap == 1 && (ie.code == 17 || ie.code == 544 || ie.code == 545)) {
                    if (ie.code == 17 && ie.value == 1) {
                        PHYSICAL_ABS_Y = 1800;
                    } else if (ie.code == 17 && ie.value == -1) {
                        PHYSICAL_ABS_Y = -1800;
                    } else if (ie.code == 544 && ie.value == 1) {
                        PHYSICAL_ABS_Y = -1800;
                    } else if (ie.code == 545 && ie.value == 1) {
                        PHYSICAL_ABS_Y = 1800;
                    } else {
                        PHYSICAL_ABS_Y = 0;
                    }
                } else {
                    if (ie.code == 17) {
                        PHYSICAL_HAT_Y = ie.value;
                    } else {
                        if (ie.code == 544) {
                            PHYSICAL_HAT_Y = -ie.value;
                        }
                        if (ie.code == 545) {
                            PHYSICAL_HAT_Y = ie.value;
                        }
                    }
                }
            }

            // DPAD LEFT/RIGHT
            if (ie.code == 16 || ie.code == 546 || ie.code == 547) {
                if (*dpad_analog_swap == 1 && (ie.code == 16 || ie.code == 546 || ie.code == 547)) {
                    if (ie.code == 16 && ie.value == 1) {
                        PHYSICAL_ABS_X = 1800;
                    } else if (ie.code == 16 && ie.value == -1) {
                        PHYSICAL_ABS_X = -1800;
                    } else if (ie.code == 546 && ie.value == 1) {
                        PHYSICAL_ABS_X = -1800;
                    } else if (ie.code == 547 && ie.value == 1) {
                        PHYSICAL_ABS_X = 1800;
                    } else {
                        PHYSICAL_ABS_X = 0;
                    }
                } else {
                    if (ie.code == 16) {
                        PHYSICAL_HAT_X = ie.value;
                    } else {
                        if (ie.code == 546) PHYSICAL_HAT_X = -ie.value;
                        if (ie.code == 547) PHYSICAL_HAT_X = ie.value;
                    }
                }
            }

            // LEFT ANALOG Y
            if (ie.code == 1) {
                if (*dpad_analog_swap == 1) {
                    if (ie.value < 1000 && ie.value > -1000) {
                        PHYSICAL_HAT_Y = 0;
                    }
                    if (ie.value >= 1000) {
                        PHYSICAL_HAT_Y = 1;
                    }
                    if (ie.value <= -1000) {
                        PHYSICAL_HAT_Y = -1;
                    }
                } else {
                    if (*analog_sensitivity != 0) {
                        PHYSICAL_ABS_Y = lookupValue(ie.value);
                    } else {
                        PHYSICAL_ABS_Y = ie.value;
                    }
                }
            }

            // LEFT ANALOG X
            if (ie.code == 0) {
                if (*dpad_analog_swap == 1) {
                    if (ie.value < 1000 && ie.value > -1000) {
                        PHYSICAL_HAT_X = 0;
                    }
                    if (ie.value >= 1000) {
                        PHYSICAL_HAT_X = 1;
                    }
                    if (ie.value <= -1000) {
                        PHYSICAL_HAT_X = -1;
                    }
                } else {
                    if (*analog_sensitivity != 0) {
                        PHYSICAL_ABS_X = lookupValue(ie.value);
                    } else {
                        PHYSICAL_ABS_X = ie.value;
                    }
                }
            }

            // RIGHT ANALOG Y
            if (ie.code == 2 || ie.code == 3) {
                if (*analog_sensitivity != 0) {
                    PHYSICAL_ABS_Z = lookupValue(ie.value);
                } else {
                    PHYSICAL_ABS_Z = ie.value;
                }
            }

            // RIGHT ANALOG X
            if (ie.code == 5 || ie.code == 4) {
                if (*analog_sensitivity != 0) {
                    PHYSICAL_ABS_RZ = lookupValue(ie.value);
                } else {
                    PHYSICAL_ABS_RZ = ie.value;
                }
            }
        }

        struct input_event mouse_ev[3];
        memset(&mouse_ev, 0, sizeof mouse_ev);

        if (mouse_mode) {
            // Adjust mouse speed based on X button press
            if (PHYSICAL_BTN_X == 1) {
                mouse_speed = MOUSE_ANALOG_SPEED * 2;
            } else {
                mouse_speed = MOUSE_ANALOG_SPEED;
            }

            // Mouse movement using DPAD
            if (PHYSICAL_HAT_Y == 1) {
                mouse_ev[0].type = EV_REL;
                mouse_ev[0].code = REL_Y;
                mouse_ev[0].value = mouse_speed;
            } else if (PHYSICAL_HAT_Y == -1) {
                mouse_ev[0].type = EV_REL;
                mouse_ev[0].code = REL_Y;
                mouse_ev[0].value = -mouse_speed;
            }

            if (PHYSICAL_HAT_X == 1) {
                mouse_ev[1].type = EV_REL;
                mouse_ev[1].code = REL_X;
                mouse_ev[1].value = mouse_speed;
            } else if (PHYSICAL_HAT_X == -1) {
                mouse_ev[1].type = EV_REL;
                mouse_ev[1].code = REL_X;
                mouse_ev[1].value = -mouse_speed;
            }

            // Mouse movement using LEFT ANALOG
            if (PHYSICAL_ABS_Y > MOUSE_ANALOG_THRESHOLD && (count % 2 == 0)) {
                mouse_ev[0].type = EV_REL;
                mouse_ev[0].code = REL_Y;
                mouse_ev[0].value = round(PHYSICAL_ABS_Y / 500.00 * mouse_speed);
            } else if (PHYSICAL_ABS_Y < -MOUSE_ANALOG_THRESHOLD && (count % 3 == 0)) {
                mouse_ev[0].type = EV_REL;
                mouse_ev[0].code = REL_Y;
                mouse_ev[0].value = round(PHYSICAL_ABS_Y / 500.00 * mouse_speed);
            }

            if (PHYSICAL_ABS_X > MOUSE_ANALOG_THRESHOLD && (count % 3 == 0)) {
                mouse_ev[1].type = EV_REL;
                mouse_ev[1].code = REL_X;
                mouse_ev[1].value = round(PHYSICAL_ABS_X / 500.00 * mouse_speed);
            } else if (PHYSICAL_ABS_X < -MOUSE_ANALOG_THRESHOLD && (count % 3 == 0)) {
                mouse_ev[1].type = EV_REL;
                mouse_ev[1].code = REL_X;
                mouse_ev[1].value = round(PHYSICAL_ABS_X / 500.00 * mouse_speed);
            }

            mouse_ev[2].type = EV_SYN;
            mouse_ev[2].code = SYN_REPORT;
            mouse_ev[2].value = 0;

            if (write(mouse_fd, &mouse_ev, sizeof(mouse_ev)) < 0) {
                perror("write mouse event");
                return 1;
            }

            // Mouse click emulation
            struct input_event mouse_click_ev[2];
            memset(&mouse_click_ev, 0, sizeof mouse_click_ev);

            // Left mouse click
            if (PHYSICAL_BTN_A == 1) {
                mouse_click_ev[0].type = EV_KEY;
                mouse_click_ev[0].code = BTN_LEFT;
                mouse_click_ev[0].value = 1;
                mouse_click_ev[1].type = EV_SYN;
                mouse_click_ev[1].code = SYN_REPORT;
                mouse_click_ev[1].value = 0;
                if (write(mouse_fd, &mouse_click_ev, sizeof(mouse_click_ev)) < 0) {
                    perror("write mouse click event");
                    return 1;
                }
            } else if (PHYSICAL_BTN_A == 0) {
                mouse_click_ev[0].type = EV_KEY;
                mouse_click_ev[0].code = BTN_LEFT;
                mouse_click_ev[0].value = 0;
                mouse_click_ev[1].type = EV_SYN;
                mouse_click_ev[1].code = SYN_REPORT;
                mouse_click_ev[1].value = 0;
                if (write(mouse_fd, &mouse_click_ev, sizeof(mouse_click_ev)) < 0) {
                    perror("write mouse click event");
                    return 1;
                }
            }

            // Right mouse click
            if (PHYSICAL_BTN_Y == 1) {
                mouse_click_ev[0].type = EV_KEY;
                mouse_click_ev[0].code = BTN_RIGHT;
                mouse_click_ev[0].value = 1;
                mouse_click_ev[1].type = EV_SYN;
                mouse_click_ev[1].code = SYN_REPORT;
                mouse_click_ev[1].value = 0;
                if (write(mouse_fd, &mouse_click_ev, sizeof(mouse_click_ev)) < 0) {
                    perror("write mouse click event");
                    return 1;
                }
            } else if (PHYSICAL_BTN_Y == 0) {
                mouse_click_ev[0].type = EV_KEY;
                mouse_click_ev[0].code = BTN_RIGHT;
                mouse_click_ev[0].value = 0;
                mouse_click_ev[1].type = EV_SYN;
                mouse_click_ev[1].code = SYN_REPORT;
                mouse_click_ev[1].value = 0;
                if (write(mouse_fd, &mouse_click_ev, sizeof(mouse_click_ev)) < 0) {
                    perror("write mouse click event");
                    return 1;
                }
            }

            // Send input keyevent 4 for B button
            if (PHYSICAL_BTN_B == 1 && bpresscomplete == 0) {
                send_shell_command("input keyevent 4");
                bpresscomplete = 1;
            }
            if (PHYSICAL_BTN_B == 0) {
                bpresscomplete = 0;
            }

            // Scroll wheel emulation
            struct input_event scroll_ev[4];
            memset(&scroll_ev, 0, sizeof scroll_ev);

            // Vertical
            if ((PHYSICAL_ABS_RZ < -MOUSE_ANALOG_THRESHOLD) && PHYSICAL_ABS_RZ != 0 && count % 20 == 0) {
                scroll_ev[0].type = EV_REL;
                scroll_ev[0].code = REL_WHEEL;
                scroll_ev[0].value = -floor(PHYSICAL_ABS_RZ / 500.00 * mouse_speed);
            } else if ((PHYSICAL_ABS_RZ > MOUSE_ANALOG_THRESHOLD) && PHYSICAL_ABS_RZ != 0 && count % 20 == 0) {
                scroll_ev[0].type = EV_REL;
                scroll_ev[0].code = REL_WHEEL;
                scroll_ev[0].value = -ceil(PHYSICAL_ABS_RZ / 500.00 * mouse_speed);
            } else if (PHYSICAL_BTN_TL == 1 && count % 20 == 0) {
                scroll_ev[0].type = EV_REL;
                scroll_ev[0].code = REL_WHEEL;
                scroll_ev[0].value = 1 * mouse_speed;
            } else if (PHYSICAL_BTN_TR == 1 && PHYSICAL_BTN_SELECT == 0 && count % 20 == 0) {
                scroll_ev[0].type = EV_REL;
                scroll_ev[0].code = REL_WHEEL;
                scroll_ev[0].value = -1 * mouse_speed;
            } else {
                scroll_ev[0].value = 0;
            }
            scroll_ev[0].type = EV_REL;

            scroll_ev[1].type = EV_SYN;
            scroll_ev[1].code = SYN_REPORT;
            scroll_ev[1].value = 0;

            // Horizontal (PHYSICAL_ABS_Z)
            if ((PHYSICAL_ABS_Z < -MOUSE_ANALOG_THRESHOLD) && PHYSICAL_ABS_Z != 0 && count % 20 == 0) {
                scroll_ev[1].type = EV_REL;
                scroll_ev[1].code = REL_HWHEEL;
                scroll_ev[1].value = ceil(PHYSICAL_ABS_Z / 500.00 * mouse_speed);
            } else if ((PHYSICAL_ABS_Z > MOUSE_ANALOG_THRESHOLD) && PHYSICAL_ABS_Z != 0 && count % 20 == 0) {
                scroll_ev[1].type = EV_REL;
                scroll_ev[1].code = REL_HWHEEL;
                scroll_ev[1].value = floor(PHYSICAL_ABS_Z / 500.00 * mouse_speed);
            } else if (PHYSICAL_BTN_TL2 == 1 && count % 20 == 0) {
                scroll_ev[1].type = EV_REL;
                scroll_ev[1].code = REL_HWHEEL;
                scroll_ev[1].value = -1 * mouse_speed;
            } else if (PHYSICAL_BTN_TR2 == 1 && count % 20 == 0) {
                scroll_ev[1].type = EV_REL;
                scroll_ev[1].code = REL_HWHEEL;
                scroll_ev[1].value = 1 * mouse_speed;
            } else {
                scroll_ev[1].value = 0;
            }

            scroll_ev[2].type = EV_SYN;
            scroll_ev[2].code = SYN_REPORT;
            scroll_ev[2].value = 0;

            if (write(mouse_fd, &scroll_ev, sizeof(scroll_ev)) < 0) {
                perror("write mouse scroll event");
                return 1;
            }

        } else {
            // Normal gamepad mode (non-mouse)
            ev[0].type = EV_KEY;
            if (*abxy_layout == 0) ev[0].code = BTN_A; else ev[0].code = BTN_B;
            ev[0].value = PHYSICAL_BTN_A;

            ev[1].type = EV_KEY;
            if (*abxy_layout == 0) ev[1].code = BTN_B; else ev[1].code = BTN_A;
            ev[1].value = PHYSICAL_BTN_B;

            ev[2].type = EV_KEY;    ev[2].code = BTN_TL2;   ev[2].value = PHYSICAL_BTN_TL2;
            ev[3].type = EV_KEY;    ev[3].code = BTN_TR2;   ev[3].value = PHYSICAL_BTN_TR2;
            ev[4].type = EV_ABS;    ev[4].code = ABS_Y;     ev[4].value = (*analog_axis == 0) ? PHYSICAL_ABS_Y : -PHYSICAL_ABS_Y;
            ev[5].type = EV_ABS;    ev[5].code = ABS_X;     ev[5].value = (*analog_axis == 0) ? PHYSICAL_ABS_X : -PHYSICAL_ABS_X;
            ev[6].type = EV_ABS;    ev[6].code = ABS_GAS;   ev[6].value = PHYSICAL_BTN_TR2;
            ev[7].type = EV_ABS;    ev[7].code = ABS_BRAKE; ev[7].value = PHYSICAL_BTN_TL2;
            ev[8].type = EV_KEY;    ev[8].code = 66;        ev[8].value = PHYSICAL_BTN_HOME;
            ev[9].type = EV_KEY;    ev[9].code = BTN_TL;    ev[9].value = PHYSICAL_BTN_TL;
            ev[10].type = EV_ABS;   ev[10].code = ABS_Z;    ev[10].value = (*rightanalog_axis == 0) ? PHYSICAL_ABS_Z : -PHYSICAL_ABS_Z;
            ev[11].type = EV_KEY;   ev[11].code = BTN_TR;   ev[11].value = PHYSICAL_BTN_TR;
            // index 12 is unused in your original code
            ev[13].type = EV_KEY;
            if (*abxy_layout == 0) ev[13].code = BTN_X; else ev[13].code = BTN_Y;
            ev[13].value = PHYSICAL_BTN_X;

            ev[14].type = EV_KEY;
            if (*abxy_layout == 0) ev[14].code = BTN_Y; else ev[14].code = BTN_X;
            ev[14].value = PHYSICAL_BTN_Y;

            // L3 / C
            ev[15].type = EV_KEY;  ev[15].code = BTN_THUMBL;
            ev[15].value = (PHYSICAL_BTN_C == 1 || PHYSICAL_BTN_THUMBL == 1) ? 1 : 0;

            // R3 / Z
            ev[16].type = EV_KEY;  ev[16].code = BTN_THUMBR;
            ev[16].value = (PHYSICAL_BTN_Z == 1 || PHYSICAL_BTN_THUMBR == 1) ? 1 : 0;

            ev[17].type = EV_KEY;  ev[17].code = BTN_SELECT;
            ev[17].value = (VIRTUAL_BTN_MODE == 1) ? VIRTUAL_BTN_MODE : PHYSICAL_BTN_SELECT;

            ev[18].type = EV_KEY;  ev[18].code = BTN_START;  ev[18].value = PHYSICAL_BTN_START;
            ev[19].type = EV_KEY;  ev[19].code = BTN_BACK;   ev[19].value = PHYSICAL_BTN_BACK;
            ev[20].type = EV_KEY;  ev[20].code = BTN_DPAD_UP; ev[20].value = PHYSICAL_BTN_DPAD_UP;
            ev[21].type = EV_ABS;  ev[21].code = ABS_HAT0Y;   ev[21].value = PHYSICAL_HAT_Y;
            ev[22].type = EV_ABS;  ev[22].code = ABS_HAT0X;   ev[22].value = PHYSICAL_HAT_X;
            ev[23].type = EV_ABS;  ev[23].code = ABS_RZ;      ev[23].value = (*rightanalog_axis == 0) ? PHYSICAL_ABS_RZ : -PHYSICAL_ABS_RZ;

            // index 24, 25 are unused
            ev[26].type = EV_KEY;  ev[26].code = BTN_1;      ev[26].value = VIRTUAL_BTN_1;
            ev[27].type = EV_KEY;  ev[27].code = BTN_2;      ev[27].value = VIRTUAL_BTN_2;
            ev[28].type = EV_KEY;  ev[28].code = KEY_POWER;  ev[28].value = PHYSICAL_BTN_POWER;

            if (PHYSICAL_BTN_BACK == 0 && isadjustingbrightness == 0) {
                ev[29].type = EV_KEY; ev[29].code = KEY_VOLUMEDOWN; ev[29].value = PHYSICAL_BTN_VOLUMEDOWN;
                ev[30].type = EV_KEY; ev[30].code = KEY_VOLUMEUP;   ev[30].value = PHYSICAL_BTN_VOLUMEUP;
            }

            ev[31].type = EV_SYN; ev[31].code = SYN_REPORT; ev[31].value = 0;

            if (!mouse_mode) {
                if (write(fd, &ev, sizeof ev) < 0) {
                    perror("write");
                    return 1;
                }
            }

            // 353 series back button
            if (adckeysie.type == 1 && adckeysie.code == 158) {
                PHYSICAL_BTN_BACK = adckeysie.value;
            }

            // back/mode/home logic
            if (PHYSICAL_BTN_BACK == 1) {
                ++backcount;
                backpressed = 1;
            }

            if (backpressed == 1 && backpresscomplete == 0 && homepresscomplete == 0) {
                // If back is pressed + another button => hotkey
                if (PHYSICAL_BTN_BACK == 1 && (
                    PHYSICAL_BTN_VOLUMEUP == 1 ||
                    PHYSICAL_BTN_VOLUMEDOWN == 1 ||
                    PHYSICAL_BTN_A == 1 || PHYSICAL_BTN_B == 1 || PHYSICAL_BTN_C == 1 ||
                    PHYSICAL_BTN_X == 1 || PHYSICAL_BTN_Y == 1 || PHYSICAL_BTN_Z == 1 ||
                    PHYSICAL_BTN_SELECT == 1 || PHYSICAL_BTN_START == 1 ||
                    PHYSICAL_BTN_TL == 1 || PHYSICAL_BTN_TL2 == 1 ||
                    PHYSICAL_BTN_TR == 1 || PHYSICAL_BTN_TR2 == 1 ||
                    PHYSICAL_BTN_THUMBL == 1 || PHYSICAL_BTN_THUMBR == 1 ||
                    PHYSICAL_HAT_X != 0 || PHYSICAL_HAT_Y != 0 ||
                    PHYSICAL_ABS_RZ > 1500 || PHYSICAL_ABS_RZ < -1500 ||
                    PHYSICAL_ABS_X > 1500 || PHYSICAL_ABS_X < -1500 ||
                    PHYSICAL_ABS_Y > 1500 || PHYSICAL_ABS_Y < -1500 ||
                    PHYSICAL_ABS_Z > 1500 || PHYSICAL_ABS_Z < -1500
                )) {
                    VIRTUAL_BTN_MODE = 1;
                    backpresscomplete = 1;
                    homepresscomplete = 1;
                    if (PHYSICAL_BTN_VOLUMEUP == 1 || PHYSICAL_BTN_VOLUMEDOWN == 1 ||
                        PHYSICAL_ABS_RZ > 1500 || PHYSICAL_ABS_RZ < -1500) {
                        isadjustingbrightness = 1;
                        VIRTUAL_BTN_MODE = 0;
                    } else {
                        isadjustingbrightness = 0;
                    }
                }
                // Quick press => back key
                if (PHYSICAL_BTN_BACK == 0 && backcount < 150 && backpresscomplete == 0) {
                    if (get_retroarch_status() == 0) {
                        send_shell_command("input keyevent 4");
                        fprintf(stderr, "RA Not Active\n");
                    } else {
                        menutoggleactivated = 1;
                        fprintf(stderr, "RA Active: Short Press\n");
                    }
                    backpresscomplete = 1;
                    homepresscomplete = 1;
                }
                // Long press => home key
                if (PHYSICAL_BTN_BACK == 1 && backcount > 150 && homepresscomplete == 0) {
                    if (get_retroarch_status() == 0) {
                        send_shell_command("input keyevent 3");
                        fprintf(stderr, "RA Not Active\n");
                    } else {
                        VIRTUAL_BTN_MODE = 1;
                        VIRTUAL_BTN_2 = 1;
                        fprintf(stderr, "RA Active: Long Press\n");
                    }
                    backpresscomplete = 1;
                    homepresscomplete = 1;
                }
            }

            // brightness control
            if (VIRTUAL_BTN_MODE == 0 && isadjustingbrightness == 1 && PHYSICAL_ABS_RZ > 1500 && count % 10 == 0) {
                lcd_brightness(0);
            }
            if (VIRTUAL_BTN_MODE == 0 && isadjustingbrightness == 1 && PHYSICAL_ABS_RZ < -1500 && count % 10 == 0) {
                lcd_brightness(1);
            }
            if (VIRTUAL_BTN_MODE == 0 && isadjustingbrightness == 1 && PHYSICAL_BTN_VOLUMEDOWN == 1 && count % 10 == 0) {
                lcd_brightness(0);
            }
            if (VIRTUAL_BTN_MODE == 0 && isadjustingbrightness == 1 && PHYSICAL_BTN_VOLUMEUP == 1 && count % 10 == 0) {
                lcd_brightness(1);
            }
            if ((ie.code == 158 || ie.code == 316) && ie.value == 0 &&
                PHYSICAL_BTN_VOLUMEUP == 0 && PHYSICAL_BTN_VOLUMEDOWN == 0 &&
                PHYSICAL_ABS_RZ < 1500 && PHYSICAL_ABS_RZ > -1500) {
                isadjustingbrightness = 0;
            }
            if ((adckeysie.code == 158 || adckeysie.code == 316) && adckeysie.value == 0 &&
                PHYSICAL_BTN_VOLUMEUP == 0 && PHYSICAL_BTN_VOLUMEDOWN == 0 &&
                PHYSICAL_ABS_RZ < 1500 && PHYSICAL_ABS_RZ > -1500) {
                isadjustingbrightness = 0;
            }

            if ((ie.code == 158 || ie.code == 316) && ie.value == 0) {
                PHYSICAL_BTN_BACK = 0;
                VIRTUAL_BTN_MODE = 0;
                VIRTUAL_BTN_1 = 0;
                VIRTUAL_BTN_2 = 0;
            }
            if ((adckeysie.code == 158 || adckeysie.code == 316) && adckeysie.value == 0) {
                PHYSICAL_BTN_BACK = 0;
                VIRTUAL_BTN_MODE = 0;
                VIRTUAL_BTN_1 = 0;
                VIRTUAL_BTN_2 = 0;
            }

            if (PHYSICAL_BTN_BACK == 0 && VIRTUAL_BTN_MODE == 0) {
                backpressed = 0;
                backcount = 0;
                backpresscomplete = 0;
                homepresscomplete = 0;
            }

            if (menutoggleactivated == 0 && menutogglecompleted == 1 && count % 10 == 0) {
                VIRTUAL_BTN_MODE = 0;
                VIRTUAL_BTN_1 = 0;
                menutogglecompleted = 0;
            }
            if (menutoggleactivated == 1 && count % 10 == 0) {
                VIRTUAL_BTN_MODE = 1;
                VIRTUAL_BTN_1 = 1;
                menutoggleactivated = 0;
                menutogglecompleted = 1;
            }

            // fan / performance updates
            if (performance_mode_isupdated_local == 1) {
                set_performance_mode();
                performance_mode_isupdated_local = 0;
                *performance_mode_isupdated = 0;
            }

            if (PHYSICAL_BTN_HOME == 1 && homepressed == 0) {
                send_shell_command("input keyevent 3");
                homepressed = 1;
            }
            if (PHYSICAL_BTN_HOME == 0) {
                homepressed = 0;
            }
        }

        // Mouse mode toggle logic: hold SELECT + R1
        if (PHYSICAL_BTN_SELECT && PHYSICAL_BTN_TR) {
            if (!select_and_r1_timer_started) {
                select_and_r1_timer_started = 1;
                select_pressed_time = count;
                r1_pressed_time = count;
            } else if (count - select_pressed_time >= 250 && count - r1_pressed_time >= 250) {
                if (mouse_mode) {
                    disable_mouse_mode();
                    send_shell_command(
                        "su -lp 2000 -c \"am start -a android.intent.action.MAIN "
                        "-e toasttext 'Mouse mode disabled. Hold down Select and R1 to enable.' "
                        "-n bellavita.toast/.MainActivity\""
                    );
                } else {
                    enable_mouse_mode(fd);
                }
                // quick screen flash effect
                send_shell_command("settings put secure accessibility_display_inversion_enabled 1 && sleep 0.5 && settings put secure accessibility_display_inversion_enabled 0");
                select_and_r1_timer_started = 0;
            }
        } else {
            select_and_r1_timer_started = 0;
        }

        msleep(4);
        ++count;
    }

    if (ioctl(fd, UI_DEV_DESTROY)) {
        printf("UI_DEV_DESTROY");
        return 1;
    }
    if (ioctl(mouse_fd, UI_DEV_DESTROY)) {
        printf("UI_DEV_DESTROY for mouse");
        return 1;
    }

    close(fd);
    close(mouse_fd);
    return 0;
}
