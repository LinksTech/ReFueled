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

/* What the runtime reads from struct stat for the guest's stat (runtime_file.c convert_stat),
 * in fixed-width types. Linux: copied field by field from stat()/fstat(); Windows: from
 * GetFileInformationByHandle (times with 100 ns resolution, the file index as ino, the volume
 * serial as dev; mode S_IFDIR|0755 or S_IFREG|0644, 0444 when read-only; blksize 4096, blocks in
 * 512-byte units of the size). */
typedef struct {
    uint64_t dev, ino, nlink, size, blocks;
    uint32_t mode, blksize;
    int64_t atime_sec, atime_nsec, mtime_sec, mtime_nsec, ctime_sec, ctime_nsec;
} PfStat;

/* stat(path) / fstat(fd) into a PfStat; 0 or a negative error (Linux: -errno exactly as stat set
 * it; Windows: PF_ENOENT, PF_EACCES, PF_EINVAL or PF_EIO). */
int pf_fs_stat(const char *path, PfStat *out);
int pf_fs_fstat(int fd, PfStat *out);

/* The d_type of a readdir entry where the platform has one (Linux: entry->d_type); Windows has
 * none and src/platform/pf_posix_gap_win32.h defines this as DT_UNKNOWN, so callers fall back
 * to pf_fs_stat of the entry, as the common code already does for DT_UNKNOWN. */
#ifndef PF_DIRENT_TYPE
#define PF_DIRENT_TYPE(entry) ((entry)->d_type)
#endif

/* Deletes `path` and everything below it, children first, without following symbolic links
 * (nftw(FTW_DEPTH|FTW_PHYS) + remove). Missing path: 0. Returns 0 or PF_E*: the walk stops at
 * the first error, as the nftw callback in runtime_savedata.c always did. */
int pf_fs_remove_tree(const char *path);

/* Replaces `to` with `from` atomically where the platform allows (rename / MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH). */
int pf_fs_rename_replace(const char *from, const char *to);

/* Modification time in nanoseconds since 1970 and size of `path` (stat st_mtim/st_size /
 * GetFileAttributesExW), for "changed since last time" checks; either pointer may be NULL.
 * 0, PF_ENOENT or PF_EIO. */
int pf_fs_stamp(const char *path, int64_t *mtime_ns, uint64_t *size);

PF_EXTERN_C_END
#endif
