// SPDX-License-Identifier: MIT
// kiln.engine: one engine call inside a held write gate (invoke.cpp) over a fake engine from both
// real profiles. The test's layout is the profile's plus three recipes over a fake code page in the fake image; the
// host's call hook stands in for the engine function (tests/host/invoke_host.cpp; windows_host runs real helper code
// under SEH, on Wine and native Windows):
// - a call that returns: its entry, its arguments in order, the result masked to the recipe's bits, the gate kept held;
// - every refusal, in order, with nothing called, nothing latched and the gate kept held;
// - the compare before the call (engine-code-changed) and after it, for every range, and an unreadable range;
// - a fault, an exception passed on and resumed by an outer handler, a code change and a gate change during the call:
//   Uncertain, latched for the session and noted once; the latch refuses every later call and Status, survives
//   STOPPING and a new hold, and clears at the next STARTED;
// - op Latch and op Status;
// - no allocation on any path (global operator new counter);
// - under ThreadSanitizer: the latch against STARTED (a latch is never lost in its own session) and a faulting holder
//   against STARTED.
#include <sys/mman.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "engine_state.h"
#include "fake_engine.h"
#include "gate.h"
#include "invoke.h"
#include "profiles.g.h"
#include "resolve.h"

namespace kiln::engine::mem {
extern std::atomic<std::uintptr_t> test_image;  // mem::image_base() of the host build (tests/host/memory_host.cpp)
}  // namespace kiln::engine::mem
namespace kiln::engine::call {
extern std::atomic<Return (*)(std::uintptr_t, const std::uint64_t*)> test_call_hook;
extern std::atomic<std::uint64_t> test_calls, test_compares;
extern std::atomic<bool> test_stack_unknown;
}  // namespace kiln::engine::call

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
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}
std::string name(std::uint32_t r) { return reason_name(static_cast<Reason>(r)); }
std::uint32_t code(Reason r) { return static_cast<std::uint32_t>(r); }
constexpr std::uint32_t kRefused = static_cast<std::uint32_t>(CallOutcome::Refused);
constexpr std::uint32_t kReturned = static_cast<std::uint32_t>(CallOutcome::Returned);
constexpr std::uint32_t kUncertain = static_cast<std::uint32_t>(CallOutcome::Uncertain);
std::atomic<std::uint64_t> g_step{5000};
std::int32_t word(std::uintptr_t a) { return __atomic_load_n(fake::word(a), __ATOMIC_SEQ_CST); }

// The engine function's stand-in: what it answers, and what it does meanwhile.
constexpr std::uint64_t kValue = 0x8877665544332211ull;
constexpr std::uint32_t kAccessViolation = 0xC0000005u, kSingleStep = 0x80000004u;
enum class Mode : int { Return, Fault, PatchCode, BreakGate, WriteBuffer, Resumed };
std::atomic<int> g_mode{0};
std::atomic<std::uintptr_t> g_entry{0}, g_patch_at{0}, g_stop_flag{0};
std::uint64_t g_args[8];  // written by the hook on the calling thread, read after it
call::Return hook(std::uintptr_t entry, const std::uint64_t* args) {
    g_entry.store(entry);
    std::memcpy(g_args, args, sizeof g_args);
    switch (static_cast<Mode>(g_mode.load())) {
        case Mode::Fault: return {0, 1, kAccessViolation};
        // Windows: the filter saw a single step, passed it on, and an outer handler resumed the call, which returned
        case Mode::Resumed: return {kValue, 0, kSingleStep};
        case Mode::PatchCode: *reinterpret_cast<std::uint8_t*>(g_patch_at.load()) ^= 0xff; return {kValue, 0, 0};
        case Mode::BreakGate: fake::put<std::uint32_t>(g_stop_flag.load(), 1); return {kValue, 0, 0};
        case Mode::WriteBuffer: std::memset(reinterpret_cast<void*>(args[0]), 0x5a, 64); return {kValue, 0, 0};
        default: return {kValue, 0, 0};
    }
}

const Layout& profile_layout(const char* target) {
    for (const auto& p : generated::kProfiles)
        if (std::string(p.target) == target) return p.layout;
    std::abort();
}
// The fake code: two pages below the end of the fake image, clear of every structure the fake engine lays out.
std::size_t code_rva(const Layout& L) { return (L.image_size - 0x10000) & ~std::size_t{0xfff}; }
Layout with_recipes(const char* target) {
    Layout L = profile_layout(target);
    const std::size_t page = code_rva(L);
    CallRecipe& one = L.calls.recipe[0];  // 8 arguments, the first a 64-byte buffer; three ranges on two pages
    one.id = 1;
    one.arg_count = 8;
    one.result_bits = 64;
    one.range_count = 3;
    one.arg_bytes[0] = 64;
    one.ranges[0] = {page, 64};
    one.ranges[1] = {page + 0x100, 32};
    one.ranges[2] = {page + 0x1000, 16};
    CallRecipe& two = L.calls.recipe[1];  // two integers, 32 result bits
    two.id = 2;
    two.arg_count = 2;
    two.result_bits = 32;
    two.range_count = 1;
    two.ranges[0] = {page + 0x200, 8};
    CallRecipe& three = L.calls.recipe[2];  // no argument, no result
    three.id = 3;
    three.range_count = 1;
    three.ranges[0] = {page + 0x300, 1};
    return L;
}

struct Bench {
    Layout L;
    fake::Engine e;
    std::string t;
    EcsView view{};
    GateState gate{};
    std::uintptr_t page = 0;
    std::vector<std::uint8_t> captured;  // recipe 1's three ranges as the caller captured them, then 64 spare bytes
    std::vector<std::uint8_t> buffer;    // recipe 1's argument buffer
    explicit Bench(const char* target)
        : L(with_recipes(target)), e(L), t(std::string(target) + ": "), captured(112 + 64, 0), buffer(64, 0) {
        page = e.image.at(code_rva(L));
        for (std::size_t i = 0; i < 0x2000; ++i) fake::put<std::uint8_t>(page + i, static_cast<std::uint8_t>(i * 37 + 11));
        std::size_t at = 0;
        for (std::size_t r = 0; r < 3; ++r) {
            const CallRange& g = L.calls.recipe[0].ranges[r];
            std::memcpy(captured.data() + at, reinterpret_cast<const void*>(e.image.at(g.rva)), g.size);
            at += g.size;
        }
        const auto& w = L.write;
        const std::size_t page_rva = code_rva(L);
        check(w.registry_lock < page_rva && w.registry_entries + w.capacity * w.stride < page_rva &&
                  w.registry_count < page_rva && w.callback < page_rva && w.tracker + 33 < page_rva &&
                  L.resource_write.callback < page_rva && (L.dedicated_server || L.root_rva + L.client_offset + 8 < page_rva),
              t + "the fake code page is clear of the fake engine's image structures");
        mem::test_image = e.image.at();  // the loaded game image is the fake engine's
        check(ecs_view(&L, ViewKind::Authority, view, e.image.at()) == Reason::None, t + "authority view");
        std::uintptr_t low = 0, high = 0;
        call::stack_range(low, high);  // read once outside any measured section
    }
    bool acquire() {
        gate = GateState{};
        gate.size = sizeof gate;
        gate.owner = 1;
        gate.step = ++g_step;
        return gate_acquire(&L, view, gate, CallerThread::Other, e.image.at()) == Reason::None;
    }
    bool held() const {
        return gate.held && word(e.registry_word()) == 1 && word(e.scene_word()) == static_cast<std::int32_t>(0x80000000u);
    }
    bool words_free() const { return word(e.registry_word()) == 0 && word(e.scene_word()) == 0; }
    KilnEngineCall call1() const {
        KilnEngineCall c{};
        c.size = sizeof c;
        c.op = static_cast<std::uint32_t>(CallOp::Invoke);
        c.recipe = 1;
        c.arg_count = 8;
        c.args = {reinterpret_cast<std::uintptr_t>(buffer.data()), 2, 3, 4, 5, 6, 7, 8};
        c.code = captured.data();
        c.code_bytes = 112;
        return c;
    }
    KilnEngineCallResult run(const KilnEngineCall& c, const GateState* g, const Layout* layout) {
        KilnEngineCallResult r{};
        r.size = sizeof r;
        gate_invoke(layout, g, c, r);
        return r;
    }
    KilnEngineCallResult run(const KilnEngineCall& c) { return run(c, &gate, &L); }
    KilnEngineCallResult op(CallOp which, std::uint32_t recipe, const GateState* g) {
        KilnEngineCall c{};
        c.size = sizeof c;
        c.op = static_cast<std::uint32_t>(which);
        c.recipe = recipe;
        return run(c, g, &L);
    }
    std::uintptr_t stop_flag() const { return e.thread + L.write.thread_stop; }
};

void returns(const char* target) {
    Bench b(target);
    const std::string& t = b.t;
    gate_reset_for_tests();
    g_mode = static_cast<int>(Mode::Return);
    check(b.acquire(), t + "gate acquired");
    const auto calls = call::test_calls.load(), compares = call::test_compares.load();
    const KilnEngineCall c = b.call1();
    KilnEngineCallResult r = b.run(c);
    check(r.size == sizeof r && r.outcome == kReturned && r.reason == 0 && !r.faulted && !r.exception && r.value == kValue,
          t + "returned with all 64 result bits (got outcome " + std::to_string(r.outcome) + ", " + name(r.reason) + ")");
    check(call::test_calls.load() == calls + 1 && g_entry.load() == b.e.image.at(b.L.calls.recipe[0].ranges[0].rva),
          t + "called once, at image + ranges[0].rva");
    check(std::memcmp(g_args, c.args.data(), sizeof g_args) == 0, t + "the 8 arguments in order, the buffer's address as it is");
    check(call::test_compares.load() == compares + 6, t + "every range compared before and after the call (3 + 3)");
    check(b.held() && !gate_calls_latched(), t + "the gate stays held; nothing latched");
    // the result is the recipe's bits of rax, the rest masked off
    const std::pair<std::uint32_t, std::uint64_t> masks[] = {{32, 0x44332211u}, {16, 0x2211u}, {8, 0x11u}, {0, 0u}};
    for (const auto& [bits, want] : masks) {
        b.L.calls.recipe[0].result_bits = bits;
        r = b.run(c);
        check(r.outcome == kReturned && r.value == want, t + std::to_string(bits) + " result bits masked");
    }
    b.L.calls.recipe[0].result_bits = 64;
    g_mode = static_cast<int>(Mode::WriteBuffer);
    r = b.run(c);
    bool written = true;
    for (std::uint8_t v : b.buffer) written = written && v == 0x5a;
    check(r.outcome == kReturned && written, t + "the engine writes the caller's argument buffer");
    g_mode = static_cast<int>(Mode::Return);
    KilnEngineCall two{};
    two.size = sizeof two;
    two.recipe = 2;
    two.arg_count = 2;
    two.args[0] = 0x1111;
    two.args[1] = 0x2222;
    two.code = b.captured.data() + 112;
    std::memcpy(b.captured.data() + 112, reinterpret_cast<const void*>(b.e.image.at(b.L.calls.recipe[1].ranges[0].rva)), 8);
    two.code_bytes = 8;
    r = b.run(two);
    check(r.outcome == kReturned && r.value == 0x44332211u && g_args[0] == 0x1111 && g_args[1] == 0x2222 && !g_args[2] &&
              !g_args[7] && g_entry.load() == b.e.image.at(b.L.calls.recipe[1].ranges[0].rva),
          t + "recipe 2: two integers in rcx and rdx, the rest zero, 32 result bits");
    KilnEngineCall three{};
    three.size = sizeof three;
    three.recipe = 3;
    three.code = b.captured.data() + 112;
    b.captured[112] = fake::get<std::uint8_t>(b.e.image.at(b.L.calls.recipe[2].ranges[0].rva));
    three.code_bytes = 1;
    r = b.run(three);
    bool zero = true;
    for (std::uint64_t a : g_args) zero = zero && !a;
    check(r.outcome == kReturned && r.value == 0 && zero, t + "recipe 3: no argument, no result");
    // no allocation on the returned path
    const long before = g_allocations.load();
    for (int i = 0; i < 3; ++i) r = b.run(c);
    const bool none = g_allocations.load() == before && r.outcome == kReturned;  // before the message is built
    check(none, t + "no allocation on the returned path");
    check(b.held(), t + "still held after every call");
    gate_release(b.gate);
    check(b.words_free(), t + "released by the caller: words free");
}

void refusals(const char* target) {
    Bench b(target);
    const std::string& t = b.t;
    gate_reset_for_tests();
    g_mode = static_cast<int>(Mode::Return);
    check(b.acquire(), t + "gate acquired");
    const auto refused = [&](const KilnEngineCall& c, const GateState* g, const Layout* layout, Reason want,
                             const std::string& what) {
        const auto calls = call::test_calls.load();
        const long allocations = g_allocations.load();
        const KilnEngineCallResult r = b.run(c, g, layout);
        const bool ok = r.size == sizeof r && r.outcome == kRefused && r.reason == code(want) && !r.faulted && !r.value &&
                        call::test_calls.load() == calls && g_allocations.load() == allocations && b.held();
        check(ok, t + what + ": refused " + reason_name(want) + ", nothing called, gate held (got outcome " +
                  std::to_string(r.outcome) + ", " + name(r.reason) + ")");
        check(want == Reason::EngineCallLatched || !gate_calls_latched(), t + what + ": nothing latched");
    };
    const KilnEngineCall good = b.call1();
    // the gate
    refused(good, nullptr, &b.L, Reason::EngineCallGateNotHeld, "no gate");
    GateState unheld{};
    unheld.size = sizeof unheld;
    refused(good, &unheld, &b.L, Reason::EngineCallGateNotHeld, "an unheld gate record");
    GateState half = b.gate;
    half.scene_lock = 0;
    refused(good, &half, &b.L, Reason::EngineCallGateNotHeld, "a record without its Scene word");
    gate_close_session(true, 0);
    refused(good, &b.gate, &b.L, Reason::KilnGateClosed, "the server world began stopping during the hold");
    gate_close_session(false);
    // the latch
    gate_calls_latch();
    refused(good, &b.gate, &b.L, Reason::EngineCallLatched, "latched");
    const KilnEngineCallResult status = b.op(CallOp::Status, 1, nullptr);
    check(status.outcome == kRefused && status.reason == code(Reason::EngineCallLatched), t + "Status while latched");
    gate_close_session(false);
    check(!gate_calls_latched() && b.op(CallOp::Status, 1, nullptr).outcome == kReturned, t + "STARTED cleared the latch");
    // the recipe
    for (std::uint32_t id : {0u, 4u, 8u, 9u, 0xffffffffu}) {
        KilnEngineCall c = good;
        c.recipe = id;
        refused(c, &b.gate, &b.L, Reason::EngineCallRecipeUnavailable, "recipe " + std::to_string(id));
    }
    refused(good, &b.gate, nullptr, Reason::EngineCallRecipeUnavailable, "no exact profile");
    check(b.op(CallOp::Status, 4, nullptr).reason == code(Reason::EngineCallRecipeUnavailable) &&
              b.op(CallOp::Status, 0, nullptr).outcome == kReturned && b.op(CallOp::Status, 2, &b.gate).outcome == kReturned,
          t + "Status: an empty slot is unavailable; no recipe or a recipe of this build is fine");
    // the arguments and the capture
    const auto with = [&](auto change) {
        KilnEngineCall c = good;
        change(c);
        return c;
    };
    const std::uintptr_t heap = reinterpret_cast<std::uintptr_t>(b.buffer.data());
    const std::uintptr_t capture = reinterpret_cast<std::uintptr_t>(b.captured.data());
    const std::uintptr_t image = b.e.image.at();
    const Reason invalid = Reason::EngineCallArgumentInvalid;
    // the call target: the gate's image must be the loaded game image itself, checked after the recipe and
    // before the arguments; the record still validates against the image it was acquired on
    for (const std::uintptr_t loaded : {image + 0x1000, image - 0x10000, std::uintptr_t{0}}) {
        mem::test_image = loaded;
        refused(good, &b.gate, &b.L, Reason::EngineCallTarget, "the gate's image is not the loaded image");
        refused(with([](KilnEngineCall& c) { c.arg_count = 7; }), &b.gate, &b.L, Reason::EngineCallTarget,
                "another image and a wrong argument count: the target first");
        KilnEngineCall c = good;
        c.recipe = 4;
        refused(c, &b.gate, &b.L, Reason::EngineCallRecipeUnavailable, "another image and an empty slot: the recipe first");
        check(b.op(CallOp::Status, 1, nullptr).outcome == kReturned, t + "another image: Status does not look at it");
    }
    mem::test_image = image;
    check(b.run(good).outcome == kReturned && !gate_calls_latched(), t + "the loaded image again: the call returns");
    refused(with([](KilnEngineCall& c) { c.arg_count = 7; }), &b.gate, &b.L, invalid, "fewer arguments than the recipe's");
    refused(with([](KilnEngineCall& c) { c.arg_count = 9; }), &b.gate, &b.L, invalid, "nine arguments");
    {
        KilnEngineCall c{};
        c.size = sizeof c;
        c.recipe = 2;
        c.arg_count = 2;
        c.args[7] = 1;
        c.code = b.captured.data() + 112;
        c.code_bytes = 8;
        refused(c, &b.gate, &b.L, invalid, "an unused argument that is not zero");
    }
    refused(with([](KilnEngineCall& c) { c.code = nullptr; }), &b.gate, &b.L, invalid, "no capture");
    refused(with([](KilnEngineCall& c) { c.code_bytes = 111; }), &b.gate, &b.L, invalid, "a capture one byte short");
    refused(with([](KilnEngineCall& c) { c.code_bytes = 113; }), &b.gate, &b.L, invalid, "a capture one byte long");
    refused(with([](KilnEngineCall& c) { c.buffer_count = 9; }), &b.gate, &b.L, invalid, "nine further buffers");
    refused(with([](KilnEngineCall& c) { c.args[0] = 0; }), &b.gate, &b.L, invalid, "a null pointer argument");
    refused(with([](KilnEngineCall& c) { c.args[0] = UINTPTR_MAX - 10; }), &b.gate, &b.L, invalid,
            "a pointer argument whose buffer overflows");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 1; c.buffers[0] = {0, 16}; }), &b.gate, &b.L, invalid,
            "a null further buffer");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 1; c.buffers[0] = {heap, 0}; }), &b.gate, &b.L, invalid,
            "an empty further buffer");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 1; c.buffers[0] = {UINTPTR_MAX - 4, 16}; }), &b.gate, &b.L,
            invalid, "a further buffer that overflows");
    refused(with([&](KilnEngineCall& c) { c.args[0] = capture + 8; }), &b.gate, &b.L, invalid,
            "the argument buffer overlaps the capture");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 1; c.buffers[0] = {capture + 111, 16}; }), &b.gate, &b.L,
            invalid, "a further buffer overlaps the capture's last byte");
    {
        KilnEngineCall c = with([&](KilnEngineCall& d) { d.buffer_count = 1; d.buffers[0] = {capture + 112, 64}; });
        check(b.run(c).outcome == kReturned, t + "a further buffer right after the capture is fine");
    }
    refused(with([&](KilnEngineCall& c) { c.args[0] = image + 0x100; }), &b.gate, &b.L, invalid,
            "the argument buffer lies inside the game image");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 1; c.buffers[0] = {image + b.L.image_size - 8, 64}; }), &b.gate,
            &b.L, invalid, "a further buffer runs into the game image's end");
    refused(with([&](KilnEngineCall& c) { c.code = reinterpret_cast<const std::uint8_t*>(b.page); }), &b.gate, &b.L, invalid,
            "the capture is the live code itself");
    refused(with([&](KilnEngineCall& c) { c.code = reinterpret_cast<const std::uint8_t*>(UINTPTR_MAX - 50); }), &b.gate,
            &b.L, invalid, "a capture that overflows");
    // the stack: the engine's own frames grow into it
    const std::uintptr_t stack = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)) - 4096;
    const Reason on_stack = Reason::EngineCallArgumentOnStack;
    refused(with([&](KilnEngineCall& c) { c.args[0] = stack; }), &b.gate, &b.L, on_stack, "the argument buffer on the stack");
    refused(with([&](KilnEngineCall& c) { c.buffer_count = 2; c.buffers[0] = {heap, 8}; c.buffers[1] = {stack, 8}; }),
            &b.gate, &b.L, on_stack, "a further buffer on the stack");
    refused(with([&](KilnEngineCall& c) { c.code = reinterpret_cast<const std::uint8_t*>(stack); }), &b.gate, &b.L, on_stack,
            "the capture on the stack");
    call::test_stack_unknown = true;
    refused(good, &b.gate, &b.L, on_stack, "the stack's range unknown (fail closed)");
    call::test_stack_unknown = false;
    // the edges are exact: a buffer ending where the stack or the image begins, or beginning where it ends, is fine; one
    // byte more overlaps (the buffers are never touched here: the hook only returns)
    {
        std::uintptr_t low = 0, high = 0;
        check(call::stack_range(low, high) && low < high, t + "the stack's range");
        const auto one = [&](std::uintptr_t address, std::size_t bytes) {
            KilnEngineCall c = good;
            c.buffer_count = 1;
            c.buffers[0] = {address, bytes};
            return b.run(c);
        };
        check(one(low - 64, 64).outcome == kReturned && one(high, 64).outcome == kReturned,
              t + "a buffer ending at the stack's low end, or starting at its high end, is fine");
        check(one(low - 64, 65).reason == code(on_stack) && one(high - 1, 64).reason == code(on_stack),
              t + "one byte into the stack at either end is refused");
        check(one(image - 64, 64).outcome == kReturned && one(image + b.L.image_size, 64).outcome == kReturned,
              t + "a buffer ending at the image's start, or starting at its end, is fine");
        check(one(image - 64, 65).reason == code(invalid) && one(image + b.L.image_size - 1, 64).reason == code(invalid),
              t + "one byte into the image at either end is refused");
    }
    // the gate no longer validates
    fake::put<std::uint32_t>(b.stop_flag(), 1);
    refused(good, &b.gate, &b.L, Reason::EngineCallGateInvalid, "the GameServer thread stopping");
    fake::put<std::uint32_t>(b.stop_flag(), 0);
    // the live code differs from the capture: every range, its first, a middle and its last byte
    for (std::size_t r = 0; r < 3; ++r) {
        const CallRange& g = b.L.calls.recipe[0].ranges[r];
        for (std::size_t at : {std::size_t{0}, g.size / 2, g.size - 1}) {
            const std::uintptr_t byte = b.e.image.at(g.rva + at);
            fake::put<std::uint8_t>(byte, fake::get<std::uint8_t>(byte) ^ 0x01);
            refused(good, &b.gate, &b.L, Reason::EngineCodeChanged,
                    "range " + std::to_string(r) + " byte " + std::to_string(at) + " changed before the call");
            fake::put<std::uint8_t>(byte, fake::get<std::uint8_t>(byte) ^ 0x01);
        }
    }
    check(mprotect(reinterpret_cast<void*>(b.page + 0x1000), 0x1000, PROT_NONE) == 0, t + "the third range's page made unreadable");
    refused(good, &b.gate, &b.L, Reason::EngineCodeChanged, "an unreadable range");
    mprotect(reinterpret_cast<void*>(b.page + 0x1000), 0x1000, PROT_READ | PROT_WRITE);
    check(b.run(good).outcome == kReturned, t + "after every refusal the same call returns");
    // malformed recipes are never used
    const auto malformed = [&](auto change, const std::string& what) {
        Layout L = b.L;
        change(L.calls.recipe[0]);
        check(!call_recipe(&L, 1) && call_recipe(&L, 2), t + "malformed recipe refused: " + what);
    };
    malformed([](CallRecipe& r) { r.id = 2; }, "an id that is not its slot + 1");
    malformed([](CallRecipe& r) { r.arg_count = 9; }, "nine arguments");
    malformed([](CallRecipe& r) { r.result_bits = 12; }, "12 result bits");
    malformed([](CallRecipe& r) { r.result_bits = 128; }, "128 result bits");
    malformed([](CallRecipe& r) { r.range_count = 0; }, "no range");
    malformed([](CallRecipe& r) { r.range_count = 33; }, "33 ranges");
    malformed([](CallRecipe& r) { r.ranges[1].size = 0; }, "an empty range");
    malformed([&](CallRecipe& r) { r.ranges[2].rva = b.L.image_size; }, "a range starting at the image's end");
    malformed([&](CallRecipe& r) { r.ranges[2] = {b.L.image_size - 8, 16}; }, "a range running past the image's end");
    malformed([](CallRecipe& r) { r.ranges[2] = {UINTPTR_MAX - 4, 16}; }, "a range whose end overflows");
    malformed([&](CallRecipe& r) { r.ranges[0].size = 40 * 1024; r.ranges[1].size = 24 * 1024 + 1; },
              "more than 64 KiB of code");
    malformed([](CallRecipe& r) { r.arg_count = 0; }, "a pointer size on an argument past the count");
    malformed([](CallRecipe& r) { r.ranges[3] = {0x1000, 4}; }, "a range past the count that is not zero");
    {
        Layout L = b.L;
        L.calls.recipe[0].ranges[0].size = 40 * 1024;
        L.calls.recipe[0].ranges[1].size = 24 * 1024;
        L.calls.recipe[0].ranges[2] = {0, 0};
        L.calls.recipe[0].range_count = 2;
        check(call_recipe(&L, 1) != nullptr, t + "exactly 64 KiB of code is fine");
    }
    check(!call_recipe(nullptr, 1) && !call_recipe(&b.L, 0) && !call_recipe(&b.L, 9) && call_recipe(&b.L, 3),
          t + "call_recipe: no layout, id 0 and id 9 are none; a filled slot is its recipe");
    check(!gate_calls_latched() && b.held(), t + "after all refusals: nothing latched, the gate held");
    gate_release(b.gate);
    check(b.words_free(), t + "released by the caller");
}

void faults(const char* target) {
    Bench b(target);
    const std::string& t = b.t;
    gate_reset_for_tests();
    check(b.acquire(), t + "gate acquired");
    const KilnEngineCall c = b.call1();
    CallNote note{};
    check(!gate_invoke_take_note(note), t + "no note before any latch");
    // a fault: the SEH handler ran (host: the hook says so)
    g_mode = static_cast<int>(Mode::Fault);
    auto calls = call::test_calls.load();
    long allocations = g_allocations.load();
    KilnEngineCallResult r = b.run(c);
    const bool fault_ok = r.outcome == kUncertain && r.reason == code(Reason::EngineCallFaulted) && r.faulted == 1 &&
                          r.exception == kAccessViolation && r.value == 0 && call::test_calls.load() == calls + 1 &&
                          g_allocations.load() == allocations;
    check(fault_ok, t + "a fault: Uncertain, engine-call-faulted, the exception code, no allocation (got " + name(r.reason) + ")");
    check(gate_calls_latched() && b.held(), t + "a fault latches every engine call; the gate stays held");
    check(gate_invoke_take_note(note) && note.reason == Reason::EngineCallFaulted && note.recipe == 1 &&
              note.exception == kAccessViolation && !gate_invoke_take_note(note),
          t + "the latch is noted once, with its cause, for the log after release");
    g_mode = static_cast<int>(Mode::Return);
    calls = call::test_calls.load();
    r = b.run(c);
    check(r.outcome == kRefused && r.reason == code(Reason::EngineCallLatched) && call::test_calls.load() == calls &&
              !gate_invoke_take_note(note),
          t + "the next call is refused engine-call-latched, nothing called, no new note");
    check(b.op(CallOp::Status, 1, nullptr).reason == code(Reason::EngineCallLatched), t + "Status says latched");
    gate_release(b.gate);
    check(b.acquire(), t + "a new hold in the same session");
    check(b.run(c).reason == code(Reason::EngineCallLatched), t + "still latched in a new hold of the same session");
    gate_close_session(true, 0);
    check(gate_calls_latched(), t + "STOPPING does not clear the latch");
    gate_close_session(false);
    check(!gate_calls_latched() && b.op(CallOp::Status, 1, nullptr).outcome == kReturned, t + "the next STARTED clears it");
    r = b.run(c);
    check(r.outcome == kReturned && r.value == kValue, t + "and the call returns again");
    // an exception that reached the call, went on and was resumed further out: the call returned, but its outcome is
    // not proven: Uncertain (not faulted at this frame, the code kept), latched and noted
    g_mode = static_cast<int>(Mode::Resumed);
    r = b.run(c);
    check(r.outcome == kUncertain && r.reason == code(Reason::EngineCallFaulted) && r.faulted == 0 &&
              r.exception == kSingleStep && r.value == 0 && gate_calls_latched(),
          t + "an exception passed on and resumed: Uncertain, not faulted, its code, latched (got outcome " +
              std::to_string(r.outcome) + ", " + name(r.reason) + ")");
    check(gate_invoke_take_note(note) && note.reason == Reason::EngineCallFaulted && note.exception == kSingleStep &&
              !gate_invoke_take_note(note),
          t + "the resumed exception is noted once, with its code");
    gate_close_session(false);
    // the code changed during the call (the helper patched its own range): the compare after it
    g_mode = static_cast<int>(Mode::PatchCode);
    const CallRange& last = b.L.calls.recipe[0].ranges[2];
    g_patch_at = b.e.image.at(last.rva + last.size - 1);
    r = b.run(c);
    check(r.outcome == kUncertain && r.reason == code(Reason::EngineCallCodeChangedDuringCall) && !r.faulted &&
              r.value == 0 && gate_calls_latched(),
          t + "code changed during the call: Uncertain and latched (got " + name(r.reason) + ")");
    check(gate_invoke_take_note(note) && note.reason == Reason::EngineCallCodeChangedDuringCall, t + "noted");
    *reinterpret_cast<std::uint8_t*>(g_patch_at.load()) ^= 0xff;
    gate_close_session(false);
    // the gate changed during the call
    g_mode = static_cast<int>(Mode::BreakGate);
    g_stop_flag = b.stop_flag();
    r = b.run(c);
    check(r.outcome == kUncertain && r.reason == code(Reason::EngineCallGateChangedDuringCall) && gate_calls_latched(),
          t + "the gate changed during the call: Uncertain and latched (got " + name(r.reason) + ")");
    check(gate_invoke_take_note(note) && note.reason == Reason::EngineCallGateChangedDuringCall, t + "noted");
    fake::put<std::uint32_t>(b.stop_flag(), 0);
    gate_close_session(false);
    g_mode = static_cast<int>(Mode::Return);
    // op Latch: a member's own check after the call failed
    r = b.op(CallOp::Latch, 1, &b.gate);
    check(r.outcome == kReturned && gate_calls_latched() && gate_invoke_take_note(note) &&
              note.reason == Reason::EngineCallMemberUncertain && note.recipe == 1,
          t + "op Latch inside the hold: latched and noted");
    gate_close_session(false);
    GateState unheld{};
    unheld.size = sizeof unheld;
    r = b.op(CallOp::Latch, 1, &unheld);
    check(r.outcome == kRefused && r.reason == code(Reason::EngineCallGateNotHeld) && !gate_calls_latched(),
          t + "op Latch without a held gate: refused, nothing latched");
    KilnEngineCall unknown = c;
    unknown.op = 3;
    r = b.run(unknown);
    check(r.outcome == kRefused && r.reason == code(Reason::EngineCallArgumentInvalid) && !gate_calls_latched(),
          t + "an unknown op: refused");
    check(b.held(), t + "the gate is held after every outcome (the call never releases it)");
    gate_release(b.gate);
    check(b.words_free(), t + "released by the caller");
}

// A faulting holder against STARTED: after a faulted call whose session did not change meanwhile, calls are latched.
void invoke_race(const char* target) {
    Bench b(target);
    const std::string& t = b.t;
    gate_reset_for_tests();
    check(b.acquire(), t + "race: gate acquired");
    g_mode = static_cast<int>(Mode::Fault);
    std::atomic<bool> stop{false};
    std::atomic<long> iterations{0};
    // A STARTED about every third call, at no fixed point of it: so faults inside an unchanged session (the case the
    // invariant is about), latched refusals after them, and STARTEDs landing inside a call all happen.
    std::thread starter([&] {
        while (!stop.load()) {
            gate_close_session(false);
            const long seen = iterations.load();
            while (!stop.load() && iterations.load() < seen + 3) std::this_thread::yield();
        }
    });
    long faulted = 0, refused = 0, moved = 0, lost = 0, other = 0;
    const KilnEngineCall c = b.call1();
    CallNote note{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int i = 0; (i < 6000 || faulted == moved || !refused) && std::chrono::steady_clock::now() < deadline; ++i) {
        ++iterations;
        const std::uint64_t s0 = gate_session();
        const KilnEngineCallResult r = b.run(c);
        const bool latched = gate_calls_latched();
        const std::uint64_t s2 = gate_session();
        if (r.outcome == kUncertain && r.reason == code(Reason::EngineCallFaulted)) {
            ++faulted;
            if (s0 != s2) ++moved;
            else if (!latched) ++lost;
        } else if (r.outcome == kRefused && r.reason == code(Reason::EngineCallLatched)) {
            ++refused;
        } else {
            ++other;
        }
        gate_invoke_take_note(note);
    }
    stop = true;
    starter.join();
    check(!lost && !other, t + "race: after a fault in an unchanged session calls are latched (" + std::to_string(lost) +
                               " lost, " + std::to_string(other) + " other outcomes)");
    check(faulted > moved && refused > 0, t + "race: faults in an unchanged session and latched refusals both happened");
    check(b.held(), t + "race: the gate stayed held");
    gate_release(b.gate);
    g_mode = static_cast<int>(Mode::Return);
    std::printf("%s invoke race: %ld faulted (%ld of them across a STARTED), %ld refused latched\n", target, faulted, moved,
                refused);
}

// The latch against STARTED alone: a latch recorded in a session is seen in that session; STARTED moves on.
void session_race() {
    gate_reset_for_tests();
    std::atomic<bool> stop{false};
    std::atomic<long> reads{0};
    std::thread starter([&] {
        while (!stop.load()) {
            gate_close_session(false);
            std::this_thread::yield();
        }
    });
    std::thread reader([&] {
        while (!stop.load()) {
            if (gate_calls_latched()) ++reads;
            (void)gate_session();
        }
    });
    long same = 0, moved = 0, lost = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int i = 0; (i < 20000 || same < 1000 || moved < 100) && std::chrono::steady_clock::now() < deadline; ++i) {
        const std::uint64_t s0 = gate_session();
        gate_calls_latch();
        const bool latched = gate_calls_latched();
        const std::uint64_t s2 = gate_session();
        if (s0 != s2) {
            ++moved;
        } else {
            ++same;
            if (!latched) ++lost;
        }
    }
    stop = true;
    starter.join();
    reader.join();
    check(!lost, "session race: a latch is never lost in its own session (" + std::to_string(lost) + ")");
    check(same > 0 && moved > 0, "session race: STARTED both stayed out of and fell inside a latch");
    gate_close_session(false);
    check(!gate_calls_latched(), "session race: the next STARTED clears the last latch");
    std::printf("session race: %ld latches in an unchanged session, %ld across a STARTED, %ld latched reads\n", same,
                moved, reads.load());
}
}  // namespace

namespace kiln::engine {
ImageFacts gather_facts(const KilnApi*) { return {}; }  // engine.cpp is linked for stats(); never booted here
}  // namespace kiln::engine

int main() {
    call::test_call_hook = &hook;
    for (const char* t : {"enshrouded.exe", "enshrouded_server.exe"}) {
        returns(t);
        refusals(t);
        faults(t);
        invoke_race(t);
    }
    session_race();
    // the reasons: one string each, distinct, Kiln's engine-call names
    std::vector<std::string> names;
    for (auto i = code(Reason::EngineCallGateNotHeld); i < code(Reason::Count); ++i) names.push_back(name(i));
    bool distinct = true, styled = true;
    for (std::size_t i = 0; i < names.size(); ++i) {
        styled = styled && (names[i].rfind("engine-call-", 0) == 0 || names[i] == "engine-code-changed");
        for (std::size_t j = i + 1; j < names.size(); ++j) distinct = distinct && names[i] != names[j];
    }
    check(names.size() == 12 && distinct && styled && names.back() == "engine-call-target", "12 distinct engine-call reasons");
    std::printf("engine invoke: %d checks, %d failures\n", checks.load(), failures.load());
    return failures ? 1 : 0;
}
