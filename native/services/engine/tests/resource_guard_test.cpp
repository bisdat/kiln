// SPDX-License-Identifier: MIT
// kiln.engine: the content worker's lifetime guard (resource_guard.cpp; ShroudForge's ResourceLifeGuard) over a fake
// engine laid out from both exact profiles, with the resource content worker registered in it.
// - every refusal with its reason, one per refusal point of ShroudForge's TryAcquire/AcquirePrepared/Validate
//   (hold_resource_lifetime.cpp:60-123, :27-50), each leaving the record released and the registry word as before;
//   test points inside the acquire change the fake engine after the word is held, so the guard's own checks answer
//   (not the owner resolution that runs before it);
// - owned (the guard takes the registry word Shared itself, never waits) vs borrowed (a held gate's share; no word taken
//   or released by the guard); refused on a world thread before any engine read; idempotent release;
// - the order: gate, then guard; the guard released before the gate (the RAII scopes), and a borrowed guard that
//   outlives its gate validates and contains nothing;
// - validate and contains, every check;
// - inside a hold: no allocation (global operator new counter), bounded engine reads;
// - a holder race under ThreadSanitizer: owned and borrowed holders read the payload with plain loads while a simulated
//   thread registration takes the registry word exclusively and rebuilds the worker with plain stores.
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "gate.h"
#include "include/kiln_engine_native.h"
#include "memory.h"
#include "profiles.g.h"
#include "resolve.h"
#include "resource_guard.h"

namespace kiln::engine::mem {
extern std::atomic<std::uint64_t> test_reads, test_cas, test_releases;
}

std::atomic<long> g_allocations{0};
void* operator new(std::size_t n) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace kiln::engine;
std::atomic<int> checks{0}, failures{0};
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
const Layout& layout_of(const char* target) {
    for (const auto& p : generated::kProfiles)
        if (std::string(p.target) == target) return p.layout;
    std::abort();
}
std::atomic<std::uint64_t> g_step{50000};
GateState gate_record(std::uint64_t owner = 1) { GateState g{}; g.size = sizeof g; g.owner = owner; g.step = ++g_step; return g; }
ResourceGuard guard_record() { ResourceGuard g{}; g.size = sizeof g; return g; }
std::int32_t word(std::uintptr_t a) { return __atomic_load_n(fake::word(a), __ATOMIC_SEQ_CST); }
void set_word(std::uintptr_t a, std::uint32_t v) { __atomic_store_n(fake::word(a), static_cast<std::int32_t>(v), __ATOMIC_SEQ_CST); }
bool released(const ResourceGuard& g) {
    ResourceGuard zero{};
    zero.size = sizeof zero;
    return std::memcmp(&g, &zero, sizeof g) == 0;
}
std::string name(Reason r) { return reason_name(r); }

// The guard's test points: what to change in the fake engine once the word is held (Held) or the scan is done (Scanned).
std::function<void()> g_on_held, g_on_scanned;
void point(GuardPoint p) {
    if (p == GuardPoint::Held && g_on_held) g_on_held();
    if (p == GuardPoint::Scanned && g_on_scanned) g_on_scanned();
}

struct Rig {
    const Layout& L;
    fake::Engine e;
    EcsView view{};
    std::string t;
    explicit Rig(const char* target) : L(layout_of(target)), e(L), t(std::string(target) + ": ") {
        e.add_content_worker();
        check(ecs_view(&L, ViewKind::Authority, view, e.image.at()) == Reason::None, t + "authority view");
    }
    std::uintptr_t image() const { return e.image.at(); }
    Reason owned(ResourceGuard& g, CallerThread c = CallerThread::Other) {
        return guard_acquire(&L, nullptr, e.service(), e.reader(), g, c, image());
    }
    Reason owned_with(ResourceGuard& g, std::uintptr_t service, std::uintptr_t reader) {
        return guard_acquire(&L, nullptr, service, reader, g, CallerThread::Other, image());
    }
    Reason borrowed(const GateState& gate, ResourceGuard& g) {
        return guard_acquire(&L, &gate, e.service(), e.reader(), g, CallerThread::Other, image());
    }
    Reason gate(GateState& g) { return gate_acquire(&L, view, g, CallerThread::Other, image()); }
    std::uintptr_t thread_field(std::size_t offset) const { return e.content_thread() + offset; }
    std::uintptr_t service_field(std::size_t offset) const { return e.service() + offset; }
};

// An owned acquire after `edit` (undone by `undo`) refuses with `want`, leaves the record released and the registry
// word as before.
void owned_refusal(Rig& r, const std::string& what, Reason want, const std::function<void()>& edit,
                   const std::function<void()>& undo, std::uintptr_t service = 0, std::uintptr_t reader = 0) {
    edit();
    const std::int32_t before = word(r.e.registry_word());
    ResourceGuard g = guard_record();
    const Reason got = r.owned_with(g, service ? service : r.e.service(), reader ? reader : r.e.reader());
    check(got == want, r.t + what + ": " + name(want) + " (got " + name(got) + ")");
    check(released(g), r.t + what + ": the record is left released");
    check(word(r.e.registry_word()) == before, r.t + what + ": the registry word as before");
    undo();
    g_on_held = nullptr;
    g_on_scanned = nullptr;
}

// The same, borrowed from a held gate: the gate's word is never released by the guard's refusal, and the gate stays held.
void borrowed_refusal(Rig& r, const std::string& what, Reason want, const std::function<void()>& edit,
                      const std::function<void()>& undo) {
    GateState gate = gate_record();
    check(r.gate(gate) == Reason::None, r.t + what + ": gate held");
    edit();
    ResourceGuard g = guard_record();
    const Reason got = r.borrowed(gate, g);
    check(got == want, r.t + "borrowed " + what + ": " + name(want) + " (got " + name(got) + ")");
    check(released(g), r.t + "borrowed " + what + ": the record is left released");
    check(gate.held && word(r.e.registry_word()) == 1 && word(r.e.scene_word()) == static_cast<std::int32_t>(0x80000000u),
          r.t + "borrowed " + what + ": the gate still holds both words");
    undo();
    g_on_held = nullptr;
    g_on_scanned = nullptr;
    gate_release(gate);
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, r.t + "borrowed " + what + ": words free");
}

// The registry rows' page after the ones in use, made unreadable: returns the first row index in it (0 when the image
// has no such page apart from the registry count and lock).
std::size_t unreadable_rows(Rig& r, std::uintptr_t& page) {
    const auto& w = r.L.write;
    const std::uintptr_t entries = r.e.row(0), count = r.e.image.at(w.registry_count), lock = r.e.registry_word();
    page = (r.e.row(4) + 4095) & ~std::uintptr_t{4095};
    const std::size_t index = (page - entries + w.stride - 1) / w.stride;
    if (index >= w.capacity || (count >= page && count < page + 4096) || (lock >= page && lock < page + 4096)) return 0;
    if (mprotect(reinterpret_cast<void*>(page), 4096, PROT_NONE) != 0) return 0;
    return index;
}

void owned_and_release(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    ResourceGuard g = guard_record();
    const std::uint64_t reads = mem::test_reads.load();
    const long allocations = g_allocations.load();
    const Reason got = r.owned(g);
    const bool valid = guard_validate(&r.L, g);
    const bool inside = guard_contains(g, r.e.payload.at(), r.e.payload.size);
    const std::int32_t held_word = word(r.e.registry_word());
    const long section_allocations = g_allocations.load() - allocations;
    const std::uint64_t section_reads = mem::test_reads.load() - reads;
    check(got == Reason::None && g.held == 1 && g.owns_registry == 1 && g.gate == nullptr, t + "owned acquire");
    check(held_word == 1 && g.registry_lock == r.e.registry_word(), t + "owned: the registry word Shared by the guard");
    check(g.service == r.e.service() && g.reader == r.e.reader() && g.thread == r.e.content_thread() &&
              g.handle == r.e.content_handle && g.payload == r.e.payload.at() && g.payload_size == r.e.payload.size,
          t + "owned: the registered worker's service, reader, thread, handle and payload");
    check(g.owners == r.view.owners, t + "owned: the owners resolved before the word are the view's");
    check(valid && inside, t + "owned: validates, contains its payload");
    check(section_allocations == 0, t + "owned: no allocation in acquire/validate/contains (" +
                                        std::to_string(section_allocations) + ")");
    check(section_reads < 1024, t + "owned: bounded engine reads (" + std::to_string(section_reads) + ")");
    // an engine reader holds the word Shared too: guards and lookups share it
    {
        ResourceGuard second = guard_record();
        check(r.owned(second) == Reason::None && word(r.e.registry_word()) == 2, t + "a second owned guard shares the word");
        guard_release(second);
        check(word(r.e.registry_word()) == 1 && released(second), t + "and releases only its own share");
    }
    // already held: refused without touching the record or its word
    const std::uint64_t cas = mem::test_cas.load();
    check(r.owned(g) == Reason::ResourceLifetimeAlreadyHeld && g.held == 1 && word(r.e.registry_word()) == 1 &&
              mem::test_cas.load() == cas,
          t + "an acquire into a held record is refused, the record and its word untouched");
    const std::uint64_t releases = mem::test_releases.load();
    guard_release(g);
    check(word(r.e.registry_word()) == 0 && released(g) && mem::test_releases.load() == releases + 1,
          t + "owned release: its one share released, the record zeroed (size kept)");
    guard_release(g);
    check(word(r.e.registry_word()) == 0 && released(g) && mem::test_releases.load() == releases + 1,
          t + "a second release changes nothing (idempotent)");
    ResourceGuard never = guard_record();
    guard_release(never);
    check(released(never) && mem::test_releases.load() == releases + 1, t + "release of a record never acquired: a no-op");
    check(!guard_validate(&r.L, g) && !guard_contains(g, r.e.payload.at(), 1), t + "a released guard validates nothing");
    // a record carrying leftovers (not held, no word) is acquired from zero: an owned guard never keeps a stale gate
    {
        GateState stale_gate = gate_record();  // never held
        ResourceGuard leftover = guard_record();
        leftover.gate = &stale_gate;
        leftover.payload = 1;
        leftover.reserved = 9;
        check(r.owned(leftover) == Reason::None && leftover.gate == nullptr && leftover.reserved == 0 &&
                  leftover.payload == r.e.payload.at() && guard_validate(&r.L, leftover),
              t + "an owned acquire into a record with leftovers starts from zero (no stale gate)");
        guard_release(leftover);
    }
    // an engine reader holding the word Shared does not refuse a guard; a registering thread (exclusive) does, at once
    set_word(r.e.registry_word(), 1);
    check(r.owned(g) == Reason::None && word(r.e.registry_word()) == 2, t + "next to an engine lookup's share");
    guard_release(g);
    check(word(r.e.registry_word()) == 1, t + "the lookup's share is left");
    set_word(r.e.registry_word(), 0);
}

void borrowed_and_order(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    GateState gate = gate_record();
    check(r.gate(gate) == Reason::None, t + "gate held");
    const std::uint64_t cas = mem::test_cas.load(), releases = mem::test_releases.load(), reads = mem::test_reads.load();
    const long allocations = g_allocations.load();
    ResourceGuard g = guard_record();
    const Reason got = r.borrowed(gate, g);
    const bool valid = guard_validate(&r.L, g), inside = guard_contains(g, r.e.payload.at(64), 16);
    const std::int32_t reg = word(r.e.registry_word()), scene = word(r.e.scene_word());
    const std::uint64_t acquire_cas = mem::test_cas.load() - cas, acquire_releases = mem::test_releases.load() - releases;
    const std::uint64_t before_release = mem::test_releases.load();
    guard_release(g);
    const std::uint64_t release_releases = mem::test_releases.load() - before_release;
    const long section_allocations = g_allocations.load() - allocations;
    const std::uint64_t section_reads = mem::test_reads.load() - reads;
    check(got == Reason::None && g.owns_registry == 0 && g.gate == nullptr, t + "borrowed acquire, released after");
    check(valid && inside, t + "borrowed: validates and contains");
    check(reg == 1 && scene == static_cast<std::int32_t>(0x80000000u), t + "borrowed: the gate's words only (no new share)");
    // ShroudForge's Validate re-resolves the owners; on the dedicated server that nests a registry share under the gate's
    // (released at once). A client takes no word at all.
    check(r.L.dedicated_server ? acquire_cas == acquire_releases : acquire_cas == 0 && acquire_releases == 0,
          t + "borrowed: no word of its own (" + std::to_string(acquire_cas) + " CAS, " + std::to_string(acquire_releases) +
              " releases, all nested)");
    check(release_releases == 0 && word(r.e.registry_word()) == 1 &&
              word(r.e.scene_word()) == static_cast<std::int32_t>(0x80000000u) && gate.held,
          t + "borrowed release touches no word; the gate still holds both");
    check(section_allocations == 0 && section_reads < 1024,
          t + "borrowed: no allocation, bounded reads (" + std::to_string(section_reads) + ")");
    // the record names its gate while held
    {
        ResourceGuard h = guard_record();
        check(r.borrowed(gate, h) == Reason::None && h.gate == &gate && h.registry_lock == gate.registry_lock &&
                  h.owners == gate.expected.owners,
              t + "borrowed: the gate's registry word and validated owners");
        // the borrow is tied to the very word it borrowed: a held gate record naming another word lends it nothing
        gate.registry_lock += 4;
        const bool other_valid = guard_validate(&r.L, h), other_inside = guard_contains(h, r.e.payload.at(), 1);
        gate.registry_lock -= 4;
        check(!other_valid && !other_inside && guard_validate(&r.L, h),
              t + "borrowed: its gate record holding another word, it validates and contains nothing");
        guard_release(h);
    }
    gate_release(gate);
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "the gate's release frees both words");
    // the wrong order: the gate released first; the borrowed guard then validates and contains nothing, and its release
    // still touches no word
    {
        GateState first = gate_record();
        check(r.gate(first) == Reason::None, t + "gate for the wrong order");
        ResourceGuard h = guard_record();
        check(r.borrowed(first, h) == Reason::None && guard_validate(&r.L, h), t + "borrowed before the gate's release");
        gate_release(first);
        check(!guard_validate(&r.L, h) && !guard_contains(h, r.e.payload.at(), 1),
              t + "a borrowed guard whose gate is released validates and contains nothing");
        const std::uint64_t rel = mem::test_releases.load();
        guard_release(h);
        check(word(r.e.registry_word()) == 0 && mem::test_releases.load() == rel && released(h),
              t + "its release afterwards touches no word (no double release of the gate's share)");
    }
    // a gate record reused for a new hold of the same word: the borrowed share is live again
    {
        GateState again = gate_record();
        check(r.gate(again) == Reason::None, t + "gate for reuse");
        ResourceGuard h = guard_record();
        check(r.borrowed(again, h) == Reason::None, t + "borrowed for reuse");
        gate_release(again);
        again.step = ++g_step;
        check(r.gate(again) == Reason::None && guard_validate(&r.L, h),
              t + "the same gate record held again with the same word: the borrow is live again");
        guard_release(h);
        gate_release(again);
    }
    // gate, then guard: a borrowed guard needs a held gate
    {
        GateState never = gate_record();
        const std::uint64_t reads0 = mem::test_reads.load(), cas0 = mem::test_cas.load();
        ResourceGuard h = guard_record();
        check(r.borrowed(never, h) == Reason::ResourceLifetimeGateNotHeld && released(h) &&
                  mem::test_reads.load() == reads0 && mem::test_cas.load() == cas0,
              t + "borrowed from a gate that is not held: refused before any engine read");
    }
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "words free at the end");
}

void refusals(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    const auto& w = r.L.write;
    const auto& rw = r.L.resource_write;
    const auto nothing = [] {};
    // before any engine read or CAS: the caller's thread, the profile, the image
    for (const auto& [caller, want] : {std::pair{CallerThread::ServerWorld, Reason::InFrame},
                                       std::pair{CallerThread::ClientWorld, Reason::WorldThread},
                                       std::pair{CallerThread::Unknown, Reason::CallerThreadUnknown}}) {
        ResourceGuard g = guard_record();
        const std::uint64_t reads = mem::test_reads.load(), cas = mem::test_cas.load();
        const Reason got = r.owned(g, caller);
        check(got == want && released(g) && mem::test_reads.load() == reads && mem::test_cas.load() == cas &&
                  word(r.e.registry_word()) == 0,
              t + "owned on a world thread: " + name(want) + " before any engine read (got " + name(got) + ")");
    }
    {
        ResourceGuard g = guard_record();
        Layout off = r.L;
        off.resource_write.enabled = false;
        const std::uint64_t reads = mem::test_reads.load(), cas = mem::test_cas.load();
        check(guard_acquire(nullptr, nullptr, r.e.service(), r.e.reader(), g, CallerThread::Other, r.image()) ==
                      Reason::ResourceLifetimeUnavailable &&
                  guard_acquire(&off, nullptr, r.e.service(), r.e.reader(), g, CallerThread::Other, r.image()) ==
                      Reason::ResourceLifetimeUnavailable &&
                  guard_acquire(&r.L, nullptr, r.e.service(), r.e.reader(), g, CallerThread::Other, 0) ==
                      Reason::ResourceLifetimeUnavailable &&
                  released(g) && mem::test_reads.load() == reads && mem::test_cas.load() == cas,
              t + "no exact profile, a disabled resource worker layout, no image: unavailable before any engine read");
    }
    // the owners, resolved before the word (Prepare)
    if (r.L.dedicated_server) {
        owned_refusal(r, "the GameServer thread unregistered", Reason::EcsServerThreadUnregistered,
                      [&] { r.e.set_rows(false); }, [&] { r.e.set_rows(true); });
    } else {
        const std::uintptr_t root = r.e.image.at(r.L.root_rva);
        owned_refusal(r, "the root owner changed", Reason::RootVtableMismatch, [&] { fake::put<std::uint64_t>(root, 0); },
                      [&] { fake::put<std::uint64_t>(root, r.e.image.at(r.L.root_vtable)); });
    }
    // the word: a registering thread holds it exclusively; one try, no retry, no wait
    {
        set_word(r.e.registry_word(), 0x80000000u);
        ResourceGuard g = guard_record();
        const std::uint64_t cas = mem::test_cas.load();
        const Reason got = r.owned(g);
        check(got == Reason::EcsThreadRegistryBusy && released(g) && mem::test_cas.load() == cas + 1 &&
                  word(r.e.registry_word()) == static_cast<std::int32_t>(0x80000000u),
              t + "a thread registering: ecs-thread-registry-busy after one try (got " + name(got) + ")");
        set_word(r.e.registry_word(), 0);
    }
    // inside AcquirePrepared, with the word held: changed at the Held point, after the owners were resolved
    const std::uintptr_t count = r.e.image.at(w.registry_count);
    const std::uint16_t rows = fake::get<std::uint16_t>(count);
    owned_refusal(r, "registry count over capacity", Reason::EcsThreadRegistryCountInvalid,
                  [&] { g_on_held = [&] { fake::put<std::uint16_t>(count, static_cast<std::uint16_t>(w.capacity + 1)); }; },
                  [&] { fake::put<std::uint16_t>(count, rows); });
    // the bound is count > capacity (hold_resource_lifetime.cpp:98): a full registry, every row after the real ones empty,
    // is scanned and acquired, owned and borrowed
    {
        std::memset(reinterpret_cast<void*>(r.e.row(rows)), 0, (w.capacity - rows) * w.stride);
        fake::put<std::uint16_t>(count, static_cast<std::uint16_t>(w.capacity));
        ResourceGuard g = guard_record();
        const Reason got = r.owned(g);
        check(got == Reason::None && g.held == 1, t + "a full registry (count == capacity): owned acquire (got " + name(got) + ")");
        guard_release(g);
        GateState gate = gate_record();
        const Reason gated = r.gate(gate);
        const Reason borrowed = gated == Reason::None ? r.borrowed(gate, g) : gated;
        check(borrowed == Reason::None && g.held == 1,
              t + "a full registry (count == capacity): borrowed acquire (got " + name(borrowed) + ")");
        guard_release(g);
        gate_release(gate);
        fake::put<std::uint16_t>(count, rows);
        check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "a full registry: words free after");
    }
    {
        std::uintptr_t page = 0;
        const std::size_t index = unreadable_rows(r, page);
        check(index > 0, t + "an unreadable registry page for the row test");
        if (index) {
            owned_refusal(r, "a registry row unreadable", Reason::EcsThreadRegistryRowUnreadable,
                          [&] { g_on_held = [&] { fake::put<std::uint16_t>(count, static_cast<std::uint16_t>(index + 1)); }; },
                          [&] { fake::put<std::uint16_t>(count, rows); });
            mprotect(reinterpret_cast<void*>(page), 4096, PROT_READ | PROT_WRITE);
        }
    }
    const std::uintptr_t content_row = r.e.row(2);
    owned_refusal(r, "the worker's name unreadable", Reason::EcsThreadNameUnreadable,
                  [&] { fake::put<std::uint64_t>(content_row + w.row_name, fake::unmapped()); }, [&] { r.e.set_rows(true); });
    owned_refusal(r, "two content worker rows", Reason::ResourceLifetimeWorkerIsAmbiguous,
                  [&] { r.e.content_rows = 2; r.e.set_rows(true); }, [&] { r.e.content_rows = 1; r.e.set_rows(true); });
    owned_refusal(r, "the worker row's handle is 0", Reason::ResourceLifetimeWorkerRangeInvalid,
                  [&] { fake::put<std::uint64_t>(content_row + w.row_handle, 0); }, [&] { r.e.set_rows(true); });
    owned_refusal(r, "no content worker registered", Reason::ResourceLifetimeWorkerUnregistered,
                  [&] { r.e.content_rows = 0; r.e.set_rows(true); }, [&] { r.e.content_rows = 1; r.e.set_rows(true); });
    owned_refusal(r, "another service expected", Reason::ResourceLifetimeServiceMismatch, nothing, nothing,
                  r.e.service() + 8);
    const std::uintptr_t payload = r.e.payload.at();
    const std::uint64_t payload_size = r.e.payload.size;
    const auto restore_payload = [&] {
        fake::put<std::uint64_t>(r.service_field(rw.payload_pointer), payload);
        fake::put<std::uint64_t>(r.service_field(rw.payload_size), payload_size);
    };
    owned_refusal(r, "no payload", Reason::ResourceLifetimePayloadUnavailable,
                  [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_pointer), 0); }, restore_payload);
    owned_refusal(r, "an empty payload", Reason::ResourceLifetimePayloadUnavailable,
                  [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_size), 0); }, restore_payload);
    owned_refusal(r, "a payload range that overflows", Reason::ResourceLifetimePayloadRangeInvalid,
                  [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_pointer), UINTPTR_MAX - 15);
                        fake::put<std::uint64_t>(r.service_field(rw.payload_size), 64); },
                  restore_payload);
    // Validate at acquire: the worker thread, each field
    const auto worker = [&](const std::string& what, std::size_t offset, std::uint64_t bad, std::size_t bytes = 8) {
        const std::uintptr_t at = r.thread_field(offset);
        std::uint64_t saved = 0;
        std::memcpy(&saved, reinterpret_cast<void*>(at), bytes);
        owned_refusal(r, what, Reason::ResourceLifetimeWorkerInvalidOrStopping,
                      [&] { std::memcpy(reinterpret_cast<void*>(at), &bad, bytes); },
                      [&] { std::memcpy(reinterpret_cast<void*>(at), &saved, bytes); });
    };
    worker("the worker is stopping", w.thread_stop, 1, 4);
    worker("the worker's context changed", w.thread_context, r.e.content_thread() + 8);
    worker("the worker's handle changed", w.thread_handle, r.e.content_handle + 1);
    worker("the worker's callback changed", w.thread_callback, r.e.image.at(rw.callback) + 16);
    worker("the worker's name pointer changed", w.name_pointer, r.e.content_thread() + w.inline_name + 1);
    worker("the worker's name length changed", w.name_length, rw.name_size + 1);
    worker("the worker's name capacity changed", w.name_capacity, w.name_capacity_value + 1);
    owned_refusal(r, "the service not initialized", Reason::ResourceLifetimeServiceNotInitialized,
                  [&] { fake::put<std::uint32_t>(r.service_field(rw.initialized), 0); },
                  [&] { fake::put<std::uint32_t>(r.service_field(rw.initialized), rw.initialized_value); });
    owned_refusal(r, "another reader expected", Reason::ResourceLifetimeReaderChanged, nothing, nothing, 0,
                  r.e.reader() + 8);
    owned_refusal(r, "the service's reader replaced", Reason::ResourceLifetimeReaderChanged,
                  [&] { fake::put<std::uint64_t>(r.service_field(rw.reader), r.e.reader() + 64); },
                  [&] { fake::put<std::uint64_t>(r.service_field(rw.reader), r.e.reader()); });
    owned_refusal(r, "the payload moved after the scan", Reason::ResourceLifetimePayloadChanged,
                  [&] { g_on_scanned = [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_pointer), payload + 8); }; },
                  restore_payload);
    owned_refusal(r, "the payload resized after the scan", Reason::ResourceLifetimePayloadChanged,
                  [&] { g_on_scanned = [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_size), payload_size - 8); }; },
                  restore_payload);
    // the owners: resolve to others, or no longer resolve, after the word was taken
    if (r.L.dedicated_server) {
        owned_refusal(r, "the world's uuid changed", Reason::ResourceLifetimeOwnerChanged,
                      [&] { g_on_held = [&] { r.e.set_entry(0, r.L.entry_ready_state, r.e.context.at(), {0x3333, 0x4444}); }; },
                      [&] { r.e.set_entry(0, r.L.entry_ready_state, r.e.context.at(), {0x1111, 0x2222}); });
        owned_refusal(r, "the GameServer thread gone after the word", Reason::ResourceLifetimeOwnerChanged,
                      [&] { g_on_held = [&] { r.e.set_rows(false); }; }, [&] { r.e.set_rows(true); });
    } else {
        const std::uintptr_t main_owner = r.e.controller.at(r.L.main_owner_offset);
        owned_refusal(r, "the main owner changed", Reason::ResourceLifetimeOwnerChanged,
                      [&] { g_on_held = [&] { fake::put<std::uint64_t>(main_owner, r.e.main_table.at()); }; },
                      [&] { fake::put<std::uint64_t>(main_owner, r.e.main_world.at()); });
        const std::uintptr_t root = r.e.image.at(r.L.root_rva);
        owned_refusal(r, "the root gone after the word", Reason::ResourceLifetimeOwnerChanged,
                      [&] { g_on_held = [&] { fake::put<std::uint64_t>(root, 0); }; },
                      [&] { fake::put<std::uint64_t>(root, r.e.image.at(r.L.root_vtable)); });
    }
    // borrowed: the gate's checks, then the same scan under the gate's share
    {
        GateState gate = gate_record();
        check(r.gate(gate) == Reason::None, t + "gate for the borrowed refusals");
        fake::put<std::uint32_t>(r.e.thread + w.thread_stop, 1);  // the GameServer thread stopping: the gate no longer validates
        ResourceGuard g = guard_record();
        check(r.borrowed(gate, g) == Reason::ResourceLifetimeGateChanged && released(g) && word(r.e.registry_word()) == 1,
              t + "borrowed from a gate that no longer validates: refused, the gate's share kept");
        fake::put<std::uint32_t>(r.e.thread + w.thread_stop, 0);
        GateState other = gate;  // a held record naming another registry word
        other.registry_lock += 4;
        check(r.borrowed(other, g) == Reason::ResourceLifetimeRegistryMismatch && released(g) && word(r.e.registry_word()) == 1,
              t + "borrowed from a gate holding another registry word: refused");
        gate_release(gate);
    }
    borrowed_refusal(r, "no content worker registered", Reason::ResourceLifetimeWorkerUnregistered,
                     [&] { r.e.content_rows = 0; r.e.set_rows(true); }, [&] { r.e.content_rows = 1; r.e.set_rows(true); });
    borrowed_refusal(r, "registry count over capacity", Reason::EcsThreadRegistryCountInvalid,
                     [&] { g_on_held = [&] { fake::put<std::uint16_t>(count, static_cast<std::uint16_t>(w.capacity + 1)); }; },
                     [&] { fake::put<std::uint16_t>(count, rows); });
    borrowed_refusal(r, "the payload moved after the scan", Reason::ResourceLifetimePayloadChanged,
                     [&] { g_on_scanned = [&] { fake::put<std::uint64_t>(r.service_field(rw.payload_pointer), payload + 8); }; },
                     restore_payload);
    borrowed_refusal(r, "the service not initialized", Reason::ResourceLifetimeServiceNotInitialized,
                     [&] { fake::put<std::uint32_t>(r.service_field(rw.initialized), 0); },
                     [&] { fake::put<std::uint32_t>(r.service_field(rw.initialized), rw.initialized_value); });
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "refusals: words free at the end");
    ResourceGuard g = guard_record();
    check(r.owned(g) == Reason::None, t + "after every refusal and undo, the guard acquires again");
    guard_release(g);
}

void validate_and_contains(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    const auto& w = r.L.write;
    const auto& rw = r.L.resource_write;
    ResourceGuard g = guard_record();
    check(r.owned(g) == Reason::None && guard_validate(&r.L, g), t + "held and valid");
    const auto flips = [&](const std::string& what, std::uintptr_t at, std::uint64_t bad, std::size_t bytes) {
        std::uint64_t saved = 0;
        std::memcpy(&saved, reinterpret_cast<void*>(at), bytes);
        std::memcpy(reinterpret_cast<void*>(at), &bad, bytes);
        const bool invalid = !guard_validate(&r.L, g);
        std::memcpy(reinterpret_cast<void*>(at), &saved, bytes);
        check(invalid && guard_validate(&r.L, g), t + "validate refuses " + what + ", and passes again once restored");
    };
    flips("a stopping worker", r.thread_field(w.thread_stop), 1, 4);
    flips("a changed worker context", r.thread_field(w.thread_context), 0, 8);
    flips("a changed worker handle", r.thread_field(w.thread_handle), 1, 8);
    flips("a changed worker callback", r.thread_field(w.thread_callback), 1, 8);
    flips("a changed worker name pointer", r.thread_field(w.name_pointer), 1, 8);
    flips("a changed worker name length", r.thread_field(w.name_length), 1, 8);
    flips("a changed worker name capacity", r.thread_field(w.name_capacity), 1, 8);
    flips("an uninitialized service", r.service_field(rw.initialized), 0, 4);
    flips("another reader", r.service_field(rw.reader), r.e.reader() + 8, 8);
    flips("a moved payload", r.service_field(rw.payload_pointer), r.e.payload.at() + 8, 8);
    flips("a resized payload", r.service_field(rw.payload_size), r.e.payload.size + 8, 8);
    if (r.L.dedicated_server)
        flips("another world", r.e.entries.at(r.L.entry_uuid_offset), 0x9999, 8);
    else
        flips("another main owner", r.e.controller.at(r.L.main_owner_offset), r.e.main_table.at(), 8);
    const std::uintptr_t p = r.e.payload.at();
    const std::size_t n = r.e.payload.size;
    check(guard_contains(g, p, n) && guard_contains(g, p, 0) && guard_contains(g, p + n, 0) && guard_contains(g, p + 1, n - 1) &&
              guard_contains(g, p + n - 8, 8),
          t + "contains: the whole payload, empty ranges at both ends, a tail");
    check(!guard_contains(g, p, n + 1) && !guard_contains(g, p + n, 1) && !guard_contains(g, p - 1, 1) &&
              !guard_contains(g, p + 1, n) && !guard_contains(g, p + 8, SIZE_MAX) && !guard_contains(g, UINTPTR_MAX, 1) &&
              !guard_contains(g, 0, 0),
          t + "contains refuses past the end, before the start, and overflowing sizes");
    guard_release(g);
    check(!guard_contains(g, p, 1) && !guard_validate(&r.L, g), t + "released: contains and validates nothing");
    ResourceGuard forged = guard_record();
    forged.payload = p;
    forged.payload_size = n;
    forged.registry_lock = r.e.registry_word();
    check(!guard_contains(forged, p, 1) && !guard_validate(&r.L, forged), t + "a record that was never acquired is not held");
    check(!guard_validate(nullptr, g), t + "no profile: nothing validates");
}

// The RAII scopes over a table wired straight to the functions (the plugin host test runs the real table): the guard is
// released before the gate, an exception inside both releases both, a borrowed scope on a refused gate is refused.
const Layout* g_table_layout = nullptr;
std::uintptr_t g_table_image = 0;
std::string g_order;
KilnEngineNativeApi table() {
    KilnEngineNativeApi n{};
    n.header = {sizeof n, KILN_ENGINE_NATIVE_VERSION};
    n.ecs_gate_acquire = [](const EcsView* v, EcsGate* g, std::uint32_t* reason) -> KilnResult {
        const Reason r = gate_acquire(g_table_layout, *v, *g, CallerThread::Other, g_table_image);
        *reason = static_cast<std::uint32_t>(r);
        return r == Reason::None ? KILN_OK : KILN_ERR_UNAVAILABLE;
    };
    n.ecs_gate_validate = [](const EcsGate* g) -> std::int32_t { return gate_validate(g_table_layout, *g, g_table_image); };
    n.ecs_gate_release = [](EcsGate* g) {
        if (g->held) g_order += 'G';
        gate_release(*g);
    };
    n.resource_guard_acquire = [](const EcsGate* gate, std::uintptr_t s, std::uintptr_t rd, ResourceGuard* out,
                                  std::uint32_t* reason) -> KilnResult {
        const Reason r = guard_acquire(g_table_layout, gate, s, rd, *out, CallerThread::Other, g_table_image);
        *reason = static_cast<std::uint32_t>(r);
        return r == Reason::None ? KILN_OK : KILN_ERR_UNAVAILABLE;
    };
    n.resource_guard_validate = [](const ResourceGuard* g) -> std::int32_t { return guard_validate(g_table_layout, *g); };
    n.resource_guard_contains = [](const ResourceGuard* g, std::uintptr_t a, std::size_t s) -> std::int32_t {
        return guard_contains(*g, a, s);
    };
    n.resource_guard_release = [](ResourceGuard* g) {
        if (g->held) g_order += 'R';
        guard_release(*g);
    };
    return n;
}

void scopes(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    g_table_layout = &r.L;
    g_table_image = r.image();
    const KilnEngineNativeApi n = table();
    g_order.clear();
    {
        EcsGateScope gate(n, r.view, 1, ++g_step);
        ResourceGuardScope guard(n, gate, r.e.service(), r.e.reader());
        check(gate.held() && guard.held() && guard.validate() && guard.contains(r.e.payload.at(), 8) &&
                  guard.payload() == r.e.payload.at() && guard.guard().owns_registry == 0,
              t + "scopes: gate, then a borrowed guard");
    }
    check(g_order == "RG", t + "scopes: the guard is released before the gate (" + g_order + ")");
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "scopes: both words free");
    g_order.clear();
    try {
        EcsGateScope gate(n, r.view, 1, ++g_step);
        ResourceGuardScope guard(n, gate, r.e.service(), r.e.reader());
        check(guard.held(), t + "scopes: held before the throw");
        throw std::runtime_error("member failed");
    } catch (const std::runtime_error&) {
    }
    check(g_order == "RG" && word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0,
          t + "scopes: an exception inside both releases the guard, then the gate");
    {
        set_word(r.e.scene_word(), 1);  // the update's frame: the gate is refused
        EcsGateScope gate(n, r.view, 1, ++g_step);
        ResourceGuardScope guard(n, gate, r.e.service(), r.e.reader());
        check(!gate.held() && !guard.held() && guard.reason() == Reason::ResourceLifetimeGateNotHeld && !guard.validate() &&
                  !guard.contains(r.e.payload.at(), 1) && guard.payload() == 0,
              t + "scopes: a borrowed guard on a refused gate is refused");
        set_word(r.e.scene_word(), 0);
    }
    {
        ResourceGuardScope owned(n, r.e.service(), r.e.reader());
        check(owned.held() && owned.guard().owns_registry == 1 && word(r.e.registry_word()) == 1, t + "scopes: owned");
        owned.release();
        check(!owned.held() && word(r.e.registry_word()) == 0, t + "scopes: owned released early");
    }
    check(word(r.e.registry_word()) == 0, t + "scopes: an early release and the destructor release once");
}

// Owned and borrowed holders against a simulated thread (un)registration: it takes the registry word exclusively
// (one CAS, as the engine's registration does), then tears the worker down and rebuilds it with plain stores, payload
// included. A holder reads the payload with plain loads: under ThreadSanitizer any overlap is a reported race, and a
// torn pair or a failed validate inside a hold is counted.
void holder_race(const char* target) {
    Rig r(target);
    const auto& t = r.t;
    const auto& rw = r.L.resource_write;
    struct Pair { std::uint64_t a, b; };
    auto* pair = reinterpret_cast<Pair*>(r.e.payload.at(256));
    std::atomic<bool> stop{false};
    std::atomic<long> registrations{0}, holds{0}, busy{0}, torn{0}, invalid{0}, borrowed_holds{0}, gate_busy{0}, wrong{0};
    std::atomic<int> inside{0}, most_inside{0}, work{0};
    std::atomic<bool> first_held{false};
    // The meeting is forced, not left to the schedule: the holders start only once the registrar holds the exclusive word
    // for its first registration, which it keeps until a holder has been refused; the holders run their quota and then go
    // on until a registration, a busy refusal, an owned hold and a borrowed hold have all happened. Bounded waits (the
    // registrar gave up after 100000 yields, the holders after 20 s) let a loaded machine schedule every try outside the
    // registrar's window (seen: registrations and holds, no busy refusal), and holders that start first can keep the
    // word shared long enough to starve the registrar's first CAS. Each of these waits is for something that must
    // happen, so the deadline is only an upper bound a passing run never reaches; reaching it fails the checks below.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(300);
    const auto open = [&] { return std::chrono::steady_clock::now() < deadline; };
    const auto contended = [&] { return busy.load() + gate_busy.load() > 0; };
    const auto met = [&] {
        return registrations.load() > 0 && contended() && holds.load() > 0 && borrowed_holds.load() > 0;
    };
    const auto more = [&](int i, int quota) { return i < quota || (!met() && open()); };
    std::thread registrar([&] {
        std::int32_t* registry = fake::word(r.e.registry_word());
        std::uint64_t value = 0;
        bool first = true;
        while (!stop.load()) {
            std::int32_t expected = 0;
            if (__atomic_compare_exchange_n(registry, &expected, static_cast<std::int32_t>(0x80000000u), false,
                                            __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                fake::put<std::uint32_t>(r.service_field(rw.initialized), 0);
                pair->a = ++value;
                for (int i = 0; i < 2000; ++i) work.fetch_add(1, std::memory_order_relaxed);  // the registration's work
                // the first registration: inside until a holder was refused (the holders start now and go on until one is)
                if (first) first_held = true;
                while (first && !contended() && !stop.load() && open()) std::this_thread::yield();
                first = false;
                pair->b = value;
                fake::put<std::uint32_t>(r.service_field(rw.initialized), rw.initialized_value);
                ++registrations;
                __atomic_store_n(registry, 0, __ATOMIC_SEQ_CST);
            }
            std::this_thread::yield();
        }
    });
    const auto read_pair = [&](const ResourceGuard& g) {
        const int now = ++inside;
        for (int seen = most_inside.load(); now > seen && !most_inside.compare_exchange_weak(seen, now);) {}
        if (!guard_contains(g, reinterpret_cast<std::uintptr_t>(pair), sizeof *pair)) ++invalid;
        const std::uint64_t a = pair->a, b = pair->b;
        if (a != b) ++torn;
        if (!guard_validate(&r.L, g)) ++invalid;
        --inside;
    };
    const auto owned = [&] {
        for (int i = 0; more(i, 3000); ++i) {
            ResourceGuard g = guard_record();
            const Reason got = r.owned(g);
            if (got != Reason::None) {
                if (got == Reason::EcsThreadRegistryBusy) ++busy; else ++wrong;
                continue;
            }
            ++holds;
            read_pair(g);
            guard_release(g);
        }
    };
    const auto borrowed = [&] {
        for (int i = 0; more(i, 1500); ++i) {
            GateState gate = gate_record(2);
            if (r.gate(gate) != Reason::None) { ++gate_busy; continue; }
            ResourceGuard g = guard_record();
            if (r.borrowed(gate, g) == Reason::None) {
                ++borrowed_holds;
                read_pair(g);
                guard_release(g);
            } else {
                ++wrong;  // a held gate holds the registry share: the registrar cannot be inside
            }
            gate_release(gate);
        }
    };
    while (!first_held.load() && open()) std::this_thread::yield();
    std::thread h1(owned), h2(owned), b1(borrowed);
    h1.join();
    h2.join();
    b1.join();
    stop = true;
    registrar.join();
    check(!torn.load() && !invalid.load(), t + "race: no torn payload read and no failed validate inside a hold");
    check(!wrong.load(), t + "race: owned holders refused only ecs-thread-registry-busy; borrowed never refused");
    // contention: the registrar was seen holding the word by an owned try or a gate's (the immediate busy refusal itself
    // is checked deterministically in refusals())
    check(holds.load() > 0 && borrowed_holds.load() > 0 && registrations.load() > 0 && busy.load() + gate_busy.load() > 0,
          t + "race: owned and borrowed holds, registrations and busy refusals all happened");
    check(word(r.e.registry_word()) == 0 && word(r.e.scene_word()) == 0, t + "race: words free at the end");
    std::printf("%s guard race: %ld owned holds, %ld busy, %ld borrowed holds, %ld gate busy, %ld registrations, up to %d "
                "holders at once\n",
                target, holds.load(), busy.load(), borrowed_holds.load(), gate_busy.load(), registrations.load(),
                most_inside.load());
}
}  // namespace

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return {}; }  // engine.cpp is linked for stats(); never booted here
}  // namespace kiln::engine

int main() {
    g_guard_test_point = &point;
    for (const char* t : {"enshrouded.exe", "enshrouded_server.exe"}) {
        owned_and_release(t);
        borrowed_and_order(t);
        refusals(t);
        validate_and_contains(t);
        scopes(t);
        holder_race(t);
    }
    // the reasons: one string each, all distinct and in ShroudForge's style
    std::vector<std::string> names;
    for (auto i = static_cast<std::uint32_t>(Reason::ResourceLifetimeAlreadyHeld);
         i < static_cast<std::uint32_t>(Reason::EngineCallGateNotHeld); ++i)  // the engine call's reasons follow (invoke_test)
        names.push_back(reason_name(static_cast<Reason>(i)));
    bool distinct = true;
    for (std::size_t i = 0; i < names.size(); ++i)
        for (std::size_t j = i + 1; j < names.size(); ++j) distinct = distinct && names[i] != names[j];
    bool styled = true;
    for (const auto& s : names) styled = styled && s.rfind("resource-lifetime-", 0) == 0;
    check(names.size() == 16 && distinct && styled, "16 distinct resource-lifetime-* reasons");
    check(offsetof(KilnEngineNativeApi, resource_guard_release) + sizeof(void*) == 136 && sizeof(Layout) != 984,
          "the guard members end at 136 bytes and the layout changed size (984 before it): a mixed build is refused");
    std::printf("engine resource guard: %d checks, %d failures\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
