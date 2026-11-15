// SPDX-License-Identifier: MIT
// Host tests of natives_core.h: the decisions kiln.abilities' native thunks make (since 0.2.0-dev.29).
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <string>
#include <vector>

#include "natives_core.h"
#include "natives_json.h"
#include "verify.h"

using namespace kab;

static int g_failed = 0, g_passed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) {                                                              \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static void names() {
    CHECK(valid_native_name("mymod.OnHit"));
    CHECK(valid_native_name("my.mod.OnHit"));
    CHECK(valid_native_name("a.b"));
    CHECK(valid_native_name("my-mod.On_Hit2"));
    CHECK(!valid_native_name("OnHit"));
    CHECK(!valid_native_name(".OnHit"));
    CHECK(!valid_native_name("mymod."));
    CHECK(!valid_native_name("mymod.On-Hit"));
    CHECK(!valid_native_name("mymod.On.Hit"));
    char longname[100];
    std::memset(longname, 'a', sizeof longname);
    longname[3] = '.';
    CHECK(!valid_native_name(std::string_view(longname, 91)));
    CHECK(valid_native_name(std::string_view(longname, 90)));
}

static void room() {
    // measured in the lab (2026-11-15, server 26430253): the game's RegisterNative takes no new key once the map holds
    // half its capacity (128 of 256; 107 are the game's own), so Kiln refuses first, with the reason
    CHECK(registry_has_room(107, 256));
    CHECK(registry_has_room(127, 256));
    CHECK(!registry_has_room(128, 256));
    CHECK(!registry_has_room(254, 256));
    CHECK(!registry_has_room(256, 256));
    CHECK(!registry_has_room(0, 0));  // unreadable map
    CHECK(!registry_has_room(0xFFFFFFFFu, 256));
}

static void results() {
    KilnNativeCall c;
    std::memset(&c, 0, sizeof c);
    uint8_t e = 9, v = 9;
    c.out_error = kUndecided;
    c.out_value = 7;
    sync_result(c, &e, &v);
    CHECK(e == 0 && v == 0);  // fn set nothing: error 0, value 0
    c.out_error = 0;
    c.out_value = 1;
    sync_result(c, &e, &v);
    CHECK(e == 0 && v == 1);
    c.out_error = kErrInternal;
    c.out_value = 0;
    sync_result(c, &e, &v);
    CHECK(e == kErrInternal);
}

static void faults() {
    // a wrapped native: run the game's function once; never twice
    CHECK(fault_answer(ThunkKind::Wrapped, false, false, true) == FaultAnswer::RunOriginal);
    CHECK(fault_answer(ThunkKind::Wrapped, false, true, true) == FaultAnswer::KeepResult);
    CHECK(fault_answer(ThunkKind::Wrapped, false, true, false) == FaultAnswer::KeepResult);
    CHECK(fault_answer(ThunkKind::Wrapped, false, false, false) == FaultAnswer::Unknown);
    // a fault inside the game's own native is never retried
    CHECK(fault_answer(ThunkKind::Wrapped, true, true, true) == FaultAnswer::Unknown);
    // a sync native: Internal (0x11), whatever ran
    CHECK(fault_answer(ThunkKind::Sync, false, false, false) == FaultAnswer::Internal);
    CHECK(fault_answer(ThunkKind::Sync, false, true, false) == FaultAnswer::Internal);
    CHECK(fault_answer(ThunkKind::Other, false, false, true) == FaultAnswer::Unknown);
}

static void budget() {
    const int64_t f = 10'000'000;  // 10 MHz, QueryPerformanceFrequency's usual value
    CHECK(!over_budget(2000, f));   // 0.2 ms exactly: within
    CHECK(over_budget(2001, f));
    CHECK(!over_budget(100, f));
    CHECK(!over_budget(1'000'000, 0));  // no frequency: never
}

static Attachment att(uint64_t id, int32_t stage, int32_t priority, uint32_t program = 0, uint32_t site = 0, uint32_t hash = 7) {
    Attachment a;
    a.id = id;
    a.order = id;
    a.hash = hash;
    a.stage = stage;
    a.priority = priority;
    a.program = program;
    a.site = site;
    return a;
}

static void stages() {
    // registration order: After, Instead, Before 5, Before 10, Before 10 (tie), an other native's Before
    std::vector<Attachment> all = {att(1, KILN_NATIVE_AFTER, 0),  att(2, KILN_NATIVE_INSTEAD, 0), att(3, KILN_NATIVE_BEFORE, 5),
                                   att(4, KILN_NATIVE_BEFORE, 10), att(5, KILN_NATIVE_BEFORE, 10), att(6, KILN_NATIVE_BEFORE, 1, 0, 0, 8),
                                   att(7, KILN_NATIVE_AFTER, -3)};
    const AttachSet set = build_set(all, 7);
    CHECK(set.before.size() == 3 && set.instead.size() == 1 && set.after.size() == 2);
    std::vector<uint64_t> order;
    bool answered = false, ran = false;
    uint64_t entered = 0, skipped = 0;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); if (a.stage == KILN_NATIVE_INSTEAD) answered = true; },
               [&] { ran = true; }, [&] { return answered || ran; }, &entered, &skipped);
    // Before high to low (ties by registration), the Instead, After low to high; the Instead answered: no original
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 2, 7, 1}));
    CHECK(!ran && entered == 6 && skipped == 0);

    // a Before that answers: no Instead, no original, the After still runs
    order.clear();
    answered = ran = false;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); if (a.id == 5) answered = true; },
               [&] { ran = true; }, [&] { return answered || ran; }, &entered, &skipped);
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 7, 1}) && !ran);

    // nothing answers: the original runs once, between the stages
    order.clear();
    answered = ran = false;
    int originals = 0;
    run_stages(set, 0x11, 0x22, [&](const Attachment& a, int32_t) { order.push_back(a.id); },
               [&] { ran = true; ++originals; order.push_back(99); }, [&] { return answered || ran; }, &entered, &skipped);
    CHECK((order == std::vector<uint64_t>{4, 5, 3, 2, 99, 7, 1}) && originals == 1);
}

static void filters() {
    std::vector<Attachment> all = {att(1, KILN_NATIVE_BEFORE, 0, 0xAAAA, 0x1), att(2, KILN_NATIVE_BEFORE, 0, 0xAAAA),
                                   att(3, KILN_NATIVE_BEFORE, 0, 0, 0x2), att(4, KILN_NATIVE_INSTEAD, 0, 0xBBBB)};
    const AttachSet set = build_set(all, 7);
    std::vector<uint64_t> order;
    uint64_t entered = 0, skipped = 0;
    bool ran = false;
    auto call = [&](uint32_t program, uint32_t site) {
        order.clear();
        entered = skipped = 0;
        ran = false;
        run_stages(set, program, site, [&](const Attachment& a, int32_t) { order.push_back(a.id); }, [&] { ran = true; },
                   [&] { return ran; }, &entered, &skipped);
    };
    call(0xAAAA, 0x1);
    CHECK((order == std::vector<uint64_t>{1, 2}) && entered == 2 && skipped == 2 && ran);
    call(0xCCCC, 0x9);  // matches nothing: no handler entered, every filter counted as skipped
    CHECK(order.empty() && entered == 0 && skipped == 4 && ran);
    call(0xBBBB, 0x2);
    CHECK((order == std::vector<uint64_t>{3, 4}));
    // an attachment whose program was not found stays off
    all[1].active = false;
    const AttachSet off = build_set(all, 7);
    order.clear();
    run_stages(off, 0xAAAA, 0x5, [&](const Attachment& a, int32_t) { order.push_back(a.id); }, [&] {}, [] { return false; },
               &entered, &skipped);
    CHECK(order.empty());
    // one Instead per (native, program, site)
    CHECK(instead_taken(all, 7, 0xBBBB, 0));
    CHECK(!instead_taken(all, 7, 0xBBBB, 0x3));
    CHECK(!instead_taken(all, 8, 0xBBBB, 0));
    CHECK(build_set(all, 9).empty());
}

static void sites() {
    // ICONST h; ECALL site hash; POP; BR 0 with an operand that looks like ECALL; HALT
    const uint32_t code[] = {11, 0xFFFF0000u, 19, 0x1234, 0xa18f5959u, 27, 11, 19, 8, 0, 30};
    CHECK(code_calls(code, 11, 0xa18f5959u, 0x1234));
    CHECK(code_calls(code, 11, 0xa18f5959u, 0));
    CHECK(!code_calls(code, 11, 0xa18f5959u, 0x9999));
    CHECK(!code_calls(code, 11, 0x01d09759u, 0));
    // the ECALL-looking operand of ICONST (19 at index 7) is not an instruction
    const uint32_t tricky[] = {11, 19, 11, 0x1234, 30};
    CHECK(!code_calls(tricky, 5, 0x1234, 0));
    const uint32_t bad[] = {99, 19, 1, 2};
    CHECK(!code_calls(bad, 4, 2, 1));
    const uint32_t cut[] = {19, 0x1234};  // operands run past the end
    CHECK(!code_calls(cut, 2, 0, 0x1234));
}

// A program's variable table (24-byte ImpactVariable entries) and its data size, for the typed checks.
struct Layout {
    std::vector<uint8_t> bytes;
    uint64_t data = 0;
    void add(uint32_t type, uint16_t size) {
        ImpactVariable v{};
        v.type = type;
        v.size = size;
        v.offset = static_cast<uint16_t>(data);
        data += size;
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof v);
    }
    uint64_t count() const { return bytes.size() / sizeof(ImpactVariable); }
};

constexpr uint32_t kSint = 0x9859f31eu, kFloat = 0xe89f7410u, kEntity = 0x737c8f86u;

static void typed() {
    Layout l;
    l.add(kSint, 4);   // 0
    l.add(kFloat, 4);  // 1
    l.add(kEntity, 4); // 2
    const KilnNativeArg decl[3] = {{kSint, 4, KILN_NATIVE_ARG_OUT, 0}, {kFloat, 4, KILN_NATIVE_ARG_IN, KILN_NATIVE_ARG_OPTIONAL},
                                   {0, 0, KILN_NATIVE_ARG_IN, KILN_NATIVE_ARG_RAW}};
    auto check = [&](std::vector<uint32_t> args, uint64_t data = 0) {
        return check_typed_args(decl, 3, args.data(), args.size(), l.bytes.data(), l.count(), data ? data : l.data);
    };
    CHECK(check({0xFFFF0000u, 0xFFFF0001u, 5}) == 0);
    CHECK(check({0xFFFF0001u, 0xFFFF0001u, 5}) == kErrTypeMismatch);   // a float32 for the sint32
    CHECK(check({0xFFFF0000u, 0xFFFF0000u, 5}) == kErrTypeMismatch);   // a sint32 for the float32
    CHECK(check({0xFFFF0000u, 0, 5}) == 0);                            // Optional<float>: not connected
    CHECK(check({0, 0xFFFF0001u, 5}) == kErrNoConnection);             // a required one not connected
    CHECK(check({7, 0xFFFF0001u, 5}) == kErrTypeMismatch);             // a raw value for a variable parameter
    CHECK(check({0xFFFF0000u, 0xFFFF0001u, 0}) == 0);                  // [Raw]: 0 is a value
    CHECK(check({0xFFFF0000u, 0xFFFF0001u, 0xFFFF0002u}) == 0);        // [Raw], type any: a variable is fine too
    CHECK(check({0xFFFF0009u, 0xFFFF0001u, 5}) == kErrTypeMismatch);   // an index past the layout
    CHECK(check({0xFFFF0000u, 0xFFFF0001u, 5}, 6) == kErrTypeMismatch); // the variable past the data
    CHECK(check({0xFFFF0000u, 0xFFFF0001u}) == kErrTypeMismatch);      // fewer arguments than declared
    // a size mismatch with the same type hash
    Layout w;
    w.add(kSint, 8);
    const uint32_t one[1] = {0xFFFF0000u};
    CHECK(check_typed_args(decl, 1, one, 1, w.bytes.data(), w.count(), w.data) == kErrTypeMismatch);
}

static void natives_json() {
    std::vector<std::string> notes;
    const char* text = R"({"format": 1, "natives": [
      {"name": "mymod.OnHit", "sync": true, "args": [
        {"name": "target", "type": "ecs.EntityId"},
        {"name": "attackMod", "type": "float32", "dir": "inout", "size": 4},
        {"name": "tag", "types": ["uint32"], "raw": true},
        {"name": "maybe", "type": "Time", "optional": true}]},
      {"name": "mymod.Old", "argc": 2, "args": [{"name": "x", "types": ["sint32", "float32"]}]},
      {"name": "other.Thing", "argc": 1},
      {"name": "mymod.Bad", "hash": "0x1234"},
      {"name": "mymod.Short", "sync": true, "argc": 2, "args": [{"type": "sint32"}]}
    ]})";
    const auto d = parse_natives_json(text, "mymod", &notes);
    CHECK(d.size() == 2);
    CHECK(notes.size() == 3);
    if (d.size() == 2) {
        CHECK(d[0].name == "mymod.OnHit" && d[0].typed && d[0].argc == 4 && d[0].hash == fnv1a_text("mymod.OnHit"));
        CHECK(d[0].args.size() == 4);
        if (d[0].args.size() == 4) {
            CHECK(d[0].args[0].type == kEntity && d[0].args[0].size == 4 && d[0].args[0].dir == KILN_NATIVE_ARG_IN);
            CHECK(d[0].args[1].type == kFloat && d[0].args[1].dir == KILN_NATIVE_ARG_INOUT);
            CHECK(d[0].args[2].type == 0x32940becu && (d[0].args[2].flags & KILN_NATIVE_ARG_RAW));
            CHECK(d[0].args[3].type == 0xdfe4e404u && d[0].args[3].size == 8 && (d[0].args[3].flags & KILN_NATIVE_ARG_OPTIONAL));
        }
        CHECK(d[1].name == "mymod.Old" && !d[1].typed && d[1].argc == 2 && d[1].args.empty());  // argc only: as before
    }
    bool threw = false;
    try { parse_natives_json(R"({"format": 2, "natives": []})", "mymod", nullptr); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { parse_natives_json("not json", "mymod", nullptr); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    CHECK(parse_natives_json(R"({"comment": "nothing"})", "mymod", nullptr).empty());
}

// keen::ImpactProgram blob (as mods/tests/abilities-test's ProgramBuilder writes it)
static std::vector<uint8_t> program(const std::vector<uint32_t>& code, const std::vector<std::pair<uint32_t, uint16_t>>& vars) {
    const uint8_t guid[16] = {6, 0xc, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const size_t codeOff = 64, shutOff = codeOff + code.size() * 4, layOff = shutOff + 4, dataOff = layOff + vars.size() * 24;
    size_t data = 0;
    for (auto& v : vars) data += v.second;
    std::vector<uint8_t> b((dataOff + data + 3) & ~size_t(3));
    const uint32_t id = fnv1a(guid, 16);
    std::memcpy(b.data(), &id, 4);
    std::memcpy(b.data() + 4, guid, 16);
    const uint16_t stack = 256;
    std::memcpy(b.data() + 0x14, &stack, 2);
    auto arr = [&](size_t field, size_t target, uint32_t count) {
        const int32_t off = count ? static_cast<int32_t>(target - field) : 0;
        std::memcpy(b.data() + field, &off, 4);
        std::memcpy(b.data() + field + 4, &count, 4);
    };
    arr(0x18, codeOff, static_cast<uint32_t>(code.size()));
    arr(0x20, shutOff, 1);
    arr(0x28, layOff, static_cast<uint32_t>(vars.size()));
    arr(0x30, dataOff, static_cast<uint32_t>(data));
    std::memcpy(b.data() + codeOff, code.data(), code.size() * 4);
    const uint32_t halt = 30;
    std::memcpy(b.data() + shutOff, &halt, 4);
    uint16_t at = 0;
    for (size_t i = 0; i < vars.size(); ++i) {
        ImpactVariable v{};
        v.type = vars[i].first;
        v.size = vars[i].second;
        v.offset = at;
        at = static_cast<uint16_t>(at + v.size);
        std::memcpy(b.data() + layOff + i * 24, &v, 24);
    }
    return b;
}

static void verifier() {
    constexpr uint32_t kTyped = 0x11112222u, kOld = 0x33334444u;
    const KilnNativeArg decl[2] = {{kSint, 4, KILN_NATIVE_ARG_OUT, 0}, {kSint, 4, KILN_NATIVE_ARG_IN, 0}};
    auto argc = [&](uint32_t h) { return h == kTyped || h == kOld ? 2 : -1; };
    auto typed_of = [&](uint32_t h) {
        TypedArgs t;
        if (h == kTyped) { t.args = decl; t.count = 2; }
        if (h == kOld) t.untyped_kiln = true;
        return t;
    };
    // Out = native(args[0] = Out (sint32), args[1] = In); POP; HALT
    auto calling = [&](uint32_t hash, uint32_t in_handle) {
        return program({11, in_handle, 11, 0xFFFF0000u, 19, 0x77, hash, 27, 30}, {{kSint, 4}, {kSint, 4}, {kFloat, 4}});
    };
    auto good = calling(kTyped, 0xFFFF0001u);
    auto r = verify_program(good.data(), good.size(), argc, typed_of);
    CHECK(r.ok && r.warnings.empty());
    auto bad = calling(kTyped, 0xFFFF0002u);  // a float32 for an int parameter
    r = verify_program(bad.data(), bad.size(), argc, typed_of);
    CHECK(!r.ok && r.error.rfind("E_ARGTYPE: code pc 4: native 0x11112222 args[1] is variable 2 (type 0xe89f7410", 0) == 0);
    auto raw = calling(kTyped, 9);  // a raw value
    r = verify_program(raw.data(), raw.size(), argc, typed_of);
    CHECK(!r.ok && r.error.find("raw value 9") != std::string::npos);
    auto none = program({12, 11, 0xFFFF0000u, 19, 0x77, kTyped, 27, 30}, {{kSint, 4}});  // ICONST0: not connected
    r = verify_program(none.data(), none.size(), argc, typed_of);
    CHECK(!r.ok && r.error.find("not connected") != std::string::npos);
    // an argc-only Kiln native: accepted, with the warning
    auto old = calling(kOld, 0xFFFF0002u);
    r = verify_program(old.data(), old.size(), argc, typed_of);
    CHECK(r.ok && r.warnings.size() == 1 && r.warnings[0].rfind("W_ARGTYPE", 0) == 0);
    // pushes the verifier cannot see (GLOAD) are left to the thunk
    auto unseen = program({22, 0xFFFF0001u, 11, 0xFFFF0000u, 19, 0x77, kTyped, 27, 30}, {{kSint, 4}, {kSint, 4}});
    r = verify_program(unseen.data(), unseen.size(), argc, typed_of);
    CHECK(r.ok);
    // without typed_of (the old entry point): as before
    r = verify_program(bad.data(), bad.size(), argc);
    CHECK(r.ok);
}

// review P1: the owner gate under real threads: a handler inside while its owner retires
static int64_t steady_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void gates() {
    OwnerGate g;
    CHECK(gate_enter(g));
    gate_leave(g);
    // a handler that stays inside: the retirement's bounded wait expires, reports not quiet, and admission is closed
    std::atomic<bool> release{false}, inside{false}, touched{false};
    int resource = 41;  // what the handler uses after the gate opens (its mod's state)
    std::thread handler([&] {
        if (!gate_enter(g)) return;
        inside = true;
        while (!release) std::this_thread::yield();
        touched = ++resource == 42;  // still intact: nothing was freed under it
        gate_leave(g);
    });
    while (!inside) std::this_thread::yield();
    const int64_t t0 = steady_ns();
    const bool quiet = gate_close_and_wait(g, 2'000'000, steady_ns, [] { std::this_thread::yield(); });
    const int64_t waited = steady_ns() - t0;
    CHECK(!quiet && g.closed && g.inflight == 1);
    CHECK(waited >= 2'000'000 && waited < 500'000'000);  // bounded: about 2 ms, never for ever
    CHECK(!gate_enter(g));                              // no new entry once closed
    CHECK(g.inflight == 1);
    // the deferred retirement keeps its payload until the gate is quiet
    std::vector<Deferred<int>> deferred{{&g, 7, 10}};
    int released = 0;
    uint64_t late = 0;
    CHECK(finish_quiet(deferred, 11, [&](int p) { released = p; }, [&](OwnerGate*, uint64_t s) { late = s; }) == 0);
    CHECK(released == 0 && deferred.size() == 1);
    release = true;
    handler.join();
    CHECK(touched && g.inflight == 0);
    CHECK(finish_quiet(deferred, 13, [&](int p) { released = p; }, [&](OwnerGate*, uint64_t s) { late = s; }) == 1);
    CHECK(released == 7 && late == 3 && deferred.empty());
    // a gate that is quiet: the wait ends at once
    OwnerGate q;
    CHECK(gate_close_and_wait(q, 2'000'000, steady_ns, [] {}));
    // many entries racing a close: every entry either got in before the close (and is counted) or was refused
    for (int round = 0; round < 50; ++round) {
        OwnerGate r;
        std::atomic<int> in{0}, refused{0};
        std::atomic<bool> go{false};
        std::vector<std::thread> ts;
        for (int k = 0; k < 4; ++k)
            ts.emplace_back([&] {
                while (!go) std::this_thread::yield();
                for (int i = 0; i < 200; ++i) {
                    if (gate_enter(r)) {
                        ++in;
                        gate_leave(r);
                    } else {
                        ++refused;
                    }
                }
            });
        go = true;
        gate_close_and_wait(r, 50'000'000, steady_ns, [] { std::this_thread::yield(); });
        const int after_close = in.load();
        for (auto& t : ts) t.join();
        CHECK(r.inflight == 0);
        CHECK(in.load() + refused.load() == 800);
        CHECK(in.load() == after_close);  // nothing entered after the close returned quiet
    }
}

static void watchdog() {
    InsideTable<1> owned;
    char mutable_name[] = "mod.Original";
    owned.claim(1, mutable_name, 1);
    mutable_name[4] = 'X';
    std::string original;
    owned.scan(100, 1, [&](const char* what, KilnOwner, int64_t) { original = what; });
    CHECK(original == "mod.Original");
    InsideTable<4> t;
    const int a = t.claim(100, "mod.Hang", 9);
    const int b = t.claim(150, "mod.Quick", 9);
    CHECK(a >= 0 && b >= 0 && a != b);
    t.release(b);
    std::vector<std::string> said;
    auto report = [&](const char* what, KilnOwner owner, int64_t inside) { said.push_back(std::string(what) + "/" + std::to_string(owner) + "/" + std::to_string(inside)); };
    CHECK(t.scan(1000, 1000, report) == 0);  // inside for 900 < 1000
    CHECK(t.scan(1200, 1000, report) == 1 && said.back() == "mod.Hang/9/1100");
    CHECK(t.scan(5000, 1000, report) == 0);  // once per call
    t.release(a);
    for (int i = 0; i < 4; ++i) CHECK(t.claim(1, "x", 1) >= 0);
    CHECK(t.claim(1, "full", 1) == -1);      // a full table watches no more calls (they still run)
}

int main() {
    names();
    gates();
    watchdog();
    typed();
    natives_json();
    verifier();
    stages();
    filters();
    sites();
    room();
    results();
    faults();
    budget();
    std::printf("natives_test: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
