// SPDX-License-Identifier: MIT
// kiln.engine: owner/Scene/ECS views and EcsComponentWriteSupported over a fake engine laid out from both real
// profiles; every refusal on these paths; a registration thread and a mutator overlapping the
// reads (ThreadSanitizer build checks the lock-word orderings); the registry word always restored.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <set>
#include <string>
#include <thread>

#include "fake_engine.h"
#include "memory.h"
#include "profiles.g.h"
#include "resolve.h"

namespace kiln::engine::mem {
extern std::atomic<std::uintptr_t> test_image;
extern std::atomic<std::uint64_t> test_reads;
extern std::atomic<void (*)(std::uint64_t)> test_read_hook;
}

namespace {
using namespace kiln::engine;
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL %s\n", what.c_str()); }
}
std::string name(Reason r) { return reason_name(r); }
void expect(const Layout& L, ViewKind kind, std::uintptr_t image, Reason want, const std::string& what) {
    EcsView v{};
    const Reason got = ecs_view(&L, kind, v, image);
    check(got == want, what + " (got " + name(got) + ", want " + name(want) + ")");
}

const Layout& layout_of(const char* target) {
    for (const auto& p : generated::kProfiles)
        if (std::string(p.target) == target) return p.layout;
    std::abort();
}

void server() {
    const Layout& L = layout_of("enshrouded_server.exe");
    const std::string s = "server: ";
    {
        fake::Engine e(L);
        EcsView v{};
        check(ecs_view(&L, ViewKind::Authority, v, e.image.at()) == Reason::None, s + "authority view resolves");
        check(v.role == Role::LocalAuthority && v.owners.dedicated_server && v.owners.main_owner == e.scene.at() &&
                  v.scene_owner == e.scene.at() && v.scene_context == e.context.at() &&
                  v.execution_owner == e.exec.at() && v.registry_owner == e.registry_owner() && v.manager == e.manager() &&
                  v.entity_table == e.table.at() && v.owners.world_context == e.context.at(),
              s + "view fields are the engine's owners");
        check(*fake::word(e.registry_word()) == 0, s + "registry word released after the traversal");
        expect(L, ViewKind::Client, e.image.at(), Reason::DedicatedServerHasNoLocalClientInput, s + "no client view");
        expect(L, ViewKind::Authority, 0, Reason::EcsThreadRegistryBusy, s + "no image");
        check(ecs_view(nullptr, ViewKind::Authority, v, e.image.at()) == Reason::EcsReadOwnerProfileUnavailable,
              s + "no exact profile");
        // the registry word held exclusively (a thread registering): refused, nothing waited, word untouched
        *fake::word(e.registry_word()) = static_cast<std::int32_t>(0x80000000u);
        expect(L, ViewKind::Authority, e.image.at(), Reason::EcsThreadRegistryBusy, s + "registry exclusive");
        check(*fake::word(e.registry_word()) == static_cast<std::int32_t>(0x80000000u), s + "busy word left as it was");
        *fake::word(e.registry_word()) = 3;  // three other readers
        check(ecs_view(&L, ViewKind::Authority, v, e.image.at()) == Reason::None && *fake::word(e.registry_word()) == 3,
              s + "shared with other readers, released back to their count");
        *fake::word(e.registry_word()) = 0;
    }
    const auto with = [&](auto edit, Reason want, const std::string& what) {
        fake::Engine e(L);
        edit(e);
        expect(L, ViewKind::Authority, e.image.at(), want, s + what);
        check(*fake::word(e.registry_word()) == 0, s + what + ": registry word released");
    };
    const auto& w = L.write;
    with([&](fake::Engine& e) { e.set_rows(false); }, Reason::EcsServerThreadUnregistered, "no GameServer row");
    with([&](fake::Engine& e) { e.set_rows(true, true); }, Reason::EcsServerThreadIsAmbiguous, "two GameServer rows");
    with([&](fake::Engine& e) { fake::put<std::uint16_t>(e.image.at(w.registry_count), 300); }, Reason::EcsThreadRegistryCountInvalid,
         "row count over capacity");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.image.at(w.registry_entries) + w.stride + w.row_name, fake::unmapped()); },
         Reason::EcsThreadNameUnreadable, "thread name unreadable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.image.at(w.registry_entries) + w.stride + w.row_handle, 0); },
         Reason::EcsServerThreadRangeInvalid, "row without handle");
    with([&](fake::Engine& e) { fake::put<std::uint32_t>(e.thread + w.thread_stop, 1); }, Reason::EcsServerThreadOwnerInvalidOrStopping,
         "thread stopping");
    with([&](fake::Engine& e) { fake::put<std::uint8_t>(e.scene.at(w.scene_state), 2); }, Reason::EcsServerThreadOwnerInvalidOrStopping,
         "Scene not running");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.thread + w.thread_callback, e.image.at(w.callback + 16)); },
         Reason::EcsServerThreadOwnerInvalidOrStopping, "another thread callback");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.thread + w.thread_context, e.scene.at()); },
         Reason::EcsServerThreadOwnerInvalidOrStopping, "thread context not the Scene's");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.thread + w.thread_handle, 7); }, Reason::EcsServerThreadOwnerInvalidOrStopping,
         "thread handle not the row's");
    with([&](fake::Engine& e) { e.set_entry(0, 1, e.context.at(), {1, 2}); }, Reason::EcsActiveSceneIsTransitioning,
         "no ready entry (dedicated)");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_count_offset), 2); e.set_entry(1, L.entry_ready_state, e.context.at(), {3, 4}); },
         Reason::EcsLocalAuthorityIsAmbiguous, "two ready entries");
    with([&](fake::Engine& e) { e.set_entry(0, L.entry_ready_state, e.context.at(), {0, 0}); }, Reason::EcsReadySceneContextInvalid,
         "ready entry without uuid");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_count_offset), L.maximum_scenes + 1); },
         Reason::EcsActiveSceneListUnavailable, "scene count over the maximum");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_entries_offset), fake::unmapped()); },
         Reason::EcsSceneStateUnreadable, "entries unreadable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.context.at(L.execution_owner_offset), 0); },
         Reason::EcsExecutionOwnerUnavailable, "no execution owner");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.manager() + L.manager_owner_offset, 8); },
         Reason::EcsManagerRegistryOwnerMismatch, "manager owned by another registry");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.manager() + L.entities.manager_count, 0); },
         Reason::EcsEntityTableUnavailable, "empty entity manager");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.manager() + L.entities.manager_count, L.entities.maximum_entities + 1); },
         Reason::EcsEntityTableUnavailable, "entity count over the maximum");
}

void client() {
    const Layout& L = layout_of("enshrouded.exe");
    const std::string c = "client: ";
    {
        fake::Engine e(L);
        EcsView v{};
        check(ecs_view(&L, ViewKind::Authority, v, e.image.at()) == Reason::None && v.role == Role::LocalAuthority &&
                  v.scene_owner == e.scene.at() && v.execution_owner == e.exec.at() && !v.owners.dedicated_server &&
                  v.owners.client == e.client.at() && v.owners.controller == e.controller.at() &&
                  v.owners.main_owner == e.main_world.at(),
              c + "hosting: the authority view is the hosted Scene's");
        EcsView mine{};
        check(ecs_view(&L, ViewKind::Client, mine, e.image.at()) == Reason::None && mine.role == Role::ClientReplica &&
                  mine.execution_owner == e.main_exec.at() && mine.scene_owner == 0,
              c + "the client view is the main world's, never the hosted authority");
        // joined another machine: an empty scene list, the authority view falls back to the client replica
        fake::put<std::uint64_t>(e.scene.at(L.scene_count_offset), 0);
        check(ecs_view(&L, ViewKind::Authority, v, e.image.at()) == Reason::None && v.role == Role::ClientReplica &&
                  v.execution_owner == e.main_exec.at(),
              c + "joined: replica role, main world's execution owner");
        check(!ecs_write_supported(&L, v, 3, e.image.at()), c + "no writes on a replica");
        check(*fake::word(e.registry_word()) == 0, c + "the client path does not touch the registry word");
    }
    const auto with = [&](auto edit, Reason want, const std::string& what) {
        fake::Engine e(L);
        edit(e);
        expect(L, ViewKind::Authority, e.image.at(), want, c + what);
    };
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.image.at(L.root_rva), e.image.at(L.root_vtable + 8)); },
         Reason::RootVtableMismatch, "root vtable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.client.at(), 0); }, Reason::ClientOwnerUnavailableOrVtableMismatch,
         "client vtable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.image.at(L.root_rva) + L.client_offset, fake::unmapped()); },
         Reason::ClientOwnerUnavailableOrVtableMismatch, "client unreadable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.controller.at(), 1); }, Reason::ClientControllerUnavailableOrVtableMismatch,
         "controller vtable");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.controller.at(L.controller_back_pointer), 1); },
         Reason::ClientControllerOwnerMismatch, "controller back pointer");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.controller.at(L.main_owner_offset), 0); }, Reason::MainOwnerUnavailable,
         "no main owner");
    with([&](fake::Engine& e) { fake::put<std::uint64_t>(e.client.at(L.client_scene_offset), 0); },
         Reason::EcsActiveSceneListUnavailable, "no client scene owner");
    with([&](fake::Engine& e) { e.set_entry(0, 1, e.context.at(), {1, 2}); }, Reason::EcsActiveSceneIsTransitioning,
         "a listed scene that is not ready");
}

void write_supported() {
    for (const char* target : {"enshrouded.exe", "enshrouded_server.exe"}) {
        const Layout& L = layout_of(target);
        const std::string t = std::string(target) + ": ";
        fake::Engine e(L);
        EcsView v{};
        check(ecs_view(&L, ViewKind::Authority, v, e.image.at()) == Reason::None, t + "view");
        check(ecs_write_supported(&L, v, 3, e.image.at()), t + "an untracked component is writable");
        fake::put<std::uint64_t>(e.registry_owner() + L.write.tracking_registry_a, 1ull << 3);
        check(!ecs_write_supported(&L, v, 3, e.image.at()) && ecs_write_supported(&L, v, 4, e.image.at()),
              t + "tracking category A refuses only its bit");
        fake::put<std::uint64_t>(e.registry_owner() + L.write.tracking_registry_b + 8, 1ull << 1);
        check(!ecs_write_supported(&L, v, 65, e.image.at()) && ecs_write_supported(&L, v, 64, e.image.at()),
              t + "category B in the second mask word refuses only its bit");
        check(ecs_write_supported(&L, v, 127, e.image.at()) && !ecs_write_supported(&L, v, 128, e.image.at()),
              t + "an index at the registration count is refused");
        EcsView other = v;
        other.manager += 8;
        check(!ecs_write_supported(&L, other, 4, e.image.at()), t + "a stale expected view refused");
        Layout off = L;
        off.write.enabled = false;
        check(!ecs_write_supported(&off, v, 4, e.image.at()) && !ecs_write_supported(nullptr, v, 4, e.image.at()),
              t + "no write gate, no write support");
        check(*fake::word(e.registry_word()) == 0, t + "registry word released");
    }
}

// A thread registering threads (engine: exclusive registry word around row edits) and a scene mutator overlap the
// reads. A view is either refused or exactly the engine's; the registry word ends at 0.
void overlap() {
    const Layout& L = layout_of("enshrouded_server.exe");
    fake::Engine e(L);
    std::atomic<bool> stop{false};
    std::atomic<int> busy{0}, ok{0}, other{0};
    std::thread registrar([&] {
        std::int32_t* word = fake::word(e.registry_word());
        while (!stop.load()) {
            std::int32_t expected = 0;
            if (!__atomic_compare_exchange_n(word, &expected, static_cast<std::int32_t>(0x80000000u), false, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST)) {
                std::this_thread::yield();
                continue;
            }
            std::this_thread::yield();  // the engine edits rows while it holds the word
            __atomic_store_n(word, 0, __ATOMIC_SEQ_CST);
            std::this_thread::sleep_for(std::chrono::microseconds(50));  // registrations are rare
        }
    });
    for (int i = 0; i < 20000; ++i) {
        EcsView v{};
        const Reason r = ecs_view(&L, ViewKind::Authority, v, e.image.at());
        if (r == Reason::None) {
            ++ok;
            if (v.execution_owner != e.exec.at() || v.manager != e.manager()) ++other;
        } else if (r == Reason::EcsThreadRegistryBusy || r == Reason::EcsEngineOwnerChangedDuringRead) {
            ++busy;  // the first or the second pass found the word held (ShroudForge reports the second as a change)
        } else {
            ++other;
        }
    }
    stop = true;
    registrar.join();
    check(!other.load() && ok.load() + busy.load() == 20000, "overlap: every view refused busy or exactly the engine's");
    check(*fake::word(e.registry_word()) == 0, "overlap: registry word back to 0");
    std::printf("overlap: %d views, %d busy\n", ok.load(), busy.load());
}

// The refusals no plain memory state reaches. Static ones: a layout or a pointer at a range limit. Unreachable through
// these members: engine-owner-profile-unavailable (ShroudForge's ResolveOwners only; no such member here) and
// ecs-server-world-unavailable (ShroudForge's fallback for a failure without a string; every path here names one).
void range_limits() {
    const Layout& L = layout_of("enshrouded_server.exe");
    const std::string s = "server: ";
    {
        fake::Engine e(L);
        fake::put<std::uint64_t>(e.scene.at(L.scene_entries_offset), 0);
        expect(L, ViewKind::Authority, e.image.at(), Reason::EcsActiveSceneListRangeInvalid, s + "listed scenes without a list");
    }
    {
        fake::Engine e(L);
        fake::put<std::uint64_t>(e.manager() + L.entities.manager_table, UINTPTR_MAX - 8);
        expect(L, ViewKind::Authority, e.image.at(), Reason::EcsEntityTableRangeOverflow, s + "entity table past the address space");
    }
    {
        fake::Engine e(L);
        Layout bad = L;
        bad.write.stride = 0;
        expect(bad, ViewKind::Authority, e.image.at(), Reason::EcsServerThreadLayoutInvalid, s + "registry row stride 0");
        bad = L;
        bad.write.registry_entries = SIZE_MAX;
        expect(bad, ViewKind::Authority, e.image.at(), Reason::EcsThreadRegistryRangeInvalid, s + "registry rows past the address space");
        check(*fake::word(e.registry_word()) == 0, s + "range limits: registry word released");
    }
}

// The "changed" refusals: one engine edit applied at every read index of a single view in turn (deterministic, one
// thread). Each read sees either the old or the new memory; the refusals ShroudForge raises for that edit must all
// appear, nothing else may.
std::uint64_t g_fire_at = 0;
std::function<void()> g_edit;
void fire(std::uint64_t n) {
    if (n != g_fire_at || !g_edit) return;
    const auto edit = std::move(g_edit);
    g_edit = nullptr;
    edit();
}
std::set<Reason> sweep(const Layout& L, ViewKind kind, const std::function<void(fake::Engine&)>& prepare,
                       const std::function<void(fake::Engine&)>& edit, int& points) {
    std::set<Reason> seen;
    for (std::uint64_t k = 1;; ++k) {
        fake::Engine e(L);
        prepare(e);
        EcsView v{};
        mem::test_reads = 0;
        g_fire_at = k;
        g_edit = [&] { edit(e); };
        mem::test_read_hook = &fire;
        const Reason r = ecs_view(&L, kind, v, e.image.at());
        mem::test_read_hook = nullptr;
        const bool fired = !g_edit;
        g_edit = nullptr;
        if (!fired) break;  // k is past the view's last read
        seen.insert(r);
        ++points;
        if (L.dedicated_server && *fake::word(e.registry_word()) != 0) seen.insert(Reason::Count);  // word not released
    }
    return seen;
}
void changed_during_read() {
    const auto none = [](fake::Engine&) {};
    // An edit landing inside the second pass fails that pass, which ShroudForge reports as a change during the read
    // (resolve_engine_owners.cpp:185-191), so ecs-engine-owner-changed-during-read belongs to every set.
    const auto expect_set = [](const Layout& L, const std::function<void(fake::Engine&)>& prepare,
                               const std::function<void(fake::Engine&)>& edit, const std::set<Reason>& want,
                               const std::string& what) {
        int points = 0;
        const std::set<Reason> got = sweep(L, ViewKind::Authority, prepare, edit, points);
        std::string names;
        for (const Reason r : got) names += std::string(" ") + (r == Reason::None ? "ok" : reason_name(r));
        check(got == want && points > 20, what + " (" + std::to_string(points) + " read points:" + names + ")");
    };
    const Reason ok = Reason::None, list = Reason::EcsActiveSceneListChanged, pass = Reason::EcsEngineOwnerChangedDuringRead;
    {
        const Layout& L = layout_of("enshrouded_server.exe");
        // the ready entry's world uuid: within one scan, between the owner and execution-owner scans, between the two
        // passes, or outside the view
        expect_set(L, none,
                   [&](fake::Engine& e) { fake::put<std::array<std::uint64_t, 2>>(e.entries.at(L.entry_uuid_offset), {7, 8}); },
                   {ok, Reason::EcsReadySceneContextChanged, Reason::EcsServerWorldOwnerChanged, pass},
                   "server: world uuid edited at each read");
        // the ready entry names another complete world context
        fake::Region context2(4096);
        expect_set(L, [&](fake::Engine& e) { e.wire_exec(context2, e.exec, e.table); },
                   [&](fake::Engine& e) { fake::put<std::uint64_t>(e.entries.at(L.entry_context_offset), context2.at()); },
                   {ok, Reason::EcsReadySceneContextChanged, Reason::EcsServerWorldOwnerChanged, pass},
                   "server: world context edited at each read");
        // a second (not ready) Scene is listed
        expect_set(L, [&](fake::Engine& e) { e.set_entry(1, 1, e.context.at(), {5, 6}); },
                   [&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_count_offset), 2); }, {ok, list, pass},
                   "server: scene list edited at each read");
        // the list moves to an identical copy
        fake::Region entries2(L.entry_stride * 4);
        expect_set(L, [&](fake::Engine& e) { std::memcpy(entries2.base, e.entries.base, L.entry_stride * 4); },
                   [&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_entries_offset), entries2.at()); },
                   {ok, list, pass}, "server: scene list moved at each read");
        // the world's execution owner is replaced by another complete one
        fake::Region exec2(L.entity_manager_offset + L.write.tracking_context + L.write.tracking_manager + 4096), table2(4096);
        expect_set(L,
                   [&](fake::Engine& e) {
                       e.wire_exec(e.context, exec2, table2);
                       fake::put<std::uint64_t>(e.context.at(L.execution_owner_offset), e.exec.at());
                   },
                   [&](fake::Engine& e) { fake::put<std::uint64_t>(e.context.at(L.execution_owner_offset), exec2.at()); },
                   {ok, pass}, "server: execution owner replaced at each read");
        // the GameServer row and its thread object get another handle (a re-registered thread)
        expect_set(L, none,
                   [&](fake::Engine& e) {
                       fake::put<std::uint64_t>(e.image.at(L.write.registry_entries) + L.write.stride + L.write.row_handle, 0x5678);
                       fake::put<std::uint64_t>(e.thread + L.write.thread_handle, 0x5678);
                   },
                   {ok, Reason::EcsServerThreadChangedDuringRead, Reason::EcsServerThreadOwnerInvalidOrStopping, pass},
                   "server: GameServer thread handle edited at each read");
    }
    {
        const Layout& L = layout_of("enshrouded.exe");
        expect_set(L, none,
                   [&](fake::Engine& e) { fake::put<std::array<std::uint64_t, 2>>(e.entries.at(L.entry_uuid_offset), {7, 8}); },
                   {ok, Reason::EcsReadySceneContextChanged, pass}, "client: hosted world uuid edited at each read");
        expect_set(L, [&](fake::Engine& e) { e.set_entry(1, 1, e.context.at(), {5, 6}); },
                   [&](fake::Engine& e) { fake::put<std::uint64_t>(e.scene.at(L.scene_count_offset), 2); }, {ok, list, pass},
                   "client: scene list edited at each read");
    }
}

// A concurrent mutator replaces the world's execution owner while views run (ThreadSanitizer build: the flips are
// atomic stores, the engine reads optimistic copies). Each view is refused as changed or is exactly one owner's.
void owner_flips() {
    const Layout& L = layout_of("enshrouded_server.exe");
    fake::Engine e(L);
    fake::Region exec2(L.entity_manager_offset + L.write.tracking_context + L.write.tracking_manager + 4096), table2(4096);
    e.wire_exec(e.context, exec2, table2);
    auto* slot = reinterpret_cast<std::uint64_t*>(e.context.at(L.execution_owner_offset));
    __atomic_store_n(slot, e.exec.at(), __ATOMIC_SEQ_CST);
    std::atomic<bool> stop{false};
    std::thread mutator([&] {
        for (bool second = true; !stop.load(); second = !second) {
            __atomic_store_n(slot, second ? exec2.at() : e.exec.at(), __ATOMIC_RELAXED);
            std::this_thread::yield();
        }
    });
    int first = 0, other_owner = 0, changed = 0, wrong = 0;
    for (int i = 0; i < 20000; ++i) {
        EcsView v{};
        const Reason r = ecs_view(&L, ViewKind::Authority, v, e.image.at());
        if (r == Reason::EcsEngineOwnerChangedDuringRead) { ++changed; continue; }
        if (r != Reason::None) { ++wrong; continue; }
        if (v.execution_owner == e.exec.at() && v.manager == e.manager() && v.entity_table == e.table.at()) ++first;
        else if (v.execution_owner == exec2.at() && v.manager == exec2.at(L.entity_manager_offset) && v.entity_table == table2.at())
            ++other_owner;
        else ++wrong;
    }
    stop = true;
    mutator.join();
    check(!wrong, "owner flips: every view refused as changed or exactly one owner's");
    check(*fake::word(e.registry_word()) == 0, "owner flips: registry word back to 0");
    std::printf("owner flips: %d + %d views, %d changed during read\n", first, other_owner, changed);
}

void copy_primitive() {
    fake::Region r(4096);
    fake::put<std::uint64_t>(r.at(8), 0x0123456789abcdefull);
    std::uint64_t v = 0;
    check(mem::read_span(r.at(8), &v, sizeof v) && v == 0x0123456789abcdefull, "copy reads engine memory");
    check(!mem::read_span(fake::unmapped(), &v, sizeof v), "an unmapped page fails the copy");
    check(!mem::read_span(0, &v, sizeof v) && !mem::read_span(UINTPTR_MAX - 2, &v, sizeof v), "null and overflowing ranges");
}
}  // namespace

int main() {
    server();
    client();
    write_supported();
    overlap();
    range_limits();
    changed_during_read();
    owner_flips();
    copy_primitive();
    std::printf("engine resolve: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
