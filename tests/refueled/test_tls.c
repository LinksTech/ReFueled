/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_tls tests (K4 Windows, K5a Linux): the GS-form thread-pointer load
 * `mov rax, gs:[0]` in generated code returns the TCB set with pf_tls_set_guest_base after
 * pf_tls_patch_image, differently in two threads. On Linux gs:[0] is the TCB's self pointer;
 * on Windows the patched load reads the TLS slot, which holds the TCB pointer: both give `tcb`. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform/pf_base.h"
#include "platform/pf_tls.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

static int checks, failures;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++checks;                                                                  \
        if (!(cond)) {                                                             \
            ++failures;                                                            \
            fprintf(stderr, "test_tls: FAILED line %d: %s\n", __LINE__, #cond);    \
        }                                                                          \
    } while (0)

static void *executable(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

/* Code: two GS loads (the second after filler bytes) and ret: 65 48 8b 04 25 00 00 00 00 | 90 90 |
 * 65 48 8b 04 25 00 00 00 00 | c3. Returns the second load's value. */
static const unsigned char code_bytes[] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, 0x90, 0x90,
                                           0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0, 0xc3};
typedef uint64_t (*LoadFn)(void);
static LoadFn load_fn;

typedef struct {
    uint64_t self; /* gs:[0] on Linux */
    uint64_t pad[7];
} Tcb;

static int thread_ok[2];
static Tcb tcbs[2];

static void thread_body(int id) {
    tcbs[id].self = (uint64_t)(uintptr_t)&tcbs[id];
    if (pf_tls_set_guest_base(&tcbs[id]) != 0) return;
    for (int i = 0; i < 100000; ++i) /* the other thread runs meanwhile */
        if (load_fn() != (uint64_t)(uintptr_t)&tcbs[id] || pf_tls_get_guest_base() != &tcbs[id]) return;
    thread_ok[id] = 1;
}

#ifdef _WIN32
static DWORD WINAPI entry(void *argument) {
    thread_body((int)(intptr_t)argument);
    return 0;
}
#else
static void *entry(void *argument) {
    thread_body((int)(intptr_t)argument);
    return NULL;
}
#endif

int main(void) {
    CHECK(pf_tls_init() == 0);
    CHECK(pf_tls_init() == 0); /* idempotent */
    unsigned char *code = executable(4096);
    CHECK(code != NULL);
    if (!code) return 1;
    memcpy(code, code_bytes, sizeof code_bytes);
    PfCodeRange range = {code, sizeof code_bytes};
    CHECK(pf_tls_patch_image(&range, 1) == 2);
#ifdef _WIN32
    CHECK(code[5] != 0 || code[6] != 0); /* displacement rewritten */
    CHECK(code[9] == 0x90 && code[20] == 0xc3);
    CHECK(pf_tls_patch_image(&range, 1) == 0); /* already patched: nothing found again */
#else
    CHECK(memcmp(code, code_bytes, sizeof code_bytes) == 0); /* Linux: counted, unchanged */
#endif
    PfCodeRange short_range = {code, 8};
    CHECK(pf_tls_patch_image(&short_range, 1) == 0);
    CHECK(pf_tls_patch_image(NULL, 0) == 0);
    load_fn = (LoadFn)(void (*)(void))code;
#ifdef _WIN32
    HANDLE threads[2];
    for (int i = 0; i < 2; ++i) threads[i] = CreateThread(NULL, 0, entry, (void *)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(2, threads, TRUE, INFINITE);
    for (int i = 0; i < 2; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i) pthread_create(&threads[i], NULL, entry, (void *)(intptr_t)i);
    for (int i = 0; i < 2; ++i) pthread_join(threads[i], NULL);
#endif
    CHECK(thread_ok[0] && thread_ok[1]);
    if (failures) {
        fprintf(stderr, "test_tls: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_tls: %d checks passed\n", checks);
    return 0;
}
