/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: F7 - SysV ABI verification. Guest-like assembler routines (sysv_caller.S) call
 * host functions declared with ABI and are called back through ABI function pointers.
 * On Linux sysv_abi is the default, so this passes trivially there; on Windows (clang, Win64
 * default ABI) it only passes when the attribute is honored everywhere. Does not include
 * runtime.h; ABI is spelled exactly like in runtime.h. Red zone: package RZ, not tested here. */
#include <stdint.h>
#include <emmintrin.h>

#define ABI __attribute__((sysv_abi))
#ifdef F7_MSABI_TRIAL
/* Optional Linux check: gcc -mabi=ms -fno-builtin -fno-tree-loop-distribute-patterns
 * -DF7_MSABI_TRIAL makes Win64 the default ABI, so a missing ABI shows up as a failure.
 * libc stays SysV, so this file then touches no libc; two ABI helpers (a separate, normally
 * compiled file, see docs/refueled/log/F7.md) print and exit. */
extern ABI void f7_fail(int line, const char *what);
extern ABI void f7_done(int checks);
static void zero(void *p, unsigned long n) { for (unsigned char *b = p; n--; ) *b++ = 0; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void zero(void *p, unsigned long n) { memset(p, 0, n); }
#endif
#define NOINLINE __attribute__((noinline))

static int g_checks;

static void check_at(int ok, const char *what, int line) {
    g_checks++;
    if (!ok) {
#ifdef F7_MSABI_TRIAL
        f7_fail(line, what);
#else
        fprintf(stderr, "test_sysv_abi: FAILED at line %d: %s\n", line, what);
        exit(1);
#endif
    }
}
#define CHECK(c) check_at(!!(c), #c, __LINE__)

typedef struct { uint64_t w[4]; } Big;            /* 32 bytes: MEMORY class, sret */
typedef struct { uint64_t a, b; } Small;          /* 16 bytes: rax:rdx */
typedef struct { double a, b; } DPair;            /* 16 bytes: xmm0:xmm1 */
typedef struct { uint64_t i; double d; } Mixed;   /* 16 bytes: rax:xmm0 */
typedef uint64_t (ABI *GuestFn)(uint64_t);
/* volatile: the compiler must really call through the pointer, not devirtualize it */
static GuestFn volatile g_entry_ptr, g_clobber_ptr;

/* ---- guest routines (sysv_caller.S), all SysV ---- */
ABI uint64_t guest_call_mix(void);
ABI void guest_call_fmix(double *out);
ABI int guest_call_big(uint64_t *out, uint64_t seed);
ABI void guest_call_small(uint64_t x, uint64_t *out);
ABI void guest_call_dpair(double x, double *out);
ABI void guest_call_mixed(uint64_t x, uint64_t *out);
ABI uint64_t guest_call_take(void);
ABI uint64_t guest_call_aligned(uint64_t skew);
ABI uint64_t guest_get_rsp(void);
ABI uint64_t guest_entry_misalign(void);
ABI uint64_t guest_clobber(uint64_t x);
ABI uint64_t guest_invoke_probe(GuestFn fn, uint64_t x, uint64_t *out);

/* ---- (a) host function with 8 integer + 10 double arguments ---- */
static const uint64_t k_int[8] = {
    0x1111111111111111ull, 0x2222222222222222ull, 0x3333333333333333ull, 0x4444444444444444ull,
    0x5555555555555555ull, 0x6666666666666666ull, 0x7777777777777777ull, 0x8888888888888888ull };
static const double k_dbl[10] = { 0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 100.25, 200.75 };

static uint64_t mix_sum(const uint64_t *iv, const double *dv) {
    uint64_t s = 0;
    for (int i = 0; i < 8; i++) s += (uint64_t)(i + 1) * iv[i];
    double d = 0;
    for (int i = 0; i < 10; i++) d += dv[i];
    return s + (uint64_t)(d * 2.0);
}

ABI uint64_t host_mix(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                      uint64_t g, uint64_t h,
                      double x0, double x1, double x2, double x3, double x4, double x5,
                      double x6, double x7, double s0, double s1) {
    const uint64_t iv[8] = { a, b, c, d, e, f, g, h };
    const double dv[10] = { x0, x1, x2, x3, x4, x5, x6, x7, s0, s1 };
    for (int i = 0; i < 8; i++) CHECK(iv[i] == k_int[i]);
    for (int i = 0; i < 10; i++) CHECK(dv[i] == k_dbl[i]);
    return mix_sum(iv, dv);
}

ABI double host_fmix(float f, double d, int32_t i) {
    CHECK(f == 1.5f);
    CHECK(d == 2.25);
    CHECK(i == -3);
    return (double)f + d + (double)i;
}

/* ---- (b) struct returns and struct arguments ---- */
ABI Big host_make_big(uint64_t seed) {
    Big b;
    for (int i = 0; i < 4; i++) b.w[i] = seed * (uint64_t)(i + 1) + (uint64_t)i;
    return b;
}
ABI Small host_make_small(uint64_t x) { Small s = { x + 1, x * 2 }; return s; }
ABI DPair host_make_dpair(double x) { DPair p = { x + 1.0, x * 2.0 }; return p; }
ABI Mixed host_make_mixed(uint64_t x) { Mixed m = { x + 5, (double)x * 0.5 }; return m; }

ABI uint64_t host_take_structs(Big b, Small s, DPair d) {
    CHECK(b.w[0] == 0x1001 && b.w[1] == 0x1002 && b.w[2] == 0x1003 && b.w[3] == 0x1004);
    CHECK(s.a == 0x2001 && s.b == 0x2002);
    CHECK(d.a == 1.25 && d.b == 3.25);
    return b.w[0] + b.w[1] + b.w[2] + b.w[3] + s.a + s.b + (uint64_t)(d.a * 4 + d.b * 4);
}

/* ---- (d) stack alignment seen by a host function ---- */
ABI uint64_t host_check_align(uint64_t guest_rsp) {
    _Alignas(16) double buf[2];
    CHECK(guest_rsp % 16 == 0);                          /* SysV rule before the call */
    CHECK(((uintptr_t)buf) % 16 == 0);
    CHECK(((uintptr_t)__builtin_frame_address(0)) % 16 == 0);
    uint64_t now = guest_get_rsp();                      /* our own rsp at this call site */
    CHECK(now % 16 == 0);
    volatile __m128d one = _mm_set1_pd(1.0);
    _mm_store_pd(buf, one);                              /* movaps: faults when misaligned */
    CHECK(buf[0] == 1.0 && buf[1] == 1.0);
    return 0xA11CE;
}

/* ---- (c) host -> guest callback, xmm6-15 / callee-saved registers ---- */
/* Twelve doubles stay live across the call. Under Win64 the compiler keeps them in xmm6-15
 * (callee-saved there); because the call target is sysv_abi it must save them itself. */
static NOINLINE double live_across_callback(GuestFn fn, volatile double *seed, uint64_t *ret) {
    double s = *seed;
    double a0 = s + 0 * 1.25, a1 = s + 1 * 1.25, a2 = s + 2 * 1.25, a3 = s + 3 * 1.25;
    double a4 = s + 4 * 1.25, a5 = s + 5 * 1.25, a6 = s + 6 * 1.25, a7 = s + 7 * 1.25;
    double a8 = s + 8 * 1.25, a9 = s + 9 * 1.25, a10 = s + 10 * 1.25, a11 = s + 11 * 1.25;
    *ret = fn(7);
    return a0 * 1 + a1 * 2 + a2 * 3 + a3 * 4 + a4 * 5 + a5 * 6 + a6 * 7 + a7 * 8
         + a8 * 9 + a9 * 10 + a10 * 11 + a11 * 12;
}

int main(void) {
    /* (a) */
    uint64_t r = guest_call_mix();
    CHECK(r == mix_sum(k_int, k_dbl));

    double fr = 0;
    guest_call_fmix(&fr);
    CHECK(fr == 0.75);

    /* (b) */
    uint64_t big[4] = { 0, 0, 0, 0 };
    CHECK(guest_call_big(big, 10) == 1);                 /* rax returned the sret pointer */
    for (int i = 0; i < 4; i++) CHECK(big[i] == 10 * (uint64_t)(i + 1) + (uint64_t)i);

    uint64_t sm[2] = { 0, 0 };
    guest_call_small(41, sm);
    CHECK(sm[0] == 42 && sm[1] == 82);

    double dp[2] = { 0, 0 };
    guest_call_dpair(2.5, dp);
    CHECK(dp[0] == 3.5 && dp[1] == 5.0);

    uint64_t mx[2] = { 0, 0 };
    guest_call_mixed(9, mx);
    CHECK(mx[0] == 14);
    { union { uint64_t u; double d; } md; md.u = mx[1]; CHECK(md.d == 4.5); }

    CHECK(guest_call_take() == 0x1001 + 0x1002 + 0x1003 + 0x1004 + 0x2001 + 0x2002 + 5 + 13);

    /* (d) guest -> host with several stack skews */
    for (uint64_t skew = 0; skew < 16; skew++) CHECK(guest_call_aligned(skew) == 0xA11CE);
    /* host -> guest: the entry must see an aligned caller rsp */
    g_entry_ptr = (GuestFn)(void *)guest_entry_misalign;
    GuestFn entry = g_entry_ptr;
    CHECK(entry(0) == 0);

    /* (c) the guest clobbers xmm6-15, preserves rbx, rbp, r12-r15 */
    g_clobber_ptr = guest_clobber;
    GuestFn clob = g_clobber_ptr;
    uint64_t out[16];
    zero(out, sizeof out);
    CHECK(guest_invoke_probe(clob, 5, out) == 16);
    for (int i = 0; i < 10; i++) CHECK(out[i] == 0x7ff8c0dec0dec0deull);   /* really clobbered */
    CHECK(out[10] == 0x1010101010101010ull && out[11] == 0x2020202020202020ull);
    CHECK(out[12] == 0x3030303030303030ull && out[13] == 0x4040404040404040ull);
    CHECK(out[14] == 0x5050505050505050ull && out[15] == 0x6060606060606060ull);

    /* host values in xmm6-15 survive a callback that destroys them */
    volatile double seed = 0.75;
    uint64_t cr = 0;
    double lv = live_across_callback(clob, &seed, &cr);
    double expect = 0;
    for (int i = 0; i < 12; i++) expect += (0.75 + i * 1.25) * (i + 1);
    CHECK(cr == 22);
    CHECK(lv == expect);

#ifdef F7_MSABI_TRIAL
    f7_done(g_checks);
#else
    printf("test_sysv_abi: %d checks passed\n", g_checks);
#endif
    return 0;
}
