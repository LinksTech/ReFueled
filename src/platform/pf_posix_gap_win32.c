/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: implementation of the POSIX gap layer (see pf_posix_gap_win32.h).
 *
 * This file is itself compiled with the gap header force-included, so the macros that map POSIX
 * names to pf_gap_*() are active at the top: they are removed first and the CRT is called through
 * its underscore names (_read, _write, _open, ...). Paths are UTF-8 where a wide Win32 call is
 * needed (rename, O_DIRECTORY open, pf_gap_entry_type); the narrow CRT calls (_open, _mkdir,
 * _access) use the process code page, so the executable manifest should select UTF-8
 * (activeCodePage) for the two to agree. */
#include "pf_posix_gap_win32.h"

#ifdef _WIN32

#ifndef _MSC_VER
/* Drop the C macros of the header; this file calls the real CRT/winpthreads functions. */
#undef open
#undef read
#undef write
#undef pread
#undef pwrite
#undef lseek
#undef fsync
#undef ftruncate
#undef truncate
#undef fseeko
#undef ftello
#undef realpath
#undef mkdir
#undef rename
#undef setenv
#undef unsetenv
#undef timegm
#undef strcasestr
#undef gmtime_r
#undef localtime_r
#undef pthread_condattr_t
#undef pthread_condattr_init
#undef pthread_condattr_destroy
#undef pthread_condattr_setclock
#undef pthread_condattr_getclock
#undef pthread_cond_init
#undef pthread_cond_destroy
#undef pthread_cond_timedwait
#undef pthread_mutex_timedlock
#undef pthread_rwlock_timedrdlock
#undef pthread_rwlock_timedwrlock
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <direct.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

/* ---- helpers ------------------------------------------------------------------------------ */

static int errno_from_win32(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE: return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_WRITE_PROTECT:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return EACCES;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS: return EEXIST;
    case ERROR_NOT_SAME_DEVICE: return EXDEV;
    case ERROR_DIR_NOT_EMPTY: return ENOTEMPTY;
    case ERROR_DIRECTORY: return ENOTDIR;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return ENOSPC;
    case ERROR_INVALID_HANDLE: return EBADF;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY: return ENOMEM;
    case ERROR_BAD_PATHNAME:
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
    case ERROR_FILENAME_EXCED_RANGE: return EINVAL;
    default: return EIO;
    }
}

static void quiet_invalid_parameter(const wchar_t *expression, const wchar_t *function,
                                    const wchar_t *file, unsigned int line, uintptr_t reserved) {
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
}

int pf_gap_init_crt(void) {
    _set_invalid_parameter_handler(quiet_invalid_parameter);
    return 0;
}

static volatile LONG g_crt_ready;
static void crt_once(void) {
    if (g_crt_ready) return;
    pf_gap_init_crt();
    InterlockedExchange(&g_crt_ready, 1);
}

/* The OS handle behind a CRT descriptor, or INVALID_HANDLE_VALUE with errno=EBADF. */
static HANDLE handle_of(int fd) {
    if (fd < 0) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    intptr_t h = _get_osfhandle(fd);
    if (h == -1 || h == -2) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    return (HANDLE)h;
}

/* UTF-8 -> absolute "\\?\" UTF-16 path (LongPathsEnabled is 0 on the target systems); malloc'd,
 * NULL with errno set on failure. Same transformation as pf_fs_win32.c. */
static wchar_t *to_wide_extended(const char *path) {
    if (!path || !*path) { errno = path ? ENOENT : EINVAL; return NULL; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    if (n <= 0) { errno = EINVAL; return NULL; }
    wchar_t *wide = malloc((size_t)n * sizeof(wchar_t));
    if (!wide) { errno = ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n);
    if (wcsncmp(wide, L"\\\\?\\", 4) == 0) return wide;
    DWORD full = GetFullPathNameW(wide, 0, NULL, NULL); /* includes the terminator */
    if (full == 0) { errno = errno_from_win32(GetLastError()); free(wide); return NULL; }
    wchar_t *absolute = malloc((size_t)full * sizeof(wchar_t));
    wchar_t *out = malloc(((size_t)full + 8) * sizeof(wchar_t));
    DWORD got = (absolute && out) ? GetFullPathNameW(wide, full, absolute, NULL) : 0;
    free(wide);
    if (got == 0 || got >= full) {
        errno = (absolute && out) ? EIO : ENOMEM;
        free(absolute);
        free(out);
        return NULL;
    }
    if (absolute[0] == L'\\' && absolute[1] == L'\\') { /* \\server\share -> \\?\UNC\server\share */
        memcpy(out, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
        memcpy(out + 8, absolute + 2, ((size_t)got - 1) * sizeof(wchar_t));
    } else {
        memcpy(out, L"\\\\?\\", 4 * sizeof(wchar_t));
        memcpy(out + 4, absolute, ((size_t)got + 1) * sizeof(wchar_t));
    }
    free(absolute);
    return out;
}

/* ---- block logic -------------------------------------------------------------------------- */

size_t pf_gap_io_chunk(uint64_t remaining) {
    return remaining > PF_GAP_IO_MAX ? (size_t)PF_GAP_IO_MAX : (size_t)remaining;
}

uint64_t pf_gap_io_chunk_count(uint64_t total) {
    return total / PF_GAP_IO_MAX + (total % PF_GAP_IO_MAX != 0);
}

/* ---- open / read / write ------------------------------------------------------------------ */

int pf_gap_open(const char *path, int flags, ...) {
    crt_once();
    int mode = 0;
    if (flags & _O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, int);
        va_end(args);
    }
    if (!path) { errno = EINVAL; return -1; }
    if (flags & PF_GAP_O_DIRECTORY) {
        if ((flags & (_O_WRONLY | _O_RDWR)) || (flags & _O_CREAT)) { errno = EISDIR; return -1; }
        wchar_t *wide = to_wide_extended(path);
        if (!wide) return -1;
        DWORD attributes = GetFileAttributesW(wide);
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            errno = errno_from_win32(GetLastError());
            free(wide);
            return -1;
        }
        if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) { errno = ENOTDIR; free(wide); return -1; }
        HANDLE h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        free(wide);
        if (h == INVALID_HANDLE_VALUE) { errno = errno_from_win32(GetLastError()); return -1; }
        int fd = _open_osfhandle((intptr_t)h, _O_RDONLY | _O_BINARY);
        if (fd < 0) { CloseHandle(h); errno = EMFILE; }
        return fd;
    }
    flags &= ~(_O_TEXT | _O_WTEXT | _O_U16TEXT | _O_U8TEXT);
    flags |= _O_BINARY;
    return _open(path, flags, mode & (_S_IREAD | _S_IWRITE));
}

pf_gap_ssize_t pf_gap_read(int fd, void *buffer, size_t size) {
    crt_once();
    if ((uint64_t)size > (uint64_t)INT64_MAX) { errno = EINVAL; return -1; }
    size_t done = 0;
    while (done < size) {
        size_t chunk = pf_gap_io_chunk(size - done);
        int n = _read(fd, (char *)buffer + done, (unsigned int)chunk);
        if (n < 0) {
            if (done) break; /* report what was read; the next call reports the error again */
            return -1;
        }
        if (n == 0) break;
        done += (size_t)n;
        if ((size_t)n < chunk) break; /* short read (end of file, pipe): like read(2) */
    }
    return (pf_gap_ssize_t)done;
}

pf_gap_ssize_t pf_gap_write(int fd, const void *buffer, size_t size) {
    crt_once();
    if ((uint64_t)size > (uint64_t)INT64_MAX) { errno = EINVAL; return -1; }
    size_t done = 0;
    while (done < size) {
        size_t chunk = pf_gap_io_chunk(size - done);
        int n = _write(fd, (const char *)buffer + done, (unsigned int)chunk);
        if (n < 0) {
            if (done) break;
            return -1;
        }
        if (n == 0) { if (!done) { errno = ENOSPC; return -1; } break; }
        done += (size_t)n;
        if ((size_t)n < chunk) break;
    }
    return (pf_gap_ssize_t)done;
}

/* ---- pread / pwrite ----------------------------------------------------------------------- */

/* ReadFile/WriteFile with an OVERLAPPED offset on a synchronous handle moves the file pointer
 * (NT updates CurrentByteOffset). POSIX pread/pwrite do not, so the pointer is saved and restored.
 * Striped locks (by descriptor) keep concurrent pread/pwrite calls on one descriptor from
 * clobbering each other's saved pointer; a concurrent plain read()/lseek() on the same descriptor
 * can still observe the temporary pointer (documented limitation). */
#define POS_LOCKS 64
static SRWLOCK g_pos_locks[POS_LOCKS]; /* zero-initialised == SRWLOCK_INIT */

typedef enum { POS_READ, POS_WRITE } PosOp;

static pf_gap_ssize_t positional_io(int fd, void *buffer, size_t size, int64_t offset, PosOp op) {
    crt_once();
    if (offset < 0 || (uint64_t)size > (uint64_t)INT64_MAX) { errno = EINVAL; return -1; }
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    if (GetFileType(h) != FILE_TYPE_DISK) { errno = ESPIPE; return -1; }
    if (size == 0) return 0;
    SRWLOCK *lock = &g_pos_locks[(unsigned int)fd % POS_LOCKS];
    AcquireSRWLockExclusive(lock);
    LARGE_INTEGER zero, saved;
    zero.QuadPart = 0;
    DWORD failure = 0;
    size_t done = 0;
    if (!SetFilePointerEx(h, zero, &saved, FILE_CURRENT)) {
        failure = GetLastError();
    } else {
        while (done < size) {
            size_t chunk = pf_gap_io_chunk(size - done);
            OVERLAPPED overlapped;
            memset(&overlapped, 0, sizeof overlapped);
            uint64_t position = (uint64_t)offset + done;
            overlapped.Offset = (DWORD)(position & 0xffffffffu);
            overlapped.OffsetHigh = (DWORD)(position >> 32);
            DWORD transferred = 0;
            BOOL ok = op == POS_READ
                          ? ReadFile(h, (char *)buffer + done, (DWORD)chunk, &transferred, &overlapped)
                          : WriteFile(h, (const char *)buffer + done, (DWORD)chunk, &transferred, &overlapped);
            if (!ok) {
                DWORD e = GetLastError();
                if (!(op == POS_READ && e == ERROR_HANDLE_EOF)) failure = e;
                break;
            }
            if (transferred == 0) break;
            done += transferred;
            if (transferred < chunk) break;
        }
        SetFilePointerEx(h, saved, NULL, FILE_BEGIN);
    }
    ReleaseSRWLockExclusive(lock);
    if (failure && done == 0) { errno = errno_from_win32(failure); return -1; }
    return (pf_gap_ssize_t)done;
}

pf_gap_ssize_t pf_gap_pread(int fd, void *buffer, size_t size, int64_t offset) {
    return positional_io(fd, buffer, size, offset, POS_READ);
}

pf_gap_ssize_t pf_gap_pwrite(int fd, const void *buffer, size_t size, int64_t offset) {
    return positional_io(fd, (void *)buffer, size, offset, POS_WRITE);
}

/* ---- 64-bit seek / truncate --------------------------------------------------------------- */

int64_t pf_gap_lseek(int fd, int64_t offset, int whence) {
    crt_once();
    return (int64_t)_lseeki64(fd, (__int64)offset, whence);
}

int pf_gap_ftruncate(int fd, int64_t length) {
    crt_once();
    if (length < 0) { errno = EINVAL; return -1; }
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = length; /* does not move the file pointer; growing zero-fills */
    if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &info, sizeof info)) {
        errno = errno_from_win32(GetLastError());
        return -1;
    }
    return 0;
}

int pf_gap_truncate(const char *path, int64_t length) {
    crt_once();
    if (!path || length < 0) { errno = EINVAL; return -1; }
    int fd = _open(path, _O_WRONLY | _O_BINARY);
    if (fd < 0) return -1;
    int result = pf_gap_ftruncate(fd, length);
    int saved = errno;
    _close(fd);
    errno = saved;
    return result;
}

int pf_gap_fseeko(FILE *stream, int64_t offset, int whence) {
    return _fseeki64(stream, (__int64)offset, whence);
}

int64_t pf_gap_ftello(FILE *stream) {
    return (int64_t)_ftelli64(stream);
}

/* ---- strcasestr / gmtime_r / localtime_r -------------------------------------------------- */

static int ascii_lower(int c) { return (c >= 'A' && c <= 'Z') ? c + ('a' - 'A') : c; }

char *pf_gap_strcasestr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*n && *h && ascii_lower((unsigned char)*h) == ascii_lower((unsigned char)*n)) {
            h++;
            n++;
        }
        if (!*n) return (char *)haystack;
        if (!*h) return NULL; /* haystack tail shorter than needle */
    }
    return NULL;
}

struct tm *pf_gap_gmtime_r(const time_t *timer, struct tm *result) {
    __time64_t t = (__time64_t)*timer;
    return _gmtime64_s(result, &t) == 0 ? result : NULL;
}

struct tm *pf_gap_localtime_r(const time_t *timer, struct tm *result) {
    __time64_t t = (__time64_t)*timer;
    return _localtime64_s(result, &t) == 0 ? result : NULL;
}

/* ---- fsync -------------------------------------------------------------------------------- */

int pf_gap_fsync(int fd) {
    crt_once();
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    BY_HANDLE_FILE_INFORMATION information;
    if (GetFileInformationByHandle(h, &information) && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return 0; /* a directory descriptor (O_DIRECTORY): nothing to flush on NTFS, FlushFileBuffers would fail */
    return _commit(fd);
}

/* ---- paths -------------------------------------------------------------------------------- */

char *pf_gap_realpath(const char *path, char *resolved) {
    crt_once();
    if (!path || !*path) { errno = path ? ENOENT : EINVAL; return NULL; }
    char *out = _fullpath(resolved, path, resolved ? PATH_MAX : 0); /* NULL buffer: malloc'd, like POSIX */
    if (!out) {
        if (errno == 0) errno = ENOENT;
        return NULL;
    }
    if (_access(out, 0) != 0) { /* POSIX realpath fails for a missing path, _fullpath does not */
        int e = errno ? errno : ENOENT;
        if (!resolved) free(out);
        errno = e;
        return NULL;
    }
    for (char *p = out; *p; ++p)
        if (*p == '\\') *p = '/'; /* the common code splits and compares on '/' */
    if (out[0] && out[1] == ':' && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 'a' + 'A');
    return out;
}

int pf_gap_mkdir(const char *path, int mode) {
    crt_once();
    (void)mode; /* no permission bits on Windows */
    return _mkdir(path);
}

int pf_gap_rename(const char *from, const char *to) {
    crt_once();
    if (!from || !to) { errno = EINVAL; return -1; }
    wchar_t *a = to_wide_extended(from);
    if (!a) return -1;
    wchar_t *b = to_wide_extended(to);
    if (!b) { int e = errno; free(a); errno = e; return -1; }
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DWORD e = ok ? 0 : GetLastError();
    if (!ok && e == ERROR_ACCESS_DENIED) {
        /* A read-only target file is still replaced by rename(2) (it only looks at the directory). */
        DWORD attributes = GetFileAttributesW(b);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) &&
            !(attributes & FILE_ATTRIBUTE_DIRECTORY) &&
            SetFileAttributesW(b, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY)) {
            ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            e = ok ? 0 : GetLastError();
            if (!ok) SetFileAttributesW(b, attributes);
        }
    }
    free(a);
    free(b);
    if (!ok) { errno = errno_from_win32(e); return -1; }
    return 0;
}

unsigned char pf_gap_entry_type(const char *dir, const char *name) {
    if (!dir || !name) return DT_UNKNOWN;
    size_t length = strlen(dir) + strlen(name) + 2;
    char *joined = malloc(length);
    if (!joined) return DT_UNKNOWN;
    snprintf(joined, length, "%s/%s", dir, name);
    wchar_t *wide = to_wide_extended(joined);
    free(joined);
    if (!wide) return DT_UNKNOWN;
    WIN32_FIND_DATAW data;
    HANDLE find = FindFirstFileW(wide, &data);
    free(wide);
    if (find == INVALID_HANDLE_VALUE) return DT_UNKNOWN;
    FindClose(find);
    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        (data.dwReserved0 == IO_REPARSE_TAG_SYMLINK || data.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT))
        return DT_LNK;
    return (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? DT_DIR : DT_REG;
}

/* ---- environment -------------------------------------------------------------------------- */

int pf_gap_setenv(const char *name, const char *value, int overwrite) {
    if (!name || !*name || strchr(name, '=') || !value) { errno = EINVAL; return -1; }
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) == 0 ? 0 : -1; /* _putenv_s also updates the process environment block */
}

int pf_gap_unsetenv(const char *name) {
    if (!name || !*name || strchr(name, '=')) { errno = EINVAL; return -1; }
    return _putenv_s(name, "") == 0 ? 0 : -1; /* an empty value removes the variable */
}

/* ---- pthread: condition variable clock and polling timed locks ---------------------------- */

#ifndef _MSC_VER

typedef struct {
    pthread_cond_t *cond;
    int clock_id;
} CondClock;

static SRWLOCK g_cond_lock; /* zero-initialised == SRWLOCK_INIT */
static CondClock *g_cond_table;
static size_t g_cond_count, g_cond_capacity;

static void cond_forget_locked(pthread_cond_t *cond) {
    for (size_t i = 0; i < g_cond_count; i++) {
        if (g_cond_table[i].cond == cond) {
            g_cond_table[i] = g_cond_table[--g_cond_count];
            return;
        }
    }
}

static int cond_remember(pthread_cond_t *cond, int clock_id) {
    int result = 0;
    AcquireSRWLockExclusive(&g_cond_lock);
    cond_forget_locked(cond); /* a re-initialised object starts from its new attributes */
    if (clock_id != CLOCK_REALTIME) {
        if (g_cond_count == g_cond_capacity) {
            size_t capacity = g_cond_capacity ? g_cond_capacity * 2 : 16;
            CondClock *grown = realloc(g_cond_table, capacity * sizeof *grown);
            if (grown) { g_cond_table = grown; g_cond_capacity = capacity; }
        }
        if (g_cond_count < g_cond_capacity) {
            g_cond_table[g_cond_count].cond = cond;
            g_cond_table[g_cond_count].clock_id = clock_id;
            g_cond_count++;
        } else {
            result = ENOMEM;
        }
    }
    ReleaseSRWLockExclusive(&g_cond_lock);
    return result;
}

static int cond_clock(pthread_cond_t *cond) {
    int clock_id = CLOCK_REALTIME;
    AcquireSRWLockShared(&g_cond_lock);
    for (size_t i = 0; i < g_cond_count; i++) {
        if (g_cond_table[i].cond == cond) { clock_id = g_cond_table[i].clock_id; break; }
    }
    ReleaseSRWLockShared(&g_cond_lock);
    return clock_id;
}

int pf_gap_pthread_condattr_init(pf_gap_condattr_t *attr) {
    if (!attr) return EINVAL;
    int result = pthread_condattr_init(&attr->native);
    attr->clock_id = CLOCK_REALTIME;
    return result;
}

int pf_gap_pthread_condattr_destroy(pf_gap_condattr_t *attr) {
    if (!attr) return EINVAL;
    return pthread_condattr_destroy(&attr->native);
}

int pf_gap_pthread_condattr_setclock(pf_gap_condattr_t *attr, int clock_id) {
    if (!attr || (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC)) return EINVAL;
    attr->clock_id = clock_id;
    return 0;
}

int pf_gap_pthread_condattr_getclock(const pf_gap_condattr_t *attr, int *clock_id) {
    if (!attr || !clock_id) return EINVAL;
    *clock_id = attr->clock_id;
    return 0;
}

int pf_gap_pthread_cond_init(pthread_cond_t *cond, const pf_gap_condattr_t *attr) {
    int result = pthread_cond_init(cond, attr ? &attr->native : NULL);
    if (result) return result;
    result = cond_remember(cond, attr ? attr->clock_id : CLOCK_REALTIME);
    if (result) pthread_cond_destroy(cond);
    return result;
}

int pf_gap_pthread_cond_destroy(pthread_cond_t *cond) {
    AcquireSRWLockExclusive(&g_cond_lock);
    cond_forget_locked(cond);
    ReleaseSRWLockExclusive(&g_cond_lock);
    return pthread_cond_destroy(cond);
}

static int64_t timespec_ns(const struct timespec *t) {
    return (int64_t)t->tv_sec * 1000000000 + (int64_t)t->tv_nsec;
}

int pf_gap_pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *deadline) {
    if (cond_clock(cond) != CLOCK_MONOTONIC) return pthread_cond_timedwait(cond, mutex, deadline);
    struct timespec mono, real, target;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    int64_t left = timespec_ns(deadline) - timespec_ns(&mono);
    if (left < 0) left = 0;
    int64_t total = timespec_ns(&real) + left;
    target.tv_sec = (time_t)(total / 1000000000);
    target.tv_nsec = (long)(total % 1000000000);
    return pthread_cond_timedwait(cond, mutex, &target);
}

static int deadline_passed(const struct timespec *deadline) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return timespec_ns(&now) >= timespec_ns(deadline);
}

int pf_gap_pthread_mutex_timedlock(pthread_mutex_t *mutex, const struct timespec *deadline) {
    for (;;) {
        int result = pthread_mutex_trylock(mutex);
        if (result != EBUSY) return result;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        Sleep(1);
    }
}

int pf_gap_pthread_rwlock_timedrdlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    for (;;) {
        int result = pthread_rwlock_tryrdlock(lock);
        if (result != EBUSY) return result;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        Sleep(1);
    }
}

int pf_gap_pthread_rwlock_timedwrlock(pthread_rwlock_t *lock, const struct timespec *deadline) {
    for (;;) {
        int result = pthread_rwlock_trywrlock(lock);
        if (result != EBUSY) return result;
        if (deadline_passed(deadline)) return ETIMEDOUT;
        Sleep(1);
    }
}

#endif /* !_MSC_VER */

#endif /* _WIN32 */
