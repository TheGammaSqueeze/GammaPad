#ifndef GAMMAPAD_H
#define GAMMAPAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <ctype.h>
#include <sys/time.h>
#include <pthread.h>
#include "timestamp.h" 

/*
 * If you want more logs, compile with -DGAMMAPAD_VERBOSE_LOGGING=1
 */
#ifndef GAMMAPAD_VERBOSE_LOGGING
#define GAMMAPAD_VERBOSE_LOGGING 0
#endif

#if GAMMAPAD_VERBOSE_LOGGING
  /* Force‐Feedback logs now get timestamped */
  #define LOG_FF(fmt, ...) log_ts(fmt, ##__VA_ARGS__)
#else
  #define LOG_FF(fmt, ...) /* no-op */
#endif

/*
 * Sleep in milliseconds.
 */
static inline void msleep(unsigned int ms)
{
    usleep(ms * 1000U);
}

/*
 * Get current time in milliseconds since the epoch.
 */
static inline unsigned long long getTimeMs(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + (tv.tv_usec / 1000ULL);
}

/* Map a physical FF effect ID back to the original aggregatorKid */
int getAggregatorKidForRealDevId(int realDevId);

/*
 * Extern: controllerFd is defined in gammapad_main.c
 */
extern int controllerFd;

/*
 * Also expose g_physicalFd so we can read absmin/absmax from the captured device.
 */
extern int g_physicalFd;

/*
 * Global FF effect adjustment variables.
 */
extern int g_ffDivisor;
extern float g_ffMagnitudeMultiplier;

/*
 * PWM globals.
 */
extern int g_ffPwmEnabled;
extern int g_ffPwmMaxMagnitude;

/* ABXY swap layout: 0 = off, 1 = swap A<->B, X<->Y */
extern int g_abxy_layout;

/* DPAD/Left-Stick swap: 0 = off, 1 = swap HAT0<->LS */
extern int g_dpad_analog_swap;

/* Invert left analog stick: 0 = off, 1 = invert ABS_X & ABS_Y */
extern int g_left_stick_invert;

/* Invert right analog stick: 0 = off, 1 = invert ABS_Z & ABS_RZ */
extern int g_right_stick_invert;

/* Invert “extra” sticks Z & RZ: 0 = off, 1 = invert ABS_Z & ABS_RZ */
extern int g_right_stick_invert_z_rz;

/* Analog sensitivity: -3…0…+3 (–50%,–25%,–10%,0,+10%,+25%,+50%) */
extern int g_analog_sensitivity;

/* Emulate GAS/BRAKE from L2/R2 if physical axes missing */
extern int g_gas_brake_emulation;

/* Configurable deadzone percentage (0…100).  
   Values within ±(deadzone% of half-range) around center will be treated as center. */
extern int g_deadzone;

/* New: Retroid Pocket Classic vibrator mode */  
extern int g_rpclassic;

/* ABS remapping: index = source ABS_* code, value = destination ABS_* code or -1 for no remap */
extern int g_absRemap[ABS_MAX + 1];

/*
 * New virtual controller parameters (set via command-line):
 */
extern char* g_uiname;
extern int g_uibus;
extern int g_uivid;
extern int g_uiproduct;
extern int g_uiversion;

/* Runtime toggle: skip unbind/rebind of the source controller */
extern int g_noSourceRebind;

/* Runtime toggle: remove the primary /dev/input/event* node after capture */
extern int g_removeSourceNode;

/*
 * Wake debounce: suppress stale analog input after sleep/resume.
 *
 * When a device recapture is detected (sleep → wake transition), we record
 * the current timestamp.  forward_physical_event() then suppresses EV_ABS
 * events until g_wakeDebounceMs milliseconds have elapsed, preventing
 * phantom scrolling caused by stale ADC values in the kernel input buffer.
 *
 * See: https://github.com/TheGammaSqueeze/GammaOSNext/issues/249
 */
extern volatile unsigned long long g_wakeTimestampMs;
extern int g_wakeDebounceMs;

#endif /* GAMMAPAD_H */
