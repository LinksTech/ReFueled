/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: host threads beyond the POSIX subset.
 *
 * Mutexes, condition variables, rwlocks, once, keys, create/join/detach stay pthread calls in
 * the common code (winpthreads on Windows; the allowed subset is in docs/refueled/INTERFACES.md).
 * This header covers what pthreads cannot express portably: running on caller-provided stacks
 * (guest stacks below 1 TiB), thread names and ids, sampling another thread's registers, listing
 * the process's threads, idle priority and CPU counts. */
#ifndef REFUELED_PF_THREAD_H
#define REFUELED_PF_THREAD_H
#include <pthread.h>
#include "pf_base.h"
#include "pf_exception.h"

PF_EXTERN_C_BEGIN

/* ---- Stacks ------------------------------------------------------------------------------ */

/* pthread_create whose thread runs `start(argument)` on [stack, stack+size) (caller-owned,
 * read/write, e.g. from runtime_low_map). Linux: pthread_attr_setstack, unchanged. Windows: the
 * thread starts on a small host stack, switches to the given one and points the TEB's stack
 * bounds (StackBase/StackLimit/DeallocationStack) at it for the duration of `start`, so stack
 * probes and exception dispatch accept it. `stack` NULL: an ordinary thread with `size` bytes of
 * stack. Returns 0 or the pthread error number (positive), as pthread_create. */
int pf_thread_create_on_stack(pthread_t *out, void *(*start)(void *), void *argument, void *stack, size_t size);

/* Calls the guest function entry(arg0, arg1) (SysV ABI) on the stack [stack_low, stack_top) of the
 * calling thread and returns its result (rax); used for the guest main thread. Windows: also
 * moves the TEB stack bounds for the call and restores them afterwards. */
uint64_t pf_call_guest_on_stack(const void *entry, uint64_t arg0, uint64_t arg1, void *stack_low, void *stack_top);

/* The calling thread's stack (pthread_getattr_np+pthread_attr_getstack /
 * GetCurrentThreadStackLimits). */
int pf_thread_get_stack(void **base, size_t *size);

/* ---- Identity ---------------------------------------------------------------------------- */

/* The kernel thread id (gettid / GetCurrentThreadId). */
uint64_t pf_thread_id(void);

/* Names the calling thread (pthread_setname_np, truncated to 15 bytes on Linux /
 * SetThreadDescription). */
int pf_thread_set_name(const char *name);

/* The calling thread's name into buffer (pthread_getname_np / GetThreadDescription). */
int pf_thread_get_name(char *buffer, size_t size);

/* ---- Scheduling -------------------------------------------------------------------------- */

/* Lowest priority for background workers (SCHED_IDLE, else nice 19 /
 * THREAD_PRIORITY_IDLE). */
int pf_thread_set_idle_priority(void);

/* Number of CPUs this process may run on (sched_getaffinity / GetProcessAffinityMask). */
int pf_cpu_count_allowed(void);

/* Number of the CPU the calling thread runs on right now (sched_getcpu, -1 on failure /
 * GetCurrentProcessorNumber). */
int pf_current_cpu(void);

/* CPU time consumed by the calling thread, in nanoseconds (pthread_getcpuclockid +
 * clock_gettime / GetThreadTimes). */
int pf_thread_cpu_time_ns(uint64_t *nanoseconds);

/* ---- Other threads ----------------------------------------------------------------------- */

/* Calls visit(tid, name, user) for every thread of the process (/proc/self/task + comm /
 * Toolhelp32 + GetThreadDescription). Returns the number visited or PF_E*. */
int pf_threads_list(void (*visit)(uint64_t tid, const char *name, void *user), void *user);

/* Interrupts thread `tid` and calls sample(context, user) with its registers while it is stopped.
 * Linux: tgkill with a real-time-safe handler on the target thread (as the SIGPROF sampler and
 * the SIGUSR2 thread dump do today); Windows: SuspendThread + GetThreadContext from the calling
 * thread, then ResumeThread. `sample` must not allocate or take locks (the target may hold them);
 * read the target's stack with pf_read_memory_safe. Waits until `sample` has returned. */
int pf_thread_sample(uint64_t tid, void (*sample)(const PfContext *context, void *user), void *user);

PF_EXTERN_C_END
#endif
