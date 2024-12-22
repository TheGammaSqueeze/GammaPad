#include "retrogamepad2xbox.h"
#include <sys/ioctl.h> // For completeness

char *send_shell_command(char *shellcmd) {
    FILE *shell_cmd_pipe;
    static char cmd_output[5000];
    shell_cmd_pipe = popen(shellcmd, "r");
    if (NULL == shell_cmd_pipe) {
        perror("pipe");
        exit(1);
    }
    fgets(cmd_output, sizeof(cmd_output), shell_cmd_pipe);
    // Remove trailing newline
    cmd_output[strcspn(cmd_output, "\r\n")] = 0;
    pclose(shell_cmd_pipe);

    return cmd_output;
}

int lcd_brightness(int value) {
    int current_brightness = atoi(send_shell_command("settings get system screen_brightness"));
    if (debug_messages_enabled == 1) {
        fprintf(stderr, "Current brightness: %i\n", current_brightness);
    }

    if (value == 1 && current_brightness < 255) {
        current_brightness += 10;
        if (current_brightness > 255) {
            current_brightness = 255;
        }
    }
    if (value == 0 && current_brightness > 1) {
        current_brightness -= 10;
        if (current_brightness < 1) {
            current_brightness = 1;
        }
    }

    char new_brightness[4];
    sprintf(new_brightness, "%d", current_brightness);

    char set_brightness_cmd[100] = "settings put system screen_brightness ";
    strcat(set_brightness_cmd, new_brightness);

    send_shell_command(set_brightness_cmd);

    if (debug_messages_enabled == 1) {
        fprintf(stderr, "New brightness: %i\n", current_brightness);
    }
    return current_brightness;
}

int get_retroarch_status() {
    // Check the current value via the shell command, return 0 if not running
    int current_retroarch_status = atoi(send_shell_command(
        "dumpsys activity activities | grep VisibleActivityProcess | grep retroarch &> /dev/null; "
        "if [ $? -eq 0 ]; then echo 1; else echo 0; fi"
    ));

    if (debug_messages_enabled == 1) {
        fprintf(stderr, "Retroarch running status: %i\n", current_retroarch_status);
    }

    return current_retroarch_status;
}

void set_performance_mode() {
    switch (*performance_mode) {
    case 0:
        send_shell_command("/system/bin/setclock_max.sh");
        break;
    case 1:
        send_shell_command("/system/bin/setclock_stock.sh");
        break;
    case 2:
        send_shell_command("/system/bin/setclock_powersave.sh");
        break;
    default:
        send_shell_command("/system/bin/setclock_max.sh");
    }
}

int get_screen_status() {
    int screenstatus = atoi(send_shell_command("cat /sys/devices/platform/backlight/backlight/backlight/brightness"));
    if (screenstatus != 0) {
        screenstatus = 1;
    } else {
        screenstatus = 0;
    }
    return screenstatus;
}

int get_fan_status() {
    int fanstatus = atoi(send_shell_command("cat /sys/devices/platform/singleadc-joypad/fan_power"));
    if (fanstatus != 0) {
        fanstatus = 1;
    } else {
        fanstatus = 0;
    }
    return fanstatus;
}
