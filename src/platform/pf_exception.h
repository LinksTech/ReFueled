/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: faults, breakpoints, register contexts, watchdog.
 *
 * One dispatcher per process receives every synchronous fault (Linux: sigaction for SIGSEGV,
 * SIGBUS, SIGILL, SIGTRAP with SA_SIGINFO, as probe.c, gpu/shim/bbport_guest_hooks.cpp and
 * bbport_free_check.cpp install them today; Windows: a first vectored exception handler) and
 * offers it to the registered handlers in registration order. The first that returns
 * PF_FAULT_HANDLED ends the search and execution resumes with the (possibly modified) context.
 * When none handles it, the platform's default action follows (Linux: the signal's default,
 * re-raised; Windows: EXCEPTION_CONTINUE_SEARCH).
 *
 * Handlers run on the faulting thread, may run inside guest code's stack frame, and must only
 * use async-signal-safe facilities (no locks the faulting thread may hold, no malloc). */
#ifndef REFUELED_PF_EXCEPTION_H
#define REFUELED_PF_EXCEPTION_H
#include "pf_base.h"
#include "pf_jmp.h"

PF_EXTERN_C_BEGIN

/* A thread's register state at a fault or a sample: wraps ucontext_t (Linux) / CONTEXT (Windows). */
typedef struct PfContext PfContext;

typedef enum {
    PF_REG_RAX, PF_REG_RBX, PF_REG_RCX, PF_REG_RDX, PF_REG_RSI, PF_REG_RDI, PF_REG_RBP, PF_REG_RSP,
    PF_REG_R8, PF_REG_R9, PF_REG_R10, PF_REG_R11, PF_REG_R12, PF_REG_R13, PF_REG_R14, PF_REG_R15,
    PF_REG_RIP, PF_REG_EFLAGS,
    PF_REG_COUNT
} PfReg;

uint64_t pf_context_get(const PfContext *context, PfReg reg);
void pf_context_set(PfContext *context, PfReg reg, uint64_t value);
/* The native structure (ucontext_t* / CONTEXT*), for src/platform only. */
void *pf_context_native(PfContext *context);

typedef enum {
    PF_FAULT_ACCESS,      /* SIGSEGV / EXCEPTION_ACCESS_VIOLATION */
    PF_FAULT_BUS,         /* SIGBUS / EXCEPTION_IN_PAGE_ERROR */
    PF_FAULT_ILLEGAL,     /* SIGILL / EXCEPTION_ILLEGAL_INSTRUCTION, EXCEPTION_PRIV_INSTRUCTION */
    PF_FAULT_BREAKPOINT,  /* int3: SIGTRAP / EXCEPTION_BREAKPOINT */
    PF_FAULT_SINGLE_STEP, /* EFLAGS.TF: SIGTRAP (TRAP_TRACE) / EXCEPTION_SINGLE_STEP */
} PfFaultKind;

typedef enum { PF_ACCESS_UNKNOWN, PF_ACCESS_READ, PF_ACCESS_WRITE, PF_ACCESS_EXEC } PfAccess;

typedef struct {
    PfFaultKind kind;
    PfAccess access;   /* PF_FAULT_ACCESS only; Linux reports PF_ACCESS_UNKNOWN */
    uintptr_t address; /* the data address of an access/bus fault (si_addr / ExceptionInformation[1]) */
    int host_code;     /* signal number / exception code, for messages */
} PfFault;
/* Breakpoints follow the Linux convention on both platforms: PF_REG_RIP is the address after the
 * int3 (Windows adds 1 before calling the handlers and subtracts nothing afterwards). */

typedef enum { PF_FAULT_CONTINUE_SEARCH = 0, PF_FAULT_HANDLED = 1 } PfFaultResult;
typedef PfFaultResult (*PfFaultHandler)(const PfFault *fault, PfContext *context, void *user);

/* Appends a handler for the given kinds (bit mask of 1u << PfFaultKind). Up to 16 handlers.
 * Installs the platform hook for those kinds on first use. Returns 0 or PF_E*. */
int pf_fault_add_handler(unsigned kinds, PfFaultHandler handler, void *user);

/* Resumes a recovery point from inside a fault handler: the pf_sigsetjmp that filled `env`
 * returns `value` (non-zero). Linux: unblocks the signal and siglongjmps (does not return);
 * Windows: loads env into the context and returns PF_FAULT_HANDLED. A handler writes
 * `return pf_fault_resume_at(context, env, 1);`. */
PfFaultResult pf_fault_resume_at(PfContext *context, pf_sigjmp_buf env, int value);

/* Ends the process now, without destructors or atexit handlers (_exit). Safe in handlers. */
__attribute__((noreturn)) void pf_exit_now(int status);

/* ---- Watchdog ---------------------------------------------------------------------------- */

/* After `seconds` (0: off), calls on_timeout(context, user) once. Linux: alarm() + SIGALRM, the
 * context is the interrupted thread's; Windows: a timer thread, context NULL. The callback
 * usually dumps all threads (pf_thread_sample) and calls pf_exit_now. Replaces an earlier timer. */
int pf_watchdog_start(unsigned seconds, void (*on_timeout)(PfContext *context, void *user), void *user);

PF_EXTERN_C_END
#endif
