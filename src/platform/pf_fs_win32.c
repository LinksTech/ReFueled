/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Windows pf_fs.
 *
 * Paths are UTF-8 with '/' or '\' separators. Each is turned into an absolute UTF-16 path with the
 * "\\?\" prefix, because LongPathsEnabled is 0 on the target systems. Buffers are on the heap. */
#include "pf_fs.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

static int pf_error(DWORD e) {
    switch (e) {
    case ERROR_ACCESS_DENIED: return PF_EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION: return PF_EBUSY;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: return PF_ENOENT;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return PF_ENOSPC;
    default: return PF_EIO;
    }
}

/* UTF-8 -> absolute extended-length UTF-16 path; malloc'd, NULL on failure (*error set). */
static wchar_t *to_wide(const char *path, int *error) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) { *error = PF_EINVAL; return NULL; }
    wchar_t *wide = malloc((size_t)n * sizeof(wchar_t));
    if (!wide) { *error = PF_ENOMEM; return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n);
    if (wcsncmp(wide, L"\\\\?\\", 4) == 0) return wide;
    DWORD full = GetFullPathNameW(wide, 0, NULL, NULL); /* includes the terminator */
    if (full == 0) { *error = pf_error(GetLastError()); free(wide); return NULL; }
    wchar_t *absolute = malloc((size_t)full * sizeof(wchar_t));
    wchar_t *out = malloc(((size_t)full + 8) * sizeof(wchar_t));
    DWORD got = (absolute && out) ? GetFullPathNameW(wide, full, absolute, NULL) : 0;
    free(wide);
    if (got == 0 || got >= full) {
        *error = (absolute && out) ? PF_EIO : PF_ENOMEM;
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

static void note(int *first, DWORD e) {
    if (*first == 0) *first = pf_error(e);
}

/* Deletes one non-directory, or a directory link (not followed). */
static void remove_entry(const wchar_t *path, DWORD attributes, int *first) {
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    BOOL ok = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(path) : DeleteFileW(path);
    if (!ok) note(first, GetLastError());
}

static void remove_walk(const wchar_t *path, int *first) {
    if (*first) return; /* the walk stops at the first error */
    DWORD attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND) note(first, e);
        return;
    }
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        remove_entry(path, attributes, first);
        return;
    }
    size_t length = wcslen(path);
    wchar_t *pattern = malloc((length + 3) * sizeof(wchar_t));
    WIN32_FIND_DATAW *data = malloc(sizeof(*data));
    if (!pattern || !data) {
        free(pattern);
        free(data);
        note(first, ERROR_NOT_ENOUGH_MEMORY);
        return;
    }
    memcpy(pattern, path, length * sizeof(wchar_t));
    pattern[length] = L'\\';
    pattern[length + 1] = L'*';
    pattern[length + 2] = 0;
    HANDLE find = FindFirstFileW(pattern, data);
    free(pattern);
    if (find == INVALID_HANDLE_VALUE) {
        note(first, GetLastError());
    } else {
        do {
            if (!wcscmp(data->cFileName, L".") || !wcscmp(data->cFileName, L"..")) continue;
            size_t name = wcslen(data->cFileName);
            wchar_t *child = malloc((length + name + 2) * sizeof(wchar_t));
            if (!child) { note(first, ERROR_NOT_ENOUGH_MEMORY); continue; }
            memcpy(child, path, length * sizeof(wchar_t));
            child[length] = L'\\';
            memcpy(child + length + 1, data->cFileName, (name + 1) * sizeof(wchar_t));
            remove_walk(child, first);
            free(child);
        } while (!*first && FindNextFileW(find, data));
        DWORD e = GetLastError();
        if (!*first && e != ERROR_NO_MORE_FILES) note(first, e);
        FindClose(find);
    }
    free(data);
    if (*first) return; /* children failed: the directory cannot be empty */
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (!RemoveDirectoryW(path)) note(first, GetLastError());
}

int pf_fs_remove_tree(const char *path) {
    if (!path) return PF_EINVAL;
    int error = 0;
    wchar_t *wide = to_wide(path, &error);
    if (!wide) return error;
    int first = 0;
    remove_walk(wide, &first);
    free(wide);
    return first;
}

int pf_fs_rename_replace(const char *from, const char *to) {
    if (!from || !to) return PF_EINVAL;
    int error = 0;
    wchar_t *a = to_wide(from, &error);
    if (!a) return error;
    wchar_t *b = to_wide(to, &error);
    if (!b) { free(a); return error; }
    BOOL ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    DWORD e = ok ? 0 : GetLastError();
    free(a);
    free(b);
    return ok ? 0 : pf_error(e);
}
