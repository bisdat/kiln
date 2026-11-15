// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <tuple>
#include <type_traits>
#include "internal.h"
#include "kiln_hook.hpp"
namespace kg {
// The legacy observers retain their argument/result logic. Their continuation is the typed frame's rest of chain,
// never a call back to the target entry. Save/restore the TLS frame for recursive/nested game calls.
struct FrameLink {
    uint32_t group;
    static inline thread_local KilnCallFrame* current = nullptr;
    static inline std::atomic<const KilnHooksApi*> api{nullptr};
    template<class T> static uint64_t bits(T value) {
        if constexpr (std::is_pointer_v<T>) return reinterpret_cast<uintptr_t>(value);
        else return static_cast<uint64_t>(value);
    }
    template<class T> static T value(uint64_t bits) {
        if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(bits);
        else return static_cast<T>(bits);
    }
    template<class Ret, class... Args> Ret call(Args... args) const {
        const uint64_t values[] = {bits(args)...};
        uint64_t saved[sizeof...(Args)];
        for (size_t i=0; i<sizeof...(Args); ++i) {
            auto& slot = i<4 ? current->gpr[i] : current->stack[i-4]; saved[i]=slot; slot=values[i];
        }
        api.load(std::memory_order_acquire)->call_original_frame(current);
        for (size_t i=0; i<sizeof...(Args); ++i) (i<4 ? current->gpr[i] : current->stack[i-4])=saved[i];
        return value<Ret>(current->rax);
    }
    uintptr_t caller() const { return api.load(std::memory_order_acquire)->caller(current->slot); }
    template<class Ret, class... Args, size_t... I>
    static Ret invoke(KilnCallFrame* f, Ret(*fn)(Args...), std::index_sequence<I...>) {
        return fn(value<Args>(I<4 ? f->gpr[I] : f->stack[I-4])...);
    }
    template<class Ret, class... Args> static Ret invoke(KilnCallFrame* f, Ret(*fn)(Args...)) {
        return invoke(f,fn,std::index_sequence_for<Args...>{});
    }
    template<auto Fn> static uintptr_t KILN_CALL dispatch(KilnCallFrame* f, void* user) {
        if (!hooks_installed(static_cast<FrameLink*>(user)->group)) return KILN_TYPED_CONTINUE;
        struct Scope { KilnCallFrame* old; ~Scope() { current=old; } } scope{current}; current=f;
        f->rax=bits(invoke(f,Fn)); return KILN_TYPED_SKIP;
    }
};
void hook_register(uint32_t group, FrameLink* link, uintptr_t target, const char* label,
                   KilnTypedFn handler, std::initializer_list<uint32_t> args, uint32_t result);
}
