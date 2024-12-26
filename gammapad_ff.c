/*****************************************************
 * gammapad_ff.c
 *
 * Minimal aggregator-based Force Feedback:
 *  - Stores up to MAX_EFFECTS aggregator slots.
 *  - Always passes effects unmodified to the real device
 *    if g_hasPhysicalFF && g_ffPhysicalFd >= 0.
 *  - If the real device rejects, aggregator sets realDevId=-1
 *    so physically no effect is produced.
 *  - If realDevId=-1 (or no physical device),
 *    uses a fallback "timed_output" approach.
 *  - Ensures both small and big motors get the
 *    same data from aggregator’s FF_RUMBLE.
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

/* Number of aggregator effect slots we maintain */
#define MAX_EFFECTS 32

/* This struct holds aggregator's notion of each effect */
struct AggregatorEffect {
    int used;              /* slot in use? */
    int aggregatorKid;     /* aggregator's effect ID (kernel_id from aggregator's perspective) */
    int realDevId;         /* real device effect ID if accepted, else -1 */
    int shouldStop;        /* aggregator STOP flag => thread ends early */
    unsigned int durationMs;/* effect->replay.length or simplified interpretation */
    __u16 ffType;          /* effect->type for reference */

    /* We'll store the entire original ff_effect in case we need it */
    struct ff_effect original;
};

/* Our aggregator effect slots */
static struct AggregatorEffect gEffects[MAX_EFFECTS];

/*
 * If we have a real FF device (e.g. /dev/input/event1),
 * we store it in g_ffPhysicalFd along with g_hasPhysicalFF=1.
 * Declared in gammapad_main.c (and set from arguments).
 */
extern int g_ffPhysicalFd;
extern int g_hasPhysicalFF;

/*
 * The aggregator’s virtual pad FD, so we can call EVIOCRMFF if needed.
 * Declared in gammapad_main.c
 */
extern int controllerFd;

/*
 * For fallback "timed_output" approach:
 */
static const char* VIB_PATH = "/sys/class/timed_output/vibrator/enable";

/****************************************************************************
 * Timed-Output fallback: toggles the motor in short bursts if realDevId<0
 ****************************************************************************/
static void toggleMotorRepeatedly(unsigned int durationMs, unsigned int magnitude)
{
    if (!durationMs || !magnitude) return;

    /* Force vibrator OFF first */
    {
        FILE* f0 = fopen(VIB_PATH,"w");
        if (f0) {
            fprintf(f0,"0\n");
            fclose(f0);
        }
    }
    unsigned long long start= getTimeMs();
    unsigned long long end  = start + durationMs;

    /* we do a simplified approach: short ON -> short OFF repeatedly */
    while (getTimeMs() < end) {
        /* turn ON */
        FILE* fOn= fopen(VIB_PATH,"w");
        if (fOn) {
            fprintf(fOn,"1\n");
            fclose(fOn);
        }
        usleep(150000 - (magnitude/4)); /* or something simpler */

        /* turn OFF */
        FILE* fOff= fopen(VIB_PATH,"w");
        if (fOff) {
            fprintf(fOff,"0\n");
            fclose(fOff);
        }
        usleep(30000);
    }
}

/***************************************************************************
 * aggregatorPlayThread => separate thread for doPlay=1
 *   - If realDevId>=0 => send real dev play
 *   - Wait duration or aggregator STOP => real dev stop
 *   - If realDevId<0 => fallback timed_output approach
 ***************************************************************************/
static void* aggregatorPlayThread(void* arg)
{
    struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
    if (!slot) return NULL;

    int kid       = slot->aggregatorKid;
    int realDevId = slot->realDevId;
    unsigned int dur = slot->durationMs/2;

    LOG_FF("[FF-Thread] aggregatorKid=%d => realDevId=%d => start => dur=%u ms\n",
           kid, realDevId, dur);

    /* If real dev accepted => EV_FF play=1 */
    if (g_hasPhysicalFF && g_ffPhysicalFd>=0 && realDevId>=0) {
        struct input_event play;
        memset(&play, 0, sizeof(play));
        play.type  = EV_FF;
        play.code  = realDevId;
        play.value = 1;
        if (write(g_ffPhysicalFd, &play, sizeof(play))<0) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => %s\n",
                   kid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => success.\n", kid);
        }
    }

    /* If real dev not accepted => fallback timed_output approach in background. */
    int fallback = 0;
    if (!(g_hasPhysicalFF && g_ffPhysicalFd>=0 && realDevId>=0)) {
        fallback=1;
        LOG_FF("[FF-Thread] aggregatorKid=%d => realDevId<0 => fallback.\n", kid);
    }

    /* We'll do a simple loop => break if aggregator STOP or time up. */
    unsigned long long startMs= getTimeMs();
    unsigned long long endMs  = startMs + dur;

    if (!fallback) {
        /* real dev approach => just sleep-check for aggregator STOP. */
        while (1) {
            if (slot->shouldStop) {
                LOG_FF("[FF-Thread] aggregatorKid=%d => aggregator STOP => break.\n", kid);
                break;
            }
            if (getTimeMs() >= endMs) {
                break;
            }
            usleep(5*1000);
        }
    } else {
        /* fallback => timed_output in short toggles, or simpler approach. */
        unsigned long long now;
        while ((now= getTimeMs()) < endMs) {
            if (slot->shouldStop) {
                LOG_FF("[FF-Thread] aggregatorKid=%d => aggregator STOP => fallback break.\n", kid);
                break;
            }
            /* We can do a short chunk: e.g. 50ms => ON, 50ms => OFF, etc. */
            unsigned long toGo= endMs - now;
            if (toGo>50) toGo=50;
            FILE* fOn= fopen(VIB_PATH,"w");
            if (fOn) {
                fprintf(fOn,"1\n");
                fclose(fOn);
            }
            usleep(toGo * 1000);

            FILE* fOff= fopen(VIB_PATH,"w");
            if (fOff) {
                fprintf(fOff,"0\n");
                fclose(fOff);
            }
            usleep(10*10000);
        }
    }

    /* aggregator STOP or done => real dev => stop=0 if realDevId>=0 */
    if (g_hasPhysicalFF && g_ffPhysicalFd>=0 && realDevId>=0) {
        struct input_event stopEv;
        memset(&stopEv, 0, sizeof(stopEv));
        stopEv.type  = EV_FF;
        stopEv.code  = realDevId;
        stopEv.value = 0;
        if (write(g_ffPhysicalFd, &stopEv, sizeof(stopEv))<0) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => %s\n", kid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => success.\n", kid);
        }
    }

    LOG_FF("[FF-Thread] aggregatorKid=%d => thread exit.\n", kid);
    return NULL;
}

/***************************************************************************
 * aggregatorFindSlotByKid => locate aggregatorKid in gEffects
 ***************************************************************************/
static struct AggregatorEffect* aggregatorFindSlotByKid(int kid)
{
    if (kid<0) return NULL;
    for (int i=0; i<MAX_EFFECTS; i++){
        if (gEffects[i].used && gEffects[i].aggregatorKid==kid) {
            return &gEffects[i];
        }
    }
    return NULL;
}

/***************************************************************************
 * aggregatorFindFreeSlot => find a free slot or fallback to slot=0
 ***************************************************************************/
static struct AggregatorEffect* aggregatorFindFreeSlot(void)
{
    for (int i=0; i<MAX_EFFECTS; i++){
        if (!gEffects[i].used) return &gEffects[i];
    }
    /* no free => reuse slot=0 */
    LOG_FF("[FF] aggregator => no free slot => reusing slot=0.\n");
    return &gEffects[0];
}

/***************************************************************************
 * aggregatorClearSlot => sets used=0 => aggregatorKid=-1 => realDevId=-1
 ***************************************************************************/
static void aggregatorClearSlot(struct AggregatorEffect* slot)
{
    if (!slot) return;
    slot->used=0;
    slot->aggregatorKid=-1;
    slot->realDevId=-1;
    slot->shouldStop=0;
    slot->durationMs=0;
    slot->ffType=0;
    memset(&slot->original,0,sizeof(slot->original));
}

/***************************************************************************
 * storeUploadedEffect => aggregator calls from UI_END_FF_UPLOAD
 *   - remove old realDev effect if needed
 *   - create aggregator slot
 *   - if real dev is available => EVIOCSFF => store realDevId
 ***************************************************************************/
void storeUploadedEffect(struct ff_effect* eff)
{
    if (!eff) return;
    LOG_FF("[FF] aggregator => storeUploadedEffect => aggregatorKid=%d => type=%u => replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);

    /* find existing slot or free slot */
    struct AggregatorEffect* slot= aggregatorFindSlotByKid(eff->id);
    if (!slot) {
        slot= aggregatorFindFreeSlot();
    }

    /* if slot used => remove old realDev effect */
    if (slot->used && slot->realDevId>=0 && g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] aggregatorKid=%d => Freed old realDevId=%d\n",
               slot->aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);

    /* fill aggregator data */
    slot->used=1;
    slot->aggregatorKid= eff->id;
    slot->realDevId= -1;
    slot->shouldStop=0;
    slot->durationMs= eff->replay.length;
    slot->ffType= eff->type;
    slot->original= *eff;

    /* Attempt to EVIOCSFF on real dev if present. */
    if (g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        struct ff_effect copy= *eff;
        copy.id= -1; /* request new ID from real dev */

        /* For FF_RUMBLE => do not manipulate big/small. Pass as is. */
        if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy)==0) {
            slot->realDevId= copy.id;
            LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => success => realDevId=%d\n",
                   eff->id, copy.id);
        } else {
            LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => fail => %s\n",
                   eff->id, strerror(errno));
            slot->realDevId= -1; /* physically does nothing */
        }
    }
}

/***************************************************************************
 * dummy_upload_ff_effect => aggregator => called on UI_BEGIN_FF_UPLOAD
 *   - always success for demonstration
 ***************************************************************************/
int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if (!eff) return -1;
    LOG_FF("[FF] aggregator => dummy_upload_ff_effect => aggregatorKid=%d => type=%u => replay=%u\n",
           eff->id, eff->type, eff->replay.length);
    return 0; /* success so kernel calls UI_END_FF_UPLOAD => storeUploadedEffect() */
}

/***************************************************************************
 * dummy_erase_ff_effect => aggregator => called on UI_BEGIN_FF_ERASE
 ***************************************************************************/
int dummy_erase_ff_effect(int aggregatorKid)
{
    LOG_FF("[FF] aggregator => dummy_erase_ff_effect => aggregatorKid=%d\n", aggregatorKid);

    /* find aggregatorKid => remove from real dev => clear slot */
    struct AggregatorEffect* slot= aggregatorFindSlotByKid(aggregatorKid);
    if (!slot) return 0;
    if (slot->used && slot->realDevId>=0 && g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] Freed slot => aggregatorKid=%d => realDevId=%d\n", aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);

    return 0;
}

/***************************************************************************
 * ff_play_effect => aggregator => EV_FF => start or stop
 *   aggregatorKid => doPlay=1 => spawn aggregatorPlayThread
 *                 => doPlay=0 => set shouldStop=1 => thread stops
 ***************************************************************************/
void ff_play_effect(int aggregatorKid, int doPlay)
{
    LOG_FF("[FF] aggregator => ff_play_effect => aggregatorKid=%d => doPlay=%d\n",
           aggregatorKid, doPlay);

    struct AggregatorEffect* slot= aggregatorFindSlotByKid(aggregatorKid);
    if (!slot || !slot->used) {
        LOG_FF("[FF] aggregatorKid=%d => no used slot => ignoring\n", aggregatorKid);
        return;
    }

    if (doPlay) {
        /* spawn aggregator thread => aggregatorPlayThread */
        slot->shouldStop=0;
        pthread_t th;
        if (pthread_create(&th, NULL, aggregatorPlayThread, slot)!=0) {
            LOG_FF("[FF] aggregatorKid=%d => pthread_create => fail => %s\n",
                   aggregatorKid, strerror(errno));
        } else {
            pthread_detach(th);
            LOG_FF("[FF] aggregatorKid=%d => spawned play thread => realDevId=%d\n",
                   aggregatorKid, slot->realDevId);
        }
    } else {
        /* aggregator => STOP => set shouldStop=1 => aggregator thread ends early */
        LOG_FF("[FF] aggregatorKid=%d => aggregator requests STOP => set shouldStop=1\n",
               aggregatorKid);
        slot->shouldStop=1;
    }
}
