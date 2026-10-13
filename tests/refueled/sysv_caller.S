/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: F7 - "guest" routines for tests/refueled/test_sysv_abi.c.
 * Everything here behaves like PS4 guest code: a SysV x86-64 caller that calls host functions
 * declared with ABI (= sysv_abi) and is itself called through ABI function pointers.
 * AT&T syntax, assembles as ELF (Linux) and COFF (Windows/clang). The red zone is not used
 * and not tested here (package RZ). */

#ifdef __ELF__
#define CALL(sym) call sym@PLT
#define FUNC(sym) .globl sym; .type sym,@function; sym:
#define ENDF(sym) .size sym,.-sym
#else
#define CALL(sym) call sym
#define FUNC(sym) .globl sym; sym:
#define ENDF(sym)
#endif

/* movabs of a double bit pattern into an xmm register, through r10 (caller-saved scratch) */
.macro LDX reg, bits
    movabs $\bits, %r10
    movq %r10, \reg
.endm

    .text

/* uint64_t guest_call_mix(void): 8 integer args (6 regs + 2 stack), 8 doubles in xmm0-7,
 * 2 doubles on the stack. Stack args in order g, h, s0, s1 at (rsp), 8, 16, 24. */
    .p2align 4
FUNC(guest_call_mix)
    push %rbp                          /* rsp = 0 mod 16 */
    movabs $0x4069180000000000, %rax   /* s1 = 200.75 */
    push %rax
    movabs $0x4059100000000000, %rax   /* s0 = 100.25 */
    push %rax
    movabs $0x8888888888888888, %rax   /* h */
    push %rax
    movabs $0x7777777777777777, %rax   /* g */
    push %rax
    movabs $0x1111111111111111, %rdi
    movabs $0x2222222222222222, %rsi
    movabs $0x3333333333333333, %rdx
    movabs $0x4444444444444444, %rcx
    movabs $0x5555555555555555, %r8
    movabs $0x6666666666666666, %r9
    LDX %xmm0, 0x3fe0000000000000      /* 0.5 */
    LDX %xmm1, 0x3ff8000000000000      /* 1.5 */
    LDX %xmm2, 0x4004000000000000      /* 2.5 */
    LDX %xmm3, 0x400c000000000000      /* 3.5 */
    LDX %xmm4, 0x4012000000000000      /* 4.5 */
    LDX %xmm5, 0x4016000000000000      /* 5.5 */
    LDX %xmm6, 0x401a000000000000      /* 6.5 */
    LDX %xmm7, 0x401e000000000000      /* 7.5 */
    mov $8, %eax
    CALL(host_mix)
    add $32, %rsp
    pop %rbp
    ret
ENDF(guest_call_mix)

/* void guest_call_fmix(double *out): host_fmix(float f, double d, int32_t i) with f=1.5f in xmm0
 * (low 32 bits), d=2.25 in xmm1, i=-3 in edi; the double result goes to *out. */
    .p2align 4
FUNC(guest_call_fmix)
    push %rbx
    mov %rdi, %rbx
    mov $0x3fc00000, %eax              /* 1.5f */
    movd %eax, %xmm0
    LDX %xmm1, 0x4002000000000000      /* 2.25 */
    mov $-3, %edi
    CALL(host_fmix)
    movsd %xmm0, (%rbx)
    pop %rbx
    ret
ENDF(guest_call_fmix)

/* int guest_call_big(uint64_t *out, uint64_t seed): struct > 16 bytes returned via sret.
 * rdi = hidden result pointer, rsi = seed; the callee must return the pointer in rax.
 * Returns 1 if rax == out. */
    .p2align 4
FUNC(guest_call_big)
    push %rbx
    mov %rdi, %rbx
    CALL(host_make_big)
    xor %ecx, %ecx
    cmp %rbx, %rax
    sete %cl
    mov %ecx, %eax
    pop %rbx
    ret
ENDF(guest_call_big)

/* void guest_call_small(uint64_t x, uint64_t *out): struct of 2 INTEGER eightbytes -> rax:rdx */
    .p2align 4
FUNC(guest_call_small)
    push %rbx
    mov %rsi, %rbx
    CALL(host_make_small)
    mov %rax, (%rbx)
    mov %rdx, 8(%rbx)
    pop %rbx
    ret
ENDF(guest_call_small)

/* void guest_call_dpair(double x, double *out): struct of 2 doubles -> xmm0:xmm1 (x is in xmm0) */
    .p2align 4
FUNC(guest_call_dpair)
    push %rbx
    mov %rdi, %rbx
    CALL(host_make_dpair)
    movsd %xmm0, (%rbx)
    movsd %xmm1, 8(%rbx)
    pop %rbx
    ret
ENDF(guest_call_dpair)

/* void guest_call_mixed(uint64_t x, uint64_t *out): struct {uint64_t; double} -> rax:xmm0 */
    .p2align 4
FUNC(guest_call_mixed)
    push %rbx
    mov %rsi, %rbx
    CALL(host_make_mixed)
    mov %rax, (%rbx)
    movsd %xmm0, 8(%rbx)
    pop %rbx
    ret
ENDF(guest_call_mixed)

/* uint64_t guest_call_take(void): structs by value. Big (32 bytes, MEMORY class) goes on the
 * stack, Small (2 INTEGER eightbytes) in rdi:rsi, DPair (2 SSE eightbytes) in xmm0:xmm1. */
    .p2align 4
FUNC(guest_call_take)
    push %rbx
    mov $0x1004, %eax
    push %rax
    mov $0x1003, %eax
    push %rax
    mov $0x1002, %eax
    push %rax
    mov $0x1001, %eax
    push %rax
    mov $0x2001, %edi
    mov $0x2002, %esi
    LDX %xmm0, 0x3ff4000000000000      /* 1.25 */
    LDX %xmm1, 0x400a000000000000      /* 3.25 */
    xor %eax, %eax
    CALL(host_take_structs)
    add $32, %rsp
    pop %rbx
    ret
ENDF(guest_call_take)

/* uint64_t guest_call_aligned(uint64_t skew): builds a stack with an arbitrary skew, then
 * re-aligns rsp to 16 as SysV guest code does before every call, passes rsp in rdi. */
    .p2align 4
FUNC(guest_call_aligned)
    push %rbp
    mov %rsp, %rbp
    sub $4096, %rsp
    sub %rdi, %rsp
    and $-16, %rsp
    mov %rsp, %rdi
    CALL(host_check_align)
    mov %rbp, %rsp
    pop %rbp
    ret
ENDF(guest_call_aligned)

/* uint64_t guest_get_rsp(void): returns the rsp of the caller as it was before the call */
    .p2align 4
FUNC(guest_get_rsp)
    lea 8(%rsp), %rax
    ret
ENDF(guest_get_rsp)

/* uint64_t guest_entry_misalign(void): host -> guest entry; returns (caller rsp) mod 16,
 * which must be 0 because the host executed the call with an aligned stack. */
    .p2align 4
FUNC(guest_entry_misalign)
    lea 8(%rsp), %rax
    and $15, %eax
    ret
ENDF(guest_entry_misalign)

/* uint64_t guest_clobber(uint64_t x): a guest function reached through an ABI pointer.
 * Destroys xmm6-15 (allowed under SysV, callee-saved on Win64), rdi and rsi; uses
 * rbx, rbp, r12-r15 but restores them as SysV requires. Returns x*3+1. */
    .p2align 4
FUNC(guest_clobber)
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov $-1, %rbx
    mov $-2, %rbp
    mov $-3, %r12
    mov $-4, %r13
    mov $-5, %r14
    mov $-6, %r15
    movabs $0x7ff8c0dec0dec0de, %r11
    movq %r11, %xmm6
    movq %r11, %xmm7
    movq %r11, %xmm8
    movq %r11, %xmm9
    movq %r11, %xmm10
    movq %r11, %xmm11
    movq %r11, %xmm12
    movq %r11, %xmm13
    movq %r11, %xmm14
    movq %r11, %xmm15
    lea 1(%rdi,%rdi,2), %rax
    mov $0x77, %edi
    mov $0x78, %esi
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret
ENDF(guest_clobber)

/* uint64_t guest_invoke_probe(GuestFn fn, uint64_t x, uint64_t *out): loads known patterns into
 * xmm6-15 and rbx, rbp, r12-r15, calls fn(x) and stores the registers afterwards:
 *   out[0..9]   xmm6..xmm15 (raw 64-bit)      out[10..15] rbx rbp r12 r13 r14 r15
 * Returns fn's result. The host checks: callee-saved GPRs unchanged, xmm6-15 clobbered. */
    .p2align 4
FUNC(guest_invoke_probe)
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    sub $24, %rsp                      /* rsp = 0 mod 16 */
    mov %rdx, (%rsp)
    mov %rdi, %rax
    mov %rsi, %rdi
    movabs $0x1010101010101010, %rbx
    movabs $0x2020202020202020, %rbp
    movabs $0x3030303030303030, %r12
    movabs $0x4040404040404040, %r13
    movabs $0x5050505050505050, %r14
    movabs $0x6060606060606060, %r15
    LDX %xmm6,  0x3ff0000000000006
    LDX %xmm7,  0x3ff0000000000007
    LDX %xmm8,  0x3ff0000000000008
    LDX %xmm9,  0x3ff0000000000009
    LDX %xmm10, 0x3ff000000000000a
    LDX %xmm11, 0x3ff000000000000b
    LDX %xmm12, 0x3ff000000000000c
    LDX %xmm13, 0x3ff000000000000d
    LDX %xmm14, 0x3ff000000000000e
    LDX %xmm15, 0x3ff000000000000f
    call *%rax
    mov (%rsp), %rcx
    movq %xmm6,  0(%rcx)
    movq %xmm7,  8(%rcx)
    movq %xmm8,  16(%rcx)
    movq %xmm9,  24(%rcx)
    movq %xmm10, 32(%rcx)
    movq %xmm11, 40(%rcx)
    movq %xmm12, 48(%rcx)
    movq %xmm13, 56(%rcx)
    movq %xmm14, 64(%rcx)
    movq %xmm15, 72(%rcx)
    mov %rbx, 80(%rcx)
    mov %rbp, 88(%rcx)
    mov %r12, 96(%rcx)
    mov %r13, 104(%rcx)
    mov %r14, 112(%rcx)
    mov %r15, 120(%rcx)
    add $24, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret
ENDF(guest_invoke_probe)

#ifdef __ELF__
    .section .note.GNU-stack,"",@progbits
#endif
