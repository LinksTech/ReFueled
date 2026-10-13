/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: faults, breakpoints, register contexts and the watchdog on Windows (pf_exception.h).
 *
 * One vectored exception handler, registered first (AddVectoredExceptionHandler(1, ...)), sees
 * every exception of the process. Access violations inside a guest view that is being split are
 * repeated first (pf_win32_memory_transient_fault, pf_memory_win32.c). Then the registered
 * handlers run in registration order, as the Linux dispatcher does; the first PF_FAULT_HANDLED
 * resumes the thread with the (possibly changed) CONTEXT. Exceptions of other kinds (C++ throw,
 * debugger output, stack overflow, ...) and faults nobody handles go on to the next handler
 * (EXCEPTION_CONTINUE_SEARCH), which ends in the default: process termination.
 * Breakpoints follow the Linux convention: RIP is behind the int3 while the handlers run; when
 * none handles it, RIP is put back for the handlers after this one. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stddef.h>
#include "pf_exception.h"
#include "pf_win32_internal.h"

/* ---- Contexts ---------------------------------------------------------------------------- */

static const unsigned short reg_offset[PF_REG_COUNT] = {
    [PF_REG_RAX] = offsetof(CONTEXT, Rax), [PF_REG_RBX] = offsetof(CONTEXT, Rbx),
    [PF_REG_RCX] = offsetof(CONTEXT, Rcx), [PF_REG_RDX] = offsetof(CONTEXT, Rdx),
    [PF_REG_RSI] = offsetof(CONTEXT, Rsi), [PF_REG_RDI] = offsetof(CONTEXT, Rdi),
    [PF_REG_RBP] = offsetof(CONTEXT, Rbp), [PF_REG_RSP] = offsetof(CONTEXT, Rsp),
    [PF_REG_R8] = offsetof(CONTEXT, R8),   [PF_REG_R9] = offsetof(CONTEXT, R9),
    [PF_REG_R10] = offsetof(CONTEXT, R10), [PF_REG_R11] = offsetof(CONTEXT, R11),
    [PF_REG_R12] = offsetof(CONTEXT, R12), [PF_REG_R13] = offsetof(CONTEXT, R13),
    [PF_REG_R14] = offsetof(CONTEXT, R14), [PF_REG_R15] = offsetof(CONTEXT, R15),
    [PF_REG_RIP] = offsetof(CONTEXT, Rip), [PF_REG_EFLAGS] = offsetof(CONTEXT, EFlags),
};

uint64_t pf_context_get(const PfContext *context, PfReg reg) {
    if ((unsigned)reg >= PF_REG_COUNT) return 0;
    const unsigned char *base = (const unsigned char *)context->native;
    if (reg == PF_REG_EFLAGS) return *(const DWORD *)(const void *)(base + reg_offset[reg]);
    return *(const DWORD64 *)(const void *)(base + reg_offset[reg]);
}

void pf_context_set(PfContext *context, PfReg reg, uint64_t value) {
    if ((unsigned)reg >= PF_REG_COUNT) return;
    unsigned char *base = (unsigned char *)context->native;
    if (reg == PF_REG_EFLAGS) *(DWORD *)(void *)(base + reg_offset[reg]) = (DWORD)value;
    else *(DWORD64 *)(void *)(base + reg_offset[reg]) = value;
}

void *pf_context_native(PfContext *context) { return context->native; }

int pf_context_fault_access(const PfContext *context) { return context->access; }

/* ---- Fault dispatcher -------------------------------------------------------------------- */

#define MAX_HANDLERS 16
#define KIND_MASK ((1u << (PF_FAULT_SINGLE_STEP + 1)) - 1)

typedef struct {
    unsigned kinds;
    PfFaultHandler handler;
    void *user;
} Entry;

static Entry entries[MAX_HANDLERS]; /* an entry is complete before entry_count covers it */
static volatile LONG entry_count;
static SRWLOCK add_lock = SRWLOCK_INIT;
static INIT_ONCE veh_once = INIT_ONCE_STATIC_INIT;
static PVOID veh_handle;

static int fault_of(const EXCEPTION_RECORD *record, PfFault *fault) {
    fault->access = PF_ACCESS_UNKNOWN;
    fault->address = 0;
    fault->host_code = (int)record->ExceptionCode;
    switch (record->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_IN_PAGE_ERROR:
        fault->kind = record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? PF_FAULT_ACCESS : PF_FAULT_BUS;
        if (record->NumberParameters >= 2) {
            const ULONG_PTR how = record->ExceptionInformation[0];
            fault->access = how == 0 ? PF_ACCESS_READ : how == 1 ? PF_ACCESS_WRITE : how == 8 ? PF_ACCESS_EXEC : PF_ACCESS_UNKNOWN;
            fault->address = (uintptr_t)record->ExceptionInformation[1];
        }
        return 1;
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
        fault->kind = PF_FAULT_ILLEGAL;
        fault->address = (uintptr_t)record->ExceptionAddress;
        return 1;
    case EXCEPTION_BREAKPOINT:
        fault->kind = PF_FAULT_BREAKPOINT;
        fault->address = (uintptr_t)record->ExceptionAddress;
        return 1;
    case EXCEPTION_SINGLE_STEP:
        fault->kind = PF_FAULT_SINGLE_STEP;
        fault->address = (uintptr_t)record->ExceptionAddress;
        return 1;
    default:
        return 0;
    }
}

static LONG CALLBACK dispatch(EXCEPTION_POINTERS *pointers) {
    PfFault fault;
    if (!fault_of(pointers->ExceptionRecord, &fault)) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *native = pointers->ContextRecord;
    if (fault.kind == PF_FAULT_ACCESS && pf_win32_memory_transient_fault(fault.address, fault.access))
        return EXCEPTION_CONTINUE_EXECUTION;
    PfContext context = {native, fault.access};
    if (fault.kind == PF_FAULT_BREAKPOINT) native->Rip += 1; /* behind the int3, as on Linux */
    const LONG count = InterlockedCompareExchange(&entry_count, 0, 0);
    for (LONG i = 0; i < count; ++i) {
        if (!(entries[i].kinds & (1u << fault.kind))) continue;
        if (entries[i].handler(&fault, &context, entries[i].user) == PF_FAULT_HANDLED) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (fault.kind == PF_FAULT_BREAKPOINT) native->Rip -= 1;
    return EXCEPTION_CONTINUE_SEARCH;
}

static BOOL CALLBACK install_veh(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    veh_handle = AddVectoredExceptionHandler(1, dispatch);
    return TRUE;
}

/* For pf_init (pf_base_win32.c): the dispatcher goes in before anything else can add a handler. */
int pf_win32_exception_init(void) {
    InitOnceExecuteOnce(&veh_once, install_veh, NULL, NULL);
    return veh_handle ? 0 : PF_EIO;
}

int pf_fault_add_handler(unsigned kinds, PfFaultHandler handler, void *user) {
    if (!handler || kinds == 0 || (kinds & ~KIND_MASK)) return PF_EINVAL;
    int result = pf_win32_exception_init();
    if (result) return result;
    AcquireSRWLockExclusive(&add_lock);
    const LONG count = entry_count;
    if (count >= MAX_HANDLERS) {
        result = PF_ENOSPC;
    } else {
        entries[count] = (Entry){kinds, handler, user};
        InterlockedExchange(&entry_count, count + 1); /* full barrier: the entry is visible first */
    }
    ReleaseSRWLockExclusive(&add_lock);
    return result;
}

/* The recovery point's registers go into the CONTEXT; the thread continues as if pf_sigsetjmp had
 * returned `value`. Slot layout: pf_win32_internal.h. */
PfFaultResult pf_fault_resume_at(PfContext *context, pf_sigjmp_buf env, int value) {
    CONTEXT *native = context->native;
    const uint64_t *slot = env[0].slots;
    native->Rbx = slot[PF_JMP_RBX];
    native->Rbp = slot[PF_JMP_RBP];
    native->Rdi = slot[PF_JMP_RDI];
    native->Rsi = slot[PF_JMP_RSI];
    native->Rsp = slot[PF_JMP_RSP];
    native->R12 = slot[PF_JMP_R12];
    native->R13 = slot[PF_JMP_R13];
    native->R14 = slot[PF_JMP_R14];
    native->R15 = slot[PF_JMP_R15];
    native->Rip = slot[PF_JMP_RIP];
    native->Rax = (DWORD64)(value ? value : 1);
    native->MxCsr = (DWORD)slot[PF_JMP_CSR];
    if (native->ContextFlags & CONTEXT_FLOATING_POINT) {
        native->FltSave.MxCsr = (DWORD)slot[PF_JMP_CSR];
        native->FltSave.ControlWord = (WORD)(slot[PF_JMP_CSR] >> 32);
    }
    M128A *xmm = &native->Xmm6;
    for (int i = 0; i < 10; ++i) {
        xmm[i].Low = slot[PF_JMP_XMM6 + 2 * i];
        xmm[i].High = (LONGLONG)slot[PF_JMP_XMM6 + 2 * i + 1];
    }
    return PF_FAULT_HANDLED;
}

PF_NORETURN void pf_exit_now(int status) {
    TerminateProcess(GetCurrentProcess(), (UINT)status);
    for (;;) Sleep(INFINITE); /* TerminateProcess on the own process does not return */
}

/* ---- Watchdog: one timer thread, re-armed by every pf_watchdog_start ------------------------ */

static SRWLOCK watchdog_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE watchdog_changed = CONDITION_VARIABLE_INIT;
static ULONGLONG watchdog_deadline; /* GetTickCount64 value, 0: off */
static void (*watchdog_callback)(PfContext *context, void *user);
static void *watchdog_user;
static HANDLE watchdog_thread;

static DWORD WINAPI watchdog_main(void *parameter) {
    (void)parameter;
    AcquireSRWLockExclusive(&watchdog_lock);
    for (;;) {
        const ULONGLONG now = GetTickCount64();
        if (watchdog_deadline && now >= watchdog_deadline) {
            void (*callback)(PfContext *, void *) = watchdog_callback;
            void *user = watchdog_user;
            watchdog_deadline = 0;
            ReleaseSRWLockExclusive(&watchdog_lock);
            if (callback) callback(NULL, user);
            AcquireSRWLockExclusive(&watchdog_lock);
            continue;
        }
        const DWORD wait = watchdog_deadline ? (DWORD)(watchdog_deadline - now) : INFINITE;
        SleepConditionVariableSRW(&watchdog_changed, &watchdog_lock, wait, 0);
    }
}

int pf_watchdog_start(unsigned seconds, void (*on_timeout)(PfContext *context, void *user), void *user) {
    if (seconds && !on_timeout) return PF_EINVAL;
    int result = 0;
    AcquireSRWLockExclusive(&watchdog_lock);
    watchdog_deadline = seconds ? GetTickCount64() + (ULONGLONG)seconds * 1000 : 0;
    watchdog_callback = on_timeout;
    watchdog_user = user;
    if (seconds && !watchdog_thread) {
        watchdog_thread = CreateThread(NULL, 64 * 1024, watchdog_main, NULL, 0, NULL);
        if (!watchdog_thread) {
            watchdog_deadline = 0;
            result = PF_EAGAIN;
        }
    }
    ReleaseSRWLockExclusive(&watchdog_lock);
    WakeConditionVariable(&watchdog_changed);
    return result;
}
