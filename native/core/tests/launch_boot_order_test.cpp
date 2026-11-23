// SPDX-License-Identifier: MIT
// The boot order around kiln.launch's freeze: boot.cpp's boot(), from its first line up to the signatures step, cut out
// of the file as it is (test-launch.sh) and run against stubs that record each call. A fake native mod of phase
// "early" and a fake early plugin read kiln.launch through the core's table (src/launch.cpp, as it is) and must see it
// frozen; in the safe idle of an image that cannot be parsed the record is frozen before the chain's [late] DLLs load.
// LP1b: they also read build_verified, which is boot's own build_verified answer handed to the freeze (0 for backend
// none), under each verified x policy combination, hosted and kiln. Staged mod updates (staged.h) are applied once
// kiln.log is open and before kiln.json, the profile or any native mod is read, in safe idle too.
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "launch.h"
#include "services/kiln_profile.h"

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}
std::vector<std::string> calls;
bool g_image_ok = true, g_verified = true, g_policy_any = false, g_kiln = false;
std::string g_safe_idle_seen;
bool g_verified_seen = false;  // the verified boot handed to the freeze
int g_freezes = 0;

// What a plugin sees when it asks kiln.launch at this moment.
std::string read_launch(const char* who) {
    KilnLaunchStatus st{};
    st.size = sizeof st;
    const KilnResult rc = kiln::launch::table()->status(&st);
    return std::string(who) + (rc == KILN_OK && st.frozen == 1 ? ": frozen " + std::string(kiln::launch::backend_name(st.backend)) +
                                                                     ", verified " + std::to_string(st.build_verified)
                                                             : ": not frozen");
}
}  // namespace

#define KLOG_I(...) (void)0
#define KLOG_W(...) (void)0
#define KLOG_E(...) (void)0
#define KILN_BUILD "+test"
using DWORD = unsigned long;
inline DWORD GetCurrentProcessId() { return 1; }

namespace kiln {
constexpr std::string_view kVersion = "0.2.0-dev";
struct BootFacts {
    double started_ms = 0;
    std::string started;
    bool image_ok = false, verified = false, hooks = false;
    std::string safe_idle;
    std::vector<std::string> errors;
};
struct Image {
    uint32_t side = KILN_SIDE_CLIENT;
    std::string sha256 = std::string(64, 'e');
};
const Image& image() {
    static Image i;
    return i;
}
struct Config {
    bool preview_all = false, policy_any = false;
    std::vector<std::string> preview;
};
const Config& config() {
    static Config c;
    c.policy_any = g_policy_any;
    return c;
}
struct BootInfo {
    uint64_t addon_log_size = 0;
} g_boot_info;
void snapshot_entry() {}
void init_paths() {}
double now_ms() { return 0; }
std::string utc_now() { return "now"; }
void log_proxy() {}
void staged_boot() { calls.push_back("staged"); }
void load_config() { calls.push_back("kiln.json"); }
bool init_image() { return g_image_ok; }
const char* side_by_name() { return "client"; }
std::vector<std::string> start_plan() { return {}; }
void check_entry_patch(BootFacts&) {}
void write_boot_json(const BootFacts&) {}
void apply_side(uint32_t) {}
const char* side_name(uint32_t) { return "client"; }
bool build_verified(uint32_t, const std::string&, std::string* note) {
    *note = "not in the build records";
    return g_verified;
}
namespace log { void start() { calls.push_back("kiln.log"); } }
namespace profile {
struct State {
    uint32_t mode = KILN_PROFILE_ACTIVE;
    std::string file, error;
};
void load() { calls.push_back("profile"); }
const State& state() {
    static State s;
    return s;
}
}  // namespace profile
namespace sigdb { inline void code_patch_map() {} }
namespace hooks {
void add_map_extension(void (*)()) {}
bool serve_early() { return true; }
}  // namespace hooks
namespace compat {
void init() {}
void apply_early() { calls.push_back("compat early"); }
}  // namespace compat
namespace progress_state {
std::string utc_now_ms() { return "now"; }
void begin(const std::string&, const char*, double, const std::string&, const std::vector<std::string>&) {}
void step(const char*, std::string_view = {}, std::string_view = {}, uint64_t = 0, uint64_t = 0) {}
void idle(const std::string&) {}
}  // namespace progress_state
namespace caps {
void core_rows(bool, bool, const std::string&) {}
void boot_complete() {}
}  // namespace caps
namespace gates {
void configure(uint32_t, bool, const std::vector<std::string>&) { calls.push_back("gates"); }
}  // namespace gates
namespace foreign { void wait_for_scanners(uint64_t) { calls.push_back("scanners"); } }
namespace native_mods {
void discover(bool) { calls.push_back("discover"); }
void run_early() { calls.push_back(read_launch("native early mod")); }
void run_late() { calls.push_back(read_launch("chain [late]")); }
}  // namespace native_mods
namespace early_plugins { void run() { calls.push_back(read_launch("early plugin")); } }
namespace launch {
// launch_boot.cpp's freeze_at_boot reads kiln.json, the profile and the plan; here it freezes what the case expects:
// hosted unless idle (no kiln.json), or kiln for the kiln cases (a plan's selection, unless idle), with boot's verified
// as launch_boot.cpp sets it (test-launch.sh checks that line in launch_boot.cpp itself).
void freeze_at_boot(uint32_t, bool verified, const std::string& safe_idle) {
    ++g_freezes;
    g_safe_idle_seen = safe_idle;
    g_verified_seen = verified;
    Inputs in;
    set_idle(in, safe_idle, KILN_PROFILE_ACTIVE, true);
    Decision d = decide(in);
    if (g_kiln && d.backend != KILN_LAUNCH_BACKEND_NONE) d.backend = KILN_LAUNCH_BACKEND_KILN, d.reason_code = KILN_LAUNCH_REASON_PLAN;
    d.build_verified = verified ? 1u : 0u;
    freeze(d, 42, 1);
    calls.push_back("freeze");
}
}  // namespace launch

namespace {
#include "launch_boot_order.inc"
}  // namespace
}  // namespace kiln

namespace {
size_t at(const std::string& what) {
    for (size_t i = 0; i < calls.size(); ++i)
        if (calls[i] == what) return i;
    return calls.size();
}
void run_case(const char* name, bool image_ok, bool verified, bool policy_any, bool kiln = false) {
    calls.clear();
    g_freezes = 0;
    g_safe_idle_seen.clear();
    g_verified_seen = !verified;
    g_image_ok = image_ok, g_verified = verified, g_policy_any = policy_any, g_kiln = kiln;
    kiln::launch::reset_for_tests();
    kiln::boot();
    check(g_freezes == 1, std::string(name) + ": frozen exactly once");
    std::string seen;
    for (const auto& c : calls) seen += "[" + c + "] ";
    check(at("kiln.log") < at("staged") && at("staged") < at("kiln.json") && at("staged") < at("profile") &&
              at("staged") < at("discover") && at("staged") < at("freeze"),
          std::string(name) + ": staged mod updates after kiln.log opens, before kiln.json, the profile and native mods: " + seen);
    if (!image_ok) {
        check(at("freeze") < at("chain [late]: frozen none, verified 0"), std::string(name) + ": frozen before chain [late]: " + seen);
        check(g_safe_idle_seen == "the game image cannot be parsed", std::string(name) + ": with the safe-idle reason");
        check(!g_verified_seen, std::string(name) + ": the build was never asked, so the freeze is told not verified");
        check(at("early plugin: frozen none, verified 0") == calls.size(), std::string(name) + ": no early plugin runs");
        return;
    }
    check(g_verified_seen == verified, std::string(name) + ": the freeze gets boot's own build_verified answer");
    // LP1b: the record says the build's answer for a backend that runs, 0 for backend none
    const bool runs = verified || policy_any;
    const std::string backend = std::string(runs ? (kiln ? "kiln" : "hosted") : "none") + ", verified " +
                                (runs && verified ? "1" : "0");
    check(at("gates") < at("freeze"), std::string(name) + ": the gate list is configured first (the held row): " + seen);
    // compat "disable" rules switch profile entries off in apply_early: the selection is computed from the profile
    // as it is after them, and still before any native mod is discovered or runs
    check(at("compat early") < at("freeze"), std::string(name) + ": frozen after compat's early rules: " + seen);
    check(at("freeze") < at("discover") && at("freeze") < at("scanners"),
          std::string(name) + ": frozen before native mods are discovered: " + seen);
    check(at("native early mod: frozen " + backend) < calls.size(),
          std::string(name) + ": a native mod of phase early sees it frozen (" + backend + "): " + seen);
    check(at("early plugin: frozen " + backend) < calls.size(),
          std::string(name) + ": the early plugin sees it frozen (" + backend + "): " + seen);
    if (verified || policy_any)
        check(g_safe_idle_seen.empty(), std::string(name) + ": no safe idle known at the freeze");
    else
        check(g_safe_idle_seen.find("is not verified (not in the build records) and policy is verified-only") !=
                  std::string::npos,
              std::string(name) + ": the safe idle of verified-only reaches the freeze: " + g_safe_idle_seen);
}
}  // namespace

int main() {
    for (const bool kiln : {false, true}) {
        run_case(kiln ? "kiln, verified build, verified-only" : "verified build", true, true, false, kiln);
        run_case(kiln ? "kiln, verified build, policy any" : "verified build, policy any", true, true, true, kiln);
        run_case(kiln ? "kiln, unverified build, policy any" : "unverified build, policy any", true, false, true, kiln);
        run_case(kiln ? "kiln, unverified build, verified-only" : "unverified build, verified-only", true, false, false, kiln);
    }
    run_case("image cannot be parsed", false, true, false);
    std::printf("launch boot order: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
