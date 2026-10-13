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

int pf_fs_rename_replace(const char *from, const char *to) {
    if (!from || !to) return PF_EINVAL;
    return rename(from, to) == 0 ? 0 : pf_errno(errno);
}
