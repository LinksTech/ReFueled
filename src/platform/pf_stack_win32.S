/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: running code on a caller-provided stack on Windows x64 (pf_thread.h, D7).
 *
 * Windows checks a thread's stack pointer against the TEB's stack bounds (stack probes in
 * __chkstk, exception dispatch), so for the duration of the call these point at the new stack
 * [low, top), as SwitchToFiber does: NT_TIB.StackBase gs:[0x08], NT_TIB.StackLimit gs:[0x10],
 * TEB.DeallocationStack gs:[0x1478]. The old values are restored before returning, which also
 * keeps the thread's own stack release at exit correct.
 * The prologues push every register they use and then set RBP as frame pointer, described with
 * .seh_* unwind codes, so a stack walk from the new stack finds its way back to the host stack.
 *
 * uint64_t pf_call_guest_on_stack(const void *entry, uint64_t arg0, uint64_t arg1, void *stack_low,
 *                                 void *stack_top)
 *   rcx entry, rdx arg0, r8 arg1, r9 low, [rsp+40] top. Calls entry(arg0, arg1) with the SysV ABI
 *   (rdi, rsi; the guest may change xmm6-xmm15, rdi and rsi, which Windows code keeps, so they are
 *   saved here).
 * void *pf_win32_run_on_stack(void *(*function)(void *), void *argument, void *stack_low,
 *                             void *stack_top)
 *   rcx function, rdx argument, r8 low, r9 top. Calls function(argument) with the Windows ABI
 *   (pf_thread_create_on_stack). */
#ifdef __ELF__
#define FUNCTION(name) .globl name; .type name, @function; name:
#define END(name) .size name, . - name
#define SEH(...)
#else
#define FUNCTION(name) .globl name; .def name; .scl 2; .type 32; .endef; name:
#define END(name)
#define SEH(...) __VA_ARGS__
#endif

    .text
    .p2align 4
FUNCTION(pf_call_guest_on_stack)
    SEH(.seh_proc pf_call_guest_on_stack)
    push %rbp
    SEH(.seh_pushreg %rbp)
    push %rbx
    SEH(.seh_pushreg %rbx)
    push %rdi
    SEH(.seh_pushreg %rdi)
    push %rsi
    SEH(.seh_pushreg %rsi)
    push %r12
    SEH(.seh_pushreg %r12)
    push %r13
    SEH(.seh_pushreg %r13)
    push %r14
    SEH(.seh_pushreg %r14)
    push %r15
    SEH(.seh_pushreg %r15)
    mov %rsp, %rbp
    SEH(.seh_setframe %rbp, 0)
    SEH(.seh_endprologue)
    sub $168, %rsp
    movdqu %xmm6, 0(%rsp)
    movdqu %xmm7, 16(%rsp)
    movdqu %xmm8, 32(%rsp)
    movdqu %xmm9, 48(%rsp)
    movdqu %xmm10, 64(%rsp)
    movdqu %xmm11, 80(%rsp)
    movdqu %xmm12, 96(%rsp)
    movdqu %xmm13, 112(%rsp)
    movdqu %xmm14, 128(%rsp)
    movdqu %xmm15, 144(%rsp)
    mov 104(%rbp), %r10          /* stack_top: 8 pushes, return address, 32 bytes shadow space */
    mov %gs:0x08, %r12
    mov %gs:0x10, %r13
    mov %gs:0x1478, %r14
    mov %r10, %gs:0x08
    mov %r9, %gs:0x10
    mov %r9, %gs:0x1478
    mov %rsp, %r15
    and $-16, %r10
    mov %r10, %rsp
    mov %rcx, %rax
    mov %rdx, %rdi
    mov %r8, %rsi
    call *%rax                   /* SysV: rbx, rbp, r12-r15 survive */
    mov %r15, %rsp
    mov %r12, %gs:0x08
    mov %r13, %gs:0x10
    mov %r14, %gs:0x1478
    movdqu 0(%rsp), %xmm6
    movdqu 16(%rsp), %xmm7
    movdqu 32(%rsp), %xmm8
    movdqu 48(%rsp), %xmm9
    movdqu 64(%rsp), %xmm10
    movdqu 80(%rsp), %xmm11
    movdqu 96(%rsp), %xmm12
    movdqu 112(%rsp), %xmm13
    movdqu 128(%rsp), %xmm14
    movdqu 144(%rsp), %xmm15
    lea 0(%rbp), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rsi
    pop %rdi
    pop %rbx
    pop %rbp
    ret
    SEH(.seh_endproc)
END(pf_call_guest_on_stack)

    .p2align 4
FUNCTION(pf_win32_run_on_stack)
    SEH(.seh_proc pf_win32_run_on_stack)
    push %rbp
    SEH(.seh_pushreg %rbp)
    push %r12
    SEH(.seh_pushreg %r12)
    push %r13
    SEH(.seh_pushreg %r13)
    push %r14
    SEH(.seh_pushreg %r14)
    push %r15
    SEH(.seh_pushreg %r15)
    mov %rsp, %rbp
    SEH(.seh_setframe %rbp, 0)
    SEH(.seh_endprologue)
    mov %gs:0x08, %r12
    mov %gs:0x10, %r13
    mov %gs:0x1478, %r14
    mov %r9, %gs:0x08
    mov %r8, %gs:0x10
    mov %r8, %gs:0x1478
    mov %rsp, %r15
    and $-16, %r9
    lea -32(%r9), %rsp           /* shadow space, 16-aligned at the call */
    mov %rcx, %rax
    mov %rdx, %rcx
    call *%rax                   /* Windows ABI: r12-r15 survive */
    mov %r15, %rsp
    mov %r12, %gs:0x08
    mov %r13, %gs:0x10
    mov %r14, %gs:0x1478
    lea 0(%rbp), %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    ret
    SEH(.seh_endproc)
END(pf_win32_run_on_stack)

#ifdef __ELF__
    .section .note.GNU-stack, "", @progbits
#endif
