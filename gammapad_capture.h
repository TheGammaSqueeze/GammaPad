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
 * In case other files need them, add function prototypes:
 * parse_android_keylayout_file_if_needed, parseKeyLayoutLine, discoverKeys, discoverAxes.
 * That way, the compiler knows their signatures *before* they're called in .c
 */
#ifdef __ANDROID__
void parse_android_keylayout_file_if_needed(int fd);
#endif

void parseKeyLayoutLine(const char* line);

void discoverKeys(int fd);
void discoverAxes(int fd);

/*
 * Accessors for raw min/max used by gammapad_controller.c
 */
int getPhysicalAbsMin(int scancode);
int getPhysicalAbsMax(int scancode);

#endif // GAMMAPAD_CAPTURE_H
