/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: Linux pf_memory (see pf_memory.h). Moved from src/runtime_memory.c, src/probe.c and
 * the GPU shims, with the same calls and flags; the low heap (pf_low_*) is pf_lowheap_linux.c.
 * Guest addresses are below 1 TiB and every mapping is fixed, so nothing here tracks ranges. */
#include "pf_memory.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int pf_errno(int e) {
    switch (e) {
    case EEXIST: return PF_EEXIST;
    case ENOMEM: return PF_ENOMEM;
    case EINVAL: return PF_EINVAL;
    case EACCES: return PF_EACCES;
    case EPERM: return PF_EPERM;
    case EBADF: return PF_EINVAL;
    case ENOSPC: return PF_ENOSPC;
    case EAGAIN: return PF_EAGAIN;
    case ENOSYS: return PF_ENOSYS;
    default: return PF_EIO;
    }
}

/* PF_PROT_* -> PROT_*; WRITE implies READ (the old host_prot in runtime_memory.c did the same). */
static int host_prot(int prot) {
    return ((prot & (PF_PROT_READ | PF_PROT_WRITE)) ? PROT_READ : 0) |
           ((prot & PF_PROT_WRITE) ? PROT_WRITE : 0) | ((prot & PF_PROT_EXEC) ? PROT_EXEC : 0);
}

uint64_t pf_page_size(void) { return (uint64_t)sysconf(_SC_PAGESIZE); }

/* Fixed mappings use MAP_FIXED_NOREPLACE / MAP_FIXED, so no address range needs claiming. */
int pf_vm_reserve_layout(uintptr_t base, uint64_t size) {
    (void)base;
    (void)size;
    return 0;
}

/* runtime_low_map (runtime_memory.c) mmap with MAP_FIXED_NOREPLACE. A kernel before 4.17 ignores
 * the flag and treats the address as a hint: a different result is an occupied range too. */
int pf_vm_alloc_fixed(uintptr_t address, uint64_t size, int prot) {
    uint64_t page = pf_page_size();
    if (size == 0 || address % page || size % page) return PF_EINVAL;
    void *p = mmap((void *)address, size, host_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return pf_errno(errno);
    if ((uintptr_t)p != address) {
        munmap(p, size);
        return PF_EEXIST;
    }
    return 0;
}

/* probe.c allocate(): mmap(NULL, ...). */
void *pf_vm_alloc(uint64_t size, int prot) {
    void *p = mmap(NULL, size, host_prot(prot), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

/* runtime_memory.c write-watch bitmap: MAP_NORESERVE. */
void *pf_vm_alloc_lazy(uint64_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int pf_vm_free(void *address, uint64_t size) { return munmap(address, size) == 0 ? 0 : pf_errno(errno); }

/* x86 keeps instruction and data caches coherent: no flush for PF_PROT_EXEC. */
int pf_vm_protect(void *address, uint64_t size, int prot) {
    return mprotect(address, size, host_prot(prot)) == 0 ? 0 : pf_errno(errno);
}

/* ---- Guest physical memory --------------------------------------------------------------- */

struct PfBacking {
    int fd;
    unsigned char *view;
    uint64_t size;
};
static struct PfBacking g_backing;
static int g_backing_created;

/* runtime_memory.c pool(): memfd_create + ftruncate + one shared view of all of it. */
int pf_backing_create(uint64_t size, PfBacking **out) {
    if (!out || size == 0) return PF_EINVAL;
    if (g_backing_created) return PF_EBUSY;
    int fd = memfd_create("bb-guest-memory", MFD_CLOEXEC);
    if (fd < 0) return pf_errno(errno);
    if (ftruncate(fd, (off_t)size) != 0) {
        int e = pf_errno(errno);
        close(fd);
        return e;
    }
    void *view = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_NORESERVE, fd, 0);
    if (view == MAP_FAILED) {
        int e = pf_errno(errno);
        close(fd);
        return e;
    }
    g_backing.fd = fd;
    g_backing.view = view;
    g_backing.size = size;
    g_backing_created = 1;
    *out = &g_backing;
    return 0;
}

unsigned char *pf_backing_view(PfBacking *backing) { return backing->view; }

/* runtime_memory.c map_phys: MAP_SHARED|MAP_FIXED replaces whatever was mapped. */
int pf_backing_map_fixed(PfBacking *backing, uint64_t offset, uintptr_t address, uint64_t size, int prot) {
    if (offset + size > backing->size || offset + size < offset) return PF_EINVAL;
    void *p = mmap((void *)address, size, host_prot(prot), MAP_SHARED | MAP_FIXED, backing->fd, (off_t)offset);
    return p == MAP_FAILED ? pf_errno(errno) : 0;
}

/* runtime_memory.c zero_phys: hole punching. */
int pf_backing_zero(PfBacking *backing, uint64_t offset, uint64_t size) {
    if (offset + size > backing->size || offset + size < offset) return PF_EINVAL;
    return fallocate(backing->fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, (off_t)offset, (off_t)size) == 0
               ? 0 : pf_errno(errno);
}

/* ---- Device memory chunks (dma-buf file descriptors) ------------------------------------- */

/* runtime_memory.c add_region / ensure_chunks / map_phys: MAP_SHARED|MAP_FIXED on the chunk fd. */
int pf_handle_map_fixed(PfMemHandle handle, uint64_t offset, uintptr_t address, uint64_t size, int prot) {
    if (handle == PF_MEM_HANDLE_NONE) return PF_EINVAL;
    void *p = mmap((void *)address, size, host_prot(prot), MAP_SHARED | MAP_FIXED, (int)handle, (off_t)offset);
    return p == MAP_FAILED ? pf_errno(errno) : 0;
}

/* bbport_guest_memory.cpp: probes of a driver's export, mmap(nullptr, ..., MAP_SHARED, fd, off). */
void *pf_handle_map(PfMemHandle handle, uint64_t offset, uint64_t size, int prot) {
    if (handle == PF_MEM_HANDLE_NONE) return NULL;
    void *p = mmap(NULL, size, host_prot(prot), MAP_SHARED, (int)handle, (off_t)offset);
    return p == MAP_FAILED ? NULL : p;
}

void pf_handle_close(PfMemHandle handle) {
    if (handle != PF_MEM_HANDLE_NONE) close((int)handle);
}

/* ---- Guest address ranges ---------------------------------------------------------------- */

/* runtime_memory.c place() for KIND_RESERVED. */
int pf_guest_reserve(uintptr_t address, uint64_t size) {
    void *p = mmap((void *)address, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
    return p == MAP_FAILED ? pf_errno(errno) : 0;
}

int pf_guest_unmap(uintptr_t address, uint64_t size) {
    return munmap((void *)address, size) == 0 ? 0 : pf_errno(errno);
}

/* ---- Fault-free access ------------------------------------------------------------------- */

/* page_manager.cpp / liverpool.cpp: process_vm_readv on the own process. The kernel copies the
 * pages that are readable and reports the bytes done; only when nothing was copied does it fail
 * (EFAULT -> 0, "the first page is unreadable"). */
int64_t pf_read_memory_safe(void *destination, uintptr_t source, uint64_t size) {
    if (size == 0) return 0;
    struct iovec local = {destination, (size_t)size};
    struct iovec remote = {(void *)source, (size_t)size};
    ssize_t n = process_vm_readv(getpid(), &local, 1, &remote, 1, 0);
    if (n >= 0) return (int64_t)n;
    return errno == EFAULT ? 0 : pf_errno(errno);
}
