/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stderr output that works inside fault handlers (a real implementation, because
 * pf_not_implemented depends on it). */
#include <string.h>
#include <windows.h>
#include "pf_log.h"

void pf_write_stderr(const char *data, size_t size) {
    HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
    if (handle == NULL || handle == INVALID_HANDLE_VALUE) return;
    while (size > 0) {
        DWORD chunk = size > 0x40000000u ? 0x40000000u : (DWORD)size;
        DWORD written = 0;
        if (!WriteFile(handle, data, chunk, &written, NULL) || written == 0) return;
        data += written;
        size -= written;
    }
}

void pf_write_stderr_str(const char *text) { pf_write_stderr(text, strlen(text)); }
