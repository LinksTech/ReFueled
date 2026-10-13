/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux time facilities (pf_time.h); moved from runtime_audio.c (sleep_until) and the
 * tm_gmtoff readers in runtime_kernel.c/runtime_rtc.c/runtime_services.c. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include <time.h>
#include "pf_time.h"

void pf_sleep_until_ns(uint64_t deadline_ns) {
    struct timespec t={(time_t)(deadline_ns/1000000000u),(long)(deadline_ns%1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&t,NULL)) {}
}

int64_t pf_utc_offset_seconds(int64_t utc_seconds, int *is_dst) {
    time_t t = (time_t)utc_seconds;
    struct tm local;
    localtime_r(&t, &local);
    if (is_dst) *is_dst = local.tm_isdst > 0;
    return (int64_t)local.tm_gmtoff;
}
