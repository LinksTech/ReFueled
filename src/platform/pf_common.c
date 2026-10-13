/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: platform-neutral part of the platform layer: pf_not_implemented.
 * Async-signal-safe and thread-safe: no printf, no malloc, no locks; a fixed table of function
 * names claimed with compare-exchange. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include <stdatomic.h>
#include <string.h>
#include "pf_base.h"
#include "pf_log.h"

#define PF_NI_TABLE_SIZE 256

static _Atomic(const char *) pf_ni_seen[PF_NI_TABLE_SIZE];

static void pf_ni_report(const char *function) {
    char line[256];
    size_t length = 0;
    const char *parts[4] = {"refueled: ", function, " not implemented on ", pf_platform_name()};
    for (int i = 0; i < 4; i++) {
        size_t n = strlen(parts[i]);
        if (n > sizeof line - 2 - length) n = sizeof line - 2 - length;
        memcpy(line + length, parts[i], n);
        length += n;
    }
    line[length++] = '\n';
    pf_write_stderr(line, length);
}

int pf_not_implemented(const char *function) {
    if (!function) function = "(null)";
    for (int i = 0; i < PF_NI_TABLE_SIZE; i++) {
        const char *entry = atomic_load(&pf_ni_seen[i]);
        if (!entry) {
            const char *expected = NULL;
            if (atomic_compare_exchange_strong(&pf_ni_seen[i], &expected, function)) {
                pf_ni_report(function);
                return PF_ENOSYS;
            }
            entry = expected; /* another thread claimed the slot first */
        }
        if (entry == function || strcmp(entry, function) == 0) return PF_ENOSYS;
    }
    pf_ni_report(function); /* table full: report anyway */
    return PF_ENOSYS;
}
