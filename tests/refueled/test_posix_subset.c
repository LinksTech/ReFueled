/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: test of the POSIX subset the common code may use (docs/refueled/INTERFACES.md section 2)
 * and of the Windows gap layer (src/platform/pf_posix_gap_win32.h/.c). Written plattform-neutral:
 * the same source runs against glibc on Linux and, with the gap header force-included, against
 * MinGW-w64 (UCRT) + winpthreads on Windows. Checks marked "(P)" are the open points of section 2.
 * Work files live in <tmp>/posix_subset_work, <tmp> = $PF_TEST_TMPDIR, $TMPDIR, $TMP, $TEMP or ".".
 * The big-file part grows a scratch file to 4 GiB + 16 bytes (sparse or lazily allocated) and
 * deletes it again. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <strings.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#endif

static int g_checks, g_failed;
#define CHECK(cond) \
    do { \
        g_checks++; \
        if (!(cond)) { \
            g_failed++; \
            fprintf(stderr, "FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #cond, errno); \
        } \
    } while (0)
#define INFO(...) do { printf("INFO "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* Types: 64-bit off_t/time_t/ssize_t is a precondition of the whole port (LLP64.md 25, 29). */
_Static_assert(sizeof(off_t) == 8, "off_t must be 64 bit (_FILE_OFFSET_BITS=64)");
_Static_assert(sizeof(time_t) == 8, "time_t must be 64 bit");
_Static_assert(sizeof(ssize_t) == 8, "ssize_t must be 64 bit");
_Static_assert(sizeof(void *) == 8, "64-bit target");

#ifdef _WIN32
#define ENTRY_TYPE(dir, entry) pf_gap_entry_type((dir), (entry)->d_name)
#else
#define ENTRY_TYPE(dir, entry) ((entry)->d_type)
#endif

/* ---- helpers ------------------------------------------------------------------------------ */

static char W[600];

static void path_of(char *out, size_t size, const char *name) {
    snprintf(out, size, "%s/%s", W, name);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + (int64_t)(t.tv_nsec / 1000000);
}

static struct timespec after_ms(int clock_id, long ms) {
    struct timespec t;
    clock_gettime(clock_id, &t);
    t.tv_sec += ms / 1000;
    t.tv_nsec += (ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    return t;
}

static int exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

static int write_file(const char *p, const char *text) {
    FILE *f = fopen(p, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static int read_file(const char *p, char *out, size_t size) {
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return 1;
}

static void cleanup(void) {
    static const char *const files[] = {"f.bin", "big.bin", "a.txt", "b.txt", "c.txt", "sub/in.txt", "uA.txt", "uB.txt"};
    static const char *const dirs[] = {"sub/deeper", "sub", "d1"};
    char p[700];
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) { path_of(p, sizeof p, files[i]); unlink(p); }
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) { path_of(p, sizeof p, dirs[i]); rmdir(p); }
    rmdir(W);
}

/* ---- types, ids ---------------------------------------------------------------------------- */

static void test_basics(void) {
    pid_t pid = getpid();
    CHECK(pid > 0);
    CHECK(sched_yield() == 0);
    CHECK(time(NULL) > 1500000000);
}

/* ---- block logic (Windows only: the functions exist in the gap layer) ---------------------- */

#ifdef _WIN32
static void test_chunks(void) {
    CHECK(PF_GAP_IO_MAX == 0x40000000u);
    CHECK(pf_gap_io_chunk(0) == 0);
    CHECK(pf_gap_io_chunk(5) == 5);
    CHECK(pf_gap_io_chunk(0x40000000u) == 0x40000000u);
    CHECK(pf_gap_io_chunk(0x40000001u) == 0x40000000u);
    CHECK(pf_gap_io_chunk(0x100000001ull) == 0x40000000u);
    CHECK(pf_gap_io_chunk_count(0) == 0);
    CHECK(pf_gap_io_chunk_count(1) == 1);
    CHECK(pf_gap_io_chunk_count(0x40000000u) == 1);
    CHECK(pf_gap_io_chunk_count(0x40000001u) == 2);
    CHECK(pf_gap_io_chunk_count(0x100000001ull) == 5);
    /* walk the sequence the read/write loops would take for 0x1'0000'0001 bytes */
    uint64_t remaining = 0x100000001ull, sum = 0, blocks = 0;
    int ok = 1;
    while (remaining) {
        size_t chunk = pf_gap_io_chunk(remaining);
        if (chunk == 0 || chunk > 0x40000000u) { ok = 0; break; }
        sum += chunk;
        remaining -= chunk;
        blocks++;
    }
    CHECK(ok && sum == 0x100000001ull && blocks == 5);
    /* a size above SSIZE_MAX is rejected before any transfer */
    char c;
    errno = 0;
    CHECK(read(0, &c, (size_t)-1) == -1 && errno == EINVAL);
    errno = 0;
    CHECK(write(1, &c, (size_t)-1) == -1 && errno == EINVAL);
}
#endif

/* ---- files: open/read/write/lseek/pread/pwrite/ftruncate/truncate/fsync/fseeko ------------- */

static void test_files(void) {
    char p[700], buf[64];
    path_of(p, sizeof p, "f.bin");

    int flags = O_RDWR | O_CREAT | O_TRUNC;
#ifdef _WIN32
    flags |= O_TEXT; /* the gap layer must still open in binary mode */
#endif
    int fd = open(p, flags, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(write(fd, "a\nb\n", 4) == 4);
    CHECK(lseek(fd, 0, SEEK_END) == 4); /* text mode would have stored 6 bytes */
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    memset(buf, 0, sizeof buf);
    CHECK(read(fd, buf, 4) == 4 && memcmp(buf, "a\nb\n", 4) == 0);
    CHECK(read(fd, buf, 4) == 0); /* end of file */
    CHECK(read(fd, buf, 0) == 0);
    CHECK(write(fd, buf, 0) == 0);

    /* pread/pwrite: positional, file pointer unchanged */
    CHECK(lseek(fd, 3, SEEK_SET) == 3);
    memset(buf, 0, sizeof buf);
    CHECK(pread(fd, buf, 4, 0) == 4 && memcmp(buf, "a\nb\n", 4) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 3);
    CHECK(pwrite(fd, "XY", 2, 1) == 2);
    CHECK(lseek(fd, 0, SEEK_CUR) == 3);
    CHECK(pread(fd, buf, 4, 0) == 4 && memcmp(buf, "aXY\n", 4) == 0);
    CHECK(pread(fd, buf, 4, 2) == 2 && memcmp(buf, "Y\n", 2) == 0); /* short read at the end */
    CHECK(pread(fd, buf, 4, 100) == 0);                             /* past the end */
    CHECK(pwrite(fd, "!", 1, 6) == 1);                              /* beyond the end: zero fill */
    CHECK(lseek(fd, 0, SEEK_END) == 7);
    CHECK(pread(fd, buf, 7, 0) == 7 && memcmp(buf, "aXY\n\0\0!", 7) == 0);
    errno = 0;
    CHECK(pread(fd, buf, 1, -1) == -1 && errno == EINVAL);

    /* fsync on a file and on a bad descriptor */
    CHECK(fsync(fd) == 0);
#ifdef _WIN32
    pf_gap_init_crt();
#endif
    errno = 0;
    CHECK(fsync(9999) == -1 && errno == EBADF);
    errno = 0;
    CHECK(pread(9999, buf, 1, 0) == -1 && errno == EBADF);
    close(fd);

    /* truncate(path) shrinks and grows; ftruncate does not move the file pointer */
    CHECK(truncate(p, 3) == 0);
    struct stat st;
    CHECK(stat(p, &st) == 0 && st.st_size == 3);
    fd = open(p, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(lseek(fd, 1, SEEK_SET) == 1);
    CHECK(ftruncate(fd, 10) == 0);
    CHECK(lseek(fd, 0, SEEK_CUR) == 1);
    CHECK(lseek(fd, 0, SEEK_END) == 10);
    close(fd);

    /* O_DIRECTORY: opening a directory, fsync on it (runtime_file.c: commit), ENOTDIR for a file */
    fd = open(W, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        CHECK(fsync(fd) == 0);
        close(fd);
    }
    errno = 0;
    CHECK(open(p, O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR);
    errno = 0;
    CHECK(open("no/such/dir/file", O_RDONLY) == -1 && errno == ENOENT);
    fd = open(p, O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);

    /* fseeko/ftello */
    FILE *f = fopen(p, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fseeko(f, 2, SEEK_SET) == 0 && ftello(f) == 2);
        CHECK(fseeko(f, 0, SEEK_END) == 0 && ftello(f) == 10);
        fclose(f);
    }
    unlink(p);
}

/* Offsets above 2 GiB and 4 GiB through lseek/ftruncate/pread/pwrite/stat/fseeko (LLP64.md 25, 26;
 * open question 1: which stat variant does _FILE_OFFSET_BITS=64 select?). */
static void test_big_file(void) {
    char p[700], buf[8];
    path_of(p, sizeof p, "big.bin");
    const long long small = 0x80000010LL, big = 0x100000010LL;
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd < 0) return;
    CHECK(ftruncate(fd, (off_t)small) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == small);
    CHECK(lseek(fd, 0x80000000LL, SEEK_SET) == 0x80000000LL);
    CHECK(write(fd, "Q", 1) == 1);
    CHECK(lseek(fd, 0, SEEK_CUR) == 0x80000001LL);
    CHECK(pread(fd, buf, 1, 0x80000000LL) == 1 && buf[0] == 'Q');
    CHECK(pread(fd, buf, 1, 0x7fffffffLL) == 1 && buf[0] == 0);

    CHECK(ftruncate(fd, (off_t)big) == 0);
    CHECK(lseek(fd, 0, SEEK_END) == big);
    CHECK(lseek(fd, 0x80000001LL, SEEK_SET) == 0x80000001LL); /* the SEEK_END above moved the pointer */
    CHECK(pwrite(fd, "Z", 1, 0x100000000LL) == 1);
    CHECK(pread(fd, buf, 1, 0x100000000LL) == 1 && buf[0] == 'Z');
    CHECK(pread(fd, buf, 1, 0x80000000LL) == 1 && buf[0] == 'Q');
    CHECK(lseek(fd, 0, SEEK_CUR) == 0x80000001LL); /* pwrite/pread left it alone */

    struct stat st;
    CHECK(fstat(fd, &st) == 0);
    INFO("sizeof(st_size)=%llu sizeof(st_mtime)=%llu sizeof(st_ino)=%llu sizeof(st_mode)=%llu",
         (unsigned long long)sizeof st.st_size, (unsigned long long)sizeof st.st_mtime,
         (unsigned long long)sizeof st.st_ino, (unsigned long long)sizeof st.st_mode);
    CHECK(sizeof st.st_size == 8);
    CHECK((long long)st.st_size == big);
    close(fd);
    CHECK(stat(p, &st) == 0 && (long long)st.st_size == big);
    CHECK(S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode));

    FILE *f = fopen(p, "rb");
    CHECK(f != NULL);
    if (f) {
        CHECK(fseeko(f, (off_t)0x80000008LL, SEEK_SET) == 0);
        CHECK(ftello(f) == 0x80000008LL);
        CHECK(fseeko(f, 0, SEEK_END) == 0 && ftello(f) == big);
        fclose(f);
    }
    CHECK(truncate(p, 16) == 0);
    CHECK(stat(p, &st) == 0 && st.st_size == 16);
    unlink(p);
}

/* ---- directories, mkdir, rename, realpath, d_type ------------------------------------------ */

static void test_directories(void) {
    char p[700], q[700], text[64];

    path_of(p, sizeof p, "sub");
    CHECK(mkdir(p, 0755) == 0);
    errno = 0;
    CHECK(mkdir(p, 0755) == -1 && errno == EEXIST); /* runtime_savedata.c relies on EEXIST */
    path_of(q, sizeof q, "d1/x/y");
    errno = 0;
    CHECK(mkdir(q, 0755) == -1 && errno == ENOENT);
    path_of(q, sizeof q, "sub/deeper");
    CHECK(mkdir(q, 0755) == 0);
    struct stat st;
    CHECK(stat(q, &st) == 0 && S_ISDIR(st.st_mode));

    /* rename: onto an existing target, missing source, same name */
    char a[700], b[700];
    path_of(a, sizeof a, "a.txt");
    path_of(b, sizeof b, "b.txt");
    CHECK(write_file(a, "from-a"));
    CHECK(write_file(b, "old-b"));
    CHECK(rename(a, b) == 0);
    CHECK(!exists(a));
    CHECK(read_file(b, text, sizeof text) && strcmp(text, "from-a") == 0);
    errno = 0;
    CHECK(rename(a, b) == -1 && errno == ENOENT);
    CHECK(rename(b, b) == 0 && exists(b));

    /* the atomic-save pattern of runtime_savedata.c: write temp, fsync, rename over the old file */
    {
        char t[700];
        path_of(t, sizeof t, "c.txt");
        CHECK(write_file(b, "generation-1"));
        int fd = open(t, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        CHECK(fd >= 0);
        if (fd >= 0) {
            CHECK(write(fd, "generation-2", 12) == 12);
            CHECK(fsync(fd) == 0);
            close(fd);
        }
        CHECK(rename(t, b) == 0);
        CHECK(read_file(b, text, sizeof text) && strcmp(text, "generation-2") == 0);
        CHECK(!exists(t));
    }

#ifdef _WIN32
    /* UTF-8 names reach the wide API: create with _wfopen, rename with the UTF-8 gap function. */
    {
        char u1[700], u2[700];
        wchar_t w1[700], w2[700];
        snprintf(u1, sizeof u1, "%s/u\xC3\xA4.txt", W);
        snprintf(u2, sizeof u2, "%s/v\xC3\xA4.txt", W);
        CHECK(MultiByteToWideChar(CP_UTF8, 0, u1, -1, w1, 700) > 0);
        CHECK(MultiByteToWideChar(CP_UTF8, 0, u2, -1, w2, 700) > 0);
        FILE *f = _wfopen(w1, L"wb");
        CHECK(f != NULL);
        if (f) {
            fputs("utf8", f);
            fclose(f);
            CHECK(rename(u1, u2) == 0);
            CHECK(_waccess(w2, 0) == 0 && _waccess(w1, 0) != 0);
            _wunlink(w2);
        }
    }
    /* a read-only target is replaced as rename(2) does */
    {
        char t[700];
        path_of(t, sizeof t, "c.txt");
        CHECK(write_file(t, "new"));
        CHECK(_chmod(b, _S_IREAD) == 0);
        CHECK(rename(t, b) == 0);
        CHECK(read_file(b, text, sizeof text) && strcmp(text, "new") == 0);
        _chmod(b, _S_IREAD | _S_IWRITE);
    }
#endif

    /* realpath: existing path (buffer and NULL buffer), normalisation, missing path */
    char full[PATH_MAX + 64];
    char *r = realpath(b, full);
    CHECK(r == full);
    if (r) {
        CHECK(strchr(full, '\\') == NULL); /* separators are '/' on both platforms */
#ifdef _WIN32
        CHECK(full[1] == ':' && full[2] == '/');
#else
        CHECK(full[0] == '/');
#endif
        size_t n = strlen(full);
        CHECK(n > 5 && strcmp(full + n - 5, "b.txt") == 0);
    }
    char *m = realpath(W, NULL);
    CHECK(m != NULL);
    if (m) {
        char dotted[700];
        snprintf(dotted, sizeof dotted, "%s/sub/../sub/deeper/..", W);
        char *n2 = realpath(dotted, NULL);
        CHECK(n2 != NULL);
        if (n2) {
            size_t len = strlen(m);
            CHECK(strncmp(n2, m, len) == 0 && strcmp(n2 + len, "/sub") == 0);
            free(n2);
        }
        free(m);
    }
    path_of(q, sizeof q, "no-such-file");
    errno = 0;
    CHECK(realpath(q, NULL) == NULL && errno == ENOENT);

    /* opendir/readdir/closedir, d_name, d_type replacement */
    path_of(q, sizeof q, "sub/in.txt");
    CHECK(write_file(q, "x"));
    path_of(p, sizeof p, "sub");
    DIR *d = opendir(p);
    CHECK(d != NULL);
    int saw_file = 0, saw_dir = 0, saw_dots = 0, total = 0;
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            total++;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) { saw_dots++; continue; }
            if (!strcmp(e->d_name, "in.txt")) saw_file = ENTRY_TYPE(p, e) == DT_REG;
            if (!strcmp(e->d_name, "deeper")) saw_dir = ENTRY_TYPE(p, e) == DT_DIR;
        }
        CHECK(closedir(d) == 0);
    }
    CHECK(saw_file && saw_dir);
    CHECK(total >= 2);
    (void)saw_dots; /* "." and ".." are listed by glibc and MinGW alike, but the code skips them */
    errno = 0;
    CHECK(opendir("no/such/dir") == NULL && errno == ENOENT);

    path_of(q, sizeof q, "sub/in.txt");
    CHECK(unlink(q) == 0);
    errno = 0;
    CHECK(unlink(q) == -1 && errno == ENOENT);
    path_of(q, sizeof q, "sub/deeper");
    CHECK(rmdir(q) == 0);
    path_of(q, sizeof q, "sub");
    CHECK(rmdir(q) == 0);
    CHECK(access(W, 0) == 0);
    path_of(q, sizeof q, "sub");
    errno = 0;
    CHECK(access(q, 0) == -1 && errno == ENOENT);
    unlink(b);
}

/* ---- environment, strings, time ------------------------------------------------------------ */

static void test_environment(void) {
    CHECK(setenv("PF_POSIX_SUBSET", "one", 1) == 0);
    CHECK(getenv("PF_POSIX_SUBSET") && strcmp(getenv("PF_POSIX_SUBSET"), "one") == 0);
    CHECK(setenv("PF_POSIX_SUBSET", "two", 0) == 0); /* no overwrite */
    CHECK(strcmp(getenv("PF_POSIX_SUBSET"), "one") == 0);
    CHECK(setenv("PF_POSIX_SUBSET", "two", 1) == 0);
    CHECK(strcmp(getenv("PF_POSIX_SUBSET"), "two") == 0);
    CHECK(unsetenv("PF_POSIX_SUBSET") == 0);
    CHECK(getenv("PF_POSIX_SUBSET") == NULL);
    errno = 0;
    CHECK(setenv("A=B", "x", 1) == -1 && errno == EINVAL);
}

static void test_strings(void) {
    CHECK(strcasecmp("AbC", "aBc") == 0);
    CHECK(strcasecmp("abc", "abd") < 0);
    CHECK(strncasecmp("HELLO world", "hello there", 5) == 0);
    CHECK(strncasecmp("HELLO world", "hello there", 7) != 0);
    CHECK(strnlen("abcdef", 3) == 3 && strnlen("ab", 10) == 2);
    char *copy = strdup("duplicate");
    CHECK(copy != NULL && strcmp(copy, "duplicate") == 0);
    free(copy);
}

static void test_time(void) {
    struct timespec a, b, real;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &b) == 0);
    CHECK(b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
    CHECK(a.tv_nsec >= 0 && a.tv_nsec < 1000000000L);
    CHECK(clock_gettime(CLOCK_REALTIME, &real) == 0);
    CHECK(real.tv_sec > 1500000000);
    /* all four clocks of runtime_kernel.c host_clock(); COARSE may be an alias of REALTIME */
    struct timespec c1, c2;
    CHECK(clock_gettime(CLOCK_REALTIME_COARSE, &c1) == 0 && c1.tv_sec > 1500000000);
    CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &c1) == 0);
    CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c2) == 0);
    CHECK(c1.tv_nsec >= 0 && c1.tv_nsec < 1000000000L && c2.tv_nsec >= 0 && c2.tv_nsec < 1000000000L);
    CHECK(c2.tv_sec > c1.tv_sec || (c2.tv_sec == c1.tv_sec && c2.tv_nsec >= c1.tv_nsec)); /* process >= thread */

    int64_t t0 = now_ms();
    struct timespec nap = {0, 30 * 1000000L};
    CHECK(nanosleep(&nap, NULL) == 0);
    int64_t t1 = now_ms();
    CHECK(t1 - t0 >= 20 && t1 - t0 < 3000);
    usleep(30000);
    int64_t t2 = now_ms();
    CHECK(t2 - t1 >= 20 && t2 - t1 < 3000);

    /* localtime_r/gmtime_r/timegm: 1971-01-01 00:00:00 UTC. tm_gmtoff is NOT used here: it does
     * not exist in the UCRT struct tm (needs pf_* in the common code, see log F6). */
    time_t day = (time_t)365 * 86400;
    struct tm utc, local;
    CHECK(gmtime_r(&day, &utc) == &utc);
    CHECK(utc.tm_year == 71 && utc.tm_mon == 0 && utc.tm_mday == 1 && utc.tm_hour == 0);
    CHECK(timegm(&utc) == day);
    CHECK(localtime_r(&day, &local) == &local);
    time_t big = (time_t)4102444800LL; /* 2100-01-01: time_t really is 64 bit */
    CHECK(gmtime_r(&big, &utc) == &utc && utc.tm_year == 200);
}

/* ---- pthread ------------------------------------------------------------------------------- */

static void *return_arg(void *arg) { return arg; }

static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static int g_once_count;
static void once_body(void) { g_once_count++; }

static volatile int g_detached_done;
static void *detached_body(void *arg) {
    (void)arg;
    g_detached_done = 1;
    return NULL;
}

static void *exit_body(void *arg) {
    (void)arg;
    pthread_exit((void *)(uintptr_t)77);
    return NULL;
}

static pthread_t g_main_thread;
static volatile int g_self_ok;
static void *self_body(void *arg) {
    (void)arg;
    g_self_ok = !pthread_equal(pthread_self(), g_main_thread) && pthread_equal(pthread_self(), pthread_self());
    return NULL;
}

static void test_pthread_basic(void) {
    void *result = NULL;
    pthread_t t;
    CHECK(pthread_create(&t, NULL, return_arg, (void *)(uintptr_t)42) == 0);
    CHECK(pthread_join(t, &result) == 0 && (uintptr_t)result == 42);

    g_main_thread = pthread_self();
    CHECK(pthread_create(&t, NULL, self_body, NULL) == 0);
    CHECK(pthread_join(t, NULL) == 0 && g_self_ok);

    CHECK(pthread_create(&t, NULL, exit_body, NULL) == 0);
    CHECK(pthread_join(t, &result) == 0 && (uintptr_t)result == 77);

    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setstacksize(&attr, 1u << 20) == 0);
    struct sched_param param;
    memset(&param, 0, sizeof param);
    int rc = pthread_attr_setschedparam(&attr, &param); /* effect on Windows: possibly none */
    INFO("pthread_attr_setschedparam -> %d", rc);
    CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    g_detached_done = 0;
    CHECK(pthread_create(&t, &attr, detached_body, NULL) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    for (int i = 0; i < 200 && !g_detached_done; i++) usleep(10000);
    CHECK(g_detached_done);

    CHECK(pthread_create(&t, NULL, return_arg, NULL) == 0);
    CHECK(pthread_detach(t) == 0);

    CHECK(pthread_once(&g_once, once_body) == 0 && pthread_once(&g_once, once_body) == 0);
    CHECK(g_once_count == 1);

    pthread_key_t key;
    CHECK(pthread_key_create(&key, NULL) == 0);
    CHECK(pthread_getspecific(key) == NULL);
    CHECK(pthread_setspecific(key, &g_once_count) == 0 && pthread_getspecific(key) == &g_once_count);
    CHECK(pthread_key_delete(key) == 0);

    /* mutex: initializer, trylock, recursive type */
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    CHECK(pthread_mutex_lock(&m) == 0);
    CHECK(pthread_mutex_trylock(&m) == EBUSY);
    CHECK(pthread_mutex_unlock(&m) == 0);
    CHECK(pthread_mutex_trylock(&m) == 0 && pthread_mutex_unlock(&m) == 0);
    CHECK(pthread_mutex_destroy(&m) == 0);

    pthread_mutexattr_t ma;
    pthread_mutex_t rm;
    CHECK(pthread_mutexattr_init(&ma) == 0);
    CHECK(pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE) == 0);
    CHECK(pthread_mutex_init(&rm, &ma) == 0);
    CHECK(pthread_mutexattr_destroy(&ma) == 0);
    CHECK(pthread_mutex_lock(&rm) == 0 && pthread_mutex_lock(&rm) == 0);
    CHECK(pthread_mutex_unlock(&rm) == 0 && pthread_mutex_unlock(&rm) == 0);
    CHECK(pthread_mutex_destroy(&rm) == 0);

    /* rwlock basics */
    pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
    CHECK(pthread_rwlock_rdlock(&rw) == 0 && pthread_rwlock_rdlock(&rw) == 0);
    CHECK(pthread_rwlock_trywrlock(&rw) == EBUSY);
    CHECK(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_wrlock(&rw) == 0);
    CHECK(pthread_rwlock_tryrdlock(&rw) == EBUSY);
    CHECK(pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_destroy(&rw) == 0);
}

/* Condition variable with signal/wait and a plain (CLOCK_REALTIME) timedwait. */
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int flag;
    long delay_ms;
} Signaller;

static void *signal_body(void *arg) {
    Signaller *s = arg;
    usleep((unsigned)(s->delay_ms * 1000));
    pthread_mutex_lock(&s->mutex);
    s->flag = 1;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static void test_cond_default(void) {
    pthread_condattr_t attr;
    CHECK(pthread_condattr_init(&attr) == 0);
    CHECK(pthread_condattr_destroy(&attr) == 0);

    Signaller s = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, 50};
    pthread_t t;
    CHECK(pthread_create(&t, NULL, signal_body, &s) == 0);
    pthread_mutex_lock(&s.mutex);
    while (!s.flag) CHECK(pthread_cond_wait(&s.cond, &s.mutex) == 0);
    pthread_mutex_unlock(&s.mutex);
    CHECK(pthread_join(t, NULL) == 0);

    /* CLOCK_REALTIME deadline (the POSIX default) on a default and a static condition variable */
    pthread_cond_t c;
    CHECK(pthread_cond_init(&c, NULL) == 0);
    pthread_cond_t statics = PTHREAD_COND_INITIALIZER;
    pthread_cond_t *both[2] = {&c, &statics};
    for (int i = 0; i < 2; i++) {
        pthread_mutex_lock(&s.mutex);
        int64_t t0 = now_ms();
        struct timespec dl = after_ms(CLOCK_REALTIME, 100);
        int rc = pthread_cond_timedwait(both[i], &s.mutex, &dl);
        int64_t elapsed = now_ms() - t0;
        pthread_mutex_unlock(&s.mutex);
        CHECK(rc == ETIMEDOUT);
        CHECK(elapsed >= 70 && elapsed < 3000);
    }
    CHECK(pthread_cond_destroy(&c) == 0);
    CHECK(pthread_cond_destroy(&statics) == 0);
    CHECK(pthread_mutex_destroy(&s.mutex) == 0);
}

typedef struct {
    pthread_mutex_t *mutex;
    pthread_cond_t *cond;
    int flag;
} SignalParam;

static void *signal_param_body(void *arg) {
    SignalParam *p = arg;
    usleep(50000);
    pthread_mutex_lock(p->mutex);
    p->flag = 1;
    pthread_cond_signal(p->cond);
    pthread_mutex_unlock(p->mutex);
    return NULL;
}

/* (P) pthread_condattr_setclock(CLOCK_MONOTONIC): the pattern of runtime_sema.c and
 * runtime_eventflag.c: init attr, set the clock, init the cond, destroy the attr, then wait with an
 * absolute CLOCK_MONOTONIC deadline. A monotonic value read as a wall-clock deadline would lie in
 * 1970 and return at once, so the elapsed-time check tells the two apart. */
static void test_cond_monotonic(void) {
    pthread_condattr_t attr;
    pthread_cond_t c;
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    CHECK(pthread_condattr_init(&attr) == 0);
    CHECK(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0);
    CHECK(pthread_cond_init(&c, &attr) == 0);
    CHECK(pthread_condattr_destroy(&attr) == 0);

    pthread_mutex_lock(&m);
    int64_t t0 = now_ms();
    struct timespec dl = after_ms(CLOCK_MONOTONIC, 100);
    int rc = pthread_cond_timedwait(&c, &m, &dl);
    int64_t elapsed = now_ms() - t0;
    CHECK(rc == ETIMEDOUT);
    CHECK(elapsed >= 70 && elapsed < 3000);

    /* a deadline in the past returns at once */
    t0 = now_ms();
    dl = after_ms(CLOCK_MONOTONIC, 0);
    if (dl.tv_sec > 0) dl.tv_sec -= 1;
    rc = pthread_cond_timedwait(&c, &m, &dl);
    elapsed = now_ms() - t0;
    CHECK(rc == ETIMEDOUT);
    CHECK(elapsed < 500);
    pthread_mutex_unlock(&m);

    /* signalled well before a far deadline: returns 0 early */
    SignalParam param = {&m, &c, 0};
    pthread_t t;
    pthread_mutex_lock(&m);
    CHECK(pthread_create(&t, NULL, signal_param_body, &param) == 0);
    t0 = now_ms();
    dl = after_ms(CLOCK_MONOTONIC, 30000);
    rc = 0;
    while (!param.flag && rc == 0) rc = pthread_cond_timedwait(&c, &m, &dl);
    elapsed = now_ms() - t0;
    pthread_mutex_unlock(&m);
    CHECK(pthread_join(t, NULL) == 0);
    CHECK(rc == 0 && param.flag == 1);
    CHECK(elapsed >= 30 && elapsed < 5000);

    CHECK(pthread_cond_destroy(&c) == 0);
    CHECK(pthread_mutex_destroy(&m) == 0);
}

/* (P) pthread_mutex_timedlock, pthread_rwlock_timedrdlock/timedwrlock (CLOCK_REALTIME deadline). */
typedef struct {
    pthread_mutex_t *mutex;
    pthread_rwlock_t *rwlock;
    int operation; /* 1 mutex, 2 rdlock, 3 wrlock */
    int result;
    int64_t elapsed;
} TimedJob;

static void *timed_body(void *arg) {
    TimedJob *job = arg;
    int64_t t0 = now_ms();
    struct timespec dl = after_ms(CLOCK_REALTIME, 100);
    switch (job->operation) {
    case 1: job->result = pthread_mutex_timedlock(job->mutex, &dl); break;
    case 2: job->result = pthread_rwlock_timedrdlock(job->rwlock, &dl); break;
    default: job->result = pthread_rwlock_timedwrlock(job->rwlock, &dl); break;
    }
    job->elapsed = now_ms() - t0;
    if (job->result == 0) {
        if (job->operation == 1) pthread_mutex_unlock(job->mutex);
        else pthread_rwlock_unlock(job->rwlock);
    }
    return NULL;
}

static TimedJob run_timed(pthread_mutex_t *m, pthread_rwlock_t *rw, int operation) {
    TimedJob job = {m, rw, operation, -1, 0};
    pthread_t t;
    if (pthread_create(&t, NULL, timed_body, &job) == 0) pthread_join(t, NULL);
    return job;
}

static void test_timed_locks(void) {
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
    TimedJob job;

    pthread_mutex_lock(&m);
    job = run_timed(&m, &rw, 1);
    CHECK(job.result == ETIMEDOUT);
    CHECK(job.elapsed >= 70 && job.elapsed < 3000);
    pthread_mutex_unlock(&m);
    job = run_timed(&m, &rw, 1);
    CHECK(job.result == 0 && job.elapsed < 1000);

    pthread_rwlock_wrlock(&rw); /* a writer blocks both */
    job = run_timed(&m, &rw, 2);
    CHECK(job.result == ETIMEDOUT);
    CHECK(job.elapsed >= 70 && job.elapsed < 3000);
    job = run_timed(&m, &rw, 3);
    CHECK(job.result == ETIMEDOUT);
    CHECK(job.elapsed >= 70 && job.elapsed < 3000);
    pthread_rwlock_unlock(&rw);

    pthread_rwlock_rdlock(&rw); /* a reader blocks only writers */
    job = run_timed(&m, &rw, 3);
    CHECK(job.result == ETIMEDOUT);
    CHECK(job.elapsed >= 70 && job.elapsed < 3000);
    job = run_timed(&m, &rw, 2);
    CHECK(job.result == 0 && job.elapsed < 1000);
    pthread_rwlock_unlock(&rw);

    job = run_timed(&m, &rw, 3); /* free: immediate */
    CHECK(job.result == 0 && job.elapsed < 1000);
    job = run_timed(&m, &rw, 2);
    CHECK(job.result == 0 && job.elapsed < 1000);

    CHECK(pthread_rwlock_destroy(&rw) == 0);
    CHECK(pthread_mutex_destroy(&m) == 0);
}

/* ---- main ---------------------------------------------------------------------------------- */

int main(void) {
#ifdef _WIN32
    pf_gap_init_crt();
#endif
    const char *base = getenv("PF_TEST_TMPDIR");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = getenv("TMP");
    if (!base || !*base) base = getenv("TEMP");
    if (!base || !*base) base = ".";
    snprintf(W, sizeof W, "%s/posix_subset_work", base);
    for (char *p = W; *p; p++)
        if (*p == '\\') *p = '/';
    cleanup();
    if (mkdir(W, 0755) != 0) {
        fprintf(stderr, "cannot create %s (errno %d)\n", W, errno);
        return 2;
    }

    test_basics();
#ifdef _WIN32
    test_chunks();
#endif
    test_files();
    test_big_file();
    test_directories();
    test_environment();
    test_strings();
    test_time();
    test_pthread_basic();
    test_cond_default();
    test_cond_monotonic();
    test_timed_locks();

    cleanup();
    if (g_failed) {
        printf("test_posix_subset: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("test_posix_subset: %d checks passed\n", g_checks);
    return 0;
}
