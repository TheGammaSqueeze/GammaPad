#ifndef GAMMAPAD_H
#define GAMMAPAD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <ctype.h>
#include <sys/time.h>
#include <pthread.h>

/* 
 * If you want more logs, compile with -DGAMMAPAD_VERBOSE_LOGGING=1
 */
#ifndef GAMMAPAD_VERBOSE_LOGGING
#define GAMMAPAD_VERBOSE_LOGGING 0
#endif

/* Logging macro for Force Feedback if verbose logging is enabled. */
#if GAMMAPAD_VERBOSE_LOGGING
  #define LOG_FF(fmt, args...) fprintf(stderr, fmt, ## args)
#else
  #define LOG_FF(fmt, args...) /* no-op */
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

/*
 * Extern: controllerFd is defined in gammapad_main.c
 * So that all other files can refer to it for EVIOCRMFF, etc.
 */
extern int controllerFd;

#endif /* GAMMAPAD_H */
