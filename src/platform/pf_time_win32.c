/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Windows time facilities (pf_time.h). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <time.h>
#include "pf_time.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/* The clock the callers' deadlines come from: clock_gettime(CLOCK_MONOTONIC) (winpthreads:
 * QueryPerformanceCounter). Read it the same way so both sides agree. */
static uint64_t monotonic_ns(void) {
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    const uint64_t c = (uint64_t)counter.QuadPart, f = (uint64_t)frequency.QuadPart;
    return c / f * 1000000000u + c % f * 1000000000u / f;
}

void pf_sleep_until_ns(uint64_t deadline_ns) {
    static __declspec(thread) HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS); /* before Windows 10 1803 */
    }
    for (;;) {
        const uint64_t now = monotonic_ns();
        if (now >= deadline_ns) return;
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((deadline_ns - now + 99) / 100); /* relative, 100 ns units */
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            Sleep((DWORD)((deadline_ns - now) / 1000000u)); /* fallback: millisecond sleep */
            continue;
        }
        WaitForSingleObject(timer, INFINITE);
    }
}

int64_t pf_utc_offset_seconds(int64_t utc_seconds, int *is_dst) {
    __time64_t t = (__time64_t)utc_seconds;
    struct tm local;
    if (_localtime64_s(&local, &t)) {
        if (is_dst) *is_dst = 0;
        return 0;
    }
    if (is_dst) *is_dst = local.tm_isdst > 0;
    local.tm_isdst = 0; /* _mkgmtime64 reads the fields as UTC */
    return (int64_t)(_mkgmtime64(&local) - t);
}
