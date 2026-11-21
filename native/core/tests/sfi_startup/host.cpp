// SPDX-License-Identifier: MIT
// the two-DLL game-free host of the startup handshake between kiln-eml and Kiln's interpreter for
// ShroudForge mods (kiln.eml.startup v1).
//
// A stand-in for kiln-core's boot with fake core services and no game: it loads the REAL kiln-eml.dll and kiln-sfi.dll
// from <game>\kiln\plugins and runs them in the core's order (native/core/src/early_plugins.cpp, boot.cpp):
//   1. every kiln-*.dll exporting kiln_plugin_early is loaded (file-name order), kiln-eml.dll first
//      (early_plugins.h provider_before), then each kiln_plugin_early (its own owner), then each early on_ready;
//   2. Kiln's plugin step: each kiln-*.dll's kiln_plugin_main (a new owner, file-name order), then each on_ready;
//   3. kiln.boot's bound() in a kiln-backend start: kiln.boot's view of the archive generation kiln-eml committed
//      (native/core/src/services/boot_archive_eml.h, the core's own code): kiln-eml's kiln.caps rows, the identity from
//      kiln.eml.startup's session-free published_identity (published_identity_of, as boot_archive_eml.cpp calls it),
//      kiln.eml.archive's view acquired with exactly those pins under kiln.boot's owner, and one OnBoot read of
//      resource:<RESOURCE_GUID>/m6.Root/0 through it ("onboot ..." lines; never a snapshot);
//   4. the game's entry point: the host waits for kiln-sfi's interpreter worker (a kiln.tasks task) to settle, then
//      the exit (kiln.tasks parks, every plugin's on_unload last loaded first) and TerminateProcess.
// Fake services: kiln.launch (backend kiln, the plan hash and policy the run script gives), kiln.profile (the game's
// kiln\profile.json), kiln.caps (reports logged), kiln.progress (its first begin asks user32 what the core's asks,
// GetProcessWindowStation, GetUserObjectInformationW and the screen size, which can load Wine's display driver; it shows
// no window; every call is logged as "progress ..."), kiln.tasks (each task on its own thread); registered services
// (kiln-eml's kiln.eml, kiln.eml.archive, kiln.eml.values) are served from the plugin step on, as the core serves them.
// The core's log member writes one line per call to the host log; it never loads a library, resolves a symbol or
// starts a thread, and formats without the C runtime, so kiln-eml and kiln-sfi may call it under the loader lock.
//
// The run script (run.py next to this file) names the inputs in the environment:
//   G14D_GAME (default: this executable's folder), G14D_LOG (the host log), G14D_EXE_SHA256 (build_info's executable
//   digest), G14D_PLAN_SHA256 and G14D_LAUNCH_ID (kiln.launch), G14D_POLICY (always | verified-only | never),
//   G14D_VERIFIED (1 | 0), G14D_KILL_AT (after-early-main | after-early-ready | after-plugin-main: the host ends itself
//   there with TerminateProcess, a kill), G14D_WORKER_WAIT_MS (how long the entry point waits for the worker),
//   G14D_WAIT_DLLMAIN (the early on_ready waits, at most 10 s, until a package DLL's initializer has begun: its
//   "attach-begin" line in G14D_DLL_LOG; the core does not wait, this makes kiln-eml's on_ready run while that
//   initializer holds the loader lock, the order a slow initializer gives a real start).
// enshrouded.exe --hold-lock <lock file> <ready file> <stop file>: holds <game>\.kiln-eml.lock as another process
// would (LockFileEx, exclusive) until the stop file exists (a busy install lock).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "services/boot_archive_eml.h"
#include "services/kiln_caps.h"
#include "services/kiln_launch.h"
#include "services/kiln_profile.h"
#include "services/kiln_progress.h"
#include "services/kiln_tasks.h"

namespace {

// ---- the host log: one line per call, no C runtime formatting, no allocation in the core's log member ----

HANDLE g_log = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_log_cs;
LARGE_INTEGER g_freq{}, g_t0{};
std::atomic<int64_t> g_last_line{0};

struct Line {
    char buf[16384];
    size_t n = 0;
    Line& s(const char* text, size_t len) {
        for (size_t i = 0; i < len && n < sizeof buf - 2; ++i) {
            const char c = text[i];
            buf[n++] = (c == '\n' || c == '\r') ? ' ' : c;
        }
        return *this;
    }
    Line& s(const char* text) { return s(text, std::strlen(text)); }
    Line& u(uint64_t v) {
        char tmp[24];
        int k = 0;
        do {
            tmp[k++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        while (k && n < sizeof buf - 2) buf[n++] = tmp[--k];
        return *this;
    }
    Line& i(int64_t v) {
        if (v < 0) {
            s("-");
            return u(uint64_t(-v));
        }
        return u(uint64_t(v));
    }
};

int64_t qpc() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

// "<qpc> <ms since start>.<us> t<thread> <tag> <text>"
void emit(const char* tag, const char* text, size_t len) {
    Line l;
    const int64_t now = qpc();
    const int64_t us = (now - g_t0.QuadPart) * 1000000 / g_freq.QuadPart;
    l.i(now).s(" ").i(us / 1000).s(".");
    const int64_t frac = us % 1000;
    if (frac < 100) l.s("0");
    if (frac < 10) l.s("0");
    l.i(frac).s(" t").u(GetCurrentThreadId()).s(" ").s(tag).s(" ").s(text, len);
    l.buf[l.n++] = '\n';
    EnterCriticalSection(&g_log_cs);
    DWORD written = 0;
    if (g_log != INVALID_HANDLE_VALUE) WriteFile(g_log, l.buf, DWORD(l.n), &written, nullptr);
    LeaveCriticalSection(&g_log_cs);
    g_last_line.store(now);
}
void mark(const std::string& text) { emit("host", text.data(), text.size()); }

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring env(const wchar_t* name, const std::wstring& fallback = {}) {
    static wchar_t buf[32768];
    const DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n && n < 32768 ? std::wstring(buf, n) : fallback;
}
std::string read_file(const std::wstring& path) {
    std::string out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    static char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) out.append(buf, got);
    CloseHandle(f);
    return out;
}
bool hex_bytes(const std::wstring& text, uint8_t* out, size_t n) {
    if (text.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const wchar_t c = text[i * 2 + k];
            v <<= 4;
            if (c >= L'0' && c <= L'9') v |= unsigned(c - L'0');
            else if (c >= L'a' && c <= L'f') v |= unsigned(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') v |= unsigned(c - L'A' + 10);
            else return false;
        }
        out[i] = uint8_t(v);
    }
    return true;
}

// ---- configuration ----

std::wstring g_game;  // Windows form, no trailing separator
std::string g_game_utf8;
std::string g_profile;  // <game>\kiln\profile.json as read
std::string g_exe_sha;  // 64 lower-case hex digits
uint8_t g_plan_hash[32]{};
uint64_t g_launch_id = 0x14d0000000000042ull;
uint32_t g_backend = KILN_LAUNCH_BACKEND_KILN, g_reason = KILN_LAUNCH_REASON_PLAN;
uint32_t g_policy = KILN_LAUNCH_NATIVE_POLICY_ALWAYS, g_verified = 1;
std::wstring g_kill_at;
std::atomic<bool> g_plugin_step{false};  // services past the early set, build_info's verified: from the plugin step on

void kill_point(const wchar_t* point) {
    if (g_kill_at != point) return;
    mark("kill at " + utf8(point));
    TerminateProcess(GetCurrentProcess(), 99);
}

// ---- KilnApi ----

std::string text_of(KilnStr s) { return s.ptr && s.len ? std::string(s.ptr, s.len) : std::string(); }

KilnStr KILN_CALL api_version() {
    static const char v[] = "0.2.0-dev.29.21";
    return KilnStr{v, sizeof v - 1};
}
void KILN_CALL api_log(KilnOwner owner, int32_t level, KilnStr text) {
    Line l;
    l.s("log o=").u(owner).s(" l=").i(level).s(" ");
    if (text.ptr) l.s(text.ptr, text.len);
    emit("core", l.buf, l.n);
}
KilnResult KILN_CALL api_build_info(KilnBuildInfo* out) {
    if (!out || out->size < offsetof(KilnBuildInfo, kiln_version) + sizeof(KilnStr)) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size < sizeof *out ? size : uint32_t(sizeof *out);
    out->side = KILN_SIDE_CLIENT;
    std::memcpy(out->exe_sha256, g_exe_sha.data(), std::min<size_t>(64, g_exe_sha.size()));
    // the core sets the verified answer after the early plugins ran (boot.cpp set_runtime_state)
    out->verified = g_plugin_step.load() ? uint8_t(g_verified) : 0;
    out->hooks_enabled = 1;
    out->kiln_version = api_version();
    return KILN_OK;
}
bool KILN_CALL api_feature(KilnStr) { return false; }
KilnResult KILN_CALL api_path(int32_t kind, char* buffer, size_t capacity, size_t* out_len) {
    static const char* const tails[] = {"", "\\kiln", "\\kiln\\mods", "\\kiln\\logs", "\\kiln\\data", "\\kiln\\control",
                                        "\\kiln\\config", "\\kiln\\state"};
    if (kind < 0 || kind > 7 || !out_len) return KILN_ERR_INVALID_ARGUMENT;
    const std::string p = g_game_utf8 + tails[kind];
    *out_len = p.size();
    if (!buffer || capacity < p.size()) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, p.data(), p.size());
    return KILN_OK;
}
std::atomic<uint64_t> g_owners{0};
KilnOwner KILN_CALL api_owner_create(KilnStr id) {
    const KilnOwner o = ++g_owners;
    mark("owner " + std::to_string(o) + " " + text_of(id));
    return o;
}
void KILN_CALL api_owner_revoke(KilnOwner owner) { mark("revoke " + std::to_string(owner)); }
KilnResult KILN_CALL api_subscribe_tick(KilnOwner, int32_t, KilnTickFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL api_unsubscribe(KilnHandle) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL api_post(KilnOwner, KilnTaskFn, void*) { return KILN_ERR_UNAVAILABLE; }
bool KILN_CALL api_on_tick_thread() { return false; }
KilnResult KILN_CALL api_tick_info(KilnTickInfo*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL api_subscribe_log(KilnOwner, KilnLogLineFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL api_symbol(KilnStr, uintptr_t*) { return KILN_ERR_NOT_FOUND; }
KilnResult KILN_CALL api_constant(KilnStr, int64_t*) { return KILN_ERR_NOT_FOUND; }
uintptr_t KILN_CALL api_server_world() { return 0; }
uintptr_t KILN_CALL api_singleton(uintptr_t, KilnStr) { return 0; }
uint32_t KILN_CALL api_side() { return KILN_SIDE_CLIENT; }
uintptr_t KILN_CALL api_world(uint32_t) { return 0; }
bool KILN_CALL api_on_world_thread(uint32_t) { return false; }
KilnResult KILN_CALL api_subscribe_world_tick(KilnOwner, uint32_t, int32_t, KilnTickFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL api_post_world(KilnOwner, uint32_t, KilnTaskFn, void*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL api_subscribe_world(KilnOwner, KilnWorldEventFn, void*, KilnHandle* out) {
    if (out) *out = 0;
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL api_session(KilnSessionInfo* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof *out);
    out->size = size;
    out->state = KILN_SESSION_NONE;
    out->side = KILN_SIDE_CLIENT;
    return KILN_OK;
}

// ---- kiln.launch ----

KilnResult KILN_CALL launch_status(KilnLaunchStatus* out) {
    if (!out || out->size < KILN_LAUNCH_STATUS_V1_SIZE) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t size =
        out->size >= sizeof(KilnLaunchStatus) ? uint32_t(sizeof(KilnLaunchStatus)) : KILN_LAUNCH_STATUS_V1_SIZE;
    std::memset(out, 0, size);
    out->size = size;
    out->backend = g_backend;
    out->frozen = 1;
    out->reason_code = g_reason;
    out->launch_id = g_launch_id;
    std::memcpy(out->plan_hash, g_plan_hash, 32);
    out->freeze_stamp = 1763600000000ull;
    static const char reason[] = "the g14d host froze backend kiln with the launch plan the profile names";
    std::memcpy(out->reason, reason, sizeof reason);
    if (size >= sizeof(KilnLaunchStatus)) {
        out->native_policy = g_policy;
        out->build_verified = g_backend == KILN_LAUNCH_BACKEND_NONE ? 0 : g_verified;
    }
    return KILN_OK;
}
KilnResult KILN_CALL launch_notice(char* buffer, size_t capacity, size_t* out_len) {
    static const char text[] = "Kiln's interpreter for ShroudForge mods runs this start (startup host).";
    if (out_len) *out_len = sizeof text - 1;
    if (!buffer || capacity < sizeof text - 1) return KILN_ERR_BUFFER_TOO_SMALL;
    std::memcpy(buffer, text, sizeof text - 1);
    return KILN_OK;
}
KilnLaunchApi g_launch{{sizeof(KilnLaunchApi), KILN_LAUNCH_VERSION}, launch_status, launch_notice};

// ---- kiln.profile ----

std::string g_config_dir, g_data_dir;
uint32_t KILN_CALL profile_mode() { return KILN_PROFILE_ACTIVE; }
KilnStr KILN_CALL profile_id() { return KilnStr{"g14d", 4}; }
KilnStr KILN_CALL profile_name() { return KilnStr{"g14d", 4}; }
uint64_t KILN_CALL profile_generation() { return 1; }
uint32_t KILN_CALL profile_count(uint32_t) { return 0; }
KilnResult KILN_CALL profile_get(uint32_t, uint32_t, KilnProfileMod* out) {
    return out && out->size >= sizeof *out ? KILN_ERR_NOT_FOUND : KILN_ERR_INVALID_ARGUMENT;
}
uint32_t KILN_CALL profile_dropins() { return 0; }
KilnStr KILN_CALL profile_config_dir() { return KilnStr{g_config_dir.data(), g_config_dir.size()}; }
KilnStr KILN_CALL profile_data_dir() { return KilnStr{g_data_dir.data(), g_data_dir.size()}; }
KilnStr KILN_CALL profile_json() { return KilnStr{g_profile.data(), g_profile.size()}; }
KilnProfileApi g_profile_api{{sizeof(KilnProfileApi), KILN_PROFILE_VERSION}, profile_mode, profile_id, profile_name,
                             profile_generation, profile_count, profile_get, profile_dropins, profile_config_dir,
                             profile_data_dir, profile_json};

// ---- kiln.progress: the core's first begin decides whether there is a desktop (progress.cpp enabled():
// GetProcessWindowStation, GetUserObjectInformationW, GetSystemMetrics); the stand-in makes those calls at its first
// begin, on the caller's thread, and shows nothing (begin answers 0, as the core does without a desktop) ----

std::atomic<uint64_t> g_progress_calls{0};
uint64_t KILN_CALL progress_begin(KilnStr title) {
    ++g_progress_calls;
    Line l;
    l.s("progress begin title=");
    if (title.ptr) l.s(title.ptr, title.len);
    emit("host", l.buf, l.n);
    static std::atomic<bool> decided{false};
    if (!decided.exchange(true)) {
        HWINSTA station = GetProcessWindowStation();
        USEROBJECTFLAGS flags{};
        DWORD needed = 0;
        const BOOL visible = station && GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof flags, &needed) &&
                             (flags.dwFlags & WSF_VISIBLE);
        const int cx = GetSystemMetrics(SM_CXSCREEN), cy = GetSystemMetrics(SM_CYSCREEN);
        Line d;
        d.s("progress decided visible=").u(visible ? 1 : 0).s(" cx=").i(cx).s(" cy=").i(cy);
        emit("host", d.buf, d.n);
    }
    return 0;
}
void KILN_CALL progress_update(uint64_t, KilnStr, uint64_t, uint64_t) { ++g_progress_calls; }
void KILN_CALL progress_end(uint64_t) { ++g_progress_calls; }
KilnResult KILN_CALL progress_step(KilnStr, KilnStr, KilnStr, uint64_t, uint64_t) { return KILN_OK; }
KilnResult KILN_CALL progress_mark(uint32_t) { return KILN_OK; }
KilnProgressApi g_progress_api{{sizeof(KilnProgressApi), KILN_PROGRESS_VERSION}, progress_begin, progress_update, progress_end,
                               progress_step, progress_mark};

// ---- kiln.caps: every report kept and logged; a row is the worst report for its name (unavailable, pending, then
// available), as the core shows it ----

struct CapReport {
    KilnOwner owner;
    uint32_t state, reason;
    std::string text;
};
struct CapRow {
    std::string name, text, source;
    uint32_t state = KILN_CAP_AVAILABLE, reason = KILN_CAP_REASON_NONE;
    std::vector<CapReport> reports;
};
CRITICAL_SECTION g_caps_cs;
std::vector<std::unique_ptr<CapRow>> g_cap_rows;  // sorted by name; rows (and their strings) live for the process
std::atomic<uint64_t> g_caps_generation{1};

int cap_rank(uint32_t state) { return state == KILN_CAP_UNAVAILABLE ? 0 : state == KILN_CAP_PENDING ? 1 : 2; }
void cap_fill(const CapRow& row, KilnCapRow* out) {
    out->state = row.state;
    out->reason = row.reason;
    out->flags = 0;
    out->name = KilnStr{row.name.data(), row.name.size()};
    out->text = KilnStr{row.text.data(), row.text.size()};
    out->source = KilnStr{row.source.data(), row.source.size()};
}
uint32_t KILN_CALL caps_count() {
    EnterCriticalSection(&g_caps_cs);
    const uint32_t n = uint32_t(g_cap_rows.size());
    LeaveCriticalSection(&g_caps_cs);
    return n;
}
KilnResult KILN_CALL caps_at(uint32_t index, KilnCapRow* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    EnterCriticalSection(&g_caps_cs);
    const bool found = index < g_cap_rows.size();
    if (found) cap_fill(*g_cap_rows[index], out);
    LeaveCriticalSection(&g_caps_cs);
    return found ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL caps_find(KilnStr name, KilnCapRow* out) {
    if (!out || out->size < sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    const std::string n = name.ptr ? std::string(name.ptr, name.len) : std::string();
    EnterCriticalSection(&g_caps_cs);
    bool found = false;
    for (const auto& row : g_cap_rows)
        if (row->name == n) {
            cap_fill(*row, out);
            found = true;
        }
    LeaveCriticalSection(&g_caps_cs);
    return found ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL caps_report(KilnOwner owner, KilnStr name, uint32_t state, uint32_t reason, KilnStr text) {
    Line l;
    l.s("caps o=").u(owner).s(" ");
    if (name.ptr) l.s(name.ptr, name.len);
    l.s(" state=").u(state).s(" reason=").u(reason).s(" ");
    if (text.ptr) l.s(text.ptr, text.len);
    emit("core", l.buf, l.n);
    if (!owner || !name.ptr || !name.len || state > KILN_CAP_PENDING || (state == KILN_CAP_AVAILABLE) != (reason == 0))
        return KILN_ERR_INVALID_ARGUMENT;
    const std::string n(name.ptr, name.len);
    EnterCriticalSection(&g_caps_cs);
    auto at = std::find_if(g_cap_rows.begin(), g_cap_rows.end(), [&](const auto& r) { return r->name >= n; });
    if (at == g_cap_rows.end() || (*at)->name != n) {
        auto row = std::make_unique<CapRow>();
        row->name = n;
        at = g_cap_rows.insert(at, std::move(row));
    }
    CapRow& row = **at;
    auto mine = std::find_if(row.reports.begin(), row.reports.end(), [&](const CapReport& r) { return r.owner == owner; });
    if (mine == row.reports.end()) row.reports.push_back({owner, state, reason, text_of(text)});
    else *mine = {owner, state, reason, text_of(text)};
    const CapReport* worst = &row.reports.front();
    for (const auto& r : row.reports)
        if (cap_rank(r.state) < cap_rank(worst->state)) worst = &r;
    // the row's strings stay valid for the process: a changed text is a new string kept beside the old one
    if (row.state != worst->state || row.reason != worst->reason || row.text != worst->text) ++g_caps_generation;
    row.state = worst->state;
    row.reason = worst->reason;
    if (row.text != worst->text) row.text = worst->text;
    row.source = "owner:" + std::to_string(worst->owner);
    LeaveCriticalSection(&g_caps_cs);
    return KILN_OK;
}
KilnResult KILN_CALL caps_subscribe(KilnOwner, KilnCapsChangedFn, void*, KilnHandle* out) {
    if (out) *out = 0x5c;
    return KILN_OK;
}
KilnResult KILN_CALL caps_unsubscribe(KilnHandle) { return KILN_OK; }
uint64_t KILN_CALL caps_generation() { return g_caps_generation.load(); }
KilnCapsApi g_caps{{sizeof(KilnCapsApi), KILN_CAPS_VERSION}, caps_count, caps_at, caps_find, caps_report, caps_subscribe,
                   caps_unsubscribe, caps_generation};

// ---- kiln.tasks: each accepted task on its own thread ----

struct Task {
    KilnHandle id = 0;
    KilnOwner owner = 0;
    std::string name;
    uint32_t flags = 0;
    KilnTaskBodyFn body = nullptr;
    KilnTaskDoneFn done = nullptr;
    void* user = nullptr;
    std::atomic<uint32_t> state{KILN_TASK_QUEUED};
    std::atomic<uint32_t> cancel{0};
    std::atomic<bool> finished{false};  // done returned
    std::atomic<bool> started{false};
    bool released = false;
};
CRITICAL_SECTION g_tasks_cs;
std::vector<Task*> g_tasks;
std::atomic<uint64_t> g_task_ids{0};
thread_local Task* t_current = nullptr;

uint32_t KILN_CALL task_cancelled(const KilnTaskRun* run) {
    return run && run->internal ? static_cast<Task*>(run->internal)->cancel.load() : 1;
}
int guarded_body(Task* t, KilnTaskRun* run, DWORD* code) {
    __try {
        t->body(run, t->user);
        return 1;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
DWORD WINAPI task_thread(void* p) {
    auto* t = static_cast<Task*>(p);
    t_current = t;
    t->started = true;
    mark("task " + std::to_string(t->id) + " body-begin " + t->name);
    t->state = KILN_TASK_RUNNING;
    KilnTaskRun run{};
    run.size = sizeof run;
    run.task = t->id;
    run.owner = t->owner;
    run.cancelled = task_cancelled;
    run.internal = t;
    DWORD code = 0;
    const uint32_t state = guarded_body(t, &run, &code) ? KILN_TASK_COMPLETED : KILN_TASK_FAULTED;
    t->state = state;
    mark("task " + std::to_string(t->id) + " body-end state=" + std::to_string(state) + " code=" + std::to_string(code));
    t->done(t->id, state, t->user);
    t->finished = true;
    mark("task " + std::to_string(t->id) + " done-end");
    return 0;
}
Task* find_task(KilnOwner owner, KilnHandle id) {
    for (Task* t : g_tasks)
        if (t->id == id && t->owner == owner && !t->released) return t;
    return nullptr;
}
KilnResult KILN_CALL tasks_submit(KilnOwner owner, const KilnTaskInfo* info, KilnTaskBodyFn body, KilnTaskDoneFn done,
                                  void* user, KilnHandle* task, uint32_t* refusal) {
    auto refuse = [&](KilnResult rc, uint32_t why) {
        if (task) *task = 0;
        if (refusal) *refusal = why;
        mark("tasks submit refused rc=" + std::to_string(rc) + " why=" + std::to_string(why));
        return rc;
    };
    if (!owner || !info || !body || !done || !task || info->size < sizeof(KilnTaskInfo) || (info->flags & ~7u) ||
        ((info->flags & KILN_TASK_RESIDENT) && !(info->flags & KILN_TASK_LONG_RUNNING)) || info->priority > 2 ||
        info->reserved || !info->name.ptr || !info->name.len || info->name.len > 128)
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_TASK_REFUSAL_ARGUMENT);
    auto* t = new Task;
    t->id = ++g_task_ids;
    t->owner = owner;
    t->name = text_of(info->name);
    t->flags = info->flags;
    t->body = body;
    t->done = done;
    t->user = user;
    EnterCriticalSection(&g_tasks_cs);
    g_tasks.push_back(t);
    LeaveCriticalSection(&g_tasks_cs);
    mark("tasks submit " + std::to_string(t->id) + " owner=" + std::to_string(owner) + " flags=" +
         std::to_string(t->flags) + " " + t->name);
    HANDLE thread = CreateThread(nullptr, 0, task_thread, t, 0, nullptr);
    if (!thread) return refuse(KILN_ERR_UNAVAILABLE, KILN_TASK_REFUSAL_CAPABILITY);
    CloseHandle(thread);
    *task = t->id;
    return KILN_OK;
}
KilnResult KILN_CALL tasks_cancel(KilnOwner owner, KilnHandle id, uint32_t* state) {
    EnterCriticalSection(&g_tasks_cs);
    Task* t = find_task(owner, id);
    if (t) t->cancel = 1;
    if (state) *state = t ? t->state.load() : uint32_t(KILN_TASK_NONE);
    LeaveCriticalSection(&g_tasks_cs);
    return t ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL tasks_status(KilnOwner owner, KilnHandle id, uint32_t* state) {
    EnterCriticalSection(&g_tasks_cs);
    Task* t = find_task(owner, id);
    if (state) *state = t ? t->state.load() : uint32_t(KILN_TASK_NONE);
    LeaveCriticalSection(&g_tasks_cs);
    return t ? KILN_OK : KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL tasks_release(KilnOwner owner, KilnHandle id) {
    EnterCriticalSection(&g_tasks_cs);
    Task* t = find_task(owner, id);
    KilnResult rc = KILN_OK;  // unknown or released before: the fake does not tell them apart
    if (t && (t->state == KILN_TASK_QUEUED || t->state == KILN_TASK_RUNNING)) rc = KILN_ERR_UNAVAILABLE;
    else if (t) t->released = true;
    LeaveCriticalSection(&g_tasks_cs);
    return rc;
}
uint32_t outstanding(KilnOwner owner) {
    uint32_t n = 0;
    EnterCriticalSection(&g_tasks_cs);
    for (Task* t : g_tasks)
        if ((!owner || t->owner == owner) && !t->finished) ++n;
    LeaveCriticalSection(&g_tasks_cs);
    return n;
}
void cancel_all(KilnOwner owner) {
    EnterCriticalSection(&g_tasks_cs);
    for (Task* t : g_tasks)
        if (!owner || t->owner == owner) t->cancel = 1;
    LeaveCriticalSection(&g_tasks_cs);
}
KilnResult KILN_CALL tasks_drain(KilnOwner owner, uint32_t timeout_ms, uint32_t* running) {
    if (!owner || !running) return KILN_ERR_INVALID_ARGUMENT;
    if (timeout_ms && t_current) return KILN_ERR_WRONG_THREAD;
    cancel_all(owner);
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    while (outstanding(owner) && GetTickCount64() < until) Sleep(10);
    *running = outstanding(owner);
    mark("tasks drain owner=" + std::to_string(owner) + " running=" + std::to_string(*running));
    return KILN_OK;
}
KilnResult KILN_CALL tasks_submit_in(KilnOwner, const KilnTaskInfo*, uint32_t, uint32_t, KilnTaskBodyFn, KilnTaskDoneFn,
                                     void*, KilnHandle* task, uint32_t* refusal) {
    if (task) *task = 0;
    if (refusal) *refusal = KILN_TASK_REFUSAL_NO_WORLD;
    return KILN_ERR_UNAVAILABLE;
}
KilnTasksApi g_tasks_api{{sizeof(KilnTasksApi), KILN_TASKS_VERSION}, tasks_submit, tasks_cancel, tasks_status,
                         tasks_release, tasks_drain, tasks_submit_in};

// ---- services: the core's set in every view; kiln.tasks and what plugins registered from the plugin step on ----

struct Registered {
    std::string name;
    const void* table;
};
CRITICAL_SECTION g_services_cs;
std::vector<Registered> g_registered;

const void* KILN_CALL api_service(KilnStr name, uint32_t min_version) {
    const std::string n = text_of(name);
    const void* table = nullptr;
    if (n == KILN_LAUNCH_SERVICE) table = &g_launch;
    else if (n == KILN_PROFILE_SERVICE) table = &g_profile_api;
    else if (n == KILN_CAPS_SERVICE) table = &g_caps;
    else if (n == KILN_PROGRESS_SERVICE) table = &g_progress_api;  // the core serves it to early plugins too
    else if (g_plugin_step.load()) {
        if (n == KILN_TASKS_SERVICE) table = &g_tasks_api;
        EnterCriticalSection(&g_services_cs);
        for (const auto& r : g_registered)
            if (r.name == n) table = r.table;
        LeaveCriticalSection(&g_services_cs);
    }
    if (table && static_cast<const KilnServiceHeader*>(table)->version < min_version) table = nullptr;
    return table;
}
KilnResult KILN_CALL api_register_service(KilnOwner owner, KilnStr name, const void* table) {
    const std::string n = text_of(name);
    if (!owner || n.empty() || !table) return KILN_ERR_INVALID_ARGUMENT;
    EnterCriticalSection(&g_services_cs);
    bool taken = n == KILN_LAUNCH_SERVICE || n == KILN_PROFILE_SERVICE || n == KILN_CAPS_SERVICE || n == KILN_TASKS_SERVICE ||
                 n == KILN_PROGRESS_SERVICE;
    for (const auto& r : g_registered) taken |= r.name == n;
    if (!taken) g_registered.push_back({n, table});
    LeaveCriticalSection(&g_services_cs);
    mark("register " + n + " owner=" + std::to_string(owner) + (taken ? " refused" : ""));
    return taken ? KILN_ERR_INVALID_ARGUMENT : KILN_OK;
}

KilnApi make_api() {
    KilnApi a{};
    a.size = sizeof a;
    a.abi_version = KILN_ABI_VERSION;
    a.version = api_version;
    a.log = api_log;
    a.build_info = api_build_info;
    a.feature = api_feature;
    a.path = api_path;
    a.owner_create = api_owner_create;
    a.owner_revoke = api_owner_revoke;
    a.subscribe_tick = api_subscribe_tick;
    a.unsubscribe = api_unsubscribe;
    a.post = api_post;
    a.on_tick_thread = api_on_tick_thread;
    a.tick_info = api_tick_info;
    a.subscribe_log = api_subscribe_log;
    a.symbol = api_symbol;
    a.constant = api_constant;
    a.service = api_service;
    a.register_service = api_register_service;
    a.server_world = api_server_world;
    a.singleton = api_singleton;
    a.side = api_side;
    a.world = api_world;
    a.on_world_thread = api_on_world_thread;
    a.subscribe_world_tick = api_subscribe_world_tick;
    a.post_world = api_post_world;
    a.subscribe_world = api_subscribe_world;
    a.session = api_session;
    return a;
}
KilnApi g_api;

// ---- the boot ----

struct Plugin {
    std::wstring file;
    std::string name;  // lower case file name
    HMODULE module = nullptr;
    KilnPluginMainFn early = nullptr, main = nullptr;
    KilnOwner early_owner = 0, owner = 0;
    KilnPluginInfo early_info{}, info{};
    bool early_ok = false, ok = false;
};

int guarded_main(KilnPluginMainFn fn, KilnOwner owner, KilnPluginInfo* info, KilnResult* rc, DWORD* code) {
    __try {
        *rc = fn(&g_api, owner, info);
        return 1;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int guarded_ready(void(KILN_CALL* fn)(void*), void* user, DWORD* code) {
    __try {
        fn(user);
        return 1;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

// ---- kiln.boot's bound() in a kiln-backend start: boot_service.cpp bind_committed() over the core's own
// boot_archive_eml.h, with the identity source boot_archive_eml.cpp uses (published_identity_of over the loaded
// kiln-eml.dll's kiln_eml_startup_service export) ----

KilnEmlStartupServiceFn g_startup_getter = nullptr;
KilnResult onboot_identity(const kiln::boot_view::LaunchFacts& launch, KilnEmlArchiveIdentityV1* out, std::string* why) {
    return kiln::boot_view::published_identity_of(g_startup_getter, launch, out, why);
}
std::string hex(const uint8_t* p, size_t n) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < n; ++i) out += digits[p[i] >> 4], out += digits[p[i] & 15];
    return out;
}
// a kiln.caps row's state as the host shows it (worst report), and its text
bool cap_available(const char* name, std::string* text) {
    EnterCriticalSection(&g_caps_cs);
    bool found = false, available = false;
    for (const auto& row : g_cap_rows)
        if (row->name == name) found = true, available = row->state == KILN_CAP_AVAILABLE, *text = row->text;
    LeaveCriticalSection(&g_caps_cs);
    if (!found) *text = "no such row";
    return available;
}
// The OnBoot read every kiln-backend case makes: the synthetic archive's resource part 0 (m6_fixture archive.rs).
constexpr char kOnbootQuery[] = "resource:01234567-89ab-cdef-0123-456789abcdef/m6.Root/0";
void onboot() {
    if (g_backend != KILN_LAUNCH_BACKEND_KILN) {
        mark("onboot snapshot (not a kiln-backend start)");
        return;
    }
    // what boot_archive_eml.cpp's eml_export() does: an export of the kiln-eml.dll already loaded, no load
    const HMODULE eml = GetModuleHandleW(L"kiln-eml.dll");
    const auto archive_get = eml ? reinterpret_cast<KilnEmlArchiveServiceFn>(GetProcAddress(eml, KILN_EML_ARCHIVE_SERVICE_EXPORT)) : nullptr;
    g_startup_getter = eml ? reinterpret_cast<KilnEmlStartupServiceFn>(GetProcAddress(eml, KILN_EML_STARTUP_SERVICE_EXPORT)) : nullptr;
    const KilnEmlArchiveApiV1* api = archive_get ? archive_get(KILN_EML_ARCHIVE_VERSION) : nullptr;
    kiln::boot_view::LaunchFacts launch;
    launch.launch_id = g_launch_id;
    std::memcpy(launch.plan_sha256.data(), g_plan_hash, 32);
    // boot_service.cpp kArchiveRows: a held, off or absent row refuses
    static const char* const rows[] = {"eml.archive.metadata",         "eml.archive.metadata.acquire", "eml.archive.metadata.identity",
                                       "eml.archive.metadata.release", "eml.archive.read",             "eml.archive.read.resource_json"};
    std::string off, text;
    if (api)
        for (const char* row : rows)
            if (!cap_available(row, &text)) {
                off = std::string("kiln.caps row ") + row + " is not available (" + text + ")";
                break;
            }
    static kiln::boot_view::CommittedView& view = *new kiln::boot_view::CommittedView;
    static const char id[] = "kiln.boot";
    const KilnOwner owner = api_owner_create(KilnStr{id, sizeof id - 1});
    std::string why;
    const bool bound = off.empty() ? view.bind(owner, launch, api, &onboot_identity, &why) : view.refuse(off, &why);
    if (!bound) {
        mark("onboot unavailable " + why);
        return;
    }
    KilnEmlArchiveIdentityV1 got{};
    got.size = sizeof got;
    std::string again;
    const KilnResult identity_rc = onboot_identity(launch, &got, &again);
    mark("onboot bound digest=" + view.digest() + " identity-rc=" + std::to_string(identity_rc) + " phase=" + std::to_string(got.phase) +
         " generation=" + std::to_string(got.generation) + " epoch=" + std::to_string(got.reload_epoch) + " launch=" + hex(got.launch_id, 16) +
         " plan=" + hex(got.plan_sha256, 32) + " archive=" + hex(got.archive_sha256, 32));
    std::string json;
    size_t required = 0, value = 0;
    const KilnResult rc = view.tree(kOnbootQuery, 1u << 20, 4u << 20, 16u << 20, &json, &required, &value, &why);
    mark("onboot read rc=" + std::to_string(rc) + " " + (rc == KILN_OK ? json : why));
    view.release();
    mark("onboot released");
}

// kiln\state\sfi.json's phase, or "" (kiln-sfi writes the file atomically; a plain substring read is enough)
std::string sfi_phase() {
    const std::string text = read_file(g_game + L"\\kiln\\state\\sfi.json");
    const size_t at = text.find("\"phase\"");
    if (at == std::string::npos) return {};
    const size_t open = text.find('"', text.find(':', at) + 1);
    const size_t close = open == std::string::npos ? open : text.find('"', open + 1);
    return close == std::string::npos ? std::string() : text.substr(open + 1, close - open - 1);
}

// The privileges that let an open pass a deny ACE (backup semantics, which every directory open uses, honour
// SeBackupPrivilege): removed from this process's token, so a deny-all ACL on <game>\shroudforge refuses every open of
// anything below it (an elevated runner's token holds them). Logged: which were removed.
void drop_privileges() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        mark("privileges: the token cannot be opened");
        return;
    }
    std::string removed;
    for (const wchar_t* name : {L"SeBackupPrivilege", L"SeRestorePrivilege", L"SeTakeOwnershipPrivilege", L"SeSecurityPrivilege"}) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) continue;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_REMOVED;
        if (AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS)
            removed += " " + utf8(name);
    }
    CloseHandle(token);
    mark("privileges removed:" + (removed.empty() ? std::string(" none held") : removed));
}

int hold_lock(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    HANDLE f = CreateFileW(argv[2], GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    OVERLAPPED overlapped{};
    if (f == INVALID_HANDLE_VALUE ||
        !LockFileEx(f, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &overlapped))
        return 7;
    HANDLE ready = CreateFileW(argv[3], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (ready == INVALID_HANDLE_VALUE) return 8;
    CloseHandle(ready);
    for (int i = 0; i < 6000 && GetFileAttributesW(argv[4]) == INVALID_FILE_ATTRIBUTES; ++i) Sleep(50);
    CloseHandle(f);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && std::wstring(argv[1]) == L"--hold-lock") return hold_lock(argc, argv);
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_tasks_cs);
    InitializeCriticalSection(&g_services_cs);
    InitializeCriticalSection(&g_caps_cs);
    static wchar_t self[32768];
    const DWORD self_len = GetModuleFileNameW(nullptr, self, 32768);
    const std::wstring exe(self, self_len);
    g_game = env(L"G14D_GAME", exe.substr(0, exe.find_last_of(L"\\/")));
    while (!g_game.empty() && (g_game.back() == L'\\' || g_game.back() == L'/')) g_game.pop_back();
    g_game_utf8 = utf8(g_game);
    g_config_dir = g_game_utf8 + "\\kiln\\config";
    g_data_dir = g_game_utf8 + "\\kiln\\data";
    const std::wstring log_path = env(L"G14D_LOG", g_game + L"\\kiln\\logs\\g14d-host.log");
    g_log = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    drop_privileges();
    mark("start pid=" + std::to_string(GetCurrentProcessId()) + " qpc-frequency=" + std::to_string(g_freq.QuadPart) +
         " qpc-start=" + std::to_string(g_t0.QuadPart) + " exe=" + utf8(exe) + " game=" + g_game_utf8);
    g_profile = read_file(g_game + L"\\kiln\\profile.json");
    g_exe_sha = utf8(env(L"G14D_EXE_SHA256"));
    if (!hex_bytes(env(L"G14D_PLAN_SHA256"), g_plan_hash, 32)) mark("no G14D_PLAN_SHA256: kiln.launch serves a zero hash");
    if (const auto id = env(L"G14D_LAUNCH_ID"); !id.empty()) g_launch_id = std::wcstoull(id.c_str(), nullptr, 16);
    const std::wstring backend = env(L"G14D_BACKEND", L"kiln");
    if (backend == L"hosted") g_backend = KILN_LAUNCH_BACKEND_HOSTED, g_reason = KILN_LAUNCH_REASON_HOSTED_DEFAULT;
    const std::wstring policy = env(L"G14D_POLICY", L"always");
    g_policy = policy == L"never"           ? KILN_LAUNCH_NATIVE_POLICY_NEVER
               : policy == L"verified-only" ? KILN_LAUNCH_NATIVE_POLICY_VERIFIED_ONLY
                                            : KILN_LAUNCH_NATIVE_POLICY_ALWAYS;
    g_verified = env(L"G14D_VERIFIED", L"1") == L"1" ? 1 : 0;
    g_kill_at = env(L"G14D_KILL_AT");
    const DWORD worker_wait = DWORD(std::wcstoul(env(L"G14D_WORKER_WAIT_MS", L"60000").c_str(), nullptr, 10));
    g_api = make_api();
    // the log member's first call, on the boot thread before any plugin
    api_log(0, KILN_LOG_INFO, KilnStr{"g14d host: kiln-core stand-in", 29});
    mark("config backend=" + std::to_string(g_backend) + " policy=" + std::to_string(g_policy) + " verified=" +
         std::to_string(g_verified) + " launch=" + std::to_string(g_launch_id) + " kill-at=" + utf8(g_kill_at) +
         " profile-bytes=" + std::to_string(g_profile.size()) + " exe-sha256=" + g_exe_sha);

    // 1. early plugins: kiln-*.dll exporting kiln_plugin_early, all loaded first (file-name order)
    std::vector<Plugin> plugins;
    WIN32_FIND_DATAW fd;
    const std::wstring dir = g_game + L"\\kiln\\plugins\\";
    if (HANDLE h = FindFirstFileW((dir + L"kiln-*.dll").c_str(), &fd); h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            Plugin p;
            p.file = dir + fd.cFileName;
            p.name = lower(utf8(fd.cFileName));
            plugins.push_back(std::move(p));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(plugins.begin(), plugins.end(), [](const Plugin& a, const Plugin& b) { return a.name < b.name; });
    for (auto& p : plugins) {
        mark("early-load " + p.name);
        p.module =
            LoadLibraryExW(p.file.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!p.module) {
            mark("early-load " + p.name + " failed " + std::to_string(GetLastError()));
            continue;
        }
        p.early = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(p.module, "kiln_plugin_early"));
        p.main = reinterpret_cast<KilnPluginMainFn>(GetProcAddress(p.module, KILN_PLUGIN_MAIN));
    }
    std::vector<Plugin*> early;
    for (auto& p : plugins)
        if (p.early) early.push_back(&p);
    std::stable_sort(early.begin(), early.end(), [](const Plugin* a, const Plugin* b) {
        return a->name == "kiln-eml.dll" && b->name != "kiln-eml.dll";
    });
    for (Plugin* p : early) {
        const std::string id = "early:" + p->name;
        p->early_owner = api_owner_create(KilnStr{id.data(), id.size()});
        p->early_info.size = sizeof p->early_info;
        KilnResult rc = KILN_ERR_INTERNAL;
        DWORD code = 0;
        mark("early-main-begin " + p->name);
        const bool ran = guarded_main(p->early, p->early_owner, &p->early_info, &rc, &code);
        p->early_ok = ran && rc == KILN_OK;
        mark("early-main-end " + p->name + " rc=" + std::to_string(rc) + (ran ? "" : " faulted=" + std::to_string(code)));
        if (!p->early_ok) api_owner_revoke(p->early_owner);
    }
    kill_point(L"after-early-main");
    if (env(L"G14D_WAIT_DLLMAIN") == L"1") {
        const std::wstring dll_log = env(L"G14D_DLL_LOG");
        const ULONGLONG until = GetTickCount64() + 10000;
        bool begun = false;
        while (!begun && GetTickCount64() < until) {
            begun = read_file(dll_log).find(" attach-begin ") != std::string::npos;
            if (!begun) Sleep(5);
        }
        mark(std::string("dllmain-") + (begun ? "begun" : "not-begun"));
    }
    for (Plugin* p : early) {
        if (!p->early_ok || p->early_info.size < offsetof(KilnPluginInfo, on_ready) + sizeof(void*) ||
            !p->early_info.on_ready)
            continue;
        DWORD code = 0;
        mark("early-ready-begin " + p->name);
        const bool ran = guarded_ready(p->early_info.on_ready, p->early_info.user, &code);
        mark("early-ready-end " + p->name + (ran ? "" : " faulted=" + std::to_string(code)));
        if (!ran) api_owner_revoke(p->early_owner);
    }
    kill_point(L"after-early-ready");

    // 2. Kiln's plugin step: services start, then every plugin's kiln_plugin_main, then every on_ready
    g_plugin_step = true;
    mark("plugin-step");
    for (auto& p : plugins) {
        if (!p.main) continue;
        const std::string id = "plugin:" + p.name;
        p.owner = api_owner_create(KilnStr{id.data(), id.size()});
        p.info.size = sizeof p.info;
        KilnResult rc = KILN_ERR_INTERNAL;
        DWORD code = 0;
        mark("plugin-main-begin " + p.name);
        const bool ran = guarded_main(p.main, p.owner, &p.info, &rc, &code);
        p.ok = ran && rc == KILN_OK;
        mark("plugin-main-end " + p.name + " rc=" + std::to_string(rc) + (ran ? "" : " faulted=" + std::to_string(code)));
        if (!p.ok) api_owner_revoke(p.owner);
    }
    kill_point(L"after-plugin-main");
    for (auto& p : plugins) {
        if (!p.ok || !p.info.on_ready) continue;
        DWORD code = 0;
        mark("plugin-ready-begin " + p.name);
        const bool ran = guarded_ready(p.info.on_ready, p.info.user, &code);
        mark("plugin-ready-end " + p.name + (ran ? "" : " faulted=" + std::to_string(code)));
    }

    // 3. kiln.boot's bound(): every on_ready ran (kiln-eml's startup barrier among them)
    onboot();

    // 4. the game's entry point: the interpreter's worker settles (its runtime runs, or the start is refused)
    mark("entry-point");
    const ULONGLONG until = GetTickCount64() + worker_wait;
    for (;;) {
        const std::string phase = sfi_phase();
        bool ended = false;
        EnterCriticalSection(&g_tasks_cs);
        for (Task* t : g_tasks) ended |= t->state.load() >= KILN_TASK_COMPLETED;
        LeaveCriticalSection(&g_tasks_cs);
        if (phase == "running" || phase == "refused" || phase == "ended" || phase == "aside" || ended) break;
        if (GetTickCount64() > until) break;
        EnterCriticalSection(&g_tasks_cs);
        const bool none = g_tasks.empty();
        LeaveCriticalSection(&g_tasks_cs);
        if (none) break;  // no interpreter worker was submitted at the plugin step: nothing more will run
        Sleep(50);
    }
    // the packages' Lua runs at runtime-create: the log quiet for 500 ms (at most 10 s more)
    const ULONGLONG quiet_until = GetTickCount64() + 10000;
    while (GetTickCount64() < quiet_until && (qpc() - g_last_line.load()) * 1000 / g_freq.QuadPart < 500) Sleep(50);
    {
        uint32_t started = 0, finished = 0;
        EnterCriticalSection(&g_tasks_cs);
        for (Task* t : g_tasks) {
            started += t->started ? 1 : 0;
            finished += t->finished ? 1 : 0;
        }
        const size_t submitted = g_tasks.size();
        LeaveCriticalSection(&g_tasks_cs);
        mark("settled sfi-phase=" + sfi_phase() + " tasks=" + std::to_string(submitted) + " started=" +
             std::to_string(started) + " finished=" + std::to_string(finished));
        // kiln-sfi writes sfi.json's later phases from its own writer thread (a temp file flushed to disk, then renamed
        // over it): ending the process right after the entry point can stop that write halfway (a game runs on for
        // minutes). When a worker ran, the stand-in waits, at most 10 s, until the file has left `starting`.
        const ULONGLONG written_until = GetTickCount64() + 10000;
        while (submitted != 0 && sfi_phase() == "starting" && GetTickCount64() < written_until) Sleep(100);
        if (submitted != 0) mark("written sfi-phase=" + sfi_phase());
    }

    // the exit: kiln.tasks parks (cancel, at most 2 s), then every plugin's on_unload, last loaded first
    cancel_all(0);
    const ULONGLONG park = GetTickCount64() + 2000;
    while (outstanding(0) && GetTickCount64() < park) Sleep(10);
    mark("parked outstanding=" + std::to_string(outstanding(0)));
    for (auto it = plugins.rbegin(); it != plugins.rend(); ++it) {
        if (!it->ok || !it->info.on_unload) continue;
        DWORD code = 0;
        mark("unload-begin " + it->name);
        const bool ran = guarded_ready(it->info.on_unload, it->info.user, &code);
        mark("unload-end " + it->name + (ran ? "" : " faulted=" + std::to_string(code)));
    }
    mark("exit sfi-phase=" + sfi_phase());
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
