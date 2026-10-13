// bbport: fault-handler registry. The loader's access-fault dispatcher (platform/pf_exception.h;
// probe.c registers bbgpu_handle_fault first) calls bbgpu_handle_fault(), which calls the
// registered handlers in priority order.
// refueled: the `context` handed to the handlers is a PfContext* (platform/pf_exception.h; read
// registers with pf_context_get), no longer a ucontext_t*; the parameter type stays void*.
#pragma once
#include <compare>
#include <cstdint>
#include <set>
#ifndef _WIN32
#include <signal.h> // refueled: kept for Linux includers that rely on it transitively
#endif
#include "common/singleton.h"
#include "common/types.h"

namespace Core {
using AccessViolationHandler = bool (*)(void* context, void* fault_address);
using IllegalInstructionHandler = bool (*)(void* context);

class SignalDispatch {
public:
    void RemoveHandlers() { access_violation_handlers.clear(); illegal_instruction_handlers.clear(); }
    void RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32 priority) {
        access_violation_handlers.emplace(handler, priority);
    }
    void RegisterIllegalInstructionHandler(const IllegalInstructionHandler& handler, u32 priority) {
        illegal_instruction_handlers.emplace(handler, priority);
    }
    bool DispatchAccessViolation(void* context, void* fault_address) const {
        for (const auto& entry : access_violation_handlers)
            if (entry.handler(context, fault_address)) return true;
        return false;
    }
    bool DispatchIllegalInstruction(void* context) const {
        for (const auto& entry : illegal_instruction_handlers)
            if (entry.handler(context)) return true;
        return false;
    }

private:
    template <typename T>
    struct HandlerEntry {
        T handler;
        u32 priority;
        // bbport: equal priorities are kept (ordered by handler), not dropped by the set.
        std::strong_ordering operator<=>(const HandlerEntry& right) const {
            if (const auto order = priority <=> right.priority; order != 0) return order;
            return reinterpret_cast<std::uintptr_t>(handler) <=> reinterpret_cast<std::uintptr_t>(right.handler);
        }
    };
    std::set<HandlerEntry<AccessViolationHandler>> access_violation_handlers;
    std::set<HandlerEntry<IllegalInstructionHandler>> illegal_instruction_handlers;
};
using Signals = Common::Singleton<SignalDispatch>;
} // namespace Core
