// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: helper thread sizing. Counts follow the hardware threads this process may run on
// (the affinity mask, so `taskset` can emulate a Steam Deck), and speculative helpers run as
// SCHED_IDLE: they use cores the game leaves idle and never take time from its threads.

#pragma once

#include <algorithm>
#include <thread>

#include "platform/pf_thread.h"

namespace BbThreads {

/// Hardware threads available to the process.
inline unsigned Available() {
    // refueled: was sched_getaffinity + CPU_COUNT (pf_thread_linux.c:110-121; the fallback of
    // pf_cpu_count_allowed is sysconf(_SC_NPROCESSORS_ONLN) instead of hardware_concurrency).
    return static_cast<unsigned>(std::max(1, pf_cpu_count_allowed()));
}

/// The calling thread only runs on otherwise idle cores (falls back to the lowest nice level).
inline void MakeBackground() {
    // refueled: was sched_setscheduler(SCHED_IDLE), else setpriority(nice 19) (pf_thread_linux.c:100-107).
    // The result was ignored before and still is.
    (void)pf_thread_set_idle_priority();
}

} // namespace BbThreads
