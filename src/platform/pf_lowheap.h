/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: allocator over a caller-supplied address range (internal to src/platform and tests).
 *
 * Used by the Windows pf_low_* functions; plain C11 so it is also tested on Linux. The header does
 * not include pthread.h (MSVC has none): the mutex lives in an opaque, aligned byte field that
 * pf_lowheap.c checks against sizeof(pthread_mutex_t) at compile time. A PfLowHeap is unusable
 * until pf_lowheap_setup succeeded; a zero-initialised one makes alloc return NULL. */
#ifndef REFUELED_PF_LOWHEAP_H
#define REFUELED_PF_LOWHEAP_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

typedef struct PfLowHeap {
    unsigned char *base;      /* first byte of the range (NULL = not set up) */
    size_t size;              /* usable bytes, multiple of 16 */
    void *free_head;          /* free list (doubly linked, LIFO) */
    size_t used_bytes;        /* bytes in used blocks incl. headers (statistics, tests) */
    _Alignas(16) unsigned char lock[64]; /* pthread_mutex_t, initialised by setup */
} PfLowHeap;

/* Takes over [base, base+size). 0, or PF_EINVAL when base is not 64-aligned, size is below 256
 * bytes, or base+size exceeds 1 TiB (checked on the numbers, the memory is not touched before
 * that). The range must be readable and writable. */
int pf_lowheap_setup(PfLowHeap *heap, void *base, size_t size);

/* `alignment`: power of two from 1 to 65536 (below 16 is rounded up to 16). zero != 0 clears the
 * `size` bytes returned. NULL on exhaustion, bad arguments or a heap that is not set up. */
void *pf_lowheap_alloc(PfLowHeap *heap, size_t alignment, size_t size, int zero);

/* NULL is ignored. A pointer that does not come from this heap, or a double free, aborts after a
 * pf_write_stderr message. */
void pf_lowheap_free(PfLowHeap *heap, void *pointer);

/* 1 when `pointer` points into the payload area of the heap's range, else 0. */
int pf_lowheap_contains(const PfLowHeap *heap, const void *pointer);

PF_EXTERN_C_END
#endif
