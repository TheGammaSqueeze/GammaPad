#include "gammapad_calibration.h"
#include "gammapad_capture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <linux/input.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#ifndef CONFIG_DIR
#define CONFIG_DIR "/data/GammaPad"
#endif

#define CALIB_FILE CONFIG_DIR "/CALIBRATION_DATA"
#define MODE_FILE  CONFIG_DIR "/CALIBRATION_MODE"

static int calibrationOffset[ABS_MAX+1];
static bool calibrated[ABS_MAX+1];
static int lastRawValue[ABS_MAX+1];
static bool calibrationDataLoaded = false;

/* Helpers to create CONFIG_DIR if needed */
static void ensureConfigDir(void) {
    struct stat st;
    if (stat(CONFIG_DIR, &st) != 0) {
        mkdir(CONFIG_DIR, 0777);
    }
    chmod(CONFIG_DIR, 0777);
}

/* Load saved centers into calibrationOffset[] */
void loadCalibrationData(void) {
    if (calibrationDataLoaded) return;
    ensureConfigDir();
    FILE *f = fopen(CALIB_FILE, "r");
    if (!f) {
        /* no prior calibration */
        calibrationDataLoaded = true;
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        int sc, off;
        if (sscanf(line, "%d %d", &sc, &off) == 2
            && sc >= 0 && sc <= ABS_MAX)
        {
            calibrationOffset[sc] = off;
            calibrated[sc] = true;
        }
    }
    fclose(f);
    calibrationDataLoaded = true;
}

/* Snapshot lastRawValue[] into file, reset MODE_FILE to "0" */
void triggerCalibration(void) {
    ensureConfigDir();
    FILE *f = fopen(CALIB_FILE, "w");
    if (!f) {
        fprintf(stderr, "[Calib] Failed to open %s for writing: %s\n",
                CALIB_FILE, strerror(errno));
        return;
    }
    for (int sc = 0; sc <= ABS_MAX; sc++) {
        /* only save axes we’ve seen */
        if (lastRawValue[sc] || calibrated[sc]) {
            fprintf(f, "%d %d\n", sc, lastRawValue[sc]);
            calibrationOffset[sc] = lastRawValue[sc];
            calibrated[sc] = true;
        }
    }
    fclose(f);
    chmod(CALIB_FILE, 0777);

    /* reset mode */
    f = fopen(MODE_FILE, "w");
    if (f) {
        fputs("0\n", f);
        fclose(f);
        chmod(MODE_FILE, 0777);
    }
    fprintf(stderr, "[Calib] Calibration complete — data written to %s\n", CALIB_FILE);
}

/* Called on every ABS event to remember the raw value */
void recordRawValue(int sc, int raw_val) {
    if (sc < 0 || sc > ABS_MAX) return;
    lastRawValue[sc] = raw_val;
}

/* Returns adjusted value (or raw_val if not calibrated) */
int applyCalibration(int sc, int raw_val) {
    if (!calibrationDataLoaded) loadCalibrationData();
    if (sc < 0 || sc > ABS_MAX || !calibrated[sc])
        return raw_val;
    /* shift so saved center → (min+max)/2 */
    int mn = getPhysicalAbsMin(sc);
    int mx = getPhysicalAbsMax(sc);
    int center = (mn + mx) / 2;
    int shifted = raw_val - calibrationOffset[sc] + center;
    if (shifted < mn) shifted = mn;
    if (shifted > mx) shifted = mx;
    return shifted;
}
