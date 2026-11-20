// SPDX-License-Identifier: MIT
// The exit sequence on the REAL Windows code path under headless Wine (test-exit-wine.sh): the real api.cpp exit block
// and boot.cpp's exit trigger (cut out of those files as they are) over the real task pool on Windows threads, ending
// in a real ExitProcess. In flight at the exit, the native Windows check's three cases: an active task (one body that
// sees its cancel, one that ignores it), a suspended managed task and a pending tick hop, plus a queued task. A stand-in
// for Kiln's own service handler (kiln.engine's gate close) and a plugin handler record the order; the plugin handler
// also checks that the pool is parked. Every line goes to stdout; test-exit-wine.sh reads them, the exit code and the
// time the process took to end.
#define NOMINMAX
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "kiln_api.h"
#include "tasks.h"

using namespace kiln::tasks;

// ---- what the cut blocks need from the core, here over plain Win32 and stdout ----
namespace kiln {
double now_ms() { return static_cast<double>(GetTickCount64()); }
struct Paths {
    std::filesystem::path logs;  // empty: no kiln.log, so the last line's wait ends at once
};
const Paths& paths() {
    static Paths p;
    return p;
}
struct Config {
    int log_level = KILN_LOG_TRACE;
};
const Config& config() {
    static Config c;
    return c;
}
namespace crash {
struct Capture {};
int filter(EXCEPTION_POINTERS*, Capture*, unsigned long* code) {
    if (code) *code = 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
void report(const Capture&, const char*, const void*) {}
}  // namespace crash
namespace log {
void write(int, const char*, std::string_view text) {
    std::printf("log: %.*s\n", static_cast<int>(text.size()), text.data());
}
void writef(int, const char*, const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::printf("log: %s\n", buf);
}
}  // namespace log
namespace caps {
bool flush() { return true; }
}  // namespace caps
#define KLOG_I(...) ::kiln::log::writef(KILN_LOG_INFO, "kiln", __VA_ARGS__)
#define KLOG_W(...) ::kiln::log::writef(KILN_LOG_WARN, "kiln", __VA_ARGS__)
#define KLOG_E(...) ::kiln::log::writef(KILN_LOG_ERROR, "kiln", __VA_ARGS__)

// kiln.tasks' exit calls (services/tasks.cpp) over this test's one Service
Service* g_service = nullptr;
namespace tasks_svc {
void exit_begins() {
    std::printf("step: admission closed\n");
    g_service->refuse_new();
}
void exiting() {
    std::printf("step: owners sealed\n");
    g_service->exiting();
}
void park(uint32_t wait_ms) {
    const Pool::ParkResult r = g_service->park(uint64_t{wait_ms} * 1000 * 1000);
    std::printf("step: parked (%u running at the park, %u still running after %.0f ms: %s)\n", r.running_at_start,
                r.still_running, static_cast<double>(r.waited_ns) / 1e6, r.names.c_str());
}
}  // namespace tasks_svc

struct LoadedModule {
    std::string name;
    KilnPluginInfo info{};
    bool own_service = false;
};
std::vector<LoadedModule> g_modules;

#include "exit_sequence.inc"
#include "exit_trigger.inc"
}  // namespace kiln

namespace {

struct Env final : ServiceEnv {
    bool on_tick_or_world_thread() override { return false; }
    bool owner_alive(KilnOwner owner) override { return owner >= 1 && owner <= 9; }
    std::string owner_name(KilnOwner owner) override {
        static const char* names[] = {"", "mod.active", "mod.stuck", "mod.queued", "mod.suspended", "mod.hop"};
        return owner < 6 ? names[owner] : "mod." + std::to_string(owner);
    }
};

std::atomic<int> g_starts{0};       // every body and done that started, except the stuck body's own loop
std::atomic<int> g_dones{0};
std::atomic<int> g_segments{0};     // managed segments dispatched
std::atomic<int> g_hops{0};         // tick hops dispatched
std::atomic<bool> g_active_saw_cancel{false}, g_queued_ran{false};
std::atomic<uint64_t> g_registration{0};

KilnTaskInfo info(const char* name, uint32_t flags = 0) {
    KilnTaskInfo i{};
    i.size = sizeof i;
    i.flags = flags;
    i.name = {name, std::strlen(name)};
    return i;
}

void KILN_CALL active_body(const KilnTaskRun* run, void*) {
    g_starts++;
    while (!run->cancelled(run)) Sleep(1);
    g_active_saw_cancel = true;
    Sleep(20);  // still inside when the park begins, back well within its bound
}
void KILN_CALL stuck_body(const KilnTaskRun*, void*) {
    g_starts++;
    for (;;) Sleep(1);  // ignores its cancel: the park names it, the process exit ends it
}
void KILN_CALL queued_body(const KilnTaskRun*, void*) {
    g_starts++;
    g_queued_ran = true;
}
void KILN_CALL any_done(KilnHandle, uint32_t, void*) {
    g_starts++;
    g_dones++;
}

Service* g_svc = nullptr;
void KILN_CALL dispatch(KilnLease lease, uint32_t what, uint32_t, void*) {
    if (what != KILN_LEASE_SEGMENT) {
        g_hops++;
        return;
    }
    g_segments++;
    uint64_t r = 0;
    g_svc->host_arm(lease, &r);  // every segment suspends: the task awaits something
    g_registration = r;
}

template <class F>
bool until(F pred, int ms = 5000) {
    // a deadline, not a count of Sleep(1): native Windows sleeps a whole timer tick (15.6 ms by default), Wine about 1 ms
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(ms);
    while (!pred() && GetTickCount64() < end) Sleep(1);
    return pred();
}

KilnLease g_suspended = 0;

void KILN_CALL service_handler(void*) { std::printf("step: service handler (the engine's gate closes)\n"); }

void KILN_CALL plugin_handler(void*) {
    // runs after the park: nothing may start any more, a resumed managed task stays suspended, no hop runs
    const int starts = g_starts.load(), segments = g_segments.load();
    const KilnResult resumed = g_svc->host_resume(g_suspended, g_registration.load());
    Sleep(150);
    std::printf("step: plugin handler (resume after the park: %d; started since: %d; segments since: %d; dones: %d; "
                "hops: %d; queued ran: %d)\n",
                resumed, g_starts.load() - starts, g_segments.load() - segments, g_dones.load(), g_hops.load(),
                g_queued_ran.load() ? 1 : 0);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("exit windows host: start\n");
    PoolConfig config;
    config.normal_workers = 2;
    config.long_running_workers = 1;
    // the production pool and its binding live for the whole process: never destroyed, ended by the process exit
    auto* pool = new Pool(config);
    static Env env;
    auto* service = new Service(*pool, env);
    kiln::g_service = g_svc = service;
    service->host_bind(&dispatch);

    // a suspended managed task and one with a pending tick hop: neither holds a worker
    service->session_started(1);
    const KilnTaskInfo lease_info = info("awaits");
    KilnLease hop = 0;
    service->host_admit(4, &lease_info, nullptr, &g_suspended, nullptr);
    if (!until([] { return g_segments.load() == 1; })) return 10;
    service->host_admit(5, &lease_info, nullptr, &hop, nullptr);
    if (!until([] { return g_segments.load() == 2; })) return 11;
    if (service->host_tick_hop(hop) != KILN_OK) return 12;
    std::printf("in flight: a suspended managed task, a pending tick hop (%zu)\n", service->pending_tick_hops());

    // an active task that sees its cancel, one that ignores it (both workers busy), and a queued one behind them
    KilnHandle h = 0;
    const KilnTaskInfo a = info("active"), s = info("stuck"), q = info("queued");
    service->submit(1, &a, &active_body, &any_done, nullptr, &h, nullptr);
    service->submit(2, &s, &stuck_body, &any_done, nullptr, &h, nullptr);
    if (!until([] { return g_starts.load() == 2; })) return 13;
    service->submit(3, &q, &queued_body, &any_done, nullptr, &h, nullptr);
    std::printf("in flight: an active task, a stuck task, a queued task\n");

    // Kiln's own service (loaded first) and a plugin, each with an on_unload; Kiln's own exit steps first
    kiln::g_modules = {{"kiln-engine.dll", {}, true}, {"a-plugin.dll", {}, false}};
    for (auto& m : kiln::g_modules) m.info.size = sizeof(KilnPluginInfo);
    kiln::g_modules[0].info.on_unload = &service_handler;
    kiln::g_modules[1].info.on_unload = &plugin_handler;
    kiln::unload_prepare(true);
    kiln::g_exit_next = reinterpret_cast<void*>(&ExitProcess);
    kiln::g_exit_via = "test";
    std::printf("exit: begins\n");
    kiln::exit_call(0);  // the trigger, as the link on ntdll!RtlExitUserProcess calls it; never returns
    std::printf("exit: returned (must not happen)\n");
    return 20;
}
