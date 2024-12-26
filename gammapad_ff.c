/*****************************************************
 * gammapad_ff.c
 *
 * Force Feedback aggregator for GammaPad.
 *
 * Behavior:
 *  - GammaPad acts as the ultimate virtual joypad, storing
 *    aggregatorKid => effect data locally.
 *  - On aggregator “upload,” we store the aggregatorKid
 *    in local memory and also replicate EVIOCSFF to the
 *    real device (same effect ID).
 *  - On aggregator “play => code=kid => value=1,” we spawn
 *    a thread to:
 *      (a) log aggregator + real device “play”
 *      (b) send (EV_FF, code=kid, value=1) to the real device
 *      (c) sleep for replay.length ms (if > 0)
 *      (d) if not explicitly stopped earlier, auto-stop
 *          (EV_FF, code=kid, value=0) so the effect won’t
 *          run forever on the real device.
 *  - On aggregator “stop => code=kid => value=0,” we set
 *    a “shouldStop” flag so that if there’s a thread running,
 *    it sends a stop to real device immediately and exits.
 *  - We do *not* remove the effect from the real device on
 *    normal stop. Only aggregator “erase” or overwriting
 *    the effect removes it (EVIOCRMFF).
 *  - No EVIOCGRAB => we do *not* take exclusive access.
 *  - We add logs for aggregatorKid and real device actions
 *    to help you debug current states of effects.
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

/* Max aggregatorKid slots. */
#define MAX_AGGREGATOR_EFFECTS 32

/* Each aggregatorKid is stored in a slot with effect data. */
struct AggregatorSlot {
    int used;
    int aggregatorKid;
    __u16 ffType;
    unsigned int replayLengthMs;
    unsigned int strongMag; /* optional usage from FF_RUMBLE */
    unsigned int weakMag;
    /* A thread to run “play => auto-stop after replayLength.” */
    pthread_t playThread;
    int threadActive;  /* whether a playThread is running */
    int shouldStop;    /* aggregator or auto-stop requests a stop */
};

/* Our aggregatorKid array. */
static struct AggregatorSlot gSlots[MAX_AGGREGATOR_EFFECTS];

/* Extern from gammapad_main. If g_ffPhysicalFd<0, no physical device is present. */
extern int g_ffPhysicalFd;
extern int g_hasPhysicalFF;
extern int controllerFd;  /* aggregator side */

static pthread_mutex_t g_ffMutex = PTHREAD_MUTEX_INITIALIZER;

/* Helper: remove aggregatorKid from real device. */
static void removeRealEffect(int aggregatorKid)
{
    if(g_ffPhysicalFd<0) return;
    ioctl(g_ffPhysicalFd, EVIOCRMFF, aggregatorKid);
}

/*
 * For aggregator “upload” => minimal log.
 */
int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if(!eff) return -1;
    LOG_FF("[FF] aggregator: upload => aggregatorKid=%d, type=%u, replay=%u ms\n",
           eff->id, eff->type, eff->replay.length);
    return 0;
}

/*
 * For aggregator “erase => aggregatorKid,” we remove from aggregator + real device.
 */
int dummy_erase_ff_effect(int aggregatorKid)
{
    LOG_FF("[FF] aggregator: erase => aggregatorKid=%d\n", aggregatorKid);

    pthread_mutex_lock(&g_ffMutex);
    for(int i=0;i<MAX_AGGREGATOR_EFFECTS;i++){
        if(gSlots[i].used && gSlots[i].aggregatorKid== aggregatorKid){
            /* Cancel any thread if running. */
            if(gSlots[i].threadActive){
                /* Mark shouldStop => the thread will auto-stop real device. */
                gSlots[i].shouldStop=1;
            }
            /* Also remove aggregatorKid from aggregator. */
            ioctl(controllerFd, EVIOCRMFF, aggregatorKid);
            gSlots[i].used=0;
            LOG_FF("[FF] aggregatorKid=%d => aggregator slot=%d => removed.\n", aggregatorKid,i);
            break;
        }
    }
    /* remove from real device. */
    removeRealEffect(aggregatorKid);
    pthread_mutex_unlock(&g_ffMutex);

    return 0;
}

/* 
 * The worker thread that handles “play => aggregatorKid => 1” logic:
 *   1) Send EV_FF => code=aggregatorKid => value=1 to real device
 *   2) Sleep replayLengthMs if > 0
 *   3) If we are not told to stop early => do “stop => aggregatorKid => 0”
 *   4) Mark thread as inactive
 */
static void* playThreadFunc(void* arg)
{
    struct AggregatorSlot* slot= (struct AggregatorSlot*)arg;
    int kid= slot->aggregatorKid;

    LOG_FF("[FF-Thread] aggregatorKid=%d => thread started => replay=%u ms\n",
           kid, slot->replayLengthMs);

    /* Step 1: start effect on real device => EV_FF => code=kid => value=1 */
    if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
        LOG_FF("[FF-Thread] aggregatorKid=%d => real dev => PLAY(1)\n", kid);

        struct input_event ev;
        memset(&ev,0,sizeof(ev));
        ev.type= EV_FF;
        ev.code= kid;
        ev.value= 1; 
        if(write(g_ffPhysicalFd, &ev, sizeof(ev))<0){
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => fail => %s\n",
                   kid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev play => success.\n", kid);
        }
    }

    /* Step 2: Sleep replayLengthMs if > 0. If replay=0 => indefinite. */
    unsigned int dur= slot->replayLengthMs;
    if(dur>0){
        unsigned int slept=0;
        while(slept < dur && !slot->shouldStop){
            unsigned int chunk= (dur - slept)>200 ? 200 : (dur - slept);
            usleep(chunk*1000);
            slept+= chunk;
        }
    } else {
        /* indefinite => wait until aggregator or user calls “stop => aggregatorKid => 0”. */
        while(!slot->shouldStop){
            usleep(200*1000); /* 200ms chunk checks */
        }
    }

    /* Step 3: if we have not been told to stop => aggregatorKid => 0 => do it automatically. */
    if(!slot->shouldStop){
        /* Auto-stop after replayLength. */
        LOG_FF("[FF-Thread] aggregatorKid=%d => auto-stop after replay=%u ms\n",
               kid, dur);
    }

    /* Either aggregator user called “stop => aggregatorKid => 0” or we’re auto-stopping. */
    if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
        LOG_FF("[FF-Thread] aggregatorKid=%d => real dev => STOP(0)\n", kid);

        struct input_event evStop;
        memset(&evStop,0,sizeof(evStop));
        evStop.type= EV_FF;
        evStop.code= kid;
        evStop.value=0; 
        if(write(g_ffPhysicalFd, &evStop, sizeof(evStop))<0){
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => fail => %s\n",
                   kid, strerror(errno));
        } else {
            LOG_FF("[FF-Thread] aggregatorKid=%d => real dev stop => success.\n", kid);
        }
    }

    pthread_mutex_lock(&g_ffMutex);
    slot->threadActive= 0;
    slot->shouldStop= 0;
    pthread_mutex_unlock(&g_ffMutex);

    LOG_FF("[FF-Thread] aggregatorKid=%d => thread exit.\n", kid);
    return NULL;
}

/*
 * storeUploadedEffect => aggregator “upload => aggregatorKid.” 
 *  - remove aggregatorKid from aggregator if exists
 *  - create aggregator slot => store replay length, magnitudes, etc.
 *  - replicate aggregatorKid => real device => EVIOCSFF => newE.id= aggregatorKid
 */
void storeUploadedEffect(struct ff_effect* eff)
{
    if(!eff) return;
    int kid= eff->id;

    LOG_FF("[FF] aggregator: storeUploadedEffect => aggregatorKid=%d => type=%u => replay=%u ms\n",
           kid, eff->type, eff->replay.length);

    pthread_mutex_lock(&g_ffMutex);

    /* remove aggregatorKid from aggregator if it exists => also remove from real dev. */
    for(int i=0; i<MAX_AGGREGATOR_EFFECTS; i++){
        if(gSlots[i].used && gSlots[i].aggregatorKid == kid){
            LOG_FF("[FF] aggregatorKid=%d => overwriting old => remove aggregator side.\n", kid);
            ioctl(controllerFd, EVIOCRMFF, kid);
            removeRealEffect(kid);
            gSlots[i].used=0;
        }
    }

    /* find free slot or fallback => slot=0. */
    int idx=-1;
    for(int i=0; i<MAX_AGGREGATOR_EFFECTS; i++){
        if(!gSlots[i].used){
            idx= i;
            break;
        }
    }
    if(idx<0){
        idx= 0;
        LOG_FF("[FF] aggregatorKid=%d => no free slot => overwriting slot=0.\n", kid);
        ioctl(controllerFd, EVIOCRMFF, gSlots[0].aggregatorKid);
        removeRealEffect(gSlots[0].aggregatorKid);
        gSlots[0].used=0;
    }

    /* fill aggregator slot. */
    gSlots[idx].used=1;
    gSlots[idx].aggregatorKid= kid;
    gSlots[idx].ffType= eff->type;
    gSlots[idx].replayLengthMs= eff->replay.length;
    gSlots[idx].threadActive= 0;
    gSlots[idx].shouldStop= 0;

    /* parse out rumble magnitudes if needed. */
    gSlots[idx].strongMag= 0;
    gSlots[idx].weakMag= 0;

    if(eff->type==FF_RUMBLE){
        gSlots[idx].strongMag= eff->u.rumble.strong_magnitude;
        gSlots[idx].weakMag= eff->u.rumble.weak_magnitude;
    }

    LOG_FF("[FF] aggregatorKid=%d => aggregator slot=%d => replay=%u ms => type=%u\n",
           kid, idx, eff->replay.length, eff->type);

    /* replicate aggregatorKid => real device if present. */
    if(!g_hasPhysicalFF || g_ffPhysicalFd<0){
        LOG_FF("[FF] no real device => ignoring EVIOCSFF.\n");
        pthread_mutex_unlock(&g_ffMutex);
        return;
    }

    struct ff_effect newE;
    memset(&newE,0,sizeof(newE));
    newE.id= kid; /* same aggregatorKid => real dev. */
    newE.type= eff->type;
    newE.replay.length= eff->replay.length;
    newE.replay.delay= 0;

    switch(eff->type){
    case FF_RUMBLE:
        newE.u.rumble.strong_magnitude= eff->u.rumble.strong_magnitude;
        newE.u.rumble.weak_magnitude= eff->u.rumble.weak_magnitude;
        break;
    case FF_CONSTANT:
        newE.u.constant.level= eff->u.constant.level;
        newE.u.constant.envelope= eff->u.constant.envelope;
        break;
    case FF_PERIODIC:
        newE.u.periodic.waveform= eff->u.periodic.waveform;
        newE.u.periodic.magnitude= eff->u.periodic.magnitude;
        newE.u.periodic.offset= eff->u.periodic.offset;
        newE.u.periodic.phase= eff->u.periodic.phase;
        newE.u.periodic.period= eff->u.periodic.period;
        newE.u.periodic.envelope= eff->u.periodic.envelope;
        break;
    case FF_RAMP:
        newE.u.ramp.start_level= eff->u.ramp.start_level;
        newE.u.ramp.end_level= eff->u.ramp.end_level;
        newE.u.ramp.envelope= eff->u.ramp.envelope;
        break;
    case FF_SPRING:
    case FF_DAMPER:
    case FF_INERTIA:
    case FF_FRICTION:
        for(int i=0;i<FF_MAX_EFFECTS;i++){
            newE.u.condition[i]= eff->u.condition[i];
        }
        break;
    default:
        break;
    }

    int rc= ioctl(g_ffPhysicalFd, EVIOCSFF, &newE);
    if(rc<0){
        LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => fail => %s\n",
               kid, strerror(errno));
    } else {
        LOG_FF("[FF] aggregatorKid=%d => EVIOCSFF => success.\n", kid);
    }

    pthread_mutex_unlock(&g_ffMutex);
}

/*
 * ff_play_effect => aggregator “play => code=kid => value=1” or “stop => code=kid => value=0.”
 *   - If doPlay=1 => spawn a background thread if not already running => to handle the effect.
 *   - If doPlay=0 => set shouldStop=1 => the thread will do “stop => aggregatorKid => 0” on real device.
 */
void ff_play_effect(int aggregatorKid, int doPlay)
{
    LOG_FF("[FF] aggregator: ff_play_effect => aggregatorKid=%d => doPlay=%d\n", aggregatorKid, doPlay);

    pthread_mutex_lock(&g_ffMutex);

    /* find aggregatorKid in aggregator slots. */
    int idx=-1;
    for(int i=0;i<MAX_AGGREGATOR_EFFECTS;i++){
        if(gSlots[i].used && gSlots[i].aggregatorKid== aggregatorKid){
            idx=i;
            break;
        }
    }
    if(idx<0){
        LOG_FF("[FF] aggregatorKid=%d => not found => ignoring.\n", aggregatorKid);
        pthread_mutex_unlock(&g_ffMutex);
        return;
    }

    if(doPlay){
        /* doPlay=1 => if threadActive=0 => spawn a new thread => handle the effect. */
        if(!gSlots[idx].threadActive){
            gSlots[idx].shouldStop=0; /* reset in case it was set */
            gSlots[idx].threadActive=1;
            if(pthread_create(&gSlots[idx].playThread, NULL, playThreadFunc, &gSlots[idx])!=0){
                LOG_FF("[FF] aggregatorKid=%d => thread create fail => %s\n",
                       aggregatorKid, strerror(errno));
                gSlots[idx].threadActive=0;
            } else {
                pthread_detach(gSlots[idx].playThread);
                LOG_FF("[FF] aggregatorKid=%d => spawned new play thread => slot=%d\n",
                       aggregatorKid, idx);
            }
        } else {
            /* If a thread is already active => aggregator re-play => no-op? */
            LOG_FF("[FF] aggregatorKid=%d => thread already running => ignoring re-play.\n",
                   aggregatorKid);
        }
    } else {
        /* doPlay=0 => aggregator is requesting a stop => set shouldStop=1 => the thread will handle real dev stop. */
        LOG_FF("[FF] aggregatorKid=%d => aggregator requests STOP => set shouldStop=1\n", aggregatorKid);
        gSlots[idx].shouldStop=1;
    }

    pthread_mutex_unlock(&g_ffMutex);
}
