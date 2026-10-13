/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: tests for pf_time.h (pf_sleep_until_ns, pf_utc_offset_seconds) and pf_fs_stamp. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "platform/pf_fs.h"
#include "platform/pf_time.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int checks, failures;
#define CHECK(cond)                                                              \
    do {                                                                         \
        ++checks;                                                                \
        if (!(cond)) {                                                           \
            ++failures;                                                          \
            fprintf(stderr, "test_time: FAILED line %d: %s\n", __LINE__, #cond); \
        }                                                                        \
    } while (0)

/* The callers' clock: clock_gettime(CLOCK_MONOTONIC) (Windows: QueryPerformanceCounter, which
 * winpthreads uses for it; read directly here so MSVC can build the test too). */
static unsigned long long now_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (unsigned long long)c.QuadPart / (unsigned long long)f.QuadPart * 1000000000ull +
           (unsigned long long)c.QuadPart % (unsigned long long)f.QuadPart * 1000000000ull / (unsigned long long)f.QuadPart;
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
#endif
}

int main(void) {
    /* Sleep until a deadline: never early, and close (Windows high-resolution timer). */
    unsigned long long worst = 0;
    for (int i = 0; i < 20; ++i) {
        const unsigned long long deadline = now_ns() + 2000000ull; /* 2 ms */
        pf_sleep_until_ns(deadline);
        const unsigned long long late = now_ns() - deadline;
        CHECK(now_ns() >= deadline);
        if (late > worst) worst = late;
    }
    CHECK(worst < 20000000ull); /* < 20 ms late in the worst of 20 */
    pf_sleep_until_ns(now_ns() - 1000); /* in the past: returns at once */
    printf("test_time: worst lateness %.3f ms\n", (double)worst / 1e6);

    /* UTC offset: whole quarter hours within +-14 h. */
    int dst = -1;
    const long long offset = (long long)pf_utc_offset_seconds((long long)time(NULL), &dst);
    CHECK(offset % 900 == 0 && offset >= -14 * 3600 && offset <= 14 * 3600);
    CHECK(dst == 0 || dst == 1);
    CHECK(pf_utc_offset_seconds(0, NULL) % 900 == 0);

    /* File stamp: size and a later modification time after rewriting. */
    const char *path = "pf_test_time_stamp.tmp";
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("abc", f);
        fclose(f);
    }
    long long m1 = 0, m2 = 0;
    unsigned long long s1 = 0, s2 = 0;
    CHECK(pf_fs_stamp(path, (int64_t *)&m1, (uint64_t *)&s1) == 0 && s1 == 3 && m1 > 0);
    pf_sleep_until_ns(now_ns() + 30000000ull);
    f = fopen(path, "wb");
    if (f) {
        fputs("abcdef", f);
        fclose(f);
    }
    CHECK(pf_fs_stamp(path, (int64_t *)&m2, (uint64_t *)&s2) == 0 && s2 == 6 && m2 > m1);
    CHECK(pf_fs_stamp("pf_test_time_missing.tmp", NULL, NULL) == PF_ENOENT);
    remove(path);

    if (failures) {
        fprintf(stderr, "test_time: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_time: %d checks passed\n", checks);
    return 0;
}
