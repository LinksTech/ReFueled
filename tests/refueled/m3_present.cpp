// SPDX-License-Identifier: GPL-2.0-or-later
// refueled: M3S1 - window + Vulkan presentation of the GPU library on Windows, without a game.
// bbgpu_init() opens the SDL3 window and builds the Vulkan instance, device, surface, swapchain and
// the presenter and the VideoOut driver, whose present thread draws blank (black) frames while no
// guest port is open. This program waits N seconds, counts them and closes the window
// through SDL_EVENT_QUIT (the window thread then exits the process with 0).
// Usage: m3-present [seconds=5]. BB_VK_VALIDATION=1 enables the Khronos validation layer.
// There is no guest process: the runtime_* services are stubs (as tests/test_gpu_runtime_stubs.cpp);
// guest memory accesses abort.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
#include <SDL3/SDL.h>
#include "platform/pf_jmp.h"
#include "bbgpu.h"

// Test-only access to the presenter's swapchain for the log (Presenter keeps it private).
#define private public
#include "video_core/renderer_vulkan/vk_presenter.h"
#undef private

extern "C" {
__thread pf_sigjmp_buf* runtime_fault_recover = nullptr;
uint64_t runtime_disabled_optimizations = 0;
uint64_t runtime_experiment_bits = 0;
uint64_t runtime_tsc_frequency() { return 1000000000; }
int runtime_file_translate(const char*, char*, size_t) { std::abort(); }
uint64_t runtime_memory_clamp(uintptr_t, uint64_t) { std::abort(); }
uint64_t runtime_process_time_us() { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int runtime_memory_region(uintptr_t, uintptr_t*, uintptr_t*, int*) { std::abort(); }
void runtime_memory_set_gpu_hooks(void (*)(uintptr_t, uint64_t),
    void (*)(uintptr_t, uint64_t), void (*)(uintptr_t, uint64_t)) {}
void runtime_thread_attach_host(const char*) {}
void runtime_memory_gpu_protect(uintptr_t, uint64_t, int, int) { std::abort(); }
void runtime_restart() { std::abort(); }
uint64_t runtime_process_time_counter() { return runtime_process_time_us() * 1000; }
int32_t* runtime_errno() { std::abort(); }
int runtime_memory_write_backing(uintptr_t, const void*, uint64_t) { std::abort(); }
void runtime_memory_read_backing(uintptr_t, void*, uint64_t) { std::abort(); }
const uint64_t* runtime_memory_generation() { static const uint64_t generation = 0; return &generation; }
void runtime_memory_note_write(uintptr_t, uint64_t) { std::abort(); }
void runtime_memory_set_note_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_cpu_write_hook(void (*)(uintptr_t, uint64_t)) {}
void runtime_memory_set_write_watch(uintptr_t, uint64_t, int) { std::abort(); }
void runtime_memory_set_guest_chunk_allocator(int (*)(uint64_t, uint64_t)) {}
void runtime_memory_set_guest_chunk_whole(int) {}
int runtime_memory_vma_info(uintptr_t, int*, int*, uintptr_t*) { std::abort(); }
int runtime_memory_direct_phys(uintptr_t, uint64_t*, uintptr_t*) { std::abort(); }
void runtime_wait_report(double) {}
void runtime_sleep_stats(uint64_t* calls, uint64_t* ns) { *calls = 0; *ns = 0; }
uint64_t runtime_heap_growths() { return 0; }
}

extern std::unique_ptr<Vulkan::Presenter> presenter;

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 5;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    BbGpuConfig config{};
    config.title = "M3S1 present test";
    config.serial = "TEST00000";
    config.user_dir = std::getenv("BB_GPU_USER_DIR") ? std::getenv("BB_GPU_USER_DIR") : "m3_user";
    config.width = 1280;
    config.height = 720;
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
    // bbgpu_init also created the VideoOut driver: its present thread draws blank frames by itself
    // while no port is open (VideoOutDriver::DrawBlankFrame: PrepareBlankFrame + Present). Calling
    // the presenter from this thread as well would race it (one thread per scheduler), so this
    // thread only waits and counts. Every blank frame is two submissions on the present scheduler.
    const uint64_t tick0 = presenter->present_scheduler.CurrentTick();
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const uint64_t tick1 = presenter->present_scheduler.CurrentTick();
    std::printf("M3: present scheduler ticks %llu -> %llu: about %llu blank frames presented in %d s\n",
                (unsigned long long)tick0, (unsigned long long)tick1,
                (unsigned long long)((tick1 - tick0) / 2), seconds);
    if (tick1 == tick0) { std::printf("M3: FAIL nothing was presented\n"); return 4; }
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&quit);
    // The window thread calls _Exit(0) when it sees the quit event.
    std::this_thread::sleep_for(std::chrono::seconds(5));
    std::printf("M3: FAIL window did not close\n");
    return 3;
}
