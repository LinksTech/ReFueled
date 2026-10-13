/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: pf_tls on Linux. The guest thread pointer is the GS base of each thread
 * (arch_prctl), as src/runtime_thread.c set_gs does today. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include <asm/prctl.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "pf_tls.h"

int pf_tls_init(void) { return 0; }

/* Moved from runtime_thread.c:61 (the caller keeps the perror/exit(21) on failure). */
int pf_tls_set_guest_base(void *tcb) {
    if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)tcb)) return -errno;
    return 0;
}

/* New (no counterpart before): ARCH_GET_GS reads the GS base back. */
void *pf_tls_get_guest_base(void) {
    unsigned long base = 0;
    if (syscall(SYS_arch_prctl, ARCH_GET_GS, &base)) return NULL;
    return (void *)base;
}

/* Linux needs no rewrite: counts the GS-form loads 65 48 8b 04 25 00 00 00 00. */
int64_t pf_tls_patch_image(const PfCodeRange *ranges, size_t count) {
    static const unsigned char pattern[9] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00};
    int64_t found = 0;
    if (!ranges && count) return PF_EINVAL;
    for (size_t r = 0; r < count; r++) {
        const unsigned char *start = ranges[r].start;
        uint64_t size = ranges[r].size;
        if (!start || size < sizeof pattern) continue;
        for (uint64_t i = 0; i + sizeof pattern <= size; i++) {
            if (start[i] == pattern[0] && memcmp(start + i, pattern, sizeof pattern) == 0) {
                found++;
                i += sizeof pattern - 1;
            }
        }
    }
    return found;
}
