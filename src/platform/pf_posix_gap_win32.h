/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: POSIX gap layer for Windows (MinGW-w64 UCRT + winpthreads).
 *
 * Force-included into every Windows translation unit by build-windows.sh
 * (`-include src/platform/pf_posix_gap_win32.h`). Effective only under _WIN32 and not in assembler
 * sources; on every other platform the file is empty. It closes the holes between what the common
 * code uses (docs/refueled/INTERFACES.md section 2) and what MinGW-w64 offers:
 *
 *   open (always O_BINARY, O_CLOEXEC/O_NONBLOCK/O_DIRECTORY), read/write (blocks <= 1 GiB),
 *   pread/pwrite (64-bit offset, file pointer untouched), lseek/ftruncate/truncate/fseeko/ftello
 *   (64-bit), fsync (also on a directory descriptor), realpath, mkdir(path, mode),
 *   rename over an existing target, setenv/unsetenv, timegm,
 *   pthread_condattr_setclock(CLOCK_MONOTONIC) with a matching pthread_cond_timedwait,
 *   d_type replacement pf_gap_entry_type(), DT_* constants.
 *
 * Mechanism: every replacement is a real function pf_gap_*() (src/platform/pf_posix_gap_win32.c)
 * and, in C only, a function-like or object-like macro maps the POSIX name to it. C++ translation
 * units get NO macros (a macro named read/write/rename would break std::istream::read and
 * std::rename); they see only the pf_gap_*() declarations plus static inline forwarders for the
 * names MinGW lacks entirely (pread, pwrite, realpath, setenv, unsetenv; opt out with
 * PF_GAP_NO_CXX_FORWARDERS). All macros can be switched off with PF_GAP_NO_MACROS.
 *
 * NOT provided (see docs/refueled/log/F6.md): aligned_alloc/posix_memalign (UCRT free() cannot
 * release _aligned_malloc memory; use pf_low_aligned_alloc/pf_low_free), tm_gmtoff (a struct field,
 * needs a pf_* function), fstatat/dirfd/lstat, struct dirent::d_type itself.
 *
 * The header also compiles under MSVC (only the pf_gap_*() declarations, no POSIX headers, no
 * macros) so that pf_posix_gap_win32.c can be checked with `cl /c /std:c11 /W4`. */
#ifndef REFUELED_PF_POSIX_GAP_WIN32_H
#define REFUELED_PF_POSIX_GAP_WIN32_H

#if defined(_WIN32) && !defined(__ASSEMBLER__)

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
#define PF_GAP_BEGIN_DECLS extern "C" {
#define PF_GAP_END_DECLS }
#define PF_GAP_STATIC_ASSERT(cond, message) static_assert(cond, message)
#else
#define PF_GAP_BEGIN_DECLS
#define PF_GAP_END_DECLS
#define PF_GAP_STATIC_ASSERT(cond, message) _Static_assert(cond, message)
#endif

#ifdef _USE_32BIT_TIME_T
#error "refueled: _USE_32BIT_TIME_T must not be defined (guest time values are 64 bit)"
#endif

#ifndef _MSC_VER /* ------------------------------------------------------------- MinGW-w64 ---- */
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <limits.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>

PF_GAP_STATIC_ASSERT(sizeof(void *) == 8, "refueled: 64-bit target required");
PF_GAP_STATIC_ASSERT(sizeof(time_t) == 8, "refueled: time_t must be 64 bit (no _USE_32BIT_TIME_T)");
#ifndef PF_GAP_NO_OFF_T_ASSERT
PF_GAP_STATIC_ASSERT(sizeof(off_t) == 8, "refueled: define _FILE_OFFSET_BITS=64 before the first include");
#endif

/* Self-sufficient fallbacks: strict -std=c11 may hide the POSIX spellings in the MinGW headers. */
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
#ifndef O_RDONLY
#define O_RDONLY _O_RDONLY
#endif
#ifndef O_WRONLY
#define O_WRONLY _O_WRONLY
#endif
#ifndef O_RDWR
#define O_RDWR _O_RDWR
#endif
#ifndef O_APPEND
#define O_APPEND _O_APPEND
#endif
#ifndef O_CREAT
#define O_CREAT _O_CREAT
#endif
#ifndef O_TRUNC
#define O_TRUNC _O_TRUNC
#endif
#ifndef O_EXCL
#define O_EXCL _O_EXCL
#endif
#ifndef O_BINARY
#define O_BINARY _O_BINARY
#endif
#ifndef O_TEXT
#define O_TEXT _O_TEXT
#endif
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#endif
#endif /* !_MSC_VER */

/* Flags without a Windows form. O_CLOEXEC is the CRT's "do not inherit" flag; O_NONBLOCK is
 * accepted and ignored; O_DIRECTORY is a private bit: pf_gap_open() then opens the directory
 * (read only) so that fsync() on it succeeds as a no-op, any non-directory fails with ENOTDIR. */
#define PF_GAP_O_DIRECTORY 0x01000000
#ifndef _MSC_VER
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY PF_GAP_O_DIRECTORY
#endif
#endif

/* winpthreads has no coarse wall clock; the exact one is a valid substitute. */
#if !defined(_MSC_VER) && !defined(CLOCK_REALTIME_COARSE)
#define CLOCK_REALTIME_COARSE CLOCK_REALTIME
#endif

/* d_type values (Linux numbering, as the common code maps them to the guest). */
#ifndef DT_UNKNOWN
#define DT_UNKNOWN 0
#endif
#ifndef DT_DIR
#define DT_DIR 4
#endif
#ifndef DT_REG
#define DT_REG 8
#endif
#ifndef DT_LNK
#define DT_LNK 10
#endif

PF_GAP_BEGIN_DECLS

/* Same type as ssize_t on Win64 (long long); spelled out so the MSVC check needs no POSIX header. */
typedef ptrdiff_t pf_gap_ssize_t;

/* Largest single CRT/Win32 transfer (UCRT _read/_write take unsigned int, ReadFile a DWORD). */
#define PF_GAP_IO_MAX ((uint64_t)0x40000000u)

/* Block logic of read/write/pread/pwrite, exposed for the unit test: size of the next block for
 * `remaining` bytes (min(remaining, 1 GiB)) and the number of blocks for `total` bytes. */
size_t pf_gap_io_chunk(uint64_t remaining);
uint64_t pf_gap_io_chunk_count(uint64_t total);

/* Installs a CRT invalid-parameter handler that returns, so that _get_osfhandle/_commit/_read on a
 * bad descriptor give errno=EBADF instead of terminating the process. Every pf_gap_* entry point
 * calls this once itself; the common code's plain CRT calls are only covered after the first
 * pf_gap_* call or an explicit call here (candidate for pf_init). Returns 0. */
int pf_gap_init_crt(void);

int pf_gap_open(const char *path, int flags, ...);
pf_gap_ssize_t pf_gap_read(int fd, void *buffer, size_t size);
pf_gap_ssize_t pf_gap_write(int fd, const void *buffer, size_t size);
pf_gap_ssize_t pf_gap_pread(int fd, void *buffer, size_t size, int64_t offset);
pf_gap_ssize_t pf_gap_pwrite(int fd, const void *buffer, size_t size, int64_t offset);
int64_t pf_gap_lseek(int fd, int64_t offset, int whence);
int pf_gap_fsync(int fd);
int pf_gap_ftruncate(int fd, int64_t length);
int pf_gap_truncate(const char *path, int64_t length);
int pf_gap_fseeko(FILE *stream, int64_t offset, int whence);
int64_t pf_gap_ftello(FILE *stream);
char *pf_gap_realpath(const char *path, char *resolved);
int pf_gap_mkdir(const char *path, int mode);
int pf_gap_rename(const char *from, const char *to);
int pf_gap_setenv(const char *name, const char *value, int overwrite);
int pf_gap_unsetenv(const char *name);

/* d_type replacement: type of the entry `name` inside directory `dir` (both UTF-8): DT_DIR, DT_REG,
 * DT_LNK (symbolic link or junction, not followed) or DT_UNKNOWN. Needs one FindFirstFileW. */
unsigned char pf_gap_entry_type(const char *dir, const char *name);

#ifndef _MSC_VER
/* pthread: winpthreads has no pthread_condattr_setclock, and its pthread_cond_timedwait takes an
 * absolute CLOCK_REALTIME deadline. The wrappers remember the clock chosen at pthread_cond_init
 * (side table keyed by the condition variable) and convert a CLOCK_MONOTONIC deadline to the
 * equivalent CLOCK_REALTIME one (now_real + (deadline - now_mono)) before the real call. */
typedef struct {
    pthread_condattr_t native;
    int clock_id;
} pf_gap_condattr_t;

int pf_gap_pthread_condattr_init(pf_gap_condattr_t *attr);
int pf_gap_pthread_condattr_destroy(pf_gap_condattr_t *attr);
int pf_gap_pthread_condattr_setclock(pf_gap_condattr_t *attr, int clock_id);
int pf_gap_pthread_condattr_getclock(const pf_gap_condattr_t *attr, int *clock_id);
int pf_gap_pthread_cond_init(pthread_cond_t *cond, const pf_gap_condattr_t *attr);
int pf_gap_pthread_cond_destroy(pthread_cond_t *cond);
int pf_gap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline);

/* Polling fallbacks for a winpthreads without the timed lock functions (every call: trylock plus
 * Sleep(1) until the CLOCK_REALTIME deadline). Mapped only with -DPF_GAP_POLL_TIMEDLOCKS. */
int pf_gap_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline);
int pf_gap_pthread_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline);
int pf_gap_pthread_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline);
#endif /* !_MSC_VER */

PF_GAP_END_DECLS

#if !defined(_MSC_VER) && !defined(PF_GAP_NO_MACROS)
#ifndef __cplusplus
/* ---- C only: POSIX spellings -> gap functions (defined after every system header above) ---- */
#define open(...) pf_gap_open(__VA_ARGS__)
#define read(fd, buffer, size) pf_gap_read((fd), (buffer), (size))
#define write(fd, buffer, size) pf_gap_write((fd), (buffer), (size))
#define pread(fd, buffer, size, offset) pf_gap_pread((fd), (buffer), (size), (offset))
#define pwrite(fd, buffer, size, offset) pf_gap_pwrite((fd), (buffer), (size), (offset))
#define lseek(fd, offset, whence) pf_gap_lseek((fd), (offset), (whence))
#define fsync(fd) pf_gap_fsync(fd)
#define ftruncate(fd, length) pf_gap_ftruncate((fd), (length))
#define truncate(path, length) pf_gap_truncate((path), (length))
#define fseeko(stream, offset, whence) pf_gap_fseeko((stream), (offset), (whence))
#define ftello(stream) pf_gap_ftello(stream)
#define realpath(path, resolved) pf_gap_realpath((path), (resolved))
#define mkdir(path, mode) pf_gap_mkdir((path), (mode))
#define rename(from, to) pf_gap_rename((from), (to))
#define setenv(name, value, overwrite) pf_gap_setenv((name), (value), (overwrite))
#define unsetenv(name) pf_gap_unsetenv(name)
#define timegm _mkgmtime
/* Names UCRT spells with an underscore (strict -std=c11 may hide the POSIX ones). */
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#ifndef strdup
#define strdup _strdup
#endif

#ifndef PF_GAP_NO_COND_SHIM
#define pthread_condattr_t pf_gap_condattr_t
#define pthread_condattr_init pf_gap_pthread_condattr_init
#define pthread_condattr_destroy pf_gap_pthread_condattr_destroy
#define pthread_condattr_setclock pf_gap_pthread_condattr_setclock
#define pthread_condattr_getclock pf_gap_pthread_condattr_getclock
#define pthread_cond_init pf_gap_pthread_cond_init
#define pthread_cond_destroy pf_gap_pthread_cond_destroy
#define pthread_cond_timedwait pf_gap_pthread_cond_timedwait
#endif
#ifdef PF_GAP_POLL_TIMEDLOCKS
#define pthread_mutex_timedlock pf_gap_pthread_mutex_timedlock
#define pthread_rwlock_timedrdlock pf_gap_pthread_rwlock_timedrdlock
#define pthread_rwlock_timedwrlock pf_gap_pthread_rwlock_timedwrlock
#endif
#elif !defined(PF_GAP_NO_CXX_FORWARDERS)
/* ---- C++: only the names MinGW-w64 lacks completely ---- */
static inline pf_gap_ssize_t pread(int fd, void *buffer, size_t size, off_t offset) {
    return pf_gap_pread(fd, buffer, size, (int64_t)offset);
}
static inline pf_gap_ssize_t pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    return pf_gap_pwrite(fd, buffer, size, (int64_t)offset);
}
static inline char *realpath(const char *path, char *resolved) { return pf_gap_realpath(path, resolved); }
static inline int setenv(const char *name, const char *value, int overwrite) {
    return pf_gap_setenv(name, value, overwrite);
}
static inline int unsetenv(const char *name) { return pf_gap_unsetenv(name); }
#endif
#endif /* !_MSC_VER && !PF_GAP_NO_MACROS */

#endif /* _WIN32 && !__ASSEMBLER__ */
#endif /* REFUELED_PF_POSIX_GAP_WIN32_H */
