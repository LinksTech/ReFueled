/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux process initialisation and platform name. */
#include <malloc.h>
#include "pf_base.h"

int pf_init(void) {
    /* Moved from probe.c main: keep host heap objects handed to the guest (thread handles, TLS)
       in the non-PIE brk heap, i.e. below 1 TiB: the guest packs pointers into 40 bits. */
    mallopt(M_ARENA_MAX,1);
    mallopt(M_MMAP_THRESHOLD,32*1024*1024);
    return 0;
}

const char *pf_platform_name(void) { return "linux"; }
