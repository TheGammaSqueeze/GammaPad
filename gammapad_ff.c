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
 *  - In storeUploadedEffect(), we now record the start time of the effect.
 *  - In ff_play_effect(), when a stop event (doPlay==0) is received for a rumble effect,
 *    we check that the effect has run for its full intended duration before stopping.
 *    If not, we schedule a delayed stop so that the vibration isn’t cut short.
 *  - A periodic resynchronization thread is added to:
 *       a) sweep expired effects,
 *       b) reassert the desired state, and
 *       c) acquire an exclusive grab (via EVIOCGRAB) only while an effect is active.
 *  - The PWM thread uses high-resolution absolute timers and, when stopped,
 *    immediately sends an "off" event to ensure the motor is turned off.
 *  - All existing code remains unabridged.
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
 
 struct AggregatorEffect {
     int used;
     int aggregatorKid;
     int realDevId;
     int shouldStop;
     unsigned int durationMs;
     __u16 ffType;
     unsigned long long startTimeMs;
     struct ff_effect original;
 };
 
 static struct AggregatorEffect gEffects[MAX_EFFECTS];
 
 /* Forward declaration for aggregatorClearSlot */
 static void aggregatorClearSlot(struct AggregatorEffect* slot);
 
 /* --- Existing code begins --- */
 
 extern int g_ffPhysicalFd;
 extern int g_hasPhysicalFF;
 extern int controllerFd;
 static const char* VIB_PATH = "/sys/class/timed_output/vibrator/enable";

 /* New Retroid Pocket Classic flag */
extern int g_rpclassic;

// track the last real FF effect we played on the physical device
static int lastEffectId = -1;

// Helper: try writing `data` to `path`, return 0 on success or -1 on error (errno set)
static int writeSysfs(const char *path, const char *data) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    ssize_t w = write(fd, data, strlen(data));
    close(fd);
    return (w == (ssize_t)strlen(data)) ? 0 : -1;
}
 
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
 
 static void fallbackToggleMotor(unsigned int durationMs, volatile int *shouldStop) {
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
     unsigned long long end = start + durationMs;
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
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     while (!pwmThreadShouldStop) {
         sendOnToPhysical(pwmEffectId);
         ts.tv_nsec += pwmOnDuration * 1000000;
         while (ts.tv_nsec >= 1000000000) {
             ts.tv_sec++;
             ts.tv_nsec -= 1000000000;
         }
         clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
         if (pwmThreadShouldStop)
             break;
         sendOffToPhysical(pwmEffectId);
         ts.tv_nsec += pwmOffDuration * 1000000;
         while (ts.tv_nsec >= 1000000000) {
             ts.tv_sec++;
             ts.tv_nsec -= 1000000000;
         }
         clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
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
         sendOffToPhysical(pwmEffectId);
         LOG_FF("[FF] PWM thread stopped\n");
     }
 }
 
 /* --- New function: sweepExpiredEffects ---
      Iterates over all stored effects and clears any that have expired.
 */
static void sweepExpiredEffects(void) {
    unsigned long long now = getTimeMs();
    int usedCount = 0;
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used) {
            usedCount++;
        }
    }
    // only clear expired slots when we've used them all
    if (usedCount < MAX_EFFECTS)
        return;

    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used
            && gEffects[i].ffType != FF_RUMBLE
            && (now - gEffects[i].startTimeMs >= gEffects[i].durationMs)) {
            aggregatorClearSlot(&gEffects[i]);
        }
    }
}
 
 /* --- New: Define update_rumble_state ---
      This function sweeps expired effects and updates the PWM or direct FF state.
 */
static void update_rumble_state(void) {
     // if we don't yet have a real FF device, nothing to do
     if (!g_hasPhysicalFF || g_ffPhysicalFd < 0)
         return;
     sweepExpiredEffects();
     static unsigned long long lastUpdate = 0;
     unsigned long long now = getTimeMs();
     if (now - lastUpdate < 50) return;
     lastUpdate = now;
  
     unsigned int maxMag = 0;
     int effectId = -1;
     int found = 0;
     for (int i = 0; i < MAX_EFFECTS; i++) {
         if (gEffects[i].used && gEffects[i].ffType == FF_RUMBLE) {
             unsigned int mag = gEffects[i].original.u.rumble.weak_magnitude;
             if (mag > maxMag) {
                 maxMag = mag;
                 effectId = (gEffects[i].realDevId >= 0)
                                ? gEffects[i].realDevId
                                : gEffects[i].aggregatorKid;
                 found = 1;
             }
         }
     }

     struct input_event ev;
     memset(&ev, 0, sizeof(ev));
     ev.type = EV_FF;

     if (!found) {
         // only clear our last effect, not unrelated ones
         if (lastEffectId >= 0) {
             stopPWMThread();
             ev.code  = lastEffectId;
             ev.value = 0;
             if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
                 LOG_FF("[FF] update_rumble_state: write failed: %s\n",
                        strerror(errno));
             }
             lastEffectId = -1;
         }
         return;
     }

     if (!g_ffPwmEnabled || maxMag >= g_ffPwmMaxMagnitude) {
         stopPWMThread();
         ev.code  = effectId;
         ev.value = 1;
         if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
             LOG_FF("[FF] update_rumble_state: write failed: %s\n",
                    strerror(errno));
         }
         lastEffectId = effectId;
         return;
     }

     #define PWM_PERIOD_MS 45
     unsigned int onDuration  = (maxMag * PWM_PERIOD_MS)
                                / g_ffPwmMaxMagnitude;
     unsigned int offDuration = PWM_PERIOD_MS - onDuration;
     setGlobalPWMParameters(onDuration, offDuration, effectId);
     if (!pwmActive) {
         startPWMThread();
     }
     // record which effect our PWM thread is modulating
     lastEffectId = effectId;
}
 
 /* --- New structure and function: delayedStopThread ---
      If a stop event is received before the effect’s full duration,
      this thread waits for the remaining time and then clears the effect.
 */
 struct DelayedStopArg {
     struct AggregatorEffect *slot;
     unsigned int remainingMs;
 };
 
 static void* delayedStopThread(void* arg) {
     struct DelayedStopArg *dsArg = (struct DelayedStopArg *)arg;
     if (!dsArg) return NULL;
     msleep(dsArg->remainingMs);
     LOG_FF("[FF] delayedStopThread: stopping effect aggregatorKid=%d after delay of %u ms\n",
            dsArg->slot->aggregatorKid, dsArg->remainingMs);
     aggregatorClearSlot(dsArg->slot);
     update_rumble_state();
     free(dsArg);
     return NULL;
 }
 
 /* --- New: Periodic Resync Thread ---
      This thread periodically re-synchronizes the physical FF device state.
      It also ensures that an exclusive EVIOCGRAB is held only while an effect is active.
 */
 static pthread_t ffResyncThread;
 static volatile int ffResyncThreadShouldStop = 0;
 static int ffGrabbed = 0;
 
static void* ff_resync_thread(void* arg) {
     (void)arg;
     while (!ffResyncThreadShouldStop) {
          update_rumble_state();  // Also sweeps expired effects.
          int active = 0;
          for (int i = 0; i < MAX_EFFECTS; i++) {
              if (gEffects[i].used) {
                  active = 1;
                  break;
              }
          }
          // only consider the device “active” (i.e. keep the grab) when we really
          // have something playing on it
          active = (lastEffectId >= 0);

          if (g_ffPhysicalFd >= 0) {
              if (active && !ffGrabbed) {
                  if (ioctl(g_ffPhysicalFd, EVIOCGRAB, 1) == 0) {
                       ffGrabbed = 1;
                       LOG_FF("[FF] ff_resync_thread: Exclusive grab acquired\n");
                  } else {
                       LOG_FF("[FF] ff_resync_thread: Failed to acquire grab: %s\n", strerror(errno));
                  }
              } else if (!active && ffGrabbed) {
                  if (ioctl(g_ffPhysicalFd, EVIOCGRAB, 0) == 0) {
                       ffGrabbed = 0;
                       LOG_FF("[FF] ff_resync_thread: Exclusive grab released\n");
                       // only clear our last effect, not generic effect 0
                       if (lastEffectId >= 0) {
                           struct input_event off;
                           memset(&off, 0, sizeof(off));
                           off.type  = EV_FF;
                           off.code  = lastEffectId;
                           off.value = 0;
                           write(g_ffPhysicalFd, &off, sizeof(off));
                       }
                  } else {
                       LOG_FF("[FF] ff_resync_thread: Failed to release grab: %s\n", strerror(errno));
                  }
              }
          }
          msleep(100);  // Resync every 100 ms.
     }
     return NULL;
}
 
 /* Start the resync thread automatically */
 __attribute__((constructor))
 static void ff_resync_init(void) {
     pthread_create(&ffResyncThread, NULL, ff_resync_thread, NULL);
 }
 
 /* Stop the resync thread on unload */
 __attribute__((destructor))
 static void ff_resync_deinit(void) {
     ffResyncThreadShouldStop = 1;
     pthread_join(ffResyncThread, NULL);
 }
 
 /* --- Existing aggregatorPlayThread and aggregator helper functions --- */
 static void* aggregatorPlayThread(void* arg) {
     struct AggregatorEffect* slot = (struct AggregatorEffect*)arg;
     if (!slot) return NULL;
     int aggregatorKid = slot->aggregatorKid;
     unsigned int duration = slot->durationMs;
     LOG_FF("[FF-Thread] aggregatorKid=%d, starting fallback effect for %u ms\n", aggregatorKid, duration);
     fallbackToggleMotor(duration, &slot->shouldStop);
     LOG_FF("[FF-Thread] aggregatorKid=%d, fallback effect completed\n", aggregatorKid);
     aggregatorClearSlot(slot);  // Clear the slot after effect completes
     return NULL;
 }
  
 static struct AggregatorEffect* aggregatorFindSlotByKid(int kid) {
     if (kid < 0) return NULL;
     for (int i = 0; i < MAX_EFFECTS; i++) {
         if (gEffects[i].used && gEffects[i].aggregatorKid == kid)
             return &gEffects[i];
     }
     return NULL;
 }
  
 static struct AggregatorEffect* aggregatorFindFreeSlot(void) {
     for (int i = 0; i < MAX_EFFECTS; i++){
         if (!gEffects[i].used) return &gEffects[i];
     }
     LOG_FF("[FF] aggregator => no free slot => reusing slot=0.\n");
     return &gEffects[0];
 }
  
/* Helper: clear an effect slot *and* free it on the physical device */
static void aggregatorClearSlot(struct AggregatorEffect* slot) {
    if (!slot)
        return;

    // If we had a realDevId on the physical device, free it now:
    if (slot->realDevId >= 0 && g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        if (ioctl(g_ffPhysicalFd, EVIOCRMFF, slot->realDevId) == 0) {
            LOG_FF("[FF] Freed effect realDevId=%d\n", slot->realDevId);
        } else {
            LOG_FF("[FF] Failed to free effect realDevId=%d: %s\n",
                   slot->realDevId, strerror(errno));
        }
    }

    // Now clear all of our internal state for this slot
    slot->used         = 0;
    slot->aggregatorKid= -1;
    slot->realDevId    = -1;
    slot->shouldStop   = 0;
    slot->durationMs   = 0;
    slot->ffType       = 0;
    slot->startTimeMs  = 0;
    memset(&slot->original, 0, sizeof(slot->original));
}
  
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
     slot->startTimeMs = getTimeMs();
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
         LOG_FF("[FF] Freed aggregatorKid=%d, realDevId=%d\n", aggregatorKid, slot->realDevId);
     }
     aggregatorClearSlot(slot);
     return 0;
 }
  
void ff_play_effect(int aggregatorKid, int doPlay) {
    LOG_FF("[FF] ff_play_effect: aggregatorKid=%d, doPlay=%d\n", aggregatorKid, doPlay);
    struct AggregatorEffect* slot = aggregatorFindSlotByKid(aggregatorKid);
    if (!slot || !slot->used) {
        LOG_FF("[FF] No active rumble effect slot found; ignoring play event\n");
        return;
    }

    if (g_rpclassic && slot->ffType == FF_RUMBLE) {
        char buf[32];
        int ok;

        if (doPlay) {
            /* 1) Write duration (ms) */
            snprintf(buf, sizeof(buf), "%u", slot->durationMs);
            ok = writeSysfs("/sys/class/leds/vibrator/activate/duration", buf);
            if (ok == 0) {
                LOG_FF("[RPCLASSIC] duration → activate/duration = %sms\n", buf);
            } else if (writeSysfs("/sys/class/leds/vibrator/duration", buf) == 0) {
                LOG_FF("[RPCLASSIC] duration → duration = %sms\n", buf);
            } else if (writeSysfs("/sys/class/timed_output/vibrator/enable", buf) == 0) {
                LOG_FF("[RPCLASSIC] duration → timed_output = %sms\n", buf);
            } else {
                LOG_FF("[RPCLASSIC] Failed to set duration via any path: %s\n", strerror(errno));
            }

            /* 2) Activate vibration */
            ok = writeSysfs("/sys/class/leds/vibrator/activate", "1");
            if (ok == 0) {
                LOG_FF("[RPCLASSIC] vibrate → activate = 1\n");
            } else if (writeSysfs("/sys/class/timed_output/vibrator/enable", buf) == 0) {
                LOG_FF("[RPCLASSIC] vibrate → timed_output = %sms\n", buf);
            } else {
                LOG_FF("[RPCLASSIC] Failed to activate vibrator: %s\n", strerror(errno));
            }
        } else {
            /* Stop vibration early */
            if (writeSysfs("/sys/class/leds/vibrator/activate", "0") == 0) {
                LOG_FF("[RPCLASSIC] vibrate → activate = 0\n");
            } else if (writeSysfs("/sys/class/timed_output/vibrator/enable", "0") == 0) {
                LOG_FF("[RPCLASSIC] vibrate → timed_output = 0\n");
            } else {
                LOG_FF("[RPCLASSIC] Failed to stop vibrator: %s\n", strerror(errno));
            }
        }
        return;
    }

    /* If we have a physical device, always passthrough rumble immediately. */
    if (g_hasPhysicalFF && g_ffPhysicalFd >= 0) {
        int effectId = (slot->realDevId >= 0) ? slot->realDevId : aggregatorKid;

        if (slot->ffType == FF_RUMBLE) {
            if (doPlay) {
                /* start/update PWM or direct-on as before */
                update_rumble_state();
                LOG_FF("[FF] Starter ping for aggregatorKid=%d\n", aggregatorKid);
            } else {
                /* STOP — ensure full intended duration before cutting off */
                unsigned long long now = getTimeMs();
                unsigned long long elapsed = now - slot->startTimeMs;
                if (elapsed < slot->durationMs) {
                    /* schedule delayed stop for remaining time */
                    unsigned int rem = slot->durationMs - elapsed;
                    struct DelayedStopArg *dsArg = malloc(sizeof(*dsArg));
                    if (dsArg) {
                        dsArg->slot = slot;
                        dsArg->remainingMs = rem;
                        pthread_t th;
                        if (pthread_create(&th, NULL, delayedStopThread, dsArg) == 0) {
                            pthread_detach(th);
                            LOG_FF("[FF] Scheduled delayed stop for aggregatorKid=%d in %u ms\n",
                                   slot->aggregatorKid, rem);
                        } else {
                            LOG_FF("[FF] Failed to create delayed stop thread: %s\n", strerror(errno));
                            /* fallback immediate stop */
                            struct input_event ev = {0};
                            ev.type  = EV_FF;
                            ev.code  = effectId;
                            ev.value = 0;
                            write(g_ffPhysicalFd, &ev, sizeof(ev));
                            aggregatorClearSlot(slot);
                        }
                    }
                } else {
                    /* duration elapsed: immediate stop */
                    struct input_event ev = {0};
                    ev.type  = EV_FF;
                    ev.code  = effectId;
                    ev.value = 0;
                    if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
                        LOG_FF("[FF] ff_play_effect: write off failed: %s\n", strerror(errno));
                    }
                    aggregatorClearSlot(slot);
                }
            }
            return;
        }

        /* Non-rumble effects still passthrough 1:1 */
        struct input_event ev = {0};
        ev.type  = EV_FF;
        ev.code  = effectId;
        ev.value = doPlay;
        if (write(g_ffPhysicalFd, &ev, sizeof(ev)) < 0) {
            LOG_FF("[FF] Direct passthrough write failed: %s\n", strerror(errno));
        }
        if (!doPlay) {
            /* also clear the slot so we won’t resync it later */
            aggregatorClearSlot(slot);
        }
        return;
    }

    /* FALLBACK: original software-only path */
    if (doPlay) {
        if (!slot->shouldStop) {
            slot->shouldStop = 1;
            msleep(100);
        }
        slot->shouldStop = 0;
        pthread_t th;
        if (pthread_create(&th, NULL, aggregatorPlayThread, slot) != 0) {
            LOG_FF("[FF] pthread_create failed: %s\n", strerror(errno));
        } else {
            pthread_detach(th);
            LOG_FF("[FF] spawned play thread for aggregatorKid=%d\n", aggregatorKid);
        }
    } else {
        slot->shouldStop = 1;
    }
}
  
 void aggregatorReuploadAllEffects(void) {
     if (!g_hasPhysicalFF || g_ffPhysicalFd < 0) {
         fprintf(stderr, "[FF] aggregatorReuploadAllEffects: no real FF device; skipping.\n");
         return;
     }
     fprintf(stderr, "[FF] aggregatorReuploadAllEffects: re-uploading aggregator slots; FD=%d\n", g_ffPhysicalFd);
     for (int i = 0; i < MAX_EFFECTS; i++) {
         if (!gEffects[i].used)
             continue;
         struct ff_effect copy = gEffects[i].original;
         copy.id = -1;
         if (!test_effect_support(copy.type)) {
             fprintf(stderr, "[FF] aggregatorKid=%d: effect type %u not supported; skipping reupload.\n",
                     gEffects[i].aggregatorKid, copy.type);
             gEffects[i].realDevId = -1;
             continue;
         }
         if (ioctl(g_ffPhysicalFd, EVIOCSFF, &copy) == 0) {
             gEffects[i].realDevId = copy.id;
             fprintf(stderr, "[FF] aggregatorKid=%d: reupload successful; new realDevId=%d\n",
                     gEffects[i].aggregatorKid, copy.id);
         } else {
             fprintf(stderr, "[FF] aggregatorKid=%d: reupload failed: %s\n",
                     gEffects[i].aggregatorKid, strerror(errno));
             gEffects[i].realDevId = -1;
         }
     }
 }
 
 /**
 * getAggregatorKidForRealDevId
 *
 * Given a realDevId coming from the kernel, return the
 * original aggregatorKid so that ff_play_effect() can
 * find the proper slot.
 */
int getAggregatorKidForRealDevId(int realDevId) {
    for (int i = 0; i < MAX_EFFECTS; i++) {
        if (gEffects[i].used && gEffects[i].realDevId == realDevId) {
            return gEffects[i].aggregatorKid;
        }
    }
    return -1;
}