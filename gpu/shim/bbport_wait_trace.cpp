// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_wait_trace.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cinttypes>

#include "platform/pf_process.h"
#include "platform/pf_thread.h"

namespace BbWaitTrace {
namespace {
constexpr int Skip = 1; // Scope::Scope itself
constexpr int Kept = 6; // frames that identify a stack

struct Key {
    std::array<void*, Kept> frames{};
    std::array<char, 16> thread{};
    bool operator<(const Key& other) const {
        if (frames != other.frames) {
            return frames < other.frames;
        }
        return thread < other.thread;
    }
};
struct Total {
    std::uint64_t ns = 0, count = 0;
};
std::mutex mutex;
std::map<Key, Total> totals;

std::string Describe(void* address) {
    PfSymbol info{}; // refueled: was Dl_info / dladdr
    if (pf_symbolize(reinterpret_cast<std::uintptr_t>(address), &info) != 0) {
        char text[32];
        std::snprintf(text, sizeof(text), "%p", address);
        return text;
    }
    // refueled: the module may be a Windows path.
    const char* file = std::strrchr(info.module, '/');
#ifdef _WIN32
    if (const char* backslash = std::strrchr(info.module, '\\')) {
        file = (!file || backslash > file) ? backslash : file;
    }
#endif
    file = file ? file + 1 : (info.module[0] ? info.module : "?");
    char text[512];
    const std::uint64_t at = reinterpret_cast<std::uintptr_t>(address);
    if (info.symbol[0]) {
        std::snprintf(text, sizeof(text), "%s+%#" PRIx64, info.symbol, at - info.symbol_address);
    } else {
        std::snprintf(text, sizeof(text), "%s+%#" PRIx64, file, at - info.module_base);
    }
    return text;
}
} // namespace

bool Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("BB_WAIT_TRACE");
        return env && env[0] == '1';
    }();
    return enabled;
}

Scope::Scope() {
    if (!Enabled()) {
        return;
    }
    // refueled: was backtrace(frames, 10). pf_backtrace's frames[0] is its own code (like glibc's
    // backtrace it is the caller of backtrace): one frame deeper, so ask for one more and drop it.
    void* all[std::size(frames) + 1];
    const int got = pf_backtrace(all, int(std::size(all)));
    depth = got > 1 ? got - 1 : 0;
    for (int i = 0; i < depth; ++i) {
        frames[i] = all[i + 1];
    }
    start = std::chrono::steady_clock::now();
}

Scope::~Scope() {
    if (depth == 0) {
        return;
    }
    const auto ns = std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count());
    Key key;
    for (int i = 0; i < Kept && Skip + i < depth; ++i) {
        key.frames[i] = frames[Skip + i];
    }
    pf_thread_get_name(key.thread.data(), key.thread.size()); // refueled: was pthread_getname_np
    std::scoped_lock lk{mutex};
    auto& total = totals[key];
    total.ns += ns;
    ++total.count;
}

void Report(double window_s) {
    if (!Enabled()) {
        return;
    }
    std::vector<std::pair<Key, Total>> list;
    {
        std::scoped_lock lk{mutex};
        list.assign(totals.begin(), totals.end());
        totals.clear();
    }
    std::sort(list.begin(), list.end(),
              [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
    std::uint64_t all = 0;
    for (const auto& [key, total] : list) {
        all += total.ns;
    }
    std::printf("GPU tick waits: %.1f%% of the time in all, by stack:\n",
                window_s > 0 ? all / (window_s * 1e7) : 0.0);
    for (std::size_t i = 0; i < list.size() && i < 6; ++i) {
        const auto& [key, total] = list[i];
        std::printf("  %5.1f%% %6llu waits, thread %s:", total.ns / (window_s * 1e7),
                    (unsigned long long)total.count, key.thread.data());
        for (void* frame : key.frames) {
            if (frame) {
                std::printf(" <- %s", Describe(frame).c_str());
            }
        }
        std::printf("\n");
    }
}
} // namespace BbWaitTrace
