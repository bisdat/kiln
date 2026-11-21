// SPDX-License-Identifier: MIT
// Stand-in Windows host for kiln-engine.dll (headless Wine; never a game binary): the real DLL reads this process's own
// PE headers, file name and <exe>.kfc header, finds no exact profile, and says so in identity and the rows. The
// internal table's copy runs under the real SEH: guard, no-access and reserved pages fail the copy, not the process.
// Mode "gate-server"/"gate-client" loads kiln-engine-test.dll instead (tests/test_facts_win.cpp): this host names an
// exact profile and a fake engine laid out from it, and the real Interlocked words, SEH writes and the SEH tracker call
// run the Scene gate; the tracker's guard bytes sit on a non-executable page, so the call faults and is contained.
// The gate modes also serve kiln.internal.launch: the owner it designates after kiln.engine's init is never
// latched by the cross-built gate. The plain modes count the Release DLL's lookups of that name (once, at init).
// The gate modes also run the content worker's lifetime guard with the real Interlocked words and SEH copies:
// owned and borrowed, and the worker's page made no-access or a guard page while a guard and its gate are held, so
// validate's guarded copy faults inside SEH and both gate words are still free after the scopes.
// The gate modes also run the engine call inside the gate over real helper code written into the fake image's
// last pages (made executable; the framed helper registered with RtlAddFunctionTable): a call that returns its
// arguments in order and a masked result; an access violation in a leaf and through a framed helper; a C++ exception
// of this host thrown through the framed helper; a byte changed before the call (nothing called); a helper that patches
// its own range; a buffer on the stack; a breakpoint, a single step and a stack overflow passed on to this host's own
// handler; a single step passed on and resumed by this host's own filter (Uncertain all the same); the compare under SEH
// with a range on a no-access or guard page before the call (refused, nothing called) and made no-access by the call
// (Uncertain); and a 50 ms call in flight across a stopping server world and across exit (the bounded 20 us waits timed).
// Each leaves the gate releasable and its words free; each fault latches until the server world's next STARTED.
// The plain modes check that the Release DLL has neither test export (KILN_ENGINE_TEST_RECIPES is compiled out).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "fake_engine.h"
#include "include/kiln_engine_native.h"
#include "profiles.g.h"
#include "services/kiln_caps.h"
#include "services/kiln_engine.h"
#include "../../../core/src/launch_internal.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}
void check(bool ok, const std::string& what) { check(ok, what.c_str()); }
std::map<std::string, const void*> tables;
std::map<std::string, uint32_t> rows;
KilnCapsApi caps{};
KilnResult KILN_CALL report(KilnOwner, KilnStr n, uint32_t s, uint32_t, KilnStr) {
    rows[std::string(n.ptr, n.len)] = s;
    return KILN_OK;
}
int launch_lookups = 0;
const void* KILN_CALL service(KilnStr n, uint32_t) {
    const std::string name(n.ptr, n.len);
    if (name == KILN_CAPS_SERVICE) return &caps;
    if (name == KILN_LAUNCH_INTERNAL_SERVICE) ++launch_lookups;
    const auto at = tables.find(name);
    return at == tables.end() ? nullptr : at->second;
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr n, const void* t) {
    tables[std::string(n.ptr, n.len)] = t;
    return KILN_OK;
}
int g_log_lines = 0;
std::string g_last_log;
void KILN_CALL log(KilnOwner, int32_t, KilnStr t) {
    std::printf("log: %.*s\n", static_cast<int>(t.len), t.ptr);
    ++g_log_lines;
    g_last_log.assign(t.ptr, t.len);
}
KilnWorldEventFn g_world_fn = nullptr;  // kiln.engine's world lifecycle subscription (gate modes)
void* g_world_user = nullptr;
KilnResult KILN_CALL subscribe_world(KilnOwner, KilnWorldEventFn fn, void* user, KilnHandle* out) {
    g_world_fn = fn;
    g_world_user = user;
    *out = KilnHandle{};
    return KILN_OK;
}
void world(uint32_t event) {
    KilnWorldEvent ev{};
    ev.size = sizeof ev;
    ev.event = event;
    ev.kind = KILN_WORLD_SERVER;
    ev.session = 1;
    if (g_world_fn) g_world_fn(&ev, g_world_user);
}
int g_world_thread = -1;
bool KILN_CALL on_world(uint32_t kind) { return g_world_thread >= 0 && kind == static_cast<uint32_t>(g_world_thread); }
KilnResult KILN_CALL build(KilnBuildInfo* out) {
    std::memset(out->exe_sha256, 'a', 64);
    out->exe_sha256[64] = 0;
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}
volatile KilnOwner g_designated = 0;  // what the core's dialect_owner() answers (one thread here)
KilnOwner KILN_CALL designated() { return g_designated; }
const KilnLaunchInternalApi launch_table = {{sizeof(KilnLaunchInternalApi), KILN_LAUNCH_INTERNAL_VERSION}, &designated};
}  // namespace

using TestSet = void(KILN_CALL*)(const char*, const char*, std::uint32_t, std::uint32_t, std::uintptr_t);

// ---- the engine call inside the gate, over real helper code ----
namespace {
// x64 helpers written into the fake image. store8 (leaf): stores its 8 arguments (rcx, rdx, r8, r9, then the 4 stack
// arguments) into the buffer in rcx and returns 0x8877665544332211. leaf_av: reads address 0. framed: calls rcx(rdx)
// in a 0x28-byte frame (prolog "sub rsp, 0x28", registered with RtlAddFunctionTable so exceptions unwind through it).
// poke (leaf): writes the byte dl at the address rcx; its range's 4th byte (a nop) is the one it is pointed at.
// int3 (leaf): a breakpoint.
const std::uint8_t kStore8[] = {0x48, 0x89, 0x09, 0x48, 0x89, 0x51, 0x08, 0x4C, 0x89, 0x41, 0x10, 0x4C, 0x89, 0x49, 0x18,
                                0x48, 0x8B, 0x44, 0x24, 0x28, 0x48, 0x89, 0x41, 0x20, 0x48, 0x8B, 0x44, 0x24, 0x30, 0x48,
                                0x89, 0x41, 0x28, 0x48, 0x8B, 0x44, 0x24, 0x38, 0x48, 0x89, 0x41, 0x30, 0x48, 0x8B, 0x44,
                                0x24, 0x40, 0x48, 0x89, 0x41, 0x38, 0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                0x88, 0xC3};
const std::uint8_t kLeafAv[] = {0x31, 0xC0, 0x48, 0x8B, 0x00, 0xC3};
const std::uint8_t kFramed[] = {0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xC8, 0x48, 0x89, 0xD1,
                                0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x28, 0xC3};
const std::uint8_t kPoke[] = {0x88, 0x11, 0xC3, 0x90};
const std::uint8_t kInt3[] = {0xCC, 0xC3};
// UNWIND_INFO for kFramed: version 1, prolog 4 bytes, one code: at offset 4, UWOP_ALLOC_SMALL of (4 + 1) * 8 = 0x28.
const std::uint8_t kFramedUnwind[] = {0x01, 0x04, 0x01, 0x00, 0x04, 0x42, 0x00, 0x00};
constexpr std::size_t kAtStore8 = 0x000, kAtLeafAv = 0x080, kAtFramed = 0x100, kAtPoke = 0x180, kAtInt3 = 0x1c0,
                      kAtUnwind = 0x200;
enum : std::uint32_t {
    kRecipeStore8 = 1,
    kRecipeLeafAv,
    kRecipeFramed,
    kRecipeFramedAv,
    kRecipePoke,
    kRecipeInt3,
    kRecipeUnreadable,  // leaf_av (never to be called) plus 16 bytes of the page after the helpers
    kRecipeUnprotect,   // the framed helper plus the same 16 bytes, which its callee makes no-access
};
constexpr std::size_t kAtSecondPage = 0x1000;  // the page after the helper page (data, never executed)

// Host functions the framed helper calls (foreign frames between them and kiln.engine's handler).
std::uint64_t throw_cpp(std::uint64_t value) { throw std::runtime_error("a host C++ exception " + std::to_string(value)); }
std::uint64_t raise_code(std::uint64_t code) {
    RaiseException(static_cast<DWORD>(code), 0, 0, nullptr);
    return 0;
}
std::uint64_t make_unreadable(std::uint64_t address) {
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(address), 0x1000, PAGE_NOACCESS, &old);
    return 0;
}
std::atomic<int> g_inside{0};
std::uint64_t spin_ms(std::uint64_t ms) {
    LARGE_INTEGER frequency{}, start{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    g_inside = 1;
    do QueryPerformanceCounter(&now);
    while (static_cast<std::uint64_t>(now.QuadPart - start.QuadPart) * 1000 < ms * static_cast<std::uint64_t>(frequency.QuadPart));
    return 7;
}
double elapsed_us(const LARGE_INTEGER& start) {
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - start.QuadPart) * 1e6 / static_cast<double>(frequency.QuadPart);
}

// The call through this host's own outer handler (only POD in the __try frame): an exception kiln.engine's filter
// passes on lands here.
std::uint32_t g_outer_code = 0;
int outer_filter(unsigned long code) {
    g_outer_code = static_cast<std::uint32_t>(code);
    return EXCEPTION_EXECUTE_HANDLER;
}
int invoke_outer(const KilnEngineNativeApi* n, const kiln::engine::EcsGate* gate, const kiln::engine::KilnEngineCall* call,
                 kiln::engine::KilnEngineCallResult* out, KilnResult* rc) {
    g_outer_code = 0;
    __try {
        *rc = n->ecs_gate_invoke(gate, call, out);
        return 0;
    } __except (outer_filter(GetExceptionCode())) {
        return 1;
    }
}
// The call through an outer filter that resumes what kiln.engine's filter passes on (a debugger or a top-level handler
// that continues after a single step): the call itself then returns.
int resume_filter(unsigned long code) {
    g_outer_code = static_cast<std::uint32_t>(code);
    return EXCEPTION_CONTINUE_EXECUTION;
}
int invoke_resumed(const KilnEngineNativeApi* n, const kiln::engine::EcsGate* gate, const kiln::engine::KilnEngineCall* call,
                   kiln::engine::KilnEngineCallResult* out, KilnResult* rc) {
    g_outer_code = 0;
    __try {
        *rc = n->ecs_gate_invoke(gate, call, out);
        return 0;
    } __except (resume_filter(GetExceptionCode())) {
        return 1;
    }
}
}  // namespace

void engine_calls(HMODULE dll, const KilnEngineNativeApi* n, const kiln::engine::Profile* profile, fake::Engine& e,
                  const kiln::engine::EcsView& view, KilnPluginInfo& info) {
    using namespace kiln::engine;
    const Layout& L = profile->layout;
    using SetRecipes = void(KILN_CALL*)(const CallRecipes*);
    using FilterOf = int(KILN_CALL*)(unsigned long);
    auto set_recipes = reinterpret_cast<SetRecipes>(GetProcAddress(dll, "kiln_engine_test_set_recipes"));
    auto filter_of = reinterpret_cast<FilterOf>(GetProcAddress(dll, "kiln_engine_test_invoke_filter"));
    check(set_recipes && filter_of && n->header.size == 144 && n->ecs_gate_invoke, "engine call: the test exports and the member");
    if (!set_recipes || !filter_of || !n->ecs_gate_invoke) return;
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    const auto reason_is = [](const KilnEngineCallResult& r, Reason why) { return r.reason == static_cast<std::uint32_t>(why); };
    constexpr std::uint32_t returned = static_cast<std::uint32_t>(CallOutcome::Returned);
    constexpr std::uint32_t uncertain = static_cast<std::uint32_t>(CallOutcome::Uncertain);
    constexpr std::uint32_t refused = static_cast<std::uint32_t>(CallOutcome::Refused);
    // the filter: everything is handled at the call's frame except a stack overflow and the debugger's codes
    const auto hex = [](unsigned long code) {
        char text[16];
        std::snprintf(text, sizeof text, "0x%08lx", code);
        return std::string(text);
    };
    for (const unsigned long passes : {static_cast<unsigned long>(EXCEPTION_STACK_OVERFLOW), static_cast<unsigned long>(EXCEPTION_BREAKPOINT),
                                       static_cast<unsigned long>(EXCEPTION_SINGLE_STEP), 0x4000001FUL, 0x4000001EUL})
        check(filter_of(passes) == EXCEPTION_CONTINUE_SEARCH, "engine call: the filter passes on " + hex(passes));
    // an access violation, a C++ exception (MSVC), an integer divide by zero, an illegal instruction, a guard page, and
    // a GCC/Rust-style SEH exception
    for (const unsigned long handled : {0xC0000005UL, 0xE06D7363UL, 0xC0000094UL, 0xC000001DUL, 0x80000001UL, 0x20474343UL})
        check(filter_of(handled) == EXCEPTION_EXECUTE_HANDLER, "engine call: the filter handles " + hex(handled));
    // the helper code: two pages below the fake image's end, executable, the framed helper's unwind data registered
    const std::size_t page_rva = (L.image_size - 0x10000) & ~std::size_t{0xfff};
    const auto& w = L.write;
    check(w.registry_lock < page_rva && w.registry_entries + w.capacity * w.stride < page_rva && w.registry_count < page_rva &&
              w.callback < page_rva && w.tracker + 33 < page_rva && L.resource_write.callback < page_rva,
          "engine call: the helper page is clear of the fake engine's image structures");
    auto* page = reinterpret_cast<std::uint8_t*>(e.image.at(page_rva));
    DWORD old = 0;
    check(VirtualProtect(page, 0x1000, PAGE_EXECUTE_READWRITE, &old) != 0, "engine call: the helper page made executable");
    std::memcpy(page + kAtStore8, kStore8, sizeof kStore8);
    std::memcpy(page + kAtLeafAv, kLeafAv, sizeof kLeafAv);
    std::memcpy(page + kAtFramed, kFramed, sizeof kFramed);
    std::memcpy(page + kAtPoke, kPoke, sizeof kPoke);
    std::memcpy(page + kAtInt3, kInt3, sizeof kInt3);
    std::memcpy(page + kAtUnwind, kFramedUnwind, sizeof kFramedUnwind);
    FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    static RUNTIME_FUNCTION framed_function[1];
    framed_function[0].BeginAddress = static_cast<DWORD>(page_rva + kAtFramed);
    framed_function[0].EndAddress = static_cast<DWORD>(page_rva + kAtFramed + sizeof kFramed);
    framed_function[0].UnwindData = static_cast<DWORD>(page_rva + kAtUnwind);
    check(RtlAddFunctionTable(framed_function, 1, e.image.at()) != 0, "engine call: the framed helper's unwind data registered");
    static CallRecipes recipes{};
    const auto recipe = [&](std::uint32_t id, std::uint32_t args, std::uint32_t bits, std::initializer_list<CallRange> ranges) {
        CallRecipe& r = recipes.recipe[id - 1];
        r = CallRecipe{};
        r.id = id;
        r.arg_count = args;
        r.result_bits = bits;
        for (const CallRange& g : ranges) r.ranges[r.range_count++] = {page_rva + g.rva, g.size};
        return &r;
    };
    CallRecipe* store8 = recipe(kRecipeStore8, 8, 64, {{kAtStore8, sizeof kStore8}});
    store8->arg_bytes[0] = 64;
    recipe(kRecipeLeafAv, 0, 64, {{kAtLeafAv, sizeof kLeafAv}});
    recipe(kRecipeFramed, 2, 64, {{kAtFramed, sizeof kFramed}});
    recipe(kRecipeFramedAv, 2, 64, {{kAtFramed, sizeof kFramed}, {kAtLeafAv, sizeof kLeafAv}});
    recipe(kRecipePoke, 2, 0, {{kAtPoke, sizeof kPoke}});
    recipe(kRecipeInt3, 0, 64, {{kAtInt3, sizeof kInt3}});
    recipe(kRecipeUnreadable, 0, 64, {{kAtLeafAv, sizeof kLeafAv}, {kAtSecondPage, 16}});
    recipe(kRecipeUnprotect, 2, 64, {{kAtFramed, sizeof kFramed}, {kAtSecondPage, 16}});
    std::uint8_t* const second_page = page + kAtSecondPage;
    // what the owning service does before the gate: the live bytes of every range, captured into its own buffer
    std::vector<std::vector<std::uint8_t>> captures(kCallRecipes + 1);
    const auto capture = [&](std::uint32_t id) {
        auto& bytes = captures[id];
        bytes.clear();
        const CallRecipe& r = recipes.recipe[id - 1];
        for (std::uint32_t i = 0; i < r.range_count; ++i) {
            const auto* at = reinterpret_cast<const std::uint8_t*>(e.image.at(r.ranges[i].rva));
            bytes.insert(bytes.end(), at, at + r.ranges[i].size);
        }
    };
    const auto make = [&](std::uint32_t id, std::initializer_list<std::uint64_t> args) {
        KilnEngineCall c{};
        c.size = sizeof c;
        c.recipe = id;
        c.arg_count = static_cast<std::uint32_t>(args.size());
        std::size_t i = 0;
        for (std::uint64_t a : args) c.args[i++] = a;
        capture(id);
        c.code = captures[id].data();
        c.code_bytes = captures[id].size();
        return c;
    };
    const auto fresh = [] {
        KilnEngineCallResult r{};
        r.size = sizeof r;
        return r;
    };
    KilnEngineCall status{};
    status.size = sizeof status;
    status.op = static_cast<std::uint32_t>(CallOp::Status);
    status.recipe = kRecipeStore8;
    KilnEngineCallResult out = fresh();
    check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
              reason_is(out, Reason::EngineCallRecipeUnavailable),
          "engine call: the ported profile has no recipe (Status)");
    set_recipes(&recipes);
    check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_OK, "engine call: with the test recipes, Status OK");
    std::uint64_t step = 600;
    const auto started = [&] {
        world(KILN_WORLD_STARTED);
        KilnEngineCallResult s = fresh();
        return n->ecs_gate_invoke(nullptr, &status, &s) == KILN_OK;
    };
    // (a) returns: the arguments in order (four in registers, four on the stack), the result masked to the recipe's bits
    std::vector<std::uint64_t> buffer(8, 0);
    const auto buf = reinterpret_cast<std::uintptr_t>(buffer.data());
    {
        const KilnEngineCall c = make(kRecipeStore8, {buf, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});  // before the gate
        EcsGateScope scope(*n, view, 1, ++step);
        KilnEngineCallResult r = fresh();
        const KilnResult rc = scope.invoke(c, r);
        check(scope.held() && rc == KILN_OK && r.outcome == returned && r.value == 0x8877665544332211ull && !r.faulted &&
                  buffer[0] == buf && buffer[1] == 0x22 && buffer[2] == 0x33 && buffer[3] == 0x44 && buffer[4] == 0x55 &&
                  buffer[5] == 0x66 && buffer[6] == 0x77 && buffer[7] == 0x88,
              "engine call (a): the helper returns; its 8 arguments arrive in order (registers and stack)");
        store8->result_bits = 16;
        check(scope.invoke(c, r) == KILN_OK && r.value == 0x2211, "engine call (a): 16 result bits masked");
        store8->result_bits = 64;
        check(scope.held() && *fake::word(e.registry_word()) == 1, "engine call (a): the gate stays held");
    }
    check(words_free(), "engine call (a): released");
    // (b) an access violation: in a leaf helper, then through the framed helper
    for (const auto& [id, what] : {std::pair{static_cast<std::uint32_t>(kRecipeLeafAv), "in a leaf helper"},
                                   std::pair{static_cast<std::uint32_t>(kRecipeFramedAv), "through a framed helper"}}) {
        const int logs = g_log_lines;
        {
            const KilnEngineCall c = id == kRecipeLeafAv ? make(id, {}) : make(id, {e.image.at(page_rva + kAtLeafAv), 0});
            EcsGateScope scope(*n, view, 1, ++step);
            KilnEngineCallResult r = fresh();
            const KilnResult rc = scope.invoke(c, r);
            check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == uncertain && r.faulted == 1 &&
                      r.exception == EXCEPTION_ACCESS_VIOLATION && reason_is(r, Reason::EngineCallFaulted) && g_log_lines == logs,
                  std::string("engine call (b): an access violation ") + what + ": caught at the call, Uncertain, the gate held, no log yet");
            check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
                      reason_is(out, Reason::EngineCallLatched) && scope.invoke(c, r) == KILN_ERR_UNAVAILABLE &&
                      reason_is(r, Reason::EngineCallLatched),
                  std::string("engine call (b): latched ") + what);
        }
        check(words_free() && g_log_lines == logs + 1 && g_last_log.find("engine-call-faulted") != std::string::npos &&
                  g_last_log.find("0xc0000005") != std::string::npos,
              std::string("engine call (b): released, one log line after the release ") + what);
        check(started(), std::string("engine call (b): STARTED clears the latch ") + what);
    }
    // (c) a C++ exception of this host thrown through the framed helper: caught at the call, never unwinding into Kiln
    {
        const KilnEngineCall c = make(kRecipeFramed, {reinterpret_cast<std::uintptr_t>(&throw_cpp), 7});
        EcsGateScope scope(*n, view, 1, ++step);
        KilnEngineCallResult r = fresh();
        const KilnResult rc = scope.invoke(c, r);
        check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == uncertain && r.faulted == 1 && r.exception == 0xE06D7363u,
              "engine call (c): a C++ exception through the framed helper: caught at the call (0xE06D7363), the gate held");
    }
    check(words_free() && started(), "engine call (c): released; STARTED clears the latch");
    // (d) a byte changed between the capture and the call: refused, nothing called
    {
        std::fill(buffer.begin(), buffer.end(), 0);
        const KilnEngineCall c = make(kRecipeStore8, {buf, 2, 3, 4, 5, 6, 7, 8});
        page[kAtStore8 + sizeof kStore8 - 2] ^= 0x01;  // the result's top byte, after the capture
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
        EcsGateScope scope(*n, view, 1, ++step);
        KilnEngineCallResult r = fresh();
        const KilnResult rc = scope.invoke(c, r);
        check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == refused && reason_is(r, Reason::EngineCodeChanged) &&
                  buffer[0] == 0 && buffer[7] == 0,
              "engine call (d): code changed before the call: engine-code-changed, the helper not called");
        page[kAtStore8 + sizeof kStore8 - 2] ^= 0x01;
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
        check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_OK, "engine call (d): nothing latched");
    }
    check(words_free(), "engine call (d): released");
    // (e) the gate's image is not the loaded game image (the test facts name another one during the hold):
    // refused engine-call-target before the arguments, the helper not called, nothing latched
    {
        using TestSet = void(KILN_CALL*)(const char*, const char*, std::uint32_t, std::uint32_t, std::uintptr_t);
        auto set_facts = reinterpret_cast<TestSet>(GetProcAddress(dll, "kiln_engine_test_set"));
        check(set_facts != nullptr, "engine call (e): the test facts export");
        const auto loaded = [&](std::uintptr_t image) {
            if (set_facts) set_facts(profile->target, profile->sha256, profile->timestamp, profile->size_of_image, image);
        };
        std::fill(buffer.begin(), buffer.end(), 0);
        const KilnEngineCall c = make(kRecipeStore8, {buf, 2, 3, 4, 5, 6, 7, 8});
        EcsGateScope scope(*n, view, 1, ++step);
        for (const std::uintptr_t other : {e.image.at() + 0x1000, std::uintptr_t{0}}) {
            loaded(other);
            KilnEngineCallResult r = fresh();
            const KilnResult rc = scope.invoke(c, r);
            KilnEngineCall wrong = c;
            wrong.arg_count = 7;
            KilnEngineCallResult w2 = fresh();
            const KilnResult rc2 = scope.invoke(wrong, w2);
            loaded(e.image.at());
            check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == refused && reason_is(r, Reason::EngineCallTarget) &&
                      rc2 == KILN_ERR_UNAVAILABLE && reason_is(w2, Reason::EngineCallTarget) && buffer[0] == 0 && buffer[7] == 0,
                  "engine call (e): the gate's image is not the loaded image: engine-call-target (before the arguments), the helper not called");
            check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_OK, "engine call (e): nothing latched");
        }
        KilnEngineCallResult r = fresh();
        check(scope.invoke(c, r) == KILN_OK && r.outcome == returned && buffer[1] == 2 && buffer[7] == 8,
              "engine call (e): the loaded image again: the helper returns");
    }
    check(words_free(), "engine call (e): released");
    // (d') a helper that patches its own range during the call: Uncertain, latched
    {
        const KilnEngineCall c = make(kRecipePoke, {e.image.at(page_rva + kAtPoke + 3), 0xCC});
        EcsGateScope scope(*n, view, 1, ++step);
        KilnEngineCallResult r = fresh();
        const KilnResult rc = scope.invoke(c, r);
        check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == uncertain && !r.faulted &&
                  reason_is(r, Reason::EngineCallCodeChangedDuringCall) && page[kAtPoke + 3] == 0xCC,
              "engine call (d'): a helper patched its own range: engine-call-code-changed-during-call");
        page[kAtPoke + 3] = 0x90;
        FlushInstructionCache(GetCurrentProcess(), page, 0x1000);
    }
    check(words_free() && started(), "engine call (d'): released; STARTED clears the latch");
    // a buffer on this thread's stack: refused, nothing called
    {
        std::uint64_t local[8]{};
        const KilnEngineCall c = make(kRecipeStore8, {reinterpret_cast<std::uintptr_t>(local), 2, 3, 4, 5, 6, 7, 8});
        EcsGateScope scope(*n, view, 1, ++step);
        KilnEngineCallResult r = fresh();
        check(scope.invoke(c, r) == KILN_ERR_UNAVAILABLE && reason_is(r, Reason::EngineCallArgumentOnStack) && local[1] == 0,
              "engine call: an argument buffer on the stack (GetCurrentThreadStackLimits): refused, nothing called");
    }
    check(words_free(), "engine call: released after the stack refusal");
    // a breakpoint, a single step and a stack overflow go on to this host's own handler; the call is latched anyway
    struct Passed {
        std::uint32_t id;
        std::uint64_t arg0, arg1;
        std::uint32_t code;
        const char* what;
    };
    for (const Passed& p : {Passed{kRecipeInt3, 0, 0, static_cast<std::uint32_t>(EXCEPTION_BREAKPOINT), "a breakpoint (int3)"},
                            Passed{kRecipeFramed, reinterpret_cast<std::uintptr_t>(&raise_code), EXCEPTION_SINGLE_STEP,
                                   static_cast<std::uint32_t>(EXCEPTION_SINGLE_STEP), "a single step"},
                            Passed{kRecipeFramed, reinterpret_cast<std::uintptr_t>(&raise_code), EXCEPTION_STACK_OVERFLOW,
                                   static_cast<std::uint32_t>(EXCEPTION_STACK_OVERFLOW), "a stack overflow"}}) {
        const int logs = g_log_lines;
        {
            const KilnEngineCall c = p.id == kRecipeInt3 ? make(p.id, {}) : make(p.id, {p.arg0, p.arg1});
            EcsGateScope scope(*n, view, 1, ++step);
            KilnEngineCallResult r = fresh();
            KilnResult rc = KILN_OK;
            const int caught = invoke_outer(n, &scope.gate(), &c, &r, &rc);
            check(caught == 1 && g_outer_code == p.code && scope.held(),
                  std::string("engine call: ") + p.what + " goes on to the outer handler; the gate stays held for its scope");
            check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE && reason_is(out, Reason::EngineCallLatched),
                  std::string("engine call: ") + p.what + " passed on still latches engine calls");
        }
        check(words_free() && g_log_lines == logs + 1, std::string("engine call: ") + p.what + ": released, logged once");
        check(started(), std::string("engine call: ") + p.what + ": STARTED clears the latch");
    }
    // a single step passed on and resumed further out: the call returns, but an exception reached it: Uncertain
    {
        const int logs = g_log_lines;
        {
            const KilnEngineCall c = make(kRecipeFramed, {reinterpret_cast<std::uintptr_t>(&raise_code), EXCEPTION_SINGLE_STEP});
            EcsGateScope scope(*n, view, 1, ++step);
            KilnEngineCallResult r = fresh();
            KilnResult rc = KILN_OK;
            const int caught = invoke_resumed(n, &scope.gate(), &c, &r, &rc);
            check(caught == 0 && g_outer_code == EXCEPTION_SINGLE_STEP && scope.held() && rc == KILN_ERR_UNAVAILABLE &&
                      r.outcome == uncertain && !r.faulted && r.exception == EXCEPTION_SINGLE_STEP &&
                      reason_is(r, Reason::EngineCallFaulted) && g_log_lines == logs,
                  "engine call: a single step passed on and resumed by an outer filter: the call returned, answered Uncertain (not "
                  "faulted, the code kept), no log inside the hold");
        }
        check(words_free() && g_log_lines == logs + 1 && g_last_log.find("0x80000004") != std::string::npos,
              "engine call: the resumed single step: released, logged once");
        check(n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE && reason_is(out, Reason::EngineCallLatched) &&
                  started(),
              "engine call: the resumed single step latched; STARTED clears it");
    }
    // the compare under SEH before the call: a range on a no-access page, then on a guard page: refused, nothing called
    // (the entry is the faulting leaf, so a call would answer Uncertain), nothing latched
    for (const auto& [protection, what] : {std::pair{static_cast<DWORD>(PAGE_NOACCESS), "a no-access page"},
                                           std::pair{static_cast<DWORD>(PAGE_READWRITE | PAGE_GUARD), "a guard page"}}) {
        const KilnEngineCall c = make(kRecipeUnreadable, {});  // captured while readable
        DWORD was = 0;
        check(VirtualProtect(second_page, 0x1000, protection, &was) != 0, std::string("engine call: a range's page made ") + what);
        {
            EcsGateScope scope(*n, view, 1, ++step);
            KilnEngineCallResult r = fresh();
            const KilnResult rc = scope.invoke(c, r);
            check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == refused && !r.faulted &&
                      reason_is(r, Reason::EngineCodeChanged),
                  std::string("engine call: a range on ") + what + " before the call: the SEH compare fails, engine-code-changed, "
                                                           "nothing called");
        }
        VirtualProtect(second_page, 0x1000, PAGE_READWRITE, &was);
        check(words_free() && n->ecs_gate_invoke(nullptr, &status, &out) == KILN_OK,
              std::string("engine call: a range on ") + what + ": released, nothing latched");
    }
    // the compare under SEH after the call: the call makes a range's page no-access: Uncertain, latched
    {
        const int logs = g_log_lines;
        {
            const KilnEngineCall c = make(kRecipeUnprotect, {reinterpret_cast<std::uintptr_t>(&make_unreadable),
                                                             reinterpret_cast<std::uintptr_t>(second_page)});
            EcsGateScope scope(*n, view, 1, ++step);
            KilnEngineCallResult r = fresh();
            const KilnResult rc = scope.invoke(c, r);
            check(scope.held() && rc == KILN_ERR_UNAVAILABLE && r.outcome == uncertain && !r.faulted &&
                      reason_is(r, Reason::EngineCallCodeChangedDuringCall) && g_log_lines == logs,
                  "engine call: a range made no-access by the call: the SEH compare after it fails, "
                  "engine-call-code-changed-during-call");
        }
        DWORD was = 0;
        VirtualProtect(second_page, 0x1000, PAGE_READWRITE, &was);
        check(words_free() && g_log_lines == logs + 1 && n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
                  reason_is(out, Reason::EngineCallLatched) && started(),
              "engine call: the range made no-access: released, logged once, latched; STARTED clears it");
    }
    // a 50 ms call in flight when the server world stops, then at exit: the bounded waits, timed
    const auto in_flight = [&](auto&& event, const char* warning, const char* what) {
        g_inside = 0;
        KilnResult rc = KILN_ERR_INTERNAL;
        KilnEngineCallResult r = fresh();
        bool held = false;
        const KilnEngineCall c = make(kRecipeFramed, {reinterpret_cast<std::uintptr_t>(&spin_ms), 50});
        const std::uint64_t holder_step = ++step;
        std::thread holder([&] {
            EcsGateScope scope(*n, view, 1, holder_step);
            held = scope.held();
            rc = scope.invoke(c, r);
            g_inside = 1;  // also when nothing was called, so the wait below always ends
        });
        while (!g_inside.load()) SwitchToThread();
        const int logs = g_log_lines;
        LARGE_INTEGER start{};
        QueryPerformanceCounter(&start);
        event();
        const double us = elapsed_us(start);
        holder.join();
        std::printf("engine call: %s with a 50 ms call in flight returned after %.1f us\n", what, us);
        check(held && us < 10000.0 && g_log_lines == logs + 1 && g_last_log.find(warning) != std::string::npos,
              std::string("engine call: ") + what + " with a call in flight: a bounded wait (20 us), not the call's 50 ms; said so");
        check(rc == KILN_OK && r.outcome == returned && r.value == 7,
              std::string("engine call: ") + what + ": the call in flight finished and returned");
        EcsGateScope after(*n, view, 1, ++step);
        check(!after.held() && after.reason() == Reason::KilnGateClosed && words_free(),
              std::string("engine call: ") + what + ": the gate released; the next acquire is refused kiln-gate-closed");
    };
    in_flight([] { world(KILN_WORLD_STOPPING); }, "at a stopping server world", "STOPPING");
    world(KILN_WORLD_STARTED);
    {
        EcsGateScope reopened(*n, view, 1, ++step);
        check(reopened.held(), "engine call: the next STARTED reopens the gate");
    }
    check(info.on_unload != nullptr, "engine call: an exit handler");
    if (info.on_unload) in_flight([&] { info.on_unload(info.user); }, "at exit", "exit");  // last: exit closes for good
    set_recipes(nullptr);
    RtlDeleteFunctionTable(framed_function);
    check(words_free(), "engine call: both words free at the end");
}

int gate_mode(const wchar_t* dll_path, bool server) {
    using namespace kiln::engine;
    const Profile* profile = nullptr;
    for (const auto& p : generated::kProfiles)
        if ((std::strcmp(p.target, "enshrouded_server.exe") == 0) == server) profile = &p;
    check(profile != nullptr, "a ported profile for this side");
    if (!profile) return 1;
    const Layout& L = profile->layout;
    fake::Engine e(L);
    const HMODULE dll = LoadLibraryW(dll_path);
    check(dll != nullptr, "kiln-engine-test.dll loads");
    if (!dll) return 1;
    auto set = reinterpret_cast<TestSet>(GetProcAddress(dll, "kiln_engine_test_set"));
    auto main_fn = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(dll, "kiln_plugin_main"));
    check(set && main_fn, "test exports");
    if (!set || !main_fn) return 1;
    set(profile->target, profile->sha256, profile->timestamp, profile->size_of_image, e.image.at());
    caps.header = {sizeof caps, KILN_CAPS_VERSION};
    caps.report = &report;
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &service;
    api.register_service = &publish;
    api.log = &log;
    api.build_info = &build;
    api.on_world_thread = &on_world;
    api.subscribe_world = &subscribe_world;
    tables[KILN_LAUNCH_INTERNAL_SERVICE] = &launch_table;  // nobody designated yet, as at kiln.engine's init
    KilnPluginInfo info{};
    info.size = sizeof info;
    check(main_fn(&api, 5, &info) == KILN_OK, "kiln_plugin_main OK");
    check(launch_lookups == 1, "kiln.internal.launch looked up once, at init");
    const auto* n = static_cast<const KilnEngineNativeApi*>(tables["kiln.internal.engine"]);
    check(n && n->header.size == sizeof(KilnEngineNativeApi) && n->profile() && std::strcmp(n->profile()->id, profile->id) == 0,
          "exact profile selected for the fake engine");
    if (!n || !n->profile()) return 1;
    const auto words_free = [&] { return *fake::word(e.registry_word()) == 0 && *fake::word(e.scene_word()) == 0; };
    EcsView view{};
    std::uint32_t reason = 0;
    check(n->ecs_view(0, &view, &reason) == KILN_OK && view.role == Role::LocalAuthority, "authority view (real SEH reads)");
    {
        g_world_thread = KILN_WORLD_SERVER;
        EcsGateScope in_frame(*n, view, 1, 100);
        check(!in_frame.held() && in_frame.reason() == Reason::InFrame && words_free(), "in-frame refused, words untouched");
        g_world_thread = -1;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    {
        EcsGateScope scope(*n, view, 1, 101);
        check(scope.held() && *fake::word(e.registry_word()) == 1 &&
                  *fake::word(e.scene_word()) == static_cast<std::int32_t>(0x80000000u),
              "acquired with the real Interlocked words");
        check(GetThreadPriority(GetCurrentThread()) >= THREAD_PRIORITY_ABOVE_NORMAL,
              "the holder is raised above normal (Wine/Proton may ignore this; to be confirmed on Windows)");
        check(scope.validate() && scope.component_supported(3), "validate and component policy");
        const std::uint64_t payload = 0x0102030405060708ull;
        check(scope.write(e.table.at(512), &payload, sizeof payload) == KILN_OK &&
                  fake::get<std::uint64_t>(e.table.at(512)) == payload,
              "guarded write");
        std::vector<unsigned char> big(n->profile()->layout.write.maximum_bytes + 1, 0xcd);
        check(scope.write(e.table.at(512), big.data(), big.size()) == KILN_ERR_BUFFER_TOO_SMALL &&
                  fake::get<std::uint64_t>(e.table.at(512)) == payload,
              "a write over the budget refuses with BUFFER_TOO_SMALL and changes nothing");
        check(scope.write(fake::unmapped(), &payload, sizeof payload) == KILN_ERR_UNAVAILABLE && scope.held(),
              "a write to a no-access page faults under SEH; the gate stays held");
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(reinterpret_cast<void*>(e.image.at(L.write.tracker)), &mbi, sizeof mbi);
        const bool executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        check(!executable, "the fake tracker's page is not executable");
        if (!executable)
            check(!scope.track_change(0x5000, 3) && scope.held() && scope.validate(),
                  "the tracker call faults (no execute) and is contained; the gate stays held and valid");
    }
    check(words_free() && GetThreadPriority(GetCurrentThread()) == THREAD_PRIORITY_BELOW_NORMAL,
          "the scope released both words and restored the thread priority after the faults");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
    KilnEngineGateStats st{};
    st.size = sizeof st;
    const auto* t = static_cast<const KilnEngineApi*>(tables[KILN_ENGINE_SERVICE]);
    check(t && t->gate_stats(&st) == KILN_OK && st.acquired == 1 && st.refused_in_frame == 1 && st.hold_ns_max > 0,
          "gate stats: one hold, one in-frame refusal, a hold time");
    // The dialect owner: the core designates owner 9 after kiln.engine's init; in one step, behind another Kiln holder, owner 9 is
    // refused kiln-gate-busy (after its retries) and admitted again, while owner 2 is latched
    g_designated = 9;
    {
        EcsGateScope holder(*n, view, 1, 300);
        EcsGateScope dialect(*n, view, 9, 300);
        EcsGateScope other(*n, view, 2, 300);
        check(holder.held() && !dialect.held() && dialect.reason() == Reason::KilnHolderBusy && !other.held() &&
                  other.reason() == Reason::KilnHolderBusy &&
                  dialect.gate().admission == static_cast<std::uint32_t>(GateAdmission::KilnMod),
              "behind another Kiln holder: the designated owner and another owner are busy");
    }
    {
        EcsGateScope dialect(*n, view, 9, 300);
        check(dialect.held(), "the designated owner is admitted again in the same step (not latched)");
    }
    {
        EcsGateScope other(*n, view, 2, 300);
        check(!other.held() && other.reason() == Reason::KilnBusyLatched,
              "another owner in the same step gets kiln-gate-busy-this-step");
    }
    check(words_free() && launch_lookups == 1, "words free; kiln.internal.launch was never looked up again");
    // The content worker's lifetime guard
    e.add_content_worker();
    {
        ResourceGuardScope owned(*n, e.service(), e.reader());
        check(owned.held() && owned.guard().owns_registry == 1 && *fake::word(e.registry_word()) == 1 && owned.validate() &&
                  owned.contains(e.payload.at(), e.payload.size),
              "resource guard: an owned guard holds the registry word Shared (real Interlocked), validates, contains its payload");
    }
    check(words_free(), "resource guard: the owned guard released its share");
    {
        g_world_thread = KILN_WORLD_SERVER;
        ResourceGuardScope in_frame(*n, e.service(), e.reader());
        check(!in_frame.held() && in_frame.reason() == Reason::InFrame && words_free(),
              "resource guard: an owned guard in-frame is refused, words untouched");
        g_world_thread = -1;
    }
    DWORD old_protect = 0;
    const auto protect = [&](DWORD how) {
        return VirtualProtect(reinterpret_cast<void*>(e.service()), 4096, how, &old_protect) != 0;
    };
    {
        EcsGateScope gate(*n, view, 1, 400);
        ResourceGuardScope guard(*n, gate, e.service(), e.reader());
        check(gate.held() && guard.held() && guard.validate() && guard.guard().owns_registry == 0 &&
                  *fake::word(e.registry_word()) == 1,
              "resource guard: gate, then a borrowed guard: only the gate's registry share");
        check(protect(PAGE_NOACCESS) && !guard.validate() && guard.held() && gate.held(),
              "resource guard: the worker's page made no-access: validate's guarded copy faults inside SEH; both holds stay");
        check(protect(PAGE_READWRITE) && guard.validate(), "resource guard: readable again: the guard validates");
        check(protect(PAGE_READWRITE | PAGE_GUARD) && !guard.validate() && guard.held(),
              "resource guard: the worker's page made a guard page: validate's first touch faults inside SEH");
        std::printf("resource guard: after the guard-page fault validate %s (Windows clears PAGE_GUARD on first touch)\n",
                    guard.validate() ? "passes" : "still fails");
        protect(PAGE_READWRITE);
    }
    check(words_free(), "resource guard: the borrowed guard, then the gate released: both words free after the faults");
    {
        check(protect(PAGE_NOACCESS), "resource guard: the worker's page no-access before acquiring");
        ResourceGuardScope owned(*n, e.service(), e.reader());
        check(!owned.held() && owned.reason() == Reason::EcsThreadNameUnreadable && words_free(),
              "resource guard: an owned guard over a no-access worker: refused (its name read faults), its own share released");
        EcsGateScope gate(*n, view, 1, 401);
        ResourceGuardScope borrowed(*n, gate, e.service(), e.reader());
        check(gate.held() && !borrowed.held() && borrowed.reason() == Reason::EcsThreadNameUnreadable &&
                  *fake::word(e.registry_word()) == 1,
              "resource guard: a borrowed guard over a no-access worker: refused, the gate's share kept");
        protect(PAGE_READWRITE);
    }
    check(words_free(), "resource guard: both words free at the end");
    engine_calls(dll, n, profile, e, view, info);  // last: it ends with exit, which closes the gate for good
    std::printf("engine Windows gate (%s): %d failure(s), max hold %llu ns\n", profile->id, failures,
                static_cast<unsigned long long>(st.hold_ns_max));
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    if (std::wstring(argv[2]) == L"gate-server" || std::wstring(argv[2]) == L"gate-client")
        return gate_mode(argv[1], std::wstring(argv[2]) == L"gate-server");
    const std::wstring kfc_mode = argv[2];  // "kfc4": write <exe>.kfc with KFC4 first; "none": remove it
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring kfc = self;
    kfc.replace(kfc.size() - 4, 4, L".kfc");
    DeleteFileW(kfc.c_str());
    if (kfc_mode == L"kfc4") {
        const HANDLE f = CreateFileW(kfc.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD n = 0;
        WriteFile(f, "KFC4rest", 8, &n, nullptr);
        CloseHandle(f);
    }
    const HMODULE dll = LoadLibraryW(argv[1]);
    check(dll != nullptr, "kiln-engine.dll loads");
    if (!dll) return 1;
    auto main_fn = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(dll, "kiln_plugin_main"));
    check(main_fn != nullptr, "kiln_plugin_main export");
    caps.header = {sizeof caps, KILN_CAPS_VERSION};
    caps.report = &report;
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &service;
    api.register_service = &publish;
    api.log = &log;
    api.build_info = &build;
    KilnPluginInfo info{};
    info.size = sizeof info;
    check(main_fn && main_fn(&api, 5, &info) == KILN_OK, "kiln_plugin_main OK on an unknown build");
    check(launch_lookups == 1, "the Release kiln-engine.dll asks for kiln.internal.launch once, at init (none here)");
    const auto* t = static_cast<const KilnEngineApi*>(service({KILN_ENGINE_SERVICE, 11}, 1));
    check(t && tables.count("kiln.internal.engine"), "public and internal tables published");
    KilnEngineIdentity id{};
    id.size = sizeof id;
    check(t && t->identity(&id) == KILN_OK, "identity");
    const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    std::printf("identity: status=%u target=%s timestamp=%u size=%u kfc=%u sha=%.8s... profile='%s'\n", id.status, id.target,
                id.timestamp, id.size_of_image, id.archive_format, id.sha256, id.profile_id);
    check(id.status == KILN_ENGINE_UNKNOWN_BUILD && !id.profile_id[0], "no exact profile for a stand-in exe");
    check(id.timestamp == nt->FileHeader.TimeDateStamp && id.size_of_image == nt->OptionalHeader.SizeOfImage,
          "TimeDateStamp and SizeOfImage come from the loaded image");
    check(std::strcmp(id.target, "engine-host.exe") == 0, "target is the exe file name");
    check(id.archive_format == (kfc_mode == L"kfc4" ? 4u : 0u), "KFC header read from <exe>.kfc");
    check(rows["engine.profile"] == KILN_CAP_UNAVAILABLE && rows["engine.ecs_write"] == KILN_CAP_UNAVAILABLE, "rows unavailable");
    const auto* n = static_cast<const KilnEngineNativeApi*>(tables["kiln.internal.engine"]);
    check(n && n->header.size == sizeof(KilnEngineNativeApi) && !n->layout() && !n->profile(), "internal table, no profile");
    check(!GetProcAddress(dll, "kiln_engine_test_set_recipes") && !GetProcAddress(dll, "kiln_engine_test_invoke_filter") &&
              !GetProcAddress(dll, "kiln_engine_test_set"),
          "the Release DLL has no test export (the engine-call recipe seam is compiled out)");
    if (n) {
        kiln::engine::KilnEngineCall status{};
        status.size = sizeof status;
        status.op = static_cast<std::uint32_t>(kiln::engine::CallOp::Status);
        status.recipe = 1;
        kiln::engine::KilnEngineCallResult out{};
        out.size = sizeof out;
        check(n->header.size == 144 && n->ecs_gate_invoke(nullptr, &status, &out) == KILN_ERR_UNAVAILABLE &&
                  out.reason == static_cast<std::uint32_t>(kiln::engine::Reason::EngineCallRecipeUnavailable),
              "engine call: no engine call without an exact profile (Status: engine-call-recipe-unavailable)");
    }
    if (n) {
        unsigned long long value = 0x0123456789abcdefull, out = 0;
        check(n->copy(reinterpret_cast<std::uintptr_t>(&value), &out, sizeof out) == KILN_OK && out == value, "copy reads");
        auto* guard = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD));
        auto* none = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS));
        auto* reserved = static_cast<unsigned char*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS));
        auto* pair = static_cast<unsigned char*>(VirtualAlloc(nullptr, 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        DWORD old = 0;
        check(guard && none && reserved && pair && VirtualProtect(pair + 4096, 4096, PAGE_NOACCESS, &old), "test pages");
        const auto at = [](const void* p) { return reinterpret_cast<std::uintptr_t>(p); };
        check(n->copy(at(guard), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a guard-page fault fails the copy");
        std::printf("guard page after the fault: copy %s (Windows clears PAGE_GUARD on first touch)\n",
                    n->copy(at(guard), &out, sizeof out) == KILN_OK ? "reads" : "still fails");
        check(n->copy(at(none), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a no-access page fails the copy");
        check(n->copy(at(reserved), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a reserved page fails the copy");
        check(n->copy(at(pair + 4092), &out, sizeof out) == KILN_ERR_UNAVAILABLE, "a range running into a no-access page fails");
        check(n->copy(at(pair + 4088), &out, sizeof out) == KILN_OK, "the readable part still reads");
        kiln::engine::EcsView view{};
        std::uint32_t reason = 0;
        check(n->ecs_view(0, &view, &reason) == KILN_ERR_UNAVAILABLE &&
                  reason == static_cast<std::uint32_t>(kiln::engine::Reason::EcsReadOwnerProfileUnavailable) &&
                  n->ecs_write_supported(&view, 3) == 0,
              "no view and no write support without an exact profile");
    }
    DeleteFileW(kfc.c_str());
    std::printf("engine Windows host: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
