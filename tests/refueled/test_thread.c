/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: tests for pf_thread and pf_tls. The Linux part (K5a) is below; K6 adds the Windows
 * part to this file. Cases go through pf_* only, platform helpers sit in #ifdef. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include "platform/pf_exception.h"
#include "platform/pf_thread.h"
#include "platform/pf_tls.h"

static int checks;
static int failures;

#define CHECK(cond) \
    do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            fprintf(stderr, "test_thread: FAILED line %d: %s\n", __LINE__, #cond); \
        } \
    } while (0)

#define NOINLINE __attribute__((noinline))

static void *map_stack(size_t size) {
#ifndef _WIN32
    void *memory = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return memory == MAP_FAILED ? NULL : memory;
#else
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#endif
}

static void sleep_ms(long ms) {
    struct timespec t = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

/* ---- Thread on its own stack with a frame above 64 KiB ------------------------------------- */

typedef struct {
    uint64_t result;
    void *stack_low;
    size_t stack_size;
    int local_inside;
} BigFrame;

/* Touches one byte per page of a 100 KiB local array: 25 pages with the values 1..25 (325),
 * plus the last byte 7. */
static NOINLINE uint64_t use_big_frame(void) {
    volatile unsigned char big[100 * 1024];
    uint64_t sum = 0;
    for (size_t i = 0; i < sizeof big; i += 4096) big[i] = (unsigned char)(i / 4096 + 1);
    big[sizeof big - 1] = 7;
    for (size_t i = 0; i < sizeof big; i += 4096) sum += big[i];
    return sum + big[sizeof big - 1];
}

static void *big_frame_thread(void *argument) {
    BigFrame *frame = argument;
    int local = 0;
    void *base = NULL;
    size_t size = 0;
    frame->result = use_big_frame();
    if (pf_thread_get_stack(&base, &size) == 0) {
        frame->stack_low = base;
        frame->stack_size = size;
        frame->local_inside = (char *)&local >= (char *)base && (char *)&local < (char *)base + size;
    }
    return NULL;
}

/* ---- Guest call on a given stack ----------------------------------------------------------- */

static uintptr_t guest_rsp;

__attribute__((sysv_abi, noinline)) static uint64_t guest_function(uint64_t a, uint64_t b) {
    uintptr_t sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    guest_rsp = sp;
    return a * 1000 + b;
}

/* ---- Names, ids, listing --------------------------------------------------------------------- */

typedef struct {
    atomic_int ready;
    atomic_int stop;
    uint64_t tid;
    char name[32];
    void *tls_set;
    void *tls_got;
} Worker;

static void *named_thread(void *argument) {
    Worker *worker = argument;
    worker->tid = pf_thread_id();
    pf_thread_set_name("pf_test_worker_long_name");
    pf_thread_get_name(worker->name, sizeof worker->name);
    worker->tls_set = (void *)(uintptr_t)0xB0B0;
    pf_tls_set_guest_base(worker->tls_set);
    worker->tls_got = pf_tls_get_guest_base();
    atomic_store(&worker->ready, 1);
    while (!atomic_load(&worker->stop)) sleep_ms(1);
    return NULL;
}

typedef struct {
    uint64_t wanted;
    int found;
    int visited;
    char name[32];
} Listing;

static void list_visit(uint64_t tid, const char *name, void *user) {
    Listing *listing = user;
    listing->visited++;
    if (tid == listing->wanted) {
        listing->found = 1;
        snprintf(listing->name, sizeof listing->name, "%s", name);
    }
}

/* ---- Sampling -------------------------------------------------------------------------------- */

typedef struct {
    atomic_int ready;
    atomic_int stop;
    uint64_t tid;
    uintptr_t stack_low, stack_high;
    volatile uint64_t counter;
} Spinner;

static void *spinner_thread(void *argument) {
    Spinner *spinner = argument;
    void *base = NULL;
    size_t size = 0;
    pf_thread_get_stack(&base, &size);
    spinner->stack_low = (uintptr_t)base;
    spinner->stack_high = (uintptr_t)base + size;
    spinner->tid = pf_thread_id();
    atomic_store(&spinner->ready, 1);
    while (!atomic_load(&spinner->stop)) spinner->counter++;
    return NULL;
}

typedef struct {
    uint64_t rip, rsp, tid;
    volatile int calls;
} Sampled;

/* pf_thread_sample is asynchronous on Linux (SIGPROF, IF-10) and synchronous on Windows. */
static int wait_calls(const Sampled *sampled, int calls) {
    for (int i = 0; i < 2000 && sampled->calls < calls; ++i) sleep_ms(1);
    return sampled->calls == calls;
}

static void sample_callback(const PfContext *context, void *user) {
    Sampled *sampled = user;
    sampled->rip = pf_context_get(context, PF_REG_RIP);
    sampled->rsp = pf_context_get(context, PF_REG_RSP);
    sampled->tid = pf_thread_id();
    sampled->calls++;
}

typedef struct {
    uint64_t wanted;
    volatile int calls, seen;
} Dumped;

static void dump_callback(const PfContext *context, uint64_t tid, void *user) {
    Dumped *dumped = user;
    dumped->calls++;
    if (tid == dumped->wanted && pf_context_get(context, PF_REG_RIP) != 0) dumped->seen = 1;
}

static void *tls_other_thread(void *argument) {
    void **result = argument;
    pf_tls_set_guest_base((void *)(uintptr_t)0xD00D);
    result[0] = pf_tls_get_guest_base();
    return NULL;
}

int main(void) {
    CHECK(pf_tls_init() == 0); /* pf_init does this in the loader; Windows needs the slot first */
    /* Thread on its own 1 MiB stack, frame above 64 KiB. */
    {
        size_t size = 1024 * 1024;
        void *stack = map_stack(size);
        BigFrame frame;
        pthread_t thread;
        memset(&frame, 0, sizeof frame);
        CHECK(stack != NULL);
        if (stack) {
            CHECK(pf_thread_create_on_stack(&thread, big_frame_thread, &frame, stack, size) == 0);
            pthread_join(thread, NULL);
            CHECK(frame.result == 332);
            CHECK(frame.stack_low == stack);
            CHECK(frame.stack_size == size);
            CHECK(frame.local_inside);
        }
        /* stack NULL: ordinary thread with the given stack size */
        memset(&frame, 0, sizeof frame);
        CHECK(pf_thread_create_on_stack(&thread, big_frame_thread, &frame, NULL, 512 * 1024) == 0);
        pthread_join(thread, NULL);
        CHECK(frame.result == 332 && frame.local_inside);
    }

    /* pf_call_guest_on_stack */
    {
        size_t size = 256 * 1024;
        unsigned char *stack = map_stack(size);
        CHECK(stack != NULL);
        if (stack) {
            uint64_t result;
            guest_rsp = 0;
            result = pf_call_guest_on_stack((const void *)guest_function, 7, 9, stack, stack + size - 8);
            CHECK(result == 7009);
            CHECK(guest_rsp > (uintptr_t)stack && guest_rsp < (uintptr_t)stack + size);
        }
    }

    /* Own stack */
    {
        int local = 0;
        void *base = NULL;
        size_t size = 0;
        CHECK(pf_thread_get_stack(&base, &size) == 0);
        CHECK((char *)&local >= (char *)base && (char *)&local < (char *)base + size);
    }

    /* Names, ids, listing, TLS in a second thread */
    {
        Worker worker;
        Listing listing;
        pthread_t thread;
        char name[32];
        memset(&worker, 0, sizeof worker);
        memset(&listing, 0, sizeof listing);
        atomic_init(&worker.ready, 0);
        atomic_init(&worker.stop, 0);
        pthread_create(&thread, NULL, named_thread, &worker);
        while (!atomic_load(&worker.ready)) sleep_ms(1);

        CHECK(pf_thread_id() != 0);
        CHECK(worker.tid != 0 && worker.tid != pf_thread_id());
#ifdef _WIN32
        CHECK(strcmp(worker.name, "pf_test_worker_long_name") == 0); /* Windows keeps the whole name */
#else
        CHECK(strcmp(worker.name, "pf_test_worker_") == 0); /* cut to 15 bytes */
#endif

        CHECK(pf_thread_set_name("main_pf_test") == 0);
        CHECK(pf_thread_get_name(name, sizeof name) == 0 && strcmp(name, "main_pf_test") == 0);

        listing.wanted = worker.tid;
        CHECK(pf_threads_list(list_visit, &listing) >= 2);
        CHECK(listing.found);
        CHECK(strcmp(listing.name, worker.name) == 0);

        /* TLS: the worker's value is its own, the main thread's another */
        CHECK(worker.tls_got == worker.tls_set);
        CHECK(pf_tls_set_guest_base((void *)(uintptr_t)0xA0A0) == 0);
        CHECK(pf_tls_get_guest_base() == (void *)(uintptr_t)0xA0A0);
        {
            void *other[1] = {NULL};
            pthread_t t2;
            pthread_create(&t2, NULL, tls_other_thread, other);
            pthread_join(t2, NULL);
            CHECK(other[0] == (void *)(uintptr_t)0xD00D);
            CHECK(pf_tls_get_guest_base() == (void *)(uintptr_t)0xA0A0);
        }
        CHECK(pf_tls_init() == 0);

        atomic_store(&worker.stop, 1);
        pthread_join(thread, NULL);
    }

    /* CPU count and time */
    {
        uint64_t before = 0, after = 0;
        volatile uint64_t sink = 0;
        CHECK(pf_cpu_count_allowed() >= 1);
        CHECK(pf_thread_cpu_time_ns(&before) == 0);
        /* Windows counts thread time in scheduler ticks (~15.6 ms): spin until it moves, at most
         * about 2 s of work. */
        for (int round = 0; round < 40 && after <= before; ++round) {
            for (uint64_t i = 0; i < 50000000u; i++) sink += i;
            CHECK(pf_thread_cpu_time_ns(&after) == 0);
        }
        CHECK(after > before);
    }

    /* Sampling a spinning thread */
    {
        Spinner spinner;
        Sampled sampled;
        pthread_t thread;
        memset(&spinner, 0, sizeof spinner);
        memset(&sampled, 0, sizeof sampled);
        atomic_init(&spinner.ready, 0);
        atomic_init(&spinner.stop, 0);
        pthread_create(&thread, NULL, spinner_thread, &spinner);
        while (!atomic_load(&spinner.ready)) sleep_ms(1);
        CHECK(pf_thread_sample(spinner.tid, sample_callback, &sampled) == 0);
        CHECK(wait_calls(&sampled, 1));
        CHECK(sampled.rip != 0);
        CHECK(sampled.rsp >= spinner.stack_low && sampled.rsp < spinner.stack_high);
#ifndef _WIN32
        CHECK(sampled.tid == spinner.tid); /* Linux: the callback ran on the target thread */
#else
        CHECK(sampled.tid == pf_thread_id()); /* Windows: on the sampling thread */
#endif
        CHECK(pf_thread_sample(spinner.tid, sample_callback, &sampled) == 0 && wait_calls(&sampled, 2));
        CHECK(pf_thread_sample(0x7ffffff0u, sample_callback, &sampled) == PF_ENOENT);
        /* Thread dump on request (IF-10): every other thread once, the spinner among them. */
        {
            static Dumped dumped;
            dumped.wanted = spinner.tid;
            CHECK(pf_dump_other_threads() == PF_EINVAL); /* nothing installed yet */
            CHECK(pf_dump_signal_install(dump_callback, &dumped) == 0);
            CHECK(pf_dump_other_threads() == 0);
            CHECK(dumped.seen == 1 && dumped.calls >= 1);
        }
        atomic_store(&spinner.stop, 1);
        pthread_join(thread, NULL);
    }

    /* pf_tls_patch_image counts the GS-form loads */
    {
        static unsigned char code[64];
        static const unsigned char load[9] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
        PfCodeRange range;
        memset(code, 0x90, sizeof code);
        memcpy(code + 5, load, sizeof load);
        memcpy(code + 40, load, sizeof load);
        range.start = code;
        range.size = sizeof code;
        CHECK(pf_tls_patch_image(&range, 1) == 2);
#ifndef _WIN32
        CHECK(memcmp(code + 5, load, sizeof load) == 0); /* Linux changes nothing */
#else
        CHECK(memcmp(code + 5, load, 5) == 0 && memcmp(code + 5, load, sizeof load) != 0); /* slot displacement */
#endif
    }

    /* Idle priority last: it lowers this thread */
    CHECK(pf_thread_set_idle_priority() == 0);

    if (failures) {
        fprintf(stderr, "test_thread: %d of %d checks FAILED\n", failures, checks);
        return 1;
    }
    printf("test_thread: %d checks passed\n", checks);
    return 0;
}
