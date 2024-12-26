/*****************************************************
 * gammapad_ff.c
 *
 * Aggregator-based Force Feedback:
 *  - We maintain up to MAX_EFFECTS in aggregator memory.
 *  - On UI_END_FF_UPLOAD => storeUploadedEffect() => aggregator slot.
 *  - If real device (g_ffPhysicalFd, g_hasPhysicalFF):
 *      => We attempt EVIOCSFF => store realDevId
 *  - For doPlay=1, we repeatedly toggle "play=1 -> short sleep -> stop=0 -> short sleep"
 *    on the real device, ignoring whether it truly supports the effect type.
 *  - If realDevId < 0 => fallback toggles /sys/class/timed_output/vibrator/enable.
 *  - We remove checks about small vs. big motors. Instead, if eff->type == FF_RUMBLE,
 *    we merge large motor magnitude into the small motor so big rumble isn't lost.
 *  - If the device rejects certain types, we only log the error, then fallback.
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

/* Number of aggregator effect slots */
#define MAX_EFFECTS 32

/* Aggregator’s in-memory representation of each effect */
struct AggregatorEffect {
    int used;             /* whether slot is in use */
    int aggregatorKid;    /* aggregator's effect ID */
    int realDevId;        /* real device effect ID if accepted, else -1 */
    int shouldStop;       /* aggregator STOP => thread ends early */
    unsigned int durationMs; /* from effect->replay.length */
    __u16 ffType;         /* effect->type (for reference) */

    struct ff_effect original; /* entire effect data, unmodified */
};

/* Our aggregator effect slots */
static struct AggregatorEffect gEffects[MAX_EFFECTS];

/*
 * Declared in gammapad_main.c:
 *   g_ffPhysicalFd => real device FD if user used --ffdev=...
 *   g_hasPhysicalFF => 1 if real device found, else 0
 */
extern int g_ffPhysicalFd;
extern int g_hasPhysicalFF;

/*
 * aggregator’s virtual pad FD => for EVIOCRMFF
 */
extern int controllerFd;

/*
 * If fallback => toggles /sys/class/timed_output/vibrator/enable
 */
static const char* VIB_PATH= "/sys/class/timed_output/vibrator/enable";

/*---------------------------------------------------------
 * fallbackToggleMotor:
 *   If realDevId < 0 or EVIOCSFF fails => short ON/OFF
 *--------------------------------------------------------*/
static void fallbackToggleMotor(unsigned int durationMs)
{
    if (!durationMs) return;

    /* Force vibrator OFF initially */
    FILE* f0= fopen(VIB_PATH, "w");
    if (f0) {
        fprintf(f0, "0\n");
        fclose(f0);
    }

    unsigned long long start= getTimeMs();
    unsigned long long end  = start + durationMs;

    while (getTimeMs() < end) {
        /* Turn ON */
        FILE* fOn= fopen(VIB_PATH, "w");
        if (fOn) {
            fprintf(fOn, "1\n");
            fclose(fOn);
        }
        /* Sleep a short chunk => e.g. 15ms */
        usleep(15000);

        /* Turn OFF */
        FILE* fOff= fopen(VIB_PATH, "w");
        if (fOff) {
            fprintf(fOff, "0\n");
            fclose(fOff);
        }
        usleep(30000);
    }
}

/*---------------------------------------------------------
 * realDevRepeatedToggles:
 *   repeated short toggles on real device => aggregatorKid
 *--------------------------------------------------------*/
static void realDevRepeatedToggles(int realDevId,
                                   struct AggregatorEffect* slot)
{
    if (!slot) return;

    /* per your code, half the aggregator's duration => /2 */
    unsigned int duration= slot->durationMs / 2;
    unsigned long long start= getTimeMs();
    unsigned long long end  = start + duration;

    while (1) {
        if (slot->shouldStop) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => aggregator STOP => break.\n",
                   slot->aggregatorKid);
            break;
        }
        unsigned long long now= getTimeMs();
        if (now >= end) {
            break;
        }

        /* EV_FF => play=1 */
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type  = EV_FF;
        ev.code  = realDevId;
        ev.value = 1;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev))<0) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => %s\n",
                   slot->aggregatorKid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => ok.\n",
                   slot->aggregatorKid);
        }
        /* short sleep => e.g. 60ms (original code used 6000 but
           presumably that was 6ms or 60ms).
           We'll keep your "usleep(6000)" => ~6ms or 60ms? */
        usleep(6000);

        /* EV_FF => stop=0 */
        memset(&ev, 0, sizeof(ev));
        ev.type  = EV_FF;
        ev.code  = realDevId;
        ev.value = 0;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev))<0) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => %s\n",
                   slot->aggregatorKid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => ok.\n",
                   slot->aggregatorKid);
        }
        usleep(30000);
    }
}

/*---------------------------------------------------------
 * aggregatorPlayThread => doPlay=1 => repeated toggles
 *--------------------------------------------------------*/
static void* aggregatorPlayThread(void* arg)
{
    struct AggregatorEffect* slot= (struct AggregatorEffect*)arg;
    if (!slot) return NULL;

    int aggregatorKid= slot->aggregatorKid;
    int realDevId    = slot->realDevId;

    /* per your code, aggregatorPlayThread => /4 */
    unsigned int dur = slot->durationMs / 4;

    LOG_FF("[FF-Thread] aggregatorKid=%d => realDevId=%d => start => dur=%u ms\n",
           aggregatorKid, realDevId, dur);

    if (g_hasPhysicalFF && g_ffPhysicalFd>=0 && realDevId>=0) {
        /* repeated toggles on real device */
        realDevRepeatedToggles(realDevId, slot);
    } else {
        /* fallback => toggling vibrator sysfs */
        LOG_FF("[FF-Thread] aggregatorKid=%d => fallback => toggling timed_output.\n",
               aggregatorKid);
        fallbackToggleMotor(dur);
    }

    /* aggregator STOP or time up => ensure real dev is “stop=0” if we had realDevId>=0 */
    if (g_hasPhysicalFF && g_ffPhysicalFd>=0 && realDevId>=0) {
        struct input_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.type  = EV_FF;
        ev.code  = realDevId;
        ev.value = 0;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev))<0) {
            LOG_FF("[FF-Thread] aggregatorKid=%d => final real dev stop => %s\n",
                   aggregatorKid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => final real dev stop => ok.\n",
                   aggregatorKid);
        }
    }

    LOG_FF("[FF-Thread] aggregatorKid=%d => thread exit.\n", aggregatorKid);
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindSlotByKid => find aggregatorKid
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindSlotByKid(int kid)
{
    if (kid < 0) return NULL;
    for (int i=0; i<MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].aggregatorKid==kid) {
            return &gEffects[i];
        }
    }
    return NULL;
}

/*---------------------------------------------------------
 * aggregatorFindFreeSlot => first free or fallback slot=0
 *--------------------------------------------------------*/
static struct AggregatorEffect* aggregatorFindFreeSlot(void)
{
    for (int i=0; i<MAX_EFFECTS; i++){
        if (!gEffects[i].used) return &gEffects[i];
    }
    LOG_FF("[FF] aggregator => no free slot => reusing slot=0.\n");
    return &gEffects[0];
}

/*---------------------------------------------------------
 * aggregatorClearSlot => reset aggregator effect
 *--------------------------------------------------------*/
static void aggregatorClearSlot(struct AggregatorEffect* slot)
{
    if (!slot) return;
    slot->used         = 0;
    slot->aggregatorKid= -1;
    slot->realDevId    = -1;
    slot->shouldStop   = 0;
    slot->durationMs   = 0;
    slot->ffType       = 0;
    memset(&slot->original, 0, sizeof(slot->original));
}

/*---------------------------------------------------------
 * storeUploadedEffect => aggregator => UI_END_FF_UPLOAD
 *   If we see FF_RUMBLE => unify strong/weak => big motor is not lost.
 *--------------------------------------------------------*/
void storeUploadedEffect(struct ff_effect* eff)
{
    if (!eff) return;

    LOG_FF("[FF] aggregator => storeUploadedEffect => aggregatorKid=%d => type=%u => replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);

    /* If it's a FF_RUMBLE effect => unify strong & weak so big motor doesn't vanish. */
    if (eff->type == FF_RUMBLE) {
        unsigned short w = eff->u.rumble.weak_magnitude;
        unsigned short s = eff->u.rumble.strong_magnitude;
        /* simplest approach => pick the bigger or sum them, depending on your preference */
        unsigned short unified = (s > w) ? s : w;
        eff->u.rumble.weak_magnitude   = unified;
        eff->u.rumble.strong_magnitude = unified;
        LOG_FF("[FF] aggregatorKid=%d => FF_RUMBLE => unify big/small => finalMag=%u\n",
               eff->id, unified);
    }

    /* find slot or free slot */
    struct AggregatorEffect* slot= aggregatorFindSlotByKid(eff->id);
    if (!slot) {
        slot= aggregatorFindFreeSlot();
    }

    /* If old slot is used => free old realDev effect if present */
    if (slot->used && slot->realDevId>=0 && g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] aggregatorKid=%d => Freed old realDevId=%d\n",
               slot->aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);

    /* Fill aggregator data */
    slot->used          = 1;
    slot->aggregatorKid = eff->id;
    slot->realDevId     = -1;
    slot->shouldStop    = 0;
    slot->durationMs    = eff->replay.length;
    slot->ffType        = eff->type;
    slot->original      = *eff;

    /* attempt EVIOCSFF => store realDevId if success */
    if (g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        struct ff_effect copy = *eff;
        copy.id= -1; /* request new ID from real dev */

        if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy)==0) {
            slot->realDevId= copy.id;
            LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => success => realDevId=%d\n",
                   eff->id, copy.id);
        } else {
            LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => fail => %s\n",
                   eff->id, strerror(errno));
            slot->realDevId= -1;
        }
    }
}

/*---------------------------------------------------------
 * dummy_upload_ff_effect => aggregator => UI_BEGIN_FF_UPLOAD
 *--------------------------------------------------------*/
int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if (!eff) return -1;
    LOG_FF("[FF] aggregator => dummy_upload_ff_effect => aggregatorKid=%d => type=%u => replay=%u\n",
           eff->id, eff->type, eff->replay.length);
    return 0;
}

/*---------------------------------------------------------
 * dummy_erase_ff_effect => aggregator => UI_BEGIN_FF_ERASE
 *--------------------------------------------------------*/
int dummy_erase_ff_effect(int aggregatorKid)
{
    LOG_FF("[FF] aggregator => dummy_erase_ff_effect => aggregatorKid=%d\n", aggregatorKid);

    struct AggregatorEffect* slot= aggregatorFindSlotByKid(aggregatorKid);
    if (!slot) return 0;

    if (slot->used && slot->realDevId>=0 && g_hasPhysicalFF && g_ffPhysicalFd>=0) {
        ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId);
        LOG_FF("[FF] Freed aggregatorKid=%d => realDevId=%d\n", aggregatorKid, slot->realDevId);
    }
    aggregatorClearSlot(slot);
    return 0;
}

/*---------------------------------------------------------
 * ff_play_effect => aggregator => EV_FF => doPlay=1 => spawn
 *                                 doPlay=0 => aggregator STOP
 *--------------------------------------------------------*/
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
        slot->shouldStop= 0;
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
        LOG_FF("[FF] aggregatorKid=%d => aggregator STOP => set shouldStop=1\n",
               aggregatorKid);
        slot->shouldStop=1;
    }
}
