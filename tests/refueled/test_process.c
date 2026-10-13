/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: tests for pf_process (both platforms). pf_restart_through_launcher is not called.
 * Linux: build with -rdynamic so dladdr reports symbol names. */
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "platform/pf_process.h"

#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif

static int checks;
static int failures;

#define CHECK(cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "test_process: FAILED line %d: %s\n", __LINE__, #cond); \
        } \
    } while (0)

static volatile int sink;

/* Exported (non-static) so that dladdr finds its name on Linux. */
NOINLINE int pf_test_marker(int value) {
    sink = value;
    return sink + 1;
}

static NOINLINE int depth_three(void **frames, int capacity) {
    int count = pf_backtrace(frames, capacity);
    sink = count;
    return count;
}

static NOINLINE int depth_two(void **frames, int capacity) {
    int count = depth_three(frames, capacity);
    sink = count;
    return count;
}

static NOINLINE int depth_one(void **frames, int capacity) {
    int count = depth_two(frames, capacity);
    sink = count;
    return count;
}

int main(void) {
    PfSymbol symbol;
    char line[512];
    uint64_t resident = 0, virtual_size = 0, gpu = 0;
    void *frames[32];
    int local_variable = 0;
    uintptr_t code = (uintptr_t)&pf_test_marker;

    CHECK(pf_test_marker(1) == 2);

#ifdef _WIN32
    CHECK(pf_process_id() == (uint32_t)GetCurrentProcessId());
#else
    CHECK(pf_process_id() == (uint32_t)getpid());
#endif
    CHECK(pf_executable_base() != 0);

    memset(&symbol, 0, sizeof symbol);
    CHECK(pf_symbolize(code, &symbol) == 0);
    CHECK(symbol.module[0] != '\0');
    CHECK(symbol.module_base != 0 && symbol.module_base <= code);
#ifndef _WIN32
    CHECK(strcmp(symbol.symbol, "pf_test_marker") == 0);
    CHECK(symbol.symbol_address == code);
#endif
    CHECK(pf_symbolize(0x10, &symbol) == PF_ENOENT);

    {
        int count = depth_one(frames, 32);
        CHECK(count >= 3);
        /* frames[0] is pf_backtrace itself on both platforms, the caller chain follows. */
        PfSymbol s0;
        CHECK(count >= 1 && pf_symbolize((uintptr_t)frames[0], &s0) == 0);
    }

    CHECK(pf_describe_mapping((uintptr_t)&local_variable, line, sizeof line) == 0);
    {
        const char *perms = strchr(line, ' ');
        CHECK(perms != NULL && perms[2] == 'w');
        CHECK(strchr(line, '-') != NULL);
    }
    CHECK(pf_describe_mapping(code, line, sizeof line) == 0);
    {
        const char *perms = strchr(line, ' ');
        CHECK(perms != NULL && perms[3] == 'x');
    }
    CHECK(pf_describe_mapping(0x10, line, sizeof line) == PF_ENOENT);

    CHECK(pf_process_memory(&resident, &virtual_size) == 0);
    CHECK(resident > 0);
    CHECK(virtual_size > 0);

    /* Linux: 0 or more (no GPU in the test); Windows: PF_ENOSYS until implemented. */
    {
        int r = pf_gpu_memory_in_use(&gpu);
#ifdef _WIN32
        CHECK(r == PF_ENOSYS);
#else
        CHECK(r == 0 || r == PF_ENOENT);
#endif
    }

    if (failures) {
        fprintf(stderr, "test_process: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_process: %d checks passed\n", checks);
    return 0;
}
