/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: unit test for the low-heap allocator (pf_lowheap.c) and the Linux pf_low_* wrappers. */
#include "platform/pf_lowheap.h"
#include "platform/pf_memory.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static int g_checks;
static int g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

#define HEAP_BYTES ((size_t)16 << 20)

/* The setup insists on a range below 1 TiB, and large malloc blocks come from mmap far above
 * that, so the test area is mapped at a fixed low hint (MAP_FIXED_NOREPLACE, first free slot). */
static unsigned char *make_area(size_t size) {
    for (uintptr_t hint = (uintptr_t)0x300000000ull; hint < ((uintptr_t)1 << 39); hint += (uintptr_t)1 << 30) {
        void *p = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uintptr_t)p == hint) return p;
        if (p != MAP_FAILED) munmap(p, size);
    }
    /* AddressSanitizer keeps a shadow gap there; its low memory is below 2 GiB. */
    for (uintptr_t hint = (uintptr_t)0x20000000; hint < (uintptr_t)0x70000000; hint += (uintptr_t)0x2000000) {
        void *p = mmap((void *)hint, size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED && (uintptr_t)p == hint) return p;
        if (p != MAP_FAILED) munmap(p, size);
    }
    fputs("test_lowheap: cannot map test area below 1 TiB\n", stderr);
    exit(2);
}
static void free_area(void *area, size_t size) { munmap(area, size); }

static void test_setup(void) {
    PfLowHeap h;
    unsigned char *area = make_area(HEAP_BYTES);
    CHECK(pf_lowheap_setup(&h, area, HEAP_BYTES) == 0);
    CHECK(pf_lowheap_setup(&h, area + 16, HEAP_BYTES - 64) == PF_EINVAL); /* not 64-aligned */
    CHECK(pf_lowheap_setup(&h, area, 128) == PF_EINVAL);                   /* too small */
    /* Artificial addresses are only compared, never dereferenced. */
    CHECK(pf_lowheap_setup(&h, (void *)((uintptr_t)1 << 40), 4096) == PF_EINVAL);
    CHECK(pf_lowheap_setup(&h, (void *)(((uintptr_t)1 << 40) - 4096), 8192) == PF_EINVAL);
    CHECK(pf_lowheap_setup(&h, (void *)((uintptr_t)1 << 41), 4096) == PF_EINVAL);
    free_area(area, HEAP_BYTES);
    PfLowHeap zero;
    memset(&zero, 0, sizeof zero);
    CHECK(pf_lowheap_alloc(&zero, 16, 16, 0) == NULL); /* not set up */
    CHECK(pf_lowheap_contains(&zero, &zero) == 0);
}

static void test_alignment_and_zero(void) {
    PfLowHeap h;
    unsigned char *area = make_area(HEAP_BYTES);
    CHECK(pf_lowheap_setup(&h, area, HEAP_BYTES) == 0);
    static const size_t aligns[] = {1, 16, 64, 4096, 65536};
    void *keep[16];
    int n = 0;
    for (size_t i = 0; i < sizeof aligns / sizeof *aligns; i++) {
        size_t a = aligns[i];
        for (size_t size = 1; size <= 3000; size += 701) {
            void *p = pf_lowheap_alloc(&h, a, size, 0);
            CHECK(p != NULL);
            if (!p) continue;
            size_t eff = a < 16 ? 16 : a;
            CHECK(((uintptr_t)p % eff) == 0);
            CHECK(pf_lowheap_contains(&h, p) == 1);
            memset(p, 0xA5, size);
            if (n < 16) keep[n++] = p; else pf_lowheap_free(&h, p);
        }
    }
    CHECK(pf_lowheap_alloc(&h, 3, 16, 0) == NULL);      /* not a power of two */
    CHECK(pf_lowheap_alloc(&h, 131072, 16, 0) == NULL); /* above 64 KiB */
    CHECK(pf_lowheap_alloc(&h, 16, HEAP_BYTES + 1, 0) == NULL);
    for (int i = 0; i < n; i++) pf_lowheap_free(&h, keep[i]);
    pf_lowheap_free(&h, NULL);

    /* zero=1 after reuse of a dirtied block */
    for (int round = 0; round < 3; round++) {
        unsigned char *p = pf_lowheap_alloc(&h, 64, 1000, 1);
        CHECK(p != NULL);
        int clean = 1;
        for (int i = 0; p && i < 1000; i++) if (p[i]) clean = 0;
        CHECK(clean);
        if (p) memset(p, 0xFF, 1000);
        pf_lowheap_free(&h, p);
    }
    CHECK(h.used_bytes == 0);
    free_area(area, HEAP_BYTES);
}

static void test_reuse_and_coalesce(void) {
    PfLowHeap h;
    unsigned char *area = make_area(HEAP_BYTES);
    CHECK(pf_lowheap_setup(&h, area, HEAP_BYTES) == 0);

    void *a = pf_lowheap_alloc(&h, 16, 100, 0);
    pf_lowheap_free(&h, a);
    void *b = pf_lowheap_alloc(&h, 16, 100, 0);
    CHECK(a != NULL && a == b);
    pf_lowheap_free(&h, b);

    enum { COUNT = 4000 };
    static void *ptr[COUNT];
    int got = 0;
    for (int i = 0; i < COUNT; i++) {
        size_t al = (i % 7 == 0) ? 256 : 16;
        ptr[i] = pf_lowheap_alloc(&h, al, 16 + (size_t)(i % 13) * 16, 0);
        if (ptr[i]) got++;
    }
    CHECK(got == COUNT);
    for (int i = 0; i < COUNT; i += 2) pf_lowheap_free(&h, ptr[i]); /* leave holes */
    for (int i = 1; i < COUNT; i += 2) pf_lowheap_free(&h, ptr[i]);
    CHECK(h.used_bytes == 0);
    void *big = pf_lowheap_alloc(&h, 16, HEAP_BYTES - 64, 0);
    CHECK(big != NULL);
    CHECK(pf_lowheap_alloc(&h, 16, 1 << 20, 0) == NULL); /* exhausted */
    pf_lowheap_free(&h, big);
    big = pf_lowheap_alloc(&h, 4096, 1 << 20, 0);        /* possible again */
    CHECK(big != NULL);
    pf_lowheap_free(&h, big);

    /* exhaustion with small blocks, then recovery */
    int count = 0;
    void *first = NULL, *last = NULL;
    for (;;) {
        void *p = pf_lowheap_alloc(&h, 16, 1000, 0);
        if (!p) break;
        *(void **)p = NULL;
        if (!first) first = p; else *(void **)last = p;
        last = p;
        count++;
    }
    CHECK(count > 1000);
    CHECK(pf_lowheap_alloc(&h, 16, 1000, 0) == NULL);
    for (void *p = first; p;) { void *next = *(void **)p; pf_lowheap_free(&h, p); p = next; }
    CHECK(h.used_bytes == 0);
    void *again = pf_lowheap_alloc(&h, 16, 1000, 0);
    CHECK(again != NULL);
    pf_lowheap_free(&h, again);
    free_area(area, HEAP_BYTES);
}

/* ---- threads ------------------------------------------------------------------------------ */

typedef struct { PfLowHeap *heap; unsigned seed; int errors; } Worker;
typedef struct { void *p; size_t size; unsigned char pattern; } Slot;

static void *worker_main(void *arg) {
    Worker *w = arg;
    enum { SLOTS = 64, OPS = 10000 };
    Slot slots[SLOTS];
    memset(slots, 0, sizeof slots);
    unsigned s = w->seed;
#define RND() (s = s * 1103515245u + 12345u, s >> 8)
    for (int op = 0; op < OPS; op++) {
        Slot *sl = &slots[RND() % SLOTS];
        if (sl->p) {
            unsigned char *q = sl->p;
            for (size_t i = 0; i < sl->size; i++) if (q[i] != sl->pattern) { w->errors++; break; }
            pf_lowheap_free(w->heap, sl->p);
            sl->p = NULL;
        } else {
            static const size_t aligns[] = {16, 16, 16, 64, 4096, 65536};
            size_t a = aligns[RND() % 6];
            size_t size = 1 + RND() % 2000;
            void *p = pf_lowheap_alloc(w->heap, a, size, (int)(RND() & 1));
            if (!p) continue; /* transient exhaustion is allowed */
            if (((uintptr_t)p % a) != 0 || !pf_lowheap_contains(w->heap, p)) w->errors++;
            sl->p = p; sl->size = size; sl->pattern = (unsigned char)(RND() | 1);
            memset(p, sl->pattern, size);
        }
    }
    for (int i = 0; i < SLOTS; i++) if (slots[i].p) {
        unsigned char *q = slots[i].p;
        for (size_t k = 0; k < slots[i].size; k++) if (q[k] != slots[i].pattern) { w->errors++; break; }
        pf_lowheap_free(w->heap, slots[i].p);
    }
    return NULL;
}

static void test_threads(void) {
    PfLowHeap h;
    unsigned char *area = make_area(HEAP_BYTES);
    CHECK(pf_lowheap_setup(&h, area, HEAP_BYTES) == 0);
    enum { THREADS = 8 };
    pthread_t th[THREADS];
    Worker w[THREADS];
    for (int i = 0; i < THREADS; i++) {
        w[i] = (Worker){&h, 1234u + (unsigned)i * 77u, 0};
        CHECK(pthread_create(&th[i], NULL, worker_main, &w[i]) == 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].errors == 0);
    }
    CHECK(h.used_bytes == 0);
    void *big = pf_lowheap_alloc(&h, 16, HEAP_BYTES - 64, 0); /* all merged back */
    CHECK(big != NULL);
    pf_lowheap_free(&h, big);
    free_area(area, HEAP_BYTES);
}

static void test_linux_wrappers(void) {
    CHECK(pf_low_heap_init(0, 0) == 0);
    unsigned char *p = pf_low_calloc(4, 32);
    CHECK(p != NULL);
    int clean = 1;
    for (int i = 0; p && i < 128; i++) if (p[i]) clean = 0;
    CHECK(clean);
    pf_low_free(p);
    void *q = pf_low_aligned_alloc(64, 128);
    CHECK(q != NULL && ((uintptr_t)q % 64) == 0);
    pf_low_free(q);
    pf_low_free(NULL);
}

int main(void) {
    test_setup();
    test_alignment_and_zero();
    test_reuse_and_coalesce();
    test_threads();
    test_linux_wrappers();
    if (g_failed) { printf("test_lowheap: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
    printf("test_lowheap: %d checks passed\n", g_checks);
    return 0;
}
