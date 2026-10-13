/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: platform layer, common definitions.
 *
 * The runtime (src/) and the GPU library's shims (gpu/shim) reach Linux-only or Windows-only
 * facilities through the pf_* functions declared in src/platform/pf_*.h. Each header has one
 * implementation per platform: src/platform/pf_<area>_linux.c (the code that used to live at the
 * call sites, moved without behaviour changes) and src/platform/pf_<area>_win32.c. Code outside
 * src/platform uses POSIX (the subset listed in docs/refueled/INTERFACES.md, which MinGW-w64 and
 * winpthreads provide on Windows) or pf_*, never a Linux-only or Windows-only API, and carries no
 * platform #ifdef.
 *
 * Conventions: functions return 0 or a positive count on success and a negative PF_E* code on
 * failure; addresses are uintptr_t, sizes uint64_t; nothing here allocates on the C heap unless
 * it says so. A function not implemented yet on a platform returns PF_ENOSYS and logs that once
 * (pf_not_implemented). */
#ifndef REFUELED_PF_BASE_H
#define REFUELED_PF_BASE_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define PF_EXTERN_C_BEGIN extern "C" {
#define PF_EXTERN_C_END }
#else
#define PF_EXTERN_C_BEGIN
#define PF_EXTERN_C_END
#endif

/* Attributes. The product compilers are gcc/clang (Linux, MSYS2 CLANG64); the MSVC spellings only
 * let the Win32-only platform files be checked with the Visual Studio tools as well. */
#if defined(_MSC_VER) && !defined(__clang__)
#define PF_NORETURN __declspec(noreturn)
#define PF_RETURNS_TWICE
#define PF_ALIGNED16 __declspec(align(16))
#else
#define PF_NORETURN __attribute__((noreturn))
#define PF_RETURNS_TWICE __attribute__((returns_twice))
#define PF_ALIGNED16 __attribute__((aligned(16)))
#endif

PF_EXTERN_C_BEGIN

/* Error codes: platform-neutral, negative, errno-like values (not the host's errno). */
enum {
    PF_OK = 0,
    PF_EPERM = -1,
    PF_ENOENT = -2,
    PF_EIO = -5,
    PF_EAGAIN = -11,
    PF_ENOMEM = -12,
    PF_EACCES = -13,
    PF_EFAULT = -14,
    PF_EBUSY = -16,
    PF_EEXIST = -17,
    PF_EINVAL = -22,
    PF_ENOSPC = -28,
    PF_ENOSYS = -38,
    PF_ETIMEDOUT = -110,
};

/* Process-wide initialisation, called once by main() before anything else in pf_*:
 * Linux: the glibc malloc settings that keep host objects below 1 TiB (mallopt, formerly in
 * probe.c main). Windows: binary mode for stdio/open, the fault dispatcher (VEH), the TLS slot
 * (pf_tls_init), timer resolution. Returns 0 or PF_E*. */
int pf_init(void);

/* "linux" or "win32". */
const char *pf_platform_name(void);

/* Logs "refueled: <function> not implemented on <platform>" once per function (thread-safe,
 * async-signal-safe) and returns PF_ENOSYS, for stubs: `return pf_not_implemented(__func__);` */
int pf_not_implemented(const char *function);

PF_EXTERN_C_END
#endif
