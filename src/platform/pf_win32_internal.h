/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: declarations shared between the Windows files of the platform layer only
 * (src/platform/pf_*_win32.c). Not part of the pf_* interface; code outside src/platform never
 * includes this header. */
#ifndef REFUELED_PF_WIN32_INTERNAL_H
#define REFUELED_PF_WIN32_INTERNAL_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* A PfContext on Windows: the CONTEXT of an exception or of a suspended thread. */
struct _CONTEXT;
struct PfContext {
    struct _CONTEXT *native;
    int access; /* PfAccess of the fault (set by the dispatcher), PF_ACCESS_UNKNOWN otherwise */
};

/* Slot layout of pf_jmp_buf (uint64_t slots[32]) written by pf_setjmp_impl (pf_jmp_win32.S, K6)
 * and read by pf_longjmp_impl and pf_fault_resume_at (pf_exception_win32.c): the Windows x64
 * non-volatile state. RSP and RIP are the values after pf_setjmp_impl returned (caller's RSP, return
 * address); MXCSR in the low 32 bits of PF_JMP_CSR, the x87 control word in bits 32-47. */
enum {
    PF_JMP_RBX, PF_JMP_RBP, PF_JMP_RDI, PF_JMP_RSI, PF_JMP_RSP, PF_JMP_R12, PF_JMP_R13, PF_JMP_R14,
    PF_JMP_R15, PF_JMP_RIP, PF_JMP_CSR, PF_JMP_RESERVED,
    PF_JMP_XMM6 = 12 /* xmm6..xmm15: two slots each, low quadword first, up to slot 31 */
};


/* pf_memory_win32.c, for the fault dispatcher (pf_exception_win32.c, K7), which asks it first for
 * every access violation. Splitting a mapped view (a guest mapping replaced or unmapped in part)
 * takes the whole view down for a moment and maps the remaining pieces again, where Linux's
 * MAP_FIXED is atomic; another thread touching those pieces in that window faults although the
 * memory is there. Returns 1 when `address` lies in a view being remapped right now (after waiting
 * for the remap to finish) or in the last remapped range and is now accessible for `access`
 * (a PfAccess value): the dispatcher then resumes the thread unchanged so it repeats the access.
 * Returns 0 otherwise. Lock-free and async-safe. */
int pf_win32_memory_transient_fault(uintptr_t address, int access);

/* pf_exception_win32.c, for pf_init: registers the vectored exception handler (once). */
int pf_win32_exception_init(void);

PF_EXTERN_C_END
#endif
