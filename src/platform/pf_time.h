/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: time facilities without a portable POSIX form. clock_gettime(CLOCK_MONOTONIC |
 * CLOCK_REALTIME), nanosleep and time() stay plain POSIX in the common code (INTERFACES.md §2). */
#ifndef REFUELED_PF_TIME_H
#define REFUELED_PF_TIME_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* Sleeps until clock_gettime(CLOCK_MONOTONIC) reaches `deadline_ns` (absolute, nanoseconds);
 * returns at once when it has passed. Linux: clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME),
 * repeated after interruptions (moved from runtime_audio.c); Windows: a high-resolution waitable
 * timer, re-armed until the deadline is reached. */
void pf_sleep_until_ns(uint64_t deadline_ns);

/* Offset of local time from UTC in seconds (east positive) at the UTC time `utc_seconds`, and
 * whether daylight saving time applies (is_dst may be NULL). Linux: localtime_r's tm_gmtoff and
 * tm_isdst > 0, as the runtime read them; Windows: _localtime64_s and _mkgmtime64. */
int64_t pf_utc_offset_seconds(int64_t utc_seconds, int *is_dst);

PF_EXTERN_C_END
#endif
