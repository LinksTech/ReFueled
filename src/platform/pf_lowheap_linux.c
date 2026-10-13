/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux low heap = the C heap (see pf_memory.h, "Low heap"). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* before the first system header, as in the runtime sources */
#endif
#include "pf_memory.h"
#include <stdlib.h>

/* The brk heap is already below 1 TiB (non-PIE, mallopt in pf_init); nothing to set up. */
int pf_low_heap_init(uintptr_t base, uint64_t size) {
    (void)base;
    (void)size;
    return 0;
}
void *pf_low_calloc(size_t count, size_t size) { return calloc(count, size); }
void *pf_low_aligned_alloc(size_t alignment, size_t size) { return aligned_alloc(alignment, size); }
void pf_low_free(void *pointer) { free(pointer); }
