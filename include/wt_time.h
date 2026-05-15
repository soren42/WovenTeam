/*
 * wt_time.h - Time helpers used by the native Phase 0 tools.
 */
#ifndef WT_TIME_H
#define WT_TIME_H

long long wtNowUnixMilliseconds(void);
void wtFormatUnixMilliseconds(long long unixMs, char *buffer, unsigned long bufferSize);

#endif
