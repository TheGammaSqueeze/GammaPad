#include "retrogamepad2xbox.h"

// Global variables need to be defined in exactly one .c file:
const int debug_messages_enabled = 0;

int * abxy_layout, * abxy_layout_isupdated, abxy_layout_isupdated_local;
int * performance_mode, * performance_mode_isupdated, performance_mode_isupdated_local;
int * analog_sensitivity, * analog_sensitivity_isupdated, analog_sensitivity_isupdated_local;
int * analog_axis, * analog_axis_isupdated, analog_axis_isupdated_local;
int * rightanalog_axis, * rightanalog_axis_isupdated, rightanalog_axis_isupdated_local;
int * dpad_analog_swap, * dpad_analog_swap_isupdated, dpad_analog_swap_isupdated_local;
int * fan_control, * fan_control_isupdated, fan_control_isupdated_local, * fan_control_isenabled, fan_control_isenabled_local;

int fd_abxy_layout, fd_abxy_layout_isupdated;
int fd_performance_mode, fd_performance_mode_isupdated;
int fd_analog_sensitivity, fd_analog_sensitivity_isupdated;
int fd_analog_axis, fd_analog_axis_isupdated;
int fd_rightanalog_axis, fd_rightanalog_axis_isupdated;
int fd_dpad_analog_swap, fd_dpad_analog_swap_isupdated;
int fd_fan_control, fd_fan_control_isupdated, fd_fan_control_isenabled;

// Lookup table
LookupEntry *lookupTable = NULL;
int lookupTableSize = 0;

// Mouse mode
int mouse_mode = 0;
int select_pressed_time = 0;
int r1_pressed_time = 0;
int select_and_r1_timer_started = 0;
int mouse_speed = MOUSE_ANALOG_THRESHOLD;
