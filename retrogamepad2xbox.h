#ifndef RETROGAMEPAD2XBOX_H
#define RETROGAMEPAD2XBOX_H

//------------------- INCLUDES ---------------------//
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/file.h>
#include <signal.h>
#include <math.h> // For ceil/floor/round

//------------------- DEFINES ---------------------//
#define BUFFER_SIZE sizeof(int)
#define DIRECTORY_PATH "/data/rgp2xbox/"
#define msleep(ms) usleep((ms) * 1000)
#define RETRY_DELAY 5000 // Retry delay in microseconds (5 milliseconds)

#define MOUSE_ANALOG_THRESHOLD 80
#define MOUSE_ANALOG_SPEED 1

//------------------- GLOBALS ---------------------//
extern const int debug_messages_enabled;

extern int * abxy_layout, * abxy_layout_isupdated, abxy_layout_isupdated_local;
extern int * performance_mode, * performance_mode_isupdated, performance_mode_isupdated_local;
extern int * analog_sensitivity, * analog_sensitivity_isupdated, analog_sensitivity_isupdated_local;
extern int * analog_axis, * analog_axis_isupdated, analog_axis_isupdated_local;
extern int * rightanalog_axis, * rightanalog_axis_isupdated, rightanalog_axis_isupdated_local;
extern int * dpad_analog_swap, * dpad_analog_swap_isupdated, dpad_analog_swap_isupdated_local;
extern int * fan_control, * fan_control_isupdated, fan_control_isupdated_local, * fan_control_isenabled, fan_control_isenabled_local;

extern int fd_abxy_layout, fd_abxy_layout_isupdated;
extern int fd_performance_mode, fd_performance_mode_isupdated;
extern int fd_analog_sensitivity, fd_analog_sensitivity_isupdated;
extern int fd_analog_axis, fd_analog_axis_isupdated;
extern int fd_rightanalog_axis, fd_rightanalog_axis_isupdated;
extern int fd_dpad_analog_swap, fd_dpad_analog_swap_isupdated;
extern int fd_fan_control, fd_fan_control_isupdated, fd_fan_control_isenabled;

//------------------- STRUCT & LOOKUP TABLE ---------------------//
typedef struct {
    int firstColumn;
    int secondColumn;
} LookupEntry;

extern LookupEntry *lookupTable;
extern int lookupTableSize;

//------------------- MOUSE MODE GLOBALS ---------------------//
extern int mouse_mode;
extern int select_pressed_time;
extern int r1_pressed_time;
extern int select_and_r1_timer_started;
extern int mouse_speed;

//------------------- FUNCTION PROTOTYPES ---------------------//

// retrogamepad2xbox_signal.c
void bus_error_handler(int sig);

// retrogamepad2xbox_abs.c
void setup_abs(int fd, unsigned chan, int min, int max);

// retrogamepad2xbox_shell.c
char * send_shell_command(char * shellcmd);
int lcd_brightness(int value);
int get_retroarch_status(void);
void set_performance_mode(void);
int get_screen_status(void);
int get_fan_status(void);

// retrogamepad2xbox_maps.c
int openAndMap(const char * filePath, int ** shared_data, int * fd);
void setupMaps(void);
void updateMapVars(void);

// retrogamepad2xbox_fan.c
void fanControl(void);
int get_cpu_temp(void);

// retrogamepad2xbox_analog.c
void createAnalogSensitvityCSV(void);
void clearLookupTable(void);
void readCSVAndBuildLookup(const char *filename);
int lookupValue(int value);
void setAnalogSensitvityTable(int mode);

// retrogamepad2xbox_mouse.c
void enable_mouse_mode(int fd);
void disable_mouse_mode(void);

// retrogamepad2xbox_main.c
int main(void);

#endif // RETROGAMEPAD2XBOX_H
