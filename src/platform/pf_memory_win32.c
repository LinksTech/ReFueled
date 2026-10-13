/* SPDX-License-Identifier: GPL-2.0-or-later */
/* refueled: virtual memory on Windows (pf_memory.h).
 *
 * The layout regions (pf_vm_reserve_layout) are held as placeholders (VirtualAlloc2 with
 * MEM_RESERVE_PLACEHOLDER), so nothing else lands there and any 4 KiB-aligned piece can be
 * replaced by a view. A region table (sorted array) describes every byte of the layout: a free
 * placeholder, a guest reservation (also a placeholder), or a view of a section at an offset.
 * Each table entry is exactly one OS object, so a mapping over part of an entry first splits it:
 * placeholders with VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER), views by unmapping them
 * to a placeholder, splitting that and mapping the pieces again at their offsets, with the page
 * protections they had. Adjacent placeholders inside a range are merged with
 * MEM_COALESCE_PLACEHOLDERS before a view replaces them.
 * The scheme follows shadPS4's AddressSpace::Impl (src/core/address_space.cpp at shadPS4 b4e7ae7:
 * Map, SplitRegion, CoalesceFreeRegions, UnmapRegion), rewritten in C with two differences:
 * anonymous fixed memory is a view of its own pagefile section too (so it can be freed in part
 * and split without losing its contents, which a private VirtualAlloc2 allocation cannot), and
 * splitting a view restores per-page protections (guest mprotect, GPU page tracking).
 *
 * The backing object is one pagefile-backed section (SEC_COMMIT, PAGE_EXECUTE_READWRITE), viewed
 * once in full for the host. Views are always mapped PAGE_EXECUTE_READWRITE and then reduced with
 * VirtualProtect, because a view's protection can later only be changed within the access it was
 * mapped with.
 *
 * Locking: one SRW lock serialises the table. pf_vm_protect takes no lock (it runs in the GPU
 * fault path); runtime_memory.c already orders its protection changes against its own mapping
 * changes. A view being split is gone for a moment; pf_win32_memory_transient_fault lets the
 * fault dispatcher retry accesses that hit that window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pf_log.h"
#include "pf_memory.h"
#include "pf_win32_internal.h"

/* PfAccess values (pf_exception.h; not included here so MSVC can check this file). */
enum { ACCESS_UNKNOWN = 0, ACCESS_READ = 1, ACCESS_WRITE = 2, ACCESS_EXEC = 3 };

#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

/* Windows 10 1803+ (kernelbase.dll). Loaded at run time: the import libraries of MinGW-w64 and of
 * older SDKs do not all declare them. */
typedef PVOID(WINAPI *VirtualAlloc2Fn)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef PVOID(WINAPI *MapViewOfFile3Fn)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, void *, ULONG);
typedef BOOL(WINAPI *UnmapViewOfFile2Fn)(HANDLE, PVOID, ULONG);
static VirtualAlloc2Fn virtual_alloc2;
static MapViewOfFile3Fn map_view_of_file3;
static UnmapViewOfFile2Fn unmap_view_of_file2;
static INIT_ONCE api_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK load_api(PINIT_ONCE once, PVOID parameter, PVOID *context) {
    (void)once;
    (void)parameter;
    (void)context;
    HMODULE kernelbase = GetModuleHandleW(L"kernelbase.dll");
    if (kernelbase) {
        virtual_alloc2 = (VirtualAlloc2Fn)(void (*)(void))GetProcAddress(kernelbase, "VirtualAlloc2");
        map_view_of_file3 = (MapViewOfFile3Fn)(void (*)(void))GetProcAddress(kernelbase, "MapViewOfFile3");
        unmap_view_of_file2 = (UnmapViewOfFile2Fn)(void (*)(void))GetProcAddress(kernelbase, "UnmapViewOfFile2");
    }
    return TRUE;
}

static int api_ready(void) {
    InitOnceExecuteOnce(&api_once, load_api, NULL, NULL);
    if (virtual_alloc2 && map_view_of_file3 && unmap_view_of_file2) return 1;
    pf_write_stderr_str("refueled: VirtualAlloc2/MapViewOfFile3 missing (Windows 10 1803 or later needed)\n");
    return 0;
}

static void report(const char *what, uintptr_t address, uint64_t size) {
    char line[200];
    const DWORD error = GetLastError();
    int n = snprintf(line, sizeof line, "refueled: %s failed at %#" PRIxPTR ", size %#" PRIx64 " (Windows error %lu)\n",
                     what, address, size, (unsigned long)error);
    if (n > 0) pf_write_stderr(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static int valid_prot(int prot) { return !(prot & ~(PF_PROT_READ | PF_PROT_WRITE | PF_PROT_EXEC)); }

static DWORD win_prot(int prot) {
    const int write = prot & PF_PROT_WRITE, read = prot & (PF_PROT_READ | PF_PROT_WRITE);
    if (prot & PF_PROT_EXEC) return write ? PAGE_EXECUTE_READWRITE : read ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    return write ? PAGE_READWRITE : read ? PAGE_READONLY : PAGE_NOACCESS;
}

uint64_t pf_page_size(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}

/* ---- Sections ------------------------------------------------------------------------------ */

typedef struct Section {
    HANDLE handle;
    long views;    /* table entries viewing it (under the table lock) */
    int permanent; /* the backing object: never closed */
} Section;

static Section *section_new(uint64_t size, int permanent) {
    HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
                                       (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!handle) {
        report("CreateFileMapping", 0, size);
        return NULL;
    }
    Section *section = malloc(sizeof *section);
    if (!section) {
        CloseHandle(handle);
        return NULL;
    }
    section->handle = handle;
    section->views = 0;
    section->permanent = permanent;
    return section;
}

static void section_release(Section *section) {
    if (section && --section->views == 0 && !section->permanent) {
        CloseHandle(section->handle);
        free(section);
    }
}

/* ---- Region table -------------------------------------------------------------------------- */

enum { REGION_FREE, REGION_RESERVED, REGION_VIEW };

typedef struct {
    uintptr_t base;
    uint64_t size;
    Section *section; /* REGION_VIEW only */
    uint64_t offset;  /* REGION_VIEW: offset of base in the section */
    int kind;
    int layout;       /* index into layouts */
} Region;

typedef struct {
    uintptr_t base, end;
} Layout;

#define MAX_LAYOUTS 8
static Layout layouts[MAX_LAYOUTS];
static int layout_count;
static Region *regions;
static size_t region_count, region_capacity;
static SRWLOCK table_lock = SRWLOCK_INIT;

/* Space for `extra` more entries, reserved before any OS call so no insert fails half-way. */
static int reserve_entries(size_t extra) {
    if (region_count + extra <= region_capacity) return 0;
    size_t capacity = region_capacity ? region_capacity * 2 : 64;
    while (capacity < region_count + extra) capacity *= 2;
    Region *grown = realloc(regions, capacity * sizeof *grown);
    if (!grown) return PF_ENOMEM;
    regions = grown;
    region_capacity = capacity;
    return 0;
}

static void insert_entry(size_t index, Region region) {
    memmove(regions + index + 1, regions + index, (region_count - index) * sizeof *regions);
    regions[index] = region;
    ++region_count;
}

static void erase_entries(size_t index, size_t count) {
    memmove(regions + index, regions + index + count, (region_count - index - count) * sizeof *regions);
    region_count -= count;
}

/* The entry containing `address` (which must lie in a layout region). */
static size_t find_entry(uintptr_t address) {
    size_t low = 0, high = region_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (regions[middle].base <= address) low = middle + 1;
        else high = middle;
    }
    return low - 1;
}

/* Layout index containing [address, end), -1 when it lies outside every layout region, -2 when
 * it crosses a layout boundary. */
static int layout_of(uintptr_t address, uintptr_t end) {
    for (int i = 0; i < layout_count; ++i) {
        if (address >= layouts[i].base && end <= layouts[i].end) return i;
        if (address < layouts[i].end && end > layouts[i].base) return -2;
    }
    return -1;
}

/* ---- Views and protections ------------------------------------------------------------------ */

typedef struct {
    uintptr_t base;
    uint64_t size;
    DWORD protect;
} ProtRun;

/* The protection runs of [address, end) as VirtualQuery reports them; *runs is malloc'd. */
static int capture_protections(uintptr_t address, uintptr_t end, ProtRun **runs, size_t *count) {
    size_t capacity = 8, n = 0;
    ProtRun *list = malloc(capacity * sizeof *list);
    if (!list) return PF_ENOMEM;
    for (uintptr_t at = address; at < end;) {
        MEMORY_BASIC_INFORMATION info;
        if (!VirtualQuery((void *)at, &info, sizeof info)) {
            free(list);
            return PF_EFAULT;
        }
        uintptr_t stop = (uintptr_t)info.BaseAddress + info.RegionSize;
        if (stop > end) stop = end;
        if (n == capacity) {
            ProtRun *grown = realloc(list, capacity * 2 * sizeof *grown);
            if (!grown) {
                free(list);
                return PF_ENOMEM;
            }
            list = grown;
            capacity *= 2;
        }
        list[n++] = (ProtRun){at, stop - at, info.Protect};
        at = stop;
    }
    *runs = list;
    *count = n;
    return 0;
}

static void restore_protections(const ProtRun *runs, size_t count, uintptr_t address, uintptr_t end) {
    for (size_t i = 0; i < count; ++i) {
        uintptr_t a = runs[i].base > address ? runs[i].base : address;
        uintptr_t b = runs[i].base + runs[i].size < end ? runs[i].base + runs[i].size : end;
        DWORD old;
        if (a < b && runs[i].protect != PAGE_EXECUTE_READWRITE && !VirtualProtect((void *)a, b - a, runs[i].protect, &old))
            report("VirtualProtect (restore)", a, b - a);
    }
}

/* Replaces the placeholder [address, address+size) with a view of section[offset, ...). */
static int map_view(Section *section, uint64_t offset, uintptr_t address, uint64_t size, DWORD protect) {
    void *view = map_view_of_file3(section->handle, GetCurrentProcess(), (void *)address, offset, (SIZE_T)size,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE, NULL, 0);
    if (view != (void *)address) {
        report("MapViewOfFile3", address, size);
        return PF_ENOMEM;
    }
    DWORD old;
    if (protect != PAGE_EXECUTE_READWRITE && !VirtualProtect(view, (SIZE_T)size, protect, &old)) {
        report("VirtualProtect", address, size);
        return PF_EACCES;
    }
    return 0;
}

static int split_placeholder(uintptr_t base, uintptr_t at) {
    if (!VirtualFree((void *)base, at - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        report("placeholder split", base, at - base);
        return PF_EINVAL;
    }
    return 0;
}

/* Makes `at` the start of a table entry. Needs one free entry (reserve_entries). */
static int split_at(uintptr_t at) {
    size_t i = find_entry(at);
    Region *region = &regions[i];
    if (region->base == at) return 0;
    const uintptr_t base = region->base, end = base + region->size;
    Region right = *region;
    right.base = at;
    right.size = end - at;
    if (region->kind != REGION_VIEW) {
        int error = split_placeholder(base, at);
        if (error) return error;
        region->size = at - base;
        insert_entry(i + 1, right);
        return 0;
    }
    ProtRun *runs;
    size_t run_count;
    int error = capture_protections(base, end, &runs, &run_count);
    if (error) return error;
    if (!unmap_view_of_file2(GetCurrentProcess(), (void *)base, MEM_PRESERVE_PLACEHOLDER)) {
        report("UnmapViewOfFile2 (split)", base, end - base);
        free(runs);
        return PF_EINVAL;
    }
    error = split_placeholder(base, at);
    if (!error) error = map_view(region->section, region->offset, base, at - base, PAGE_EXECUTE_READWRITE);
    if (!error) error = map_view(region->section, region->offset + (at - base), at, end - at, PAGE_EXECUTE_READWRITE);
    if (error) {
        /* Not recoverable without losing the table's agreement with the OS; the message above
         * names the call. The process cannot continue safely. */
        free(runs);
        pf_write_stderr_str("refueled: splitting a guest mapping failed; exiting\n");
        ExitProcess(70);
    }
    restore_protections(runs, run_count, base, at); /* one view per call */
    restore_protections(runs, run_count, at, end);
    free(runs);
    region->size = at - base;
    right.offset = region->offset + (at - base);
    ++region->section->views;
    insert_entry(i + 1, right);
    return 0;
}

/* Turns [address, end) (inside one layout region) into a single free placeholder entry and returns
 * its index in *index. Views inside are unmapped, views across the edges keep their outer parts.
 * Needs two free entries (reserve_entries). */
static int clear_range(uintptr_t address, uintptr_t end, size_t *index) {
    const Layout *layout = &layouts[regions[find_entry(address)].layout];
    int error = split_at(address);
    if (!error && end < layout->end) error = split_at(end);
    if (error) return error;
    size_t first = find_entry(address), next = first;
    for (; next < region_count && regions[next].base < end; ++next) {
        Region *region = &regions[next];
        if (region->kind == REGION_VIEW) {
            if (!unmap_view_of_file2(GetCurrentProcess(), (void *)region->base, MEM_PRESERVE_PLACEHOLDER)) {
                report("UnmapViewOfFile2", region->base, region->size);
                return PF_EINVAL;
            }
            section_release(region->section);
            region->section = NULL;
            region->offset = 0;
        }
        region->kind = REGION_FREE;
    }
    if (next - first > 1) {
        if (!VirtualFree((void *)address, end - address, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            report("placeholder coalesce", address, end - address);
            return PF_EINVAL;
        }
        regions[first].size = end - address;
        erase_entries(first + 1, next - first - 1);
    }
    *index = first;
    return 0;
}

/* Merges the free entry `index` with free neighbours of the same layout region. */
static void merge_free(size_t index) {
    if (index > 0 && regions[index - 1].kind == REGION_FREE && regions[index - 1].layout == regions[index].layout) {
        Region *left = &regions[index - 1];
        if (VirtualFree((void *)left->base, left->size + regions[index].size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            left->size += regions[index].size;
            erase_entries(index, 1);
            --index;
        }
    }
    if (index + 1 < region_count && regions[index + 1].kind == REGION_FREE &&
        regions[index + 1].layout == regions[index].layout) {
        Region *region = &regions[index];
        if (VirtualFree((void *)region->base, region->size + regions[index + 1].size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
            region->size += regions[index + 1].size;
            erase_entries(index + 1, 1);
        }
    }
}

/* ---- Accesses during a remap ---------------------------------------------------------------- */

static volatile LONG remap_active;
static volatile uintptr_t remap_low, remap_high; /* the current, else the last, remapped window */

/* Called with the table lock held, around operations that unmap views another thread may use. */
static void remap_begin(uintptr_t address, uintptr_t end) {
    size_t first = find_entry(address), last = find_entry(end - 1);
    remap_low = regions[first].base;
    remap_high = regions[last].base + regions[last].size;
    MemoryBarrier();
    InterlockedExchange(&remap_active, 1);
}

static void remap_end(void) { InterlockedExchange(&remap_active, 0); }

static int protect_allows(DWORD protect, int access) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
    protect &= 0xff;
    const int writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE ||
                         protect == PAGE_EXECUTE_WRITECOPY;
    const int executable = protect == PAGE_EXECUTE || protect == PAGE_EXECUTE_READ || protect == PAGE_EXECUTE_READWRITE ||
                           protect == PAGE_EXECUTE_WRITECOPY;
    if (access == ACCESS_WRITE) return writable;
    if (access == ACCESS_EXEC) return executable;
    return protect != PAGE_EXECUTE;
}

int pf_win32_memory_transient_fault(uintptr_t address, int access) {
    if (address < remap_low || address >= remap_high) return 0;
    while (remap_active) SwitchToThread();
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)address, &info, sizeof info) || info.State != MEM_COMMIT) return 0;
    return protect_allows(info.Protect, access);
}

/* ---- Address-space layout ------------------------------------------------------------------ */

int pf_vm_reserve_layout(uintptr_t base, uint64_t size) {
    if (!size || (base | size) & 0xffff) return PF_EINVAL;
    if (!api_ready()) return PF_ENOSYS;
    int error = 0;
    AcquireSRWLockExclusive(&table_lock);
    if (layout_of(base, base + size) != -1) error = PF_EEXIST;
    else if (layout_count == MAX_LAYOUTS) error = PF_ENOSPC;
    else if (!(error = reserve_entries(1))) {
        void *p = virtual_alloc2(GetCurrentProcess(), (void *)base, (SIZE_T)size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, NULL, 0);
        if (p != (void *)base) {
            report("layout reservation", base, size);
            error = PF_EEXIST;
        } else {
            layouts[layout_count] = (Layout){base, base + size};
            size_t index = 0;
            while (index < region_count && regions[index].base < base) ++index;
            insert_entry(index, (Region){base, size, NULL, 0, REGION_FREE, layout_count});
            ++layout_count;
        }
    }
    ReleaseSRWLockExclusive(&table_lock);
    return error;
}

/* ---- Anonymous memory -------------------------------------------------------------------- */

static int page_aligned(uintptr_t address, uint64_t size) { return size && !((address | size) & 0xfff); }

int pf_vm_alloc_fixed(uintptr_t address, uint64_t size, int prot) {
    if (!page_aligned(address, size) || !valid_prot(prot)) return PF_EINVAL;
    const uintptr_t end = address + size;
    const int layout = layout_of(address, end);
    if (layout == -2) return PF_EINVAL;
    if (layout == -1) {
        /* Outside the layout (e.g. stubs placed near the image): an ordinary allocation, which
         * Windows only places at allocation-granularity (64 KiB) addresses. */
        if (address & 0xffff) return PF_EINVAL;
        if (VirtualAlloc((void *)address, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, win_prot(prot))) return 0;
        return GetLastError() == ERROR_INVALID_ADDRESS ? PF_EEXIST : PF_ENOMEM;
    }
    int error = 0;
    AcquireSRWLockExclusive(&table_lock);
    for (size_t i = find_entry(address); i < region_count && regions[i].base < end; ++i)
        if (regions[i].kind != REGION_FREE) error = PF_EEXIST;
    Section *section = NULL;
    size_t index = 0;
    if (!error) error = reserve_entries(2);
    if (!error && !(section = section_new(size, 0))) error = PF_ENOMEM;
    if (!error) error = clear_range(address, end, &index);
    if (!error) error = map_view(section, 0, address, size, win_prot(prot));
    if (!error) {
        regions[index].kind = REGION_VIEW;
        regions[index].section = section;
        regions[index].offset = 0;
        section->views = 1;
    } else if (section) {
        CloseHandle(section->handle);
        free(section);
    }
    ReleaseSRWLockExclusive(&table_lock);
    return error;
}

void *pf_vm_alloc(uint64_t size, int prot) {
    if (!size || !valid_prot(prot)) return NULL;
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, win_prot(prot));
}

/* Committed at once: commit charge only, physical pages appear on first touch. */
void *pf_vm_alloc_lazy(uint64_t size) {
    if (!size) return NULL;
    return VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

/* Inside the layout: [address, end) becomes a placeholder of `kind`. */
static int release_range(uintptr_t address, uintptr_t end, int kind) {
    int error;
    size_t index = 0;
    AcquireSRWLockExclusive(&table_lock);
    error = reserve_entries(2);
    if (!error) {
        remap_begin(address, end);
        error = clear_range(address, end, &index);
        remap_end();
    }
    if (!error) {
        regions[index].kind = kind;
        if (kind == REGION_FREE) merge_free(index);
    }
    ReleaseSRWLockExclusive(&table_lock);
    return error;
}

/* Outside the layout (pf_vm_alloc, pf_vm_alloc_lazy, pf_vm_alloc_fixed there): the whole
 * allocation is released, a part of it decommitted (inaccessible, as after munmap). */
static int free_outside(uintptr_t address, uint64_t size) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery((void *)address, &info, sizeof info)) return PF_EINVAL;
    if (info.State == MEM_FREE) return 0;
    uintptr_t allocation = (uintptr_t)info.AllocationBase, end = address;
    while (VirtualQuery((void *)end, &info, sizeof info) && (uintptr_t)info.AllocationBase == allocation &&
           info.State != MEM_FREE)
        end = (uintptr_t)info.BaseAddress + info.RegionSize;
    if (address == allocation && address + size >= end) {
        if (!VirtualFree((void *)address, 0, MEM_RELEASE)) return PF_EINVAL;
        return 0;
    }
    if (!VirtualFree((void *)address, (SIZE_T)size, MEM_DECOMMIT)) return PF_EINVAL;
    return 0;
}

int pf_vm_free(void *address, uint64_t size) {
    const uintptr_t start = (uintptr_t)address;
    if (!page_aligned(start, size)) return PF_EINVAL;
    AcquireSRWLockShared(&table_lock);
    const int layout = layout_of(start, start + size);
    ReleaseSRWLockShared(&table_lock);
    if (layout == -2) return PF_EINVAL;
    return layout >= 0 ? release_range(start, start + size, REGION_FREE) : free_outside(start, size);
}

int pf_vm_protect(void *address, uint64_t size, int prot) {
    if (!size || !valid_prot(prot)) return PF_EINVAL;
    const DWORD protect = win_prot(prot);
    DWORD old;
    if (!VirtualProtect(address, (SIZE_T)size, protect, &old)) {
        /* The range spans several allocations or views: one call per piece. */
        const uintptr_t end = (uintptr_t)address + size;
        for (uintptr_t at = (uintptr_t)address; at < end;) {
            MEMORY_BASIC_INFORMATION info;
            if (!VirtualQuery((void *)at, &info, sizeof info) || info.State != MEM_COMMIT) return PF_ENOMEM;
            uintptr_t stop = (uintptr_t)info.BaseAddress + info.RegionSize;
            if (stop > end) stop = end;
            if (!VirtualProtect((void *)at, stop - at, protect, &old)) return PF_EACCES;
            at = stop;
        }
    }
    if (prot & PF_PROT_EXEC) FlushInstructionCache(GetCurrentProcess(), address, (SIZE_T)size);
    return 0;
}

/* ---- Guest physical memory --------------------------------------------------------------- */

struct PfBacking {
    Section *section;
    unsigned char *view;
    uint64_t size;
};
static struct PfBacking backing_object;

int pf_backing_create(uint64_t size, PfBacking **out) {
    if (!size || !out) return PF_EINVAL;
    if (!api_ready()) return PF_ENOSYS;
    if (backing_object.section) return PF_EBUSY;
    Section *section = section_new(size, 1);
    if (!section) return PF_ENOMEM;
    void *view = MapViewOfFile(section->handle, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)size);
    if (!view) {
        report("MapViewOfFile (backing view)", 0, size);
        CloseHandle(section->handle);
        free(section);
        return PF_ENOMEM;
    }
    backing_object = (struct PfBacking){section, view, size};
    *out = &backing_object;
    return 0;
}

unsigned char *pf_backing_view(PfBacking *backing) { return backing ? backing->view : NULL; }

int pf_backing_map_fixed(PfBacking *backing, uint64_t offset, uintptr_t address, uint64_t size, int prot) {
    if (!backing || !page_aligned(address, size) || (offset & 0xfff) || offset > backing->size ||
        size > backing->size - offset || !valid_prot(prot))
        return PF_EINVAL;
    const uintptr_t end = address + size;
    int error;
    size_t index = 0;
    AcquireSRWLockExclusive(&table_lock);
    if (layout_of(address, end) < 0) error = PF_EINVAL;
    else error = reserve_entries(2);
    if (!error) {
        remap_begin(address, end);
        error = clear_range(address, end, &index);
        if (!error) error = map_view(backing->section, offset, address, size, win_prot(prot));
        if (!error) {
            regions[index].kind = REGION_VIEW;
            regions[index].section = backing->section;
            regions[index].offset = offset;
            ++backing->section->views;
        }
        remap_end();
    }
    ReleaseSRWLockExclusive(&table_lock);
    return error;
}

/* No hole punching for pagefile sections: the pages are cleared and stay committed. */
int pf_backing_zero(PfBacking *backing, uint64_t offset, uint64_t size) {
    if (!backing || offset > backing->size || size > backing->size - offset) return PF_EINVAL;
    memset(backing->view + offset, 0, (size_t)size);
    return 0;
}

/* ---- Device memory chunks: not supported on Windows (ARCHITECTURE.md D12) ------------------ */

int pf_handle_map_fixed(PfMemHandle handle, uint64_t offset, uintptr_t address, uint64_t size, int prot) {
    (void)handle;
    (void)offset;
    (void)address;
    (void)size;
    (void)prot;
    return pf_not_implemented(__func__);
}

void *pf_handle_map(PfMemHandle handle, uint64_t offset, uint64_t size, int prot) {
    (void)handle;
    (void)offset;
    (void)size;
    (void)prot;
    pf_not_implemented(__func__);
    return NULL;
}

void pf_handle_close(PfMemHandle handle) { (void)handle; }

/* ---- Guest address ranges ---------------------------------------------------------------- */

int pf_guest_reserve(uintptr_t address, uint64_t size) {
    if (!page_aligned(address, size)) return PF_EINVAL;
    AcquireSRWLockShared(&table_lock);
    const int layout = layout_of(address, address + size);
    ReleaseSRWLockShared(&table_lock);
    return layout >= 0 ? release_range(address, address + size, REGION_RESERVED) : PF_EINVAL;
}

int pf_guest_unmap(uintptr_t address, uint64_t size) { return pf_vm_free((void *)address, size); }

/* ---- Fault-free reads ---------------------------------------------------------------------- */

int64_t pf_read_memory_safe(void *destination, uintptr_t source, uint64_t size) {
    unsigned char *out = destination;
    uint64_t done = 0;
    while (done < size) {
        const uintptr_t at = source + done;
        uint64_t chunk = 0x1000 - (at & 0xfff);
        if (chunk > size - done) chunk = size - done;
        SIZE_T copied = 0;
        const BOOL ok = ReadProcessMemory(GetCurrentProcess(), (const void *)at, out + done, (SIZE_T)chunk, &copied);
        done += copied;
        if (!ok || copied < chunk) break;
    }
    return (int64_t)done;
}
