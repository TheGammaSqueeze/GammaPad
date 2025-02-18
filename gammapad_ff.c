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
 *  - In storeUploadedEffect, we query the physical device to check if the
 *    requested effect type is supported. If not supported, we do not attempt to upload it.
 *  - The fallbackToggleMotor function is implemented using timerfd for high-resolution timing
 *    and runs in its own thread so as not to block the main input loop.
 *  - In the physical-device branch, for FF_RUMBLE effects we now mix multiple active effects
 *    (by taking the maximum magnitude) and update the physical motor accordingly.
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
    int used;             /* whether slot is in use */
    int aggregatorKid;    /* aggregator's effect ID */
    int realDevId;        /* real device effect ID if accepted, else -1 */
    int shouldStop;       /* aggregator STOP => thread ends early */
    unsigned int durationMs; /* from effect->replay.length */
    __u16 ffType;         /* effect->type (for reference) */
    struct ff_effect original; /* unmodified effect data */
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
 * fallbackToggleMotor:
 *   If EVIOCSFF fails or no physical effect is available, toggle the vibrator.
 *   This updated version uses timerfd for high-resolution timing and checks a cancellation flag.
 *--------------------------------------------------------*/
static void fallbackToggleMotor(unsigned int durationMs, volatile int *shouldStop)
{
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
 * update_rumble_state:
 *   When using a physical FF device and FF_RUMBLE effects,
 *   this function mixes all active rumble effects by selecting the maximum magnitude.
 *   It then sends a single EV_FF event to update the physical motor.
 *--------------------------------------------------------*/
static void update_rumble_state(void)
{
    unsigned int maxMag = 0;
    int effectId = -1;
    int found = 0;
    /* Iterate over all aggregator slots to mix FF_RUMBLE effects */
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].ffType == FF_RUMBLE) {
            unsigned int mag = gEffects[i].original.u.rumble.weak_magnitude; // already unified in storeUploadedEffect
            if (mag > maxMag) {
                maxMag = mag;
                /* Use the realDevId if available, else fallback to aggregatorKid */
                effectId = (gEffects[i].realDevId >= 0) ? gEffects[i].realDevId : gEffects[i].aggregatorKid;
                found = 1;
            }
        }
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_FF;
    if (found && effectId >= 0) {
        ev.code = effectId;
        /* In this design, a nonzero magnitude indicates play.
           (The physical FF device should already have been programmed with the effect parameters.)
           We assume that a play event with value 1 will cause the device to run at the intensity set in the effect. */
        ev.value = 1;
    } else {
        /* No active rumble effect: send a stop event.
           If an effect id exists from a previous effect, use it. */
        for (int i = 0; i < MAX_EFFECTS; i++) {
            if (gEffects[i].used && gEffects[i].ffType == FF_RUMBLE) {
                effectId = (gEffects[i].realDevId >= 0) ? gEffects[i].realDevId : gEffects[i].aggregatorKid;
                break;
            }
        }
        if (effectId < 0) return; /* nothing to stop */
        ev.code = effectId;
        ev.value = 0;
    }
    if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
        LOG_FF("[FF] update_rumble_state: write failed: %s\n", strerror(errno));
    }
}

/*---------------------------------------------------------
 * aggregatorPlayThread:
 *   Plays an effect for the intended duration using fallback toggling.
 *   This thread is spawned only when no physical FF device is available.
 *--------------------------------------------------------*/
static void* aggregatorPlayThread(void* arg)
{
    struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
    if (!slot) return NULL;
    int aggregatorKid = slot->aggregatorKid;
    unsigned int duration = slot->durationMs;
    
    LOG_FF("[FF-Thread] aggregatorKid=%d, starting fallback effect for %u ms\n",
           aggregatorKid, duration);
    
    fallbackToggleMotor(duration, &slot->shouldStop);
    
    LOG_FF("[FF-Thread] aggregatorKid=%d, fallback effect completed\n", aggregatorKid);
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindSlotByKid:
 *   Locate the aggregator slot for a given kid.
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindSlotByKid(int kid)
{
    if (kid < 0) return NULL;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].aggregatorKid == kid)
            return &gEffects[i];
    }
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindFreeSlot:
 *   Find the first available aggregator slot (or reuse slot 0 if needed).
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindFreeSlot(void)
{
    for (int i = 0; i < MAX_EFFECTS; i++){
        if (!gEffects[i].used) return &gEffects[i];
    }
    LOG_FF("[FF] aggregator => no free slot => reusing slot=0.\n");
    return &gEffects[0];
}

/*---------------------------------------------------------
 * aggregatorClearSlot:
 *   Reset an aggregator effect slot.
 *--------------------------------------------------------*/
static void aggregatorClearSlot(struct AggregatorEffect* slot)
{
    if (!slot) return;
    slot->used          = 0;
    slot->aggregatorKid = -1;
    slot->realDevId     = -1;
    slot->shouldStop    = 0;
    slot->durationMs    = 0;
    slot->ffType        = 0;
    memset(&slot->original, 0, sizeof(slot->original));
}

/*---------------------------------------------------------
 * storeUploadedEffect:
 *   Called on UI_END_FF_UPLOAD to store the effect in an aggregator slot.
 *   For FF_RUMBLE effects, merges strong and weak magnitudes.
 *
 * New change: Before uploading the effect to the physical device,
 * test whether the physical device supports the requested effect type.
 * If not supported, do not attempt EVIOCSFF.
 *--------------------------------------------------------*/
void storeUploadedEffect(struct ff_effect* eff)
{
    if (!eff) return;
    LOG_FF("[FF] aggregator => storeUploadedEffect => aggregatorKid=%d => type=%u => replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);
    if (eff->type == FF_RUMBLE) {
        unsigned short w = eff->u.rumble.weak_magnitude;
        unsigned short s = eff->u.rumble.strong_magnitude;
        unsigned short unified = (s > w) ? s : w;
        eff->u.rumble.weak_magnitude   = unified;
        eff->u.rumble.strong_magnitude = unified;
        LOG_FF("[FF] aggregatorKid=%d => FF_RUMBLE => unify big/small => finalMag=%u\n",
               eff->id, unified);
    }
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(eff->id);
    if (!slot) {
        slot = aggregatorFindFreeSlot();
    }
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] aggregatorKid=%d => Freed old realDevId=%d\n",
               slot->aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);
    slot->used          = 1;
    slot->aggregatorKid = eff->id;
    slot->realDevId     = -1;
    slot->shouldStop    = 0;
    slot->durationMs    = eff->replay.length;
    slot->ffType        = eff->type;
    slot->original      = *eff;
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        /* Only attempt to upload if the physical device supports this effect type */
        if (!test_effect_support(eff->type)) {
            LOG_FF("[FF] aggregatorKid=%d => physical device does not support effect type %u\n",
                   eff->id, eff->type);
            slot->realDevId = -1;
        } else {
            struct ff_effect copy = *eff;
            copy.id = -1;  /* Request new ID from real device */
            if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
                slot->realDevId = copy.id;
                LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => success => realDevId=%d\n",
                       eff->id, copy.id);
            } else {
                LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => fail => %s\n",
                       eff->id, strerror(errno));
                slot->realDevId = -1;
            }
        }
    }
}

/*---------------------------------------------------------
 * dummy_upload_ff_effect:
 *   Dummy handler for UI_BEGIN_FF_UPLOAD.
 *--------------------------------------------------------*/
int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if (!eff) return -1;
    LOG_FF("[FF] aggregator => dummy_upload_ff_effect => aggregatorKid=%d => type=%u => replay=%u\n",
           eff->id, eff->type, eff->replay.length);
    return 0;
}

/*---------------------------------------------------------
 * dummy_erase_ff_effect:
 *   Dummy handler for UI_BEGIN_FF_ERASE.
 *--------------------------------------------------------*/
int dummy_erase_ff_effect(int aggregatorKid)
{
    LOG_FF("[FF] aggregator => dummy_erase_ff_effect => aggregatorKid=%d\n", aggregatorKid);
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(aggregatorKid);
    if (!slot) return 0;
    if (slot->used && slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] Freed aggregatorKid=%d => realDevId=%d\n", aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);
    return 0;
}

/*---------------------------------------------------------
 * ff_play_effect:
 *   Handles EV_FF events.
 *   If a physical FF device is available, we perform a direct 1:1 passthrough.
 *   For FF_RUMBLE effects, we mix active effects and update the physical motor using update_rumble_state().
 *   Otherwise, we spawn a thread that plays the effect using fallback toggling.
 *
 * Note: When using a physical ffdev motor, we do not fall back.
 *--------------------------------------------------------*/
void ff_play_effect(int aggregatorKid, int doPlay)
{
    LOG_FF("[FF] aggregator => ff_play_effect => aggregatorKid=%d => doPlay=%d\n",
           aggregatorKid, doPlay);
    
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(aggregatorKid);
    if (!slot || !slot->used) {
        LOG_FF("[FF] aggregatorKid=%d => no used slot => ignoring\n", aggregatorKid);
        return;
    }
    
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
         if (slot->ffType == FF_RUMBLE) {
             /* For rumble effects, update the state of this slot and then update the global rumble mix. */
             if (doPlay) {
                 /* Mark effect as active; its magnitude was set in storeUploadedEffect */
                 // No individual event is sent here.
             } else {
                 /* Effect stopped: clear the slot */
                 aggregatorClearSlot(slot);
             }
             update_rumble_state();
         } else {
             /* For non-rumble effects, simply send the EV_FF event */
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
         return;
    }
    
    /* Fallback branch (should not be reached when a physical ffdev is defined) */
    if (doPlay) {
         if (!slot->shouldStop) {
             slot->shouldStop = 1;
             msleep(100); // wait briefly for previous thread to terminate
         }
         slot->shouldStop = 0;
         pthread_t th;
         if (pthread_create(&th, NULL, aggregatorPlayThread, slot) != 0) {
             LOG_FF("[FF] aggregatorKid=%d => pthread_create => fail => %s\n",
                    aggregatorKid, strerror(errno));
         } else {
             pthread_detach(th);
             LOG_FF("[FF] aggregatorKid=%d => spawned play thread => realDevId=%d\n",
                    aggregatorKid, slot->realDevId);
         }
    } else {
         LOG_FF("[FF] aggregatorKid=%d => aggregator STOP => set shouldStop=1\n", aggregatorKid);
         slot->shouldStop = 1;
    }
}

/*---------------------------------------------------------
 * aggregatorReuploadAllEffects:
 *   Re-upload all aggregator effects to the current physical FF device.
 *--------------------------------------------------------*/
void aggregatorReuploadAllEffects(void)
{
    if (!g_hasPhysicalFF || g_ffPhysicalFd < 0) {
        fprintf(stderr, "[FF] aggregatorReuploadAllEffects => no real FF device => skip.\n");
        return;
    }
    fprintf(stderr, "[FF] aggregatorReuploadAllEffects => re-uploading aggregator slots => FD=%d\n",
            g_ffPhysicalFd);
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (!gEffects[i].used)
            continue;
        struct ff_effect copy = gEffects[i].original;
        copy.id = -1;
        /* Only re-upload if supported */
        if (!test_effect_support(copy.type)) {
            fprintf(stderr, "[FF] aggregatorKid=%d => effect type %u not supported; skipping reupload.\n",
                    gEffects[i].aggregatorKid, copy.type);
            gEffects[i].realDevId = -1;
            continue;
        }
        if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
            gEffects[i].realDevId = copy.id;
            fprintf(stderr, "[FF] aggregatorKid=%d => reupload => new realDevId=%d\n",
                    gEffects[i].aggregatorKid, copy.id);
        } else {
            fprintf(stderr, "[FF] aggregatorKid=%d => reupload => fail => %s\n",
                    gEffects[i].aggregatorKid, strerror(errno));
            gEffects[i].realDevId = -1;
        }
    }
}
