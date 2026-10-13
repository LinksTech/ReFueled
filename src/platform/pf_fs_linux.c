/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux pf_fs. Moved from src/runtime_savedata.c (remove_entry + nftw calls) and
 * src/runtime_file.c (rename); behaviour unchanged. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include "pf_fs.h"
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <sys/stat.h>

static int pf_errno(int e) {
    switch (e) {
    case ENOENT: return PF_ENOENT;
    case EACCES: return PF_EACCES;
    case EPERM: return PF_EPERM;
    case EBUSY: return PF_EBUSY;
    case ENOSPC: return PF_ENOSPC;
    case EIO: return PF_EIO;
    default: return PF_EIO;
    }
}

/* nftw callbacks get no user pointer, so the errno of the failed remove() is kept per thread. */
static _Thread_local int callback_errno;

/* As in runtime_savedata.c: a failing remove() ends the walk (nonzero return). */
static int remove_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
    (void)st; (void)flag; (void)ftw;
    int r = remove(path);
    if (r != 0) callback_errno = errno;
    return r;
}

int pf_fs_remove_tree(const char *path) {
    struct stat st;
    if (!path) return PF_EINVAL;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : pf_errno(errno);
    callback_errno = 0;
    errno = 0;
    if (nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) == 0) return 0;
    return pf_errno(callback_errno ? callback_errno : errno);
}

int pf_fs_stamp(const char *path, int64_t *mtime_ns, uint64_t *size) {
    struct stat st;
    if (!path) return PF_EINVAL;
    if (stat(path, &st) != 0) return errno == ENOENT ? PF_ENOENT : PF_EIO;
    if (mtime_ns) *mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
    if (size) *size = (uint64_t)st.st_size;
    return 0;
}

int pf_fs_rename_replace(const char *from, const char *to) {
    if (!from || !to) return PF_EINVAL;
    return rename(from, to) == 0 ? 0 : pf_errno(errno);
}

/* runtime_file.c convert_stat: the fields it read from struct stat. */
static int to_pf_stat(int result, const struct stat *s, PfStat *out) {
    if (result != 0) return errno == ENOENT ? PF_ENOENT : errno == EACCES ? PF_EACCES : PF_EIO;
    out->dev = (uint64_t)s->st_dev; out->ino = (uint64_t)s->st_ino; out->nlink = (uint64_t)s->st_nlink;
    out->size = (uint64_t)s->st_size; out->blocks = (uint64_t)s->st_blocks;
    out->mode = (uint32_t)s->st_mode; out->blksize = (uint32_t)s->st_blksize;
    out->atime_sec = s->st_atim.tv_sec; out->atime_nsec = s->st_atim.tv_nsec;
    out->mtime_sec = s->st_mtim.tv_sec; out->mtime_nsec = s->st_mtim.tv_nsec;
    out->ctime_sec = s->st_ctim.tv_sec; out->ctime_nsec = s->st_ctim.tv_nsec;
    return 0;
}

int pf_fs_stat(const char *path, PfStat *out) {
    struct stat s;
    if (!path || !out) return PF_EINVAL;
    return to_pf_stat(stat(path, &s), &s, out);
}

int pf_fs_fstat(int fd, PfStat *out) {
    struct stat s;
    if (!out) return PF_EINVAL;
    return to_pf_stat(fstat(fd, &s), &s, out);
}
