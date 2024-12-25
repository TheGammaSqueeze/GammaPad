/*****************************************************
 * gammapad_ff.c
 *
 * Provides a "dummy" force-feedback implementation
 * that toggles a single motor (via /sys/class/timed_output/vibrator/enable)
 * unless a real FF device is specified in gammapad_main 
 * (see g_ffPhysicalFd/g_hasPhysicalFF).
 *
 * We forward EV_FF code=<kid> value=1 => play, 0 => stop
 * to the real device if available, else fallback toggling.
 *
 * EXTRA STEP to avoid repeated "play" from the real device:
 * On 'stop' => we forcibly do EVIOCRMFF(kid) so the real device 
 * can't keep replaying effect=0.
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

#define MAX_EFFECTS 32

/*
 * We'll store effect data in gEffects[] for local usage.
 */
struct StoredEffect {
    int used;               /* Whether this slot is in use */
    int kernel_id;          /* Kernel-assigned FF effect ID */
    unsigned int magnitude; /* 0..65535 */
    unsigned int durationMs;/* from effect->replay.length */
    __u16 ffType;
};

static struct StoredEffect gEffects[MAX_EFFECTS];

/*
 * If no real FF device is open, we fallback to the 'timed_output' path.
 */
static const char* VIB_PATH= "/sys/class/timed_output/vibrator/enable";

/*
 * We'll reference these externs from gammapad_main.c
 * If g_hasPhysicalFF=1, we forward events to the real motor.
 */
extern int g_ffPhysicalFd;
extern int g_hasPhysicalFF;

/*
 * Also need controllerFd from gammapad_main for EVIOCRMFF calls
 */
extern int controllerFd;

/*
 * toggleMotorRepeatedly => fallback approach using timed_output
 */
static void toggleMotorRepeatedly(unsigned int durationMs, unsigned int magnitude)
{
    if(!durationMs||!magnitude) return;

    /* Force vibrator OFF first */
    {
        FILE* f0= fopen(VIB_PATH,"w");
        if(f0){
            fprintf(f0,"0\n");
            fclose(f0);
        }
    }

    unsigned long long start= getTimeMs();
    unsigned long long end= start + durationMs;

    /* approximate sleep in microseconds */
    unsigned int sleepUs= 150000 - (unsigned int)((400000.0*magnitude)/65535.0);
    if(sleepUs<10000) sleepUs=10000;

    while(getTimeMs()<end){
        /* turn motor ON */
        FILE* fOn= fopen(VIB_PATH,"w");
        if(fOn){
            fprintf(fOn,"1\n");
            fclose(fOn);
        }
        usleep(sleepUs);
        /* turn motor OFF */
        FILE* fOff= fopen(VIB_PATH,"w");
        if(fOff){
            fprintf(fOff,"0\n");
            fclose(fOff);
        }
    }
}

/*
 * Worker thread data for fallback timed_output approach
 */
struct EffectThreadData {
    unsigned int magnitude;
    unsigned int durationMs;
    __u16 ffType;
};

static void* effectThreadFunc(void* arg)
{
    struct EffectThreadData* ed= (struct EffectThreadData*)arg;
    LOG_FF("[FF-Thread] Type=%u, Magnitude=%u, Duration=%u ms (timed_output fallback)\n",
           ed->ffType, ed->magnitude, ed->durationMs);

    toggleMotorRepeatedly(ed->durationMs, ed->magnitude);
    free(ed);
    return NULL;
}

/*
 * storeUploadedEffect => after UI_END_FF_UPLOAD
 */
void storeUploadedEffect(struct ff_effect* eff)
{
    if(!eff) return;

    int kid= eff->id;
    LOG_FF("[FF] storeUploadedEffect => kid=%d\n", kid);

    /* remove existing effect with same kid if any */
    for(int i=0; i<MAX_EFFECTS; i++){
        if(gEffects[i].used && gEffects[i].kernel_id==kid){
            LOG_FF("[FF] Overwriting existing effect kid=%d in slot=%d\n", kid, i);
            ioctl(controllerFd, EVIOCRMFF, kid);
            gEffects[i].used=0;
        }
    }

    /* find free slot or fallback to 0 */
    int idx=-1;
    for(int i=0; i<MAX_EFFECTS; i++){
        if(!gEffects[i].used){
            idx=i;
            break;
        }
    }
    if(idx<0){
        idx=0;
        if(gEffects[idx].used){
            if(ioctl(controllerFd, EVIOCRMFF, gEffects[idx].kernel_id)==0){
                LOG_FF("[FF] Freed old effect in slot=0 (kid=%d)\n", gEffects[idx].kernel_id);
            }
            gEffects[idx].used=0;
        }
    }

    gEffects[idx].used=1;
    gEffects[idx].kernel_id= kid;

    unsigned int mag=0;
    switch(eff->type){
    case FF_RUMBLE:{
        unsigned int smallMotor= eff->u.rumble.weak_magnitude/2;
        unsigned int largeMotor= eff->u.rumble.strong_magnitude/3;
        if(largeMotor>0 && smallMotor>0){
            mag= largeMotor;
        } else if(largeMotor>0){
            mag= largeMotor;
        } else {
            mag= smallMotor;
        }
        break;
    }
    case FF_CONSTANT:
        mag= eff->u.constant.level;
        break;
    case FF_PERIODIC:
        mag= eff->u.periodic.magnitude;
        break;
    case FF_RAMP:
        mag= (eff->u.ramp.start_level + eff->u.ramp.end_level)/2;
        break;
    case FF_SPRING:
    case FF_DAMPER:
    case FF_INERTIA:
        mag= (eff->u.condition[0].right_coeff + eff->u.condition[0].left_coeff)/2;
        break;
    default:
        mag=20000;
        break;
    }

    gEffects[idx].magnitude= mag;
    gEffects[idx].durationMs= eff->replay.length;
    gEffects[idx].ffType= eff->type;

    LOG_FF("[FF] Stored => slot=%d, mag=%u, dur=%u, type=%u\n",
           idx, mag, eff->replay.length, eff->type);

    /* If we have a real FF device => replicate upload. */
    if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
        struct ff_effect copy= *eff;
        copy.id= -1; 
        if(ioctl(g_ffPhysicalFd, EVIOCSFF, &copy)==0){
            LOG_FF("[FF] Also uploaded effect to real FF device.\n");
        } else {
            LOG_FF("[FF] EVIOCSFF to real dev => %s\n", strerror(errno));
        }
    }
}

int dummy_upload_ff_effect(struct ff_effect* eff)
{
    if(!eff) return -1;
    LOG_FF("[FF] Upload => type=%u, replay=%u ms\n", eff->type, eff->replay.length);
    return 0;
}

int dummy_erase_ff_effect(int kernel_id)
{
    LOG_FF("[FF] Erase => kid=%d\n", kernel_id);
    for(int i=0; i<MAX_EFFECTS; i++){
        if(gEffects[i].used && gEffects[i].kernel_id==kernel_id){
            if(ioctl(controllerFd, EVIOCRMFF, kernel_id)==0){
                LOG_FF("[FF] Freed slot for kid=%d\n", kernel_id);
            }
            gEffects[i].used=0;
            break;
        }
    }
    /* also remove from real FF device if open */
    if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
        if(ioctl(g_ffPhysicalFd, EVIOCRMFF, kernel_id)==0){
            LOG_FF("[FF] Freed effect on real dev => kid=%d\n", kernel_id);
        }
    }
    return 0;
}

/*
 * ff_play_effect => EV_FF code=<kid> value=1 => play, 0 => stop
 *
 * To avoid the device re-sending play for kid=0, on stop we forcibly do EVIOCRMFF
 * on the real device for that effect. This prevents lingering replays.
 */
void ff_play_effect(int kid, int doPlay)
{
    if(doPlay){
        for(int i=0; i<MAX_EFFECTS; i++){
            if(gEffects[i].used && gEffects[i].kernel_id==kid){
                if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
                    /* forward EV_FF => real device => no fallback */
                    struct input_event play;
                    memset(&play,0,sizeof(play));
                    play.type= EV_FF;
                    play.code= kid;
                    play.value=1;
                    if(write(g_ffPhysicalFd, &play, sizeof(play))<0){
                        LOG_FF("[FF] Real dev play => %s\n", strerror(errno));
                    } else {
                        LOG_FF("[FF] Real dev play => kid=%d\n", kid);
                    }
                } else {
                    /* fallback => timed_output toggling */
                    struct EffectThreadData* ed= calloc(1,sizeof(*ed));
                    if(!ed){
                        LOG_FF("[FF] Allocation fail => kid=%d\n", kid);
                        return;
                    }
                    ed->magnitude= gEffects[i].magnitude;
                    ed->durationMs= gEffects[i].durationMs;
                    ed->ffType= gEffects[i].ffType;

                    pthread_t th;
                    if(pthread_create(&th,NULL,effectThreadFunc,ed)!=0){
                        LOG_FF("[FF] pthread_create fail => kid=%d\n", kid);
                        free(ed);
                        gEffects[i].used=0;
                        return;
                    }
                    pthread_detach(th);
                    LOG_FF("[FF] effect kid=%d => playing fallback.\n", kid);
                }
                return;
            }
        }
        LOG_FF("[FF] No stored effect => kid=%d => ignoring.\n", kid);
    } else {
        LOG_FF("[FF] Stop effect => kid=%d\n", kid);
        if(g_hasPhysicalFF && g_ffPhysicalFd>=0){
            /* We write a stop event, then forcibly remove the effect so it won't keep replaying. */
            struct input_event stop;
            memset(&stop,0,sizeof(stop));
            stop.type= EV_FF;
            stop.code= kid;
            stop.value=0;
            if(write(g_ffPhysicalFd,&stop,sizeof(stop))<0){
                LOG_FF("[FF] Real dev stop => %s\n", strerror(errno));
            } else {
                LOG_FF("[FF] Real dev stop => kid=%d\n", kid);
            }
            /* forcibly remove from real device so it can't re-emit */
            ioctl(g_ffPhysicalFd, EVIOCRMFF, kid);
        }
        // We do not forcibly kill fallback threads, but the effect ends naturally.
    }
}
