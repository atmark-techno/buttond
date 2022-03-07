// SPDX-License-Identifier: MIT

#ifndef BUTTOND_TIME_H
#define BUTTOND_TIME_H

#include <time.h>

#include "utils.h"

#define NSECS_IN_SEC  1000000000L
#define NSECS_IN_MSEC 1000000L
#define NSECS_IN_USEC 1000L
#define USECS_IN_SEC  1000000L
#define USECS_IN_MSEC 1000L

/* time difference in msecs */
static inline int64_t time_diff_ts(struct timespec *ts1, struct timespec *ts2) {
	return (ts1->tv_nsec - ts2->tv_nsec) / NSECS_IN_MSEC
		+ (ts1->tv_sec - ts2->tv_sec) * 1000;
}

static inline int64_t time_diff_tv(struct timeval *tv1, struct timeval *tv2) {
	return (tv1->tv_usec - tv2->tv_usec) / USECS_IN_MSEC
		+ (tv1->tv_sec - tv2->tv_sec) * 1000;
}

/* add number of msec to given timespec */
static inline void time_add_ts(struct timespec *ts, int msec) {
	ts->tv_nsec += msec * NSECS_IN_MSEC;
	if (ts->tv_nsec >= NSECS_IN_SEC) {
		ts->tv_sec += ts->tv_nsec / NSECS_IN_SEC;
		ts->tv_nsec %= NSECS_IN_SEC;
	}
}

/* convert timeval to timespec and add offset msec */
static inline void time_tv2ts(struct timespec *ts, struct timeval *base, int msec) {
	ts->tv_nsec = base->tv_usec * NSECS_IN_USEC + msec * NSECS_IN_MSEC;
	ts->tv_sec = base->tv_sec + ts->tv_nsec / NSECS_IN_SEC;
	ts->tv_nsec %= NSECS_IN_SEC;
}
/* convert timespec to timeval and add offset msec */
static inline void time_ts2tv(struct timeval *tv, struct timespec *base, int msec) {
	tv->tv_usec = base->tv_nsec / NSECS_IN_USEC + msec * USECS_IN_MSEC;
	tv->tv_sec = base->tv_sec + tv->tv_usec / USECS_IN_SEC;
	tv->tv_usec %= USECS_IN_SEC;
}

static inline void time_gettime(struct timespec *ts) {
	int rc = clock_gettime(CLOCK_MONOTONIC, ts);
	xassert(rc == 0, "Could not get time: %m");
}

#endif
