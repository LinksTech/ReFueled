/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: file-system operations without a portable POSIX form.
 *
 * Plain POSIX file calls (open/read/write/close/lseek/fstat/stat/opendir/readdir/fopen, ...) stay
 * in the common code; on Windows MinGW-w64 provides them and src/platform/pf_posix_gap_win32.h
 * (force-included on Windows only) fills the gaps (pread/pwrite, fsync, realpath, two-argument
 * mkdir, rename over an existing file, binary mode). This header holds what has no POSIX form or
 * needs different semantics. */
#ifndef REFUELED_PF_FS_H
#define REFUELED_PF_FS_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* Deletes `path` and everything below it, children first, without following symbolic links
 * (nftw(FTW_DEPTH|FTW_PHYS) + remove). Missing path: 0. Returns 0 or PF_E*: the walk stops at
 * the first error, as the nftw callback in runtime_savedata.c always did. */
int pf_fs_remove_tree(const char *path);

/* Replaces `to` with `from` atomically where the platform allows (rename / MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH). */
int pf_fs_rename_replace(const char *from, const char *to);

PF_EXTERN_C_END
#endif
