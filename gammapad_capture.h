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
 * Now we do scancode → final code if needed, referencing .kl-based maps.
 */
void forward_physical_event(const struct input_event* ev);

/*
 * We store discovered scancodes for EV_KEY and EV_ABS, plus abs min/max.
 * We expose them so gammapad_controller.c can replicate them.
 */
extern int g_discoveredKeys[KEY_MAX+1];     /* 1 if present, else 0 */
extern int g_discoveredAxes[ABS_MAX+1];     /* 1 if present, else 0 */

/*
 * For min/max:
 */
int getPhysicalAbsMin(int scancode);
int getPhysicalAbsMax(int scancode);

/*
 * For scancode => final code transformations:
 *   If .kl says: key 304 BUTTON_A => we store g_keyMap[304] = BTN_A
 *   If .kl says: axis 0x02 LTRIGGER => we store g_axisMap[2] = ABS_GAS (example).
 */
extern int g_keyMap[KEY_MAX+1];  /* default is same scancode if not mapped */
extern int g_absMap[ABS_MAX+1];  /* default is same scancode if not mapped */

#endif // GAMMAPAD_CAPTURE_H
