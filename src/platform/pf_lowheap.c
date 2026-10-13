/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: allocator over a fixed address range (see pf_lowheap.h).
 *
 * Purpose: host objects whose address the guest sees as a 40-bit value (thread handles, TCB/TLS
 * blocks, attributes) must come from the range below 1 TiB; on Windows the C heap gives no such
 * guarantee, so they are carved out of a block the platform maps there.
 *
 * Method: the range is a sequence of blocks, each with a 16-byte header (own size | used bit, size
 * of the block before it), so free() finds both neighbours in O(1) and merges them. Free blocks sit
 * in a doubly linked list (links in their payload, minimum block 32 bytes); allocation is first
 * fit. For alignment above 16 the front of the chosen block is split off as a free block of its
 * own (it must be at least a minimum block, else the payload moves to the next aligned address),
 * and the tail is split off when at least a minimum block remains. The range never grows. One
 * mutex guards everything. */
#include "pf_lowheap.h"
#include "pf_log.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct Block {
    uint64_t size_used; /* block size including this header; bit 0 = used */
    uint64_t prev_size; /* size of the physically preceding block, 0 for the first */
} Block;

typedef struct FreeLinks { Block *next, *prev; } FreeLinks; /* in the payload of a free block */

#define HDR ((size_t)sizeof(Block))
#define MIN_BLOCK (HDR + sizeof(FreeLinks))
#define MAX_ALIGN ((size_t)65536)
#define LOW_LIMIT ((uint64_t)1 << 40)

_Static_assert(sizeof(Block) == 16, "block header must be 16 bytes");
_Static_assert(sizeof(pthread_mutex_t) <= sizeof(((PfLowHeap *)0)->lock), "lock field too small");

static pthread_mutex_t *mutex_of(PfLowHeap *heap) { return (pthread_mutex_t *)(void *)heap->lock; }
static size_t bsize(const Block *b) { return (size_t)(b->size_used & ~(uint64_t)1); }
static int bused(const Block *b) { return (int)(b->size_used & 1); }
static FreeLinks *links(Block *b) { return (FreeLinks *)(void *)(b + 1); }
static Block *block_end(const PfLowHeap *heap) { return (Block *)(void *)(heap->base + heap->size); }
static Block *next_block(const PfLowHeap *heap, Block *b) {
    Block *n = (Block *)(void *)((unsigned char *)b + bsize(b));
    return n < block_end(heap) ? n : NULL;
}
static void fix_next_prev(const PfLowHeap *heap, Block *b) {
    Block *n = next_block(heap, b);
    if (n) n->prev_size = bsize(b);
}
static void list_insert(PfLowHeap *heap, Block *b) {
    Block *head = (Block *)heap->free_head;
    links(b)->prev = NULL;
    links(b)->next = head;
    if (head) links(head)->prev = b;
    heap->free_head = b;
}
static void list_remove(PfLowHeap *heap, Block *b) {
    FreeLinks *l = links(b);
    if (l->prev) links(l->prev)->next = l->next; else heap->free_head = l->next;
    if (l->next) links(l->next)->prev = l->prev;
}
static uintptr_t align_up(uintptr_t v, size_t a) { return (v + a - 1) & ~(uintptr_t)(a - 1); }

int pf_lowheap_setup(PfLowHeap *heap, void *base, size_t size) {
    uintptr_t b = (uintptr_t)base;
    if (!heap || !base || (b & 63) || size < 256) return PF_EINVAL;
    size &= ~(size_t)15;
    if ((uint64_t)b >= LOW_LIMIT || (uint64_t)size > LOW_LIMIT - (uint64_t)b) return PF_EINVAL;
    memset(heap, 0, sizeof(*heap));
    if (pthread_mutex_init(mutex_of(heap), NULL) != 0) return PF_ENOMEM;
    heap->base = (unsigned char *)base;
    heap->size = size;
    Block *first = (Block *)base;
    first->size_used = size;
    first->prev_size = 0;
    list_insert(heap, first);
    return PF_OK;
}

int pf_lowheap_contains(const PfLowHeap *heap, const void *pointer) {
    if (!heap || !heap->base) return 0;
    const unsigned char *p = (const unsigned char *)pointer;
    return p >= heap->base + HDR && p < heap->base + heap->size;
}

void *pf_lowheap_alloc(PfLowHeap *heap, size_t alignment, size_t size, int zero) {
    if (!heap || !heap->base) return NULL;
    if (alignment == 0 || alignment > MAX_ALIGN || (alignment & (alignment - 1))) return NULL;
    if (alignment < 16) alignment = 16;
    if (size > heap->size) return NULL;
    size_t need = (size + 15) & ~(size_t)15;
    need += HDR;
    if (need < MIN_BLOCK) need = MIN_BLOCK;

    void *result = NULL;
    pthread_mutex_lock(mutex_of(heap));
    for (Block *b = (Block *)heap->free_head; b; b = links(b)->next) {
        uintptr_t start = (uintptr_t)b, limit = start + bsize(b);
        uintptr_t payload = start + HDR;
        if (payload & (alignment - 1)) payload = align_up(start + MIN_BLOCK + HDR, alignment);
        if (payload - HDR + need > limit) continue;

        list_remove(heap, b);
        size_t gap = (size_t)(payload - HDR - start);
        Block *blk = b;
        if (gap) { /* the front stays free as its own block */
            b->size_used = gap;
            list_insert(heap, b);
            blk = (Block *)(payload - HDR);
            blk->prev_size = gap;
            blk->size_used = limit - (uintptr_t)blk;
        }
        size_t total = bsize(blk);
        if (total - need >= MIN_BLOCK) { /* the tail stays free */
            Block *rest = (Block *)((unsigned char *)blk + need);
            rest->size_used = total - need;
            rest->prev_size = need;
            blk->size_used = need;
            fix_next_prev(heap, rest);
            list_insert(heap, rest);
        } else {
            fix_next_prev(heap, blk);
        }
        blk->size_used |= 1;
        heap->used_bytes += bsize(blk);
        result = blk + 1;
        break;
    }
    pthread_mutex_unlock(mutex_of(heap));
    if (result && zero) memset(result, 0, size);
    return result;
}

static void bad_free(const char *why) {
    pf_write_stderr_str("refueled: pf_lowheap_free: ");
    pf_write_stderr_str(why);
    pf_write_stderr_str("\n");
    abort();
}

void pf_lowheap_free(PfLowHeap *heap, void *pointer) {
    if (!pointer) return;
    if (!heap || !pf_lowheap_contains(heap, pointer) || ((uintptr_t)pointer & 15))
        bad_free("pointer does not belong to this heap");
    Block *b = (Block *)pointer - 1;
    pthread_mutex_lock(mutex_of(heap));
    if (!bused(b)) {
        pthread_mutex_unlock(mutex_of(heap));
        bad_free("double free or invalid pointer");
    }
    heap->used_bytes -= bsize(b);
    b->size_used &= ~(uint64_t)1;
    Block *n = next_block(heap, b);
    if (n && !bused(n)) {
        list_remove(heap, n);
        b->size_used += bsize(n);
    }
    if (b->prev_size) {
        Block *p = (Block *)(void *)((unsigned char *)b - b->prev_size);
        if (!bused(p)) {
            list_remove(heap, p);
            p->size_used += bsize(b);
            b = p;
        }
    }
    fix_next_prev(heap, b);
    list_insert(heap, b);
    pthread_mutex_unlock(mutex_of(heap));
}
