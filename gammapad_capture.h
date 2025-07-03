#ifndef GAMMAPAD_CAPTURE_H
#define GAMMAPAD_CAPTURE_H

#include "gammapad.h"
#include <linux/input.h>

/*
 * Open the physical device for capturing, attempt to parse driver info,
 * parse .kl, discover scancodes, etc. Returns fd or -1 on error.
 */
int open_physical_device(const char* device_path);

/*
 * Forwards relevant events (EV_KEY or EV_ABS) to the global 'controllerFd',
 * doing scancode => final code transforms if .kl says so.
 */
void forward_physical_event(const struct input_event* ev);

/*
 * parse_android_keylayout_file_if_needed, parseKeyLayoutLine
 * used to handle .kl parsing for each device.
 */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd);
#endif
void parseKeyLayoutLine(const char* line);

/*
 * aggregator discover => merges scancodes from physical devices
 *   - The first device is "primary" =>  discoverKeys(fd, 1)
 *   - Subsequent devices => discoverKeys(fd, 0)
 */
void discoverKeys(int fd, int isPrimary);
void discoverAxes(int fd);

/*
 * Accessors for raw min/max used by gammapad_controller.c
 */
int getPhysicalAbsMin(int scancode);
int getPhysicalAbsMax(int scancode);

/*
 * removePrimaryPhysicalNode():
 *   Called AFTER we create the virtual pad, so we can do
 *   "rm -f /dev/input/eventX" for the primary device. 
 */
void removePrimaryPhysicalNode(void);

/*
 * We'll also expose a function to run unbindAndRebind at exit 
 * so it can be called from both the destructor and gammapad_main.
 */
void unbindAndRebind(void);

/*
 * Unbind only the primary device’s driver (3×), but do not re-bind.
 */
void unbindPrimaryDriver(void);

/*
 * Bind only the primary device’s driver (3×).
 */
void bindPrimaryDriver(void);

#endif // GAMMAPAD_CAPTURE_H
