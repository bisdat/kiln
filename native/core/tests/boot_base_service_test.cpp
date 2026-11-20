// SPDX-License-Identifier: MIT
// Host tests of kiln.boot's base policy (src/services/boot_service.cpp over boot_archive_view.h): OnBoot's game-data
// reads go through the one base view the policy picks in decide(). The snapshot is the production policy and its
// behaviour is boot_service_test.cpp's, unchanged; here the live base: its row, an author's read before and after the
// game's resources are up (unavailable either way: live bytes have no decoded values), the release at the commit, and
// the policy picked again by the next decide(). Built with ASan/UBSan and with TSan by test-boot-archive-view.sh.
// Prints one line per case and "BOOT BASE SERVICE TESTS <n> PASS <m> FAIL"; exits 1 when anything failed.
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

#include <windows.h>  // tests/stubs/windows.h
struct EXCEPTION_POINTERS;
#ifndef _WIN32
#define __try try
#define __except(x) catch (...)
#endif
[[noreturn]] inline void ExitProcess(unsigned int) { std::abort(); }  // the adapter is replaced before any call
inline void* GetCurrentProcess() { return nullptr; }
inline int TerminateProcess(void*, unsigned int) { std::abort(); }  // the late adapter is replaced too

#include "../src/services/service.cpp"
#include "../src/services/boot_service.cpp"


namespace fs = std::filesystem;
using nlohmann::json;

// ---- stubs ----
namespace {
std::vector<std::string> t_log;
std::map<KilnOwner, std::string> t_owners;
std::vector<KilnOwner> t_revoked;
KilnOwner t_next_owner = 100;
kiln::Config t_config;
kiln::ImageInfo t_image;
kiln::Paths t_paths;
uintptr_t t_worlds[KILN_WORLD_KINDS]{};
uint32_t t_sessions[KILN_WORLD_KINDS]{};  // tick::session's worlds per kind since boot
kiln::profile::State t_profile;
uint32_t t_dropins = 1u << KILN_PROFILE_KIND_KILN;

// The capability registry, reduced: per name the reports of each owner, the worst shown (kiln_caps.h).
std::map<std::string, std::map<KilnOwner, KilnCapRow>> t_caps;
std::map<std::string, std::string> t_texts;  // storage for the shown row's text
KilnResult KILN_CALL fake_report(KilnOwner owner, KilnStr name, uint32_t state, uint32_t reason, KilnStr text) {
    static std::map<std::string, std::string> keep;
    const std::string n(name.ptr, name.len), key = n + "#" + std::to_string(owner);
    keep[key] = std::string(text.ptr ? text.ptr : "", text.len);
    KilnCapRow row{};
    row.size = sizeof row;
    row.state = state;
    row.reason = reason;
    row.text = KilnStr{keep[key].data(), keep[key].size()};
    t_caps[n][owner] = row;
    return KILN_OK;
}
KilnResult KILN_CALL fake_find(KilnStr name, KilnCapRow* out) {
    const auto it = t_caps.find(std::string(name.ptr, name.len));
    if (it == t_caps.end() || it->second.empty()) return KILN_ERR_NOT_FOUND;
    const KilnCapRow* w = nullptr;
    auto rank = [](uint32_t s) { return s == KILN_CAP_UNAVAILABLE ? 0 : s == KILN_CAP_PENDING ? 1 : 2; };
    for (const auto& [o, r] : it->second)
        if (!w || rank(r.state) < rank(w->state)) w = &r;
    *out = *w;
    return KILN_OK;
}
KilnCapsApi t_caps_table = [] {
    KilnCapsApi t{};
    t.header = {sizeof(KilnCapsApi), 1};
    t.report = &fake_report;
    t.find = &fake_find;
    return t;
}();
const KilnCapRow* shown(const std::string& name) {
    static KilnCapRow r;
    r.size = sizeof r;
    return fake_find(KilnStr{name.data(), name.size()}, &r) == KILN_OK ? &r : nullptr;
}
}  // namespace

namespace kiln {
const Config& config() { return t_config; }
const ImageInfo& image() { return t_image; }
const Paths& paths() { return t_paths; }
double now_ms() { return 0; }
std::string to_utf8(std::wstring_view w) { return std::string(w.begin(), w.end()); }
std::wstring to_wide(std::string_view s) { return std::wstring(s.begin(), s.end()); }
bool read_file(const std::filesystem::path& file, std::string* out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out->assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
std::string jsonc_to_json(std::string_view text) { return std::string(text); }  // the fixtures are plain JSON
namespace log {
std::mutex t_log_mutex;  // the threaded cases log from several threads
void writef(int, const char*, const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::lock_guard lk(t_log_mutex);
    t_log.emplace_back(b);
}
}  // namespace log
namespace reg {
KilnOwner owner_create(std::string_view id) {
    t_owners[t_next_owner] = std::string(id);
    return t_next_owner++;
}
void owner_revoke(KilnOwner owner) {
    t_owners.erase(owner);
    t_revoked.push_back(owner);
}
std::string owner_name(KilnOwner owner) {
    const auto it = t_owners.find(owner);
    return it == t_owners.end() ? "(revoked)" : it->second;
}
}  // namespace reg
namespace worldinfo {
uintptr_t world(uint32_t kind) { return kind < KILN_WORLD_KINDS ? t_worlds[kind] : 0; }
}
namespace tick {
KilnResult session(KilnSessionInfo* out) {
    for (uint32_t k = 0; k < KILN_WORLD_KINDS; ++k) out->sessions[k] = t_sessions[k];
    return KILN_OK;
}
}
namespace profile {
const State& state() { return t_profile; }
bool dropins(uint32_t kind) { return (t_dropins & (1u << kind)) != 0; }
}  // namespace profile
namespace crash {
int filter(EXCEPTION_POINTERS*, Capture*, unsigned long*) { return 1; }
void report(const Capture&, const char*, const void*) {}
}  // namespace crash
namespace caps {
const KilnCapsApi* table() { return &t_caps_table; }
}  // namespace caps
}  // namespace kiln


// ---- the checks ----
namespace {
int t_pass = 0, t_fail = 0;
std::string t_case;
#define CHECK(x) check((x), #x, __LINE__)
void check(bool ok, const char* what, int line) {
    if (ok) {
        ++t_pass;
        return;
    }
    ++t_fail;
    std::printf("FAIL %s: %s (line %d)\n", t_case.c_str(), what, line);
}
void pass_line(const char* name, int fails_before) { std::printf("%s %s\n", t_fail == fails_before ? "PASS" : "FAIL", name); }

const KilnBootApiV1& B = kiln::boot_svc::kTable;
KilnStr ks(const std::string& s) { return KilnStr{s.data(), s.size()}; }
std::vector<uint32_t> t_exits;
void fake_exit(uint32_t code) { t_exits.push_back(code); }
fs::path t_root;

struct Plan {
    int collector;
};
KilnResult KILN_CALL ok_prepare(void* user, const KilnBootPrepareInputV1*, KilnBootPreparedV1* out, KilnBootIssueV1*) {
    out->ok = 1;
    out->plan = new Plan{static_cast<int>(reinterpret_cast<intptr_t>(user))};
    out->destroy = [](const void* p) { delete static_cast<const Plan*>(p); };
    return KILN_OK;
}

void reset_boot() {
    using namespace kiln::boot_svc;
    g_decided = g_deferred = g_bound = false;
    g_why.clear();
    g_boot_mods.clear();
    g_scanned.clear();
    g_status = KILN_BOOT_STATUS_COLLECTING;
    g_owners.clear();
    g_by_author.clear();
    g_host = 0;
    g_envelope = nullptr;
    g_envelope_bytes = 0;
    g_issues.clear();
    for (auto& c : g_collectors) c = {};
    if (const Root* r = g_root.exchange(nullptr)) {
        for (const void* p : r->plans) delete static_cast<const Plan*>(p);
        delete r;
    }
    g_self = 0;
    g_exit = &fake_exit;
    t_exits.clear();
    t_caps.clear();
    t_log.clear();
}

// One server boot mod "a" (deferred mode), every collector registered, bound; the author frame of "a" open.
KilnOwner deferred_author() {
    reset_boot();
    t_image.side = KILN_SIDE_SERVER;
    t_config.clr = true;
    t_profile = kiln::profile::State{};
    t_profile.mode = KILN_PROFILE_ACTIVE;
    const fs::path dir = t_root / "mods" / "a";
    fs::create_directories(dir);
    std::ofstream(dir / "kiln.mod.json") << json{{"id", "a"}, {"name", "a"}, {"version", "1.0.0"}, {"apiVersion", "0.2"}, {"side", "server"}, {"boot", true}}.dump();
    kiln::profile::Entry e;
    e.id = "a";
    e.path = dir.string();
    e.enabled = true;
    t_profile.mods[KILN_PROFILE_KIND_KILN].push_back(e);
    fake_report(0, ks("mods.boot"), KILN_CAP_AVAILABLE, KILN_CAP_REASON_NONE, ks(""));
    kiln::boot_svc::decide(true, "");
    for (int i = 0; i < 7; ++i)
        B.collector(50 + i, ks(kiln::boot_svc::kCollectors[i]), &ok_prepare, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    kiln::boot_svc::bound();
    uint64_t gen = 0;
    B.begin_owner(41, ks("a"), 1, &gen);
    return 41;
}

std::string row_text(const char* name) {
    const KilnCapRow* r = shown(name);
    return r ? std::string(r->text.ptr, r->text.len) : std::string("(no row)");
}

// ---- a fake kiln.assets for the live base ----
void(KILN_CALL* t_ready_fn)(void*) = nullptr;
void* t_ready_user = nullptr;
bool t_up = false, t_assets_running = false;
int t_resource_reads = 0;
KilnResult KILN_CALL fake_read(const KilnAssetKey*, void*, size_t, size_t* out_len) {
    ++t_resource_reads;
    *out_len = 4;
    return KILN_ERR_BUFFER_TOO_SMALL;
}
KilnResult KILN_CALL fake_when_ready(void(KILN_CALL* fn)(void*), void* user) {
    if (t_up) {
        fn(user);
        return KILN_OK;
    }
    t_ready_fn = fn, t_ready_user = user;
    return KILN_OK;
}
KilnAssetsApi t_assets = [] {
    KilnAssetsApi a{};
    a.header = {sizeof(KilnAssetsApi), KILN_ASSETS_VERSION};
    a.resource_read = &fake_read;
    a.when_resources_ready = &fake_when_ready;
    return a;
}();
const KilnAssetsApi* fake_assets() { return t_assets_running ? &t_assets : nullptr; }
kiln::boot_svc::Base live_base() { return kiln::boot_svc::Base::Live; }

std::string read_one(KilnOwner a, const std::string& q, KilnResult* rc) {
    char buf[4096];
    std::memset(buf, 0x5A, sizeof buf);
    size_t need = 99;
    *rc = B.read_resource(a, ks(q), buf, sizeof buf, &need);
    return *rc == KILN_OK ? std::string(buf) : std::string();
}

void production_policy_case() {
    t_case = "production-policy";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    CHECK(g_base_policy == &default_base && default_base() == Base::Snapshot);
    deferred_author();
    CHECK(g_base.load() == Base::Snapshot && &base_view() == &g_snapshot_view);
    CHECK(row_text("mods.boot.game_data").find("no game-data snapshot") != std::string::npos);  // none written here
    pass_line("BASE-POLICY production picks the snapshot", f0);
}

void live_policy_case() {
    t_case = "live-policy";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    g_base_policy = &live_base;
    g_live_assets = &fake_assets;
    KilnResult rc = KILN_OK;
    // kiln.assets not running yet (OnBoot runs before the service DLLs' resources): unavailable, the row says why
    const KilnOwner a = deferred_author();
    CHECK(g_base.load() == Base::Live && &base_view() == &g_live_view);
    const KilnCapRow* row = shown("mods.boot.game_data");
    CHECK(row && row->state == KILN_CAP_UNAVAILABLE && row_text("mods.boot.game_data").find("live game data") != std::string::npos);
    CHECK(read_one(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    // running, the game's resources not up (they come after the entry point): unavailable, nothing read
    t_assets_running = true;
    CHECK(read_one(a, "resource:0f8fad5b-d9cb-469f-a165-70867728950e/1a2b3c4d/0", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    CHECK(t_resource_reads == 0 && t_ready_fn != nullptr);
    // up: still no tree from live bytes, said once in kiln.log as any unavailable answer
    t_up = true;
    if (t_ready_fn) t_ready_fn(t_ready_user);
    std::string why;
    CHECK(g_live_view.ready(&why));
    t_log.clear();
    CHECK(read_one(a, "item:Test_Item_Logs", &rc).empty() && rc == KILN_ERR_UNAVAILABLE);
    CHECK(t_log.size() == 1 && t_log[0].find("boot: a: game data item:Test_Item_Logs: the live game data has no decoded values") == 0);
    CHECK(kiln::boot_svc::g_owners["a"].read_bytes == 0);  // a refusal costs nothing
    // malformed and closed frames answer as before the base view
    size_t need = 0;
    char buf[8];
    CHECK(B.read_resource(a, ks("nonsense"), buf, sizeof buf, &need) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(B.finish_owner(a, 1, KILN_BOOT_FINISH_ACCEPT) == KILN_OK);
    CHECK(B.read_resource(a, ks("item:Test_Item_Logs"), buf, sizeof buf, &need) == KILN_ERR_REVOKED);
    // the commit ends the reads: the live view is released
    KilnBootCommitResultV1 out{};
    out.size = sizeof out;
    CHECK(B.commit(7, 1, &out) == KILN_OK && out.status == KILN_BOOT_STATUS_PUBLISHED);
    CHECK(!g_live_view.ready(&why) && why.find("released") != std::string::npos);
    // the next decide() asks the policy again: a live base reads again; the snapshot policy picks the snapshot
    deferred_author();
    CHECK(g_live_view.ready(&why));
    g_base_policy = &default_base;
    deferred_author();
    CHECK(g_base.load() == Base::Snapshot && &base_view() == &g_snapshot_view);
    CHECK(row_text("mods.boot.game_data").find("no game-data snapshot") != std::string::npos);
    g_live_assets = &assets_service;
    pass_line("BASE-POLICY-LIVE OnBoot reads through the live base: unavailable before and after the resources, released at the commit", f0);
}

// Reads of two authors from several threads against a commit (the TSan build): the view is touched outside g_mutex,
// the budget under it; every answer is unavailable or revoked, none is torn.
void live_threads_case() {
    t_case = "live-threads";
    const int f0 = t_fail;
    using namespace kiln::boot_svc;
    g_base_policy = &live_base;
    g_live_assets = &fake_assets;
    const KilnOwner a = deferred_author();
    std::atomic<int> bad{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> th;
    for (int t = 0; t < 4; ++t)
        th.emplace_back([&] {
            while (!go.load()) std::this_thread::yield();
            for (int i = 0; i < 200; ++i) {
                char buf[64];
                size_t need = 0;
                const KilnResult rc = B.read_resource(a, ks("item:Test_Item_Logs"), buf, sizeof buf, &need);
                if (rc != KILN_ERR_UNAVAILABLE && rc != KILN_ERR_REVOKED) ++bad;
            }
        });
    go = true;
    KilnBootCommitResultV1 out{};
    out.size = sizeof out;
    B.finish_owner(a, 1, KILN_BOOT_FINISH_ACCEPT);
    CHECK(B.commit(7, 1, &out) == KILN_OK);
    for (auto& t : th) t.join();
    CHECK(bad == 0);
    g_base_policy = &default_base;
    g_live_assets = &assets_service;
    pass_line("BASE-POLICY-LIVE-THREADS author reads from 4 threads across the commit", f0);
}
}  // namespace

int main(int argc, char** argv) {
    t_root = fs::path(argc > 1 ? argv[1] : ".") / "boot-base-service-test";
    std::error_code ec;
    fs::remove_all(t_root, ec);
    fs::create_directories(t_root);
    t_paths.mods = t_root / "no-dropins";
    t_paths.exe = t_root / "game" / "enshrouded_server.exe";
    t_paths.data = t_root / "game" / "kiln" / "data";
    production_policy_case();
    live_policy_case();
    live_threads_case();
    reset_boot();
    fs::remove_all(t_root, ec);
    std::printf("BOOT BASE SERVICE TESTS %d PASS %d FAIL\n", t_pass, t_fail);
    return t_fail ? 1 : 0;
}
