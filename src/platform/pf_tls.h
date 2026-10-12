/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: the guest's thread pointer.
 *
 * The game reads its thread control block with `mov rax, fs:[0]` (initial-exec TLS).
 * scripts/link_libc.py and scripts/link_modules.py rewrite each of these 9-byte instructions
 * (64 48 8b 04 25 00 00 00 00) to the GS form (65 48 8b 04 25 00 00 00 00) when they build the
 * boot file, so the boot file is the same on every platform.
 *   Linux: the runtime points the GS base of each thread at its TCB (arch_prctl(ARCH_SET_GS)),
 *          and gs:[0] is the TCB's self pointer.
 *   Windows: GS is the TEB and cannot move. The loads are rewritten once more after loading
 *          (pf_tls_patch_image) to read gs:[0x1480 + 8*slot], the TEB's TlsSlots[slot] of a
 *          TlsAlloc slot below 64, which holds the TCB pointer (the scheme shadPS4 uses). */
#ifndef REFUELED_PF_TLS_H
#define REFUELED_PF_TLS_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* Called by pf_init. Windows: allocates the slot (PF_ENOSPC when TlsAlloc returns >= 64). */
int pf_tls_init(void);

/* Makes `tcb` the calling thread's guest thread pointer (what gs:[0] / the patched loads read). */
int pf_tls_set_guest_base(void *tcb);

/* The calling thread's guest thread pointer, NULL if none was set. */
void *pf_tls_get_guest_base(void);

/* An executable range of the loaded image. */
typedef struct {
    unsigned char *start;
    uint64_t size;
} PfCodeRange;

/* Adapts the GS-form thread-pointer loads in the given ranges to this platform. Called by the
 * loader after relocation and before the image is made executable. Linux: nothing to change;
 * returns the number of loads found. Windows: rewrites their displacement to the slot; returns
 * the number patched. PF_E* on failure. */
int64_t pf_tls_patch_image(const PfCodeRange *ranges, size_t count);

PF_EXTERN_C_END
#endif
