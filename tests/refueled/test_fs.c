/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: unit test for pf_fs_remove_tree and pf_fs_rename_replace (Linux and Windows).
 * Works in ./fs_test_work below the current directory; start it from a scratch folder. */
#include "platform/pf_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#define MAKE_DIR(p) _mkdir(p)
#define MAKE_READONLY(p) _chmod((p), _S_IREAD)
#else
#include <unistd.h>
#define MAKE_DIR(p) mkdir((p), 0755)
#define MAKE_READONLY(p) chmod((p), 0444)
#endif

static int g_checks, g_failed;
#define CHECK(cond) do { g_checks++; if (!(cond)) { g_failed++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}
static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(text);
    int ok = fwrite(text, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static int read_text(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = 0;
    return 1;
}

#ifdef _WIN32
/* Creates a directory whose absolute path may exceed MAX_PATH (extended-length form). */
static int make_dir_long(const char *relative) {
    char cwd[512], full[1400];
    if (!_getcwd(cwd, sizeof cwd)) return -1;
    snprintf(full, sizeof full, "\\\\?\\%s\\%s", cwd, relative);
    for (char *p = full + 4; *p; p++) if (*p == '/') *p = '\\';
    wchar_t wide[1400];
    if (!MultiByteToWideChar(CP_UTF8, 0, full, -1, wide, 1400)) return -1;
    return CreateDirectoryW(wide, NULL) ? 0 : -1;
}
#else
#define make_dir_long(p) MAKE_DIR(p)
#endif

#define W "fs_test_work"

static void test_tree(void) {
    pf_fs_remove_tree(W);
    CHECK(MAKE_DIR(W) == 0);
    CHECK(MAKE_DIR(W "/a") == 0);
    CHECK(MAKE_DIR(W "/a/b") == 0);
    CHECK(MAKE_DIR(W "/a/b/c") == 0);
    CHECK(MAKE_DIR(W "/empty") == 0);
    CHECK(write_text(W "/top.txt", "top"));
    CHECK(write_text(W "/a/one.txt", "one"));
    CHECK(write_text(W "/a/b/two.txt", "two"));
    CHECK(write_text(W "/a/b/c/three.txt", "three"));
    CHECK(write_text(W "/a/b/c/readonly.txt", "ro"));
    CHECK(MAKE_READONLY(W "/a/b/c/readonly.txt") == 0);
    CHECK(pf_fs_remove_tree(W) == 0);
    CHECK(!exists(W));
    CHECK(pf_fs_remove_tree(W) == 0);                 /* missing path */
    CHECK(pf_fs_remove_tree(W "/never/was/here") == 0);

    CHECK(MAKE_DIR(W) == 0);
    CHECK(write_text(W "/single.txt", "x"));
    CHECK(pf_fs_remove_tree(W "/single.txt") == 0);   /* a file as path */
    CHECK(!exists(W "/single.txt") && exists(W));
    CHECK(pf_fs_remove_tree(W) == 0);

    /* long path (over 260 characters) */
    char deep[900];
    snprintf(deep, sizeof deep, "%s", W);
    CHECK(MAKE_DIR(W) == 0);
    for (int i = 0; i < 12; i++) {
        strcat(deep, "/directory_name_with_twenty_five_chars");
        if (make_dir_long(deep) != 0) break;
    }
    int made = (int)strlen(deep) > 300;
    CHECK(made);
    if (!made) fprintf(stderr, "note: long path not created, skipped part of the long-path check\n");
    CHECK(pf_fs_remove_tree(W) == 0);
    CHECK(!exists(W));
}

static void test_link(void) {
    pf_fs_remove_tree(W);
    CHECK(MAKE_DIR(W) == 0);
    CHECK(MAKE_DIR(W "/outside") == 0);
    CHECK(write_text(W "/outside/keep.txt", "keep"));
    CHECK(MAKE_DIR(W "/tree") == 0);
    CHECK(write_text(W "/tree/file.txt", "f"));
#ifdef _WIN32
    /* A junction needs no admin rights; mklink is a cmd builtin. */
    int made = system("cmd /c mklink /J fs_test_work\\tree\\link fs_test_work\\outside >nul 2>&1") == 0;
#else
    int made = symlink("../outside", W "/tree/link") == 0;
#endif
    if (!made) {
        fprintf(stderr, "note: directory link not creatable here, link check skipped\n");
    } else {
        CHECK(exists(W "/tree/link/keep.txt"));
        CHECK(pf_fs_remove_tree(W "/tree") == 0);
        CHECK(!exists(W "/tree"));
        CHECK(exists(W "/outside/keep.txt"));        /* target untouched */
    }
    CHECK(pf_fs_remove_tree(W) == 0);
    CHECK(!exists(W));
}

static void test_rename(void) {
    char buffer[64];
    pf_fs_remove_tree(W);
    CHECK(MAKE_DIR(W) == 0);
    CHECK(write_text(W "/new.txt", "new"));
    CHECK(write_text(W "/old.txt", "old"));
    CHECK(pf_fs_rename_replace(W "/new.txt", W "/old.txt") == 0);   /* target exists */
    CHECK(!exists(W "/new.txt"));
    CHECK(read_text(W "/old.txt", buffer, sizeof buffer) && strcmp(buffer, "new") == 0);
    CHECK(write_text(W "/src.txt", "src"));
    CHECK(pf_fs_rename_replace(W "/src.txt", W "/created.txt") == 0); /* target missing */
    CHECK(!exists(W "/src.txt"));
    CHECK(read_text(W "/created.txt", buffer, sizeof buffer) && strcmp(buffer, "src") == 0);
    CHECK(pf_fs_rename_replace(W "/does_not_exist", W "/x") == PF_ENOENT);

    /* save game round trip: write temp, replace, read back */
    const char *data = "save-data-0123456789";
    CHECK(write_text(W "/save.bin", "previous"));
    CHECK(write_text(W "/save.bin.bbtmp", data));
    CHECK(pf_fs_rename_replace(W "/save.bin.bbtmp", W "/save.bin") == 0);
    CHECK(read_text(W "/save.bin", buffer, sizeof buffer) && strcmp(buffer, data) == 0);
    CHECK(!exists(W "/save.bin.bbtmp"));
    CHECK(pf_fs_remove_tree(W) == 0);
}

#ifdef _WIN32
/* An open file (no FILE_SHARE_DELETE) makes the removal fail: the first error is returned and the
 * walk stops. On Linux no reliable way exists without root/permissions on a Windows drive. */
static void test_locked(void) {
    pf_fs_remove_tree(W);
    CHECK(MAKE_DIR(W) == 0);
    CHECK(MAKE_DIR(W "/d") == 0);
    CHECK(write_text(W "/d/locked.txt", "x"));
    FILE *held = fopen(W "/d/locked.txt", "rb");
    CHECK(held != NULL);
    CHECK(pf_fs_remove_tree(W) == PF_EBUSY);
    CHECK(exists(W "/d/locked.txt"));
    if (held) fclose(held);
    CHECK(pf_fs_remove_tree(W) == 0);
    CHECK(!exists(W));
}
#endif

int main(void) {
    test_tree();
    test_link();
    test_rename();
#ifdef _WIN32
    test_locked();
#endif
    if (g_failed) { printf("test_fs: %d of %d checks FAILED\n", g_failed, g_checks); return 1; }
    printf("test_fs: %d checks passed\n", g_checks);
    return 0;
}
