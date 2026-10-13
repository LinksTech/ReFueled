/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: the guest's thread pointer on Windows (pf_tls.h).
 *
 * GS is the TEB and cannot be moved, so the guest's TCB pointer lives in a TLS slot below 64,
 * whose value the TEB keeps inline at gs:[0x1480 + 8*slot] (TEB.TlsSlots). The boot file's
 * GS-form loads `mov rax, gs:[0]` (65 48 8b 04 25 00000000) get that displacement once after
 * loading; the instruction keeps its length, so no trampoline is needed (the scheme shadPS4
 * uses, src/core/tls.cpp at shadPS4 b4e7ae7). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "pf_tls.h"

#define TEB_TLS_SLOTS 0x1480 /* offsetof(TEB, TlsSlots) on x64 */
#define INLINE_SLOTS 64

static DWORD slot = TLS_OUT_OF_INDEXES;

int pf_tls_init(void) {
    if (slot != TLS_OUT_OF_INDEXES) return 0;
    DWORD index = TlsAlloc();
    if (index == TLS_OUT_OF_INDEXES) return PF_ENOSPC;
    if (index >= INLINE_SLOTS) { /* expansion slots are not in the TEB itself */
        TlsFree(index);
        return PF_ENOSPC;
    }
    slot = index;
    return 0;
}

int pf_tls_set_guest_base(void *tcb) {
    if (slot == TLS_OUT_OF_INDEXES) return PF_EINVAL;
    return TlsSetValue(slot, tcb) ? 0 : PF_EINVAL;
}

void *pf_tls_get_guest_base(void) { return slot == TLS_OUT_OF_INDEXES ? NULL : TlsGetValue(slot); }

int64_t pf_tls_patch_image(const PfCodeRange *ranges, size_t count) {
    static const unsigned char load[9] = {0x65, 0x48, 0x8b, 0x04, 0x25, 0, 0, 0, 0};
    if (slot == TLS_OUT_OF_INDEXES) return PF_EINVAL;
    if (count && !ranges) return PF_EINVAL;
    const uint32_t displacement = TEB_TLS_SLOTS + 8 * slot;
    int64_t patched = 0;
    for (size_t r = 0; r < count; ++r) {
        unsigned char *code = ranges[r].start;
        for (uint64_t at = 0; ranges[r].size >= sizeof load && at <= ranges[r].size - sizeof load; ++at) {
            if (code[at] != 0x65 || memcmp(code + at, load, sizeof load)) continue;
            memcpy(code + at + 5, &displacement, sizeof displacement);
            ++patched;
            at += sizeof load - 1;
        }
    }
    return patched;
}
