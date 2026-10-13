/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: stubs, K4 replaces them. */
#include "pf_tls.h"

int pf_tls_init(void) { return pf_not_implemented(__func__); }

int pf_tls_set_guest_base(void *tcb) {
    (void)tcb;
    return pf_not_implemented(__func__);
}

void *pf_tls_get_guest_base(void) {
    pf_not_implemented(__func__);
    return NULL;
}

int64_t pf_tls_patch_image(const PfCodeRange *ranges, size_t count) {
    (void)ranges;
    (void)count;
    return pf_not_implemented(__func__);
}
