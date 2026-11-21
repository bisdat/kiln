// SPDX-License-Identifier: MIT
// Guarded access to engine memory from any thread. Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer),
// modules/game-runtime/native/engine-context/engine_context_state.h:80-91 (read, field),
// resolve_engine_owners.cpp:16-30 (read_span, add, vtable), lock_scene_access.cpp:21-63 (try_shared, try_exclusive,
// release_shared, release_exclusive, tracking_call) and ecs/read_entity_membership.cpp:52-58 (guarded_copy). The
// Windows bodies (memory_win.cpp) copy, CAS and call under SEH; host tests link their own (tests/host/memory_host.cpp).
// No allocation, no lock, no wait.
#pragma once
#include <cstddef>
#include <cstdint>

namespace kiln::engine::mem {

// Copy `size` bytes from `address`; false on a null or overflowing range or a fault (nothing is guaranteed in `out`).
bool read_span(std::uintptr_t address, void* out, std::size_t size);

template <class T> bool read(std::uintptr_t address, T& value) {
    if (!address || address > UINTPTR_MAX - sizeof(value)) return false;
    return read_span(address, &value, sizeof value);
}

inline bool add(std::uintptr_t base, std::size_t offset, std::uintptr_t& address) {
    if (!base || offset > UINTPTR_MAX - base) return false;
    address = base + offset;
    return true;
}

template <class T> bool field(std::uintptr_t base, std::size_t offset, T& value) {
    std::uintptr_t address{};
    return add(base, offset, address) && read(address, value);
}

inline bool vtable(std::uintptr_t object, std::uintptr_t rva, std::uintptr_t image, std::size_t size) {
    std::uintptr_t actual{};
    return rva < size && read(object, actual) && actual == image + rva;
}

// The engine's own reader/writer word (Shared = CAS v -> v+1 while v >= 0). Nonblocking:
// the CAS is retried only while the word stays Shared (at most 128 attempts, no pause), never against an exclusive
// holder, never a wait.
bool try_shared(std::uintptr_t address);
void release_shared(std::uintptr_t address) noexcept;
// Writer: one CAS 0 -> 0x80000000, never retried.
bool try_exclusive(std::uintptr_t address);
void release_exclusive(std::uintptr_t address) noexcept;

// Copy `size` bytes to engine memory at `address` under SEH; false on a null or overflowing range or a fault, after
// which a partial write may have happened (the caller restores and reads back).
bool write_span(std::uintptr_t address, const void* data, std::size_t size);

// Call the engine's change-tracking recorder `fn(manager, context, entity, index, 0)` (x64 __fastcall) under SEH;
// false when it faulted. Host tests record the call instead.
bool call_tracker(std::uintptr_t fn, std::uintptr_t manager, std::uintptr_t context, std::uintptr_t entity,
                  std::uint16_t index);

// The calling thread's priority while it may hold an engine word: raised to THREAD_PRIORITY_ABOVE_NORMAL when lower
// (the engine creates its GameServer thread at THREAD_PRIORITY_NORMAL), so a
// holder is never preempted by the engine's normal-priority threads while the GameServer thread spins on the word.
// boost_priority returns true and the previous value in *saved when it changed it; restore_priority puts it back.
// Both are syscalls: called only before the first word is taken and after the last one is released.
bool boost_priority(int* saved);
void restore_priority(int saved) noexcept;

// The process image base: GetModuleHandleW(nullptr), read once when kiln-engine.dll loads (facts_win.cpp), so a call
// inside a hold is a plain read; host tests and kiln-engine-test.dll set their fake image.
std::uintptr_t image_base();

}  // namespace kiln::engine::mem
