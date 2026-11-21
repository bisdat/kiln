// SPDX-License-Identifier: MIT
// The content worker's lifetime guard (Kiln-internal). Adapted from ShroudForge (MIT, Copyright
// (c) 2026 bonsaibauer), modules/game-runtime/native/engine-context/hold_resource_lifetime.{h,cpp} (ResourceLifeGuard:
// TryAcquire, Prepare, AcquirePrepared, Validate, Contains, PayloadBase, Release). It holds the engine's thread-registry
// word Shared so the registered resource content worker cannot be unregistered before its reader and bulk payload are
// destroyed; it guards that lifetime only (hold_resource_lifetime.h:5-7). Same reads and checks in the same order.
// Differences, all safety-only:
// - the state is the caller's record (ResourceGuard, kiln_engine_view.h): no unique_ptr and no allocation at all, no
//   context mutex (the layout is the compiled exact profile, immutable for the process; ShroudForge copies it under a
//   mutex in Prepare);
// - one acquire for both TryAcquire overloads. With a held gate it borrows that gate's registry share; the owners it
//   validates against are the gate's validated view's (ShroudForge's Prepare snapshot, resolved before the gate there;
//   the gate re-validated them inside its hold). Without a gate it refuses a world's own thread first (the gate's thread
//   rules, before any engine read), resolves the owners (Prepare), then tries the word Shared itself;
// - every refusal has its reason (ShroudForge's guard answers a bare false), ShroudForge's own string where its engine
//   context makes the same check elsewhere (resolve_registered_scene.cpp for the registry rows);
// - a borrowed guard stops validating and containing once its gate no longer holds the borrowed share (ShroudForge
//   leaves "use it only while Scene remains held" to its callers);
// - registry row and worker addresses are overflow-checked, as in resolve_registered_scene.
// No allocation, lock, wait, KilnApi call, log or engine call; one loop, over at most the profile's registry capacity
// (tests/lint_gate.py).
#include "resource_guard.h"

#include <array>
#include <cstring>

#include "memory.h"
#include "resolve.h"

namespace kiln::engine {
using mem::add;
using mem::field;
using mem::read_span;

#ifdef KILN_ENGINE_GATE_TEST_POINTS
void (*g_guard_test_point)(GuardPoint) = nullptr;
#define KILN_GUARD_POINT(p) \
    do {                      \
        if (g_guard_test_point) g_guard_test_point(p); \
    } while (0)
#else
#define KILN_GUARD_POINT(p) \
    do {                      \
    } while (0)
#endif

namespace {
// A borrowed guard is live only while its gate still holds the very share it borrowed.
bool borrow_live(const ResourceGuard& guard) {
    return !guard.gate || (guard.gate->held && guard.gate->registry_lock == guard.registry_lock);
}

// Validate (hold_resource_lifetime.cpp:27-50), its checks in its order, with a reason per group.
Reason check(const Layout& layout, const ResourceGuard& guard) {
    const auto& w = layout.write;
    const auto& r = layout.resource_write;
    std::uintptr_t name{}, handle{}, callback{}, context{}, reader{}, payload{}, wanted_name{}, wanted_callback{};
    std::uint64_t length{}, capacity{}, payload_size{};
    std::uint32_t stop{}, initialized{};
    if (!add(guard.thread, w.inline_name, wanted_name) || !field(guard.thread, w.name_pointer, name) || name != wanted_name ||
        !field(guard.thread, w.name_length, length) || length != r.name_size ||
        !field(guard.thread, w.name_capacity, capacity) || capacity != w.name_capacity_value ||
        !field(guard.thread, w.thread_handle, handle) || handle != guard.handle ||
        !add(guard.owners.image, r.callback, wanted_callback) || !field(guard.thread, w.thread_callback, callback) ||
        callback != wanted_callback || !field(guard.thread, w.thread_context, context) || context != guard.thread ||
        !field(guard.thread, w.thread_stop, stop) || stop != 0)
        return Reason::ResourceLifetimeWorkerInvalidOrStopping;
    if (!field(guard.service, r.initialized, initialized) || initialized != r.initialized_value)
        return Reason::ResourceLifetimeServiceNotInitialized;
    if (!field(guard.service, r.reader, reader) || reader != guard.reader) return Reason::ResourceLifetimeReaderChanged;
    if (!field(guard.service, r.payload_pointer, payload) || payload != guard.payload ||
        !field(guard.service, r.payload_size, payload_size) || payload_size != guard.payload_size)
        return Reason::ResourceLifetimePayloadChanged;
    Owners current{};
    if (resolve_owners(layout, current, guard.owners.image) != Reason::None || !(current == guard.owners))
        return Reason::ResourceLifetimeOwnerChanged;
    return Reason::None;
}
}  // namespace

void guard_release(ResourceGuard& guard) noexcept {
    // Release (:18-25): the registry word only when this guard took it; a borrowed share is the gate's to release.
    if (guard.registry_lock && guard.owns_registry) mem::release_shared(guard.registry_lock);
    const std::uint32_t size = guard.size;
    std::memset(&guard, 0, sizeof guard);
    guard.size = size;
}

Reason guard_acquire(const Layout* layout, const GateState* gate, std::uintptr_t expected_service,
                     std::uintptr_t expected_reader, ResourceGuard& guard, CallerThread caller, std::uintptr_t image) {
    // A held record: never touch its word (:65, :87).
    if (guard.held || guard.registry_lock) return Reason::ResourceLifetimeAlreadyHeld;
    guard_release(guard);  // no word to release (registry_lock is 0): start from a zeroed record, whatever it carried
    // Owned: the gate's thread rules, before any engine read. Borrowed: the caller is inside a held gate, whose acquire
    // already refused a world thread.
    if (!gate && caller != CallerThread::Other)
        return caller == CallerThread::ServerWorld   ? Reason::InFrame
               : caller == CallerThread::ClientWorld ? Reason::WorldThread
                                                     : Reason::CallerThreadUnknown;
    const auto refuse = [&](Reason why) {
        guard_release(guard);
        return why;
    };
    // Prepare (:64-77): the exact profile with an enabled content worker layout (and one this code can walk).
    if (!layout || !layout->resource_write.enabled || !image || !layout->write.stride || !layout->write.capacity ||
        !layout->resource_write.name_size || layout->resource_write.name_size > layout->resource_write.name.size())
        return refuse(Reason::ResourceLifetimeUnavailable);
    const auto& w = layout->write;
    const auto& r = layout->resource_write;
    std::uintptr_t lock{};
    if (!add(image, w.registry_lock, lock)) return refuse(Reason::ResourceLifetimeUnavailable);
    if (gate) {
        // TryAcquire(scene, ...) (:79-83): a held gate that still validates, holding this image's registry word (:93).
        if (!gate->held || !gate->registry_lock || !gate->scene_lock) return refuse(Reason::ResourceLifetimeGateNotHeld);
        if (!gate_validate(layout, *gate, image)) return refuse(Reason::ResourceLifetimeGateChanged);
        if (gate->registry_lock != lock || gate->expected.owners.image != image)
            return refuse(Reason::ResourceLifetimeRegistryMismatch);
        guard.owners = gate->expected.owners;
        guard.registry_lock = lock;
        guard.owns_registry = 0;
        guard.gate = gate;
    } else {
        // Prepare's owners (resolved before the word), then TryAcquire's nonblocking Shared try (:93).
        Owners owners{};
        if (const Reason why = resolve_owners(*layout, owners, image); why != Reason::None) return refuse(why);
        if (!mem::try_shared(lock)) return refuse(Reason::EcsThreadRegistryBusy);
        guard.owners = owners;
        guard.registry_lock = lock;
        guard.owns_registry = 1;
    }
    KILN_GUARD_POINT(GuardPoint::Held);
    // AcquirePrepared (:97-120): the one registered content worker, by name and handle.
    std::uint16_t count{};
    if (!field(image, w.registry_count, count) || count > w.capacity) return refuse(Reason::EcsThreadRegistryCountInvalid);
    std::uintptr_t entries{};
    if (!add(image, w.registry_entries, entries)) return refuse(Reason::EcsThreadRegistryRangeInvalid);
    unsigned matches{};
    for (std::size_t index = 0; index < count; ++index) {
        std::uintptr_t row{}, name{}, handle{};
        std::uint64_t length{};
        if (index > UINTPTR_MAX / w.stride || !add(entries, index * w.stride, row) || !field(row, w.row_length, length) ||
            !field(row, w.row_name, name) || !field(row, w.row_handle, handle))
            return refuse(Reason::EcsThreadRegistryRowUnreadable);
        if (length != r.name_size) continue;
        std::array<char, 64> text{};
        if (!read_span(name, text.data(), length)) return refuse(Reason::EcsThreadNameUnreadable);
        if (std::memcmp(text.data(), r.name.data(), length)) continue;
        if (++matches != 1) return refuse(Reason::ResourceLifetimeWorkerIsAmbiguous);
        if (!handle || name < w.inline_name || name - w.inline_name < r.service_thread)
            return refuse(Reason::ResourceLifetimeWorkerRangeInvalid);
        guard.thread = name - w.inline_name;
        guard.service = guard.thread - r.service_thread;
        guard.handle = handle;
    }
    if (!matches) return refuse(Reason::ResourceLifetimeWorkerUnregistered);
    if (guard.service != expected_service) return refuse(Reason::ResourceLifetimeServiceMismatch);
    guard.reader = expected_reader;
    if (!field(guard.service, r.payload_pointer, guard.payload) || !guard.payload ||
        !field(guard.service, r.payload_size, guard.payload_size) || !guard.payload_size)
        return refuse(Reason::ResourceLifetimePayloadUnavailable);
    if (guard.payload_size > UINTPTR_MAX - guard.payload) return refuse(Reason::ResourceLifetimePayloadRangeInvalid);
    KILN_GUARD_POINT(GuardPoint::Scanned);
    if (const Reason why = check(*layout, guard); why != Reason::None) return refuse(why);  // Validate (:122)
    guard.held = 1;
    return Reason::None;
}

bool guard_validate(const Layout* layout, const ResourceGuard& guard) {
    return layout && guard.held && guard.registry_lock && borrow_live(guard) && check(*layout, guard) == Reason::None;
}

bool guard_contains(const ResourceGuard& guard, std::uintptr_t address, std::size_t size) {
    // Contains (:52-56).
    return guard.held && guard.registry_lock && borrow_live(guard) && address >= guard.payload &&
           address - guard.payload <= guard.payload_size && size <= guard.payload_size - (address - guard.payload);
}

}  // namespace kiln::engine
