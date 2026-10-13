/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_memory tests (K1 Windows, K2 Linux): layout reservation, the first low allocation
 * at 0x800000000, fixed allocations and partial frees, the backing object viewed several times
 * (aliasing), replacing and unmapping parts of views at 16 KiB granularity with their contents and
 * page protections kept, guest reservations, zeroing, fault-free reads and concurrent mapping.
 * Platform helpers (protection query, threads) are the only #ifdef code. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "platform/pf_memory.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <stdlib.h>
#endif

#define KIB UINT64_C(1024)
#define MIB (1024 * KIB)
#define GIB (1024 * MIB)
#define LOW_MIN UINT64_C(0x0800000000)
#define USER_MIN UINT64_C(0x1000000000)
#define USER_MAX UINT64_C(0xfc00000000)
#define PAGE (16 * KIB) /* PS4 page */

static int checks, failures;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++checks;                                                                      \
        if (!(cond)) {                                                                 \
            ++failures;                                                                \
            fprintf(stderr, "test_memory: FAILED line %d: %s\n", __LINE__, #cond);     \
        }                                                                              \
    } while (0)

/* PF_PROT_* of the page at `address`, -1 when nothing accessible is mapped there. */
static int query_prot(uintptr_t address) {
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)address, &info, sizeof info) || info.State != MEM_COMMIT) return -1;
    switch (info.Protect & 0xff) {
    case PAGE_NOACCESS: return PF_PROT_NONE;
    case PAGE_READONLY: return PF_PROT_READ;
    case PAGE_READWRITE: return PF_PROT_READ | PF_PROT_WRITE;
    case PAGE_EXECUTE: return PF_PROT_EXEC;
    case PAGE_EXECUTE_READ: return PF_PROT_READ | PF_PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PF_PROT_READ | PF_PROT_WRITE | PF_PROT_EXEC;
    default: return -2;
    }
#else
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    int result = -1;
    while (maps && fgets(line, sizeof line, maps)) {
        unsigned long long start, end;
        char perms[5];
        if (sscanf(line, "%llx-%llx %4s", &start, &end, perms) == 3 && address >= start && address < end) {
            result = (perms[0] == 'r' ? PF_PROT_READ : 0) | (perms[1] == 'w' ? PF_PROT_WRITE : 0) |
                     (perms[2] == 'x' ? PF_PROT_EXEC : 0);
            break;
        }
    }
    if (maps) fclose(maps);
    return result;
#endif
}

static uint32_t read_u32(uintptr_t address) {
    uint32_t value = 0xdeadbeef;
    if (pf_read_memory_safe(&value, address, sizeof value) != sizeof value) return 0xdeadbeef;
    return value;
}

static int readable(uintptr_t address) {
    unsigned char byte;
    return pf_read_memory_safe(&byte, address, 1) == 1;
}

/* Tag of a backing page: page number + 1 in its first word. */
static void tag_backing(unsigned char *view, uint64_t offset, uint64_t size) {
    for (uint64_t at = offset; at < offset + size; at += PAGE) *(uint32_t *)(void *)(view + at) = (uint32_t)(at / PAGE) + 1;
}
static uint32_t tag_of(uint64_t offset) { return (uint32_t)(offset / PAGE) + 1; }

static void test_layout_and_low(void) {
    CHECK(pf_page_size() == 4096);
    CHECK(pf_vm_reserve_layout(LOW_MIN, USER_MIN - LOW_MIN) == 0);
    CHECK(pf_vm_reserve_layout(USER_MIN, USER_MAX - USER_MIN) == 0);
#ifdef _WIN32
    CHECK(pf_vm_reserve_layout(LOW_MIN, 64 * KIB) == PF_EEXIST);
    CHECK(query_prot(LOW_MIN) == -1); /* a placeholder, not memory */
#endif
    /* The image: first low allocation, exactly at LOW_MIN (U1 §1). */
    const uintptr_t image = LOW_MIN;
    CHECK(pf_vm_alloc_fixed(image, MIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(image < (UINT64_C(1) << 40));
    memset((void *)image, 0x5a, MIB);
    CHECK(((unsigned char *)image)[MIB - 1] == 0x5a);
    CHECK(pf_vm_alloc_fixed(image, PAGE, PF_PROT_READ) == PF_EEXIST);
    CHECK(pf_vm_alloc_fixed(image + MIB - PAGE, 2 * PAGE, PF_PROT_READ) == PF_EEXIST); /* overlaps the end */
    CHECK(pf_vm_alloc_fixed(image + MIB + PAGE, PAGE, PF_PROT_READ | PF_PROT_WRITE) == 0); /* after a gap */
    CHECK(pf_vm_alloc_fixed(image + 1, PAGE, PF_PROT_READ) == PF_EINVAL);
    /* Protection of a part, then back. */
    CHECK(pf_vm_protect((void *)image, 64 * KIB, PF_PROT_READ | PF_PROT_EXEC) == 0);
    CHECK(query_prot(image) == (PF_PROT_READ | PF_PROT_EXEC));
    CHECK(query_prot(image + 64 * KIB) == (PF_PROT_READ | PF_PROT_WRITE));
    CHECK(pf_vm_protect((void *)image, 64 * KIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    /* Free a 16 KiB page in the middle: neighbours keep their contents, the hole is free again. */
    CHECK(pf_vm_protect((void *)(image + 128 * KIB), PAGE, PF_PROT_READ) == 0);
    CHECK(pf_vm_free((void *)(image + 256 * KIB), PAGE) == 0);
    CHECK(!readable(image + 256 * KIB));
    CHECK(((unsigned char *)image)[256 * KIB - 1] == 0x5a && ((unsigned char *)image)[256 * KIB + PAGE] == 0x5a);
    CHECK(query_prot(image + 128 * KIB) == PF_PROT_READ); /* kept across the split */
    CHECK(query_prot(image + 128 * KIB + PAGE) == (PF_PROT_READ | PF_PROT_WRITE));
    CHECK(pf_vm_alloc_fixed(image + 256 * KIB, PAGE, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(((unsigned char *)image)[256 * KIB] == 0);
    /* Fault-free read across a hole: stops at the hole. */
    CHECK(pf_vm_free((void *)(image + 512 * KIB), PAGE) == 0);
    unsigned char buffer[2 * 16384];
    CHECK(pf_read_memory_safe(buffer, image + 512 * KIB - PAGE, sizeof buffer) == (int64_t)PAGE);
    /* Freeing the whole range returns it to the reservation: a new allocation is zero. */
    CHECK(pf_vm_free((void *)image, MIB) == 0);
    CHECK(!readable(image) && !readable(image + 300 * KIB));
    CHECK(pf_vm_alloc_fixed(image, MIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(read_u32(image) == 0 && read_u32(image + MIB - 4) == 0);
}

static PfBacking *backing;

static void test_backing(void) {
    CHECK(pf_backing_create(64 * MIB, &backing) == 0);
    unsigned char *view = pf_backing_view(backing);
    CHECK(view != NULL);
    if (!view) return;
    tag_backing(view, 0, 8 * MIB);
    const uintptr_t a = USER_MIN, b = USER_MIN + 16 * MIB;
    CHECK(pf_backing_map_fixed(backing, 0, a, MIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(pf_backing_map_fixed(backing, 0, b, MIB, PF_PROT_READ) == 0);
    CHECK(read_u32(a) == tag_of(0) && read_u32(a + 5 * PAGE) == tag_of(5 * PAGE));
    *(volatile uint32_t *)(a + 3 * PAGE + 8) = 0x12345678; /* aliasing: guest A -> guest B and view */
    CHECK(read_u32(b + 3 * PAGE + 8) == 0x12345678 && *(uint32_t *)(void *)(view + 3 * PAGE + 8) == 0x12345678);
    CHECK(query_prot(b) == PF_PROT_READ);
    /* Replace the middle of A with other backing pages; protect a page left of it first. */
    CHECK(pf_vm_protect((void *)(a + 2 * PAGE), PAGE, PF_PROT_READ) == 0);
    CHECK(pf_vm_protect((void *)(a + 40 * PAGE), PAGE, PF_PROT_NONE) == 0);
    CHECK(pf_backing_map_fixed(backing, 2 * MIB, a + 16 * PAGE, 16 * PAGE, PF_PROT_READ | PF_PROT_WRITE) == 0);
    int middle_ok = 1, edges_ok = 1;
    for (int i = 16; i < 32; ++i) middle_ok &= read_u32(a + (uint64_t)i * PAGE) == tag_of(2 * MIB + (uint64_t)(i - 16) * PAGE);
    for (int i = 0; i < 64; ++i)
        if ((i < 16 || i >= 32) && i != 40) edges_ok &= read_u32(a + (uint64_t)i * PAGE) == tag_of((uint64_t)i * PAGE);
    CHECK(middle_ok);
    CHECK(edges_ok);
    CHECK(query_prot(a + 2 * PAGE) == PF_PROT_READ);                          /* kept, left piece */
    CHECK(query_prot(a + 40 * PAGE) == PF_PROT_NONE && !readable(a + 40 * PAGE)); /* kept, right piece */
    CHECK(query_prot(a + 3 * PAGE) == (PF_PROT_READ | PF_PROT_WRITE));
    CHECK(pf_vm_protect((void *)(a + 40 * PAGE), PAGE, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(read_u32(a + 40 * PAGE) == tag_of(40 * PAGE));
    /* Protection across several views in one call (A's three pieces), then executable. */
    CHECK(pf_vm_protect((void *)a, MIB, PF_PROT_READ | PF_PROT_EXEC) == 0);
    CHECK(query_prot(a) == (PF_PROT_READ | PF_PROT_EXEC) && query_prot(a + 20 * PAGE) == (PF_PROT_READ | PF_PROT_EXEC) &&
          query_prot(a + MIB - PAGE) == (PF_PROT_READ | PF_PROT_EXEC));
    CHECK(pf_vm_protect((void *)a, MIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    /* Unmap one 16 KiB page, then map another backing page into the hole. */
    CHECK(pf_guest_unmap(a + 32 * PAGE, PAGE) == 0);
    CHECK(!readable(a + 32 * PAGE) && read_u32(a + 31 * PAGE) == tag_of(2 * MIB + 15 * PAGE) &&
          read_u32(a + 33 * PAGE) == tag_of(33 * PAGE));
    CHECK(pf_backing_map_fixed(backing, 3 * MIB, a + 32 * PAGE, PAGE, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(read_u32(a + 32 * PAGE) == tag_of(3 * MIB));
    /* A mapping spanning several earlier views and a hole. */
    CHECK(pf_guest_unmap(a + 48 * PAGE, 2 * PAGE) == 0);
    CHECK(pf_backing_map_fixed(backing, 4 * MIB, a + 24 * PAGE, 32 * PAGE, PF_PROT_READ) == 0);
    int span_ok = 1;
    for (int i = 0; i < 32; ++i) span_ok &= read_u32(a + (uint64_t)(24 + i) * PAGE) == tag_of(4 * MIB + (uint64_t)i * PAGE);
    CHECK(span_ok);
    CHECK(read_u32(a + 23 * PAGE) == tag_of(2 * MIB + 7 * PAGE) && read_u32(a + 56 * PAGE) == tag_of(56 * PAGE));
    /* Zeroing is visible through every view of the range. */
    CHECK(pf_backing_zero(backing, 0, PAGE) == 0);
    CHECK(read_u32(a) == 0 && read_u32(b) == 0 && view[100] == 0);
    CHECK(pf_guest_unmap(a, MIB) == 0 && pf_guest_unmap(b, MIB) == 0);
    CHECK(!readable(a) && !readable(a + MIB - 1) && !readable(b));
}

static void test_reserve(void) {
    const uintptr_t r = USER_MIN + 64 * MIB;
    CHECK(pf_guest_reserve(r, MIB) == 0);
    CHECK(!readable(r));
    CHECK(pf_vm_alloc_fixed(r + PAGE, PAGE, PF_PROT_READ) == PF_EEXIST);
    CHECK(pf_backing_map_fixed(backing, 5 * MIB, r + 4 * PAGE, 2 * PAGE, PF_PROT_READ) == 0); /* over a reservation */
    CHECK(read_u32(r + 4 * PAGE) == tag_of(5 * MIB) && !readable(r + 6 * PAGE));
    CHECK(pf_guest_unmap(r, MIB) == 0);
    CHECK(pf_vm_alloc_fixed(r, MIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(read_u32(r + 4 * PAGE) == 0);
    CHECK(pf_vm_free((void *)r, MIB) == 0);
}

static void test_anywhere(void) {
    unsigned char *p = pf_vm_alloc(64 * KIB, PF_PROT_READ | PF_PROT_WRITE);
    CHECK(p != NULL);
    if (p) {
        p[0] = 1;
        p[64 * KIB - 1] = 2;
        CHECK(pf_vm_free(p, 64 * KIB) == 0);
    }
    unsigned char *lazy = pf_vm_alloc_lazy(32 * MIB);
    CHECK(lazy != NULL);
    if (lazy) {
        CHECK(lazy[31 * MIB] == 0);
        lazy[31 * MIB] = 7;
        CHECK(pf_vm_free(lazy, 32 * MIB) == 0);
    }
    /* Below the layout, 16 MiB under the image (gnm_hooks.cpp MapNear). */
    const uintptr_t near_image = LOW_MIN - 16 * MIB;
    CHECK(pf_vm_alloc_fixed(near_image, 64 * KIB, PF_PROT_READ | PF_PROT_WRITE) == 0);
    CHECK(read_u32(near_image) == 0);
    CHECK(pf_vm_alloc_fixed(near_image, 64 * KIB, PF_PROT_READ | PF_PROT_WRITE) == PF_EEXIST);
    CHECK(pf_vm_free((void *)near_image, 64 * KIB) == 0);
    CHECK(pf_vm_alloc_fixed(USER_MIN - PAGE, 2 * PAGE, PF_PROT_READ) == PF_EINVAL); /* crosses low/user */
}

/* ---- Concurrent mapping: each thread owns a 64 MiB window and its own backing range ---------- */

#define THREADS 4
#define ROUNDS 300
static int thread_errors[THREADS];

static void worker(int id) {
    const uintptr_t window = USER_MIN + GIB + (uint64_t)id * 64 * MIB;
    const uint64_t source = 8 * MIB + (uint64_t)id * 8 * MIB; /* 8 MiB of tagged backing per thread */
    uint32_t seed = 0x9e3779b9u * (uint32_t)(id + 1);
    for (int round = 0; round < ROUNDS; ++round) {
        seed = seed * 1664525u + 1013904223u;
        const uint64_t page = (seed >> 8) % 256, count = 1 + (seed >> 20) % 64, from = (seed >> 4) % 256;
        const uintptr_t at = window + page * PAGE;
        const uint64_t offset = source + (from % (512 - count)) * PAGE;
        if (pf_backing_map_fixed(backing, offset, at, count * PAGE, PF_PROT_READ | PF_PROT_WRITE)) ++thread_errors[id];
        if (read_u32(at) != tag_of(offset) || read_u32(at + (count - 1) * PAGE) != tag_of(offset + (count - 1) * PAGE))
            ++thread_errors[id];
        if ((seed & 3) == 0 && pf_guest_unmap(at, count * PAGE)) ++thread_errors[id];
    }
    if (pf_guest_unmap(window, 64 * MIB)) ++thread_errors[id];
}

#ifdef _WIN32
static DWORD WINAPI worker_entry(void *argument) {
    worker((int)(intptr_t)argument);
    return 0;
}
#else
static void *worker_entry(void *argument) {
    worker((int)(intptr_t)argument);
    return NULL;
}
#endif

static void test_threads(void) {
    tag_backing(pf_backing_view(backing), 8 * MIB, THREADS * 8 * MIB);
#ifdef _WIN32
    HANDLE threads[THREADS];
    for (int i = 0; i < THREADS; ++i) threads[i] = CreateThread(NULL, 0, worker_entry, (void *)(intptr_t)i, 0, NULL);
    WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);
    for (int i = 0; i < THREADS; ++i) CloseHandle(threads[i]);
#else
    pthread_t threads[THREADS];
    for (int i = 0; i < THREADS; ++i) pthread_create(&threads[i], NULL, worker_entry, (void *)(intptr_t)i);
    for (int i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
#endif
    for (int i = 0; i < THREADS; ++i) CHECK(thread_errors[i] == 0);
}

int main(void) {
    test_layout_and_low();
    test_backing();
    if (backing) {
        test_reserve();
        test_threads();
    }
    test_anywhere();
    if (failures) {
        fprintf(stderr, "test_memory: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("test_memory: %d checks passed\n", checks);
    return 0;
}
