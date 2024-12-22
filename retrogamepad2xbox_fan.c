#include "retrogamepad2xbox.h"

void fanControl() {
    switch (*fan_control) {
    case 0:
        send_shell_command("/system/bin/setfan_off.sh");
        break;
    case 1:
        send_shell_command("/system/bin/setfan_auto.sh");
        break;
    case 2:
        send_shell_command("/system/bin/setfan_cool.sh");
        break;
    case 3:
        send_shell_command("/system/bin/setfan_max.sh");
        break;
    default:
        send_shell_command("/system/bin/setfan_off.sh");
    }
}

int get_cpu_temp() {
    int current_cpu_temp = atoi(send_shell_command(
        "cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | "
        "awk '{sum += $1; n++} END {if (n > 0) print int((sum / n + 99) / 1000)}'"
    ));
    return current_cpu_temp;
}
