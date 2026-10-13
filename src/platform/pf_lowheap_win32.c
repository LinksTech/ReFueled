/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Windows low heap: pf_lowheap.c over a range mapped inside the low region. */
#include "pf_lowheap.h"
#include "pf_log.h"
#include "pf_memory.h"

static PfLowHeap g_low_heap;
static int g_low_ready;

int pf_low_heap_init(uintptr_t base, uint64_t size) {
    if (g_low_ready) return PF_EBUSY;
    if (size > (size_t)-1) return PF_EINVAL;
    int rc = pf_vm_alloc_fixed(base, size, PF_PROT_READ | PF_PROT_WRITE);
    if (rc < 0) return rc;
    rc = pf_lowheap_setup(&g_low_heap, (void *)base, (size_t)size);
    if (rc < 0) {
        pf_vm_free((void *)base, size);
        return rc;
    }
    g_low_ready = 1;
    return 0;
}

static void *low_alloc(size_t alignment, size_t size, int zero) {
    if (!g_low_ready) {
        pf_write_stderr_str("refueled: low heap used before pf_low_heap_init\n");
        return NULL;
    }
    return pf_lowheap_alloc(&g_low_heap, alignment, size, zero);
}

void *pf_low_calloc(size_t count, size_t size) {
    if (size && count > (size_t)-1 / size) return NULL;
    return low_alloc(16, count * size, 1);
}
void *pf_low_aligned_alloc(size_t alignment, size_t size) { return low_alloc(alignment, size, 0); }
void pf_low_free(void *pointer) {
    if (!pointer) return;
    if (!g_low_ready) {
        pf_write_stderr_str("refueled: low heap used before pf_low_heap_init\n");
        return;
    }
    pf_lowheap_free(&g_low_heap, pointer);
}
