// SPDX-License-Identifier: MIT
// kiln.world's named services (named_service_open, named_service_snapshot, named_service_close and
// named_service_compare_exchange in kiln_world.h): one of the game's original named plain-data services of the server
// world, read and compared-and-swapped from any thread under the engine's own write gate (kiln.internal.engine's
// EcsGateScope), between frames only. Portable: no Windows and no KilnApi here, so the host tests run this exact code over
// the real gate and a fake engine (tests/named_services_test.cpp); named_prod.cpp wires it to the running game.
//
// Adapted from ShroudForge (MIT, Copyright (c) 2026 bonsaibauer): modules/game-runtime/native/named-services/
// read_and_update_named_service.cpp:26-85 (the service read and the service compare-exchange: the session, the
// gate, the budget, the binding resolved, the writable range, the copy, the compare, the merge, the write, the readback,
// the rollback that is read back too, the binding resolved again, "uncertain" when nothing proves the bytes) and
// resolve_named_service.cpp (resolve_binding, resolve: the name map, the record, the one bound mutable system, the type's
// metadata). Differences: a view is the caller's own handle bound to the service and the server session it was opened in
// (the session ShroudForge's SessionGuard checks per call is checked per call here too, before the gate and after it);
// the gate's write budget refuses before any byte through ecs_gate_write; an uncertain write latches the service for the
// session (ShroudForge latches the whole session's writes; Kiln's interpreter does that on top).
//
// Inside the hold (named_service_hold.cpp): no wait, allocation, lock, log or KilnApi call, and every loop over engine
// data has a fixed bound from the profile (records, map capacity, systems, 64 access views) or the service's size
// (tests/lint_hold.py). Everything that may allocate or lock runs before the gate is taken or after it is released.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_set>
#include <vector>

#include "kiln_engine_native.h"
#include "services/kiln_types.h"
#include "services/kiln_world.h"

namespace kw::named {

// ---- the hold (named_service_hold.cpp) ----
enum class HoldPoint : uint32_t { Begin, AfterResolve, AfterCompare, AfterWrite, AfterRollback };
#ifdef KILN_WORLD_HOLD_TEST_POINTS
extern void (*g_hold_test_point)(HoldPoint);
#endif
struct HoldInput {
    const kiln::engine::EcsGateScope* scope = nullptr;  // held
    const kiln::engine::ServiceLayout* layout = nullptr;
    const kiln::engine::ServiceContract* contract = nullptr;
    // kiln.internal.engine's guarded copy (any thread, no allocation, no lock)
    KilnResult(KILN_CALL* copy)(std::uintptr_t address, void* out, std::size_t bytes) = nullptr;
    // ShroudForge's writable(): the range is committed private read-write memory (production: VirtualQuery)
    bool (*writable)(void* user, std::uintptr_t address, std::size_t bytes) = nullptr;
    void* user = nullptr;
    bool write = false;  // false: a snapshot (before gets the bytes)
    const uint8_t* expected = nullptr;
    const uint8_t* mask = nullptr;  // NULL: every byte from desired
    const uint8_t* desired = nullptr;
    uint8_t* before = nullptr;
    uint8_t* merged = nullptr;
    uint8_t* verified = nullptr;
    const std::atomic<uint64_t>* retirements = nullptr;
    uint64_t retirements_seen = 0;
    // A write: the service's latch (the session it is latched uncertain in, 0 for none) and the view's session. The hold
    // writes nothing while the latch names the session, and sets it on an uncertain outcome before the gate is released,
    // so a writer that checked the latch before that release never writes after it.
    std::atomic<uint64_t>* latch = nullptr;
    uint64_t session = 0;
};
struct HoldOutput {
    uint32_t status = KILN_ECS_UNAVAILABLE;
    uint32_t flags = 0;  // KILN_MUTATION_*
    uint32_t why = KILN_ECS_WRITE_WHY_NONE;
};
void hold(const HoldInput& in, HoldOutput& out) noexcept;
// ShroudForge's resolve (resolve_named_service.cpp): the service's payload address in the gate's world, or false. Hold-safe.
bool resolve(const HoldInput& in, std::uintptr_t& payload) noexcept;

// ---- the service ----
struct Source {
    // kiln.internal.engine with its gate members (header.size covers ecs_gate_release), or NULL.
    const KilnEngineNativeApi* engine = nullptr;
    const KilnTypesApi* types = nullptr;  // kiln.types (open's type) or NULL
    void* user = nullptr;
    // Kiln's own facts of the server world, sampled consistently: *session its live session, 0 when there is none.
    // False when they cannot be read now (a sample that moved under the read).
    bool (*server)(void* user, uint64_t* session) = nullptr;
    // The live server step; 0 when unknown.
    uint64_t (*server_step)(void* user) = nullptr;
    // See HoldInput::writable; NULL refuses every service (fails closed).
    bool (*writable)(void* user, std::uintptr_t address, std::size_t bytes) = nullptr;
    // A line for kiln.log (never called inside a hold).
    void (*log)(void* user, int level, const char* text) = nullptr;
};

class Services {
public:
    static constexpr size_t kViewsPerOwner = KILN_NAMED_SERVICE_VIEWS;
    static constexpr size_t kViews = 4096;  // every owner's together

    explicit Services(const Source& source) : src_(source) {}
    Services(const Services&) = delete;
    Services& operator=(const Services&) = delete;

    // Whether named services can work on this build (an exact profile with its serviceLayout and write gate, the gate
    // members, the writable-range check); the text says which failed.
    const char* unavailable_reason() const;
    bool available() const { return unavailable_reason() == nullptr; }

    KilnResult open(KilnOwner owner, KilnStr name, uint64_t expected_session, KilnHandle* view, const KilnType** type);
    KilnResult snapshot(KilnOwner owner, KilnHandle view, void* out, uint32_t capacity, uint32_t* required);
    KilnResult close(KilnOwner owner, KilnHandle view);
    KilnResult compare_exchange(KilnOwner owner, KilnHandle view, const void* expected, const void* mask,
                                const void* desired, uint32_t bytes, void* confirmed, KilnMutationReply* reply);
    // The owner unloads or reloads (kiln.hooks' owner-retiring observer): its views end and its calls answer stale. Never
    // waits on a hold.
    void owner_retired(KilnOwner owner);

    // For tests and diagnostics.
    size_t views() const;
    size_t views_of(KilnOwner owner) const;

private:
    struct View {
        KilnHandle handle = 0;
        KilnOwner owner = 0;
        uint32_t contract = 0;   // index into the layout's contracts
        uint64_t session = 0;    // the server session it was opened in
        kiln::engine::Owners owners{};
        std::uintptr_t registry_owner = 0;
        std::array<std::uint64_t, 2> scene_uuid{};
    };
    enum class Found { None, Mine, Other };
    const kiln::engine::Layout* layout() const;
    bool retired(KilnOwner owner) const;
    Found find(KilnOwner owner, KilnHandle handle, View* out) const;
    bool latched(uint64_t session, uint32_t contract) const;
    // One hold over the view's (or a fresh) world: the session, the engine's view, the gate, the hold, the session again.
    struct Attempt {
        uint32_t status = KILN_ECS_UNAVAILABLE, flags = 0, why = 0, engine_reason = 0;
        uint64_t session = 0, step = 0, hold_ns = 0, restore_ns = 0;
        kiln::engine::EcsView view{};
    };
    // want_session: the session an open names (0: the live one); a bound view's own session otherwise.
    void attempt(KilnOwner owner, const View* bound, uint32_t contract, uint64_t want_session, HoldInput& in,
                 Attempt& out);
    void log(int level, const char* text) const;

    Source src_;
    mutable std::mutex mu_;  // the views and the retired owners (never across a hold)
    std::vector<View> views_;
    std::unordered_set<KilnOwner> retired_;
    KilnHandle next_handle_ = 0;
    // Per contract: the server session it is latched uncertain in (0: none). Set inside the hold (HoldInput::latch).
    std::array<std::atomic<uint64_t>, kiln::engine::kServiceContracts> latches_{};
    std::atomic<uint64_t> retirements_{0};
};

}  // namespace kw::named
