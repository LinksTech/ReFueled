/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: virtual memory, guest physical memory and the low heap.
 *
 * Model (from src/runtime_memory.c): the guest's direct and flexible memory is one "backing"
 * object (Linux: a sparse memfd); any range of it can be viewed at any guest address, several
 * times (aliasing), and through one extra host view (pf_backing_view) whose writes bypass guest
 * and GPU page protection. Guest addresses live below 1 TiB: [low region) for host-owned objects
 * the guest sees (image, stacks, trampolines), [user region) for the guest's own mappings. The
 * runtime decides the layout and tells the platform with pf_vm_reserve_layout(); Windows holds
 * those ranges as placeholders so no host allocation lands in them.
 *
 * Granularity: guest mappings are 16 KiB aligned (PS4 pages); every function here accepts any
 * multiple of pf_page_size() (4 KiB on both platforms). */
#ifndef REFUELED_PF_MEMORY_H
#define REFUELED_PF_MEMORY_H
#include "pf_base.h"

PF_EXTERN_C_BEGIN

/* Protection bits (combine with |). WRITE implies READ on both platforms. */
enum { PF_PROT_NONE = 0, PF_PROT_READ = 1, PF_PROT_WRITE = 2, PF_PROT_EXEC = 4 };

/* Host page size (sysconf(_SC_PAGESIZE) / GetSystemInfo().dwPageSize). */
uint64_t pf_page_size(void);

/* ---- Address-space layout ---------------------------------------------------------------- */

/* Claims [base, base+size) for later fixed mappings; called once per region at start-up, before
 * the first pf_vm_alloc_fixed/pf_backing_map_fixed there. Linux: nothing to do (fixed mappings use
 * MAP_FIXED_NOREPLACE / MAP_FIXED as before), returns 0. Windows: placeholder reservation
 * (VirtualAlloc2 MEM_RESERVE_PLACEHOLDER); PF_EEXIST when part of the range is already in use. */
int pf_vm_reserve_layout(uintptr_t base, uint64_t size);

/* ---- Anonymous memory -------------------------------------------------------------------- */

/* Private zeroed memory at exactly `address`, only if nothing is mapped there
 * (MAP_FIXED_NOREPLACE semantics). PF_EEXIST when occupied, PF_ENOMEM otherwise. */
int pf_vm_alloc_fixed(uintptr_t address, uint64_t size, int prot);

/* Private zeroed memory anywhere (mmap(NULL, ...)); NULL on failure. */
void *pf_vm_alloc(uint64_t size, int prot);

/* Read/write zeroed memory anywhere whose pages are only backed when first touched
 * (MAP_NORESERVE; Windows: reserve + commit on access). For large, sparse tables. NULL on failure. */
void *pf_vm_alloc_lazy(uint64_t size);

/* Releases memory from pf_vm_alloc / pf_vm_alloc_lazy / pf_vm_alloc_fixed (munmap). For
 * pf_vm_alloc_fixed ranges inside a reserved layout region the range returns to the reservation. */
int pf_vm_free(void *address, uint64_t size);

/* Changes the protection of [address, address+size) of any mapping made here or of the
 * executable's own pages (mprotect / VirtualProtect). Also flushes the instruction cache when
 * PF_PROT_EXEC is set. */
int pf_vm_protect(void *address, uint64_t size, int prot);

/* ---- Guest physical memory (the backing object) ------------------------------------------ */

typedef struct PfBacking PfBacking;

/* Creates the backing object of `size` bytes, zero-filled, sparse where the platform allows
 * (Linux: memfd_create("bb-guest-memory") + ftruncate; Windows: pagefile-backed section,
 * CreateFileMapping2 with PAGE_EXECUTE_READWRITE) and its host view of the whole object
 * (read/write, MAP_SHARED|MAP_NORESERVE). Returns 0 or PF_E*. One per process. */
int pf_backing_create(uint64_t size, PfBacking **out);

/* The host view of the whole backing object (offset 0 at the returned address). */
unsigned char *pf_backing_view(PfBacking *backing);

/* Maps backing[offset, offset+size) at guest `address` with `prot`, replacing whatever was mapped
 * there (MAP_SHARED|MAP_FIXED semantics; Windows: placeholder split + MapViewOfFile3 with
 * MEM_REPLACE_PLACEHOLDER, unmapping or splitting earlier views first). `address` must lie in a
 * region claimed with pf_vm_reserve_layout. Returns 0 or PF_E*. */
int pf_backing_map_fixed(PfBacking *backing, uint64_t offset, uintptr_t address, uint64_t size, int prot);

/* Makes backing[offset, offset+size) read as zero and lets the host reclaim its memory where the
 * platform can (Linux: fallocate PUNCH_HOLE|KEEP_SIZE; Windows: zero through the host view). */
int pf_backing_zero(PfBacking *backing, uint64_t offset, uint64_t size);

/* ---- Device memory chunks (Linux dma-buf; Windows: not supported, PF_ENOSYS) --------------- */

/* A handle to memory exported by the GPU driver that can be mapped like the backing object
 * (Linux: a dma-buf fd). PF_MEM_HANDLE_NONE marks "no handle". */
typedef intptr_t PfMemHandle;
#define PF_MEM_HANDLE_NONE ((PfMemHandle)-1)

/* Maps handle[offset, offset+size) at `address`, replacing what is there (MAP_SHARED|MAP_FIXED).
 * Used for guest addresses and for the backing view's range of a chunk. */
int pf_handle_map_fixed(PfMemHandle handle, uint64_t offset, uintptr_t address, uint64_t size, int prot);

/* Maps handle[offset, offset+size) anywhere (probe of a driver's export); NULL on failure. */
void *pf_handle_map(PfMemHandle handle, uint64_t offset, uint64_t size, int prot);

void pf_handle_close(PfMemHandle handle);

/* ---- Guest address ranges ---------------------------------------------------------------- */

/* Replaces [address, address+size) with an inaccessible reservation (guest
 * sceKernelReserveVirtualRange: MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE|MAP_FIXED, PROT_NONE). */
int pf_guest_reserve(uintptr_t address, uint64_t size);

/* Removes guest mappings in [address, address+size) (munmap). The range is free afterwards (on
 * Windows it returns to the layout's placeholder). */
int pf_guest_unmap(uintptr_t address, uint64_t size);

/* ---- Low heap: host objects the guest sees as 40-bit pointers ------------------------------ */

/* Host allocations whose addresses reach the guest (thread handles, TCB/TLS blocks, attribute
 * objects) must lie below 1 TiB. Linux: the C heap already does (non-PIE brk heap, mallopt in
 * pf_init), so these forward to calloc/aligned_alloc/free unchanged. Windows: a heap inside
 * [base, base+size) of the low region, set up by pf_low_heap_init. */
int pf_low_heap_init(uintptr_t base, uint64_t size);
void *pf_low_calloc(size_t count, size_t size);
void *pf_low_aligned_alloc(size_t alignment, size_t size);
void pf_low_free(void *pointer);

/* Needed only on Windows, where the C heap, the exe's _Thread_local data and host thread stacks
 * lie above 1 TiB; on Linux they lie below it already, so the runtime keeps its pre-port layout
 * there (IF-11, ARCHITECTURE.md D21).
 * PF_LOW_HEAP_SIZE: bytes at the top of the low region that go to pf_low_heap_init and are kept out
 * of runtime_low_map (Linux 0: runtime_low_map uses the whole low region, as before).
 * PF_GUEST_ERRNO_IN_TCB: the guest errno cell lives in the TCB block from pf_low_aligned_alloc
 * (Linux 0: a _Thread_local cell as before, in glibc's static TLS on the thread's low stack). */
#ifdef _WIN32
#define PF_LOW_HEAP_SIZE (UINT64_C(512) << 20)
#define PF_GUEST_ERRNO_IN_TCB 1
#else
#define PF_LOW_HEAP_SIZE UINT64_C(0)
#define PF_GUEST_ERRNO_IN_TCB 0
#endif

/* ---- Fault-free access to memory that may be unmapped --------------------------------------- */

/* Copies up to `size` bytes from `source` into `destination` without faulting
 * (process_vm_readv on the own process / ReadProcessMemory). Returns the bytes copied (0 when the
 * first page is unreadable) or PF_E*. */
int64_t pf_read_memory_safe(void *destination, uintptr_t source, uint64_t size);

PF_EXTERN_C_END
#endif
