/*****************************************************
 * gammapad_ff.c
 *
 * Aggregator-based Force Feedback:
 *  - We maintain up to MAX_EFFECTS in aggregator memory.
 *  - On UI_END_FF_UPLOAD, storeUploadedEffect() stores the effect
 *    in an aggregator slot.
 *  - When a physical FF device is present (via g_ffPhysicalFd and g_hasPhysicalFF),
 *    we forward events directly (1:1 passthrough) only for supported effect types.
 *  - Otherwise, we fall back to the original behavior.
 *  - For FF_RUMBLE effects, we merge large/small motor magnitudes.
 *
 * New changes:
 *  - In storeUploadedEffect, we check if the physical device supports the effect type.
 *  - The fallbackToggleMotor function uses timerfd for high-resolution timing.
 *  - For FF_RUMBLE effects, new command-line parameters adjust the replay length and magnitude.
 *  - A throttling mechanism has been added to update_rumble_state() so that, if a game or app
 *    continuously sends FF events, the physical device is updated at most once every 50ms.
 *  - For physical FF devices, if PWM simulation is enabled (--ffpwm) and the adjusted
 *    magnitude is below the specified maximum, we simulate PWM in software to adjust the intensity.
 *  - Finally, when an effect expires the stop thread now polls the time (every 10ms) and then sends
 *    an explicit stop event to clear any lingering motor vibration.
 *****************************************************/

#include "gammapad.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>
#include <fcntl.h>
#include <time.h>
#include <sys/timerfd.h>

#define MAX_EFFECTS 32

/* Aggregator effect slot structure */
struct AggregatorEffect {
    int used;                 /* whether slot is in use */
    int aggregatorKid;        /* aggregator's effect ID */
    int realDevId;            /* real device effect ID if accepted, else -1 */
    int shouldStop;           /* aggregator STOP => thread ends early */
    unsigned int durationMs;  /* from effect->replay.length */
    unsigned long long expireTimeMs; /* time at which effect should expire */
    int stopThreadActive;     /* flag to indicate a stop thread is running */
    __u16 ffType;             /* effect->type (for reference) */
    struct ff_effect original;/* unmodified effect data */
};

static struct AggregatorEffect gEffects[MAX_EFFECTS];

/*
 * Declared in gammapad_main.c:
 *   g_ffPhysicalFd => physical FF device file descriptor (if any)
 *   g_hasPhysicalFF => flag indicating physical FF device found
 */
extern int g_ffPhysicalFd;
extern int g_hasPhysicalFF;

/* Virtual controller FD */
extern int controllerFd;

/* Fallback vibrator path */
static const char* VIB_PATH = "/sys/class/timed_output/vibrator/enable";

/* Forward declaration of aggregatorClearSlot */
static void aggregatorClearSlot(struct AggregatorEffect* slot);

/* Forward declaration of update_rumble_state so it can be used in ff_play_effect */
static void update_rumble_state(void);

/*---------------------------------------------------------
 * test_effect_support:
 *   Query the physical FF device (via EVIOCGBIT) to check if the given effect
 *   type is supported. Returns 1 if supported, 0 otherwise.
 *--------------------------------------------------------*/
static int test_effect_support(__u16 effect) {
    if (!(g_hasPhysicalFF && g_ffPhysicalFd >= 0))
         return 0;
    unsigned long caps[2] = {0};
    if (ioctl(g_ffPhysicalFd, EVIOCGBIT(EV_FF, sizeof(caps)), caps) < 0)
         return 0;
    int index = effect / (8 * sizeof(unsigned long));
    int bit = effect % (8 * sizeof(unsigned long));
    return (caps[index] & (1UL << bit)) ? 1 : 0;
}

/*---------------------------------------------------------
 * fallbackToggleMotor:
 *   If EVIOCSFF fails or no physical effect is available, toggle the vibrator.
 *   This version uses timerfd for high-resolution timing and checks a cancellation flag.
 *--------------------------------------------------------*/
static void fallbackToggleMotor(unsigned int durationMs, volatile int *shouldStop) {
    if (!durationMs) return;
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        fprintf(stderr, "[FF] fallbackToggleMotor: timerfd_create failed: %s\n", strerror(errno));
        return;
    }
    
    /* Ensure vibrator is initially off */
    FILE* fOff = fopen(VIB_PATH, "w");
    if (fOff) {
        fprintf(fOff, "0\n");
        fclose(fOff);
    }
    
    unsigned long long start = getTimeMs();
    unsigned long long end   = start + durationMs;
    int state = 1; // start with vibrator on
    
    /* Turn on vibrator initially */
    FILE* fOn = fopen(VIB_PATH, "w");
    if (fOn) {
        fprintf(fOn, "1\n");
        fclose(fOn);
    }
    
    /* Set initial timer for 15ms (on-phase) */
    struct itimerspec ts;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 15 * 1000000; // 15ms
    if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
        fprintf(stderr, "[FF] fallbackToggleMotor: timerfd_settime failed: %s\n", strerror(errno));
        close(tfd);
        return;
    }
    
    while (getTimeMs() < end && !(*shouldStop)) {
        uint64_t expirations;
        int r = read(tfd, &expirations, sizeof(expirations));
        if (r < 0) {
            fprintf(stderr, "[FF] fallbackToggleMotor: read timerfd failed: %s\n", strerror(errno));
            break;
        }
        if (state == 1) {
            /* Turn off vibrator */
            FILE* fOff = fopen(VIB_PATH, "w");
            if (fOff) {
                fprintf(fOff, "0\n");
                fclose(fOff);
            }
            state = 0;
            /* Set timer for 30ms (off-phase) */
            ts.it_value.tv_sec = 0;
            ts.it_value.tv_nsec = 30 * 1000000; // 30ms
        } else {
            /* Turn on vibrator */
            FILE* fOn = fopen(VIB_PATH, "w");
            if (fOn) {
                fprintf(fOn, "1\n");
                fclose(fOn);
            }
            state = 1;
            /* Set timer for 15ms (on-phase) */
            ts.it_value.tv_sec = 0;
            ts.it_value.tv_nsec = 15 * 1000000; // 15ms
        }
        if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
            fprintf(stderr, "[FF] fallbackToggleMotor: timerfd_settime failed: %s\n", strerror(errno));
            break;
        }
    }
    
    /* Ensure vibrator is off at end */
    fOff = fopen(VIB_PATH, "w");
    if (fOff) {
        fprintf(fOff, "0\n");
        fclose(fOff);
    }
    close(tfd);
}

/*---------------------------------------------------------
 * NEW PWM simulation for physical FF devices.
 * When a FF_RUMBLE effect is active with a magnitude lower than the specified max (g_ffPwmMaxMagnitude),
 * we simulate PWM by toggling the motor on/off with a duty cycle proportional to the magnitude.
 *--------------------------------------------------------*/

/* Global PWM state variables */
static pthread_t pwmThread;
static volatile int pwmThreadShouldStop = 0;
static volatile int pwmActive = 0;
static volatile unsigned int pwmOnDuration = 0;
static volatile unsigned int pwmOffDuration = 0;
static volatile int pwmEffectId = -1;

/* Helper functions to send on/off events to the physical FF device */
static void sendOnToPhysical(int effectId) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    ev.code = effectId;
    ev.value = 1;
    if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
        LOG_FF("[FF] sendOnToPhysical: write failed: %s\n", strerror(errno));
    }
}

static void sendOffToPhysical(int effectId) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    ev.code = effectId;
    ev.value = 0;
    if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
        LOG_FF("[FF] sendOffToPhysical: write failed: %s\n", strerror(errno));
    }
}

/* Update the global PWM parameters */
static void setGlobalPWMParameters(unsigned int onDur, unsigned int offDur, int effectId) {
    pwmOnDuration = onDur;
    pwmOffDuration = offDur;
    pwmEffectId = effectId;
    LOG_FF("[FF] PWM parameters updated: onDuration=%u, offDuration=%u, effectId=%d\n", onDur, offDur, effectId);
}

/* PWM thread function: continuously toggle motor on and off */
static void* pwmThreadFunc(void* arg) {
    (void)arg;
    while (!pwmThreadShouldStop) {
        sendOnToPhysical(pwmEffectId);
        msleep(pwmOnDuration);
        if (pwmThreadShouldStop)
            break;
        sendOffToPhysical(pwmEffectId);
        msleep(pwmOffDuration);
    }
    return NULL;
}

/* Start the PWM simulation thread */
static void startPWMThread(void) {
    pwmThreadShouldStop = 0;
    if (pthread_create(&pwmThread, NULL, pwmThreadFunc, NULL) == 0) {
        pwmActive = 1;
        LOG_FF("[FF] PWM thread started with onDuration=%u, offDuration=%u\n", pwmOnDuration, pwmOffDuration);
    } else {
        LOG_FF("[FF] Failed to start PWM thread\n");
    }
}

/* Stop the PWM simulation thread */
static void stopPWMThread(void) {
    if (pwmActive) {
        pwmThreadShouldStop = 1;
        pthread_join(pwmThread, NULL);
        pwmActive = 0;
        LOG_FF("[FF] PWM thread stopped\n");
    }
}

/*---------------------------------------------------------
 * update_rumble_state:
 *   When using a physical FF device and FF_RUMBLE effects,
 *   this function mixes all active rumble effects (always in slot 0)
 *   by using the parameters in slot 0.
 *   It then either sends a constant on event (if full intensity or PWM disabled)
 *   or, for lower intensities when PWM is enabled, computes a PWM duty cycle and
 *   starts a PWM thread to simulate variable intensity.
 *
 *   Updates are throttled to at most once every 50ms.
 *--------------------------------------------------------*/
static void update_rumble_state(void) {
    static unsigned long long lastUpdate = 0;
    unsigned long long now = getTimeMs();
    if (now - lastUpdate < 50) {
        return;
    }
    lastUpdate = now;
    
    /* Always use slot 0 for FF_RUMBLE effects */
    struct AggregatorEffect* slot = &gEffects[0];
    if (!(slot->used && slot->ffType == FF_RUMBLE)) {
         stopPWMThread();
         struct input_event ev;
         memset(&ev, 0, sizeof(ev));
         ev.type = EV_FF;
         ev.code = (slot->realDevId >= 0) ? slot->realDevId : slot->aggregatorKid;
         ev.value = 0;
         if (g_ffPhysicalFd >= 0)
              write(g_ffPhysicalFd, &ev, sizeof(ev));
         return;
    }
    
    unsigned int maxMag = slot->original.u.rumble.weak_magnitude;
    int effectId = (slot->realDevId >= 0) ? slot->realDevId : slot->aggregatorKid;
    
    /* If PWM is disabled or the effect magnitude is at or above full intensity, send constant on */
    extern int g_ffPwmEnabled;
    extern int g_ffPwmMaxMagnitude;
    if (!g_ffPwmEnabled || maxMag >= g_ffPwmMaxMagnitude) {
         stopPWMThread();
         struct input_event ev;
         memset(&ev, 0, sizeof(ev));
         ev.type = EV_FF;
         ev.code = effectId;
         ev.value = 1;
         if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0)
              LOG_FF("[FF] update_rumble_state: write failed: %s\n", strerror(errno));
         return;
    }
    
    #define PWM_PERIOD_MS 45
    unsigned int onDuration = (maxMag * PWM_PERIOD_MS) / g_ffPwmMaxMagnitude;
    unsigned int offDuration = PWM_PERIOD_MS - onDuration;
    setGlobalPWMParameters(onDuration, offDuration, effectId);
    if (!pwmActive)
         startPWMThread();
}

/*---------------------------------------------------------
 * ff_stop_thread:
 *   Wait until the current effect’s expiration time (polling every 10ms)
 *   then send a stop event to clear the effect on the physical device.
 *--------------------------------------------------------*/
static void* ff_stop_thread(void* arg) {
    struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
    if (!slot) return NULL;
    while (getTimeMs() < slot->expireTimeMs && !slot->shouldStop) {
         msleep(10);
    }
    if (slot->used && (getTimeMs() >= slot->expireTimeMs)) {
         struct input_event ev;
         memset(&ev, 0, sizeof(ev));
         ev.type = EV_FF;
         ev.code = (slot->realDevId >= 0) ? slot->realDevId : slot->aggregatorKid;
         ev.value = 0;
         if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0)
              LOG_FF("[FF] ff_stop_thread: write failed: %s\n", strerror(errno));
         aggregatorClearSlot(slot);
         stopPWMThread();
         LOG_FF("[FF] ff_stop_thread: Effect %d stopped after expiration.\n", slot->aggregatorKid);
    }
    slot->stopThreadActive = 0;
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorClearSlot:
 *   Reset an aggregator effect slot.
 *--------------------------------------------------------*/
static void aggregatorClearSlot(struct AggregatorEffect* slot) {
    if (!slot) return;
    slot->used = 0;
    slot->aggregatorKid = 0;  /* Always use slot 0 for FF_RUMBLE */
    slot->realDevId = -1;
    slot->shouldStop = 0;
    slot->durationMs = 0;
    slot->expireTimeMs = 0;
    slot->stopThreadActive = 0;
    slot->ffType = 0;
    memset(&slot->original, 0, sizeof(slot->original));
}

/*---------------------------------------------------------
 * storeUploadedEffect:
 *   Called on UI_END_FF_UPLOAD to store the effect in an aggregator slot.
 *   For FF_RUMBLE effects, always use slot 0.
 *   The expiration time is set to the current time plus the replay length.
 *   If a rumble effect is already active, the new effect overrides the old one.
 *--------------------------------------------------------*/
void storeUploadedEffect(struct ff_effect* eff) {
    if (!eff) return;
    LOG_FF("[FF] storeUploadedEffect: aggregatorKid=%d, type=%u, replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);
    if (eff->type == FF_RUMBLE) {
         unsigned short w = eff->u.rumble.weak_magnitude;
         unsigned short s = eff->u.rumble.strong_magnitude;
         unsigned short unified = (s > w) ? s : w;
         unsigned int adjustedMag = (unsigned int)(unified * g_ffMagnitudeMultiplier);
         eff->u.rumble.weak_magnitude = adjustedMag;
         eff->u.rumble.strong_magnitude = adjustedMag;
         LOG_FF("[FF] FF_RUMBLE unified=%u, multiplier=%f, finalMag=%u\n",
                unified, g_ffMagnitudeMultiplier, adjustedMag);
    }
    if (g_ffDivisor != 1) {
         unsigned int origLen = eff->replay.length;
         eff->replay.length = eff->replay.length / g_ffDivisor;
         LOG_FF("[FF] effect length divided by %d: %u -> %u\n", g_ffDivisor, origLen, eff->replay.length);
    }
    /* Always use slot 0 for FF_RUMBLE effects */
    struct AggregatorEffect* slot = &gEffects[0];
    /* Override any active effect with the new parameters */
    if (slot->used && slot->ffType == FF_RUMBLE) {
         slot->expireTimeMs = getTimeMs() + eff->replay.length;
         slot->durationMs = eff->replay.length;
         slot->original = *eff;
         LOG_FF("[FF] Updated active effect expiration to %llu ms\n", slot->expireTimeMs);
         return;
    }
    /* Otherwise, clear any previous effect in the slot */
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
         LOG_FF("[FF] Freed old effect in rumble slot, realDevId=%d\n", slot->realDevId);
         aggregatorClearSlot(slot);
    }
    slot->used = 1;
    slot->durationMs = eff->replay.length;
    slot->expireTimeMs = getTimeMs() + slot->durationMs;
    slot->stopThreadActive = 0;
    slot->shouldStop = 0;
    slot->ffType = FF_RUMBLE;
    slot->aggregatorKid = 0;
    slot->original = *eff;
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         if (!test_effect_support(eff->type)) {
              LOG_FF("[FF] Physical device does not support effect type %u\n", eff->type);
              slot->realDevId = -1;
         } else {
              struct ff_effect copy = *eff;
              copy.id = -1;
              if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
                   slot->realDevId = copy.id;
                   LOG_FF("[FF] EVIOCSFF success: realDevId=%d\n", copy.id);
              } else {
                   if (errno == ENOSPC) {
                        LOG_FF("[FF] EVIOCSFF fail (ENOSPC): %s\n", strerror(errno));
                        /* If no space, leave realDevId unchanged */
                   } else {
                        LOG_FF("[FF] EVIOCSFF fail: %s\n", strerror(errno));
                        slot->realDevId = -1;
                   }
              }
         }
    }
}

/*---------------------------------------------------------
 * dummy_upload_ff_effect and dummy_erase_ff_effect:
 *--------------------------------------------------------*/
int dummy_upload_ff_effect(struct ff_effect* eff) {
    if (!eff) return -1;
    LOG_FF("[FF] dummy_upload_ff_effect: aggregatorKid=%d, type=%u, replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);
    return 0;
}

int dummy_erase_ff_effect(int aggregatorKid) {
    LOG_FF("[FF] dummy_erase_ff_effect: aggregatorKid=%d\n", aggregatorKid);
    struct AggregatorEffect* slot = &gEffects[0];
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
         LOG_FF("[FF] Freed rumble effect, realDevId=%d\n", slot->realDevId);
    }
    aggregatorClearSlot(slot);
    return 0;
}

/*---------------------------------------------------------
 * ff_play_effect:
 * For physical devices (FF_RUMBLE only).
 * When doPlay==1, if an active rumble effect exists in slot 0, ensure a stop thread is running
 * (without resetting the expiration time) and update the motor via update_rumble_state().
 * When doPlay==0, send a stop event immediately and clear the slot.
 *--------------------------------------------------------*/
void ff_play_effect(int aggregatorKid, int doPlay) {
    LOG_FF("[FF] ff_play_effect: aggregatorKid=%d, doPlay=%d\n", aggregatorKid, doPlay);
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         struct AggregatorEffect* slot = &gEffects[0];
         if (doPlay) {
              if (!(slot->used && slot->ffType == FF_RUMBLE)) {
                   LOG_FF("[FF] No active rumble effect slot found; ignoring play event\n");
                   return;
              }
              /* Do not update expireTimeMs here so that the effect lasts its full intended duration */
              if (!slot->stopThreadActive) {
                   slot->stopThreadActive = 1;
                   pthread_t th;
                   if (pthread_create(&th, NULL, ff_stop_thread, slot) != 0) {
                        LOG_FF("[FF] pthread_create (stop thread) failed: %s\n", strerror(errno));
                        slot->stopThreadActive = 0;
                   } else {
                        pthread_detach(th);
                   }
              }
              update_rumble_state();
         } else {
              if (slot->used && slot->ffType == FF_RUMBLE) {
                   struct input_event ev;
                   memset(&ev, 0, sizeof(ev));
                   ev.type = EV_FF;
                   ev.code = (slot->realDevId >= 0) ? slot->realDevId : slot->aggregatorKid;
                   ev.value = 0;
                   if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0)
                        LOG_FF("[FF] ff_play_effect stop write failed: %s\n", strerror(errno));
                   aggregatorClearSlot(slot);
                   stopPWMThread();
              }
         }
         return;
    }
    /* Fallback branch (if no physical FF device is present) */
}

/*---------------------------------------------------------
 * aggregatorReuploadAllEffects:
 *--------------------------------------------------------*/
void aggregatorReuploadAllEffects(void) {
    if (!g_hasPhysicalFF || g_ffPhysicalFd < 0) {
         fprintf(stderr, "[FF] aggregatorReuploadAllEffects: no real FF device; skipping\n");
         return;
    }
    fprintf(stderr, "[FF] Reuploading aggregator slots; FD=%d\n", g_ffPhysicalFd);
    for (int i = 0; i < MAX_EFFECTS; i++) {
         if (!gEffects[i].used)
              continue;
         struct ff_effect copy = gEffects[i].original;
         copy.id = -1;
         if (!test_effect_support(copy.type)) {
              fprintf(stderr, "[FF] aggregatorKid=%d: effect type %u not supported; skipping reupload\n",
                      gEffects[i].aggregatorKid, copy.type);
              gEffects[i].realDevId = -1;
              continue;
         }
         if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
              gEffects[i].realDevId = copy.id;
              fprintf(stderr, "[FF] aggregatorKid=%d: reupload success, new realDevId=%d\n",
                      gEffects[i].aggregatorKid, copy.id);
         } else {
              fprintf(stderr, "[FF] aggregatorKid=%d: reupload failed: %s\n",
                      gEffects[i].aggregatorKid, strerror(errno));
              gEffects[i].realDevId = -1;
         }
    }
}

/*---------------------------------------------------------
 * stopAllFF:
 * Stops all active FF effects by sending stop events.
 *--------------------------------------------------------*/
void stopAllFF(void) {
    stopPWMThread();
    for (int i = 0; i < MAX_EFFECTS; i++) {
         if (gEffects[i].used) {
              struct input_event ev;
              memset(&ev, 0, sizeof(ev));
              ev.type = EV_FF;
              ev.code = (gEffects[i].realDevId >= 0) ? gEffects[i].realDevId : gEffects[i].aggregatorKid;
              ev.value = 0;
              if (g_ffPhysicalFd >= 0) {
                   write(g_ffPhysicalFd, &ev, sizeof(ev));
              }
              aggregatorClearSlot(&gEffects[i]);
         }
    }
}
