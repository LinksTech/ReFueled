// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_guest_hooks.h"
#include "bbport_heap_sites.h"
#include "bbport_toggles.h"

#include "platform/pf_exception.h"
#include "platform/pf_memory.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>

extern "C" void runtime_memory_note_write(uintptr_t address, uint64_t size);

namespace BbGuestHooks {
namespace {
using u64 = std::uint64_t;
constexpr u64 ImageBase = 0x800000000ull;

/// The return of the game's GPU memory range allocator 0x26aa070 (rdi allocator, rsi size,
/// rdx alignment; it returns a node: [node + 8] the start, [[node + 0x18] + 8] the end, the next
/// node's start): `add rsp, 0x18` in its epilogue, reached by its own branches only.
constexpr u64 AllocReturn = 0x26aa255;
constexpr unsigned char AllocReturnCode[4] = {0x48, 0x83, 0xc4, 0x18};
RangeCallback on_range_allocated = nullptr;

/// The game's other GPU memory heap (TLSF-like, headers in the memory: free at 0x20858a0, alloc at
/// 0x2085a60), reached through a pointer at 0x54b9f38 set at startup. Its only three callers
/// (wrappers that record {address, size} in a map) pass r8 = rbp - 0x50 for the size handed out and
/// follow the call with `mov rbx, rax` (rax: the address).
constexpr std::array<u64, 3> HeapAllocReturns = {0x11e6629, 0x11e6736, 0x11e6846};
constexpr unsigned char HeapAllocReturnCode[3] = {0x48, 0x89, 0xc3};

/// BB_FRAME_STATS: the range allocator's collector (0x26aa860, `push rbp`; rdi the allocator): its
/// live range count [+0xc8] and byte count [+0xd0] are noted for the frame stats.
constexpr u64 CollectorEntry = 0x26aa860;
constexpr unsigned char CollectorEntryCode[1] = {0x55};

/// BB_HEAP_SITES=1: the game's deferred object release (0xbb5f10) skips the destruction when the
/// registry check (0xbf0790) fails: `js 0xbb5fc7` there, its result counted (eax).
constexpr u64 ReleaseCheck = 0xbb5fb0;
constexpr unsigned char ReleaseCheckCode[2] = {0x78, 0x15};

/// `call memcpy` (0x261a1b0: rdi destination, rsi source, rdx size) in the resource loaders:
/// 0x216d420 and 0x2171d60 copy texture and buffer data into GPU memory (most of the write
/// faults while areas stream in).
struct CallSite {
    u64 offset;
    unsigned char code[5];
};
constexpr std::array<CallSite, 2> MemcpySites = {{
    {0x216d5ea, {0xe8, 0xc1, 0xcb, 0x4a, 0x00}},
    {0x2171fea, {0xe8, 0xc1, 0x81, 0x4a, 0x00}},
}};
std::array<std::atomic<u64>, MemcpySites.size()> hits{};

/// The game's memcpy at those sites: the copy, then the GPU side is told of the data now there.
__attribute__((sysv_abi)) void* LoaderCopy(void* dst, const void* src, std::size_t size) {
    std::memcpy(dst, src, size);
    runtime_memory_note_write(reinterpret_cast<u64>(dst), size);
    return dst;
}
// refueled: the SIGTRAP action and its chain to the previous one moved into pf_exception; this is
// a handler of the shared dispatcher (offered in registration order, CONTINUE_SEARCH chains on).
PfFaultResult OnTrap(const PfFault*, PfContext* context, void*) {
    const u64 rip = pf_context_get(context, PF_REG_RIP) - 1; // past the int3
    if (rip == ImageBase + ReleaseCheck) {
        const auto result = static_cast<std::int32_t>(pf_context_get(context, PF_REG_RAX) & 0xffffffff);
        BbHeapSites::NoteReleaseCheck(std::uint32_t(result));
        pf_context_set(context, PF_REG_RIP, ImageBase + (result < 0 ? 0xbb5fc7 : ReleaseCheck + 2));
        return PF_FAULT_HANDLED;
    }
    if (rip == ImageBase + CollectorEntry) {
        const u64 allocator = pf_context_get(context, PF_REG_RDI);
        for (std::size_t i = 0; i < BbStats::range_allocators.size(); ++i) {
            u64 expected = 0;
            if (BbStats::range_allocators[i].load(std::memory_order_relaxed) == allocator ||
                BbStats::range_allocators[i].compare_exchange_strong(expected, allocator)) {
                BbStats::range_live[i].store(*reinterpret_cast<const u64*>(allocator + 0xc8),
                                             std::memory_order_relaxed);
                BbStats::range_bytes[i].store(*reinterpret_cast<const u64*>(allocator + 0xd0),
                                              std::memory_order_relaxed);
                break;
            }
        }
        const u64 rsp = pf_context_get(context, PF_REG_RSP) - 8; // push rbp
        pf_context_set(context, PF_REG_RSP, rsp);
        *reinterpret_cast<u64*>(rsp) = pf_context_get(context, PF_REG_RBP);
        pf_context_set(context, PF_REG_RIP, ImageBase + CollectorEntry + 1);
        return PF_FAULT_HANDLED;
    }
    for (const u64 site : HeapAllocReturns) {
        if (rip != ImageBase + site) {
            continue;
        }
        const u64 address = pf_context_get(context, PF_REG_RAX);
        const u64 size = *reinterpret_cast<const u64*>(pf_context_get(context, PF_REG_RBP) - 0x50);
        if (address != 0 && size != 0 && size < (u64(1) << 32)) {
            on_range_allocated(address, size);
        }
        pf_context_set(context, PF_REG_RBX, pf_context_get(context, PF_REG_RAX));
        pf_context_set(context, PF_REG_RIP, ImageBase + site + sizeof(HeapAllocReturnCode));
        return PF_FAULT_HANDLED;
    }
    if (rip == ImageBase + AllocReturn) {
        if (const u64 node = pf_context_get(context, PF_REG_RAX)) {
            const u64 start = *reinterpret_cast<const u64*>(node + 8);
            const u64 next = *reinterpret_cast<const u64*>(node + 0x18);
            const u64 end = next ? *reinterpret_cast<const u64*>(next + 8) : start;
            if (end > start && end - start < (u64(1) << 32)) {
                on_range_allocated(start, end - start);
            }
        }
        pf_context_set(context, PF_REG_RSP, pf_context_get(context, PF_REG_RSP) + 0x18);
        pf_context_set(context, PF_REG_RIP, ImageBase + AllocReturn + sizeof(AllocReturnCode));
        return PF_FAULT_HANDLED;
    }
    for (std::size_t i = 0; i < MemcpySites.size(); ++i) {
        const auto& site = MemcpySites[i];
        if (rip != ImageBase + site.offset) {
            continue;
        }
        hits[i].fetch_add(1, std::memory_order_relaxed);
        // The call, to LoaderCopy instead of the game's memcpy: push the return address, jump.
        const u64 ret = ImageBase + site.offset + 5;
        const u64 rsp = pf_context_get(context, PF_REG_RSP) - 8;
        pf_context_set(context, PF_REG_RSP, rsp);
        *reinterpret_cast<u64*>(rsp) = ret;
        pf_context_set(context, PF_REG_RIP, reinterpret_cast<u64>(&LoaderCopy));
        return PF_FAULT_HANDLED;
    }
    return PF_FAULT_CONTINUE_SEARCH; // not ours: the dispatcher chains to the previous action
}

bool Patch(u64 address, const unsigned char* expected, std::size_t size) {
    unsigned char bytes[16]{};
    if (pf_read_memory_safe(bytes, address, size) != std::int64_t(size) ||
        std::memcmp(bytes, expected, size) != 0) {
        return false;
    }
    const u64 page_size = pf_page_size();
    void* page = reinterpret_cast<void*>(address & ~(page_size - 1));
    const std::size_t span = (address + size) - reinterpret_cast<u64>(page);
    if (pf_vm_protect(page, span, PF_PROT_READ | PF_PROT_WRITE | PF_PROT_EXEC) != 0) {
        return false;
    }
    __atomic_store_n(reinterpret_cast<unsigned char*>(address), static_cast<unsigned char>(0xcc),
                     __ATOMIC_SEQ_CST);
    pf_vm_protect(page, span, PF_PROT_READ | PF_PROT_EXEC);
    return true;
}
} // namespace

void Install(RangeCallback on_gpu_range_allocated) {
    static std::atomic<bool> installed{false};
    if (installed.exchange(true)) {
        return;
    }
    pf_fault_add_handler((1u << PF_FAULT_BREAKPOINT) | (1u << PF_FAULT_SINGLE_STEP), OnTrap, nullptr);
    int patched = 0;
    for (const auto& site : MemcpySites) {
        patched += Patch(ImageBase + site.offset, site.code, sizeof(site.code));
    }
    std::printf("Guest hooks: %d of %zu resource copy sites tell the GPU side of their writes\n",
                patched, MemcpySites.size());
    on_range_allocated = on_gpu_range_allocated;
    const bool allocator = Patch(ImageBase + AllocReturn, AllocReturnCode, sizeof(AllocReturnCode));
    // BB_HEAP_HOOKS=0 (bisecting): the heap's ranges are not reported.
    const char* heap_env = std::getenv("BB_HEAP_HOOKS");
    const bool heap_hooks = !heap_env || heap_env[0] != '0';
    if (BbStats::enabled) {
        Patch(ImageBase + CollectorEntry, CollectorEntryCode, sizeof(CollectorEntryCode));
    }
    int heap_sites = 0;
    for (const u64 site : HeapAllocReturns) {
        if (!heap_hooks) {
            break;
        }
        heap_sites += Patch(ImageBase + site, HeapAllocReturnCode, sizeof(HeapAllocReturnCode));
    }
    std::printf("Guest hooks: GPU heap allocations seen at %d of %zu call sites\n", heap_sites,
                HeapAllocReturns.size());
    std::printf("Guest hooks: GPU memory allocator %s\n", allocator ? "tells the GPU side of new ranges" : "not found");
    BbHeapSites::Install();
    if (const char* env = std::getenv("BB_HEAP_SITES"); env && env[0] == '1') {
        Patch(ImageBase + ReleaseCheck, ReleaseCheckCode, sizeof(ReleaseCheckCode));
    }
}
} // namespace BbGuestHooks
