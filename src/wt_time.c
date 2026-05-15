/*
 * wt_time.c - Millisecond timestamps for room messages and readable output.
 */
#include "wt_time.h"

#include <stdio.h>
#include <time.h>

long long wtNowUnixMilliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return ((long long)now.tv_sec * 1000LL) + ((long long)now.tv_nsec / 1000000LL);
}

void wtFormatUnixMilliseconds(long long unixMs, char *buffer, unsigned long bufferSize) {
    time_t seconds = (time_t)(unixMs / 1000LL);
    struct tm utcTime;
    gmtime_r(&seconds, &utcTime);
    strftime(buffer, (size_t)bufferSize, "%Y-%m-%dT%H:%M:%SZ", &utcTime);
}
