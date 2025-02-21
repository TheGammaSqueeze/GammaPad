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
 *    continuously sends FF events (especially when using extreme --ffdiv/--ffmag values),
 *    the physical device is updated at most once every 50ms.
 *  - NEW: For physical FF devices, if PWM simulation is enabled (--ffpwm) and the adjusted
 *         magnitude is below the specified maximum (default 32767), we simulate PWM in software
 *         to adjust the intensity by toggling the motor on and off.
 *  - NEW: When an FF effect is played, overlapping effects are prevented by recording an expiration
 *         timestamp (expireTimeMs) and spawning a stop thread that stops the effect after its duration.
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
    unsigned int durationMs;  /* effect duration (adjusted) */
    unsigned long long expireTimeMs; /* NEW: expiration time (start time + duration) */
    int stopThreadActive;     /* NEW: flag to indicate a stop thread is active */
    __u16 ffType;             /* effect->type (for reference) */
    struct ff_effect original;/* unmodified effect data */
};

static struct AggregatorEffect gEffects[MAX_EFFECTS];

/* Forward declaration to fix implicit declaration error */
static void aggregatorClearSlot(struct AggregatorEffect* slot);

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

/*
 * test_effect_support:
 *   Query the physical FF device (via EVIOCGBIT) to check if the given effect
 *   type is supported. Returns 1 if supported, 0 otherwise.
 */
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
 * fallbackToggleMotor: [unchanged]
 *--------------------------------------------------------*/
static void fallbackToggleMotor(unsigned int durationMs, volatile int *shouldStop)
{
    if (!durationMs) return;
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) {
        fprintf(stderr, "[FF] fallbackToggleMotor: timerfd_create failed: %s\n", strerror(errno));
        return;
    }
    
    FILE* fOff = fopen(VIB_PATH, "w");
    if (fOff) {
        fprintf(fOff, "0\n");
        fclose(fOff);
    }
    
    unsigned long long start = getTimeMs();
    unsigned long long end   = start + durationMs;
    int state = 1;
    
    FILE* fOn = fopen(VIB_PATH, "w");
    if (fOn) {
        fprintf(fOn, "1\n");
        fclose(fOn);
    }
    
    struct itimerspec ts;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = 0;
    ts.it_value.tv_sec = 0;
    ts.it_value.tv_nsec = 15 * 1000000; 
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
            FILE* fOff = fopen(VIB_PATH, "w");
            if (fOff) {
                fprintf(fOff, "0\n");
                fclose(fOff);
            }
            state = 0;
            ts.it_value.tv_sec = 0;
            ts.it_value.tv_nsec = 30 * 1000000;
        } else {
            FILE* fOn = fopen(VIB_PATH, "w");
            if (fOn) {
                fprintf(fOn, "1\n");
                fclose(fOn);
            }
            state = 1;
            ts.it_value.tv_sec = 0;
            ts.it_value.tv_nsec = 15 * 1000000;
        }
        if (timerfd_settime(tfd, 0, &ts, NULL) < 0) {
            fprintf(stderr, "[FF] fallbackToggleMotor: timerfd_settime failed: %s\n", strerror(errno));
            break;
        }
    }
    
    fOff = fopen(VIB_PATH, "w");
    if (fOff) {
        fprintf(fOff, "0\n");
        fclose(fOff);
    }
    close(tfd);
}

/*---------------------------------------------------------
 * PWM simulation for physical FF devices: [unchanged]
 *--------------------------------------------------------*/
static pthread_t pwmThread;
static volatile int pwmThreadShouldStop = 0;
static volatile int pwmActive = 0;
static volatile unsigned int pwmOnDuration = 0;
static volatile unsigned int pwmOffDuration = 0;
static volatile int pwmEffectId = -1;

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

static void setGlobalPWMParameters(unsigned int onDur, unsigned int offDur, int effectId) {
    pwmOnDuration = onDur;
    pwmOffDuration = offDur;
    pwmEffectId = effectId;
    LOG_FF("[FF] PWM parameters updated: onDuration=%u, offDuration=%u, effectId=%d\n", onDur, offDur, effectId);
}

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

static void startPWMThread(void) {
    pwmThreadShouldStop = 0;
    if (pthread_create(&pwmThread, NULL, pwmThreadFunc, NULL) == 0) {
        pwmActive = 1;
        LOG_FF("[FF] PWM thread started with onDuration=%u, offDuration=%u\n", pwmOnDuration, pwmOffDuration);
    } else {
        LOG_FF("[FF] Failed to start PWM thread\n");
    }
}

static void stopPWMThread(void) {
    if (pwmActive) {
        pwmThreadShouldStop = 1;
        pthread_join(pwmThread, NULL);
        pwmActive = 0;
        LOG_FF("[FF] PWM thread stopped\n");
    }
}

/*---------------------------------------------------------
 * NEW: ff_stop_thread - stops an effect when its duration expires.
 * It sleeps until the slot's expireTimeMs, then, if the effect is still active,
 * sends a stop event and clears the slot.
 *--------------------------------------------------------*/
static void* ff_stop_thread(void* arg) {
    struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
    if (!slot) return NULL;
    unsigned long long now = getTimeMs();
    if (slot->expireTimeMs > now) {
        msleep((unsigned int)(slot->expireTimeMs - now));
    }
    if (slot->used && (getTimeMs() >= slot->expireTimeMs)) {
         struct input_event ev;
         memset(&ev, 0, sizeof(ev));
         ev.type = EV_FF;
         ev.code = (slot->realDevId >= 0) ? slot->realDevId : slot->aggregatorKid;
         ev.value = 0;
         if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
             LOG_FF("[FF] ff_stop_thread: write failed: %s\n", strerror(errno));
         }
         aggregatorClearSlot(slot);
         LOG_FF("[FF] ff_stop_thread: Effect %d stopped after expiration.\n", slot->aggregatorKid);
    }
    slot->stopThreadActive = 0;
    return NULL;
}

/*---------------------------------------------------------
 * update_rumble_state: [unchanged]
 *--------------------------------------------------------*/
static void update_rumble_state(void) {
    static unsigned long long lastUpdate = 0;
    unsigned long long now = getTimeMs();
    if (now - lastUpdate < 50) {
        return;
    }
    lastUpdate = now;
    
    unsigned int maxMag = 0;
    int effectId = -1;
    int found = 0;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].ffType == FF_RUMBLE) {
            unsigned int mag = gEffects[i].original.u.rumble.weak_magnitude;
            if (mag > maxMag) {
                maxMag = mag;
                effectId = (gEffects[i].realDevId >= 0) ? gEffects[i].realDevId : gEffects[i].aggregatorKid;
                found = 1;
            }
        }
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    if (!found) {
        stopPWMThread();
        ev.code = (effectId >= 0) ? effectId : 0;
        ev.value = 0;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
            LOG_FF("[FF] update_rumble_state: write failed: %s\n", strerror(errno));
        }
        return;
    }
    if (!g_ffPwmEnabled || maxMag >= g_ffPwmMaxMagnitude) {
        stopPWMThread();
        ev.code = effectId;
        ev.value = 1;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
            LOG_FF("[FF] update_rumble_state: write failed: %s\n", strerror(errno));
        }
        return;
    }
    #define PWM_PERIOD_MS 45
    unsigned int onDuration = (maxMag * PWM_PERIOD_MS) / g_ffPwmMaxMagnitude;
    unsigned int offDuration = PWM_PERIOD_MS - onDuration;
    setGlobalPWMParameters(onDuration, offDuration, effectId);
    if (!pwmActive) {
        startPWMThread();
    }
}

/*---------------------------------------------------------
 * aggregatorPlayThread: [unchanged fallback branch]
 *--------------------------------------------------------*/
static void* aggregatorPlayThread(void* arg) {
    struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
    if (!slot) return NULL;
    int aggregatorKid = slot->aggregatorKid;
    unsigned int duration = slot->durationMs;
    LOG_FF("[FF-Thread] aggregatorKid=%d, starting fallback effect for %u ms\n", aggregatorKid, duration);
    fallbackToggleMotor(duration, &slot->shouldStop);
    LOG_FF("[FF-Thread] aggregatorKid=%d, fallback effect completed\n", aggregatorKid);
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindSlotByKid:
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindSlotByKid(int kid) {
    if (kid < 0) return NULL;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].aggregatorKid == kid)
            return &gEffects[i];
    }
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindFreeSlot:
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindFreeSlot(void) {
    for (int i = 0; i < MAX_EFFECTS; i++){
        if (!gEffects[i].used) return &gEffects[i];
    }
    LOG_FF("[FF] aggregator => no free slot, reusing slot=0.\n");
    return &gEffects[0];
}

/*---------------------------------------------------------
 * aggregatorClearSlot:
 *   Reset an aggregator effect slot.
 *--------------------------------------------------------*/
static void aggregatorClearSlot(struct AggregatorEffect* slot) {
    if (!slot) return;
    slot->used = 0;
    slot->aggregatorKid = -1;
    slot->realDevId = -1;
    slot->shouldStop = 0;
    slot->durationMs = 0;
    slot->ffType = 0;
    slot->expireTimeMs = 0;
    slot->stopThreadActive = 0;
    memset(&slot->original, 0, sizeof(slot->original));
}

/*---------------------------------------------------------
 * storeUploadedEffect:
 *   Called on UI_END_FF_UPLOAD to store the effect.
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
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(eff->id);
    if (!slot) {
        slot = aggregatorFindFreeSlot();
    }
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] Freed old effect: aggregatorKid=%d, realDevId=%d\n", slot->aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);
    slot->used = 1;
    slot->aggregatorKid = eff->id;
    slot->realDevId = -1;
    slot->shouldStop = 0;
    slot->durationMs = eff->replay.length;
    slot->ffType = eff->type;
    slot->original = *eff;
    /* When the effect is played, expireTimeMs will be set */
    slot->expireTimeMs = 0;
    slot->stopThreadActive = 0;
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        if (!test_effect_support(eff->type)) {
            LOG_FF("[FF] physical device does not support effect type %u\n", eff->type);
            slot->realDevId = -1;
        } else {
            struct ff_effect copy = *eff;
            copy.id = -1;
            if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
                slot->realDevId = copy.id;
                LOG_FF("[FF] EVIOCSFF success: realDevId=%d\n", copy.id);
            } else {
                LOG_FF("[FF] EVIOCSFF fail: %s\n", strerror(errno));
                slot->realDevId = -1;
            }
        }
    }
}

int dummy_upload_ff_effect(struct ff_effect* eff) {
    if (!eff) return -1;
    LOG_FF("[FF] dummy_upload_ff_effect: aggregatorKid=%d, type=%u, replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);
    return 0;
}

int dummy_erase_ff_effect(int aggregatorKid) {
    LOG_FF("[FF] dummy_erase_ff_effect: aggregatorKid=%d\n", aggregatorKid);
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(aggregatorKid);
    if (!slot) return 0;
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] Freed effect: aggregatorKid=%d, realDevId=%d\n", aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);
    return 0;
}

/*---------------------------------------------------------
 * ff_play_effect:
 *   When an FF event is played, we set the expiration timestamp for the effect
 *   (expireTimeMs = current time + duration). If a stop thread is not already active,
 *   we spawn one that will sleep until the expiration time and then send a stop event.
 *--------------------------------------------------------*/
void ff_play_effect(int aggregatorKid, int doPlay) {
    LOG_FF("[FF] ff_play_effect: aggregatorKid=%d, doPlay=%d\n", aggregatorKid, doPlay);
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(aggregatorKid);
    if (!slot || !slot->used) {
        LOG_FF("[FF] No used slot for aggregatorKid=%d; ignoring\n", aggregatorKid);
        return;
    }
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         if (slot->ffType == FF_RUMBLE) {
             if (doPlay) {
                 unsigned long long now = getTimeMs();
                 /* Set expireTimeMs to now + duration if not already set or if new duration is longer */
                 unsigned long long newExpire = now + slot->durationMs;
                 if (slot->expireTimeMs < newExpire)
                     slot->expireTimeMs = newExpire;
             } else {
                 aggregatorClearSlot(slot);
             }
             update_rumble_state();
         } else {
             int effectId = (slot->realDevId >= 0) ? slot->realDevId : aggregatorKid;
             struct input_event ev;
             memset(&ev, 0, sizeof(ev));
             ev.type = EV_FF;
             ev.code = effectId;
             ev.value = doPlay;
             if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
                  LOG_FF("[FF] Direct passthrough write failed: %s\n", strerror(errno));
             }
         }
         if (doPlay) {
             if (slot->expireTimeMs == 0)
                 slot->expireTimeMs = getTimeMs() + slot->durationMs;
             if (!slot->stopThreadActive) {
                 slot->stopThreadActive = 1;
                 pthread_t th;
                 if (pthread_create(&th, NULL, ff_stop_thread, slot) != 0) {
                     LOG_FF("[FF] pthread_create (stop thread) failed for aggregatorKid=%d: %s\n", aggregatorKid, strerror(errno));
                     slot->stopThreadActive = 0;
                 } else {
                     pthread_detach(th);
                 }
             }
         }
         return;
    }
    /* Fallback branch (if no physical FF device) */
    if (doPlay) {
         if (!slot->shouldStop) {
             slot->shouldStop = 1;
             msleep(100);
         }
         slot->shouldStop = 0;
         pthread_t th;
         if (pthread_create(&th, NULL, aggregatorPlayThread, slot) != 0) {
             LOG_FF("[FF] fallback pthread_create failed for aggregatorKid=%d: %s\n", aggregatorKid, strerror(errno));
         } else {
             pthread_detach(th);
             LOG_FF("[FF] Spawned fallback play thread for aggregatorKid=%d\n", aggregatorKid);
         }
    } else {
         LOG_FF("[FF] aggregator STOP for aggregatorKid=%d\n", aggregatorKid);
         slot->shouldStop = 1;
    }
}

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
