// SPDX-License-Identifier: GPL-2.0-or-later
// refueled: M3S1/M3S2 - window + Vulkan presentation of the GPU library on Windows, without a game.
// bbgpu_init() opens the SDL3 window and builds the Vulkan instance, device, surface, swapchain, the
// presenter and the VideoOut driver. M3S2: the test image goes the way the game's frames will go:
//   sceVideoOutOpen -> sceVideoOutRegisterBuffers (host buffers play the guest framebuffers)
//   -> CPU writes the pixels -> runtime_memory_note_write (as the runtime tells the GPU side of a
//   CPU write) -> sceVideoOutSubmitFlip -> GPU command thread: VideoOutDriver::SubmitFlipInternal
//   (Presenter::PrepareFrame, texture cache) -> VideoOut present thread: Flip -> Presenter::Present.
// Nothing here calls the presenter directly. The picture: a horizontal gradient whose colour changes
// every second (red, green, blue, yellow, cyan, magenta) with a white bar that moves every frame.
// After N seconds the program closes the window through SDL_EVENT_QUIT (the window thread then
// exits the process with 0).
// Usage: m3-present [seconds=5]. BB_VK_VALIDATION=1 enables the Khronos validation layer.
// There is no guest process: the runtime_* services are stubs. Guest memory is plain host memory
// (identity mapping, VirtualAlloc at a fixed address inside the GPU library's guest range).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <memory>
#include <queue>
#include <deque>
#include <vector>
#include <string>
#include <array>
#include <functional>
#include <filesystem>
#include <windows.h>
#include <SDL3/SDL.h>
#include "platform/pf_jmp.h"
#include "bbgpu.h"

// Test-only access to the presenter's swapchain for the log (Presenter keeps it private).
#define private public
#include "video_core/renderer_vulkan/vk_presenter.h"
#undef private
#include "core/libraries/videoout/video_out.h"

namespace {
using GpuRange = void (*)(uintptr_t, uint64_t);
GpuRange hook_map, hook_note_write, hook_invalidate;
} // namespace

extern "C" {
__thread pf_sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
// The guest memory of this test is host memory: everything is mapped, nothing is clamped.
uint64_t runtime_memory_clamp(uintptr_t, uint64_t size) { return size; }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t* start, uintptr_t* end, int* mapped) {
    *start = 0; *end = ~uintptr_t(0); *mapped = 1; return 1;
}
void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t),
    void (*invalidate)(uintptr_t, uint64_t)) { hook_map = map; hook_invalidate = invalidate; }
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) {}
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t a, const void* d, uint64_t n) { std::memcpy((void*)a, d, n); return 1; }
void runtime_memory_read_backing(uintptr_t a, void* d, uint64_t n) { std::memcpy(d, (const void*)a, n); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
// The runtime's own note_write: the GPU side is told of a CPU write (hook_note_write when set,
// else hook_invalidate), as in src/runtime_memory.c.
void runtime_memory_note_write(uintptr_t address, uint64_t size) {
    GpuRange hook = hook_note_write ? hook_note_write : hook_invalidate;
    if (hook && size) hook(address, size);
}
void runtime_memory_set_note_write_hook(void (*note)(uintptr_t, uint64_t)) { hook_note_write = note; }
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) {}
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { return 0; }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { return 0; }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
}

extern std::unique_ptr<Vulkan::Presenter> presenter;

namespace {
constexpr uint32_t kWidth = 1280, kHeight = 720, kBuffers = 3;
constexpr uint64_t kBufferBytes = uint64_t(kWidth) * kHeight * 4;
constexpr uint64_t kBufferStride = (kBufferBytes + 0xFFFF) & ~uint64_t(0xFFFF);

// Colour of the gradient's right end for second n (R, G, B).
void SecondColor(int n, int& r, int& g, int& b) {
    static const int table[6][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255},
                                    {255, 255, 0}, {0, 255, 255}, {255, 0, 255}};
    r = table[n % 6][0]; g = table[n % 6][1]; b = table[n % 6][2];
}

// A8B8G8R8Srgb: bytes R, G, B, A. Black at the left, the second's colour at the right,
// a white bar (32 px wide, full height) at x = frame * 8 mod width.
void FillFrame(uint8_t* pixels, int second, uint32_t frame) {
    int r, g, b;
    SecondColor(second, r, g, b);
    const uint32_t bar = (frame * 8) % kWidth;
    for (uint32_t y = 0; y < kHeight; ++y) {
        uint8_t* row = pixels + uint64_t(y) * kWidth * 4;
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* p = row + x * 4;
            const bool in_bar = x >= bar && x < bar + 32;
            p[0] = in_bar ? 255 : uint8_t(r * x / (kWidth - 1));
            p[1] = in_bar ? 255 : uint8_t(g * x / (kWidth - 1));
            p[2] = in_bar ? 255 : uint8_t(b * x / (kWidth - 1));
            p[3] = 255;
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    BbGpuConfig config{};
    config.title = "M3S2 present test";
    config.serial = "TEST00000";
    config.user_dir = std::getenv("BB_GPU_USER_DIR") ? std::getenv("BB_GPU_USER_DIR") : "m3_user";
    config.width = kWidth;
    config.height = kHeight;
    std::printf("M3: user_dir=%s\n", config.user_dir);
    std::printf("M3: bbgpu_init\n");
    if (bbgpu_init(&config)) { std::printf("M3: FAIL bbgpu_init\n"); return 2; }
    if (!presenter) { std::printf("M3: FAIL no presenter\n"); return 2; }
    const char* driver = SDL_GetCurrentVideoDriver();
    std::printf("M3: SDL video driver: %s\n", driver ? driver : "(none)");
    {
        auto& sc = presenter->swapchain;
        const auto fmt = sc.GetSurfaceFormat();
        const auto ext = sc.GetExtent();
        std::printf("M3: swapchain format=%s colorspace=%s extent=%ux%u images=%u\n",
                    vk::to_string(fmt.format).c_str(), vk::to_string(fmt.colorSpace).c_str(),
                    ext.width, ext.height, sc.GetImageCount());
    }

    // "Guest" framebuffers: host memory inside the library's guest range (0x10'0000'0000 ..).
    const uint64_t total = kBuffers * kBufferStride;
    uint8_t* memory = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x20'0000'0000ull),
                                                         total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!memory) { std::printf("M3: FAIL VirtualAlloc\n"); return 2; }
    void* addresses[kBuffers];
    for (uint32_t i = 0; i < kBuffers; ++i) {
        addresses[i] = memory + i * kBufferStride;
        FillFrame(static_cast<uint8_t*>(addresses[i]), 0, 0);
    }
    if (hook_map) hook_map(reinterpret_cast<uintptr_t>(memory), total); // the runtime's "mapping created"

    using namespace Libraries::VideoOut;
    const int handle = sceVideoOutOpen(0, 0, 0, nullptr);
    if (handle < 0) { std::printf("M3: FAIL sceVideoOutOpen %d\n", handle); return 2; }
    BufferAttribute attribute{};
    sceVideoOutSetBufferAttribute(&attribute, PixelFormat::A8B8G8R8Srgb, uint32_t(TilingMode::Linear), 0,
                                  kWidth, kHeight, kWidth);
    const int reg = sceVideoOutRegisterBuffers(handle, 0, addresses, kBuffers, &attribute);
    if (reg < 0) { std::printf("M3: FAIL sceVideoOutRegisterBuffers %d\n", reg); return 2; }
    std::printf("M3: video out handle %d, %u buffers %ux%u A8B8G8R8Srgb linear\n", handle, kBuffers, kWidth, kHeight);

    // The guest's frame loop: write a buffer, tell the GPU side, flip. The flip is queued by
    // SubmitFlip to the GPU command thread; the VideoOut present thread presents at 60 Hz.
    using Clock = std::chrono::steady_clock;
    const auto start = Clock::now();
    uint32_t frame = 0;
    int last_second = -1;
    while (Clock::now() - start < std::chrono::seconds(seconds)) {
        // Keep the flip queue short (the real game waits on the flip event / pending count).
        while (sceVideoOutIsFlipPending(handle) >= 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const int second = int(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count());
        if (second != last_second) { last_second = second; std::printf("M3: second %d colour index %d\n", second, second % 6); }
        const uint32_t index = frame % kBuffers;
        uint8_t* pixels = static_cast<uint8_t*>(addresses[index]);
        FillFrame(pixels, second, frame);
        runtime_memory_note_write(reinterpret_cast<uintptr_t>(pixels), kBufferBytes);
        const int result = sceVideoOutSubmitFlip(handle, int(index), 1, frame);
        if (result < 0) { std::printf("M3: FAIL sceVideoOutSubmitFlip %d\n", result); return 4; }
        ++frame;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("M3: %u flips submitted in %.2f s (%.1f per second)\n", frame, elapsed, frame / elapsed);
    if (frame == 0) { std::printf("M3: FAIL nothing was flipped\n"); return 4; }
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
    // The window thread calls _Exit(0) when it sees the quit event.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::printf("M3: FAIL window did not close\n");
    return 3;
}
