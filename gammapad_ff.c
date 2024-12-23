/*****************************************************
 * gammapad_ff.c
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

/*
 * Maximum number of FF effects that can be stored
 */
#define MAX_EFFECTS 32

/*
 * Structure to store FF effect details
 */
struct StoredEffect {
    int used;               /* Whether this slot is in use */
    int kernel_id;          /* Kernel-assigned FF effect ID */
    unsigned int magnitude; /* 0..65535 */
    unsigned int durationMs;/* Duration in ms (from effect->replay.length) */
    __u16 ffType;
};

/*
 * Global array of stored effects
 */
static struct StoredEffect gEffects[MAX_EFFECTS];

/*
 * Path to the vibrator for device
 *
 * In this example, we treat VIB_PATH as controlling a single “hardware”
 * but in this project we might differentiate between Vibrator0 vs Vibrator1
 * if actually using different sysfs nodes. Here it’s a single path for both.
 */
static const char* VIB_PATH = "/sys/class/timed_output/vibrator/enable";

/*
 * toggleMotorRepeatedly:
 * Toggles the motor based on 'magnitude' for 'durationMs' milliseconds.
 *
 * If magnitude >= 60000, attempt a "timed" approach by writing the entire
 * durationMs once (if supported). Otherwise, fallback to repeated short ~50ms
 * ON cycles until we reach the end time.
 *
 * For magnitude < 60000, do repeated ON->sleep->OFF toggles using a variable
 * sleep time derived from 'magnitude'.
 *
 * We also force the motor OFF first to avoid leftover vibrations from
 * previously started or stale effect data.
 */
static void toggleMotorRepeatedly(unsigned int durationMs, unsigned int magnitude)
{
    if (!durationMs || !magnitude) return;

    /* Force the vibrator OFF before we begin any new effect. */
    {
        FILE* fOff = fopen(VIB_PATH, "w");
        if (fOff) {
            fprintf(fOff, "0\n");
            fclose(fOff);
        }
    }

    unsigned long long start   = getTimeMs();
    unsigned long long endTime = start + durationMs;

    /*
     * For toggling intervals, map magnitude (0..65535) to an
     * approximate [10,000..150,000] range for sleep in microseconds.
     */
    unsigned int sleepUs = 150000 - (unsigned int)((400000.0 * magnitude) / 65535.0);
    if (sleepUs < 10000) sleepUs = 10000; /* clamp to avoid too frequent toggles */

    /*
     * For simplicity, we illustrate one style: toggling ON->sleepUs->OFF
     * repeatedly until the effect time is up.
     */
    while (getTimeMs() < endTime) {
        /* Turn the motor ON. */
        FILE* fOn = fopen(VIB_PATH, "w");
        if (fOn) {
            fprintf(fOn, "1\n");
            fclose(fOn);
        }

        usleep(sleepUs);

        /* Turn the motor OFF. */
        FILE* fOff2 = fopen(VIB_PATH, "w");
        if (fOff2) {
            fprintf(fOff2, "0\n");
            fclose(fOff2);
        }
    }
}

/*
 * EffectThreadData:
 * Holds parameters for a single effect in a worker thread.
 */
struct EffectThreadData {
    unsigned int magnitude;
    unsigned int durationMs;
    __u16 ffType;
};

/*
 * effectThreadFunc:
 * Spawns a new thread to run toggleMotorRepeatedly for the entire
 * requested duration, ignoring leftover or expiry logic.
 */
static void* effectThreadFunc(void* arg)
{
    struct EffectThreadData* ed = (struct EffectThreadData*)arg;

    LOG_FF("[FF-Thread] Type=%u, Magnitude=%u, Duration=%u ms\n",
           ed->ffType, ed->magnitude, ed->durationMs);

    toggleMotorRepeatedly(ed->durationMs, ed->magnitude);

    free(ed);
    return NULL;
}

/*
 * storeUploadedEffect:
 * Called after UI_END_FF_UPLOAD to store effect data in gEffects[].
 *
 * IMPORTANT: We treat "weak_magnitude" as the LARGE motor (Vibrator1),
 * and "strong_magnitude" as the SMALL motor (Vibrator0). If both are
 * non-zero, we prioritize the large motor (the old “weak”).
 */
void storeUploadedEffect(struct ff_effect* eff)
{
    if (!eff) return;

    int kid = eff->id;
    LOG_FF("[FF] storeUploadedEffect => kernel_id=%d\n", kid);

    /* Remove existing effect with same kernel_id if any. */
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && (gEffects[i].kernel_id == kid)) {
            LOG_FF("[FF] Overwriting existing effect kernel_id=%d in slot=%d\n",
                   kid, i);
            ioctl(controllerFd, EVIOCRMFF, kid); /* remove from kernel */
            gEffects[i].used = 0;
        }
    }

    /* Find a free slot or fallback to slot=0. */
    int idx = -1;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (!gEffects[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        idx = 0;
        if (gEffects[idx].used) {
            if (ioctl(controllerFd, EVIOCRMFF, gEffects[idx].kernel_id) == 0) {
                LOG_FF("[FF] Cleared effect in slot=0 (kid=%d)\n",
                       gEffects[idx].kernel_id);
            }
            gEffects[idx].used = 0;
        }
    }

    gEffects[idx].used      = 1;
    gEffects[idx].kernel_id = kid;

    /* Determine the magnitude based on effect->type, now swapped for rumble. */
    unsigned int mag = 0;
    switch (eff->type) {
    case FF_RUMBLE:
    {
        /*
         * SWAPPED logic:
         *  - weak_magnitude => small motor
         *  - strong_magnitude => large motor
         * We also do minimal scaling (we can adjust to taste).
         */
        unsigned int smallMotor = eff->u.rumble.weak_magnitude / 2;
        unsigned int largeMotor = eff->u.rumble.strong_magnitude / 3;

        /* If both present => pick largeMotor first. */
        if (largeMotor > 0 && smallMotor > 0) {
            mag = largeMotor;
        } else if (largeMotor > 0) {
            mag = largeMotor;
        } else {
            mag = smallMotor; /* fallback */
        }
        break;
    }
    case FF_CONSTANT:
        mag = eff->u.constant.level;
        break;
    case FF_PERIODIC:
        mag = eff->u.periodic.magnitude;
        break;
    case FF_RAMP:
        mag = (eff->u.ramp.start_level + eff->u.ramp.end_level) / 2;
        break;
    case FF_SPRING:
    case FF_DAMPER:
    case FF_INERTIA:
        mag = (eff->u.condition[0].right_coeff + eff->u.condition[0].left_coeff) / 2;
        break;
    default:
        mag = 20000; /* default guess */
        break;
    }

    gEffects[idx].magnitude  = mag;
    gEffects[idx].durationMs = eff->replay.length;
    gEffects[idx].ffType     = eff->type;

    LOG_FF("[FF] Stored effect => slot=%d, mag=%u, dur=%u, type=%u\n",
           idx, mag, eff->replay.length, eff->type);
}

/*
 * dummy_upload_ff_effect:
 * Minimal logs for effect upload.
 */
int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if (!eff) return -1;
    LOG_FF("[FF] Upload => type=%u, replay=%u ms\n",
           eff->type, eff->replay.length);
    return 0;
}

/*
 * dummy_erase_ff_effect:
 * Minimal logs for effect erase.
 */
int dummy_erase_ff_effect(int kernel_id)
{
    LOG_FF("[FF] Erase effect => kernel_id=%d\n", kernel_id);
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].kernel_id == kernel_id) {
            if (ioctl(controllerFd, EVIOCRMFF, kernel_id) == 0) {
                LOG_FF("[FF] Freed slot for kernel_id=%d\n", kernel_id);
            }
            gEffects[i].used = 0;
            break;
        }
    }
    return 0;
}

/*
 * ff_play_effect:
 * Called on EV_FF code=<kid> value=1 => play, value=0 => stop.
 *
 * If doPlay==1 => spawn a new thread for the full effect duration.
 * If doPlay==0 => just log "stop" and do not forcibly kill threads.
 */
void ff_play_effect(int kid, int doPlay)
{
    if (doPlay) {
        /* find effect by kernel_id */
        for (int i = 0; i < MAX_EFFECTS; i++) {
            if (gEffects[i].used && gEffects[i].kernel_id == kid) {
                /* Launch worker thread with updated magnitude/duration. */
                struct EffectThreadData* ed = calloc(1, sizeof(*ed));
                if (!ed) {
                    LOG_FF("[FF] Allocation failed for kernel_id=%d\n", kid);
                    return;
                }
                ed->magnitude  = gEffects[i].magnitude;
                ed->durationMs = gEffects[i].durationMs;
                ed->ffType     = gEffects[i].ffType;

                pthread_t th;
                if (pthread_create(&th, NULL, effectThreadFunc, ed) != 0) {
                    LOG_FF("[FF] Thread creation failed for kernel_id=%d\n", kid);
                    free(ed);
                    gEffects[i].used = 0; /* discard data */
                    return;
                }
                pthread_detach(th);
                LOG_FF("[FF] Effect kernel_id=%d => playing in background.\n", kid);
                return;
            }
        }
        LOG_FF("[FF] No stored effect found for kernel_id=%d, ignoring.\n", kid);
    }
    else {
        /* doPlay==0 => just log a "stop" without forcibly killing threads. */
        LOG_FF("[FF] Stop effect: kernel_id=%d (not forcibly cancelled)\n", kid);
    }
}
