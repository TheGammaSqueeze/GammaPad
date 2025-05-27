#ifndef GAMMAPAD_CALIBRATION_H
#define GAMMAPAD_CALIBRATION_H

#include <linux/input.h>
#include <limits.h>
#include <stdbool.h>

/*
 * Call once at startup (or lazily on first event) to load any saved
 * calibration centers from /data/GammaPad/CALIBRATION_DATA.
 */
void loadCalibrationData(void);

/*
 * When CALIBRATION_MODE is set to 1, this will snapshot the last
 * raw ABS values and write them out to /data/GammaPad/CALIBRATION_DATA.
 * It also resets CALIBRATION_MODE back to 0.
 */
void triggerCalibration(void);

/*
 * recordRawValue() should be called on every incoming EV_ABS,
 * before any other transform, to keep track of the last raw.
 */
void recordRawValue(int sc, int raw_val);

/*
 * applyCalibration() takes a raw_val and, if we have a saved center
 * for that scancode, will shift it so that the saved center becomes
 * (min+max)/2 again.  Call before inversion/sensitivity/etc.
 */
int applyCalibration(int sc, int raw_val);

#endif // GAMMAPAD_CALIBRATION_H
