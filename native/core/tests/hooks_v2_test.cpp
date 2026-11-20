// SPDX-License-Identifier: MIT
// kiln.hooks v2 under Wine, no game: the real service (hooks.cpp), registry engine (hooks_chain.cpp), static machine
// code (hooks_chain_asm.cpp) and SEH helpers, with the core's logging, sigdb and owner registry replaced by stubs.
//
// The "game" is the exe's own `.game` section: compiled functions with the signatures the tests need, and a few
// hand-assembled ones whose instruction boundaries are known (the stand-in foreign hooker and the mid hooks need
// them). The exe's other code (the handlers, the foreign hooker) plays "another module".
//
//   native/core/tests/test-hooks-v2.sh            (builds hooks_v2_test.exe, runs it twice: caller-table slot < 64
//                                                  and >= 64 [TlsExpansionSlots])
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "kiln_hook.hpp"
#include "common.h"
#include "image.h"
#include "log.h"
#include "nlohmann/json.hpp"
#include "registry.h"
#include "services/hooks_chain.h"
#include "services/hooks_code.h"
#include "services/hooks_internal.h"
#include "services/kiln_hooks.h"
#include "services/service.h"
#include "sigdb.h"
#include "tick.h"

using json = nlohmann::json;
namespace chain = kiln::hooks::chain;

// ---------------------------------------------------------------------------------------------------------------------
// Stubs for the rest of kiln-core
namespace {
std::mutex g_log_mutex;
std::vector<std::string> g_lines;
std::map<uintptr_t, std::vector<uint8_t>> g_file;  // "the exe file's" first bytes of functions a foreign hooker patched
// test_code_wait_during_stop: a one-shot pause inside a first install, at its read of the file's bytes (target_for does
// that after the install took code_state's lock and before it stops the other threads).
std::atomic<uintptr_t> g_pause_at{0};      // the target whose file read is watched
std::atomic<DWORD> g_pause_installer{0};   // the installing thread: it runs g_pause_fn once, at that read
std::atomic<DWORD> g_pause_reader{0};      // a code_state caller: its read of the file's bytes is noted
std::atomic<bool> g_pause_reader_read{false};
std::function<void()> g_pause_fn;
// A thread watched at kiln.hooks' code lock: services/hooks_code.h calls test_lock_probe() before lock() takes the
// mutex (KILN_HOOKS_TEST_LOCK_PROBE, this test's build only). A test that holds the lock then knows the caller is at it.
std::atomic<DWORD> g_lock_watch{0};
std::atomic<bool> g_lock_reached{false};
// Texts written to state/hooks.json: the map timer may write the file at any time after boot, also while the process
// exits, so the list and its lock are never destroyed.
std::mutex& g_map_writes_mutex = *new std::mutex;
std::vector<std::string>& g_map_writes = *new std::vector<std::string>;
// The bound of a wait that must end. Generous: the shared build machine can be loaded far beyond its cores, and a
// passing wait returns as soon as its condition holds. test-hooks-v2.sh's own limit per run ends a run sooner.
constexpr std::chrono::seconds kMustEnd{300};
constexpr DWORD kMustEndMs = 300000;
template <class F>
bool until(F done) {
    const auto give_up = std::chrono::steady_clock::now() + kMustEnd;
    while (!done()) {
        if (std::chrono::steady_clock::now() > give_up) return false;
        Sleep(0);
    }
    return true;
}
void file_read_noted() {
    const DWORD me = GetCurrentThreadId();
    if (me == g_pause_reader.load()) g_pause_reader_read = true;  // its next step is the lock
    else if (DWORD installer = me; g_pause_installer.compare_exchange_strong(installer, 0) && g_pause_fn) g_pause_fn();
}
std::vector<kiln::sigdb::LiveDiff> g_live_diffs;
std::filesystem::path g_state_dir;
kiln::svc::Service* g_service = nullptr;
std::map<KilnOwner, std::string> g_owner_names = {{0, "kiln"},
                                                  {1, "service:kiln-chat.dll"},
                                                  {2, "plugin:kiln-emf.dll"},
                                                  {3, "emf:coolmod"},
                                                  {4, "native:alpha"},
                                                  {5, "native:beta"},
                                                  {6, "test"},
                                                  {7, "shroudtopia:modx"},
                                                  {8, "emf:mody"}};
}  // namespace

namespace kiln {
ImageInfo g_image;
const ImageInfo& image() { return g_image; }
const Paths& paths() {
    static Paths p;
    p.state = g_state_dir;
    if (p.game.empty()) {  // as init_paths: the test exe's folder plays the game folder
        wchar_t exe[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        p.game = std::filesystem::path(std::wstring(exe, n)).parent_path();
    }
    return p;
}
std::string to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    std::string s(static_cast<size_t>(WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                                          nullptr, nullptr)),
                  0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), static_cast<int>(s.size()), nullptr,
                        nullptr);
    return s;
}
std::wstring to_wide(std::string_view s) {
    if (s.empty()) return {};
    std::wstring w(static_cast<size_t>(MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0)),
                   0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), static_cast<int>(w.size()));
    return w;
}
double now_ms() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}
std::string utc_now() { return "2026-01-01T00:00:00Z"; }
bool write_file_atomic(const std::filesystem::path& file, std::string_view text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (file.filename() == L"hooks.json") {
        std::lock_guard lock(g_map_writes_mutex);
        g_map_writes.emplace_back(text);
    }
    return static_cast<bool>(out);
}
namespace log {
void writef(int level, const char*, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_log_mutex);
    g_lines.push_back(std::string(level == KILN_LOG_ERROR ? "E " : level == KILN_LOG_WARN ? "W " : "I ") + buf);
}
}  // namespace log
namespace reg {
std::string owner_name(KilnOwner o) {
    auto it = g_owner_names.find(o);
    return it == g_owner_names.end() ? "(revoked)" : it->second;
}
KilnResult add_owner_retiring(KilnOwner, OwnerRetiringFn, void*, KilnHandle*) { return KILN_ERR_UNAVAILABLE; }
KilnResult remove_owner_retiring(KilnHandle) { return KILN_ERR_NOT_FOUND; }
}  // namespace reg
namespace tick {
bool typed_install_pre() { return false; }
KilnResult session(KilnSessionInfo* out) { *out = {}; out->size = sizeof *out; return KILN_OK; }
KilnResult subscribe_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle*) { return KILN_ERR_UNAVAILABLE; }
}  // namespace tick
namespace sigdb {
bool any_symbol(std::string_view, uintptr_t*) { return false; }
bool any_constant(std::string_view, int64_t*) { return false; }
std::vector<uintptr_t> scan_code(const Pattern&, size_t) { return {}; }
bool hole_value_at(const Pattern&, uintptr_t, std::string_view, int64_t*) { return false; }
bool load_fragment(std::string_view, const std::filesystem::path&, const std::filesystem::path&, FragmentSummary*,
                   std::string*) {
    return false;
}
bool unload_fragment(std::string_view) { return false; }
bool file_bytes(uintptr_t a, uint8_t* out, size_t n) {
    bool found = false;
    for (auto& [at, bytes] : g_file)
        if (a >= at && a + n <= at + bytes.size()) {
            std::memcpy(out, bytes.data() + (a - at), n);
            found = true;
            break;
        }
    // "the file" equals the live code unless a hooker patched it
    if (!found) std::memcpy(out, reinterpret_cast<const void*>(a), n);
    if (a == g_pause_at.load()) file_read_noted();
    return true;
}
std::atomic<int> g_sym_calls{0};
std::string symbol_name_at(uintptr_t) {
    ++g_sym_calls;
    return {};
}
std::vector<LiveDiff> live_differs() { return g_live_diffs; }
uint64_t sig_revision() { return 1; }
bool sig(std::string_view, SigInfo*) { return false; }
bool sig_status(std::string_view, uintptr_t*, SigStatus*, std::string*) { return false; }
bool sig_at(uintptr_t, SigInfo*) { return false; }
bool fingerprint(uintptr_t, hooks::fp::Fingerprint*) { return false; }
}  // namespace sigdb
namespace svc {
Registrar::Registrar(Service* s) { g_service = s; }
}  // namespace svc
namespace hooks::code {
void test_lock_probe() {
    if (GetCurrentThreadId() == g_lock_watch.load()) g_lock_reached = true;
}
}  // namespace hooks::code
}  // namespace kiln

// ---------------------------------------------------------------------------------------------------------------------
namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const char* what, const std::string& detail = "") {
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
    ok ? ++g_pass : ++g_fail;
}

bool log_has(const std::string& needle) {
    std::lock_guard lock(g_log_mutex);
    for (const auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}
std::string log_line(const std::string& needle) {
    std::lock_guard lock(g_log_mutex);
    for (const auto& l : g_lines)
        if (l.find(needle) != std::string::npos) return l;
    return "(no line)";
}
size_t log_count(const std::string& needle) {
    std::lock_guard lock(g_log_mutex);
    size_t n = 0;
    for (const auto& l : g_lines)
        if (l.find(needle) != std::string::npos) ++n;
    return n;
}

const KilnHooksApi* A = nullptr;
std::string g_order;  // what ran, in order (single-threaded tests)
std::mutex g_order_mutex;
void ran(char c) {
    std::lock_guard lock(g_order_mutex);
    g_order += c;
}
std::string take_order() {
    std::lock_guard lock(g_order_mutex);
    std::string s;
    s.swap(g_order);
    return s;
}

std::atomic<uint64_t> g_real{0};  // real-function executions
std::atomic<bool> g_probe{false};
std::atomic<uintptr_t> g_probe_ret{0};

KilnHookOrder ord(int prio, const char* label, const char* before = nullptr, const char* after = nullptr) {
    KilnHookOrder o{};
    o.size = sizeof o;
    o.priority = prio;
    o.label = KilnStr{label, std::strlen(label)};
    if (before) o.before = KilnStr{before, std::strlen(before)};
    if (after) o.after = KilnStr{after, std::strlen(after)};
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The game: functions in the .game section (the image range the stub image() reports).
#pragma code_seg(".game")
extern "C" {
__declspec(noinline) uint64_t game_sum9(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                        uint64_t g, uint64_t h, uint64_t i) {
    g_real.fetch_add(1);
    if (g_probe.load()) g_probe_ret.store(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}
__declspec(noinline) double game_fmix(double a, float b, int c, double d, float e) {
    g_real.fetch_add(1);
    return a * 1000.0 + static_cast<double>(b) * 100.0 + c * 10.0 + d + static_cast<double>(e) / 100.0;
}
__declspec(noinline) uint64_t game_v1(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) {
    g_real.fetch_add(1);
    return a + 10 * b + 100 * c + 1000 * d + 10000 * e + 100000 * f;
}
#define GAME_ORD(n)                                                                                                    \
    __declspec(noinline) uint64_t game_ord##n(uint64_t x) {                                                            \
        g_real.fetch_add(1);                                                                                           \
        return x * 3 + n;                                                                                              \
    }
GAME_ORD(1)
GAME_ORD(2)
GAME_ORD(3)
GAME_ORD(4)
GAME_ORD(5)
__declspec(noinline) uint64_t game_hdr(uint64_t a, int b) {
    g_real.fetch_add(1);
    return a * 10 + static_cast<uint64_t>(b);
}
#define GAME_LATE(n)                                                                                                   \
    __declspec(noinline) uint64_t game_late##n(uint64_t x) {                                                           \
        g_real.fetch_add(1);                                                                                           \
        return x * 3 + n;                                                                                              \
    }
GAME_LATE(1)
GAME_LATE(2)
GAME_LATE(3)
GAME_LATE(4)
__declspec(noinline) uint64_t game_rr(uint64_t x) {
    g_real.fetch_add(1);
    return x * 3 + 5;
}
__declspec(noinline) uint64_t game_rv(uint64_t x) {
    g_real.fetch_add(1);
    return x * 3 + 6;
}
__declspec(noinline) uint64_t game_chaos(uint64_t x) {
    g_real.fetch_add(1);
    return x * 3 + 7;
}
__declspec(noinline) uint64_t game_stress(uint64_t x) {
    g_real.fetch_add(1);
    return x * 3 + 1;
}
__declspec(noinline) uint64_t game_nested(uint64_t x) {  // calls another hooked target
    g_real.fetch_add(1);
    return game_stress(x) + 1;
}
__declspec(noinline) int game_throw(int x) {
    g_real.fetch_add(1);
    if (x) throw 42;
    return x + 7;
}
__declspec(noinline) int game_av(int x) {
    g_real.fetch_add(1);
    if (x) *static_cast<volatile int*>(nullptr) = x;
    return x + 7;
}
__declspec(noinline) uint64_t game_fault(uint64_t x) {
    g_real.fetch_add(1);
    return x + 1;
}
}
#pragma code_seg()

// Hand-assembled game functions with known instruction boundaries (1, 5, 7, 10, 14), `int f(int x)` = x + 1:
//   push rbx; sub rsp,20h; mov eax,ecx; add eax,1; add rsp,20h; pop rbx; ret
#define GAME_ASM(name)                                                                                                 \
    ".globl " #name "\n.def " #name "; .scl 2; .type 32; .endef\n" #name ":\n"                                         \
    "  push rbx\n  sub rsp, 0x20\n  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n"                    \
    "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"    \
    "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
extern "C" {
int game_asm_flags(int);
extern char game_asm_flags_hook[];
int game_asm_regs(int);
extern char game_asm_regs_hook[];
int game_asm_fb(int);
int game_asm_code(int);
int game_asm_code_hold(int);
int game_asm_code_stop(int);
extern char game_code_many[];
int game_asm_fo(int);
int game_asm_mid(int);
int game_asm_mid2(int);
int game_asm_mid3(int);
int game_asm_mix(int);
int game_asm_park(int);
int game_asm_idle(int);
int game_asm_idle2(int);
extern char game_many[];
extern char game_start_many[];
int game_asm_park_jcc(int);
void game_asm_park_done(void);
int game_asm_er1(int);
int game_asm_er2(int);
int game_asm_er3(int);
int game_asm_alt(int);
extern unsigned char game_asm_bad[];
int kiln_check_call(void* fn, int x);
void* g_exit_thread = nullptr;  // kernel32!ExitThread, for game_asm_park_done
}
// Mid hooks at a function entry that return early, redirect or do nothing (test_mid_entry_semantics):
//   game_asm_er*: x + 1 (GAME_ASM). game_asm_alt: x + 1000. game_asm_bad: bytes no decoder takes (push es is invalid in
//   64-bit mode), so the first detour on it is refused; the test rewrites them into GAME_ASM's code later.
// kiln_check_call(fn, x): calls fn(x) and returns 0xBADBAD when rsp after the call is not what it was before (a mid
// stub that resumed with a wrong rsp).
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n" GAME_ASM(game_asm_er1) GAME_ASM(game_asm_er2) GAME_ASM(game_asm_er3)
        ".globl game_asm_alt\n.def game_asm_alt; .scl 2; .type 32; .endef\ngame_asm_alt:\n"
        "  lea eax, [rcx + 1000]\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".globl game_asm_bad\ngame_asm_bad:\n"
        "  .byte 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".text\n"
        ".globl kiln_check_call\n.def kiln_check_call; .scl 2; .type 32; .endef\n.seh_proc kiln_check_call\nkiln_check_call:\n"
        "  push rbx\n  .seh_pushreg rbx\n  sub rsp, 0x20\n  .seh_stackalloc 0x20\n  .seh_endprologue\n"
        "  mov rbx, rsp\n  mov rax, rcx\n  mov ecx, edx\n  call rax\n  cmp rsp, rbx\n  je 1f\n"
        "  mov rsp, rbx\n  mov eax, 0xBADBAD\n"
        "1:\n  add rsp, 0x20\n  pop rbx\n  ret\n.seh_endproc\n");
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n" GAME_ASM(game_asm_fb) GAME_ASM(game_asm_fo)
            GAME_ASM(game_asm_mid) GAME_ASM(game_asm_mid2) GAME_ASM(game_asm_mid3) GAME_ASM(game_asm_mix)
                GAME_ASM(game_asm_code) GAME_ASM(game_asm_code_hold) GAME_ASM(game_asm_code_stop) ".text\n");

// 16 more of them for test_code_lock_order: first installs while other threads poll code_state.
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n.balign 32, 0xCC\n"
        ".globl game_code_many\n.def game_code_many; .scl 2; .type 32; .endef\ngame_code_many:\n"
        ".rept 16\n  push rbx\n  sub rsp, 0x20\n  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n  .balign 32, 0xCC\n.endr\n"
        ".text\n");

// 48 more for test_installs_while_threads_start: first installs while other threads keep creating threads.
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n.balign 32, 0xCC\n"
        ".globl game_start_many\n.def game_start_many; .scl 2; .type 32; .endef\ngame_start_many:\n"
        ".rept 48\n  push rbx\n  sub rsp, 0x20\n  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n  .balign 32, 0xCC\n.endr\n"
        ".text\n");

// Filler that makes the test's code section longer than KILN_HOOKS_CODE_RANGE_MAX (test_code_range_limit); never run.
__asm__(".section .game,\"xr\"\n.balign 4096, 0xCC\ngame_code_filler:\n.fill 0x10400, 1, 0xCC\n.text\n");

// 600 small functions, 32 bytes apart, `int f(int x)` = x + 1, for the target-cap test (512 distinct addresses to hook).
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n.balign 32, 0xCC\n"
        ".globl game_many\n.def game_many; .scl 2; .type 32; .endef\ngame_many:\n"
        ".rept 600\n  push rbx\n  sub rsp, 0x20\n  mov eax, ecx\n  add eax, 1\n  add rsp, 0x20\n  pop rbx\n  ret\n  .balign 32, 0xCC\n.endr\n"
        ".text\n");

// Functions for threads that are paused inside the bytes a first detour replaces (test_parked_threads).
//   game_asm_park:     push rbx (1 byte) | sub rsp,20h (4 bytes) ...      int f(int x) = x + 1
//   game_asm_park_jcc: test ecx,ecx (2) | jne short (2; safetyhook relocates it to a 6-byte jcc) | mov eax,100 (5)
//                      f(0) = 100, f(x) = x + 1. A thread paused after the jne is at offset 4 here and at offset 8 in the
//                      trampoline.
// game_asm_park_done is where a parked thread's crafted stack returns to: ExitThread(eax).
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n" GAME_ASM(game_asm_park) GAME_ASM(game_asm_idle) GAME_ASM(game_asm_idle2)
        ".globl game_asm_park_jcc\n.def game_asm_park_jcc; .scl 2; .type 32; .endef\ngame_asm_park_jcc:\n"
        "  test ecx, ecx\n  jne 1f\n  mov eax, 100\n  ret\n1:\n  lea eax, [rcx + 1]\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".globl game_asm_park_done\n.def game_asm_park_done; .scl 2; .type 32; .endef\ngame_asm_park_done:\n"
        "  mov ecx, eax\n  sub rsp, 8\n  jmp qword ptr [rip + g_exit_thread]\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        ".text\n");

// flags: `cmp ecx,5` then (hook point) `mov eax,0` (does not touch the flags), then `setz al`: 1 when x == 5. A hook at the
// hook point sees live flags.
__asm__(".intel_syntax noprefix\n.section .game,\"xr\"\n"
        ".globl game_asm_flags\n.def game_asm_flags; .scl 2; .type 32; .endef\ngame_asm_flags:\n"
        "  cmp ecx, 5\n"
        ".globl game_asm_flags_hook\ngame_asm_flags_hook:\n"
        "  mov eax, 0\n  setz al\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n  int3\n"
        // regs: load distinct values, hook point (mov eax,1), then check each (r13 is rewritten by the handler)
        ".globl game_asm_regs\n.def game_asm_regs; .scl 2; .type 32; .endef\ngame_asm_regs:\n"
        "  push rbx\n  push rsi\n  push rdi\n  push r12\n  push r13\n  push r14\n  push r15\n  sub rsp, 0x20\n"
        "  mov rbx, 0x1111\n  mov rsi, 0x2222\n  mov rdi, 0x3333\n  mov r12, 0x4444\n  mov r13, 0x5555\n"
        "  mov r14, 0x6666\n  mov r15, 0x7777\n  mov r10, 0x8888\n  mov r11, 0x9999\n  mov r8, 0xaaaa\n  mov r9, 0xbbbb\n"
        "  mov rdx, 0xcccc\n"
        ".globl game_asm_regs_hook\ngame_asm_regs_hook:\n"
        "  mov eax, 1\n"
        "  cmp rbx, 0x1111\n  jne 1f\n  cmp rsi, 0x2222\n  jne 1f\n  cmp rdi, 0x3333\n  jne 1f\n"
        "  cmp r12, 0x4444\n  jne 1f\n  cmp r13, 0x5556\n  jne 1f\n  cmp r14, 0x6666\n  jne 1f\n"
        "  cmp r15, 0x7777\n  jne 1f\n  cmp r10, 0x8888\n  jne 1f\n  cmp r11, 0x9999\n  jne 1f\n"
        "  cmp r8, 0xaaaa\n  jne 1f\n  cmp r9, 0xbbbb\n  jne 1f\n  cmp rdx, 0xcccc\n  jne 1f\n"
        "  jmp 2f\n"
        "1:\n  xor eax, eax\n"
        "2:\n  add rsp, 0x20\n  pop r15\n  pop r14\n  pop r13\n  pop r12\n  pop rdi\n  pop rsi\n  pop rbx\n  ret\n"
        "  int3\n  int3\n  int3\n  int3\n"
        ".text\n");

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Handlers
using Fn9 = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

template <int N>
struct L9 {  // raw link on a 9-argument function; link 2 also bumps arg 0 and (stack) arg 8
    static inline Fn9 orig = nullptr;
    static inline KilnHandle h = 0;
    static inline uintptr_t seen = 0;
    static inline uintptr_t align = 0;
    static uint64_t fn(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t hh,
                       uint64_t i) {
        seen = A->caller(h);  // before anything else is called
        align = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress()) & 15;
        ran(static_cast<char>('0' + N));
        if (N == 2) a += 1, i += 100;
        return orig(a, b, c, d, e, f, g, hh, i);
    }
};

uint64_t expect9(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h,
                 uint64_t i) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i;
}

using FnF = double (*)(double, float, int, double, float);
template <int N>
struct LF {
    static inline FnF orig = nullptr;
    static inline KilnHandle h = 0;
    static double fn(double a, float b, int c, double d, float e) {
        ran(static_cast<char>('0' + N));
        if (N == 1) a += 1.0;
        if (N == 2) e *= 2.0f, d += 0.25;
        const double r = orig(a, b, c, d, e);
        return N == 3 ? r + 0.5 : r;
    }
};
double expectF(double a, float b, int c, double d, float e) {
    return a * 1000.0 + static_cast<double>(b) * 100.0 + c * 10.0 + d + static_cast<double>(e) / 100.0;
}

// Structured (detour2) on the 9-argument function.
std::atomic<uintptr_t> g_s2_caller{0};
uintptr_t KILN_CALL s2_fn(KilnHookCall2* c, void* user) {
    g_s2_caller = c->caller;
    ran(static_cast<char>(reinterpret_cast<uintptr_t>(user)));
    c->args[8] += 1000;  // a stack argument
    c->args[2] += 1;
    return A->call_original2(c) + 1;
}

// Ordering links: each records its letter and forwards.
template <int N>
struct LO {
    static inline uint64_t (*orig)(uint64_t) = nullptr;
    static uint64_t fn(uint64_t x) {
        ran(static_cast<char>('a' + N));
        return orig(x);
    }
};
template <int N>
KilnHandle add_ord(uintptr_t target, KilnOwner owner, int prio, const char* before = nullptr, const char* after = nullptr) {
    KilnHandle h = 0;
    const std::string label = std::string("ord") + static_cast<char>('a' + N);
    const KilnHookOrder o = ord(prio, label.c_str(), before, after);
    // the label pointer only has to live for the call
    const KilnResult r = A->raw_detour(owner, target, &o, reinterpret_cast<void*>(&LO<N>::fn),
                                       reinterpret_cast<void**>(&LO<N>::orig), &h);
    return r == KILN_OK ? h : 0;
}

// An ordering link with order flags; `size` lets a test pass the KilnHookOrder of a plugin built before `flags` existed.
template <int N>
KilnHandle add_ord_flags(uintptr_t target, KilnOwner owner, int prio, uint32_t flags, uint32_t size = sizeof(KilnHookOrder)) {
    KilnHandle h = 0;
    const std::string label = std::string("ord") + static_cast<char>('a' + N);
    KilnHookOrder o = ord(prio, label.c_str());
    o.flags = flags;
    o.size = size;
    const KilnResult r = A->raw_detour(owner, target, &o, reinterpret_cast<void*>(&LO<N>::fn),
                                       reinterpret_cast<void**>(&LO<N>::orig), &h);
    return r == KILN_OK ? h : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Stand-in "other engine": Enshwick-style E9 hook (hookchain spike). The relay is allocated near the function.
uint8_t* near_alloc(const void* where, size_t n) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(where) & ~uintptr_t{0xFFFF};
    for (uintptr_t d = 0x10000; d < 0x70000000; d += 0x10000)
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t a = dir ? base + d : base - d;
            void* p = VirtualAlloc(reinterpret_cast<void*>(a), n, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    return nullptr;
}
void abs_jmp(uint8_t* at, const void* to) {  // FF 25 00000000 <abs64>
    at[0] = 0xFF, at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    const uint64_t a = reinterpret_cast<uint64_t>(to);
    std::memcpy(at + 6, &a, 8);
}
void rel_jmp(uint8_t* at, const void* to) {  // E9 rel32
    at[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(static_cast<const uint8_t*>(to) - (at + 5));
    std::memcpy(at + 1, &rel, 4);
}

uint8_t* g_fb_tramp = nullptr;
uint8_t* g_fo_tramp = nullptr;
int foreign_detour_fb(int x) {
    ran('F');
    return reinterpret_cast<int (*)(int)>(g_fb_tramp)(x);
}
int foreign_detour_fo(int x) {
    ran('F');
    return reinterpret_cast<int (*)(int)>(g_fo_tramp)(x);
}

// Hooks `func` the way Enshwick does (5-byte E9 to a nearby relay; its trampoline relocates an E9 already there).
// Returns the relay so the caller can inspect it.
uint8_t* foreign_hook(uint8_t* func, void* detour, uint8_t** tramp_out) {
    uint8_t* arena = near_alloc(func, 0x1000);
    if (!arena) return nullptr;
    uint8_t* relay = arena + 0x100;
    uint8_t* tramp = arena + 0x200;
    abs_jmp(relay, detour);
    std::memcpy(tramp, func, 5);
    if (func[0] == 0xE9) {
        int32_t rel;
        std::memcpy(&rel, func + 1, 4);
        rel_jmp(tramp, func + 5 + rel);
    }
    abs_jmp(tramp + 5, func + 5);
    *tramp_out = tramp;
    DWORD old;
    VirtualProtect(func, 16, PAGE_EXECUTE_READWRITE, &old);
    rel_jmp(func, relay);
    FlushInstructionCache(GetCurrentProcess(), func, 16);
    return relay;
}

void snapshot_file(const void* func) {
    g_file[reinterpret_cast<uintptr_t>(func)] =
        std::vector<uint8_t>(static_cast<const uint8_t*>(func), static_cast<const uint8_t*>(func) + 16);
}

std::string exe_name_lower() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::string s = kiln::to_utf8(std::wstring_view(path, n));
    s = s.substr(s.find_last_of("\\/") + 1);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// call sites (never tail calls: a real return address into each)
std::atomic<uint64_t> g_sink{0};
Fn9 volatile g_p9 = game_sum9;
__declspec(noinline) uint64_t callsite_a(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                         uint64_t g, uint64_t h, uint64_t i) {
    const uint64_t r = g_p9(a, b, c, d, e, f, g, h, i);
    g_sink.fetch_add(1);
    return r;
}
__declspec(noinline) uint64_t callsite_b(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                                         uint64_t g, uint64_t h, uint64_t i) {
    const uint64_t r = g_p9(a, b, c, d, e, f, g, h, i);
    g_sink.fetch_add(2);
    return r;
}

// SEH wrapper: no C++ objects here.
int call_av(int x, DWORD* code) {
    __try {
        return game_av(x);
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
void test_code_inspection() {
    std::puts("-- original code: read_original and code_state on the real service (detours, patches, write, an unreadable page)");
    const uintptr_t at = reinterpret_cast<uintptr_t>(&game_asm_code);
    snapshot_file(reinterpret_cast<void*>(at));
    uint8_t original[16] = {};
    uint32_t state = 99; uint64_t generation = 0;
    check(A->read_original && A->code_state && A->read_original(at, original, sizeof original) == KILN_OK,
          "the table has read_original and code_state; read_original copies the file's bytes");
    check(A->code_state(at, 16, &state, &generation) == KILN_OK && state == KILN_CODE_ORIGINAL,
          "a function nobody changed is ORIGINAL");
    const auto before = generation;
    static int (*continuation)(int) = nullptr;
    static auto handler = +[](int x) { return continuation(x); };
    KilnHandle h = 0;
    check(A->raw_detour(6, at, nullptr, reinterpret_cast<void*>(handler), reinterpret_cast<void**>(&continuation), &h) == KILN_OK,
          "the registry places a detour on it");
    check(A->code_state(at, 16, &state, &generation) == KILN_OK && state == KILN_CODE_MANAGED && generation > before,
          "its detour is MANAGED and the generation moved");
    uint8_t after[16] = {};
    check(A->read_original(at, after, sizeof after) == KILN_OK && !std::memcmp(original, after, sizeof after),
          "read_original still returns the file's bytes under the detour");
    check(A->code_state(at + 7, 9, &state, &generation) == KILN_OK && state == KILN_CODE_ORIGINAL,
          "the detour owns the instructions it overwrote, not the rest of the function");
    uint64_t again = 0;
    check(A->code_state(at, 16, &state, &generation) == KILN_OK && A->code_state(at, 16, &state, &again) == KILN_OK &&
              again == generation,
          "the same range asked twice with nothing changed keeps the generation");
    const auto installed = generation;
    A->revoke(h);  // the bytes stay: only the chain changes
    check(A->code_state(at, 16, &state, &generation) == KILN_OK && state == KILN_CODE_MANAGED && generation > installed,
          "after the revoke the detour stays MANAGED (an empty chain) and the generation moved");
    const uint8_t patch = original[10] ^ 0x7f;
    KilnHandle p = 0;
    check(A->patch(6, at + 10, &patch, 1, &p) == KILN_OK &&
          A->code_state(at + 10, 1, &state, &generation) == KILN_OK && state == KILN_CODE_MANAGED,
          "a patch's byte is MANAGED");
    const auto patched = generation;
    A->unpatch(p);
    check(A->code_state(at + 10, 1, &state, &generation) == KILN_OK && state == KILN_CODE_ORIGINAL && generation > patched,
          "unpatch makes it ORIGINAL again and moves the generation");
    A->write(at + 10, &patch, 1);  // write owns nothing: like another module's patch
    check(A->code_state(at, 16, &state, &generation) == KILN_OK && state == KILN_CODE_FOREIGN,
          "a byte from write beside Kiln's detour is FOREIGN");
    A->write(at + 10, original + 10, 1);
    DWORD old = 0, ignored = 0;
    check(VirtualProtect(reinterpret_cast<void*>(at), 16, PAGE_NOACCESS, &old) != 0, "the test page is made unreadable");
    const auto result = A->code_state(at, 16, &state, &generation);
    VirtualProtect(reinterpret_cast<void*>(at), 16, old, &ignored);
    check(result == KILN_OK && state == KILN_CODE_UNKNOWN, "an unreadable page is UNKNOWN (guarded read), never the file's bytes");
    std::atomic<bool> done{false}; std::atomic<unsigned> polls{0}, bad{0};
    std::thread observer([&] {
        do { uint32_t s; uint64_t g;
             if (A->code_state(at + 10, 1, &s, &g) != KILN_OK || s > KILN_CODE_MANAGED) ++bad;
             ++polls;
        } while (!done.load());
    });
    for (int i = 0; i < 1000; ++i) {
        KilnHandle handle = 0;
        if (A->patch(6, at + 10, &patch, 1, &handle) != KILN_OK || A->unpatch(handle) != KILN_OK) ++bad;
        if (A->raw_detour(6, at, nullptr, reinterpret_cast<void*>(handler), reinterpret_cast<void**>(&continuation), &handle) != KILN_OK ||
            A->revoke(handle) != KILN_OK) ++bad;
    }
    done = true; observer.join();
    check(polls > 0 && bad == 0, "a poller sees only ORIGINAL or MANAGED while patches and links come and go 1000 times");
}

// The range limit on the real members: exactly KILN_HOOKS_CODE_RANGE_MAX (64 KiB) is accepted; one byte more is refused
// with nothing written, and the generation and the remembered range are left alone. game_code_filler makes the
// test's code section longer than that.
void test_code_range_limit() {
    std::puts("-- original code: a range of exactly 64 KiB is accepted, one byte more is refused");
    constexpr size_t cap = KILN_HOOKS_CODE_RANGE_MAX;
    const uintptr_t at = reinterpret_cast<uintptr_t>(kiln::g_image.text);
    check(kiln::g_image.text_size > cap, "the test's code section is longer than 64 KiB",
          std::to_string(kiln::g_image.text_size));
    std::vector<uint8_t> out(cap + 1, 0x55);
    const auto untouched = [&] { return std::all_of(out.begin(), out.end(), [](uint8_t b) { return b == 0x55; }); };
    check(A->read_original(at, out.data(), cap) == KILN_OK && out[cap] == 0x55 &&
              !std::memcmp(out.data(), reinterpret_cast<const void*>(at), cap),
          "read_original accepts exactly 64 KiB");
    std::fill(out.begin(), out.end(), 0x55);
    check(A->read_original(at, out.data(), cap + 1) == KILN_ERR_INVALID_ARGUMENT && untouched(),
          "read_original refuses 64 KiB + 1 and writes nothing");
    uint32_t state = 99;
    uint64_t generation = 0;
    check(A->code_state(at, cap, &state, &generation) == KILN_OK && state <= KILN_CODE_UNKNOWN,
          "code_state accepts exactly 64 KiB");
    const uint64_t kept = generation;
    uint32_t refused_state = 77;
    uint64_t refused_generation = 88;
    check(A->code_state(at, cap + 1, &refused_state, &refused_generation) == KILN_ERR_INVALID_ARGUMENT &&
              refused_state == 77 && refused_generation == 88,
          "code_state refuses 64 KiB + 1 and leaves state and generation alone");
    uint64_t again = 0;
    check(A->code_state(at, cap, &state, &again) == KILN_OK && again == kept,
          "the refused call left the remembered range alone (the same range keeps the generation)");
}

// A first install takes code_state's lock before it reads the target and stops the game's threads. A code_state call
// holds that lock only for its two guarded copies, so a thread the install stops is never one that holds it. An install
// that wrote without the lock fails the checks below; one that stopped threads first and then waited for the lock would
// hang here for good, which is this test's failure too. The same lock makes patch, unpatch and write wait for a reader.
void test_code_lock_order() {
    std::puts("-- original code: installs, patches and write wait for a thread holding code_state's lock; polls during first installs");
    const uintptr_t at = reinterpret_cast<uintptr_t>(&game_asm_code_hold);
    const size_t unstoppable = log_count("could not be opened or stopped");
    snapshot_file(reinterpret_cast<void*>(at));
    uint8_t file[16];
    std::memcpy(file, reinterpret_cast<const void*>(at), sizeof file);
    auto live_is = [&](const uint8_t* bytes) { return !std::memcmp(reinterpret_cast<const void*>(at), bytes, 16); };
    // One writer call, made while another thread holds the lock: it must not return and must not change a byte until
    // the lock is free.
    auto while_held = [&](const char* what, auto&& call) {
        uint8_t before[16];
        std::memcpy(before, reinterpret_cast<const void*>(at), sizeof before);
        std::atomic<int> phase{0};
        std::thread holder([&] {
            auto guard = kiln::hooks::code::registry().lock();
            phase = 1;
            while (phase.load() < 2) Sleep(1);
            guard.unlock();
            // Stay alive until the writer returned: a thread that is ending while an install stops the others cannot
            // always be stopped, and that is another test's failure (test_many_threads_stopped counts those lines).
            while (phase.load() < 3) Sleep(1);
        });
        const bool holding = until([&] { return phase.load() >= 1; });
        std::atomic<bool> returned{false};
        KilnResult rc = KILN_ERR_INTERNAL;
        g_lock_reached = false;
        std::thread writer([&] {
            g_lock_watch = GetCurrentThreadId();
            rc = call();
            returned = true;
        });
        // The writer is at the lock the holder has: from here on it can only wait, so it has not returned and has
        // written nothing. A writer that wrote before taking the lock, or never takes it, fails here (the second at
        // the time limit).
        const bool reached = until([] { return g_lock_reached.load(); });
        const bool waited = holding && reached && !returned.load() && live_is(before);
        phase = 2;
        writer.join();
        g_lock_watch = 0;
        phase = 3;
        holder.join();
        check(waited, (std::string(what) + " waits for a thread that holds code_state's lock: nothing written meanwhile").c_str());
        return rc;
    };
    static int (*continuation)(int) = nullptr;
    static auto handler = +[](int x) { return continuation(x); };
    KilnHandle h = 0;
    const KilnResult installed = while_held("a first install", [&] {
        return A->raw_detour(6, at, nullptr, reinterpret_cast<void*>(handler), reinterpret_cast<void**>(&continuation), &h);
    });
    uint32_t state = 99;
    uint64_t generation = 0;
    check(installed == KILN_OK && !live_is(file) && reinterpret_cast<int (*)(int)>(at)(41) == 42 &&
              A->code_state(at, 16, &state, &generation) == KILN_OK && state == KILN_CODE_MANAGED,
          "...then it installs, and its detour reads MANAGED");
    const uint8_t byte = file[10] ^ 0x7f;
    KilnHandle p = 0;
    check(while_held("patch", [&] { return A->patch(6, at + 10, &byte, 1, &p); }) == KILN_OK &&
              *reinterpret_cast<const uint8_t*>(at + 10) == byte,
          "...then the patch is written");
    check(while_held("unpatch", [&] { return A->unpatch(p); }) == KILN_OK && *reinterpret_cast<const uint8_t*>(at + 10) == file[10],
          "...then the byte is put back");
    check(while_held("write", [&] { return A->write(at + 10, &byte, 1); }) == KILN_OK &&
              *reinterpret_cast<const uint8_t*>(at + 10) == byte,
          "...then write copies its byte");
    A->write(at + 10, file + 10, 1);
    A->revoke(h);

    // Four threads poll code_state on 16 functions while this thread places their first detours (each one stops every
    // other thread) and revokes the links: no poll waits forever, and none sees a half-written jump (UNKNOWN or FOREIGN).
    constexpr int kTargets = 16;
    uintptr_t targets[kTargets];
    for (int i = 0; i < kTargets; ++i) {
        targets[i] = reinterpret_cast<uintptr_t>(game_code_many) + 32 * static_cast<uintptr_t>(i);
        snapshot_file(reinterpret_cast<void*>(targets[i]));
    }
    std::atomic<bool> done{false};
    std::atomic<unsigned> polls{0}, bad{0};
    std::vector<std::thread> pollers;
    for (int k = 0; k < 4; ++k)
        pollers.emplace_back([&, k] {
            for (unsigned n = static_cast<unsigned>(k); !done.load(); ++n) {
                uint32_t s = 99;
                uint64_t g = 0;
                if (A->code_state(targets[n % kTargets], 16, &s, &g) != KILN_OK ||
                    (s != KILN_CODE_ORIGINAL && s != KILN_CODE_MANAGED))
                    ++bad;
                ++polls;
            }
        });
    const bool polling = until([&] { return polls.load() >= 1000; });
    const unsigned first = polls.load();
    unsigned refused = 0;
    static auto many = +[](int x) { return x + 1; };  // the functions' own result; it never calls on
    for (int i = 0; i < kTargets; ++i) {
        const unsigned at_start = polls.load();
        KilnHandle link = 0;
        if (A->raw_detour(6, targets[i], nullptr, reinterpret_cast<void*>(many), nullptr, &link) != KILN_OK ||
            A->revoke(link) != KILN_OK)
            ++refused;
        // the pollers ran again before the next install (a loaded machine may not schedule them otherwise)
        if (!until([&] { return polls.load() > at_start; })) ++refused;
    }
    const unsigned during = polls.load() - first;
    const bool polled = until([&] { return polls.load() >= first + during + 1000; });
    done = true;
    for (auto& t : pollers) t.join();
    bool all_managed = true;
    for (int i = 0; i < kTargets; ++i) {
        uint32_t s = 99;
        uint64_t g = 0;
        all_managed = all_managed && A->code_state(targets[i], 16, &s, &g) == KILN_OK && s == KILN_CODE_MANAGED;
    }
    check(polling && polled, "the 4 pollers ran before and after the installs");
    check(refused == 0 && all_managed, "16 first installs while 4 threads poll code_state: each placed, each MANAGED after");
    check(bad == 0 && during > 0, "the polls during them see only ORIGINAL or MANAGED", std::to_string(bad.load()) + " bad of " +
          std::to_string(polls.load()) + ", " + std::to_string(during) + " during the installs");
    check(log_count("could not be opened or stopped") == unstoppable, "these installs stopped every other thread they listed");
}

// A code_state call that waits for the lock while a first install stops every thread. The install holds the lock from
// before it reads the target until it has written the jump and let the threads run again, so a caller waiting for it
// holds nothing when it is stopped, and it finishes after the install, with the new bytes. The pause inside the
// install (at its read of the file's bytes, after it took the lock and before it stops the threads) makes this
// deterministic: the caller has read the file's bytes, its next step is the lock, and it cannot finish before the
// install does. read_original takes no lock: a call made during the pause returns at once. A lock taken only after the
// threads are stopped would deadlock in test_code_lock_order; an install without the lock fails the checks here.
void test_code_wait_during_stop() {
    std::puts("-- original code: a code_state call waits for the lock while a first install stops every thread");
    const uintptr_t at = reinterpret_cast<uintptr_t>(&game_asm_code_stop);
    snapshot_file(reinterpret_cast<void*>(at));
    uint8_t file[16];
    std::memcpy(file, reinterpret_cast<const void*>(at), sizeof file);
    const size_t unstoppable = log_count("could not be opened or stopped");
    const size_t stops = log_count("hooks: detour on rva");
    struct Original {  // a read_original call made during the pause; it outlives the pause if it ever waits
        std::atomic<bool> done{false};
        KilnResult rc = KILN_ERR_INTERNAL;
        uint8_t bytes[16] = {};
    };
    auto original = std::make_shared<Original>();
    std::atomic<bool> reader_done{false};
    KilnResult reader_rc = KILN_ERR_INTERNAL;
    uint32_t reader_state = 99;
    bool paused = false, reached = false, done_in_pause = true, original_in_pause = false;
    std::thread reader;
    g_pause_reader_read = false;
    g_pause_fn = [&] {  // on the installing thread, inside the install
        paused = true;
        g_lock_reached = false;
        reader = std::thread([&] {
            g_lock_watch = GetCurrentThreadId();
            g_pause_reader = GetCurrentThreadId();
            uint64_t g = 0;
            reader_rc = A->code_state(at, 16, &reader_state, &g);
            reader_done = true;
        });
        reached = until([] { return g_pause_reader_read.load(); });
        // its next step is the lock, which this install holds: once it is there, it can only wait
        reached = until([] { return g_lock_reached.load(); }) && reached;
        done_in_pause = reader_done.load();
        std::thread reading([original, at] {
            original->rc = A->read_original(at, original->bytes, sizeof original->bytes);
            original->done = true;
        });
        until([&] { return original->done.load(); });
        original_in_pause = original->done.load() && original->rc == KILN_OK &&
                            !std::memcmp(original->bytes, file, sizeof file);
        // Joined, it has ended before the install lists the threads (a thread still ending then cannot always be
        // stopped). One that waits for this install's lock is let go instead.
        if (original->done.load()) reading.join();
        else reading.detach();
    };
    g_pause_at = at;
    g_pause_installer = GetCurrentThreadId();
    static int (*continuation)(int) = nullptr;
    static auto handler = +[](int x) { return continuation(x); };
    KilnHandle h = 0;
    const KilnResult installed =
        A->raw_detour(6, at, nullptr, reinterpret_cast<void*>(handler), reinterpret_cast<void**>(&continuation), &h);
    if (reader.joinable()) reader.join();
    g_pause_at = 0;
    g_pause_installer = 0;
    g_pause_reader = 0;
    g_lock_watch = 0;
    g_pause_fn = nullptr;
    check(installed == KILN_OK && paused && reached,
          "the install paused after taking the lock; the caller read the file's bytes meanwhile and reached the lock");
    check(!done_in_pause, "...and could not finish while the install held the lock");
    check(reader_done && reader_rc == KILN_OK && reader_state == KILN_CODE_MANAGED,
          "...it finished after the install and saw the detour (MANAGED)", "state " + std::to_string(reader_state));
    check(log_count("hooks: detour on rva") == stops + 1 && log_count("could not be opened or stopped") == unstoppable,
          "the install stopped every other thread it listed, the waiting caller among them");
    check(original_in_pause, "read_original returned the file's bytes while the install held the lock");
    check(reinterpret_cast<int (*)(int)>(at)(41) == 42 && A->revoke(h) == KILN_OK, "the detour works");
}

void test_raw_chain_9args() {
    std::puts("-- raw links: 9 arguments through a chain of 3, caller() in each link");
    // probe the real return address of each call site before anything is hooked
    g_probe = true;
    callsite_a(1, 1, 1, 1, 1, 1, 1, 1, 1);
    const uintptr_t ret_a = g_probe_ret;
    callsite_b(1, 1, 1, 1, 1, 1, 1, 1, 1);
    const uintptr_t ret_b = g_probe_ret;
    g_probe = false;
    check(ret_a && ret_b && ret_a != ret_b, "probe: two distinct call sites", "a/b differ");

    const uintptr_t target = reinterpret_cast<uintptr_t>(&game_sum9);
    const auto o1 = ord(30, "t/sum9-1"), o2 = ord(20, "t/sum9-2"), o3 = ord(10, "t/sum9-3");
    check(A->raw_detour(6, target, &o1, reinterpret_cast<void*>(&L9<1>::fn), reinterpret_cast<void**>(&L9<1>::orig),
                        &L9<1>::h) == KILN_OK && L9<1>::orig,
          "raw link 1 installed, original set before the first call");
    check(A->raw_detour(6, target, &o2, reinterpret_cast<void*>(&L9<2>::fn), reinterpret_cast<void**>(&L9<2>::orig),
                        &L9<2>::h) == KILN_OK,
          "raw link 2 installed");
    check(A->raw_detour(6, target, &o3, reinterpret_cast<void*>(&L9<3>::fn), reinterpret_cast<void**>(&L9<3>::orig),
                        &L9<3>::h) == KILN_OK,
          "raw link 3 installed");
    take_order();
    g_real = 0;
    const uint64_t r = callsite_a(1, 2, 3, 4, 5, 6, 7, 8, 9);
    const uint64_t want = expect9(1 + 1, 2, 3, 4, 5, 6, 7, 8, 9 + 100);
    check(r == want, "all nine arguments reach the real function (link 2's changes included)",
          std::to_string(r) + " vs " + std::to_string(want));
    check(take_order() == "123" && g_real == 1, "links ran in priority order, the real function once");
    {
        char b[200];
        std::snprintf(b, sizeof b, "want %p, got %p %p %p", reinterpret_cast<void*>(ret_a), reinterpret_cast<void*>(L9<1>::seen),
                      reinterpret_cast<void*>(L9<2>::seen), reinterpret_cast<void*>(L9<3>::seen));
        check(L9<1>::seen == ret_a && L9<2>::seen == ret_a && L9<3>::seen == ret_a,
              "caller() is the game's call site in every link", b);
    }
    check(L9<1>::align == 8 && L9<2>::align == 8 && L9<3>::align == 8,
          "stack alignment at the handlers is the standard one (rsp = 8 mod 16)");
    const uint64_t r2 = callsite_b(9, 8, 7, 6, 5, 4, 3, 2, 1);
    check(r2 == expect9(9 + 1, 8, 7, 6, 5, 4, 3, 2, 1 + 100) && L9<1>::seen == ret_b && L9<3>::seen == ret_b,
          "the other call site is reported correctly");
    take_order();
}

void test_floats() {
    std::puts("-- raw links: float/double arguments and a double return");
    const uintptr_t target = reinterpret_cast<uintptr_t>(&game_fmix);
    const auto o1 = ord(3, "t/fmix-1"), o2 = ord(2, "t/fmix-2"), o3 = ord(1, "t/fmix-3");
    A->raw_detour(6, target, &o1, reinterpret_cast<void*>(&LF<1>::fn), reinterpret_cast<void**>(&LF<1>::orig), &LF<1>::h);
    A->raw_detour(6, target, &o2, reinterpret_cast<void*>(&LF<2>::fn), reinterpret_cast<void**>(&LF<2>::orig), &LF<2>::h);
    A->raw_detour(6, target, &o3, reinterpret_cast<void*>(&LF<3>::fn), reinterpret_cast<void**>(&LF<3>::orig), &LF<3>::h);
    take_order();
    volatile double a = 2.5, d = 3.75;
    volatile float b = 1.5f, e = 4.0f;
    const double r = game_fmix(a, b, 7, d, e);
    const double want = expectF(a + 1.0, b, 7, d + 0.25, e * 2.0f) + 0.5;
    check(std::fabs(r - want) < 1e-9, "xmm0-3 and the stack float survive the chain; the float return passes back",
          std::to_string(r) + " vs " + std::to_string(want));
    check(take_order() == "123", "float links ran in order");
}

void test_structured() {
    std::puts("-- detour2: 9 integer arguments (stack arguments included) with the caller");
    const uintptr_t target = reinterpret_cast<uintptr_t>(&game_sum9);
    // a structured link in front of the raw ones (priority 40) and one behind them (priority 0)
    const auto front = ord(40, "t/sum9-s-front"), back = ord(0, "t/sum9-s-back");
    KilnHandle hf = 0, hb = 0;
    check(A->detour2(6, target, &front, 9, &s2_fn, reinterpret_cast<void*>('S'), &hf) == KILN_OK, "detour2 installed (front)");
    check(A->detour2(6, target, &back, 9, &s2_fn, reinterpret_cast<void*>('s'), &hb) == KILN_OK, "detour2 installed (back)");
    check(A->detour2(6, target, &back, 13, &s2_fn, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "detour2 refuses more than 12 arguments");
    take_order();
    g_probe = false;
    const uint64_t r = callsite_a(1, 2, 3, 4, 5, 6, 7, 8, 9);
    // chain: S (front) -> 1 -> 2 -> 3 -> s (back) -> real. Structured links add +1000 to arg 8, +1 to arg 2, +1 return.
    const uint64_t want = expect9(1 + 1, 2, 3 + 2, 4, 5, 6, 7, 8, 9 + 100 + 2000) + 2;
    check(r == want, "structured links in a mixed chain change stack arguments and the return",
          std::to_string(r) + " vs " + std::to_string(want));
    check(take_order() == "S123s", "structured links ran at their priority among the raw ones");
    check(g_s2_caller != 0 && g_s2_caller == L9<1>::seen, "KilnHookCall2.caller is the game's call site");
    A->revoke(hf);
    A->revoke(hb);
    take_order();
    const uint64_t r2 = callsite_a(1, 2, 3, 4, 5, 6, 7, 8, 9);
    check(r2 == expect9(1 + 1, 2, 3, 4, 5, 6, 7, 8, 9 + 100) && take_order() == "123", "revoking them leaves the raw chain");
}

void test_ordering() {
    std::puts("-- ordering: priority, registration order, before/after owners, an unsatisfiable constraint");
    // owners: 1 = service:kiln-chat.dll ("kiln.chat"), 2 = plugin:kiln-emf.dll ("kiln.emf"), 3 = emf:coolmod,
    //         4 = native:alpha, 5 = native:beta
    take_order();
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_ord1);
        add_ord<0>(t, 4, 0);
        add_ord<1>(t, 4, 10);
        add_ord<2>(t, 5, 5);
        add_ord<3>(t, 5, 5);  // same priority as c: registration order
        game_ord1(1);
        check(take_order() == "bcda", "priority first (higher runs first), then registration order", "bcda");
    }
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_ord2);
        add_ord<0>(t, 1, 0);                          // a: kiln.chat
        add_ord<1>(t, 2, 100, nullptr, "kiln.chat");  // b: kiln-emf, must run after kiln.chat despite priority 100
        add_ord<2>(t, 3, 50);                         // c
        game_ord2(1);
        const std::string got = take_order();
        check(got == "cab", "after=\"kiln.chat\" beats priority: kiln-emf's link follows kiln.chat's", got);
    }
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_ord3);
        add_ord<0>(t, 4, 0, "native:beta");   // a before beta's link
        add_ord<1>(t, 5, 99);                 // b: beta, high priority
        add_ord<2>(t, 6, 50);                 // c
        game_ord3(1);
        const std::string got = take_order();
        check(got == "acb" || got == "cab", "before=\"native:beta\": alpha's link precedes beta's despite priority", got);
        check(got.find('a') < got.find('b'), "a before b");
    }
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_ord4);
        add_ord<0>(t, 4, 0, "native:beta");   // a before b
        add_ord<1>(t, 5, 0, "native:alpha");  // b before a: contradicts a's constraint, dropped
        game_ord4(1);
        const std::string got = take_order();
        check(got == "ab", "a contradictory constraint is dropped; the earlier one holds", got);
        check(log_has("contradicts an earlier constraint"), "the dropped constraint is logged");
    }
}

// ---- ur8 finding 5: mods of different engines hooking one function: the newest detour runs first
void test_newest_first() {
    std::puts("-- KILN_HOOK_NEWEST_FIRST: equal-priority mod links of different engines run newest first");
    // owners: 3 = emf:coolmod, 8 = emf:mody (two EMF mods), 7 = shroudtopia:modx; 4/5/6 = other owners
    take_order();
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_ord5);
    constexpr uint32_t F = KILN_HOOK_NEWEST_FIRST;
    const KilnHandle ha = add_ord_flags<0>(t, 3, 1, F);  // a: first
    const KilnHandle hb = add_ord_flags<1>(t, 7, 1, F);  // b: a Shroudtopia mod, second (installed again below)
    const KilnHandle hc = add_ord_flags<2>(t, 8, 1, F);  // c: another EMF mod, third
    check(ha && hb && hc, "the three links are installed");
    (void)hb;
    game_ord5(1);
    std::string got = take_order();
    check(got == "cba", "three mod links of three owners at one priority: the newest runs first, as layered detours did", got);

    // Kiln's own links (priority 0), a link without the flag at the mods' priority, and a higher priority
    const KilnHandle hd = add_ord<3>(t, 4, 0);                   // d: priority 0, no flag
    const KilnHandle he = add_ord_flags<4>(t, 5, 1, 0);          // e: the mods' priority, no flag
    const KilnHandle hf = add_ord_flags<5>(t, 5, 2, F);          // f: higher priority
    game_ord5(1);
    got = take_order();
    check(got == "fcbaed", "priority first; at one priority the flagged links (newest first) precede the others (registration order)", got);

    // installing the oldest again (revoke, then the same install: a revival) makes it the newest
    A->revoke(ha);
    const KilnHandle ha2 = add_ord_flags<0>(t, 3, 1, F);
    game_ord5(1);
    got = take_order();
    check(ha2 && got == "facbed", "a revived link counts as the newest", got);
    // installing a live link again (replaces it in place) does too
    const KilnHandle hb2 = add_ord_flags<1>(t, 7, 1, F);
    game_ord5(1);
    got = take_order();
    check(hb2 && got == "fbaced", "a link installed again in place counts as the newest", got);

    // a plugin built before `flags` existed passes the smaller KilnHookOrder: the flag is not read, the link is a plain one
    const KilnHandle hg = add_ord_flags<6>(t, 6, 1, F, static_cast<uint32_t>(offsetof(KilnHookOrder, flags)));
    game_ord5(1);
    got = take_order();
    check(hg && got == "fbacegd", "an order without the flags field (older size) is read as before: no flag", got);

    for (KilnHandle h : {hb2, ha2, hc, hd, he, hf, hg}) A->revoke(h);
    take_order();
    check(game_ord5(2) == 2 * 3 + 5 && take_order().empty(), "all revoked: the real function alone");
}

void test_mid() {
    std::puts("-- mid links: at a function entry, in the middle of a function, beside raw links, xmm preserved");
    auto* fe = reinterpret_cast<int (*)(int)>(&game_asm_mid);
    static KilnHandle h1 = 0;
    static std::atomic<uintptr_t> top_of_stack{0}, mid_caller{0};
    static void* walk[32] = {};
    static USHORT walk_n = 0;
    static auto cb1 = +[](KilnRegisters* r, void*) {
        mid_caller = A->caller(h1);
        ran('M');
        r->rcx += 10;
        top_of_stack = *reinterpret_cast<uintptr_t*>(r->rsp);  // the hooked function's return address
        walk_n = RtlCaptureStackBackTrace(0, 32, walk, nullptr);  // unwinds through kiln_hk_mid's realigned frame
    };
    const auto o = ord(0, "t/mid-entry");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_mid), &o, cb1, nullptr, &h1) == KILN_OK, "mid2 installed at an entry");
    take_order();
    check(fe(41) == 52 && take_order() == "M", "a mid hook at a function entry changes an argument register");
    check(top_of_stack != 0 && top_of_stack == mid_caller,
          "ctx.rsp is the hooked code's rsp: [rsp] is the return address caller() reports");
    {
        bool found = false;
        for (USHORT i = 0; i < walk_n; ++i) found = found || reinterpret_cast<uintptr_t>(walk[i]) == top_of_stack;
        check(found, "a stack walk from a mid handler unwinds through kiln_hk_mid to the game's caller",
              std::to_string(walk_n) + " frames");
    }

    // beside a raw link on the same target (priority decides: raw first)
    static int (*orig3)(int) = nullptr;
    static auto raw3 = +[](int x) -> int {
        ran('R');
        return orig3(x + 1);
    };
    const auto oraw = ord(5, "t/mid3-raw"), omid = ord(0, "t/mid3-mid");
    check(A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_asm_mid3), &oraw, reinterpret_cast<void*>(raw3),
                        reinterpret_cast<void**>(&orig3), nullptr) == KILN_OK,
          "a raw link on the target");
    static auto cb3 = +[](KilnRegisters* r, void*) {
        ran('m');
        r->rcx += 100;
    };
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_mid3), &omid, cb3, nullptr, nullptr) == KILN_OK,
          "a mid link on the same target (supported: they share one physical detour)");
    take_order();
    const int v = game_asm_mid3(0);
    check(v == 102 && take_order() == "Rm", "raw then mid on one target: 0 +1 (raw) +100 (mid) +1 (function)", std::to_string(v));

    // the middle of a function: offset 5 (mov eax,ecx), stack not at the usual alignment, first call of a new thread
    static auto cb2 = +[](KilnRegisters* r, void*) {
        ran('x');
        r->rcx += 1;
    };
    uint8_t* mid_at = reinterpret_cast<uint8_t*>(&game_asm_mid2) + 5;
    check(A->mid(6, reinterpret_cast<uintptr_t>(mid_at), 0, cb2, nullptr, nullptr) == KILN_OK, "v1 mid installed mid-function");
    take_order();
    int got = 0;
    std::string order_in_thread;
    std::thread t([&] {
        got = reinterpret_cast<int (*)(int)>(&game_asm_mid2)(41);  // this thread's first hooked call: kiln_hk_slow at a mid-function point
    });
    t.join();
    check(got == 43 && take_order() == "x", "mid hook in the middle of a function, first call of a fresh thread", std::to_string(got));

    // xmm registers across a mid link: the handler clobbers them, the context restores them
    static auto clobber = +[](KilnRegisters*, void*) {
        ran('c');
        volatile double x = 1.25;
        for (int i = 0; i < 4; ++i) x = x * 1.5 + std::sqrt(x);
    };
    const auto oc = ord(1000, "t/fmix-clobber");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_fmix), &oc, clobber, nullptr, nullptr) == KILN_OK, "mid link on the float function");
    take_order();
    volatile double a = 2.5, d = 3.75;
    volatile float b = 1.5f, e = 4.0f;
    const double r = game_fmix(a, b, 7, d, e);
    const double want = expectF(a + 1.0, b, 7, d + 0.25, e * 2.0f) + 0.5;
    check(std::fabs(r - want) < 1e-9 && take_order() == "c123", "xmm0-3 and flags survive a mid link", std::to_string(r));
}

void test_mid_registers_and_flags() {
    std::puts("-- mid links keep every register and the flags (also on a thread's first call)");
    static auto flags_cb = +[](KilnRegisters*, void*) {
        ran('f');
        volatile int x = 0;  // plain code that is sure to write the flags
        for (int i = 0; i < 3; ++i) x = x + 1 - 7 * (x > 5);
    };
    const auto of = ord(0, "t/flags");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_flags_hook), &of, flags_cb, nullptr, nullptr) == KILN_OK,
          "mid link between a cmp and its consumer");
    take_order();
    int a = -1, b = -1;
    std::thread t1([&] { a = game_asm_flags(5); });  // first hooked call of the thread: slow path with live flags
    t1.join();
    std::thread t2([&] { b = game_asm_flags(4); });
    t2.join();
    check(a == 1 && b == 0, "flags survive the entry stub's slow path and the context capture", std::to_string(a) + "," + std::to_string(b));
    take_order();

    static auto regs_cb = +[](KilnRegisters* r, void*) {
        ran('r');
        volatile double x = 1.0;
        for (int i = 0; i < 5; ++i) x = x * 1.1 + std::sqrt(x);  // clobbers xmm0-5
        r->r13 = 0x5556;                                           // a write-back
    };
    const auto orr = ord(0, "t/regs");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_regs_hook), &orr, regs_cb, nullptr, nullptr) == KILN_OK, "mid link in a register-heavy function");
    int rr = -1;
    std::thread t3([&] { rr = game_asm_regs(0); });
    t3.join();
    check(rr == 1 && take_order() == "r", "rbx/rsi/rdi/r8-r15/rdx intact, and a handler's write to r13 takes effect");
    A->revoke_owner(6);
}

void test_v1_compat() {
    std::puts("-- v1: detour (4 args, now forwards every argument), mid, stats, revoke, v1 + raw on one target");
    static int calls = 0;
    static auto v1fn = +[](KilnHookCall* c, void*) -> uintptr_t {
        ++calls;
        ran('V');
        c->args[0] += 1;
        return A->call_original(c) + 1000000;
    };
    const uintptr_t target = reinterpret_cast<uintptr_t>(&game_v1);
    KilnHandle hv = 0;
    check(A->detour(6, target, 5, v1fn, nullptr, &hv) == KILN_OK && hv, "v1 detour installed");
    static uint64_t (*orig_r)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) = nullptr;
    static auto rawfn = +[](uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) -> uint64_t {
        ran('R');
        return orig_r(a, b, c + 1, d, e, f);
    };
    const auto o = ord(1, "t/v1-raw");
    check(A->raw_detour(6, target, &o, reinterpret_cast<void*>(rawfn), reinterpret_cast<void**>(&orig_r), nullptr) == KILN_OK,
          "a raw link on the same target");
    take_order();
    const uint64_t r = game_v1(1, 2, 3, 4, 5, 6);
    check(r == (1 + 1) + 10 * 2 + 100 * (3 + 1) + 1000 * 4 + 10000 * 5 + 100000 * 6 + 1000000,
          "v1 + raw share the physical detour; arguments past the 4th reach the original", std::to_string(r));
    check(take_order() == "VR", "v1 (priority 5) ran before the raw link (priority 1)");
    KilnHooksStats st{};
    st.size = sizeof st;
    const bool stats_ok = A->stats(&st) == KILN_OK;
    check(stats_ok && st.handlers > 0 && st.raw_links > 0 && st.detour_calls > 0 && st.targets > 0,
          "stats: v2 fields filled", "raw=" + std::to_string(st.raw_links) + " targets=" + std::to_string(st.targets));
    struct V1Stats {  // the v1 layout (a v1 caller passes this size)
        uint32_t size, a, b, c, d;
        uint64_t e, f;
    } old{};
    old.size = sizeof old;
    check(A->stats(reinterpret_cast<KilnHooksStats*>(&old)) == KILN_OK && old.c == st.handlers,
          "stats: a v1-sized struct still works");
    check(A->revoke(hv) == KILN_OK && A->revoke(hv) == KILN_ERR_NOT_FOUND, "revoke a v1 link, twice");
    take_order();
    game_v1(1, 2, 3, 4, 5, 6);
    check(take_order() == "R", "the revoked v1 link is gone");
    // v1 patch handles still revoke through the same function
    static uint8_t scratch[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const uint8_t patch[2] = {9, 9};
    KilnHandle hp = 0;
    check(A->patch(6, reinterpret_cast<uintptr_t>(scratch), patch, 2, &hp) == KILN_OK && scratch[0] == 9, "patch applied");
    check(A->revoke(hp) == KILN_OK && scratch[0] == 1 && scratch[1] == 2, "revoke() of a patch handle restores the bytes");
}

void test_continuation_after_revoke() {
    std::puts("-- a revoked link's continuation stays safe");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_fault);
    static uint64_t (*orig_a)(uint64_t) = nullptr;
    static uint64_t (*orig_b)(uint64_t) = nullptr;
    static auto fa = +[](uint64_t x) -> uint64_t { ran('a'); return orig_a(x); };
    static auto fb = +[](uint64_t x) -> uint64_t { ran('b'); return orig_b(x + 100); };
    KilnHandle ha = 0, hb = 0;
    const auto oa = ord(10, "t/fault-a"), ob = ord(5, "t/fault-b");
    A->raw_detour(6, t, &oa, reinterpret_cast<void*>(fa), reinterpret_cast<void**>(&orig_a), &ha);
    A->raw_detour(6, t, &ob, reinterpret_cast<void*>(fb), reinterpret_cast<void**>(&orig_b), &hb);
    take_order();
    check(game_fault(1) == 102 && take_order() == "ab", "baseline");
    A->revoke(ha);
    take_order();
    // a handler that still holds `orig_a` (a's continuation) calls it after the revoke: it forwards to b, then the function
    check(orig_a(1) == 102 && take_order() == "b", "the revoked link's continuation forwards to the next live link");
    A->revoke(hb);
    take_order();
    check(orig_a(1) == 2 && take_order().empty(), "...and to the real function once nothing is left");
    check(game_fault(1) == 2, "an empty chain just calls the real function");
    A->revoke_owner(6);  // owner 6 owns most test links: no crash, idempotent
    A->revoke_owner(6);
    check(game_fault(7) == 8, "revoke_owner is idempotent and leaves the original reachable");
}

void test_faults_and_exceptions() {
    std::puts("-- faults and exceptions");
    // structured handler faults before calling the original: disabled, the original still runs exactly once
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_stress);
        static auto bad = +[](KilnHookCall2* c, void*) -> uintptr_t {
            ran('B');
            *static_cast<volatile int*>(nullptr) = 1;
            return A->call_original2(c);
        };
        static auto good = +[](KilnHookCall2* c, void*) -> uintptr_t {
            ran('G');
            return A->call_original2(c);
        };
        KilnHandle hb = 0;
        const auto ob = ord(10, "t/stress-bad"), og = ord(5, "t/stress-good");
        A->detour2(6, t, &ob, 1, bad, nullptr, &hb);
        A->detour2(6, t, &og, 1, good, nullptr, nullptr);
        take_order();
        g_real = 0;
        const uint64_t r = game_stress(5);
        check(r == 16 && g_real == 1 && take_order() == "BG", "a faulting structured handler is disabled; the chain continues, once",
              std::to_string(r));
        check(log_has("faulted") && log_has("was disabled"), "the fault is logged");
        take_order();
        check(game_stress(5) == 16 && take_order() == "G", "the disabled link is skipped afterwards");
        A->revoke_owner(6);
    }
    // a structured handler that faults AFTER calling the original: the original is not run a second time
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_nested);
        static auto late = +[](KilnHookCall2* c, void*) -> uintptr_t {
            const uintptr_t r = A->call_original2(c);
            ran('L');
            *static_cast<volatile int*>(nullptr) = 1;
            return r + 1;
        };
        KilnHandle hl = 0;
        const auto ol = ord(10, "t/nested-late");
        A->detour2(6, t, &ol, 1, late, nullptr, &hl);
        take_order();
        g_real = 0;
        const uint64_t r = game_nested(3);
        // game_nested runs once and calls game_stress once: 2 real executions; value = stress(3) + 1 = 11
        check(r == 11 && g_real == 2 && take_order() == "L", "a handler that faults after calling the original does not run it twice",
              std::to_string(r) + " real=" + std::to_string(g_real.load()));
        A->revoke_owner(6);
    }
    // exception thrown by the game function, through raw + structured + raw links
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_throw);
        static int (*oa)(int) = nullptr;
        static int (*ob)(int) = nullptr;
        static auto ra = +[](int x) -> int { ran('1'); return oa(x); };
        static auto rb = +[](int x) -> int { ran('3'); return ob(x); };
        static auto sf = +[](KilnHookCall2* c, void*) -> uintptr_t { ran('2'); return A->call_original2(c); };
        KilnHandle hs = 0;
        const auto o1 = ord(30, "t/throw-1"), o2 = ord(20, "t/throw-2"), o3 = ord(10, "t/throw-3");
        A->raw_detour(6, t, &o1, reinterpret_cast<void*>(ra), reinterpret_cast<void**>(&oa), nullptr);
        A->detour2(6, t, &o2, 1, sf, nullptr, &hs);
        A->raw_detour(6, t, &o3, reinterpret_cast<void*>(rb), reinterpret_cast<void**>(&ob), nullptr);
        take_order();
        int caught = 0;
        try {
            game_throw(1);
        } catch (int v) {
            caught = v;
        }
        check(caught == 42 && take_order() == "123", "a C++ exception thrown by the game unwinds through raw and structured links");
        check(game_throw(0) == 7, "the chain still works afterwards");
        take_order();
        std::string info;
        KilnHandle probe = hs;
        check(A->revoke(probe) == KILN_OK, "the structured link was still live (the exception was not blamed on it)");
        A->revoke_owner(6);
    }
    // a hardware fault in the game function itself is not the handler's: it reaches the caller's SEH handler
    {
        const uintptr_t t = reinterpret_cast<uintptr_t>(&game_av);
        static auto sf = +[](KilnHookCall2* c, void*) -> uintptr_t { ran('s'); return A->call_original2(c); };
        KilnHandle hs = 0;
        const auto o = ord(1, "t/av-s");
        A->detour2(6, t, &o, 1, sf, nullptr, &hs);
        take_order();
        DWORD code = 0;
        const int r = call_av(1, &code);
        check(r == -1 && code == EXCEPTION_ACCESS_VIOLATION && take_order() == "s",
              "an access violation inside the game function reaches the caller's handler", "code=" + std::to_string(code));
        check(call_av(0, &code) == 7, "...and the structured link is still active (not disabled)");
        A->revoke_owner(6);
    }
}

void test_foreign() {
    std::puts("-- foreign hooks: before Kiln (chained, recorded) and over Kiln (detected, never unhooked)");
    // before: another engine's E9 jmp is on the function when Kiln hooks it
    {
        auto* f = reinterpret_cast<uint8_t*>(&game_asm_fb);
        snapshot_file(f);
        foreign_hook(f, reinterpret_cast<void*>(&foreign_detour_fb), &g_fb_tramp);
        {   // what sigdb saw at resolve time: the live bytes of a symbol that differ from the file's
            kiln::sigdb::LiveDiff d;
            d.symbol = "fake/Fb";
            d.address = reinterpret_cast<uintptr_t>(f);
            d.at = d.address;
            d.bytes_ok = true;
            std::memcpy(d.bytes, f, 16);
            d.by = "jmp to test";
            g_live_diffs.push_back(d);
        }
        take_order();
        check(reinterpret_cast<int (*)(int)>(f)(41) == 42 && take_order() == "F", "the other engine's hook alone");
        static int (*orig)(int) = nullptr;
        static auto fn = +[](int x) -> int { ran('K'); return orig(x); };
        const auto o = ord(0, "t/fb");
        KilnHandle h = 0;
        check(A->raw_detour(6, reinterpret_cast<uintptr_t>(f), &o, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&orig), &h) == KILN_OK,
              "Kiln hooks a function that starts with a foreign jmp");
        check(reinterpret_cast<int (*)(int)>(f)(41) == 42 && take_order() == "KF", "both run: Kiln, then the other engine, then the original once");
        check(log_has("hooks: t/fb (test) chained over " + exe_name_lower()) || log_has("hooks: t/fb (test) chained over hooks_v2_test.exe"),
              "kiln.log: \"<label> (<owner>) chained over <module>+0x...\" (the hook and its owner, GitHub issue 39)");
        const auto ch = chain::chain_of(reinterpret_cast<uintptr_t>(f));
        check(ch.size() == 1, "one link on the target");
        bool found = false;
        for (const auto& ti : chain::targets())
            if (ti.address == reinterpret_cast<uintptr_t>(f)) {
                found = ti.has_before && lower(ti.before.module) == exe_name_lower() && !ti.before.own && ti.links.size() == 1 &&
                        ti.links[0].kind == chain::Kind::Raw && ti.links[0].label == "t/fb" && !ti.has_over;
            }
        check(found, "targets(): the snapshot carries the links and the foreign hook it was chained over");
    }
    // over: Kiln hooks first, then another engine hooks over it (its decoder relocates Kiln's jmp)
    {
        auto* f = reinterpret_cast<uint8_t*>(&game_asm_fo);
        static int (*orig)(int) = nullptr;
        static auto fn = +[](int x) -> int { ran('K'); return orig(x); };
        const auto o = ord(0, "t/fo");
        A->raw_detour(6, reinterpret_cast<uintptr_t>(f), &o, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&orig), nullptr);
        take_order();
        check(reinterpret_cast<int (*)(int)>(f)(41) == 42 && take_order() == "K", "Kiln's hook alone");
        check(chain::check_foreign() == 0, "check_foreign: nothing hooked over yet");
        uint8_t own[16];
        std::memcpy(own, f, 16);
        foreign_hook(f, reinterpret_cast<void*>(&foreign_detour_fo), &g_fo_tramp);
        check(reinterpret_cast<int (*)(int)>(f)(41) == 42 && take_order() == "FK", "both run: the other engine, then Kiln, then the original once");
        const size_t over = chain::check_foreign();
        check(over == 1, "check_foreign: one target hooked over", std::to_string(over));
        check(log_has("hooked over Kiln's t/fo (test)"), "kiln.log: \"<module> hooked over Kiln's <label> (<owner>)\"");
        check(log_has(" [game folder] hooked over Kiln's t/fo (test)"),
              "kiln.log: the module's file follows its name (\"<module>+0x.. [game folder] hooked over\"; two DLLs can share a name)");
        KilnHooksStats st{};
        st.size = sizeof st;
        A->stats(&st);
        check(st.foreign_over == 1 && st.foreign_before == 1, "stats count foreign-before and foreign-over", "before=" + std::to_string(st.foreign_before) + " over=" + std::to_string(st.foreign_over));
        // the hook map records both and the sigdb live-differs list
        size_t len = 0;
        check(A->map_json(nullptr, 0, &len) == KILN_ERR_BUFFER_TOO_SMALL && len > 100, "map_json reports the needed length");
        std::string buf(len + 1, '\0');
        check(A->map_json(buf.data(), buf.size(), &len) == KILN_OK, "map_json fills the buffer");
        json j = json::parse(buf.data(), buf.data() + len, nullptr, false);
        check(j.is_object() && j.value("format", 0) == 1 && j["targets"].is_array(), "map: format 1 with targets");
        bool saw_before = false, saw_over = false;
        for (auto& t : j["targets"]) {
            if (!t["foreignBefore"].is_null()) {
                saw_before = true;
                check(lower(t["foreignBefore"].value("module", "")) == exe_name_lower() && !t["foreignBefore"].value("offset", "").empty(),
                      "map: foreignBefore names the foreign module and offset", t["foreignBefore"].dump());
            }
            if (!t["foreignOver"].is_null()) {
                saw_over = true;
                check(lower(t["foreignOver"].value("module", "")) == exe_name_lower(), "map: foreignOver names the module",
                      t["foreignOver"].dump());
                check(lower(t["foreignOver"].value("path", "")) == exe_name_lower(),
                      "map: foreignOver's path is relative to the game folder", t["foreignOver"].dump());
            }
            check(t.contains("rva") && t.contains("symbol") && t.contains("label") && t["links"].is_array(), "map: target has rva/symbol/label/links");
            for (auto& l : t["links"])
                if (!(l.contains("owner") && l.contains("kind") && l.contains("priority") && l.contains("label"))) check(false, "map: link fields");
        }
        check(saw_before && saw_over, "map: both foreign directions are listed");
        check(j["foreign"].is_array() && !j["foreign"].empty() && j["foreign"][0]["symbol"] == "fake/Fb" &&
                  lower(j["foreign"][0].value("module", "")) == exe_name_lower(),
              "map: sigdb's live-differs symbols appear under \"foreign\" with module attribution", j["foreign"].dump());
        // the foreign engine removes its jump: Kiln notices
        std::memcpy(f, own, 16);
        FlushInstructionCache(GetCurrentProcess(), f, 16);
        check(chain::check_foreign() == 0 && log_has("no longer hooked over"), "the foreign hook goes away: check_foreign clears the record");
    }
}

void test_analyze() {
    std::puts("-- foreign hook attribution: a jump into a trampoline outside any module");
    // like safetyhook's mid-hook trampoline: some code, then `call [rip+slot]` to the handler in a module
    uint8_t* arena = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t* tr = arena + 0x40;
    const uint8_t pre[] = {0x50, 0x53, 0x51, 0x52, 0x9C};  // push rax; push rbx; push rcx; push rdx; pushfq
    std::memcpy(tr, pre, sizeof pre);
    tr[5] = 0xFF, tr[6] = 0x15;
    const int32_t disp = static_cast<int32_t>((arena + 0x200) - (tr + 11));
    std::memcpy(tr + 7, &disp, 4);
    tr[11] = 0x9D, tr[12] = 0xC3;
    const uint64_t handler = reinterpret_cast<uint64_t>(&foreign_detour_fb);
    std::memcpy(arena + 0x200, &handler, 8);
    uint8_t site[16] = {};
    uint8_t* at = arena + 0x800;  // where the foreign E9 would sit
    rel_jmp(at, tr);
    std::memcpy(site, at, 16);
    const chain::Dest d = chain::analyze(reinterpret_cast<uintptr_t>(at), site);
    check(d.present && lower(d.module) == exe_name_lower() && d.address == handler,
          "an E9 into a trampoline that calls a handler through [rip+slot] is attributed to the handler's module", d.module);
    // plain cases
    uint8_t relay[16];
    abs_jmp(relay, reinterpret_cast<void*>(&foreign_detour_fb));
    uint8_t* at2 = arena + 0x900;
    std::memcpy(at2, relay, 14);
    uint8_t site2[16] = {};
    std::memcpy(site2, at2, 14);
    const chain::Dest d2 = chain::analyze(reinterpret_cast<uintptr_t>(at2), site2);
    check(d2.present && lower(d2.module) == exe_name_lower() && d2.offset != 0, "a 14-byte absolute jmp is followed to its module+offset");
    uint8_t plain[16] = {0x90, 0x90, 0x90, 0x90, 0x90};
    const chain::Dest d3 = chain::analyze(reinterpret_cast<uintptr_t>(at2), plain);
    check(!d3.present && d3.in_place, "bytes without a jump are reported as patched in place");
    VirtualFree(arena, 0, MEM_RELEASE);
}

void test_map_file_and_extension() {
    std::puts("-- hooks.json on disk, map extensions");
    static auto ext = +[](void* j, const char* phase) {
        auto& doc = *static_cast<nlohmann::ordered_json*>(j);
        doc["overlaps"] = nlohmann::ordered_json::array();
        doc["extPhase"] = phase;
    };
    kiln::hooks::add_map_extension(ext);
    kiln::hooks::boot_complete();
    std::ifstream in(g_state_dir / "hooks.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    json j = json::parse(text, nullptr, false);
    check(j.is_object() && j.value("phase", "") == "boot" && j.contains("overlaps") && j.value("extPhase", "") == "boot",
          "boot_complete writes state/hooks.json; extensions add their sections");
    check(log_has("map state/hooks.json"), "boot_complete logs its summary");
}

// Threads that call a function in a loop count their calls apiece. made(n): each of them made n more calls from now (at
// most kMustEnd), so all but the first of those began after whatever the test changed just before. Used instead of
// sleeping and hoping the callers ran meanwhile.
struct Callers {
    std::atomic<uint64_t> calls[4] = {};
    bool made(uint64_t n) {
        uint64_t from[4];
        for (int k = 0; k < 4; ++k) from[k] = calls[k].load();
        return until([&] {
            for (int k = 0; k < 4; ++k)
                if (calls[k].load() < from[k] + n) return false;
            return true;
        });
    }
};

void test_threads() {
    std::puts("-- 4 threads call in a loop while links are added and revoked");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_stress);
    static std::atomic<uint64_t> hits[6];
    static uint64_t (*orig[6])(uint64_t) = {};
    // handlers per index: count, forward
    static auto h0 = +[](uint64_t x) -> uint64_t { hits[0]++; return orig[0](x); };
    static auto h1 = +[](uint64_t x) -> uint64_t { hits[1]++; return orig[1](x); };
    static auto h2 = +[](uint64_t x) -> uint64_t { hits[2]++; return orig[2](x); };
    static auto h3 = +[](uint64_t x) -> uint64_t { hits[3]++; return orig[3](x); };
    static auto h4 = +[](uint64_t x) -> uint64_t { hits[4]++; return orig[4](x); };
    static auto h5 = +[](uint64_t x) -> uint64_t { hits[5]++; return orig[5](x); };
    void* fns[6] = {reinterpret_cast<void*>(h0), reinterpret_cast<void*>(h1), reinterpret_cast<void*>(h2),
                    reinterpret_cast<void*>(h3), reinterpret_cast<void*>(h4), reinterpret_cast<void*>(h5)};
    KilnHandle hd[6] = {};
    auto add = [&](int i, int prio) {
        const auto o = ord(prio, "t/stress");
        return A->raw_detour(6, t, &o, fns[i], reinterpret_cast<void**>(&orig[i]), &hd[i]) == KILN_OK;
    };
    check(add(0, 30) && add(1, 20) && add(2, 10), "three links installed");
    g_real = 0;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> calls{0}, bad{0};
    Callers each;
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k)
        th.emplace_back([&, k] {
            uint64_t i = static_cast<uint64_t>(k) * 1000003;
            while (!stop.load()) {
                const uint64_t r = game_stress(i);
                if (r != i * 3 + 1) ++bad;
                ++calls;
                ++each.calls[k];
                ++i;
            }
        });
    bool kept_up = true;
    auto nap = [&] { kept_up = each.made(100) && kept_up; };  // every caller goes through the chain as it is now
    static std::atomic<uint64_t> s_hits{0}, m_hits{0};
    static auto s_fn = +[](KilnHookCall2* c, void*) -> uintptr_t { s_hits++; return A->call_original2(c); };
    static auto m_fn = +[](KilnRegisters*, void*) { m_hits++; };
    KilnHandle hs = 0, hm = 0;
    nap();
    A->revoke(hd[1]);  // the middle one
    nap();
    add(3, 25);        // in front of one, behind another
    {
        const auto os = ord(22, "t/stress-s"), om = ord(18, "t/stress-m");
        A->detour2(6, t, &os, 1, s_fn, nullptr, &hs);
        A->mid2(6, t, &om, m_fn, nullptr, &hm);
    }
    nap();
    A->revoke(hs);
    nap();
    A->revoke(hd[0]);  // the head
    nap();
    add(4, 5);         // the tail
    add(5, 50);        // a new head
    nap();
    A->revoke(hd[2]);
    A->revoke(hd[5]);
    nap();
    check(kept_up && m_hits > 0 && s_hits > 0, "structured and mid links in the churn were reached", "s=" + std::to_string(s_hits.load()) + " m=" + std::to_string(m_hits.load()));
    A->revoke_owner(6);  // everything, at once
    nap();
    stop = true;
    for (auto& x : th) x.join();
    check(bad == 0, "every call returned the right value through every chain state", "bad=" + std::to_string(bad.load()));
    check(g_real == calls, "no lost calls: the real function ran once per call", std::to_string(g_real.load()) + " vs " + std::to_string(calls.load()));
    check(calls > 1000, "the loops really ran", std::to_string(calls.load()));
    uint64_t total_hits = 0;
    for (auto& x : hits) total_hits += x;
    check(total_hits > 0, "links were reached while they were live");
    check(game_stress(2) == 7, "the target still works with an empty chain");
}

// Random adds and revokes of every kind, with random priorities and constraints, while 4 threads call.
template <int N>
struct CL {
    static inline uint64_t (*orig)(uint64_t) = nullptr;
    static inline std::atomic<uint64_t> hits{0};
    static uint64_t fn(uint64_t x) {
        hits++;
        return orig(x);
    }
};
constexpr int kPool = 16;
template <int... I>
void add_chaos_raw(int pick, uintptr_t target, KilnOwner owner, const KilnHookOrder& o, KilnHandle* h, std::integer_sequence<int, I...>) {
    ((pick == I ? (void)A->raw_detour(owner, target, &o, reinterpret_cast<void*>(&CL<I>::fn), reinterpret_cast<void**>(&CL<I>::orig), h) : (void)0), ...);
}
struct Rec {
    KilnHandle h;
    KilnOwner owner;
    int tmpl;  // the CL<> template a raw link uses (its `original` variable belongs to that one link), else -1
};

void test_chaos() {
    std::puts("-- random link churn (raw, structured, mid; priorities, constraints, revoke_owner) under 4 calling threads");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_chaos);
    static std::atomic<uint64_t> shits{0}, mhits{0};
    static auto s_fn = +[](KilnHookCall2* c, void*) -> uintptr_t { shits++; return A->call_original2(c); };
    static auto m_fn = +[](KilnRegisters*, void*) { mhits++; };
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> calls{0}, bad{0};
    g_real = 0;
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k)
        th.emplace_back([&, k] {
            uint64_t i = static_cast<uint64_t>(k) * 104729;
            while (!stop.load()) {
                if (game_chaos(i) != i * 3 + 7) ++bad;
                ++calls;
                ++i;
            }
        });
    uint32_t rng = 12345;
    auto rnd = [&](uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; };
    const char* names[] = {"service:kiln-chat.dll", "plugin:kiln-emf.dll", "emf:coolmod", "native:alpha"};
    const char* tokens[] = {"kiln.chat", "kiln.emf", "coolmod", "native:alpha", "nobody"};
    std::vector<Rec> live;
    unsigned used = 0;  // bit i: CL<i> belongs to a live link
    int adds = 0, revokes = 0;
    // A fixed number of steps of the seeded churn (no clock), and on until the churn has really run. The bound of a
    // wait that must end stops a churn that never gets there.
    constexpr int kSteps = 3000;
    const auto give_up = std::chrono::steady_clock::now() + kMustEnd;
    bool kept_up = true;
    for (int step = 0; step < kSteps || adds <= 100 || revokes <= 20 || calls.load() <= 1000; ++step) {
        if (std::chrono::steady_clock::now() > give_up) {
            kept_up = false;
            break;
        }
        const uint32_t act = rnd(10);
        if (act < 6 || live.size() < 3) {
            const KilnOwner owner = 1 + rnd(4);
            const char* before = rnd(4) == 0 ? tokens[rnd(5)] : nullptr;
            const char* after = rnd(4) == 0 ? tokens[rnd(5)] : nullptr;
            const KilnHookOrder o = ord(static_cast<int>(rnd(7)) - 3, "t/chaos", before, after);
            KilnHandle h = 0;
            uint32_t kind = rnd(3);
            int tmpl = -1;
            if (kind == 0) {
                for (int k = 0; k < kPool && tmpl < 0; ++k)
                    if (!(used & (1u << k))) tmpl = k;
                if (tmpl < 0) kind = 1;
            }
            KilnResult rc = KILN_ERR_INTERNAL;
            if (kind == 0) {
                add_chaos_raw(tmpl, t, owner, o, &h, std::make_integer_sequence<int, kPool>{});
                rc = h ? KILN_OK : KILN_ERR_INTERNAL;
                if (rc == KILN_OK) used |= 1u << tmpl;
            } else if (kind == 1) {
                rc = A->detour2(owner, t, &o, 1, s_fn, nullptr, &h);
            } else {
                rc = A->mid2(owner, t, &o, m_fn, nullptr, &h);
            }
            if (rc == KILN_OK && h) live.push_back({h, owner, kind == 0 ? tmpl : -1}), ++adds;
        } else if (act < 9) {
            const size_t i = rnd(static_cast<uint32_t>(live.size()));
            A->revoke(live[i].h);
            if (live[i].tmpl >= 0) used &= ~(1u << live[i].tmpl);
            live.erase(live.begin() + static_cast<long>(i));
            ++revokes;
        } else {
            const KilnOwner o = 1 + rnd(4);
            A->revoke_owner(o);
            for (size_t i = 0; i < live.size();)
                if (live[i].owner == o) {
                    if (live[i].tmpl >= 0) used &= ~(1u << live[i].tmpl);
                    live.erase(live.begin() + static_cast<long>(i));
                } else {
                    ++i;
                }
            ++revokes;
        }
        if (rnd(3) == 0) {  // the callers go through the chain as it is now
            const uint64_t c = calls.load();
            kept_up = until([&] { return calls.load() > c + 4; }) && kept_up;
        }
    }
    stop = true;
    for (auto& x : th) x.join();
    check(bad == 0, "every call returned the right value through every chain state", "bad=" + std::to_string(bad.load()));
    check(g_real == calls, "no lost calls: the real function ran once per call", std::to_string(g_real.load()) + " vs " + std::to_string(calls.load()));
    check(kept_up && adds > 100 && revokes > 20 && calls > 1000, "the churn really ran", "adds=" + std::to_string(adds) + " revokes=" + std::to_string(revokes) + " calls=" + std::to_string(calls.load()));
    // quiescent: priority order with no constraints, via the raw links that are left
    for (KilnOwner o = 1; o <= 4; ++o) A->revoke_owner(o);
    check(chain::chain_of(t).empty() && game_chaos(5) == 22, "revoke_owner of everything empties the chain");
    take_order();
    static uint64_t (*qo[4])(uint64_t) = {};
    static auto q0 = +[](uint64_t x) -> uint64_t { ran('0'); return qo[0](x); };
    static auto q1 = +[](uint64_t x) -> uint64_t { ran('1'); return qo[1](x); };
    static auto q2 = +[](uint64_t x) -> uint64_t { ran('2'); return qo[2](x); };
    static auto q3 = +[](uint64_t x) -> uint64_t { ran('3'); return qo[3](x); };
    void* qf[4] = {reinterpret_cast<void*>(q0), reinterpret_cast<void*>(q1), reinterpret_cast<void*>(q2), reinterpret_cast<void*>(q3)};
    const int prios[4] = {1, 5, 5, -2};
    for (int i = 0; i < 4; ++i) {
        const KilnHookOrder o = ord(prios[i], "t/chaos-q");
        A->raw_detour(5, t, &o, qf[i], reinterpret_cast<void**>(&qo[i]), nullptr);
    }
    game_chaos(1);
    check(take_order() == "1203", "after the churn a fresh chain still orders by priority, then registration");
    A->revoke_owner(5);
}

// A RawHook installed again in place reuses its `original` variable. A thread still inside the revoked link's handler
// reads the variable and jumps through the new link's continuation the moment install() writes it: before the chain is
// published. That cell must already name the link after the new one (here C): not 0 (it was, and the thread jumped to
// address 0) and not the real function (C would be skipped for that call). The window is a few hundred nanoseconds wide,
// so the test repeats it 400 times with a thread that spins on the variable and jumps at once.
void test_reinstall_shares_original() {
    constexpr int kRounds = 400;
    std::puts("-- a thread inside a revoked raw link continues through the re-installed link: no jump to 0, no link skipped");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_late4);
    static void* volatile ao = nullptr;
    static std::atomic<int> inside{0};
    static auto ah = +[](uint64_t x) -> uint64_t {
        ran('A');
        void* old = ao;
        inside.fetch_add(1);
        while (ao == old) _mm_pause();  // until install() hands out the new continuation
        return reinterpret_cast<uint64_t (*)(uint64_t)>(ao)(x);
    };
    static uint64_t (*co)(uint64_t) = nullptr;
    static auto ch = +[](uint64_t x) -> uint64_t { ran('C'); return co(x); };
    take_order();
    inside = 0;
    const auto oc = ord(-5, "t/reinst-c"), oa = ord(5, "t/reinst-a");
    KilnHandle cur = 0;
    bool ok = A->raw_detour(6, t, &oc, reinterpret_cast<void*>(ch), reinterpret_cast<void**>(&co), nullptr) == KILN_OK;
    ok = A->raw_detour(6, t, &oa, reinterpret_cast<void*>(ah), const_cast<void**>(&ao), &cur) == KILN_OK && ok;
    std::atomic<int> finished{0}, taken{0};
    std::atomic<uint64_t> wrong{0};
    std::thread th([&] {
        for (int i = 0; i < kRounds; ++i) {
            while (taken.load() < i) _mm_pause();  // the previous round's record is taken before this one starts
            if (game_late4(10) != 34) ++wrong;
            finished.store(i + 1);
        }
    });
    auto wait_for = [](const std::atomic<int>& v, int n) {
        const auto t0 = std::chrono::steady_clock::now();
        while (v.load() < n) {
            if (std::chrono::steady_clock::now() - t0 > kMustEnd) return false;
            _mm_pause();
        }
        return true;
    };
    int skipped = 0, rounds = 0;
    for (int i = 0; i < kRounds && ok; ++i) {
        if (!wait_for(inside, i + 1)) { ok = false; break; }  // the thread is inside the current A
        A->revoke(cur);
        // a label of its own each round: a new link (the same label would revive the revoked one, and its continuation,
        // the value the thread waits to change, would stay the same)
        const std::string lab = "t/reinst-a" + std::to_string(i);
        const auto oai = ord(5, lab.c_str());
        ok = A->raw_detour(6, t, &oai, reinterpret_cast<void*>(ah), const_cast<void**>(&ao), &cur) == KILN_OK && ok;
        if (!wait_for(finished, i + 1)) { ok = false; break; }
        const std::string got = take_order();
        if (got != "AC" && ++skipped <= 3) std::printf("  round %d ran '%s'\n", i, got.c_str());
        ++rounds;
        taken.store(i + 1);
    }
    if (!ok) {  // let a stuck thread out
        taken = kRounds;
        ao = reinterpret_cast<void*>(+[](uint64_t x) -> uint64_t { return x; });
    }
    th.join();
    check(ok && rounds == kRounds, "every round installed A again while the thread was inside the old A", "rounds " + std::to_string(rounds));
    check(skipped == 0 && wrong == 0, "the thread always went on through the new link to C and the real function",
          "rounds that skipped C: " + std::to_string(skipped) + ", wrong results: " + std::to_string(wrong.load()));
    A->revoke_owner(6);
}

void test_first_install_under_load() {
    std::puts("-- the first link on a target goes in while 4 threads call it");
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> calls{0}, bad{0};
    Callers each;
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k)
        th.emplace_back([&, k] {
            uint64_t i = static_cast<uint64_t>(k) * 7919;
            while (!stop.load()) {
                if (game_late1(i) != i * 3 + 1) ++bad;
                if (game_late2(i) != i * 3 + 2) ++bad;
                if (game_late3(i) != i * 3 + 3) ++bad;
                calls += 3;
                ++each.calls[k];
                ++i;
            }
        });
    bool kept_up = true;
    // the 4 threads are calling (before the installs) and went through each new link (after it)
    auto nap = [&] { kept_up = each.made(100) && kept_up; };
    static std::atomic<uint64_t> hit1{0}, hit3{0};
    static uint64_t (*o1)(uint64_t) = nullptr;
    static auto f1 = +[](uint64_t x) -> uint64_t { hit1++; return o1(x); };
    static auto s2 = +[](KilnHookCall2* c, void*) -> uintptr_t { return A->call_original2(c); };
    static auto m3 = +[](KilnRegisters*, void*) { hit3++; };
    nap();
    const auto a = ord(0, "t/late1"), b = ord(0, "t/late2"), c = ord(0, "t/late3");
    bool ok = A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_late1), &a, reinterpret_cast<void*>(f1), reinterpret_cast<void**>(&o1), nullptr) == KILN_OK;
    nap();
    ok = A->detour2(6, reinterpret_cast<uintptr_t>(&game_late2), &b, 1, s2, nullptr, nullptr) == KILN_OK && ok;
    nap();
    ok = A->mid2(6, reinterpret_cast<uintptr_t>(&game_late3), &c, m3, nullptr, nullptr) == KILN_OK && ok;
    nap();
    stop = true;
    for (auto& t : th) t.join();
    check(ok && bad == 0, "three first installs under load: every call still returned the right value", "bad=" + std::to_string(bad.load()));
    check(kept_up && hit1 > 0 && hit3 > 0 && calls > 1000, "the new links were reached afterwards", "calls=" + std::to_string(calls.load()));
    A->revoke_owner(6);
}

// A thread the scheduler stopped right after a function's first instruction sits inside the bytes the first detour
// replaces. It must resume at the same instruction in the trampoline, not in the middle of the new jump. The test parks
// threads there deliberately (a suspended thread with a crafted context), so the case does not wait for bad luck.
HANDLE park_thread(const void* fn, size_t at, bool pushed_rbx, uint64_t rcx, bool zf) {
    DWORD tid = 0;
    HANDLE h = CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(const_cast<void*>(fn)), nullptr,
                            CREATE_SUSPENDED, &tid);
    if (!h) return nullptr;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(h, &c)) return TerminateThread(h, 1), CloseHandle(h), nullptr;
    // the stack as it is just after `call fn` (rsp = 8 mod 16), then the instructions before `at` have run
    const uint64_t entry_rsp = ((c.Rsp - 0x400) & ~uint64_t{15}) + 8;
    auto* top = reinterpret_cast<uint64_t*>(entry_rsp);
    top[0] = reinterpret_cast<uint64_t>(&game_asm_park_done);
    c.Rsp = entry_rsp;
    if (pushed_rbx) {
        c.Rsp -= 8;
        *reinterpret_cast<uint64_t*>(c.Rsp) = 0;
    }
    c.Rip = reinterpret_cast<uint64_t>(fn) + at;
    c.Rcx = rcx;
    c.EFlags = static_cast<DWORD>((c.EFlags & ~0x40u) | (zf ? 0x40u : 0u));
    if (!SetThreadContext(h, &c)) return TerminateThread(h, 1), CloseHandle(h), nullptr;
    CONTEXT back{};
    back.ContextFlags = CONTEXT_CONTROL;
    if (!GetThreadContext(h, &back) || back.Rip != c.Rip) return TerminateThread(h, 1), CloseHandle(h), nullptr;
    return h;
}

void test_parked_threads() {
    std::puts("-- threads paused inside the replaced bytes are moved to the trampoline (first detour, nobody runs)");
    g_exit_thread = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "ExitThread"));
    const auto park = reinterpret_cast<uintptr_t>(&game_asm_park), jcc = reinterpret_cast<uintptr_t>(&game_asm_park_jcc);
    struct P {
        uintptr_t fn;
        size_t at;
        bool pushed;
        uint64_t rcx;
        bool zf;
        DWORD want;
        const char* what;
        HANDLE h = nullptr;
    } ps[] = {
        {park, 0, false, 9, false, 10, "at the function's first byte (stays: it runs the new jump into the chain)"},
        {park, 1, true, 41, false, 42, "after push rbx (offset 1, inside the jmp's rel32)"},
        {jcc, 2, false, 1, false, 2, "before the jne, jne taken (offset 2)"},
        {jcc, 4, false, 0, true, 100, "after the jne (offset 4; the trampoline has it at 8)"},
    };
    bool parked = true;
    for (P& p : ps) {
        p.h = park_thread(reinterpret_cast<void*>(p.fn), p.at, p.pushed, p.rcx, p.zf);
        parked = parked && p.h;
    }
    check(parked, "four threads parked at or inside the first bytes of two functions");
    if (!parked) {
        for (P& p : ps)
            if (p.h) TerminateThread(p.h, 1), CloseHandle(p.h);
        return;
    }
    static int (*po)(int) = nullptr;
    static int (*jo)(int) = nullptr;
    static std::atomic<int> phits{0}, jhits{0};
    static auto ph = +[](int x) -> int { phits++; return po(x); };
    static auto jh = +[](int x) -> int { jhits++; return jo(x); };
    const auto o = ord(0, "t/park");
    bool ok = A->raw_detour(6, park, &o, reinterpret_cast<void*>(ph), reinterpret_cast<void**>(&po), nullptr) == KILN_OK;
    ok = A->raw_detour(6, jcc, &o, reinterpret_cast<void*>(jh), reinterpret_cast<void**>(&jo), nullptr) == KILN_OK && ok;
    check(ok, "the first detours on both functions are in");
    bool moved = true;
    for (P& p : ps) {
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        const bool got = GetThreadContext(p.h, &c);
        const bool inside = got && c.Rip >= p.fn + 1 && c.Rip < p.fn + 16;
        const bool kept = p.at != 0 || (got && c.Rip == p.fn);  // a thread at the first byte is left where it is
        check(got && !inside && kept, (std::string("parked thread left the replaced bytes: ") + p.what).c_str(),
              got ? "rip " + std::to_string(c.Rip - p.fn) + " bytes from the function" : "no context");
        moved = moved && got && !inside && kept;
    }
    if (!moved) {  // resuming a thread in the middle of the jump would take the whole test down
        for (P& p : ps) TerminateThread(p.h, 1), CloseHandle(p.h);
        return;
    }
    for (P& p : ps) ResumeThread(p.h);
    for (P& p : ps) {
        DWORD code = 0xDEAD;
        const bool done = WaitForSingleObject(p.h, kMustEndMs) == WAIT_OBJECT_0 && GetExitCodeThread(p.h, &code);
        check(done && code == p.want, (std::string("it runs on from the trampoline to the right result: ") + p.what).c_str(),
              "exit code " + std::to_string(code) + ", wanted " + std::to_string(p.want));
        if (!done) TerminateThread(p.h, 1);
        CloseHandle(p.h);
    }
    const bool live = game_asm_park(7) == 8 && game_asm_park_jcc(0) == 100 && game_asm_park_jcc(5) == 6;
    // park: the thread parked at its first byte went through the detour (1) + the call here (1)
    check(live && phits == 2 && jhits == 2, "the detours are live for threads that enter the functions normally",
          "park hits " + std::to_string(phits.load()) + ", jcc hits " + std::to_string(jhits.load()));
    check(log_has("1 thread(s) were paused inside the first bytes") && log_has("2 thread(s) were paused inside the first bytes"),
          "kiln.log says how many threads were moved");
    A->revoke_owner(6);
}

// Under Wine a new thread is listed (NtGetNextThread, Toolhelp) as soon as the wineserver has made it, before its creator
// has started it at the Unix level. A first install that stopped the creator in that window and then asked the new
// thread for its context waited for an answer only the started thread can give: forever (4 hangs in ~1300 runs of this
// test under Wine 11.20 with ntsync, 2026-11-20; a thread pool thread was inside NtCreateThreadEx each time). Here two
// threads create threads nonstop while 48 first installs go in: each must finish, and nothing may break.
void test_installs_while_threads_start() {
    std::puts("-- 48 first installs while 2 other threads keep creating threads (some listed before they have started)");
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> made{0};
    std::vector<std::thread> churn;
    for (int k = 0; k < 2; ++k)
        churn.emplace_back([&] {
            while (!stop.load()) {
                HANDLE h = CreateThread(nullptr, 0, [](LPVOID) -> DWORD { return 0; }, nullptr, 0, nullptr);
                if (!h) continue;
                WaitForSingleObject(h, INFINITE);
                CloseHandle(h);
                ++made;
            }
        });
    while (made.load() < 20) Sleep(1);
    static std::atomic<int> hits{0};
    static auto m = +[](KilnRegisters*, void*) { hits++; };
    const auto o = ord(0, "t/start");
    const auto first = reinterpret_cast<uintptr_t>(game_start_many);
    const size_t lines_before = log_count("hooks: detour on rva");
    int placed = 0;
    for (int i = 0; i < 48; ++i)
        placed += A->mid2(6, first + 32 * static_cast<uintptr_t>(i), &o, m, nullptr, nullptr) == KILN_OK;
    stop = true;
    for (auto& t : churn) t.join();
    check(placed == 48, "48 first installs while threads were being created: each finished and was placed",
          "placed " + std::to_string(placed) + ", threads created meanwhile " + std::to_string(made.load()));
    bool right = true;
    for (int i = 0; i < 48; ++i)
        right = right && reinterpret_cast<int (*)(int)>(first + 32 * static_cast<uintptr_t>(i))(i) == i + 1;
    check(right && hits == 48, "every function still returns x + 1 and runs its mid link once",
          "hits " + std::to_string(hits.load()));
    check(log_count("hooks: detour on rva") >= lines_before + 48, "each install logged its stop");
    size_t with_unstarted = 0;  // Wine only (Windows makes a thread and its TEB at once): not asserted
    {
        std::lock_guard lock(g_log_mutex);
        for (const auto& l : g_lines)
            if (l.find("hooks: detour on rva") != std::string::npos && l.find(" 0 not started yet") == std::string::npos &&
                l.find("not started yet") != std::string::npos)
                ++with_unstarted;
    }
    std::printf("  info: %zu of the installs stopped a thread that had not started yet\n", with_unstarted);
    A->revoke_owner(6);
}

// The first install stops every other thread of the process. Many of them (the game has dozens): all must come back.
void test_many_threads_stopped() {
    std::puts("-- a first install with 64 other threads alive stops them, writes the jump and resumes every one");
    HANDLE go = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<HANDLE> th;
    static std::atomic<int> started{0};
    started = 0;
    for (int i = 0; i < 64; ++i)
        th.push_back(CreateThread(
            nullptr, 0,
            [](LPVOID ev) -> DWORD {
                ++started;
                return WaitForSingleObject(static_cast<HANDLE>(ev), kMustEndMs) == WAIT_OBJECT_0 ? 7 : 1;
            },
            go, 0, nullptr));
    // every thread runs its own code (it is past its start), its next step the wait
    check(until([] { return started.load() == 64; }), "64 threads started and are at their wait");
    static int (*io)(int) = nullptr;
    static int (*io2)(int) = nullptr;
    static auto ih = +[](int x) -> int { return io(x); };
    static auto ih2 = +[](int x) -> int { return io2(x); };
    const auto o = ord(0, "t/idle");
    const size_t lines_before = log_count("hooks: detour on rva");
    // two first installs in a row: each lists, stops and resumes all threads again
    bool ok = A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_asm_idle), &o, reinterpret_cast<void*>(ih),
                            reinterpret_cast<void**>(&io), nullptr) == KILN_OK;
    ok = A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_asm_idle2), &o, reinterpret_cast<void*>(ih2),
                       reinterpret_cast<void**>(&io2), nullptr) == KILN_OK && ok;
    SetEvent(go);
    bool all = true;
    for (HANDLE h : th) {
        DWORD code = 0;
        all = all && WaitForSingleObject(h, kMustEndMs) == WAIT_OBJECT_0 && GetExitCodeThread(h, &code) && code == 7;
        CloseHandle(h);
    }
    CloseHandle(go);
    check(ok && game_asm_idle(1) == 2 && game_asm_idle2(1) == 2, "the detours are in and work");
    check(all, "all 64 threads were resumed (none left stopped)");
    check(log_count("hooks: detour on rva") == lines_before + 2, "kiln.log reports each stop");
    size_t least = SIZE_MAX;
    {
        std::lock_guard lock(g_log_mutex);
        int shown = 0;
        for (auto it = g_lines.rbegin(); it != g_lines.rend() && shown < 2; ++it)
            if (const size_t at = it->find("hooks: detour on rva"); at != std::string::npos) {
                std::printf("  info: %s\n", it->c_str());
                const size_t w = it->find(" written with ", at);
                least = std::min<size_t>(least, w == std::string::npos ? 0 : std::strtoull(it->c_str() + w + 14, nullptr, 10));
                ++shown;
            }
    }
    check(least >= 64, "each install stopped the 64 waiting threads (and more)", "least " + std::to_string(least));
    check(log_count("could not be opened or stopped") == 0, "no thread failed to open or stop (the count is of stopped threads)");
    A->revoke_owner(6);
}

// ---- the client header
const KilnHooksApi* g_fake_table = nullptr;
const void* KILN_CALL fake_service(KilnStr name, uint32_t min_version) {
    if (kiln::sv(name) != "kiln.hooks") return nullptr;
    return g_fake_table && g_fake_table->header.version >= min_version ? g_fake_table : nullptr;
}
kiln::RawHook g_hdr;
kiln::RawHook g_hdr_moved;
uintptr_t g_hdr_caller = 0;
uint64_t hdr_fn(uint64_t a, int b) {
    g_hdr_caller = g_hdr.caller();
    ran('H');
    return g_hdr.call<uint64_t>(a + 1, b);
}
struct SameLayoutCtx {  // a safetyhook::Context stand-in: same size and field order
    KilnRegisters r;
};
void hdr_mid(SameLayoutCtx& c) {
    ran('m');
    c.r.rcx += 5;
}

void test_client_header() {
    std::puts("-- kiln_hook.hpp: RawHook and MidHook2 against the v2 table");
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &fake_service;
    g_fake_table = A;
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_hdr);
    check(g_hdr.install(&api, 6, t, reinterpret_cast<void*>(&hdr_fn), "hdr/game_hdr", 3) && g_hdr && g_hdr.handle(), "RawHook::install in place");
    take_order();
    g_probe = false;
    volatile uint64_t av = 4;
    volatile int bv = 2;
    const uint64_t r = game_hdr(av, bv);
    check(r == 5 * 10 + 2 && take_order() == "H", "call<R>(args...) continues the chain and the real function");
    check(g_hdr_caller != 0, "caller() inside the handler is the game's call site (non-zero)");
    check(g_hdr.original<uint64_t (*)(uint64_t, int)>() != nullptr, "original<T>() gives the continuation");
    kiln::MidHook2 mh = kiln::MidHook2::create(&api, 6, t, &hdr_mid, "hdr/mid", 0);
    check(static_cast<bool>(mh), "MidHook2::create takes a handler of any same-layout context type");
    take_order();
    const uint64_t r2 = game_hdr(av, bv);  // mid (prio 0) after the raw link (prio 3): rcx 4 -> raw +1 -> mid +5 = 10 -> 102
    check(r2 == (4 + 1 + 5) * 10 + 2 && take_order() == "Hm", "raw link then mid link on one target through the header", std::to_string(r2));
    mh.revoke();
    check(!mh && take_order().empty(), "MidHook2::revoke");
    g_hdr_moved = std::move(g_hdr);
    check(g_hdr_moved && !g_hdr, "RawHook moves");
    g_hdr_moved.revoke();
    check(!g_hdr_moved, "RawHook::revoke");
    uint64_t (*stale)(uint64_t, int) = nullptr;
    (void)stale;
    take_order();
    check(game_hdr(av, bv) == 4 * 10 + 2 && take_order().empty(), "after revoke the chain is empty again");
    // re-installing / assigning replaces the link instead of stacking a second one (safetyhook's assignment semantics)
    check(g_hdr.install(&api, 6, t, reinterpret_cast<void*>(&hdr_fn), "hdr/game_hdr", 3), "RawHook::install again");
    const KilnHandle first = g_hdr.handle();
    check(g_hdr.install(&api, 6, t, reinterpret_cast<void*>(&hdr_fn), "hdr/game_hdr", 3) && g_hdr.handle() != first,
          "RawHook::install on a live hook");
    take_order();
    check(game_hdr(av, bv) == 5 * 10 + 2 && take_order() == "H" && chain::chain_of(t).size() == 1,
          "install() on a live hook replaced the link (one link, the handler runs once)");
    g_hdr = kiln::RawHook::create(&api, 6, t, reinterpret_cast<void*>(&hdr_fn), "hdr/game_hdr", 3);
    take_order();
    check(g_hdr && game_hdr(av, bv) == 5 * 10 + 2 && take_order() == "H" && chain::chain_of(t).size() == 1,
          "g = RawHook::create(...) over a live hook revokes the old link");
    g_hdr = {};
    take_order();
    check(!g_hdr && g_hdr.original<uint64_t (*)(uint64_t, int)>() != nullptr && chain::chain_of(t).empty() &&
              game_hdr(av, bv) == 4 * 10 + 2 && take_order().empty(),
          "g = {} revokes the link and keeps original() valid for a handler still running");
    kiln::MidHook2 m1 = kiln::MidHook2::create(&api, 6, t, &hdr_mid, "hdr/mid", 0);
    m1 = kiln::MidHook2::create(&api, 6, t, &hdr_mid, "hdr/mid", 0);
    check(m1 && chain::chain_of(t).size() == 1, "MidHook2 assignment revokes the old link");
    m1 = {};
    check(!m1 && chain::chain_of(t).empty(), "MidHook2 = {} revokes");
    // a core that only has v1
    KilnHooksApi v1 = *A;
    v1.header.version = 1;
    v1.header.size = offsetof(KilnHooksApi, raw_detour);
    g_fake_table = &v1;
    kiln::RawHook none;
    check(!none.install(&api, 6, t, reinterpret_cast<void*>(&hdr_fn), "x") && !none && none.error() != nullptr,
          "a v1-only kiln.hooks makes install() fail with an error text", none.error() ? none.error() : "");
    g_fake_table = A;
}

void test_thread_tables() {
    std::puts("-- per-thread caller tables are recycled");
    const size_t before = chain::thread_tables();
    for (int round = 0; round < 3; ++round) {
        std::vector<std::thread> th;
        for (int k = 0; k < 8; ++k) th.emplace_back([] { game_stress(1); });
        for (auto& t : th) t.join();
    }
    const size_t after = chain::thread_tables();
    check(after <= before + 8 + 2, "24 short-lived threads reuse the tables of finished ones", "before=" + std::to_string(before) + " after=" + std::to_string(after));
    // a thread's first hooked call (table lookup, recycling: OpenThread on finished threads) keeps its last-error value
    DWORD err = 0;
    std::thread([&] {
        SetLastError(0xBEEF);
        game_stress(1);
        err = GetLastError();
    }).join();
    check(err == 0xBEEF, "the first hooked call of a thread preserves GetLastError()", std::to_string(err));
}


// ---- finding 1: mid links keep safetyhook's trampoline_rsp contract (v1 mid and mid2)
std::atomic<int> g_er_mode{0};  // 0 = leave alone, 1 = early return (rax = 777, trampoline_rsp = rsp), 2 = rip -> alt
std::atomic<int> g_er_hits{0};
void KILN_CALL er_cb(KilnRegisters* r, void*) {
    ++g_er_hits;
    const int m = g_er_mode.load();
    if (m == 1) {
        r->rax = 777;
        r->trampoline_rsp = r->rsp;  // safetyhook's rule: the top of that stack is where execution resumes
    } else if (m == 2) {
        r->rip = reinterpret_cast<uintptr_t>(&game_asm_alt);
    }
}

void test_mid_entry_semantics() {
    std::puts("-- mid links at a function entry: early return with trampoline_rsp = rsp, rip redirect, no change (v1 mid, mid2)");
    KilnRegisters probe{};
    check(reinterpret_cast<uint8_t*>(&probe.trampoline_rsp) - reinterpret_cast<uint8_t*>(&probe) == 0x188 &&
              reinterpret_cast<uint8_t*>(&probe.rip) - reinterpret_cast<uint8_t*>(&probe) == 0x190,
          "KilnRegisters keeps Context64's offsets");
    KilnHandle h1 = 0, h2 = 0;
    check(A->mid(6, reinterpret_cast<uintptr_t>(&game_asm_er1), 0, er_cb, nullptr, &h1) == KILN_OK, "v1 mid at an entry");
    const auto o = ord(0, "t/er2");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_er2), &o, er_cb, nullptr, &h2) == KILN_OK, "mid2 at an entry");
    struct {
        int (*fn)(int);
        const char* api;
    } cases[] = {{&game_asm_er1, "v1 mid"}, {&game_asm_er2, "mid2"}};
    for (auto& c : cases) {
        void* f = reinterpret_cast<void*>(c.fn);
        g_er_hits = 0;
        g_er_mode = 0;
        const int a = kiln_check_call(f, 41);
        g_er_mode = 1;
        const int b = kiln_check_call(f, 41);
        g_er_mode = 2;
        const int d = kiln_check_call(f, 41);
        g_er_mode = 0;
        // the same from a fresh thread (its first hooked call takes the slow path)
        int e = 0;
        g_er_mode = 1;
        std::thread([&] { e = kiln_check_call(f, 5); }).join();
        g_er_mode = 0;
        check(a == 42, (std::string(c.api) + ": a handler that changes nothing: the function runs, the stack is intact").c_str(),
              std::to_string(a));
        check(b == 777, (std::string(c.api) + ": early return (rax = 777, trampoline_rsp = rsp) returns to the caller with its stack").c_str(),
              std::to_string(b));
        check(d == 1041, (std::string(c.api) + ": rip redirected to another function at the entry").c_str(), std::to_string(d));
        check(e == 777 && g_er_hits == 4, (std::string(c.api) + ": early return on a thread's first hooked call").c_str(),
              std::to_string(e));
    }
    // in a chain: a mid link that changes nothing, then one that returns early; the raw link after them never runs
    static std::atomic<int> plain{0}, raw_after{0};
    static int (*o3)(int) = nullptr;
    static auto plain_cb = +[](KilnRegisters*, void*) { ++plain; };
    static auto raw3 = +[](int x) -> int { ++raw_after; return o3(x); };
    const auto op = ord(10, "t/er3-plain"), oe = ord(5, "t/er3-early"), orr = ord(0, "t/er3-raw");
    bool ok = A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_er3), &op, plain_cb, nullptr, nullptr) == KILN_OK;
    ok = A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_er3), &oe, er_cb, nullptr, nullptr) == KILN_OK && ok;
    ok = A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_asm_er3), &orr, reinterpret_cast<void*>(raw3),
                       reinterpret_cast<void**>(&o3), nullptr) == KILN_OK && ok;
    g_er_mode = 1;
    const int r = kiln_check_call(reinterpret_cast<void*>(&game_asm_er3), 1);
    g_er_mode = 0;
    const int r2 = kiln_check_call(reinterpret_cast<void*>(&game_asm_er3), 1);
    check(ok && r == 777 && plain == 2 && raw_after == 1 && r2 == 2,
          "mid, early-return mid, raw on one target: the early return skips the rest of the chain, stack intact",
          std::to_string(r) + "/" + std::to_string(r2));
    A->revoke_owner(6);
}

// ---- findings 3 and 8: RawHook re-installed on a live hook while threads call it
kiln::RawHook g_rr;
thread_local int t_rr_depth = 0;
std::atomic<int> g_rr_max{0};
std::atomic<uint64_t> g_rr_runs{0};
uint64_t rr_fn(uint64_t x) {
    const int d = ++t_rr_depth;
    int m = g_rr_max.load();
    while (d > m && !g_rr_max.compare_exchange_weak(m, d)) {}
    g_rr_runs.fetch_add(1);
    uint64_t r = 0xDEAD;
    if (d < 64) r = g_rr.call<uint64_t>(x);  // stop long before the stack overflows: the check below fails instead
    --t_rr_depth;
    return r;
}

void test_reinstall_live(int mode) {
    static const char* names[] = {"priority raised on each re-install", "same priority", "g = RawHook::create(...) with raised priority",
                                  "a new label each time, raised priority"};
    std::printf("-- RawHook re-installed on a live hook 300 times while 4 threads call it: %s\n", names[mode]);
    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &fake_service;
    g_fake_table = A;
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_rr);
    static uint64_t (*other_o)(uint64_t) = nullptr;  // another link after it: must run exactly once per call too
    static std::atomic<uint64_t> other_runs{0};
    static auto other = +[](uint64_t x) -> uint64_t { ++other_runs; return other_o(x); };
    const auto oo = ord(-100, "t/rr-other");
    bool ok = A->raw_detour(5, t, &oo, reinterpret_cast<void*>(other), reinterpret_cast<void**>(&other_o), nullptr) == KILN_OK;
    ok = g_rr.install(&api, 6, t, reinterpret_cast<void*>(&rr_fn), "t/rr", 0) && ok;
    g_rr_max = 0;
    g_rr_runs = 0;
    other_runs = 0;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> wrong{0}, calls{0};
    const uint64_t real0 = g_real.load();
    std::vector<std::thread> th;
    for (int i = 0; i < 4; ++i)
        th.emplace_back([&] {
            volatile uint64_t v = 10;
            while (!stop.load(std::memory_order_relaxed)) {
                if (game_rr(v) != 35) wrong.fetch_add(1);
                calls.fetch_add(1);
            }
        });
    for (int i = 1; i <= 300 && ok; ++i) {
        const int prio = mode == 1 ? 0 : i;
        if (mode == 2) {
            g_rr = kiln::RawHook::create(&api, 6, t, reinterpret_cast<void*>(&rr_fn), "t/rr", prio);
            ok = static_cast<bool>(g_rr);
        } else if (mode == 3) {
            const std::string lab = "t/rr-" + std::to_string(i);
            ok = g_rr.install(&api, 6, t, reinterpret_cast<void*>(&rr_fn), lab.c_str(), prio);
        } else {
            ok = g_rr.install(&api, 6, t, reinterpret_cast<void*>(&rr_fn), "t/rr", prio);
        }
        if (i % 50 == 0) {  // the callers go through the chain as it is now
            const uint64_t c = calls.load();
            ok = until([&] { return calls.load() > c + 4; }) && ok;
        }
    }
    stop = true;
    for (auto& x : th) x.join();
    const uint64_t n = calls.load(), real = g_real.load() - real0;
    check(ok && chain::chain_of(t).size() == 2, "every re-install succeeded; one link of the handler is left");
    check(g_rr_max == 1, "the handler never ran nested (no recursion through the shared original)",
          "deepest nesting " + std::to_string(g_rr_max.load()));
    check(wrong == 0 && g_rr_runs == n && other_runs == n && real == n,
          "each call ran the handler once, the link after it once and the real function once",
          "calls " + std::to_string(n) + ", handler " + std::to_string(g_rr_runs.load()) + ", other " +
              std::to_string(other_runs.load()) + ", real " + std::to_string(real) + ", wrong " + std::to_string(wrong.load()));
    g_rr.revoke();
    A->revoke_owner(5);
    A->revoke_owner(6);
}

// ---- finding 4: revoked links leave the chain order and an identical re-install takes the old slot again
void test_revival() {
    std::puts("-- revival: the same handler installed again after a revoke takes its old link slot");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_rv);
    static std::atomic<int> sruns{0};
    static auto sfn = +[](KilnHookCall2* c, void*) -> uintptr_t { ++sruns; return A->call_original2(c); };
    static int tag = 0;
    const auto o = ord(0, "t/rev-s");
    KilnHandle h1 = 0, h2 = 0, h3 = 0;
    A->detour2(6, t, &o, 1, sfn, &tag, &h1);
    const size_t n0 = chain::link_count();
    check(A->revoke(h1) == KILN_OK && A->detour2(6, t, &o, 1, sfn, &tag, &h2) == KILN_OK, "structured: revoke, install again");
    check(chain::link_count() == n0 && h2 != h1, "...it took the same slot with a new handle", std::to_string(chain::link_count() - n0));
    check(A->revoke(h1) == KILN_ERR_NOT_FOUND && chain::chain_of(t).size() == 1, "the old handle no longer revokes it");
    sruns = 0;
    check(game_rv(1) == 9 && sruns == 1, "the revived link runs");
    const auto o2 = ord(0, "t/rev-s2");
    A->detour2(6, t, &o2, 1, sfn, &tag, &h3);
    check(chain::link_count() == n0 + 1, "another label is another link");
    A->revoke(h2);
    A->revoke(h3);
    for (int i = 0; i < 50; ++i) {  // a service that toggles its hook
        KilnHandle h = 0;
        A->detour2(6, t, &o, 1, sfn, &tag, &h);
        A->revoke(h);
    }
    check(chain::link_count() == n0 + 1, "50 install/revoke rounds of one handler take no new slot");
    // raw: a thread inside the old incarnation's handler finishes correctly after the revival
    static uint64_t (*ro)(uint64_t) = nullptr;
    static std::atomic<int> rruns{0}, inside{0};
    static std::atomic<bool> hold{false};
    static auto rfn = +[](uint64_t x) -> uint64_t {
        ++rruns;
        if (hold.load()) {
            ++inside;
            while (hold.load()) _mm_pause();
        }
        return ro(x);
    };
    const auto orw = ord(0, "t/rev-raw");
    KilnHandle r1 = 0, r2 = 0;
    A->raw_detour(6, t, &orw, reinterpret_cast<void*>(rfn), reinterpret_cast<void**>(&ro), &r1);
    const size_t n1 = chain::link_count();
    void* const cont1 = reinterpret_cast<void*>(ro);
    rruns = 0;
    g_real = 0;
    hold = true;
    uint64_t got = 0;
    std::thread th([&] { got = game_rv(2); });
    if (!until([] { return inside.load() != 0; })) hold = false;  // the check below fails
    A->revoke(r1);
    A->raw_detour(6, t, &orw, reinterpret_cast<void*>(rfn), reinterpret_cast<void**>(&ro), &r2);
    hold = false;
    th.join();
    check(chain::link_count() == n1 && reinterpret_cast<void*>(ro) == cont1, "raw: revived in its slot, the same continuation");
    check(got == 12 && rruns == 1 && g_real == 1, "a call inside the old incarnation goes on once to the real function",
          std::to_string(got) + " runs " + std::to_string(rruns.load()));
    rruns = 0;
    check(game_rv(2) == 12 && rruns == 1, "a new call runs the revived link once");
    // the map and stats walk live links only
    size_t live_rows = 0;
    for (const auto& ti : chain::targets())
        if (ti.address == t) live_rows = ti.links.size();
    check(live_rows == 1, "targets(): only the live link is listed", std::to_string(live_rows));
    KilnHooksStats st{};
    st.size = sizeof st;
    const KilnResult src = A->stats(&st);
    check(src == KILN_OK && st.links_used == chain::link_count() && st.links_cap == chain::kMaxLinks,
          "stats: links_used / links_cap", std::to_string(st.links_used) + "/" + std::to_string(st.links_cap));
    struct V2Stats {  // dev.21's first v2 layout (without links_used/links_cap)
        uint32_t size, a, b, c, d;
        uint64_t e, f;
        uint32_t raw_links, fb, fo, targets;
    } old{};
    old.size = sizeof old;
    check(A->stats(reinterpret_cast<KilnHooksStats*>(&old)) == KILN_OK && old.targets == st.targets && old.raw_links == st.raw_links,
          "stats: the earlier v2 size still gets its v2 fields");
    A->revoke_owner(6);
}

// ---- finding 2: the map writer never throws
void test_map_robust() {
    std::puts("-- the hook map: an invalid UTF-8 label and a throwing extension");
    static uint64_t (*o)(uint64_t) = nullptr;
    static auto fn = +[](uint64_t x) -> uint64_t { return o(x); };
    const auto bad = ord(0, "t/bad\xff\xfe label");
    check(A->raw_detour(6, reinterpret_cast<uintptr_t>(&game_ord3), &bad, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&o),
                        nullptr) == KILN_OK, "a link with an invalid UTF-8 label");
    static auto thrower = +[](void* j, const char*) {
        auto& doc = *static_cast<nlohmann::ordered_json*>(j);
        nlohmann::ordered_json x = 5;
        x.push_back(1);  // json type_error
        doc["never"] = x;
    };
    kiln::hooks::add_map_extension(thrower);
    bool all = true;
    for (int i = 0; i < 2; ++i) {
        size_t len = 0;
        A->map_json(nullptr, 0, &len);
        std::string buf(len + 1, '\0');
        const KilnResult rc = A->map_json(buf.data(), buf.size(), &len);
        json j = json::parse(buf.data(), buf.data() + len, nullptr, false);
        bool label = false;
        if (j.is_object())
            for (auto& t : j["targets"])
                for (auto& l : t["links"]) label = label || l.value("label", "").rfind("t/bad", 0) == 0;
        all = all && rc == KILN_OK && j.is_object() && j.contains("overlaps") && !j.contains("never") && label &&
              j["linkSlots"].value("cap", 0) == 65536 && j["linkSlots"].value("used", 0) > 0;
    }
    check(all, "map_json works twice: the label is replaced, the other extensions' sections stay, linkSlots is there");
    check(log_count("hook-map extension failed") == 1, "the failing extension is logged once");
    // The map timer (armed by every change since boot) may write the file too, at any moment: look at what this call
    // wrote, not at what the file holds when it is read.
    size_t writes_before = 0;
    {
        std::lock_guard lock(g_map_writes_mutex);
        writes_before = g_map_writes.size();
    }
    kiln::hooks::write_map("test");
    bool written = false;
    {
        std::lock_guard lock(g_map_writes_mutex);
        for (size_t i = writes_before; i < g_map_writes.size(); ++i) {
            const json w = json::parse(g_map_writes[i], nullptr, false);
            written = written || (w.is_object() && w.value("phase", "") == "test");
        }
    }
    check(written, "hooks.json is still written");
    A->revoke_owner(6);
}

// ---- finding 5: a refused address costs no stub memory and no new log line
void test_refused_target() {
    std::puts("-- a target whose first detour is refused: refused again at once, retried when its bytes change");
    static int (*o)(int) = nullptr;
    static auto fn = +[](int x) -> int { return o(x) + 100; };
    const auto ob = ord(0, "t/bad-bytes");
    const uintptr_t t = reinterpret_cast<uintptr_t>(&game_asm_bad[0]);
    const size_t c0 = chain::code_bytes_used(), l0 = log_count("could not detour");
    const KilnResult r1 = A->raw_detour(6, t, &ob, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&o), nullptr);
    const size_t c1 = chain::code_bytes_used();
    const KilnResult r2 = A->raw_detour(6, t, &ob, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&o), nullptr);
    const KilnResult r3 = A->raw_detour(6, t, &ob, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&o), nullptr);
    check(r1 == KILN_ERR_UNAVAILABLE && log_count("could not detour") == l0 + 1, "the first attempt fails and is logged",
          std::to_string(r1));
    check(r2 == KILN_ERR_UNAVAILABLE && r3 == KILN_ERR_UNAVAILABLE && chain::code_bytes_used() == c1 &&
              log_count("could not detour") == l0 + 1,
          "the next attempts are refused at once: no stub memory, no new line", std::to_string(c1 - c0) + " bytes once");
    const uint8_t good[] = {0x53, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xC8, 0x83, 0xC0, 0x01, 0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3};
    DWORD old = 0;
    VirtualProtect(game_asm_bad, sizeof good, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(game_asm_bad, good, sizeof good);
    VirtualProtect(game_asm_bad, sizeof good, old, &old);
    FlushInstructionCache(GetCurrentProcess(), game_asm_bad, sizeof good);
    const KilnResult r4 = A->raw_detour(6, t, &ob, reinterpret_cast<void*>(fn), reinterpret_cast<void**>(&o), nullptr);
    check(r4 == KILN_OK && reinterpret_cast<int (*)(int)>(t)(1) == 102,
          "after the bytes changed it is tried again and works", std::to_string(r4));
    check(chain::code_bytes_used() == c1 + 48, "the failed target's entry stub was used again (only the link's stub is new)",
          std::to_string(chain::code_bytes_used() - c1));
    A->revoke_owner(6);
}

// ---- findings 7 and 10: the map does not look symbols up again; mid calls are counted per thread
void test_symbol_cache_and_counts() {
    std::puts("-- the map reuses each target's symbol name; dispatch counts stay per thread");
    size_t len = 0;
    A->map_json(nullptr, 0, &len);
    const int a = kiln::sigdb::g_sym_calls.load();
    A->map_json(nullptr, 0, &len);
    A->map_json(nullptr, 0, &len);
    const int b = kiln::sigdb::g_sym_calls.load();
    chain::refresh_symbols();
    A->map_json(nullptr, 0, &len);
    const int c = kiln::sigdb::g_sym_calls.load();
    check(b == a && c > b, "two map builds look up no symbol; after refresh_symbols() each target is asked once",
          std::to_string(b - a) + ", then " + std::to_string(c - b));
    static auto m = +[](KilnRegisters*, void*) {};
    const auto o = ord(0, "t/count");
    A->mid2(6, reinterpret_cast<uintptr_t>(&game_asm_mid), &o, m, nullptr, nullptr);
    KilnHooksStats s0{}, s1{};
    s0.size = s1.size = sizeof s0;
    A->stats(&s0);
    std::vector<std::thread> th;
    for (int k = 0; k < 4; ++k)
        th.emplace_back([] {
            for (int i = 0; i < 1000; ++i) game_asm_mid(i);
        });
    for (auto& x : th) x.join();
    A->stats(&s1);
    check(s1.mid_calls - s0.mid_calls == 4000, "stats mid_calls sums the threads' own counts",
          std::to_string(s1.mid_calls - s0.mid_calls));
    A->revoke_owner(6);
}

// ---- hooks inside another module (open_module, 0.2.0-dev.29.19): XHL-Creative-Mode-Fix's mid hook in creative.dll
struct HooksModuleInfo {
    void* add;
    void* add_mid;
    void* twice;
    void* marked;
    const void* data;
};

std::wstring exe_dir() {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring w(path, n);
    return w.substr(0, w.find_last_of(L"\\/") + 1);
}

KilnStr kstr(const char* s) { return KilnStr{s, std::strlen(s)}; }

void test_foreign_module() {
    std::puts("-- hooks inside another module: refused until the module is opened; then pinned, hooked, named, scanned");
    const HMODULE dll = LoadLibraryW((exe_dir() + L"hooks-v2-module.dll").c_str());
    check(dll != nullptr, "hooks-v2-module.dll loads (another mod's DLL)");
    if (!dll) return;
    HooksModuleInfo mi{};
    const auto info = reinterpret_cast<void (*)(HooksModuleInfo*)>(reinterpret_cast<void*>(GetProcAddress(dll, "hooks_module_info")));
    if (info) info(&mi);
    check(info && mi.add && mi.add_mid && mi.marked && mi.data, "it hands out its functions");
    if (!info) return;
    const auto mod_add = reinterpret_cast<int (*)(int)>(mi.add);
    const auto mod_twice = reinterpret_cast<int (*)(int)>(mi.twice);
    const uintptr_t mid_at = reinterpret_cast<uintptr_t>(mi.add_mid);
    static auto plus100 = +[](KilnRegisters* r, void*) { r->rcx += 100; ran('X'); };
    const auto om = ord(0, "t/module-mid");
    // what netjunk's kiln.log showed: "Hooks.Mid(0x7FFB83A7F5F7): invalid argument" for an address in creative.dll
    check(A->mid2(6, mid_at, &om, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT &&
              A->mid(6, mid_at, 0, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "an address in a module nobody opened is still refused (the game's image only)");
    const bool has = A->header.size >= offsetof(KilnHooksApi, module_scan) + sizeof(void*) && A->open_module && A->module_scan;
    check(has && offsetof(KilnHooksApi, open_module) == 312 && offsetof(KilnHooksApi, module_scan) == 320 && A->header.size == 328,
          "the table carries open_module at 312 and module_scan at 320 (328 bytes)", "size " + std::to_string(A->header.size));
    if (!has) return;
    KilnHooksModule m{};
    m.size = sizeof m;
    check(A->open_module(6, kstr("hooks-v2-never-loaded.dll"), &m) == KILN_ERR_NOT_FOUND &&
              GetModuleHandleW(L"hooks-v2-never-loaded.dll") == nullptr,
          "a module that is not loaded: not found, and open_module does not load it");
    check(A->open_module(6, kstr(""), &m) == KILN_ERR_INVALID_ARGUMENT, "an empty name is refused");
    KilnHooksModule small{};
    small.size = 8;
    check(A->open_module(6, kstr("hooks-v2-module.dll"), &small) == KILN_ERR_INVALID_ARGUMENT, "a short out struct is refused");
    // one of Kiln's own modules (a copy of the test DLL under a kiln-*.dll name)
    const std::wstring own = exe_dir() + L"kiln-hooks-v2-own.dll";
    CopyFileW((exe_dir() + L"hooks-v2-module.dll").c_str(), own.c_str(), FALSE);
    const HMODULE own_h = LoadLibraryW(own.c_str());
    check(own_h && A->open_module(6, kstr("kiln-hooks-v2-own.dll"), &m) == KILN_ERR_UNAVAILABLE,
          "Kiln's own modules (kiln-*.dll) are not opened for hooks");
    if (own_h) FreeLibrary(own_h);
    check(GetModuleHandleW(L"kiln-hooks-v2-own.dll") == nullptr, "...and not pinned: it unloads");

    m = {};
    m.size = sizeof m;
    const KilnResult rc = A->open_module(6, kstr("HOOKS-V2-MODULE.DLL"), &m);
    const uintptr_t base = reinterpret_cast<uintptr_t>(dll);
    check(rc == KILN_OK && m.base == base && m.image_size > 0 && (m.flags & KILN_HOOKS_MODULE_PINNED) &&
              !(m.flags & KILN_HOOKS_MODULE_GAME) && m.code_sections >= 1 && m.code_lo <= mid_at && mid_at < m.code_hi,
          "open_module finds it by file name (any case), pins it and reports its code", std::to_string(rc));
    check(log_has("hooks: hooks-v2-module.dll [") && log_has("] opened for hooks by test"),
          "kiln.log names the module, its file in brackets as the overlap lines do, and who opened it", log_line("opened for hooks"));
    KilnHooksModule again{};
    again.size = sizeof again;
    const size_t opened_lines = log_count("opened for hooks");
    check(A->open_module(5, kstr("hooks-v2-module.dll"), &again) == KILN_OK && again.base == base &&
              log_count("opened for hooks") == opened_lines,
          "opening it again is the same module and logs nothing new");
    // pinned: every reference the test holds is given back (and more), the module stays
    for (int i = 0; i < 4; ++i) FreeLibrary(dll);
    check(GetModuleHandleW(L"hooks-v2-module.dll") == dll, "pinned: FreeLibrary no longer unloads it");

    check(A->mid2(6, base, &om, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "its headers are no hook target");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(mi.data), &om, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "its read-only data is no hook target");
    check(A->mid2(6, base + m.image_size + 0x1000, &om, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "an address past its image is no hook target");
    KilnHandle hm = 0;
    check(A->mid2(6, mid_at, &om, plus100, nullptr, &hm) == KILN_OK && hm, "a mid hook inside its code is installed");
    take_order();
    check(mod_add(1) == 102 && take_order() == "X", "the mid hook runs inside the other module and changes a register");
    check(log_line("on hooks-v2-module.dll+0x").find(" [") != std::string::npos, "kiln.log names the link's place as module+offset [file]", log_line("on hooks-v2-module.dll+0x"));
    check(!log_has("on rva 0xffff"), "...not as an rva of the game");
    // v1 mid, raw and structured links work there too
    static int (*orig_twice)(int) = nullptr;
    static auto raw_twice = +[](int x) -> int { ran('R'); return orig_twice(x) * 2; };
    static auto s2 = +[](KilnHookCall2* c, void*) -> uintptr_t { ran('S'); c->args[0] += 10; return A->call_original2(c); };
    const auto orw = ord(2, "t/module-raw"), os2 = ord(1, "t/module-s2");
    const uintptr_t twice = reinterpret_cast<uintptr_t>(mi.twice);
    check(A->raw_detour(6, twice, &orw, reinterpret_cast<void*>(raw_twice), reinterpret_cast<void**>(&orig_twice), nullptr) == KILN_OK &&
              A->detour2(6, twice, &os2, 1, s2, nullptr, nullptr) == KILN_OK,
          "raw and structured detours on a function of the module");
    check(mod_twice(1) == 24 && take_order() == "RS", "both run in order, then the module's function once", "");
    static auto v1 = +[](KilnRegisters* r, void*) { r->rcx += 1000; ran('V'); };
    check(A->mid(6, mid_at, 5, v1, nullptr, nullptr) == KILN_OK && mod_add(1) == 1102 && take_order() == "VX",
          "a v1 mid link joins the mid hook's chain (priority 5 first)");
    // the map lists the targets with module and offset
    size_t len = 0;
    A->map_json(nullptr, 0, &len);
    std::string buf(len + 1, '\0');
    A->map_json(buf.data(), buf.size(), &len);
    const json j = json::parse(buf.data(), buf.data() + len, nullptr, false);
    bool row = false;
    char want[32];
    std::snprintf(want, sizeof want, "0x%llx", static_cast<unsigned long long>(mid_at - base));
    if (j.is_object())
        for (auto& t : j["targets"])
            if (lower(t.value("module", "")) == "hooks-v2-module.dll" && t.value("offset", "") == want && t["rva"].is_null())
                row = true;
    check(row, "the hook map lists the target with module, offset and rva null", want);
    // module_scan: the live bytes of its code
    uintptr_t hits[4] = {};
    uint32_t count = 0;
    const KilnResult sr = A->module_scan(base, kstr("41 BA 4D 3C 2B 1A 89 C8 83 C0 07 C3"), hits, 4, &count);
    check(sr == KILN_OK && count == 1 && hits[0] == reinterpret_cast<uintptr_t>(mi.marked),
          "module_scan finds the one match in its code", std::to_string(sr) + " / " + std::to_string(count));
    check(A->module_scan(base, kstr("41 BA 4D 3C 2B 1A 89 C8 83 C0 08 C3"), hits, 4, &count) == KILN_ERR_NOT_FOUND && count == 0,
          "a pattern that is not there: not found");
    check(A->module_scan(base + 0x10, kstr("41 BA"), hits, 4, &count) == KILN_ERR_INVALID_ARGUMENT &&
              A->module_scan(base, kstr("zz"), hits, 4, &count) == KILN_ERR_INVALID_ARGUMENT,
          "module_scan wants an opened module's base and a valid pattern");
    // the game's exe: reported, not pinned; an address of the exe outside the game's image stays refused
    KilnHooksModule game{};
    game.size = sizeof game;
    check(A->open_module(6, kstr(exe_name_lower().c_str()), &game) == KILN_OK && (game.flags & KILN_HOOKS_MODULE_GAME) &&
              !(game.flags & KILN_HOOKS_MODULE_PINNED),
          "the game's exe answers as the game, nothing pinned");
    check(A->mid2(6, reinterpret_cast<uintptr_t>(&kiln_check_call), &om, plus100, nullptr, nullptr) == KILN_ERR_INVALID_ARGUMENT,
          "the exe's code outside the game's image is still refused");
    // typed links stay the game's: there is no exe code to check a descriptor against in another module
    KilnSigArg targ{};
    targ.name = kstr("x");
    targ.kind = KILN_SIG_S32;
    KilnSig tsig{};
    tsig.size = sizeof tsig;
    tsig.argc = 1;
    tsig.args = &targ;
    tsig.ret_kind = KILN_SIG_S32;
    static auto typed_fn = +[](KilnCallFrame*, void*) -> uintptr_t { return KILN_TYPED_CONTINUE; };
    const auto oty = ord(0, "t/module-typed");
    check(A->detour_typed_checked(6, twice, &oty, &tsig, typed_fn, nullptr, nullptr) == KILN_HOOKS_ERR_SIG_UNKNOWN &&
              log_has("typed hook on t/module-typed refused: typed hooks check their descriptor against the game exe's code"),
          "a typed hook inside the module is refused with a reason (its descriptor needs the game's code)");
    check(A->fingerprint(twice, nullptr, 0, &len) == KILN_ERR_INVALID_ARGUMENT, "fingerprint stays the game's code");
    // read_original and code_state compare with the exe file's code: they do not apply inside another module
    uint8_t orig[8];
    std::memset(orig, 0x55, sizeof orig);
    uint32_t cstate = 0x77;
    uint64_t cgen = 0x77;
    check(A->read_original(twice, orig, sizeof orig) == KILN_ERR_INVALID_ARGUMENT && orig[0] == 0x55 && orig[7] == 0x55 &&
              A->code_state(twice, 8, &cstate, &cgen) == KILN_ERR_INVALID_ARGUMENT && cstate == 0x77 && cgen == 0x77,
          "read_original and code_state refuse an address in the opened module and write nothing");
    A->revoke_owner(6);
    check(mod_add(1) == 2 && mod_twice(1) == 2 && take_order().empty(), "revoked: the module's functions run as before");
}

// ---- ur8 finding 6: the target cap (its own process: it uses up every target slot)
void test_target_cap() {
    std::puts("-- every target slot used: a new address is refused and the plugins can tell; hooked addresses still take links");
    static auto pass = +[](KilnHookCall2* c, void*) -> uintptr_t { return A->call_original2(c); };
    static int (*orig2)(int) = nullptr;
    static auto plus10 = +[](int x) -> int { return orig2(x) + 10; };
    const auto o = ord(1, "t/cap");
    KilnHooksStats st{};
    st.size = sizeof st;
    const KilnResult sr0 = A->stats(&st);
    check(sr0 == KILN_OK && st.targets == 0 && st.targets_cap == chain::kMaxTargets && !kiln::hooks_targets_full(A),
          "an empty registry reports its target cap and is not full", std::to_string(st.targets_cap));
    const uintptr_t first = reinterpret_cast<uintptr_t>(game_many);
    size_t made = 0;
    KilnResult last = KILN_OK;
    for (size_t i = 0; i < chain::kMaxTargets + 5 && last == KILN_OK; ++i) {
        last = A->detour2(6, first + 32 * i, &o, 1, pass, nullptr, nullptr);
        if (last == KILN_OK) ++made;
    }
    check(made == chain::kMaxTargets && last == KILN_ERR_UNAVAILABLE, "512 addresses, then the next one is refused with UNAVAILABLE",
          std::to_string(made) + " / " + std::to_string(last));
    check(log_has("no free target slot"), "kiln.log says why");
    st = {};
    st.size = sizeof st;
    const KilnResult sr1 = A->stats(&st);
    check(sr1 == KILN_OK && st.targets == st.targets_cap && st.targets_cap == 512 && kiln::hooks_targets_full(A),
          "stats: targets == targets_cap, and the client header says the table is full",
          std::to_string(st.targets) + "/" + std::to_string(st.targets_cap));
    // an older client's stats size gets links_used / links_cap but no targets_cap
    KilnHooksStats older{};
    older.size = static_cast<uint32_t>(offsetof(KilnHooksStats, targets_cap));
    check(A->stats(&older) == KILN_OK && older.links_cap == chain::kMaxLinks && older.targets_cap == 0 && older.targets == 512,
          "an older stats size still gets its fields and no targets_cap");
    // an address that is hooked already takes more links, and the chain runs
    const auto o2 = ord(2, "t/cap-more");
    const KilnResult more = A->raw_detour(4, first + 32 * 3, &o2, reinterpret_cast<void*>(plus10), reinterpret_cast<void**>(&orig2), nullptr);
    check(more == KILN_OK && reinterpret_cast<int (*)(int)>(first + 32 * 3)(1) == 12, "a hooked address still takes a link and the chain runs",
          std::to_string(more));
    check(A->raw_detour(4, first + 32 * 512, &o2, reinterpret_cast<void*>(plus10), reinterpret_cast<void**>(&orig2), nullptr) == KILN_ERR_UNAVAILABLE,
          "the refused address stays refused");
}

// ---- finding 4c: the link-slot cap (its own process: it uses up every slot)
void test_slots() {
    std::puts("-- every link slot used: one warning at 75%, one at 100%, then new links are refused, revivals still work");
    static auto sfn = +[](KilnHookCall2* c, void*) -> uintptr_t { return A->call_original2(c); };
    const uintptr_t ts[] = {reinterpret_cast<uintptr_t>(&game_ord1), reinterpret_cast<uintptr_t>(&game_ord2),
                            reinterpret_cast<uintptr_t>(&game_ord3), reinterpret_cast<uintptr_t>(&game_ord4),
                            reinterpret_cast<uintptr_t>(&game_late1), reinterpret_cast<uintptr_t>(&game_late2),
                            reinterpret_cast<uintptr_t>(&game_late3), reinterpret_cast<uintptr_t>(&game_late4)};
    const auto o = ord(0, "t/slot");
    KilnResult last = KILN_OK;
    size_t made = 0;
    for (uintptr_t i = 0; last == KILN_OK && i < chain::kMaxLinks + 10; ++i) {
        KilnHandle h = 0;
        last = A->detour2(6, ts[i % 8], &o, 1, sfn, reinterpret_cast<void*>(i + 1), &h);
        if (last == KILN_OK) ++made, A->revoke(h);
    }
    check(made == chain::kMaxLinks && last == KILN_ERR_UNAVAILABLE, "65536 links, then refused", std::to_string(made));
    check(log_count("of 65536 link slots are used") == 1 && log_count("all 65536 link slots are used") == 1,
          "one warning at 75% and one at 100%");
    KilnHandle h = 0;
    check(A->detour2(6, ts[3], &o, 1, sfn, reinterpret_cast<void*>(uintptr_t{4}), &h) == KILN_OK && game_ord4(1) == 7,
          "a revoked handler can still be installed again (its slot is revived)");
    KilnHooksStats st{};
    st.size = sizeof st;
    A->stats(&st);
    check(st.links_used == st.links_cap && st.links_cap == 65536 && st.handlers == 1, "stats: every slot used, one live link");
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------------------
int main(int argc, char** argv) {
    // The game's section: the range the hooks service treats as the image.
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
        if (!std::memcmp(sec[i].Name, ".game", 5)) {
            kiln::g_image.base = reinterpret_cast<uintptr_t>(base) + sec[i].VirtualAddress;
            kiln::g_image.size = (sec[i].Misc.VirtualSize + 0xFFF) & ~size_t{0xFFF};
            kiln::g_image.text = base + sec[i].VirtualAddress;
            kiln::g_image.text_size = sec[i].Misc.VirtualSize;
        }
    if (!kiln::g_image.base) {
        std::puts("FAIL no .game section");
        return 1;
    }
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    // per process: the stress run starts several copies at once, and each reads back the hooks.json it wrote
    g_state_dir = std::filesystem::path(tmp) / (L"kiln-hooks-v2-test-" + std::to_wstring(GetCurrentProcessId()));
    if (argc > 1 && !std::strcmp(argv[1], "chaos")) {  // the churn alone, many times (hunting rare chain states)
        g_service->init();
        A = static_cast<const KilnHooksApi*>(g_service->table);
        for (int i = 0; i < 20; ++i) test_chaos();
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "slots")) {
        g_service->init();
        A = static_cast<const KilnHooksApi*>(g_service->table);
        test_slots();
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        std::puts(g_fail ? "FAILED" : "all passed");
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "targets")) {
        g_service->init();
        A = static_cast<const KilnHooksApi*>(g_service->table);
        test_target_cap();
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        std::puts(g_fail ? "FAILED" : "all passed");
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "starts")) {  // test_installs_while_threads_start alone (hunting the Wine hang)
        g_service->init();
        A = static_cast<const KilnHooksApi*>(g_service->table);
        test_installs_while_threads_start();
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        std::puts(g_fail ? "FAILED" : "all passed");
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "module")) {  // the foreign-module test alone (a quick run on real Windows)
        g_service->init();
        A = static_cast<const KilnHooksApi*>(g_service->table);
        test_foreign_module();
        std::printf("%d passed, %d failed\n", g_pass, g_fail);
        std::puts(g_fail ? "FAILED" : "all passed");
        return g_fail ? 1 : 0;
    }
    if (argc > 1 && !std::strcmp(argv[1], "tls64"))  // push the caller-table slot into TlsExpansionSlots
        for (int i = 0; i < 70; ++i) TlsAlloc();
    std::printf("hooks_v2_test: game section %p +0x%zx, tls %s\n", reinterpret_cast<void*>(kiln::g_image.base),
                kiln::g_image.size, argc > 1 ? argv[1] : "normal");

    if (!g_service || !g_service->init || !g_service->init()) {
        std::puts("FAIL kiln.hooks service did not initialise");
        return 1;
    }
    // dev.21 (WP-F4): early plugins get the service before svc::init_all; safe idle takes it away again
    g_service->available = false;
    check(kiln::hooks::serve_early() && g_service->available, "serve_early() makes kiln.hooks available to early plugins");
    kiln::hooks::withdraw_early();
    check(!g_service->available, "withdraw_early() takes it away again (safe idle)");
    g_service->available = true;
    check(kiln::hooks::ensure_ready() && chain::ready(), "ensure_ready() is idempotent (core code that hooks before the services start)");
    A = static_cast<const KilnHooksApi*>(g_service->table);
    check(A->header.version == KILN_HOOKS_VERSION && A->header.size == sizeof(KilnHooksApi), "service table is v2",
          "size " + std::to_string(A->header.size));
    check(A->raw_detour && A->caller && A->detour2 && A->call_original2 && A->mid2 && A->map_json, "every v2 entry is set");

    test_code_inspection();
    test_code_range_limit();
    test_code_lock_order();
    test_code_wait_during_stop();
    test_raw_chain_9args();
    test_floats();
    test_structured();
    test_ordering();
    test_newest_first();
    test_mid();
    test_mid_registers_and_flags();
    test_v1_compat();
    test_continuation_after_revoke();
    test_faults_and_exceptions();
    test_foreign();
    test_analyze();
    test_map_file_and_extension();
    test_threads();
    test_chaos();
    test_reinstall_shares_original();
    test_first_install_under_load();
    test_parked_threads();
    test_many_threads_stopped();
    test_installs_while_threads_start();
    test_client_header();
    test_thread_tables();
    test_mid_entry_semantics();
    for (int mode = 0; mode < 4; ++mode) test_reinstall_live(mode);
    test_revival();
    test_map_robust();
    test_refused_target();
    test_symbol_cache_and_counts();
    test_foreign_module();

    {
        std::lock_guard lock(g_log_mutex);
        size_t errors = 0;
        for (const auto& l : g_lines)
            if (l[0] == 'E' && l.find("faulted") == std::string::npos && l.find("t/bad-bytes") == std::string::npos && l.find("could not detour rva") == std::string::npos) ++errors, std::printf("  unexpected log: %s\n", l.c_str());
        if (errors) ++g_fail;
    }
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    std::puts(g_fail ? "FAILED" : "all passed");
    std::error_code ec;
    std::filesystem::remove_all(g_state_dir, ec);
    return g_fail ? 1 : 0;
}
