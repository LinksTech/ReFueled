/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux faults, register contexts, watchdog (pf_exception.h).
 *
 * Moved code, same sigaction flags and order as before:
 *   SIGSEGV/SIGBUS/SIGILL  SA_SIGINFO, empty mask         (src/probe.c, fault)
 *   SIGTRAP                SA_SIGINFO|SA_NODEFER, empty mask (gpu/shim/bbport_guest_hooks.cpp,
 *                                                            bbport_free_check.cpp)
 *   SIGALRM                SA_SIGINFO, empty mask         (src/probe.c, watchdog)
 *   SIGUSR2                SA_SIGINFO|SA_RESTART, empty mask (src/probe.c, thread_dump; D20)
 * The action that was installed before ours is remembered per signal and is called for faults no
 * registered handler claims, exactly as the shims chained to theirs; without one the signal's
 * default action runs. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include "pf_linux_internal.h"
#include "pf_jmp.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef TRAP_TRACE
#define TRAP_TRACE 2
#endif

/* ---- Contexts ---------------------------------------------------------------------------- */

static const int reg_index[PF_REG_COUNT] = {
    [PF_REG_RAX] = REG_RAX, [PF_REG_RBX] = REG_RBX, [PF_REG_RCX] = REG_RCX, [PF_REG_RDX] = REG_RDX,
    [PF_REG_RSI] = REG_RSI, [PF_REG_RDI] = REG_RDI, [PF_REG_RBP] = REG_RBP, [PF_REG_RSP] = REG_RSP,
    [PF_REG_R8] = REG_R8, [PF_REG_R9] = REG_R9, [PF_REG_R10] = REG_R10, [PF_REG_R11] = REG_R11,
    [PF_REG_R12] = REG_R12, [PF_REG_R13] = REG_R13, [PF_REG_R14] = REG_R14, [PF_REG_R15] = REG_R15,
    [PF_REG_RIP] = REG_RIP, [PF_REG_EFLAGS] = REG_EFL,
};

uint64_t pf_context_get(const PfContext *context, PfReg reg) {
    if ((unsigned)reg >= PF_REG_COUNT) return 0;
    return (uint64_t)context->native->uc_mcontext.gregs[reg_index[reg]];
}

void pf_context_set(PfContext *context, PfReg reg, uint64_t value) {
    if ((unsigned)reg >= PF_REG_COUNT) return;
    context->native->uc_mcontext.gregs[reg_index[reg]] = (greg_t)value;
}

void *pf_context_native(PfContext *context) { return context->native; }

/* Page-fault error code bit 1 = write (shadPS4 common/signal_context.cpp IsWriteError, Linux). */
int pf_context_fault_access(const PfContext *context) {
    return (context->native->uc_mcontext.gregs[REG_ERR] & 0x2) ? PF_ACCESS_WRITE : PF_ACCESS_READ;
}

/* ---- Fault dispatcher -------------------------------------------------------------------- */

#define MAX_HANDLERS 16
#define KIND_MASK ((1u << (PF_FAULT_SINGLE_STEP + 1)) - 1)

typedef struct {
    unsigned kinds;
    PfFaultHandler handler;
    void *user;
} Entry;

static Entry entries[MAX_HANDLERS];       /* an entry is complete before the count covers it */
static atomic_uint entry_count;
static pthread_mutex_t add_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sigaction previous[NSIG];
static unsigned char installed[NSIG];
static _Thread_local int current_signal; /* the signal being dispatched on this thread */

static int signal_of(PfFaultKind kind) {
    switch (kind) {
    case PF_FAULT_ACCESS: return SIGSEGV;
    case PF_FAULT_BUS: return SIGBUS;
    case PF_FAULT_ILLEGAL: return SIGILL;
    default: return SIGTRAP; /* breakpoint and single step share it */
    }
}

/* The previous action, then the default action: the chain the shims used. */
static void chain_or_default(int sig, siginfo_t *info, void *context) {
    const struct sigaction *old = &previous[sig];
    if (old->sa_flags & SA_SIGINFO) {
        if (old->sa_sigaction) {
            old->sa_sigaction(sig, info, context);
            return;
        }
    } else if (old->sa_handler != SIG_DFL && old->sa_handler != SIG_IGN && old->sa_handler) {
        old->sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void dispatch(int sig, siginfo_t *info, void *context) {
    PfFault fault;
    fault.kind = sig == SIGSEGV ? PF_FAULT_ACCESS : sig == SIGBUS ? PF_FAULT_BUS :
                 sig == SIGILL ? PF_FAULT_ILLEGAL :
                 info->si_code == TRAP_TRACE ? PF_FAULT_SINGLE_STEP : PF_FAULT_BREAKPOINT;
    fault.access = PF_ACCESS_UNKNOWN;
    fault.address = (uintptr_t)info->si_addr;
    fault.host_code = sig;
    PfContext pf_context = {(ucontext_t *)context};
    int outer = current_signal;
    current_signal = sig;
    unsigned count = atomic_load_explicit(&entry_count, memory_order_acquire);
    for (unsigned i = 0; i < count; i++) {
        if (!(entries[i].kinds & (1u << fault.kind))) continue;
        if (entries[i].handler(&fault, &pf_context, entries[i].user) == PF_FAULT_HANDLED) {
            current_signal = outer;
            return;
        }
    }
    current_signal = outer;
    chain_or_default(sig, info, context);
}

static int install_signal(int sig) {
    if (installed[sig]) return 0;
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = dispatch;
    action.sa_flags = sig == SIGTRAP ? (SA_SIGINFO | SA_NODEFER) : SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(sig, &action, &previous[sig]) != 0) return PF_EIO;
    installed[sig] = 1;
    return 0;
}

int pf_fault_add_handler(unsigned kinds, PfFaultHandler handler, void *user) {
    if (!handler || kinds == 0 || (kinds & ~KIND_MASK)) return PF_EINVAL;
    pthread_mutex_lock(&add_lock);
    int result = 0;
    unsigned count = atomic_load_explicit(&entry_count, memory_order_relaxed);
    if (count >= MAX_HANDLERS) {
        result = PF_ENOSPC;
    } else {
        for (int kind = 0; kind <= PF_FAULT_SINGLE_STEP && result == 0; kind++)
            if (kinds & (1u << kind)) result = install_signal(signal_of((PfFaultKind)kind));
        if (result == 0) {
            entries[count].kinds = kinds;
            entries[count].handler = handler;
            entries[count].user = user;
            atomic_store_explicit(&entry_count, count + 1, memory_order_release);
        }
    }
    pthread_mutex_unlock(&add_lock);
    return result;
}

PfFaultResult pf_fault_resume_at(PfContext *context, pf_sigjmp_buf env, int value) {
    (void)context;
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, current_signal ? current_signal : SIGSEGV);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);
    pf_siglongjmp(env, value);
}

__attribute__((noreturn)) void pf_exit_now(int status) { _exit(status); }

/* ---- Watchdog ---------------------------------------------------------------------------- */

static void (*_Atomic watchdog_callback)(PfContext *, void *);
static void *_Atomic watchdog_user;

static void watchdog_signal(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    void (*callback)(PfContext *, void *) = atomic_load(&watchdog_callback);
    if (!callback) return;
    PfContext pf_context = {(ucontext_t *)context};
    callback(&pf_context, atomic_load(&watchdog_user));
}

int pf_watchdog_start(unsigned seconds, void (*on_timeout)(PfContext *context, void *user), void *user) {
    if (seconds == 0) {
        alarm(0);
        return 0;
    }
    if (!on_timeout) return PF_EINVAL;
    atomic_store(&watchdog_user, user);
    atomic_store(&watchdog_callback, on_timeout);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = watchdog_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGALRM, &action, NULL) != 0) return PF_EIO;
    alarm(seconds);
    return 0;
}

/* ---- Thread dump on request (old src/probe.c:228, 233-246, 397-398) ---------------------- */

static void (*_Atomic dump_callback)(const PfContext *, uint64_t, void *);
static void *_Atomic dump_user;

static void dump_signal(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    void (*dump)(const PfContext *, uint64_t, void *) = atomic_load(&dump_callback);
    if (!dump) return;
    PfContext pf_context = {(ucontext_t *)context};
    dump(&pf_context, (uint64_t)gettid(), atomic_load(&dump_user));
}

int pf_dump_signal_install(void (*dump)(const PfContext *context, uint64_t tid, void *user), void *user) {
    if (!dump) return PF_EINVAL;
    atomic_store(&dump_user, user);
    atomic_store(&dump_callback, dump);
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = dump_signal;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGUSR2, &action, NULL) == 0 ? 0 : PF_EIO;
}

int pf_dump_other_threads(void) {
    if (!atomic_load(&dump_callback)) return PF_EINVAL;
    int dir = open("/proc/self/task", O_RDONLY | O_DIRECTORY);
    char buffer[4096];
    long n;
    pid_t self = gettid();
    while (dir >= 0 && (n = syscall(SYS_getdents64, dir, buffer, sizeof(buffer))) > 0)
        for (long at = 0; at < n;) {
            struct { uint64_t ino; int64_t off; unsigned short reclen; unsigned char type; char name[]; } *d = (void *)(buffer + at);
            pid_t tid = (pid_t)strtol(d->name, NULL, 10);
            if (tid > 0 && tid != self) { syscall(SYS_tgkill, getpid(), tid, SIGUSR2); usleep(20000); }
            at += d->reclen;
        }
    usleep(100000);
    return 0;
}
