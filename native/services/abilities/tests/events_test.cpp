// SPDX-License-Identifier: MIT
// Production event queue/stop/delivery on the host; no engine or Windows hooks.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "kiln_api.h"
#include "services/kiln_abilities.h"

std::mutex g_mu, g_ev_mu;
std::vector<KilnAbilityEvent> g_events, g_batch;
constexpr size_t kMaxEvents = 4096;
std::atomic<uint64_t> g_step{1}, c_dropped{0}, c_delivered{0};
std::atomic<uint32_t> g_interest{0}, g_inflight{0}, g_ecs_state{0}, g_ecs_reason{0};
std::atomic<uintptr_t> g_system{0}, g_prog_registry{0}, g_native_registry{0};
std::atomic<void*> g_ecs{nullptr};
std::atomic<bool> g_programs_ready{false};
uintptr_t g_cap_registry = 0, g_setup_world = 0, g_setup_registry = 0;
struct Slot { uintptr_t registered_in = 0; };
std::vector<Slot> g_slots;
struct KProgram { uintptr_t inserted_in = 0; };
std::vector<KProgram> g_programs;
std::unordered_set<uint32_t> g_held;
std::unordered_map<uint32_t, uint64_t> g_canceled;
std::unordered_map<uint32_t, int> g_impact_ovr, g_entity_ovr;
struct Tracked { uint32_t program, self; };
std::unordered_map<uint32_t, Tracked> g_tracked;
struct Call { int state = 0, error = 0, value = 0; };
std::unordered_map<uint64_t, Call> g_calls;
std::unordered_map<uint64_t, uint64_t> g_call_key;
struct { int inst_program = 0; } E;
KilnAbilityListenerFn g_listener = nullptr;
void* g_listener_user = nullptr;
std::vector<uint32_t> destroyed;
struct Spawn { void (*destroy)(uint32_t); } spawn{[](uint32_t e) { destroyed.push_back(e); }};
Spawn* g_spawn = &spawn;
constexpr int64_t kRetireWaitNs = 2'000'000;
int64_t now_ns() { return 0; }
void SwitchToThread() { std::this_thread::yield(); }
void logw(const char*, ...) {}
void logi(const char*, ...) {}
void holds_clear() {}
void recount_pending() {}
bool bounded_lock(std::mutex& mu) { mu.lock(); return true; }
uint8_t* instance(uintptr_t, uint32_t) { return nullptr; }
bool read_bytes(const void* p, void* out, size_t n) { std::memcpy(out, p, n); return true; }
uint32_t root_of(uintptr_t, uint32_t e) { return e; }
KilnAbilityEvent new_event(uint32_t kind) { KilnAbilityEvent ev{}; ev.kind = kind; return ev; }
uint32_t seh_listener(KilnAbilityListenerFn fn, KilnAbilityEvent* ev, uint32_t n, void* user, bool*) {
    return fn(ev, n, user);
}
#include "events_under_test.h"

int failed = 0;
#define CHECK(c) do { if (!(c)) { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
int received = 0;
uint32_t KILN_CALL cancel(KilnAbilityEvent* events, uint32_t count, void*) {
    received += count;
    for (uint32_t i = 0; i < count; ++i) events[i].canceled = 1;
    return KILN_ABILITY_INTEREST_HOLD;
}

int main() {
    g_listener = &cancel;  // resident plugin, intentionally retained between worlds
    g_interest = KILN_ABILITY_INTEREST_HOLD;
    KilnAbilityEvent ev{};
    ev.kind = KILN_ABILITY_EV_IMPACT_STARTED;
    ev.impact_id = 27;
    ev.self = 2048;
    ev.held = 1;
    // A worker's final step queues this; no further Pre in A.
    std::thread worker([&] { push_event(ev); });
    worker.join();
    g_batch.push_back(ev);  // previous Pre's scratch must also retire
    KilnWorldEvent world{};
    world.kind = KILN_WORLD_CLIENT;
    world.event = KILN_WORLD_STOPPING;
    on_world(&world, nullptr);
    CHECK(g_events.size() == 1 && g_batch.size() == 1);
    world.kind = KILN_WORLD_SERVER;
    on_world(&world, nullptr);  // engine jobs have drained
    CHECK(g_events.empty() && g_batch.empty());
    CHECK(g_listener == &cancel);
    deliver(0xB);  // B reused entity 2048
    CHECK(received == 0);
    CHECK(destroyed.empty());
    CHECK(g_canceled.empty());
    // Current-world events still deliver and cancel normally.
    push_event(ev);
    deliver(0xB);
    CHECK(received == 1);
    CHECK(destroyed == std::vector<uint32_t>{2048});
    CHECK(g_canceled.count(27) == 1);
    // #108: a server world that starts while the last one's state is here (its stop was not seen) clears it as a stop
    // does; a start after a stop clears nothing (events of the new world stay)
    g_setup_world = 0xB;
    g_prog_registry = 0x300;
    g_programs.push_back(KProgram{0x300});
    push_event(ev);
    world.event = KILN_WORLD_STARTED;
    world.session = 2;
    on_world(&world, nullptr);
    CHECK(g_events.empty() && g_setup_world == 0 && g_prog_registry == 0 && g_programs[0].inserted_in == 0);
    push_event(ev);
    on_world(&world, nullptr);
    CHECK(g_events.size() == 1);
    std::printf("events_test: %d failures\n", failed);
    return failed ? 1 : 0;
}
