// SPDX-License-Identifier: MIT
// The release hold of Kiln's interpreter for ShroudForge mods (src/sfi_release_policy.h) through the client gate
// (src/gates.cpp), both as they are, on the host. test-sfi-release-policy.sh builds it as every release and preview build
// is, with ASan+UBSan and with TSan, against the compiled list as the core's build generates it from native/core/gates.
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "gates.h"
#include "kiln_gates.g.h"  // kBuiltIn: the compiled list, as gates.cpp includes it
#include "log.h"
#include "services/kiln_caps.h"
#include "sfi_release_policy.h"

namespace gates = kiln::gates;
namespace policy = kiln::sfi_release_policy;

#ifdef _WIN32  // the same test cross-built with clang-cl (Wine, Windows)
int setenv(const char* name, const char* value, int) { return _putenv_s(name, value); }
int unsetenv(const char* name) { return _putenv_s(name, ""); }
#endif

namespace {
std::mutex t_mutex;
std::vector<std::string> t_lines;  // KLOG_* lines

int checks = 0, failures = 0;
void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", what.c_str());
    }
}

// kDev: this build opens the kiln-sfi.sf18 veto itself; never a release or preview build.
constexpr bool kDev = false;
const char* const kBuild = "release build";

const std::string kInterp(policy::kInterpreter), kSf18(policy::kSf18);
const std::string kGate = KILN_CAP_GATE_TEXT;
const std::string kInterpText =
    kGate + "sfi-backend-roundtrip, sfi-save-sensitive, sfi-three-surface-warning, sfi-ui-actions-notices, sfi-ui-page, "
            "sfi-ui-settings, sfi-warning-accessibility";
const std::string kSf18Text = kGate + "sfi-sf18-final";

std::string text_of(const std::string& name) {
    for (const gates::Held& h : gates::held())
        if (h.name == name) return h.text;
    return {};
}
bool held(const std::string& name) {
    const bool listed = !text_of(name).empty();
    check(listed == gates::is_held(name), "is_held agrees with held() for " + name);
    return listed;
}
bool logged(const std::string& part) {
    std::lock_guard l(t_mutex);
    for (const std::string& line : t_lines)
        if (line.find(part) != std::string::npos) return true;
    return false;
}
void clear_log() {
    std::lock_guard l(t_mutex);
    t_lines.clear();
}
std::string sf18_expected() { return kDev ? kSf18Text : std::string(policy::kVetoText); }
// The compiled list's form: kiln-sfi.interpreter on both sides (a preview), or no row of it (a normal choice).
bool interp_listed() {
    for (const gates::Gate* g = gates::kBuiltIn; g->name; ++g)
        if (kInterp == g->name && g->side == gates::kBoth) return true;
    return false;
}
}  // namespace

namespace kiln::log {
void writef(int, const char*, const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    std::lock_guard l(t_mutex);
    t_lines.emplace_back(b);
}
}  // namespace kiln::log

namespace {

void api() {
    check(!policy::upstream_180_final(), "no final ShroudForge 1.8.x is recorded in this build");
    check(policy::release_allows_180() == kDev, std::string("the kiln-sfi.sf18 veto: ") + (kDev ? "open" : "closed") +
                                                     " in a " + kBuild);
    for (std::string_view t : {policy::kVetoText, policy::kUnlistedText})
        check(t.substr(0, kGate.size()) == kGate && t.size() > kGate.size() && t.find("preview") != std::string_view::npos,
              "the policy's texts begin with KILN_CAP_GATE_TEXT (kiln_caps_held) and say preview does not lift them");
}

// The compiled list: kiln-sfi.sf18, and kiln-sfi.interpreter while it is a preview (native/core/gates/sfi.json), nothing
// of dev.29's (the list ships empty on main); each row as the list holds it, the policy's rows on top. Without the
// interpreter's row the interpreter is open on both sides, with or without "preview"; the sf18 veto holds either way.
void compiled_list() {
    const bool listed = interp_listed();
    std::printf("sfi_release_policy (%s): the compiled list %s\n", kBuild,
                listed ? "holds kiln-sfi.interpreter (a preview)" : "has no kiln-sfi.interpreter row (a normal choice)");
    gates::use_list_for_tests(nullptr);
    std::set<std::string> names;
    for (const gates::Held& h : gates::held()) names.insert(h.name);
    check(names == (listed ? std::set<std::string>{kInterp, kSf18} : std::set<std::string>{kSf18}),
          "the compiled list holds kiln-sfi.sf18, kiln-sfi.interpreter only as a preview, and nothing else (no dev.29 row "
          "is held again)");
    check(listed ? text_of(kInterp) == kInterpText : !held(kInterp),
          std::string("before configure: kiln-sfi.interpreter ") +
              (listed ? "held with its guided checks (" + text_of(kInterp) + ")" : "open"));
    check(text_of(kSf18) == sf18_expected(), "before configure: kiln-sfi.sf18 (" + text_of(kSf18) + ")");

    for (uint32_t side : {KILN_SIDE_CLIENT, KILN_SIDE_SERVER}) {
        const std::string on = side == KILN_SIDE_CLIENT ? " (client)" : " (server)";
        gates::configure(side, false, {});
        check(held(kInterp) == listed && (!listed || text_of(kInterp) == kInterpText),
              std::string(listed ? "no preview: the interpreter is held" : "no preview: the interpreter is open") + on);
        check(held(kSf18) && text_of(kSf18) == sf18_expected(), "no preview: kiln-sfi.sf18 is held" + on);

        clear_log();
        gates::configure(side, true, {});
        check(!held(kInterp), "preview true lifts the interpreter" + on);
        check(held(kSf18) != kDev, kDev ? "preview true lifts kiln-sfi.sf18 in a developer build" + on
                                        : "preview true does not lift kiln-sfi.sf18" + on);
        if (!kDev)
            check(text_of(kSf18) == std::string(policy::kVetoText) &&
                      logged("kiln-sfi.sf18 stays held by the release policy"),
                  "...it reads the veto and kiln.log says the release policy holds it" + on);

        gates::configure(side, false, {kInterp});
        check(!held(kInterp) && held(kSf18) && text_of(kSf18) == sf18_expected(),
              "preview naming the interpreter lifts it alone" + on);

        gates::configure(side, false, {kInterp, kSf18});
        check(!held(kInterp) && held(kSf18) != kDev, "preview naming both rows: sf18 stays held unless the veto is open" + on);

        gates::configure(side, false, {kSf18});
        check(held(kInterp) == listed && held(kSf18) != kDev,
              "preview naming kiln-sfi.sf18 alone never lifts the interpreter, nor holds it when the list does not" + on);
    }
}

// Forged inputs: no environment variable, kiln.json "preview" word or capability name opens the veto.
void forged_inputs() {
    const std::vector<std::string> vars = {
        "KILN_SFI_UPSTREAM_180_FINAL", "KILN_UPSTREAM_180_FINAL", "upstream180Final", "KILN_SHROUDFORGE_RUNTIME",
        "KILN_RELEASE_POLICY"};
    const std::vector<std::string> words = {
        "kiln-sfi.sf18", "upstream180Final", "all", "kiln-sfi.*", "Kiln-Sfi.Sf18"};
    for (const std::string& var : vars) setenv(var.c_str(), "1", 1);
    check(policy::release_allows_180() == kDev && !policy::upstream_180_final(),
          "environment variables do not change the release policy");
    gates::use_list_for_tests(nullptr);
    gates::configure(KILN_SIDE_CLIENT, false, words);
    check(held(kSf18) != kDev, "preview words that look like a switch do not open the veto");
    check(held(kInterp) == interp_listed(), "...and do not lift the interpreter (only its exact name, or true, does)");
    for (const std::string& var : vars) unsetenv(var.c_str());
}

// The list's forms: kiln-sfi.interpreter on both sides is a gate row ("preview" lifts it); no row of that name (a near
// name is none) leaves it open, with or without "preview"; a row on one side only holds it on both by the policy, and
// "preview" does not lift that. The sf18 veto holds in every form.
void fail_closed() {
    enum Interp { kOpen, kRow, kOneSide };
    struct Case {
        const char* what;
        std::vector<gates::Gate> rows;
        Interp interp;
        bool sf18_listed;
    };
    const std::vector<Case> cases = {
        {"an empty list", {}, kOpen, false},
        {"another capability only", {{"camera.fov", "03-c11", gates::kBoth}}, kOpen, false},
        {"the interpreter on the client only", {{"kiln-sfi.interpreter", "sfi-ui-page", gates::kClient}}, kOneSide, false},
        {"the interpreter on the server only", {{"kiln-sfi.interpreter", "sfi-ui-page", gates::kServer}}, kOneSide, false},
        {"the interpreter on one side, sf18 listed", {{"kiln-sfi.interpreter", "sfi-ui-page", gates::kClient},
                                                     {"kiln-sfi.sf18", "sfi-sf18-final", gates::kBoth}}, kOneSide, true},
        {"a near name", {{"kiln-sfi.interpreterx", "sfi-ui-page", gates::kBoth}, {"Kiln-Sfi.Interpreter", "a", gates::kBoth},
                         {"kiln-sfi.sf1", "b", gates::kBoth}}, kOpen, false},
        {"sf18 only (no interpreter row)", {{"kiln-sfi.sf18", "sfi-sf18-final", gates::kBoth}}, kOpen, true},
        {"sf18 on one side only", {{"kiln-sfi.sf18", "sfi-sf18-final", gates::kClient}}, kOpen, false},
        {"both rows", {{"kiln-sfi.interpreter", "sfi-ui-page", gates::kBoth}, {"kiln-sfi.sf18", "sfi-sf18-final", gates::kBoth}},
         kRow, true},
    };
    for (const Case& c : cases) {
        std::vector<gates::Gate> list = c.rows;
        list.push_back({nullptr, nullptr, 0});
        gates::use_policy_list_for_tests(list.data());
        const bool fov_listed = !c.rows.empty() && c.rows[0].name == std::string("camera.fov");
        for (uint32_t side : {KILN_SIDE_CLIENT, KILN_SIDE_SERVER})
            for (int mode = 0; mode < 3; ++mode) {  // no preview, preview true, preview naming every row
                const bool lifted = mode != 0;
                const std::string what = std::string(c.what) + (side == KILN_SIDE_CLIENT ? ", client" : ", server") +
                                         (mode == 0 ? ", no preview" : mode == 1 ? ", preview true" : ", preview names");
                gates::configure(side, mode == 1,
                                 mode == 2 ? std::vector<std::string>{kInterp, kSf18, "camera.fov"} : std::vector<std::string>{});
                if (c.interp == kOneSide)
                    check(held(kInterp) && text_of(kInterp) == std::string(policy::kUnlistedText),
                          what + ": the interpreter stays held by the policy");
                else if (c.interp == kRow)
                    check(held(kInterp) == !lifted && (lifted || text_of(kInterp) == kGate + "sfi-ui-page"),
                          what + ": the listed interpreter row behaves as any gate row");
                else
                    check(!held(kInterp), what + ": without its row the interpreter is open");
                if (!kDev)
                    check(held(kSf18) && text_of(kSf18) == std::string(policy::kVetoText), what + ": the veto holds sf18");
                else if (!c.sf18_listed)
                    check(held(kSf18) && text_of(kSf18) == std::string(policy::kUnlistedText),
                          what + ": a developer build without the sf18 row holds it by the policy");
                else
                    check(held(kSf18) == !lifted, what + ": a developer build's sf18 row behaves as any gate row");
                check(held("camera.fov") == (fov_listed && !lifted), what + ": another capability follows preview as before");
            }
        // before configure (the early safe idle): the policy holds too
        gates::use_policy_list_for_tests(list.data());
        check(held(kInterp) == (c.interp != kOpen) && held(kSf18),
              std::string(c.what) + ": before configure, sf18 held, the interpreter as its list says");
    }
    // the policy's rows are sorted in with the list's
    static const gates::Gate mixed[] = {{"a.first", "x", gates::kBoth}, {"z.last", "y", gates::kBoth}, {nullptr, nullptr, 0}};
    gates::use_policy_list_for_tests(mixed);
    gates::configure(KILN_SIDE_CLIENT, false, {});
    std::vector<std::string> order;
    for (const gates::Held& h : gates::held()) order.push_back(h.name);
    check(order == std::vector<std::string>{"a.first", kSf18, "z.last"}, "held() stays sorted with the policy's rows");
    static const gates::Gate one_side[] = {{"a.first", "x", gates::kBoth}, {"kiln-sfi.interpreter", "x", gates::kServer},
                                           {"z.last", "y", gates::kBoth}, {nullptr, nullptr, 0}};
    gates::use_policy_list_for_tests(one_side);
    gates::configure(KILN_SIDE_CLIENT, false, {});
    order.clear();
    for (const gates::Held& h : gates::held()) order.push_back(h.name);
    check(order == std::vector<std::string>{"a.first", kInterp, kSf18, "z.last"},
          "held() stays sorted with both policy rows (the interpreter on one side only)");
    check(gates::is_held("a.first") && gates::is_held("z.last"), "is_held's binary search finds every row after the merge");
    // a test list without the policy (caps_test's): no policy rows
    gates::use_list_for_tests(mixed);
    gates::configure(KILN_SIDE_CLIENT, true, {});
    check(gates::held().empty(), "use_list_for_tests: a test list has no policy rows (the registry tests' own lists)");
    gates::use_list_for_tests(nullptr);
    check(held(kInterp) == interp_listed() && held(kSf18), "use_list_for_tests(nullptr): the compiled list brings the policy back");
}

// Readers race configure(): in a release build kiln-sfi.sf18 is never seen open, whatever "preview" says meanwhile.
void readers_race_configure() {
    gates::use_list_for_tests(nullptr);
    std::atomic<bool> stop{false};
    std::atomic<int> open_seen{0}, reads{0}, started{0};
    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
        readers.emplace_back([&] {
            ++started;
            while (!stop.load()) {
                if (!gates::is_held(kSf18)) ++open_seen;
                bool in_list = false;
                for (const gates::Held& h : gates::held()) in_list |= h.name == kSf18;
                if (!in_list) ++open_seen;
                ++reads;
            }
        });
    while (started.load() < 4) std::this_thread::yield();
    // at least 300 configures, and on until the readers read 2000 times (a slow thread start, as under Wine), 10 s at most
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (int i = 0; i < 300 || (reads.load() < 2000 && std::chrono::steady_clock::now() < until); ++i)
        gates::configure(i % 2 ? KILN_SIDE_CLIENT : KILN_SIDE_SERVER, i % 3 == 0, {kSf18, kInterp});
    stop = true;
    for (auto& t : readers) t.join();
    if (!kDev) check(open_seen == 0, "readers racing configure never see kiln-sfi.sf18 open (" + std::to_string(reads.load()) + " reads)");
    check(reads > 0, "the readers ran");
}

}  // namespace

int main() {
    api();
    compiled_list();
    forged_inputs();
    fail_closed();
    readers_race_configure();
    std::printf("sfi_release_policy (%s): %d checks, %d failures\n", kBuild, checks, failures);
    return failures ? 1 : 0;
}
