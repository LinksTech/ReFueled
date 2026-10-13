/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_thread on Linux. Moved code with the same system calls and flags; the old places
 * are listed next to each function and in docs/refueled/log/K5a.md. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include "pf_linux_internal.h"
#include "pf_thread.h"

/* ---- Stacks ------------------------------------------------------------------------------ */

/* runtime_thread.c:239-250: attr_init, setstack (or setstacksize), pthread_create, attr_destroy.
 * The detach decision stays with the caller (runtime_thread.c:251). */
int pf_thread_create_on_stack(pthread_t *out, void *(*start)(void *), void *argument, void *stack, size_t size) {
    pthread_attr_t attributes;
    pthread_t created;
    int result;
    pthread_attr_init(&attributes);
    if (stack) pthread_attr_setstack(&attributes, stack, size);
    else if (size) pthread_attr_setstacksize(&attributes, size);
    result = pthread_create(out ? out : &created, &attributes, start, argument);
    pthread_attr_destroy(&attributes);
    return result;
}

/* probe.c:133-137 (enter_on_stack), same instructions: rcx = stack top is aligned down to 16,
 * the call is made with rsp 16-aligned, rbp keeps the old rsp. */
uint64_t pf_call_on_stack_asm(const void *entry, uint64_t arg0, uint64_t arg1, void *top);
__asm__(".pushsection .text\n.globl pf_call_on_stack_asm\n.hidden pf_call_on_stack_asm\n"
        ".type pf_call_on_stack_asm,@function\npf_call_on_stack_asm:\n"
        " push %rbp\n mov %rsp,%rbp\n and $-16,%rcx\n mov %rcx,%rsp\n"
        " mov %rdi,%rax\n mov %rsi,%rdi\n mov %rdx,%rsi\n call *%rax\n"
        " mov %rbp,%rsp\n pop %rbp\n ret\n"
        ".size pf_call_on_stack_asm,.-pf_call_on_stack_asm\n.popsection\n");

uint64_t pf_call_guest_on_stack(const void *entry, uint64_t arg0, uint64_t arg1, void *stack_low, void *stack_top) {
    (void)stack_low; /* Linux has no stack bounds to move */
    return pf_call_on_stack_asm(entry, arg0, arg1, stack_top);
}

/* runtime_kernel.c:451 */
int pf_thread_get_stack(void **base, size_t *size) {
    pthread_attr_t attributes;
    void *low = NULL;
    size_t length = 0;
    int result = pthread_getattr_np(pthread_self(), &attributes);
    if (result) return -result;
    result = pthread_attr_getstack(&attributes, &low, &length);
    pthread_attr_destroy(&attributes);
    if (result) return -result;
    if (base) *base = low;
    if (size) *size = length;
    return 0;
}

/* ---- Identity ---------------------------------------------------------------------------- */

uint64_t pf_thread_id(void) { return (uint64_t)syscall(SYS_gettid); }

/* runtime_thread.c:213-219 (set_host_name): 15 characters, longer names would be rejected. */
int pf_thread_set_name(const char *name) {
    char host[16] = {0};
    int result;
    if (!name) return PF_EINVAL;
    memcpy(host, name, strnlen(name, sizeof host - 1));
    result = pthread_setname_np(pthread_self(), host);
    return result ? -result : 0;
}

int pf_thread_get_name(char *buffer, size_t size) {
    char host[16] = {0};
    int result;
    size_t length;
    if (!buffer || size == 0) return PF_EINVAL;
    result = pthread_getname_np(pthread_self(), host, sizeof host);
    if (result) return -result;
    length = strnlen(host, sizeof host - 1);
    if (length > size - 1) length = size - 1;
    memcpy(buffer, host, length);
    buffer[length] = '\0';
    return 0;
}

/* ---- Scheduling -------------------------------------------------------------------------- */

/* gpu/shim/bbport_threads.h:26-31 (MakeBackground). The old code ignored a failure of both. */
int pf_thread_set_idle_priority(void) {
    struct sched_param param;
    memset(&param, 0, sizeof param);
    if (sched_setscheduler(0, SCHED_IDLE, &param) != 0) {
        if (setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19) != 0) return PF_EPERM;
    }
    return 0;
}

/* bbport_threads.h:15-22 (Available). */
int pf_cpu_count_allowed(void) {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int count = CPU_COUNT(&set);
        return count > 1 ? count : 1;
    }
    {
        long online = sysconf(_SC_NPROCESSORS_ONLN); /* the old fallback: hardware_concurrency */
        return online > 1 ? (int)online : 1;
    }
}

/* liverpool.cpp:123 (pthread_getcpuclockid) + clock_gettime. */
int pf_thread_cpu_time_ns(uint64_t *nanoseconds) {
    clockid_t clock;
    struct timespec now;
    if (!nanoseconds) return PF_EINVAL;
    if (pthread_getcpuclockid(pthread_self(), &clock) != 0) return PF_EINVAL;
    if (clock_gettime(clock, &now) != 0) return PF_EINVAL;
    *nanoseconds = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
    return 0;
}

/* ---- Other threads ----------------------------------------------------------------------- */

/* probe.c:234-244 (getdents64 over /proc/self/task, no stdio), runtime_kernel.c:540-545 and
 * vk_scheduler.cpp:41 (comm of each task). */
int pf_threads_list(void (*visit)(uint64_t tid, const char *name, void *user), void *user) {
    char buffer[4096];
    int visited = 0;
    long n;
    int dir;
    if (!visit) return PF_EINVAL;
    dir = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir < 0) return PF_ENOENT;
    while ((n = syscall(SYS_getdents64, dir, buffer, sizeof buffer)) > 0) {
        for (long at = 0; at < n;) {
            struct {
                uint64_t ino;
                int64_t off;
                unsigned short reclen;
                unsigned char type;
                char name[];
            } *entry = (void *)(buffer + at);
            uint64_t tid = 0;
            const char *digit = entry->name;
            at += entry->reclen;
            if (*digit < '0' || *digit > '9') continue;
            for (; *digit >= '0' && *digit <= '9'; digit++) tid = tid * 10 + (uint64_t)(*digit - '0');
            if (tid) {
                char path[64] = "/proc/self/task/";
                char comm[32] = "?";
                int fd;
                strcat(path, entry->name);
                strcat(path, "/comm");
                fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd >= 0) {
                    ssize_t got = read(fd, comm, sizeof comm - 1);
                    close(fd);
                    if (got > 0) {
                        if (comm[got - 1] == '\n') got--;
                        comm[got] = '\0';
                    } else {
                        strcpy(comm, "?");
                    }
                }
                visit(tid, comm, user);
                visited++;
            }
        }
    }
    close(dir);
    return visited;
}

/* Sampling: the BB_SAMPLE_THREAD sampler of runtime_kernel.c as before the port (old
 * runtime_kernel.c:511-535, 555 and sample_start): SIGPROF, handler SA_SIGINFO|SA_RESTART with an
 * empty mask, tgkill per sample, no waiting for the handler (IF-10, ARCHITECTURE.md D20). The
 * handler is installed on the first call instead of at sample_start; no SIGPROF is sent before. */
static void (*_Atomic sample_function)(const PfContext *context, void *user);
static void *_Atomic sample_user;
static pthread_once_t sample_once = PTHREAD_ONCE_INIT;
static int sample_install_result;

static void sample_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    void (*sample)(const PfContext *, void *) = atomic_load(&sample_function);
    if (!sample) return;
    PfContext wrapped;
    wrapped.native = (ucontext_t *)context;
    sample(&wrapped, atomic_load(&sample_user));
}

static void sample_install(void) {
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = sample_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sample_install_result = sigaction(SIGPROF, &action, NULL) == 0 ? 0 : PF_EINVAL;
}

int pf_thread_sample(uint64_t tid, void (*sample)(const PfContext *context, void *user), void *user) {
    if (!sample || tid == 0) return PF_EINVAL;
    atomic_store(&sample_user, user);
    atomic_store(&sample_function, sample);
    pthread_once(&sample_once, sample_install);
    if (sample_install_result) return sample_install_result;
    if (syscall(SYS_tgkill, getpid(), (pid_t)tid, SIGPROF) != 0) return errno == ESRCH ? PF_ENOENT : PF_EINVAL;
    return 0;
}

/* runtime_kernel.c get_current_cpu: sched_getcpu. */
int pf_current_cpu(void) { return sched_getcpu(); }
