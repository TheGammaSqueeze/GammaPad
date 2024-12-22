#include "retrogamepad2xbox.h"
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

int openAndMap(const char *filePath, int **shared_data, int *fd) {
    *fd = open(filePath, O_RDWR | O_CREAT, 0666);
    if (*fd == -1) {
        fprintf(stderr, "Error opening file: %s\n", filePath);
        return -1;
    }

    if (chmod(filePath, 0666) == -1) {
        fprintf(stderr, "Error setting file permissions: %s\n", filePath);
        close(*fd);
        return -1;
    }

    ftruncate(*fd, BUFFER_SIZE);

    *shared_data = mmap(NULL, BUFFER_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, *fd, 0);
    if (*shared_data == MAP_FAILED) {
        fprintf(stderr, "Error mapping file: %s\n", filePath);
        close(*fd);
        return -1;
    }

    return 0;
}

void setupMaps() {
    struct stat st = {0};
    if (stat(DIRECTORY_PATH, &st) == -1) {
        if (mkdir(DIRECTORY_PATH, 0777) == -1) {
            fprintf(stderr, "Error creating directory: %s\n", DIRECTORY_PATH);
            return;
        }
        chmod(DIRECTORY_PATH, 0777);
    }

    char filePath[255];

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ABXY_LAYOUT"), &abxy_layout, &fd_abxy_layout);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ABXY_LAYOUT_ISUPDATED"), &abxy_layout_isupdated, &fd_abxy_layout_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "PERFORMANCE_MODE"), &performance_mode, &fd_performance_mode);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "PERFORMANCE_MODE_ISUPDATED"), &performance_mode_isupdated, &fd_performance_mode_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ANALOG_SENSITIVITY"), &analog_sensitivity, &fd_analog_sensitivity);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ANALOG_SENSITIVITY_ISUPDATED"), &analog_sensitivity_isupdated, &fd_analog_sensitivity_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ANALOG_AXIS"), &analog_axis, &fd_analog_axis);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "ANALOG_AXIS_ISUPDATED"), &analog_axis_isupdated, &fd_analog_axis_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "RIGHTANALOG_AXIS"), &rightanalog_axis, &fd_rightanalog_axis);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "RIGHTANALOG_AXIS_ISUPDATED"), &rightanalog_axis_isupdated, &fd_rightanalog_axis_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "DPAD_ANALOG_SWAP"), &dpad_analog_swap, &fd_dpad_analog_swap);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "DPAD_ANALOG_SWAP_ISUPDATED"), &dpad_analog_swap_isupdated, &fd_dpad_analog_swap_isupdated);

    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "FAN_CONTROL"), &fan_control, &fd_fan_control);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "FAN_CONTROL_ISUPDATED"), &fan_control_isupdated, &fd_fan_control_isupdated);
    openAndMap(strcat(strcpy(filePath, DIRECTORY_PATH), "FAN_CONTROL_ISENABLED"), &fan_control_isenabled, &fd_fan_control_isenabled);
}

void updateMapVars() {
    abxy_layout_isupdated_local = *abxy_layout_isupdated;
    performance_mode_isupdated_local = *performance_mode_isupdated;
    analog_sensitivity_isupdated_local = *analog_sensitivity_isupdated;
    analog_axis_isupdated_local = *analog_axis_isupdated;
    rightanalog_axis_isupdated_local = *rightanalog_axis_isupdated;
    dpad_analog_swap_isupdated_local = *dpad_analog_swap_isupdated;
    fan_control_isupdated_local = *fan_control_isupdated;
    fan_control_isenabled_local = *fan_control_isenabled;
}
