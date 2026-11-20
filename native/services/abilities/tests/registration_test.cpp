// SPDX-License-Identifier: MIT
// Actual slot and add_kiln_native code, against a fake engine registry.
#include <atomic>
#include <string>
#include <cstdio>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "natives_core.h"
using namespace kab;
#include "slot_under_test.h"
std::mutex g_mu;
std::unordered_map<uint32_t, int> g_slot_of;
std::unordered_map<uint32_t, uint32_t> g_vanilla_argc, g_declared;
std::unordered_map<uint32_t, std::vector<KilnNativeArg>> g_typed;
std::vector<const std::vector<KilnNativeArg>*> g_retired_typed;
struct Captured { uint32_t hash; };
std::vector<Captured> g_captured;
uintptr_t g_setup_registry = 1;
uint32_t engine_argc = 0;
int alloc_slot() { for (int i = 0; i < kSlots; ++i) if (g_slots[i].kind == SLOT_FREE) return i; return -1; }
bool registry_room(uintptr_t) { return true; }
bool register_slot_in(int i, uintptr_t r) { engine_argc = g_slots[i].argc; g_slots[i].registered_in = r; return true; }
void logw(const char*, ...) {}
std::atomic<bool> g_programs_ready{false};
bool g_have_programs = true, g_have_natives = false;
std::atomic<uintptr_t> g_prog_registry{1};
std::unordered_set<uint32_t> g_vanilla_programs{1};
uint32_t g_last_count = 0, registry_count = 1;
uint64_t g_count_stable_since = 0;
std::atomic<uint64_t> g_step{1};
bool kab::program_map(uintptr_t, MapView* m) { m->count = &registry_count; return true; }
bool kab::read_bytes(const void* p, void* out, size_t n) { std::memcpy(out, p, n); return true; }
void selftest_programs(uintptr_t) {}
bool program_present = false, attachment_active = true;
void check_attachments(uintptr_t) { attachment_active = program_present; }
uint32_t refresh_programs() { program_present = true; return 1; }
void logi(const char*, ...) {}
struct ManagedCall { uint64_t id; int slot; uint8_t state, error, value; uint64_t step; bool delivered; };
std::unordered_map<uint64_t, ManagedCall> g_calls;
std::unordered_map<uint64_t, uint64_t> g_call_key;
uint64_t g_call_seq = 0;
std::atomic<uint64_t> c_managed_calls{0}, c_managed_suspends{0};
bool refused_typed(Slot&, NativeResult*, ArgSlice*, NativeCtx*) { return false; }
bool bounded_lock(std::mutex& m) { return m.try_lock(); }
uint32_t self_of(uint32_t) { return 0; }
KilnAbilityEvent new_event(uint32_t kind) { KilnAbilityEvent e{}; e.kind = kind; return e; }
void capture_args(KilnAbilityEvent&, ArgSlice*, NativeCtx*) {}
bool event_room = false;
bool push_event(const KilnAbilityEvent&) { return event_room; }
std::atomic<uint32_t> g_inflight{0};
std::vector<const AttachSet*> g_retired;
#include "registration_under_test.h"
int main() {
    // A thunk has loaded a binding but has not entered its owner's gate yet.
    auto* binding = new SyncBinding{nullptr, nullptr, g_gates.of(77), "test.Native"};
    g_retired_bindings.push_back(binding);
    g_inflight = 1;
    free_retired();
    assert(g_retired_bindings.size() == 1);
    g_inflight = 0;
    free_retired();
    assert(g_retired_bindings.empty());
    std::string why;
    uint32_t hash;
    int i = add_kiln_native(1, "test.Native", 1, SLOT_MANAGED, &hash, why);
    assert(i >= 0 && engine_argc == 1);
    g_slots[i].kind = SLOT_DEAD; // unregister retains the registry's thunk and hash
    int next = add_kiln_native(2, "test.Native", 2, SLOT_MANAGED, &hash, why);
    assert(next == i);
    assert(engine_argc == 2); // VM must pop the same count as the verifier and new handler expect
    // the same argc again: the engine's entry stays, nothing is registered (the map may be full by then)
    g_slots[i].kind = SLOT_DEAD;
    engine_argc = 0;
    assert(add_kiln_native(3, "test.Native", 2, SLOT_MANAGED, &hash, why) == i && engine_argc == 0);
    check_programs_ready();
    assert(attachment_active); // boot attachment must see the cooked program inserted by refresh
    // review P1 item 4: registered before a world (no setup registry) it waits; the world's setup puts it in or not
    {
        const uintptr_t world = g_setup_registry;
        g_setup_registry = 0;
        uint32_t h1 = 0, h2 = 0, st = 99;
        const int a = add_kiln_native(4, "test.EarlyIn", 0, SLOT_MANAGED_SYNC, &h1, why);
        const int b = add_kiln_native(4, "test.EarlyOut", 0, SLOT_MANAGED_SYNC, &h2, why);
        assert(a >= 0 && b >= 0 && g_slots[a].registered_in == 0 && g_slots[b].registered_in == 0);
        assert(t_native_state(h1, &st) == KILN_OK && st == KILN_NATIVE_STATE_WAITING);
        g_setup_registry = world;
        assert(register_slot_in(a, world));  // the setup had room for the first only
        assert(t_native_state(h1, &st) == KILN_OK && st == KILN_NATIVE_STATE_LIVE);
        assert(t_native_state(h2, &st) == KILN_OK && st == KILN_NATIVE_STATE_REFUSED);
        g_slots[a].kind = SLOT_DEAD;
        assert(t_native_state(h1, &st) == KILN_OK && st == KILN_NATIVE_STATE_DEAD);
        assert(t_native_state(0x12345678u, &st) == KILN_OK && st == KILN_NATIVE_STATE_NONE);
        assert(t_native_state(h1, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    }
    NativeResult result{};
    ArgSlice args{};
    NativeCtx ctx{};
    ctx.impact_id = 1;
    ctx.site = 2;
    dispatch_managed(i, &result, &args, &ctx);
    assert(result.error == 1 && g_calls.empty() && g_call_key.empty());
    event_room = true;
    dispatch_managed(i, &result, &args, &ctx);
    assert(g_calls.size() == 1 && g_call_key.size() == 1);
    auto& call = g_calls.begin()->second;
    call.state = 1;
    call.value = 9;
    dispatch_managed(i, &result, &args, &ctx);
    assert(result.error == 0 && result.value == 9 && g_calls.empty());
    puts("registration_test: reload argc, deferred attachment, dropped-call retry and native states passed");
}
