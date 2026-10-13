/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stubs, K6 replaces them. */
#include "pf_thread.h"

int pf_thread_create_on_stack(pthread_t *out, void *(*start)(void *), void *argument, void *stack, size_t size) {
    (void)out;
    (void)start;
    (void)argument;
    (void)stack;
    (void)size;
    return pf_not_implemented(__func__);
}

uint64_t pf_call_guest_on_stack(const void *entry, uint64_t arg0, uint64_t arg1, void *stack_low, void *stack_top) {
    (void)entry;
    (void)arg0;
    (void)arg1;
    (void)stack_low;
    (void)stack_top;
    pf_not_implemented(__func__);
    return 0;
}

int pf_thread_get_stack(void **base, size_t *size) {
    (void)base;
    (void)size;
    return pf_not_implemented(__func__);
}

uint64_t pf_thread_id(void) {
    pf_not_implemented(__func__);
    return 0;
}

int pf_thread_set_name(const char *name) {
    (void)name;
    return pf_not_implemented(__func__);
}

int pf_thread_get_name(char *buffer, size_t size) {
    (void)buffer;
    (void)size;
    return pf_not_implemented(__func__);
}

int pf_thread_set_idle_priority(void) { return pf_not_implemented(__func__); }

int pf_cpu_count_allowed(void) { return pf_not_implemented(__func__); }

int pf_thread_cpu_time_ns(uint64_t *nanoseconds) {
    (void)nanoseconds;
    return pf_not_implemented(__func__);
}

int pf_threads_list(void (*visit)(uint64_t tid, const char *name, void *user), void *user) {
    (void)visit;
    (void)user;
    return pf_not_implemented(__func__);
}

int pf_thread_sample(uint64_t tid, void (*sample)(const PfContext *context, void *user), void *user) {
    (void)tid;
    (void)sample;
    (void)user;
    return pf_not_implemented(__func__);
}
