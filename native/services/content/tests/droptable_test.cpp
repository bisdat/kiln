// SPDX-License-Identifier: MIT
// kiln.content: the drop-table hook (DefaultInventoryAcquire, content.cpp note_table) patches a table the game has just
// loaded on the caller's thread, before the caller walks it. On the host, content.cpp's own hook body over a fake game:
//  - a second caller of a table another thread is still patching waits for that patch: it does not walk a half-written
//    table (two of the server world's job threads rolling the same table in one step, on a dedicated server too)
//  - a game that hosts its world: the prediction world's acquire on the main thread (immediate_loot_action is one of
//    its programs and calls the acquire) while one of the server world's job threads is still patching the same table
//    waits for that patch too; a table the main thread acquires first it patches, and the server world's later acquire
//    finds it patched
//  - a joined client (no server world here): its main thread patches as before
//  - the world goes away while a caller waits for the store lock: that caller neither patches the table nor marks it
//    seen, so the next world's first acquire patches it
// Each case runs in a child process.
#include <windows.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// What content.cpp takes from <windows.h> beyond host/windows.h; libstdc++ already defines __try as `try`.
inline unsigned long long GetTickCount64() { return 1000; }
inline unsigned long GetExceptionCode() { return 0; }
#define EXCEPTION_EXECUTE_HANDLER 1
#define __except(...) catch (...)
#define __declspec(x)
struct MEMORY_BASIC_INFORMATION {
    void *BaseAddress, *AllocationBase;
    unsigned long Protect, Type;
    size_t RegionSize;
};
#define MEM_PRIVATE 0x20000
#define MEM_IMAGE 0x1000000
inline size_t VirtualQuery(const void*, MEMORY_BASIC_INFORMATION*, size_t) { return 0; }

#include "services/kiln_gamedata.h"
#include "../content.cpp"

// ============================================================================================= the fake game =====
namespace fake {
std::atomic<int> patches{0};           // nc_drops_table calls: the table was patched
std::atomic<bool> hold_patch{false};   // the patch waits for `release` (a slow first patch on one job thread)
std::atomic<bool> in_patch{false};
std::atomic<int> lock_waits{0};       // StoreMutex::lock calls that found the lock held
std::mutex mu;
std::condition_variable cv;
bool release = false;
thread_local uint32_t t_world = 0xFF;  // the world whose thread this is (0xFF: a job thread of the server world)

bool KILN_CALL on_world_thread(uint32_t kind) { return t_world == kind; }
std::mutex log_mu;
std::vector<std::string> lines;  // kiln.log
void KILN_CALL log(KilnOwner, int32_t, KilnStr text) {
    std::lock_guard lock(log_mu);
    lines.emplace_back(text.ptr, text.len);
}
bool logged(const std::string& part) {
    std::lock_guard lock(log_mu);
    for (const auto& l : lines)
        if (l.find(part) != std::string::npos) return true;
    return false;
}
KilnApi api = [] {
    KilnApi a{};
    a.size = sizeof a;
    a.on_world_thread = &on_world_thread;
    a.log = &log;
    return a;
}();
}  // namespace fake

namespace kc {
// what note_table reaches in the service's other files
Registry& reg(uint32_t kind) {
    static Registry r[5];
    return r[kind < 5 ? kind : 0];
}
void Registry::add(Entry e) {
    by_id[e.id] = static_cast<uint32_t>(entries.size());
    entries.push_back(std::move(e));
}
const Entry* Registry::get(uint32_t id) const {
    const auto it = by_id.find(id);
    return it == by_id.end() ? nullptr : &entries[it->second];
}
bool drops_have_rules() { return false; }
void drops_apply_rules(const Entry&) {}
void nc_drops_table(const Entry&) {
    fake::in_patch = true;
    if (fake::hold_patch) {
        std::unique_lock lock(fake::mu);
        fake::cv.wait(lock, [] { return fake::release; });
    }
    fake::patches.fetch_add(1);
    fake::in_patch = false;
}
// registry_live.cpp's (dev.31): note_table hands the new table to it; the registry's layers are not part of this test
void drops_table_loaded(const Entry& table) {
    if (drops_have_rules()) drops_apply_rules(table);
    nc_drops_table(table);
}
size_t store_flush(std::string*) { return 0; }
void timing_mark_job_thread(bool) {}
PhaseTimer::PhaseTimer(Phase) {}
PhaseTimer::~PhaseTimer() {}
void StoreMutex::lock() {
    if (m_.try_lock()) return;
    fake::lock_waits.fetch_add(1);  // a thread is waiting for the store lock now
    m_.lock();
}
void StoreMutex::unlock() { m_.unlock(); }
bool StoreMutex::try_lock() { return m_.try_lock(); }
StoreMutex g_store;
StoreMutex& store_mutex() { return g_store; }
uint32_t fnv1a(const void* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 16777619u;
    return h;
}
std::string guid_text(const uint8_t*) { return "a20e8eae-7833-462e-9ea9-59d1e17a1b98"; }
// Reached from content.cpp's C table and its other paths (the table is kept with the plugin's entry); never called here.
Engine E;
PacksReport apply_all_packs(const std::vector<PackFile>&) { return {}; }
bool apply_pack(KilnOwner, uint64_t, const std::string&, const std::string&, ApplyResult*) { return false; }
std::string apply_result_json(const ApplyResult&) { return {}; }
KilnContentValue current_value(const Entry&, const Field&) { return {}; }
KilnContentValue original_value(const Entry&, const Field&) { return {}; }
std::string data_dir() { return {}; }
std::vector<PackFile> discover_packs(std::string*) { return {}; }
void drops_drop_owner(KilnOwner) {}
bool dump_all(const std::string&, std::string*) { return false; }
std::string export_changes_json(uint32_t, uint32_t*) { return {}; }
bool guard_on() { return false; }
bool guard_set(bool, std::string*) { return false; }
KilnResult nc_define(KilnOwner, uint32_t, const std::string&, bool, const std::string&, std::string*) { return KILN_ERR_UNAVAILABLE; }
std::string nc_definitions_json() { return {}; }
bool nc_icon_alias(const uint8_t[16], const uint8_t[16]) { return false; }
std::string nc_info_json(const std::string&) { return {}; }
void nc_packs_begin() {}
void nc_packs_end() {}
uint32_t nc_undefine_owner(KilnOwner) { return 0; }
uint64_t next_code_seq() { return 1; }
std::vector<PackFile> order_packs(const std::vector<PackFile>&, const std::vector<std::string>&, std::vector<std::string>*) { return {}; }
std::string owner_label(KilnOwner) { return {}; }
void set_owner_label(KilnOwner, const std::string&) {}
void read_drop_stack(uintptr_t, uint32_t, KilnContentStack*) {}
bool read_recipe_stacks(const Entry&, uint32_t, std::vector<KilnContentStack>*) { return false; }
std::string refs_info_json(const Schema&, const std::vector<uintptr_t>&) { return {}; }
std::vector<uintptr_t> registry_items() { return {}; }
const Entry* Registry::lookup(const std::string&) const { return nullptr; }
std::string report_json(const PacksReport&) { return {}; }
const Field* Schema::find(const std::string&) const { return nullptr; }
KilnResult set_drop_stacks(KilnOwner, uint64_t, const Entry&, const std::vector<KilnContentStack>&, const std::string&, std::string*) {
    return KILN_ERR_UNAVAILABLE;
}
KilnResult set_field(KilnOwner, uint64_t, uint32_t, const Entry&, const Field&, const KilnContentValue&, const std::string&, std::string*,
                     int32_t) {
    return KILN_ERR_UNAVAILABLE;
}
KilnResult set_recipe_stacks(KilnOwner, uint64_t, const Entry&, uint32_t, const std::vector<KilnContentStack>&, const std::string&,
                             std::string*) {
    return KILN_ERR_UNAVAILABLE;
}
uint32_t store_changed() { return 0; }
std::vector<Conflict> store_conflicts() { return {}; }
uint32_t store_drop_owner(KilnOwner) { return 0; }
bool store_drop(KilnOwner, uintptr_t) { return false; }
void store_for_each(const std::function<void(uintptr_t, const Slot&)>&) {}
uint64_t store_generation() { return 0; }
bool store_txn_begin(KilnOwner, uint64_t*) { return false; }
bool store_txn_commit(uint64_t) { return false; }
TxnOutcome store_txn_rollback(uint64_t, const std::function<void()>&, KilnOwner*, uint32_t*, std::string*) { return TxnOutcome::Cancelled; }
bool valid_kind(uint32_t k) { return k >= 1 && k <= 4; }
bool walk_drop_table(uintptr_t, std::vector<DropStackRef>*) { return false; }
WriteStats write_stats() { return {}; }
KilnResult apply_kitbash(KilnOwner, uint64_t, const Entry&, const std::string&, const nlohmann::ordered_json&, bool, const std::string&,
                         std::vector<std::string>*) {
    return KILN_ERR_UNAVAILABLE;
}
KilnResult KILN_CALL nc_text_api(uint32_t, char*, size_t, size_t*) { return KILN_ERR_UNAVAILABLE; }
KilnResult KILN_CALL nc_text_info_api(KilnContentTextInfo*) { return KILN_ERR_UNAVAILABLE; }
}  // namespace kc

// ============================================================================================= the cases =========
namespace {

int failures = 0;
void check(bool ok, const std::string& what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

const uint8_t kTableGuid[16] = {0xa2, 0x0e, 0x8e, 0xae, 0x78, 0x33, 0x46, 0x2e, 0x9e, 0xa9, 0x59, 0xd1, 0xe1, 0x7a, 0x1b, 0x98};
uint8_t table_data[64];  // the loaded DefaultInventoryResource
const uintptr_t kData = reinterpret_cast<uintptr_t>(table_data);

void ready(uint32_t owning_kind) {
    kc::g_api = &fake::api;
    g_kind.store(owning_kind);
    g_ready.store(true);
}

// One caller patches the table slowly (`hold_patch`) on its thread; another, on `second_world`'s thread, acquires the same
// table meanwhile. The second one must wait for the store lock, not come back to walk the half-written table.
void second_waits(uint32_t first_world, uint32_t second_world, const char* who) {
    fake::hold_patch = true;
    std::thread first([first_world] {
        fake::t_world = first_world;
        note_table(kTableGuid, kData);
    });
    for (const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
         !fake::in_patch && std::chrono::steady_clock::now() < until;)
        std::this_thread::yield();
    if (!fake::in_patch) {
        check(false, "the first caller patches the table it acquired first");
        fake::hold_patch = false;  // later patches in the case do not wait
        first.join();
        return;
    }
    std::atomic<bool> second_in{false}, second_back{false};
    std::thread second([&] {
        fake::t_world = second_world;
        second_in = true;
        note_table(kTableGuid, kData);  // then the game walks the table
        second_back = true;
    });
    while (!second_in) std::this_thread::yield();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!second_back && fake::lock_waits == 0 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    check(!second_back && fake::lock_waits == 1, std::string(who) + " does not walk the table while it is still being patched (it waits for the store lock)");
    {
        std::lock_guard lock(fake::mu);
        fake::release = true;
    }
    fake::cv.notify_all();
    first.join();
    second.join();
    check(second_back && fake::patches == 1, "it goes on once the patch is in, and the table is patched once");
}

// Two of the server world's job threads acquire a table nobody has seen, the first one's patch is slow (a dedicated
// server too).
void case_two_jobs() {
    ready(KILN_WORLD_SERVER);
    second_waits(0xFF, 0xFF, "a second job thread");
    note_table(kTableGuid, kData);
    check(fake::patches == 1, "a later acquire finds it patched");
}

// A game that hosts its world: one of the server world's job threads is patching a new table when the prediction world
// (the main thread) acquires it.
void case_hosted_main_waits() {
    ready(KILN_WORLD_SERVER);
    second_waits(0xFF, KILN_WORLD_PREDICTION, "hosting: the prediction world's acquire on the main thread");
    fake::t_world = KILN_WORLD_CLIENT;
    note_table(kTableGuid, kData);
    check(fake::patches == 1, "hosting: the client world's later acquire finds it patched");
}

// A game that hosts its world: the main thread acquires a new table first and patches it; a server job thread that
// acquires it meanwhile waits for that patch.
void case_hosted_main_first() {
    ready(KILN_WORLD_SERVER);
    second_waits(KILN_WORLD_PREDICTION, 0xFF, "hosting: a server job thread");
    std::thread later([] {
        fake::t_world = 0xFF;
        note_table(kTableGuid, kData);
    });
    later.join();
    check(fake::patches == 1, "hosting: the server world's later acquire finds it patched");
}

// The world goes away (world_reset clears g_ready first, then takes the store lock) while a caller waits for the store
// lock: that caller neither patches the table nor marks it seen.
void case_reset_while_waiting() {
    ready(KILN_WORLD_SERVER);
    std::atomic<bool> back{false};
    kc::store_mutex().lock();  // the reset holds the store lock
    std::thread job([&] {
        note_table(kTableGuid, kData);
        back = true;
    });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (fake::lock_waits == 0 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    g_ready.store(false);  // the world went away while the job thread waited
    kc::store_mutex().unlock();
    job.join();
    bool marked = false;
    {
        std::lock_guard lock(g_seen_mu);
        marked = g_seen.count(kData) != 0;
    }
    check(back && fake::patches == 0 && !marked && !kc::reg(KILN_CONTENT_DROPS).get(kc::fnv1a(kTableGuid, 16)),
          "a caller that waited through the world's reset leaves the table unpatched, unregistered and not marked seen");
    g_ready.store(true);  // the next world
    note_table(kTableGuid, kData);
    check(fake::patches == 1, "the next world's first acquire patches it");
}

// A joined client: kiln.content serves the client world, whose thread is the main thread.
void case_joined() {
    ready(KILN_WORLD_CLIENT);
    fake::t_world = KILN_WORLD_PREDICTION;
    note_table(kTableGuid, kData);
    check(fake::patches == 1, "a join: the main thread patches the table as before");
}

}  // namespace

int main() {
    const std::pair<const char*, void (*)()> cases[] = {
        {"two job threads, one table", &case_two_jobs},
        {"hosted: the main thread while a server job patches", &case_hosted_main_waits},
        {"hosted: the main thread first", &case_hosted_main_first},
        {"the world goes away while a caller waits", &case_reset_while_waiting},
        {"joined", &case_joined},
    };
    std::string failed;
    for (const auto& [name, fn] : cases) {
        std::printf("== %s\n", name);
        std::fflush(stdout);
        const pid_t pid = fork();
        if (pid == 0) {
            fn();
            std::fflush(stdout);
            _exit(failures ? 1 : 0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
            if (WIFSIGNALED(status)) std::printf("  FAIL the case crashed (signal %d)\n", WTERMSIG(status));
            failed += std::string(failed.empty() ? "" : ", ") + name;
        }
    }
    if (failed.empty()) std::printf("droptable-test: all %zu cases passed\n", std::size(cases));
    else std::printf("droptable-test: FAILED: %s\n", failed.c_str());
    return failed.empty() ? 0 : 1;
}