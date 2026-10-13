/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: non-local jumps across guest frames.
 *
 * Guest frames have no unwind information. Two jumps cross them: a guest thread's exit
 * (scePthreadExit -> back to the host start routine) and the recovery from a fault in a
 * speculative guest-memory read (runtime_fault_recover). They must restore registers only, never
 * unwind. glibc's setjmp/longjmp and sigsetjmp/siglongjmp do exactly that and stay in use on
 * Linux; MinGW's longjmp unwinds with SEH, so Windows uses its own register-only pair.
 *
 * pf_setjmp/pf_longjmp: the thread exit (was setjmp/longjmp).
 * pf_sigsetjmp/pf_siglongjmp: recovery points (was sigsetjmp(buf, 0)/siglongjmp); the fault
 * dispatcher resumes one with pf_fault_resume_at (pf_exception.h).
 *
 * Linux: needs sigjmp_buf, i.e. _GNU_SOURCE or _POSIX_C_SOURCE before the first system header,
 * as every runtime source already defines (runtime.h has the same requirement). */
#ifndef REFUELED_PF_JMP_H
#define REFUELED_PF_JMP_H
#include "pf_base.h"

#if defined(_WIN32)
PF_EXTERN_C_BEGIN
/* rbx rbp rdi rsi rsp r12-r15 rip, mxcsr/fpucw, xmm6-xmm15 (Windows x64 non-volatile set). */
typedef struct PF_ALIGNED16 { uint64_t slots[32]; } pf_jmp_buf[1];
typedef pf_jmp_buf pf_sigjmp_buf;
PF_RETURNS_TWICE int pf_setjmp_impl(pf_jmp_buf env);
PF_NORETURN void pf_longjmp_impl(pf_jmp_buf env, int value);
PF_EXTERN_C_END
#define pf_setjmp(env) pf_setjmp_impl(env)
#define pf_longjmp(env, value) pf_longjmp_impl(env, value)
#define pf_sigsetjmp(env) pf_setjmp_impl(env)
#define pf_siglongjmp(env, value) pf_longjmp_impl(env, value)
#else
#include <setjmp.h>
typedef jmp_buf pf_jmp_buf;
typedef sigjmp_buf pf_sigjmp_buf;
#define pf_setjmp(env) setjmp(env)
#define pf_longjmp(env, value) longjmp(env, value)
#define pf_sigsetjmp(env) sigsetjmp(env, 0)
#define pf_siglongjmp(env, value) siglongjmp(env, value)
#endif

#endif
