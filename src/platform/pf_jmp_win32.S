/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: register-only setjmp/longjmp for Windows x64 (pf_jmp.h, ARCHITECTURE.md D9).
 *
 * MinGW's longjmp unwinds with SEH and fails on guest frames, which have no unwind information.
 * These save and restore the Windows x64 non-volatile state only; slot layout PF_JMP_* in
 * pf_win32_internal.h (pf_fault_resume_at in pf_exception_win32.c reads the same buffer):
 *   0 rbx  1 rbp  2 rdi  3 rsi  4 rsp (after return)  5 r12  6 r13  7 r14  8 r15
 *   9 rip (return address)  10 mxcsr (bits 0-31) | x87 control word (bits 32-47)  11 unused
 *   12..31 xmm6..xmm15 (16 bytes each)
 * Both are leaf functions that leave RSP alone until the final jump, so they need no unwind data.
 * int pf_setjmp_impl(pf_jmp_buf env)              env in rcx
 * void pf_longjmp_impl(pf_jmp_buf env, int value)  env in rcx, value in edx; returns `value`
 *                                                  (1 for 0) from the matching pf_setjmp_impl */
#ifdef __ELF__
#define FUNCTION(name) .globl name; .type name, @function; name:
#define END(name) .size name, . - name
#else
#define FUNCTION(name) .globl name; .def name; .scl 2; .type 32; .endef; name:
#define END(name)
#endif

    .text
    .p2align 4
FUNCTION(pf_setjmp_impl)
    mov %rbx, 0(%rcx)
    mov %rbp, 8(%rcx)
    mov %rdi, 16(%rcx)
    mov %rsi, 24(%rcx)
    lea 8(%rsp), %rax
    mov %rax, 32(%rcx)
    mov %r12, 40(%rcx)
    mov %r13, 48(%rcx)
    mov %r14, 56(%rcx)
    mov %r15, 64(%rcx)
    mov (%rsp), %rax
    mov %rax, 72(%rcx)
    stmxcsr 80(%rcx)
    fnstcw 84(%rcx)
    movdqu %xmm6, 96(%rcx)
    movdqu %xmm7, 112(%rcx)
    movdqu %xmm8, 128(%rcx)
    movdqu %xmm9, 144(%rcx)
    movdqu %xmm10, 160(%rcx)
    movdqu %xmm11, 176(%rcx)
    movdqu %xmm12, 192(%rcx)
    movdqu %xmm13, 208(%rcx)
    movdqu %xmm14, 224(%rcx)
    movdqu %xmm15, 240(%rcx)
    xor %eax, %eax
    ret
END(pf_setjmp_impl)

    .p2align 4
FUNCTION(pf_longjmp_impl)
    mov 0(%rcx), %rbx
    mov 8(%rcx), %rbp
    mov 16(%rcx), %rdi
    mov 24(%rcx), %rsi
    mov 40(%rcx), %r12
    mov 48(%rcx), %r13
    mov 56(%rcx), %r14
    mov 64(%rcx), %r15
    ldmxcsr 80(%rcx)
    fldcw 84(%rcx)
    movdqu 96(%rcx), %xmm6
    movdqu 112(%rcx), %xmm7
    movdqu 128(%rcx), %xmm8
    movdqu 144(%rcx), %xmm9
    movdqu 160(%rcx), %xmm10
    movdqu 176(%rcx), %xmm11
    movdqu 192(%rcx), %xmm12
    movdqu 208(%rcx), %xmm13
    movdqu 224(%rcx), %xmm14
    movdqu 240(%rcx), %xmm15
    mov %edx, %eax
    test %eax, %eax
    jnz 1f
    inc %eax
1:  mov 32(%rcx), %rsp
    jmp *72(%rcx)
END(pf_longjmp_impl)

#ifdef __ELF__
    .section .note.GNU-stack, "", @progbits
#endif
