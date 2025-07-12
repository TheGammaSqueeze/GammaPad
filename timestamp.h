#ifndef TIMESTAMP_H
#define TIMESTAMP_H

#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <time.h>

/*
 * log_ts(fmt, …):
 *   Prepends a timestamp "[YYYY-MM-DD HH:MM:SS.mmm] " to stderr,
 *   then vfprintf’s the rest.
 */
static inline void log_ts(const char *fmt, ...)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);

    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);

    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    /* Print timestamp with milliseconds */
    fprintf(stderr, "[%s.%03ld] ",
            tbuf,
            (long)(tv.tv_usec / 1000));

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* 
 * General‐purpose log macro.
 * Use this instead of fprintf(stderr, …).
 */
#define LOG(fmt, ...) log_ts(fmt, ##__VA_ARGS__)

#endif // TIMESTAMP_H
