// SPDX-License-Identifier: MIT
// Host test: kiln.world's named services (named_services.cpp, named_service_hold.cpp) over the real kiln.engine (booted
// through kiln_plugin_main with a fake core: its write gate, views and guarded copies and writes) and a fake engine laid
// out from each exact profile, with the game's named-service registry in it as the profile's typed serviceLayout
// describes it: the records, the name map, the one system bound to the service, the type's metadata.
// Covered: open (the contract by name, the type from kiln.types, the argument rules, a name the profile does not have,
// the view bound per owner), snapshot (the bytes, a short buffer without a gate), compare-exchange outcomes (written,
// masked, no-op, conflict with the current bytes, the size, a rolled-back write that is proven, an uncertain one and the
// latch for the session, a binding that changes after the write), the gate hold (held for the whole hold, no allocation
// and no call into the core inside it, busy while a frame holds the Scene word, the owner's busy latch for the step, on
// the server thread), a writer past the latch check while another write ends uncertain (nothing written after it), a
// session change (stale), the wrong owner (not found, the view stays its owner's), a retired owner,
// refusals while the world is not live, a binding that does not resolve (each of ShroudForge's checks), the memory-range
// check, a fault, the view bound, and racing writers against a simulated frame under ThreadSanitizer.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "fake_types.h"
#include "gate.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"

#include "../named_services.h"
#include "named_fake_registry.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);
namespace kiln::engine::mem {
extern std::atomic<std::uintptr_t> test_image;
}

thread_local bool g_in_hold = false;
std::atomic<long> g_allocations_in_hold{0};
void* operator new(std::size_t n) {
    if (g_in_hold) g_allocations_in_hold.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {
using namespace kiln::engine;
using namespace kw::named;
using fake::get;
using fake::put;

int checks = 0, failures = 0;
std::string g_case;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s\n", g_case.c_str(), what.c_str());
    }
}

// ---- a fake core for the real kiln-engine entry (as ecs_harness.h) ----
std::atomic<long> g_core_calls_in_hold{0};
void core_call() { if (g_in_hold) g_core_calls_in_hold.fetch_add(1); }
ImageFacts g_facts;
std::map<std::string, const void*> g_tables;
KilnCapsApi g_caps{};
KilnResult KILN_CALL cap_report(KilnOwner, KilnStr, uint32_t, uint32_t, KilnStr) { return KILN_OK; }
const void* KILN_CALL lookup(KilnStr n, uint32_t v) {
    core_call();
    const std::string name(n.ptr, n.len);
    if (name == KILN_CAPS_SERVICE) return &g_caps;
    const auto at = g_tables.find(name);
    if (at == g_tables.end()) return nullptr;
    return static_cast<const KilnServiceHeader*>(at->second)->version >= v ? at->second : nullptr;
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr n, const void* t) {
    core_call();
    g_tables[std::string(n.ptr, n.len)] = t;
    return KILN_OK;
}
void KILN_CALL logger(KilnOwner, int32_t, KilnStr) { core_call(); }
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    core_call();
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
thread_local bool g_on_server_thread = false;
bool KILN_CALL on_world(uint32_t kind) {
    core_call();
    return kind == KILN_WORLD_SERVER && g_on_server_thread;
}
KilnResult KILN_CALL subscribe_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle* out) {
    core_call();
    *out = 0;
    return KILN_OK;
}
KilnResult KILN_CALL subscribe_tick(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* out) {
    core_call();
    *out = 0;
    return KILN_OK;
}
KilnApi make_api() {
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &lookup;
    api.register_service = &publish;
    api.log = &logger;
    api.build_info = &build;
    api.on_world_thread = &on_world;
    api.subscribe_world = &subscribe_world;
    api.subscribe_tick = &subscribe_tick;
    return api;
}
const KilnEngineNativeApi* boot_engine(const Profile& p, KilnApi& api) {
    reset_for_tests();
    g_tables.clear();
    g_facts = {};
    g_facts.headers = true;
    g_facts.target = p.target;
    g_facts.sha256 = p.sha256;
    g_facts.timestamp = p.timestamp;
    g_facts.size_of_image = p.size_of_image;
    g_facts.archive_format = 4;
    g_facts.side = std::string(p.target) == "enshrouded.exe" ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER;
    KilnPluginInfo info{};
    info.size = sizeof info;
    if (kiln_plugin_main(&api, 7, &info) != KILN_OK) return nullptr;
    char why[160]{};
    return kiln_engine_native(&api, 7, sizeof(KilnEngineNativeApi), why, sizeof why);
}

// kiln.internal.engine's copy with failure injection: a read overlapping [g_fail_lo, g_fail_hi) fails as a fault does.
const KilnEngineNativeApi* g_real = nullptr;
std::atomic<uintptr_t> g_fail_lo{0}, g_fail_hi{0};
std::atomic<uint64_t> g_hold_copies{0};
KilnResult KILN_CALL failing_copy(std::uintptr_t a, void* out, std::size_t n) {
    if (g_in_hold) g_hold_copies.fetch_add(1, std::memory_order_relaxed);
    if (n && a < g_fail_hi.load() && a + n > g_fail_lo.load()) return KILN_ERR_UNAVAILABLE;
    return g_real->copy(a, out, n);
}

// Hold points, the in-hold flag and what the gate looked like there.
std::function<void(HoldPoint)> g_point;
std::atomic<bool> g_holder_seen_in_hold{false};
void on_hold_point(HoldPoint p) {
    if (p == HoldPoint::Begin) {
        g_in_hold = true;
        if (gate_holder_taken()) g_holder_seen_in_hold = true;
    }
    if (g_point) {
        const bool was = g_in_hold;
        g_in_hold = false;  // the test's own action is not the hold's
        const auto point = g_point;
        point(p);
        g_in_hold = was;
    }
}
// Called at the end of every gate release (the gate's words and its Kiln holder already free), on the releasing thread.
std::function<void()> g_released;
void on_release(bool begin) {
    if (begin) g_in_hold = false;
    if (!begin && g_released) {
        const auto released = g_released;
        released();
    }
}

// Kiln's own server-world facts, scripted, and the memory-range check.
struct Rig {
    std::atomic<uint64_t> session{41}, step{100};
    std::atomic<bool> readable{true}, writable{true};
    std::atomic<uintptr_t> lo{0}, hi{0};
    std::mutex mu;
    std::vector<std::string> logs;
};
// A thread with t_pause set waits in its next read of the server-world facts (before its gate) until g_pause is 2.
thread_local bool t_pause = false, t_second = false;
std::atomic<int> g_pause{0};  // 1: the paused thread waits; 2: it goes on
bool rig_server(void* u, uint64_t* s) {
    if (t_pause) {
        t_pause = false;
        g_pause = 1;
        while (g_pause.load() != 2) std::this_thread::yield();
    }
    auto& r = *static_cast<Rig*>(u);
    if (!r.readable.load()) return false;
    *s = r.session.load();
    return true;
}
uint64_t rig_step(void* u) { return static_cast<Rig*>(u)->step.load(); }
bool rig_writable(void* u, std::uintptr_t a, std::size_t n) {
    auto& r = *static_cast<Rig*>(u);
    return r.writable.load() && a >= r.lo.load() && a + n <= r.hi.load();
}
void rig_log(void* u, int, const char* text) {
    auto& r = *static_cast<Rig*>(u);
    std::lock_guard lock(r.mu);
    r.logs.emplace_back(text);
}

using Registry = namedfake::Registry;

constexpr KilnOwner kOwner = 21, kOther = 22;
KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }
uint64_t u64_at(uintptr_t a) { return get<uint64_t>(a); }
std::vector<uint8_t> bytes_at(uintptr_t a, size_t n) {
    std::vector<uint8_t> v(n);
    std::memcpy(v.data(), reinterpret_cast<const void*>(a), n);
    return v;
}

struct Call {
    KilnResult rc = KILN_OK;
    KilnMutationReply reply{};
    std::vector<uint8_t> confirmed;
};
Call cx(Services& s, KilnOwner owner, KilnHandle view, const std::vector<uint8_t>& expected,
        const std::vector<uint8_t>& desired, const std::vector<uint8_t>* mask = nullptr) {
    Call c;
    c.reply.size = sizeof c.reply;
    c.confirmed.assign(expected.size(), 0xEE);
    c.rc = s.compare_exchange(owner, view, expected.data(), mask ? mask->data() : nullptr, desired.data(),
                              static_cast<uint32_t>(expected.size()), c.confirmed.data(), &c.reply);
    return c;
}
KilnHandle open_view(Services& s, KilnOwner owner, const ServiceContract& c, KilnResult* rc = nullptr) {
    KilnHandle h = 0;
    const KilnResult r = s.open(owner, KilnStr{c.key.data(), c.key_size}, 0, &h, nullptr);
    if (rc) *rc = r;
    return h;
}

struct Env {
    const Profile& p;
    fake::Engine& e;
    Registry& reg;
    Rig& rig;
    Services& s;
    const ServiceContract& C;
};
std::vector<uint8_t> with_counter(const std::vector<uint8_t>& base, uint64_t value) {
    std::vector<uint8_t> v = base;
    std::memcpy(v.data(), &value, 8);
    return v;
}

void open_cases(Env& v, faketypes::Types& types) {
    const std::string k = std::string(v.p.id) + ": open: ";
    KilnHandle h = 0;
    const KilnType* type = nullptr;
    check(v.s.open(kOwner, str("ecs::GameSettings"), 0, &h, &type) == KILN_OK && h != 0 && type != nullptr &&
              type == faketypes::by_hash(v.C.type_hash),
          k + "the profile's contract opens, with its kiln.types type");
    KilnTypeInfo info{};
    info.size = sizeof info;
    check(type && faketypes::info(type, &info) == KILN_OK && info.type_size == v.C.size, k + "the type is the service's size");
    check(v.s.close(kOwner, h) == KILN_OK && v.s.close(kOwner, h) == KILN_ERR_NOT_FOUND, k + "close, once");
    KilnHandle h2 = 0;
    check(v.s.open(kOwner, str("ecs::GameSettings"), v.rig.session.load(), &h2, nullptr) == KILN_OK && h2 && h2 != h,
          k + "the live session named: a new handle");
    v.s.close(kOwner, h2);
    check(v.s.open(kOwner, str("ecs::GameSettings"), v.rig.session.load() + 1, &h2, nullptr) == KILN_ECS_ERR_STALE && !h2,
          k + "another session named: stale");
    check(v.s.open(kOwner, str("ecs::Other"), 0, &h2, nullptr) == KILN_ERR_NOT_FOUND, k + "a name the profile does not have");
    check(v.s.open(kOwner, str("keen::ecs::GameSettings"), 0, &h2, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          k + "the qualified keen:: form is refused");
    check(v.s.open(kOwner, KilnStr{"", 0}, 0, &h2, nullptr) == KILN_ERR_INVALID_ARGUMENT, k + "an empty name");
    const std::string long_name(128, 'x');
    check(v.s.open(kOwner, KilnStr{long_name.data(), long_name.size()}, 0, &h2, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          k + "a 128-byte name");
    check(v.s.open(kOwner, KilnStr{"ecs::Game\0Settings", 18}, 0, &h2, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          k + "a name with a NUL");
    check(v.s.open(0, str("ecs::GameSettings"), 0, &h2, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
              v.s.open(kOwner, str("ecs::GameSettings"), 0, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          k + "owner 0 or no out handle");
    // kiln.types with another size for the type: the view opens, without a type
    types.by_hash[v.C.type_hash]->size = v.C.size + 8;
    check(v.s.open(kOwner, str("ecs::GameSettings"), 0, &h2, &type) == KILN_OK && type == nullptr,
          k + "a type of another size is not handed out");
    types.by_hash[v.C.type_hash]->size = v.C.size;
    v.s.close(kOwner, h2);
    // the view bound per owner
    std::vector<KilnHandle> all;
    KilnResult rc = KILN_OK;
    for (size_t i = 0; i < Services::kViewsPerOwner; ++i) all.push_back(open_view(v.s, kOwner, v.C, &rc));
    const bool all_open = std::all_of(all.begin(), all.end(), [](KilnHandle x) { return x != 0; });
    KilnHandle extra = open_view(v.s, kOwner, v.C, &rc);
    check(all_open && !extra && rc == KILN_ERR_BUFFER_TOO_SMALL, k + "KILN_NAMED_SERVICE_VIEWS views, then too many");
    const KilnHandle others = open_view(v.s, kOther, v.C, &rc);
    check(others && rc == KILN_OK, k + "another owner still opens");
    v.s.close(kOwner, all.back());
    all.back() = open_view(v.s, kOwner, v.C, &rc);
    check(all.back() && rc == KILN_OK, k + "a close frees one");
    for (KilnHandle x : all) v.s.close(kOwner, x);
    v.s.close(kOther, others);
    check(v.s.views_of(kOwner) == 0 && v.s.views() == 0, k + "the views close");
}

void snapshot_cases(Env& v) {
    const std::string k = std::string(v.p.id) + ": snapshot: ";
    for (size_t i = 0; i < v.C.size; ++i) put<uint8_t>(v.reg.service() + i, static_cast<uint8_t>(i * 7 + 1));
    const KilnHandle h = open_view(v.s, kOwner, v.C);
    std::vector<uint8_t> out(v.C.size + 16, 0xEE);
    uint32_t required = 0;
    g_holder_seen_in_hold = false;
    g_allocations_in_hold = 0;
    g_core_calls_in_hold = 0;
    check(v.s.snapshot(kOwner, h, out.data(), static_cast<uint32_t>(out.size()), &required) == KILN_OK &&
              required == v.C.size && std::equal(out.begin(), out.begin() + v.C.size, bytes_at(v.reg.service(), v.C.size).begin()) &&
              out[v.C.size] == 0xEE,
          k + "the service's bytes, nothing past them");
    check(g_holder_seen_in_hold.load() && !gate_holder_taken(), k + "under the gate, released after");
    check(g_allocations_in_hold.load() == 0 && g_core_calls_in_hold.load() == 0, k + "no allocation or core call in the hold");
    bool held = false;
    g_point = [&](HoldPoint) { held = true; };
    check(v.s.snapshot(kOwner, h, out.data(), v.C.size - 1, &required) == KILN_ERR_BUFFER_TOO_SMALL &&
              required == v.C.size && !held,
          k + "a short buffer: the size, no gate");
    g_point = nullptr;
    check(v.s.snapshot(kOwner, h, nullptr, 4, &required) == KILN_ERR_INVALID_ARGUMENT &&
              v.s.snapshot(0, h, out.data(), v.C.size, &required) == KILN_ERR_INVALID_ARGUMENT,
          k + "invalid arguments");
    v.s.close(kOwner, h);
}

void cas_cases(Env& v) {
    const std::string k = std::string(v.p.id) + ": compare-exchange: ";
    const uintptr_t at = v.reg.service();
    std::memset(reinterpret_cast<void*>(at), 0, v.C.size);
    put<uint64_t>(at, 10);
    const std::vector<uint8_t> base = bytes_at(at, v.C.size);
    const KilnHandle h = open_view(v.s, kOwner, v.C);
    v.rig.step.fetch_add(1);
    g_allocations_in_hold = 0;
    g_core_calls_in_hold = 0;
    g_holder_seen_in_hold = false;
    Call c = cx(v.s, kOwner, h, base, with_counter(base, 11));
    check(c.rc == KILN_OK && c.reply.status == KILN_ECS_OK &&
              c.reply.flags == (KILN_MUTATION_WRITTEN | KILN_MUTATION_CONFIRMED) && u64_at(at) == 11 &&
              c.confirmed == with_counter(base, 11) && c.reply.session == 41 && c.reply.step == v.rig.step.load() &&
              c.reply.generation == 0,
          k + "written and read back");
    check(g_holder_seen_in_hold.load() && g_allocations_in_hold.load() == 0 && g_core_calls_in_hold.load() == 0,
          k + "one gate hold, no allocation or core call inside it");
    // the mask: only the second u64 from desired
    std::vector<uint8_t> mask(v.C.size, 0), desired = with_counter(bytes_at(at, v.C.size), 999);
    std::memcpy(desired.data() + 8, "\x05\0\0\0\0\0\0\0", 8);
    std::fill(mask.begin() + 8, mask.begin() + 16, 1);
    c = cx(v.s, kOwner, h, bytes_at(at, v.C.size), desired, &mask);
    check(c.rc == KILN_OK && u64_at(at) == 11 && u64_at(at + 8) == 5, k + "masked: only the selected bytes");
    const std::vector<uint8_t> now = bytes_at(at, v.C.size);
    c = cx(v.s, kOwner, h, now, now);
    check(c.rc == KILN_OK && c.reply.flags == (KILN_MUTATION_NO_OP | KILN_MUTATION_CONFIRMED) && bytes_at(at, v.C.size) == now,
          k + "nothing to change");
    c = cx(v.s, kOwner, h, with_counter(now, 77), with_counter(now, 78));
    check(c.rc == KILN_ECS_ERR_CONFLICT && c.reply.status == KILN_ECS_CONFLICT &&
              c.reply.why_code == KILN_ECS_WRITE_WHY_COMPARE && c.confirmed == now && bytes_at(at, v.C.size) == now,
          k + "conflict: the current bytes, nothing written");
    std::vector<uint8_t> small(v.C.size - 1, 0);
    c = cx(v.s, kOwner, h, small, small);
    check(c.rc == KILN_ECS_ERR_NOT_WRITABLE && c.reply.why_code == KILN_ECS_WRITE_WHY_SIZE, k + "not the service's size");
    KilnMutationReply r{};
    r.size = offsetof(KilnMutationReply, restore_ns) - 1;
    check(v.s.compare_exchange(kOwner, h, now.data(), nullptr, now.data(), v.C.size, nullptr, &r) ==
              KILN_ERR_INVALID_ARGUMENT &&
              v.s.compare_exchange(kOwner, h, nullptr, nullptr, now.data(), v.C.size, nullptr, nullptr) ==
                  KILN_ERR_INVALID_ARGUMENT,
          k + "invalid arguments");
    // a caller built before restore_ns passes 48 bytes: answered as a full-size caller is, nothing written past them,
    // the size given back (GitHub #58's reply rule, as ecs_compare_exchange)
    {
        const Call full = cx(v.s, kOwner, h, now, now);
        alignas(KilnMutationReply) unsigned char buf[sizeof(KilnMutationReply)];
        std::memset(buf, 0xAB, sizeof buf);
        auto* const old = reinterpret_cast<KilnMutationReply*>(buf);
        old->size = offsetof(KilnMutationReply, restore_ns);
        std::vector<uint8_t> conf(v.C.size, 0xEE);
        const KilnResult rc =
            v.s.compare_exchange(kOwner, h, now.data(), nullptr, now.data(), v.C.size, conf.data(), old);
        check(rc == full.rc && old->size == offsetof(KilnMutationReply, restore_ns) && old->status == full.reply.status &&
                  old->flags == full.reply.flags && full.reply.size == sizeof(KilnMutationReply) &&
                  std::all_of(buf + offsetof(KilnMutationReply, restore_ns), buf + sizeof buf,
                              [](unsigned char b) { return b == 0xAB; }),
              k + "a 48-byte reply: the same outcome, nothing past its 48 bytes, its size given back");
    }
    // a rogue write between the write and the readback: rolled back in the same hold, and the rollback proven
    g_point = [&](HoldPoint p) {
        if (p == HoldPoint::AfterWrite) put<uint64_t>(at, 4242);
    };
    c = cx(v.s, kOwner, h, now, with_counter(now, 12));
    g_point = nullptr;
    check(c.rc == KILN_ECS_ERR_UNREADABLE && c.reply.flags == KILN_MUTATION_ROLLED_BACK &&
              c.reply.why_code == KILN_ECS_WRITE_WHY_READBACK && bytes_at(at, v.C.size) == now,
          k + "a write that does not read back is rolled back and proven");
    // the binding changes after the write: uncertain, latched for the session
    g_point = [&](HoldPoint p) {
        if (p == HoldPoint::AfterWrite) put<uint64_t>(v.reg.system(1) + v.p.layout.services.normal_bindings + 8, 0);
    };
    c = cx(v.s, kOwner, h, now, with_counter(now, 13));
    g_point = nullptr;
    v.reg.wire();
    check(c.rc == KILN_ECS_ERR_UNCERTAIN && (c.reply.flags & KILN_MUTATION_TARGET_LATCHED) &&
              c.reply.why_code == KILN_ECS_WRITE_WHY_READBACK && u64_at(at) == 13,
          k + "the binding changed after the write: uncertain");
    bool held = false;
    g_point = [&](HoldPoint) { held = true; };
    const std::vector<uint8_t> after = bytes_at(at, v.C.size);
    c = cx(v.s, kOwner, h, after, with_counter(after, 14));
    g_point = nullptr;
    check(c.rc == KILN_ECS_ERR_UNCERTAIN && c.reply.why_code == KILN_ECS_WRITE_WHY_TARGET_LATCHED &&
              (c.reply.flags & KILN_MUTATION_TARGET_LATCHED) && !held && u64_at(at) == 13,
          k + "latched for the session: no gate, nothing written");
    std::vector<uint8_t> out(v.C.size);
    check(v.s.snapshot(kOwner, h, out.data(), v.C.size, nullptr) == KILN_OK && out == after, k + "snapshots go on");
    // a rogue write after the write and after the rollback: uncertain (a new session: the latch is the old one's)
    v.rig.session = 42;
    const KilnHandle h2 = open_view(v.s, kOwner, v.C);
    g_point = [&](HoldPoint p) {
        if (p == HoldPoint::AfterWrite || p == HoldPoint::AfterRollback) put<uint64_t>(at, 9999);
    };
    c = cx(v.s, kOwner, h2, after, with_counter(after, 15));
    g_point = nullptr;
    check(c.rc == KILN_ECS_ERR_UNCERTAIN && c.reply.why_code == KILN_ECS_WRITE_WHY_READBACK && c.reply.session == 42,
          k + "neither the write nor the rollback reads back: uncertain");
    bool warned = false;
    {
        std::lock_guard lock(v.rig.mu);
        for (const auto& line : v.rig.logs) warned = warned || line.find("latched uncertain") != std::string::npos;
    }
    check(warned, k + "an uncertain write is logged after the release");
    v.rig.session = 43;
    const KilnHandle h3 = open_view(v.s, kOwner, v.C);
    put<uint64_t>(at, 1);
    const std::vector<uint8_t> fresh = bytes_at(at, v.C.size);
    c = cx(v.s, kOwner, h3, fresh, with_counter(fresh, 2));
    check(c.rc == KILN_OK && u64_at(at) == 2, k + "the next session writes again");
    v.s.close(kOwner, h);
    v.s.close(kOwner, h2);
    v.s.close(kOwner, h3);
}

// Two writers of one service: the second checked the latch before the first's hold, which ends uncertain, and takes the
// gate after that hold's release but before the first call is back. It writes nothing: the first hold latched the
// service before it released the gate (the rollback was read back, the binding then did not resolve).
void uncertain_race(Env& v) {
    const std::string k = std::string(v.p.id) + ": an uncertain write and a writer past the latch check: ";
    const uintptr_t at = v.reg.service();
    v.rig.session = 44;
    v.rig.step.fetch_add(1);
    const KilnHandle first = open_view(v.s, kOwner, v.C), second = open_view(v.s, kOther, v.C);
    put<uint64_t>(at, 100);
    const std::vector<uint8_t> base = bytes_at(at, v.C.size);
    size_t warned_before = 0;
    {
        std::lock_guard lock(v.rig.mu);
        warned_before = v.rig.logs.size();
    }
    g_pause = 0;
    g_point = [&](HoldPoint p) {
        if (t_second) return;
        if (p == HoldPoint::AfterWrite) put<uint64_t>(at, 4242);  // the readback fails: rolled back
        if (p == HoldPoint::AfterRollback)                       // the rollback reads back, the binding is gone
            put<uint64_t>(v.reg.system(1) + v.p.layout.services.normal_bindings + 8, 0);
    };
    g_released = [&] {
        if (t_second || g_pause.load() != 1) return;
        v.reg.wire();  // the binding resolves again for the second writer
        g_pause = 2;
    };
    Call late;
    std::thread writer([&] {
        t_second = true;
        t_pause = true;
        late = cx(v.s, kOther, second, base, with_counter(base, 300));
    });
    while (g_pause.load() != 1) std::this_thread::yield();
    const Call c = cx(v.s, kOwner, first, base, with_counter(base, 200));
    writer.join();
    g_point = nullptr;
    g_released = nullptr;
    v.reg.wire();
    check(c.rc == KILN_ECS_ERR_UNCERTAIN && c.reply.why_code == KILN_ECS_WRITE_WHY_READBACK &&
              (c.reply.flags & KILN_MUTATION_TARGET_LATCHED),
          k + "the first ends uncertain");
    check(late.rc == KILN_ECS_ERR_UNCERTAIN && late.reply.why_code == KILN_ECS_WRITE_WHY_TARGET_LATCHED &&
              (late.reply.flags & KILN_MUTATION_TARGET_LATCHED) && !(late.reply.flags & KILN_MUTATION_WRITTEN) &&
              bytes_at(at, v.C.size) == base,
          k + "the second writes nothing (rc " + std::to_string(late.rc) + ", why " +
              std::to_string(late.reply.why_code) + ", value " + std::to_string(u64_at(at)) + ")");
    size_t warned = 0;
    {
        std::lock_guard lock(v.rig.mu);
        for (size_t i = warned_before; i < v.rig.logs.size(); ++i)
            warned += v.rig.logs[i].find("latched uncertain") != std::string::npos;
    }
    check(warned == 1, k + "one warning, for the write that was not proven");
    v.s.close(kOwner, first);
    v.s.close(kOther, second);
    v.rig.session = 45;  // a new session: the cases below write again
}

void gate_cases(Env& v) {
    const std::string k = std::string(v.p.id) + ": gate: ";
    const uintptr_t at = v.reg.service();
    const KilnHandle h = open_view(v.s, kOwner, v.C);
    const KilnHandle o = open_view(v.s, kOther, v.C);
    v.rig.step.fetch_add(1);
    std::vector<uint8_t> now = bytes_at(at, v.C.size);
    *fake::word(v.e.scene_word()) = 1;  // a frame: the engine holds the Scene word Shared
    Call c = cx(v.s, kOwner, h, now, with_counter(now, 50));
    std::vector<uint8_t> out(v.C.size);
    const KilnResult snap = v.s.snapshot(kOther, o, out.data(), v.C.size, nullptr);
    *fake::word(v.e.scene_word()) = 0;
    check(c.rc == KILN_ECS_ERR_BUSY && c.reply.why_code == KILN_ECS_WRITE_WHY_GATE &&
              c.reply.engine_reason == static_cast<uint32_t>(Reason::EcsSceneFrameBusy) && bytes_at(at, v.C.size) == now &&
              snap == KILN_ECS_ERR_BUSY,
          k + "a frame holds the Scene word: busy, nothing written");
    c = cx(v.s, kOwner, h, now, with_counter(now, 50));
    check(c.rc == KILN_ECS_ERR_BUSY && c.reply.engine_reason == static_cast<uint32_t>(Reason::KilnBusyLatched),
          k + "the same owner in the same step is latched");
    v.rig.step.fetch_add(1);
    c = cx(v.s, kOwner, h, now, with_counter(now, 50));
    check(c.rc == KILN_OK && u64_at(at) == 50, k + "a later step clears it");
    g_on_server_thread = true;
    now = bytes_at(at, v.C.size);
    c = cx(v.s, kOwner, h, now, with_counter(now, 51));
    g_on_server_thread = false;
    check(c.rc == KILN_ECS_ERR_BUSY && c.reply.why_code == KILN_ECS_WRITE_WHY_IN_FRAME && u64_at(at) == 50,
          k + "on the server thread: in frame");
    // one Kiln holder at a time: a snapshot inside another call's hold
    KilnResult inner = KILN_OK;
    g_point = [&](HoldPoint p) {
        if (p != HoldPoint::AfterResolve) return;
        g_point = nullptr;
        inner = v.s.snapshot(kOther, o, out.data(), v.C.size, nullptr);
    };
    c = cx(v.s, kOwner, h, now, with_counter(now, 52));
    g_point = nullptr;
    check(inner == KILN_ECS_ERR_BUSY && c.rc == KILN_OK && u64_at(at) == 52, k + "one Kiln holder at a time");
    v.s.close(kOwner, h);
    v.s.close(kOther, o);
}

void owner_and_session_cases(Env& v) {
    const std::string k = std::string(v.p.id) + ": owners and sessions: ";
    const uintptr_t at = v.reg.service();
    v.rig.step.fetch_add(1);
    const KilnHandle h = open_view(v.s, kOwner, v.C);
    std::vector<uint8_t> now = bytes_at(at, v.C.size), out(v.C.size);
    // another owner's view: not found, and it stays its owner's
    Call c = cx(v.s, kOther, h, now, with_counter(now, 60));
    check(c.rc == KILN_ERR_NOT_FOUND && c.reply.status == KILN_ECS_ABSENT &&
              c.reply.why_code == KILN_ECS_WRITE_WHY_NOT_OWNERS_REF && bytes_at(at, v.C.size) == now,
          k + "another owner's view cannot write");
    check(v.s.snapshot(kOther, h, out.data(), v.C.size, nullptr) == KILN_ERR_NOT_FOUND &&
              v.s.close(kOther, h) == KILN_ERR_NOT_FOUND && v.s.snapshot(kOwner, h, out.data(), v.C.size, nullptr) == KILN_OK,
          k + "nor read or close it; it stays its owner's");
    c = cx(v.s, kOwner, 0xdead, now, now);
    check(c.rc == KILN_ERR_NOT_FOUND && c.reply.why_code == KILN_ECS_WRITE_WHY_REF, k + "a handle nobody opened");
    // a session change: the view is stale, a new one works
    v.rig.session = 50;
    c = cx(v.s, kOwner, h, now, with_counter(now, 61));
    check(c.rc == KILN_ECS_ERR_STALE && c.reply.why_code == KILN_ECS_WRITE_WHY_REF && bytes_at(at, v.C.size) == now &&
              v.s.snapshot(kOwner, h, out.data(), v.C.size, nullptr) == KILN_ECS_ERR_STALE,
          k + "a view of an ended session is stale");
    const KilnHandle h2 = open_view(v.s, kOwner, v.C);
    c = cx(v.s, kOwner, h2, now, with_counter(now, 61));
    check(h2 && c.rc == KILN_OK && u64_at(at) == 61, k + "a view of the new session writes");
    // the session moves during the hold: the snapshot is not of this session
    now = bytes_at(at, v.C.size);
    g_point = [&](HoldPoint p) {
        if (p == HoldPoint::AfterResolve) v.rig.session = 51;
    };
    const KilnResult moved = v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr);
    g_point = nullptr;
    check(moved == KILN_ECS_ERR_UNREADABLE, k + "a session that moved across the hold: unreadable");
    v.rig.session = 50;
    // a world the view was not opened in (same session clock, another Scene): stale
    v.e.set_entry(0, v.p.layout.entry_ready_state, v.e.context.at(), {0x3333, 0x4444});
    const KilnResult other_world = v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr);
    v.e.set_entry(0, v.p.layout.entry_ready_state, v.e.context.at(), {0x1111, 0x2222});
    check(other_world == KILN_ECS_ERR_STALE && v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr) == KILN_OK,
          k + "a view of another Scene's world is stale");
    // the facts cannot be read now: unreadable
    v.rig.readable = false;
    check(v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr) == KILN_ECS_ERR_UNREADABLE, k + "unreadable world facts");
    v.rig.readable = true;
    // no server world: unavailable
    v.rig.session = 0;
    KilnHandle none = 0;
    check(v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr) == KILN_ERR_UNAVAILABLE &&
              v.s.open(kOwner, str("ecs::GameSettings"), 0, &none, nullptr) == KILN_ERR_UNAVAILABLE && !none,
          k + "no server world: unavailable");
    c = cx(v.s, kOwner, h2, now, with_counter(now, 62));
    check(c.rc == KILN_ERR_UNAVAILABLE && c.reply.why_code == KILN_ECS_WRITE_WHY_NO_WORLD && u64_at(at) == 61,
          k + "no server world: nothing written");
    v.rig.session = 50;
    // the owner retires: its views end and its calls answer stale
    v.s.owner_retired(kOwner);
    c = cx(v.s, kOwner, h2, now, with_counter(now, 63));
    check(c.rc == KILN_ECS_ERR_STALE && c.reply.why_code == KILN_ECS_WRITE_WHY_OWNER_RETIRED && u64_at(at) == 61 &&
              v.s.views_of(kOwner) == 0 && v.s.snapshot(kOwner, h2, out.data(), v.C.size, nullptr) == KILN_ECS_ERR_STALE &&
              v.s.open(kOwner, str("ecs::GameSettings"), 0, &none, nullptr) == KILN_ECS_ERR_STALE,
          k + "a retired owner: views gone, every call stale");
    // an owner retires during a call (it may be the caller): nothing is written
    const KilnOwner third = 23;
    const KilnHandle t = open_view(v.s, third, v.C);
    now = bytes_at(at, v.C.size);
    g_point = [&](HoldPoint p) {
        if (p == HoldPoint::AfterCompare) v.s.owner_retired(99);
    };
    c = cx(v.s, third, t, now, with_counter(now, 64));
    g_point = nullptr;
    check(c.rc == KILN_ECS_ERR_BUSY && c.reply.why_code == KILN_ECS_WRITE_WHY_RETIRE_RACE && u64_at(at) == 61,
          k + "an owner retiring during the call: busy, nothing written");
    v.s.close(third, t);
}

void binding_cases(Env& v) {
    const std::string k = std::string(v.p.id) + ": binding: ";
    const auto& S = v.p.layout.services;
    const KilnOwner owner = 31;
    v.rig.step.fetch_add(1);
    const KilnHandle h = open_view(v.s, owner, v.C);
    std::vector<uint8_t> out(v.C.size);
    auto refused = [&](const std::string& what, const std::function<void()>& damage) {
        damage();
        const KilnResult rc = v.s.snapshot(owner, h, out.data(), v.C.size, nullptr);
        KilnHandle fresh = 0;
        const KilnResult orc = v.s.open(owner, str("ecs::GameSettings"), 0, &fresh, nullptr);
        v.reg.wire();
        check(rc == KILN_ECS_ERR_UNREADABLE && orc == KILN_ECS_ERR_UNREADABLE && !fresh, k + what);
        check(v.s.snapshot(owner, h, out.data(), v.C.size, nullptr) == KILN_OK, k + what + ": repaired, it reads again");
    };
    refused("the type's registry slot names another metadata", [&] { put<uint64_t>(v.e.image.at(v.C.metadata_slot), 0x1000); });
    refused("the type's size differs", [&] { put<uint32_t>(v.e.image.at(v.C.metadata) + 0x40, v.C.size + 4); });
    refused("the type's hash differs", [&] { put<uint32_t>(v.e.image.at(v.C.metadata) + 0x50, 1); });
    refused("no record list", [&] { put<uint64_t>(v.reg.registry + S.records, 0); });
    refused("more records than the profile allows", [&] { put<uint64_t>(v.reg.registry + S.record_count, S.maximum_records + 1); });
    refused("a map capacity that is not a power of two", [&] { put<uint64_t>(v.reg.registry + S.map_capacity, 12); });
    refused("the name's bit is not set", [&] { std::memset(v.reg.bits.base, 0, 8); });
    refused("the record's name is another", [&] { std::memcpy(v.reg.names.base, "ecs::GameSettingz", 17); });
    refused("the record's hash differs", [&] { put<uint32_t>(v.reg.record(1) + S.record_hash, 7); });
    refused("the record's two pointers differ", [&] { put<uint64_t>(v.reg.record(1) + S.alternate, v.reg.payload.at(64)); });
    refused("a second record of the same name", [&] { v.reg.set_record(0, v.reg.names.at(0), v.C.key_size, 5, v.reg.payload.at(2048)); });
    refused("no system writes it", [&] { put<uint64_t>(v.reg.system(1), 0); });
    refused("two systems bound", [&] {
        put<uint16_t>(v.reg.registry + S.system_count, 3);
        put<uint64_t>(v.reg.system(2), v.e.image.at(v.C.descriptor));
    });
    refused("the system's function is another", [&] { put<uint64_t>(v.reg.system(1) + 0x28, v.e.image.at(v.C.function + 16)); });
    refused("the service is also among the system's reads", [&] {
        put<uint64_t>(v.reg.reads.at(0), v.reg.names.at(0));
        put<uint64_t>(v.reg.reads.at(8), v.C.key_size);
    });
    refused("the binding table points elsewhere", [&] { put<uint64_t>(v.reg.system(1) + S.normal_bindings + 8, v.reg.payload.at(8)); });
    refused("the binding count differs", [&] { put<uint8_t>(v.reg.system(1) + S.normal_count, 3); });
    // the memory-range check and a fault
    v.rig.writable = false;
    KilnResult rc = v.s.snapshot(owner, h, out.data(), v.C.size, nullptr);
    v.rig.writable = true;
    check(rc == KILN_ECS_ERR_UNREADABLE, k + "not committed private read-write memory: unreadable");
    g_fail_lo = v.reg.service();
    g_fail_hi = v.reg.service() + 1;
    rc = v.s.snapshot(owner, h, out.data(), v.C.size, nullptr);
    std::vector<uint8_t> now = bytes_at(v.reg.service(), v.C.size);
    Call c = cx(v.s, owner, h, now, with_counter(now, 70));
    g_fail_lo = g_fail_hi = 0;
    check(rc == KILN_ECS_ERR_UNREADABLE && c.rc == KILN_ECS_ERR_UNREADABLE && c.reply.why_code == KILN_ECS_WRITE_WHY_FAULT &&
              bytes_at(v.reg.service(), v.C.size) == now,
          k + "a fault reading the service: unreadable, nothing written");
    v.s.close(owner, h);
}

void unavailable_cases(const Profile& p, const KilnEngineNativeApi* hooked, Rig& rig) {
    const std::string k = std::string(p.id) + ": unavailable: ";
    Source none;
    Services off(none);
    KilnHandle h = 0;
    check(off.unavailable_reason() && off.open(kOwner, str("ecs::GameSettings"), 0, &h, nullptr) == KILN_ERR_UNAVAILABLE,
          k + "no engine: open unavailable");
    // a profile without a serviceLayout
    static Layout layout{};
    layout = p.layout;
    layout.services = ServiceLayout{};
    static const Layout* served = nullptr;
    served = &layout;
    KilnEngineNativeApi plain = *hooked;
    plain.layout = []() -> const Layout* { return served; };
    Source s;
    s.engine = &plain;
    s.user = &rig;
    s.server = &rig_server;
    s.writable = &rig_writable;
    Services no_layout(s);
    check(no_layout.unavailable_reason() != nullptr &&
              std::string(no_layout.unavailable_reason()).find("names no named service") != std::string::npos &&
              no_layout.open(kOwner, str("ecs::GameSettings"), 0, &h, nullptr) == KILN_ERR_UNAVAILABLE,
          k + "a profile without named services");
    s.engine = hooked;
    s.writable = nullptr;
    Services no_check(s);
    check(no_check.unavailable_reason() != nullptr, k + "no memory-range check: off (fails closed)");
}

void race(Env& v) {
    const std::string k = std::string(v.p.id) + ": race: ";
    const uintptr_t at = v.reg.service();
    put<uint64_t>(at, 0);
    // the views first, while no frame runs (an open takes the gate once)
    constexpr int kThreads = 4, kEach = 150;
    v.rig.step.fetch_add(1);
    KilnHandle views[kThreads + 1] = {};
    for (int t = 0; t <= kThreads; ++t) views[t] = open_view(v.s, 300 + t, v.C);
    check(std::all_of(std::begin(views), std::end(views), [](KilnHandle x) { return x != 0; }), k + "the views open");
    std::atomic<bool> stop{false};
    std::atomic<long> frames{0}, torn{0};
    std::thread frame([&] {
        auto* word = fake::word(v.e.scene_word());
        while (!stop.load()) {
            std::int32_t free = 0;
            if (!__atomic_compare_exchange_n(word, &free, 1, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) continue;
            uint64_t x = 0;
            std::memcpy(&x, reinterpret_cast<const void*>(at), 8);  // the game's own plain read inside its frame
            if (x > 1000000) ++torn;
            ++frames;
            __atomic_fetch_sub(word, 1, __ATOMIC_SEQ_CST);
        }
    });
    std::thread tick([&] {
        while (!stop.load()) {
            v.rig.step.fetch_add(1);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });
    std::atomic<long> snaps{0}, snap_bad{0};
    std::thread reader([&] {
        const KilnOwner owner = 300 + kThreads;
        const KilnHandle h = views[kThreads];
        std::vector<uint8_t> out(v.C.size);
        uint64_t last = 0;
        while (!stop.load()) {
            const KilnResult rc = v.s.snapshot(owner, h, out.data(), v.C.size, nullptr);
            if (rc == KILN_OK) {
                uint64_t x = 0;
                std::memcpy(&x, out.data(), 8);
                if (x < last) ++snap_bad;  // the counter only grows
                last = x;
                ++snaps;
            } else if (rc != KILN_ECS_ERR_BUSY) {
                ++snap_bad;
            }
        }
        v.s.close(owner, h);
    });
    std::atomic<long> oks{0}, conflicts{0}, busy{0}, bad{0};
    std::vector<std::thread> writers;
    for (int t = 0; t < kThreads; ++t) {
        writers.emplace_back([&, t] {
            const KilnOwner owner = 300 + t;
            const KilnHandle h = views[t];
            std::vector<uint8_t> mask(v.C.size, 0);
            std::fill(mask.begin(), mask.begin() + 8, 1);
            std::vector<uint8_t> guess(v.C.size, 0);
            int done = 0;
            for (int attempts = 0; h && done < kEach && attempts < 200000; ++attempts) {
                uint64_t g = 0;
                std::memcpy(&g, guess.data(), 8);
                Call c = cx(v.s, owner, h, guess, with_counter(guess, g + 1), &mask);
                if (c.rc == KILN_OK) {
                    ++oks;
                    ++done;
                    guess = with_counter(guess, g + 1);
                    continue;
                }
                if (c.rc == KILN_ECS_ERR_CONFLICT) {
                    ++conflicts;
                    guess = c.confirmed;
                    continue;
                }
                if (c.rc == KILN_ECS_ERR_BUSY) {
                    ++busy;
                    continue;
                }
                ++bad;
            }
            v.s.close(owner, h);
        });
    }
    for (auto& w : writers) w.join();
    stop = true;
    frame.join();
    tick.join();
    reader.join();
    std::printf("  [%s] race: %ld ok, %ld conflicts, %ld busy, %ld snapshots, %ld frames\n", v.p.id, oks.load(),
                conflicts.load(), busy.load(), snaps.load(), frames.load());
    check(bad.load() == 0 && snap_bad.load() == 0 && torn.load() == 0 && oks.load() == kThreads * kEach &&
              u64_at(at) == static_cast<uint64_t>(kThreads * kEach),
          k + "every increment exactly once, the snapshots monotonic, nothing torn");
}

void exact(const Profile& p) {
    g_case = p.id;
    check(p.layout.services.enabled && p.layout.services.contract_count == 1, "the profile types its serviceLayout");
    KilnApi api = make_api();
    const KilnEngineNativeApi* native = boot_engine(p, api);
    check(native != nullptr, "the real kiln.internal.engine table");
    if (!native) return;
    fake::Engine e(p.layout);
    kiln::engine::mem::test_image = e.image.at();
    Registry reg(p.layout, e);
    Rig rig;
    rig.lo = reg.payload.at();
    rig.hi = reg.payload.at() + reg.payload.size;
    faketypes::Types types;
    types.add("keen::ecs::GameSettings", KILN_TYPE_STRUCT, p.layout.services.contracts[0].size);
    types.link();
    g_real = native;
    KilnEngineNativeApi hooked = *native;
    hooked.copy = &failing_copy;
    Source s;
    s.engine = &hooked;
    s.types = faketypes::api(types);
    s.user = &rig;
    s.server = &rig_server;
    s.server_step = &rig_step;
    s.writable = &rig_writable;
    s.log = &rig_log;
    Services services(s);
    check(services.available(), std::string(p.id) + ": named services are available on the fake engine");
    Env v{p, e, reg, rig, services, p.layout.services.contracts[0]};
    open_cases(v, types);
    snapshot_cases(v);
    cas_cases(v);
    uncertain_race(v);
    gate_cases(v);
    owner_and_session_cases(v);
    binding_cases(v);
    unavailable_cases(p, &hooked, rig);
    race(v);
    kiln::engine::mem::test_image = 0;
    kiln::engine::gate_reset_for_tests();
}

}  // namespace

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return g_facts; }
}  // namespace kiln::engine

int main() {
    g_caps.header = {sizeof g_caps, KILN_CAPS_VERSION};
    g_caps.report = &cap_report;
    kw::named::g_hold_test_point = &on_hold_point;
    kiln::engine::g_gate_release_point = &on_release;
    for (const auto& p : generated::kProfiles) exact(p);
    std::printf("named_services: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
