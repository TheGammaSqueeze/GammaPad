#ifndef GAMMAPAD_CAPTURE_H
#define GAMMAPAD_CAPTURE_H

#include "gammapad.h"
#include <linux/input.h>

/*
 * Open the physical device for capturing, attempt to remove the node,
 * grab the device, and identify driver for future unbind/rebind.
 */
int open_physical_device(const char* device_path);

/*
 * Forwards relevant events (EV_KEY or EV_ABS) to the global 'controllerFd'.
 */
void forward_physical_event(const struct input_event* ev);

#endif // GAMMAPAD_CAPTURE_H
