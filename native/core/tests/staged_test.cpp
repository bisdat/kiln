// SPDX-License-Identifier: MIT
// kiln.staged (src/staged.cpp as it is): staged mod and Kiln updates applied at the start, with rollback.
//   - the path rules (traversal, absolute, drive, device names, Kiln's own folders);
//   - a mod update, a new mod, .disabled carried over, the kept version, the probation record;
//   - refusals: a bad checksum, a file the manifest does not list, a missing file, a path outside the unit, a link in the
//     staged files or on the way to the target, an id that is not the mod's, a Kiln update planned on another install;
//   - a requested rollback; the automatic rollback of a mod that failed to load (or whose start ended while it loaded)
//     and of a Kiln update whose next start did not confirm it, once and never in a loop;
//   - every crash point: a child process runs the start and ends itself at the k-th change (each rename and journal
//     write), for every k, in five scenarios; the next start must see the old version before it tries again (or the
//     new one when the crash came after the commit) and end with the new version; a crash while putting back too;
//   - a change interrupted three times is dropped; a rename that fails puts everything back; a damaged or foreign
//     journal is left alone;
//   - kiln/data (persistent state: kiln-network/server-id, dev.31's registry/entry-compat.json, the admin token) is the
//     same byte for byte after every stage, apply, crash, recovery and rollback, and no kept version holds any of it,
//     even when a plan, a kept version or a journal names a path under it.
// Built twice: on the host (POSIX paths, fork) and with clang-cl under Wine (the Win32 calls, CreateProcess):
//   native/core/tests/test-staged.sh   native/core/tests/test-staged-wine.sh
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "../src/services/boot_gamedata.h"
#include "../src/staged.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;
using nlohmann::json;
using namespace kiln::staged;

static int failures = 0, checks = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        ++checks;                                                                  \
        if (!(cond)) {                                                             \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
            ++failures;                                                            \
        }                                                                          \
    } while (0)
#define CHECK_MSG(cond, msg)                                                                          \
    do {                                                                                              \
        ++checks;                                                                                     \
        if (!(cond)) {                                                                                \
            std::fprintf(stderr, "FAIL %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); \
            ++failures;                                                                               \
        }                                                                                             \
    } while (0)

// ---- scaffolding ----
using Tree = std::map<std::string, std::string>;  // relative path -> content

static fs::path g_scratch;
static fs::path g_self;

static std::string read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
static void put(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}
static std::string sha(const std::string& text) { return kiln::boot_gd::hex_of(kiln::boot_gd::sha256(text)); }
static bool there(const fs::path& p) {
    std::error_code ec;
    return fs::exists(fs::symlink_status(p, ec));
}
static Tree tree(const fs::path& dir, const std::string& skip = "") {
    Tree t;
    std::error_code ec;
    if (!fs::is_directory(dir)) return t;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        const std::string rel = it->path().lexically_relative(dir).generic_string();
        if (!skip.empty() && rel.rfind(skip, 0) == 0) continue;
        t[rel] = read(it->path());
    }
    return t;
}
static void write_tree(const fs::path& dir, const Tree& t) {
    for (const auto& [rel, text] : t) put(dir / rel, text);
}
static fs::path fresh(const std::string& name) {
    const fs::path d = g_scratch / name;
    fs::remove_all(d);
    fs::create_directories(d / "kiln");
    put(d / "enshrouded_server.exe", "exe");
    return d;
}

struct Log {
    std::vector<std::pair<Level, std::string>> lines;
    bool has(const std::string& s) const {
        for (const auto& l : lines)
            if (l.second.find(s) != std::string::npos) return true;
        return false;
    }
    std::string all() const {
        std::string out;
        for (const auto& l : lines) out += l.second + "\n";
        return out;
    }
};
static Env env_for(const fs::path& game, Log* log, bool server = true) {
    Env e;
    e.game = game;
    e.server = server;
    e.by = "test";
    e.kiln = "0.2.0-test";
    e.pid = 4242;
    if (log) e.log = [log](Level l, const std::string& s) { log->lines.emplace_back(l, s); };
    return e;
}
static fs::path staged(const fs::path& game) { return game / "kiln" / "staged"; }

// ---- a mod ----
static Tree mod_files(const std::string& id, const std::string& version) {
    return {{"kiln.mod.json", json{{"id", id}, {"version", version}, {"apiVersion", "0.2"}, {"entryAssembly", "Mod.dll"}}.dump(2)},
            {"Mod.dll", "dll " + id + " " + version},
            {"data/blocks/a.json", "{\"v\":\"" + version + "\"}"}};
}
static json manifest_of(const Tree& files) {
    json arr = json::array();
    for (const auto& [rel, text] : files) arr.push_back({{"path", rel}, {"sha256", sha(text)}, {"size", text.size()}});
    return arr;
}
// What Kiln.Admin's update_stage writes: files/, then staged.json last.
static void stage_mod(const fs::path& game, const std::string& folder, const std::string& id, const std::string& version,
                      const Tree& files, const std::function<void(json&)>& edit = {}) {
    const fs::path dir = staged(game) / "mods" / folder;
    write_tree(dir / "files", files);
    json doc = {{"format", 1}, {"kind", "mod"}, {"folder", folder}, {"id", id}, {"version", version},
                {"staged", "2026-11-23T10:00:00Z"}, {"by", "test"}, {"files", manifest_of(files)}};
    if (edit) edit(doc);
    put(dir / "staged.json", doc.dump(2));
}
static Tree mod_view(const fs::path& game, const std::string& folder = "m") { return tree(game / "kiln" / "mods" / folder); }

// ---- Kiln ----
static const std::string kReceipt1 = R"({"format":1,"runtime":{"version":"0.2.0-dev.30.9"},"files":[]})";
static const std::string kReceipt2 = R"({"format":1,"runtime":{"version":"0.2.0-dev.30.10"},"files":[]})";
static Tree kiln_old() {
    return {{"dinput8.dll", "proxy 1"},          {"kiln/kiln-core.dll", "core 1"}, {"kiln/services/a.dll", "a 1"},
            {"kiln/services/b.dll", "b 1 edited"}, {"kiln/.install.json", kReceipt1}};
}
static Tree kiln_new() {  // a.dll removed (unchanged), b.dll kept (the player changed it), host/new.dll added
    return {{"dinput8.dll", "proxy 2"},          {"kiln/kiln-core.dll", "core 2"}, {"kiln/host/new.dll", "new 2"},
            {"kiln/services/b.dll", "b 1 edited"}, {"kiln/.install.json", kReceipt2}};
}
// kiln/data: persistent Kiln state that no staged change may write, move or delete (dev.31 adds registry/ there).
static Tree data_tree() {
    return {{"kiln-network/server-id", "srv-4b8e2c\n"}, {"registry/entry-compat.json", "{\"format\":1,\"entries\":[]}"},
            {"kiln-admin/admin-token", "token"}};
}
static void put_data(const fs::path& game) { write_tree(game / "kiln" / "data", data_tree()); }
static void check_data(const fs::path& game, const std::string& label) {
    CHECK_MSG(tree(game / "kiln" / "data") == data_tree(), label + ": kiln/data changed");
    for (const auto& [rel, text] : tree(staged(game))) {
        (void)text;
        CHECK_MSG(rel.find("kiln/data") == std::string::npos && rel.find("server-id") == std::string::npos &&
                      rel.find("entry-compat") == std::string::npos,
                  label + ": kiln/staged holds a copy of kiln/data: " + rel);
    }
}

static void setup_kiln(const fs::path& game) {
    write_tree(game, kiln_old());
    put_data(game);  // the server's own files are never touched
    const Tree files = {{"dinput8.dll", "proxy 2"}, {"kiln/kiln-core.dll", "core 2"}, {"kiln/host/new.dll", "new 2"}, {"kiln/.install.json", kReceipt2}};
    const fs::path dir = staged(game) / "kiln";
    write_tree(dir / "files", files);
    json doc = {{"format", 1}, {"kind", "kiln"}, {"version", "0.2.0-dev.30.10"}, {"from", "0.2.0-dev.30.9"},
                {"base", {{"receipt", sha(kReceipt1)}}}, {"files", manifest_of(files)},
                {"remove", json::array({{{"path", "kiln/services/a.dll"}, {"sha256", sha("a 1")}},
                                        {{"path", "kiln/services/b.dll"}, {"sha256", sha("b 1")}}})}};
    put(dir / "staged.json", doc.dump(2));
}
static Tree kiln_view(const fs::path& game) {
    Tree t = tree(game, "kiln/staged/");
    t.erase("enshrouded_server.exe");
    for (auto it = t.begin(); it != t.end();) it = it->first.rfind("kiln/data/", 0) == 0 ? t.erase(it) : std::next(it);
    return t;
}

// ---- children (a start that ends itself at the k-th change) ----
static int g_crash_at = 0, g_calls = 0;
static std::vector<std::string> g_names;
static void crash_hook(const char*) {
    if (++g_calls == g_crash_at) {
#ifdef _WIN32
        TerminateProcess(GetCurrentProcess(), 77);
#else
        _exit(77);
#endif
    }
}
static void count_hook(const char* s) {
    ++g_calls;
    g_names.emplace_back(s);
}
enum class Kind { Mods, Kiln };
static Summary boot(Kind k, const fs::path& game, Log* log) {
    const Env e = env_for(game, log);
    return k == Kind::Mods ? boot_mods(e) : boot_kiln(e);
}
static int child_main(Kind k, int crash_at, const fs::path& game) {
    g_crash_at = crash_at;
    g_calls = 0;
    g_step_hook = &crash_hook;
    boot(k, game, nullptr);
    return 0;
}
static int run_child(Kind k, const fs::path& game, int crash_at) {
#ifdef _WIN32
    std::wstring cmd = L"\"" + g_self.wstring() + L"\" child " + (k == Kind::Mods ? L"mods " : L"kiln ") + std::to_wstring(crash_at) +
                       L" \"" + game.wstring() + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    std::fflush(nullptr);
    const pid_t pid = fork();
    if (pid == 0) _exit(child_main(k, crash_at, game));
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

// ---- a crash at every point ----
struct Scenario {
    const char* name;
    Kind kind;
    std::function<void(const fs::path&)> setup;
    std::function<Tree(const fs::path&)> view;
    Tree before, after;
    std::function<void(const fs::path&)> after_checks;
};

static void crash_everywhere(const Scenario& s) {
    // the clean run: how many changes, and where the commit is
    const fs::path clean = fresh(std::string("clean-") + s.name);
    put_data(clean);
    s.setup(clean);
    CHECK_MSG(s.view(clean) == s.before, s.name);
    g_calls = 0;
    g_names.clear();
    g_step_hook = &count_hook;
    Log log;
    boot(s.kind, clean, &log);
    g_step_hook = nullptr;
    const int n = g_calls;
    const std::vector<std::string> names = g_names;
    int commit = 0;
    for (int i = 0; i < n; ++i)
        if (names[static_cast<size_t>(i)] == "commit") commit = i + 1;
    CHECK_MSG(commit > 0 && n > commit, s.name);
    CHECK_MSG(s.view(clean) == s.after, std::string(s.name) + ": " + log.all());
    check_data(clean, s.name);
    if (s.after_checks) s.after_checks(clean);
    std::printf("  %s: %d changes (commit at %d)\n", s.name, n, commit);

    for (int k = 1; k <= n; ++k) {
        const std::string label = std::string(s.name) + " crash at " + std::to_string(k) + " (" + names[static_cast<size_t>(k - 1)] + ")";
        const fs::path g = fresh(std::string("crash-") + s.name + "-" + std::to_string(k));
        put_data(g);
        s.setup(g);
        CHECK_MSG(run_child(s.kind, g, k) == 77, label);
        check_data(g, label + ", right after the crash");
        // The next start: recovery first. Before the commit, the retry's journal must find the old version exactly.
        Tree at_retry;
        bool retried = false;
        static std::function<void(const char*)> inspect;
        inspect = [&](const char* step) {
            if (!retried && std::strcmp(step, "journal") == 0) {
                retried = true;
                at_retry = s.view(g);
            }
        };
        g_step_hook = [](const char* step) { inspect(step); };
        Log l2;
        const Summary sum = boot(s.kind, g, &l2);
        g_step_hook = nullptr;
        // a crash right after the journal went (the very end) leaves only an empty transaction folder: nothing to recover
        const bool journal_gone = names[static_cast<size_t>(k - 1)] == "finish-journal";
        CHECK_MSG(sum.recovered == (journal_gone ? 0 : 1), label + ": " + l2.all());
        if (journal_gone) {
            CHECK_MSG(!retried, label);
        } else if (k < commit) {
            CHECK_MSG(retried && at_retry == s.before, label + ": the start after the crash did not see the old version");
            CHECK_MSG(l2.has("was interrupted"), label + ": " + l2.all());
        } else {
            CHECK_MSG(!retried, label);
            CHECK_MSG(l2.has("had already committed"), label + ": " + l2.all());
        }
        CHECK_MSG(s.view(g) == s.after, label + ": " + l2.all());
        check_data(g, label + ", after the recovery");
        CHECK_MSG(!there(staged(g) / "tx"), label);
        CHECK_MSG(!there(staged(g) / "attempts"), label);
        if (s.after_checks) s.after_checks(g);

        // a second crash, at the first change of the start that puts the first one back
        if (k > 1 && k < commit) {
            const fs::path g2 = fresh(std::string("crash2-") + s.name + "-" + std::to_string(k));
            put_data(g2);
            s.setup(g2);
            CHECK_MSG(run_child(s.kind, g2, k) == 77, label + " (twice)");
            CHECK_MSG(run_child(s.kind, g2, 1) == 77, label + " (twice)");
            Log l3;
            boot(s.kind, g2, &l3);
            CHECK_MSG(s.view(g2) == s.after, label + " (twice): " + l3.all());
            check_data(g2, label + " (twice)");
            CHECK_MSG(!there(staged(g2) / "tx"), label + " (twice)");
            fs::remove_all(g2);
        }
        fs::remove_all(g);
    }
    fs::remove_all(clean);
}

static json read_json(const fs::path& p) {
    json j = json::parse(read(p), nullptr, false);
    return j.is_object() ? j : json::object();
}
static std::vector<json> results(const fs::path& game) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::directory_iterator it(staged(game) / "results", ec), end; !ec && it != end; it.increment(ec)) files.push_back(it->path());
    std::sort(files.begin(), files.end());
    std::vector<json> out;
    for (const auto& f : files) out.push_back(read_json(f));
    return out;
}

// ---- tests ----
static void test_paths() {
    std::string why;
    for (const char* ok : {"kiln.mod.json", "Mod.dll", "data/blocks/a.json", ".disabled", "a b/c-d_e.f", "ünïcode/ok.txt"})
        CHECK_MSG(rel_path_ok(ok, &why), ok);
    for (const char* bad : {"", "../evil.dll", "a/../../evil.dll", "a/./b", "/etc/passwd", "C:/Windows/x.dll", "a\\b", "a//b", "a/",
                            "con", "CON.txt", "data/nul.json", "com1", "lpt9.log", "x.", "x ", "a:stream", "a*b", "a?b", "a<b", "a|b",
                            "tab\there", "\x7f"})
        CHECK_MSG(!rel_path_ok(bad, &why), bad);
    CHECK(!rel_path_ok(std::string("\xff\xfe"), &why));
    CHECK(!rel_path_ok(std::string(241, 'a'), &why));
    CHECK(rel_path_ok(std::string(240, 'a'), &why));
    for (const char* ok : {"dinput8.dll", "DINPUT8.DLL", "kiln/kiln-core.dll", "kiln/.install.json", "kiln/mods/kiln-admin/Kiln.Admin.dll",
                           "kiln/host/Kiln.Host.dll", "kiln/dataset.txt"})
        CHECK_MSG(kiln_path_ok(ok, &why), ok);
    for (const char* bad : {"enshrouded_server.exe", "steam_api64.dll", "kiln/staged/kiln/staged.json", "kiln/data/kiln-admin/admin-token",
                            "Kiln/Config/x.json", "kiln/logs/kiln.log", "kiln/state/boot.json", "kiln/backups/x", "kiln/control/x.cmd",
                            "kiln/crash-reports/x.txt", "kiln/staged", "../kiln/x", "kiln/../x"})
        CHECK_MSG(!kiln_path_ok(bad, &why), bad);
    for (const char* ok : {"m", "kiln-webadmin", "Kiln.Example", "my mod"}) CHECK_MSG(mod_folder_ok(ok), ok);
    const std::string long_name(65, 'a');
    for (const std::string bad : {"", ".stage-1", "a/b", "..", ".", "con"}) CHECK_MSG(!mod_folder_ok(bad), bad);
    CHECK(!mod_folder_ok(long_name) && mod_folder_ok(long_name.substr(1)));
}

// schema/fixtures/staged/paths.json: the cases Kiln.Admin's Staging (C#) answers the same way (StagingTests).
static void test_paths_fixture() {
    const char* file = std::getenv("KILN_STAGED_FIXTURE");
    CHECK_MSG(file && *file, "KILN_STAGED_FIXTURE is not set (the test scripts set it)");
    if (!file || !*file) return;
    const json f = json::parse(read(fs::path(file)), nullptr, false);
    CHECK_MSG(f.is_object() && f.contains("rel") && f.contains("kiln") && f.contains("folder"), file);
    if (!f.is_object()) return;
    std::string why;
    int n = 0;
    for (const char* kind : {"rel", "kiln", "folder"}) {
        for (const char* verdict : {"ok", "bad"}) {
            for (const auto& c : f[kind][verdict]) {
                const std::string path = c.get<std::string>();
                const bool got = std::string(kind) == "rel" ? rel_path_ok(path, &why) : std::string(kind) == "kiln" ? kiln_path_ok(path, &why) : mod_folder_ok(path);
                CHECK_MSG(got == (std::string(verdict) == "ok"), std::string(kind) + " " + verdict + ": " + path);
                ++n;
            }
        }
    }
    CHECK(n > 60);
}

static void test_mod_update_and_rollbacks() {
    const fs::path g = fresh("mod-update");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    put(g / "kiln" / "mods" / "other" / "kiln.mod.json", "{\"id\":\"other\",\"version\":\"1\"}");
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    Log log;
    Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(s.applied == 1 && s.failed == 0, log.all());
    CHECK(mod_view(g) == mod_files("m", "2.0.0"));
    CHECK(tree(staged(g) / "previous" / "mods" / "m" / "files") == mod_files("m", "1.0.0"));
    CHECK(!there(staged(g) / "mods" / "m"));
    CHECK(mod_view(g, "other").size() == 1);  // the other mod is not touched
    const json kept = read_json(staged(g) / "previous" / "mods" / "m" / "kept.json");
    CHECK(kept.value("version", "") == "1.0.0" && kept.value("replacedBy", "") == "2.0.0" && kept.value("reason", "") == "update");
    CHECK(kept["entries"].size() == 1 && kept["entries"][0].value("path", "") == "kiln/mods/m" && kept["entries"][0].value("before", "") == "present");
    json p = read_json(staged(g) / "probation" / "mod.m.json");
    CHECK(p.value("state", "") == "pending" && p.value("version", "") == "2.0.0" && p.value("from", "") == "1.0.0" && p.value("id", "") == "m");
    CHECK(log.has("mod folder m: updated to 2.0.0 (from 1.0.0)"));
    auto r = results(g);
    CHECK(r.size() == 1 && r[0].value("op", "") == "update" && r[0].value("ok", false) && r[0].value("version", "") == "2.0.0");

    // "pending" (not tried yet: the server stopped before the mods loaded, or the mod is switched off): nothing happens
    Log l2;
    s = boot(Kind::Mods, g, &l2);
    CHECK(s.applied == 0 && s.rolled_back == 0 && s.failed == 0);
    CHECK(mod_view(g) == mod_files("m", "2.0.0"));

    // Kiln.Host's verdict "failed": rolled back once at the next start, and said so
    p["state"] = "failed";
    p["error"] = "OnLoad threw InvalidOperationException: boom";
    put(staged(g) / "probation" / "mod.m.json", p.dump());
    Log l3;
    s = boot(Kind::Mods, g, &l3);
    CHECK_MSG(s.rolled_back == 1, l3.all());
    CHECK(mod_view(g) == mod_files("m", "1.0.0"));
    CHECK(tree(staged(g) / "previous" / "mods" / "m" / "files") == mod_files("m", "2.0.0"));
    CHECK(read_json(staged(g) / "previous" / "mods" / "m" / "kept.json").value("reason", "") == "auto-rollback");
    CHECK(!there(staged(g) / "probation" / "mod.m.json"));
    CHECK_MSG(l3.has("it failed to load right after its update to 2.0.0: OnLoad threw InvalidOperationException: boom; rolled back to 1.0.0 (once"), l3.all());
    r = results(g);
    CHECK(r.back().value("op", "") == "auto-rollback" && r.back().value("ok", false) && r.back().value("version", "") == "1.0.0");
    // never in a loop: the version it went back to has no probation, so nothing happens again
    for (int i = 0; i < 3; ++i) {
        Log l4;
        s = boot(Kind::Mods, g, &l4);
        CHECK(s.rolled_back == 0 && s.applied == 0);
        CHECK(mod_view(g) == mod_files("m", "1.0.0"));
    }

    // a requested rollback goes back to the kept version (2.0.0 now) and keeps the one it replaced
    put(staged(g) / "rollback" / "mod.m.json", json{{"format", 1}, {"unit", "mod.m"}, {"by", "kiln-webadmin"}}.dump());
    Log l5;
    s = boot(Kind::Mods, g, &l5);
    CHECK_MSG(s.rolled_back == 1, l5.all());
    CHECK(mod_view(g) == mod_files("m", "2.0.0"));
    CHECK(tree(staged(g) / "previous" / "mods" / "m" / "files") == mod_files("m", "1.0.0"));
    CHECK(!there(staged(g) / "rollback" / "mod.m.json"));
    CHECK(l5.has("rolled back to 2.0.0 (from 1.0.0, requested by kiln-webadmin)"));
    CHECK(!there(staged(g) / "probation" / "mod.m.json"));  // a rollback is not watched
    fs::remove_all(g);
}

static void test_mod_loading_state_and_new_mod() {
    // a new mod (nothing kept): its rollback removes it again
    const fs::path g = fresh("mod-new");
    stage_mod(g, "fresh", "fresh", "1.0.0", mod_files("fresh", "1.0.0"));
    Log log;
    boot(Kind::Mods, g, &log);
    CHECK(mod_view(g, "fresh") == mod_files("fresh", "1.0.0"));
    const json kept = read_json(staged(g) / "previous" / "mods" / "fresh" / "kept.json");
    CHECK(kept["entries"][0].value("before", "") == "absent" && kept.value("version", "x").empty());
    CHECK(!there(staged(g) / "previous" / "mods" / "fresh" / "files"));
    // the start ended while it loaded ("loading" left behind): rolled back = removed
    json p = read_json(staged(g) / "probation" / "mod.fresh.json");
    p["state"] = "loading";
    put(staged(g) / "probation" / "mod.fresh.json", p.dump());
    Log l2;
    const Summary s = boot(Kind::Mods, g, &l2);
    CHECK_MSG(s.rolled_back == 1, l2.all());
    CHECK(!there(g / "kiln" / "mods" / "fresh"));
    CHECK(l2.has("ended while it was loading; rolled back to (not installed)"));
    CHECK(tree(staged(g) / "previous" / "mods" / "fresh" / "files") == mod_files("fresh", "1.0.0"));
    fs::remove_all(g);
}

static void test_disabled_carried_over() {
    const fs::path g = fresh("mod-disabled");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    put(g / "kiln" / "mods" / "m" / ".disabled", "");
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    Log log;
    boot(Kind::Mods, g, &log);
    Tree want = mod_files("m", "2.0.0");
    want[".disabled"] = "";
    CHECK_MSG(mod_view(g) == want, log.all());
    fs::remove_all(g);
}

static void refused(const char* name, const std::function<void(const fs::path&)>& setup, const std::string& reason) {
    const fs::path g = fresh(std::string("refused-") + name);
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    setup(g);
    Log log;
    const Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(s.failed == 1 && s.applied == 0, std::string(name) + ": " + log.all());
    CHECK_MSG(mod_view(g) == mod_files("m", "1.0.0"), name);  // untouched
    CHECK_MSG(!there(staged(g) / "mods" / "m"), name);      // the refused update is removed
    CHECK_MSG(!there(staged(g) / "tx"), name);
    CHECK_MSG(log.has("the staged update") && log.has("is refused and removed") && log.has(reason), std::string(name) + ": " + log.all());
    const auto r = results(g);
    CHECK_MSG(r.size() == 1 && !r[0].value("ok", true) && r[0].value("error", "").find(reason) != std::string::npos, name);
    fs::remove_all(g);
}

static void test_refusals() {
    refused("checksum", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        put(staged(g) / "mods" / "m" / "files" / "Mod.dll", "dll m 2.0.X");  // same size, other bytes
    }, "Mod.dll: checksum mismatch");
    refused("size", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        put(staged(g) / "mods" / "m" / "files" / "Mod.dll", "short");
    }, "Mod.dll: 5 bytes, staged.json says");
    refused("unlisted", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        put(staged(g) / "mods" / "m" / "files" / "extra.dll", "planted");
    }, "extra.dll is in the staged files but not in staged.json");
    refused("missing", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        fs::remove(staged(g) / "mods" / "m" / "files" / "data" / "blocks" / "a.json");
    }, "data/blocks/a.json is missing");
    refused("traversal", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) {
            d["files"].push_back({{"path", "../../evil.dll"}, {"sha256", sha("x")}, {"size", 1}});
        });
    }, "a . or .. segment");
    refused("absolute", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) {
            d["files"].push_back({{"path", "/tmp/evil.dll"}, {"sha256", sha("x")}, {"size", 1}});
        });
    }, "absolute");
    refused("drive", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) {
            d["files"].push_back({{"path", "C:/Windows/evil.dll"}, {"sha256", sha("x")}, {"size", 1}});
        });
    }, "the character :");
    refused("case-twice", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) {
            d["files"].push_back({{"path", "MOD.DLL"}, {"sha256", sha("x")}, {"size", 1}});
        });
    }, "listed twice");
    refused("id", [](const fs::path& g) { stage_mod(g, "m", "other-id", "2.0.0", mod_files("m", "2.0.0")); },
            "the staged kiln.mod.json says id \"m\", staged.json says other-id");
    refused("folder", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) { d["folder"] = "elsewhere"; });
    }, "folder is not m");
    refused("no-manifest", [](const fs::path& g) {
        Tree t = mod_files("m", "2.0.0");
        t.erase("kiln.mod.json");
        stage_mod(g, "m", "m", "2.0.0", t);
    }, "no kiln.mod.json");
    refused("format", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"), [](json& d) { d["format"] = 2; });
    }, "format is not 1");
    refused("garbage", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        put(staged(g) / "mods" / "m" / "staged.json", "{not json");
    }, "is not a JSON object");
#ifndef _WIN32
    refused("link-in-files", [](const fs::path& g) {
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
        fs::create_directory_symlink(g / "kiln", staged(g) / "mods" / "m" / "files" / "escape");
    }, "the staged files hold a link: escape");
    refused("link-file", [](const fs::path& g) {
        Tree t = mod_files("m", "2.0.0");
        put(g / "secret.txt", t["Mod.dll"]);
        stage_mod(g, "m", "m", "2.0.0", t);
        fs::remove(staged(g) / "mods" / "m" / "files" / "Mod.dll");
        fs::create_symlink(g / "secret.txt", staged(g) / "mods" / "m" / "files" / "Mod.dll");
    }, "the staged files hold a link: Mod.dll");
    refused("target-link", [](const fs::path& g) {
        fs::remove_all(g / "kiln" / "mods" / "m");
        write_tree(g / "outside" / "m", mod_files("m", "1.0.0"));
        fs::create_directory_symlink(g / "outside" / "m", g / "kiln" / "mods" / "m");
        stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    }, "kiln/mods/m is a link");
#endif
}

#ifndef _WIN32
static void test_link_on_the_way() {
    // kiln/mods itself a link to elsewhere: refused (the rename would land outside the server folder)
    const fs::path g = fresh("link-ancestor");
    write_tree(g / "elsewhere" / "m", mod_files("m", "1.0.0"));
    fs::create_directory_symlink(g / "elsewhere", g / "kiln" / "mods");
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    Log log;
    const Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(s.failed == 1 && log.has("leads through a link"), log.all());
    CHECK(tree(g / "elsewhere" / "m") == mod_files("m", "1.0.0"));
    fs::remove_all(g);
}

static void test_rename_failure_puts_back() {
    const fs::path g = fresh("rename-fails");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    // the staged files cannot move: their folder is read-only. The current version has gone to its backup by then.
    chmod((staged(g) / "mods" / "m").c_str(), 0555);
    Log log;
    Summary s = boot(Kind::Mods, g, &log);
    chmod((staged(g) / "mods" / "m").c_str(), 0755);
    CHECK_MSG(s.failed == 1 && log.has("failed and nothing changed") && log.has("(attempt 1 of 3)"), log.all());
    CHECK(mod_view(g) == mod_files("m", "1.0.0"));
    CHECK(!there(staged(g) / "tx"));
    CHECK(there(staged(g) / "mods" / "m" / "staged.json"));  // kept for the next start
    Log l2;
    s = boot(Kind::Mods, g, &l2);
    CHECK_MSG(s.applied == 1, l2.all());
    CHECK(mod_view(g) == mod_files("m", "2.0.0"));
    CHECK(!there(staged(g) / "attempts"));
    fs::remove_all(g);
}
#endif

static void test_kiln_update() {
    const fs::path g = fresh("kiln-update");
    setup_kiln(g);
    Log log;
    Summary s = boot(Kind::Kiln, g, &log);
    CHECK_MSG(s.applied == 1 && s.failed == 0, log.all());
    CHECK_MSG(kiln_view(g) == kiln_new(), log.all());
    CHECK(read(g / "kiln" / "data" / "kiln-admin" / "admin-token") == "token");
    CHECK(log.has("Kiln update: kiln/services/b.dll is kept: it changed since Kiln installed it"));
    CHECK(log.has("Kiln: updated to 0.2.0-dev.30.10 (from 0.2.0-dev.30.9)"));
    const fs::path prev = staged(g) / "previous" / "kiln";
    CHECK((tree(prev / "files") == Tree{{"dinput8.dll", "proxy 1"}, {"kiln/kiln-core.dll", "core 1"}, {"kiln/services/a.dll", "a 1"},
                                        {"kiln/.install.json", kReceipt1}}));
    const json kept = read_json(prev / "kept.json");
    std::map<std::string, std::string> before;
    for (const auto& e : kept["entries"]) before[e.value("path", "")] = e.value("before", "");
    CHECK((before == std::map<std::string, std::string>{{"dinput8.dll", "present"}, {"kiln/kiln-core.dll", "present"},
                                                         {"kiln/host/new.dll", "absent"}, {"kiln/services/a.dll", "present"},
                                                         {"kiln/.install.json", "present"}}));
    // the receipt goes in last, dinput8.dll right before it
    const auto& entries = kept["entries"];
    CHECK(entries[entries.size() - 1].value("path", "") == "kiln/.install.json" && entries[entries.size() - 2].value("path", "") == "dinput8.dll");
    CHECK(read_json(staged(g) / "probation" / "kiln.json").value("state", "") == "booting");
    CHECK(!there(staged(g) / "kiln"));

    // confirmed (Kiln.Host after the mods loaded): stays
    CHECK(confirm_kiln(g));
    CHECK(!confirm_kiln(g));
    Log l2;
    s = boot(Kind::Kiln, g, &l2);
    CHECK(s.rolled_back == 0 && kiln_view(g) == kiln_new());

    // a requested rollback of Kiln: every file back, the added one gone, the removed one back
    put(staged(g) / "rollback" / "kiln.json", json{{"format", 1}, {"unit", "kiln"}}.dump());
    Log l3;
    s = boot(Kind::Kiln, g, &l3);
    CHECK_MSG(s.rolled_back == 1, l3.all());
    CHECK_MSG(kiln_view(g) == kiln_old(), l3.all());
    fs::remove_all(g);

    // not confirmed: the next start rolls Kiln back by itself, once
    const fs::path g2 = fresh("kiln-auto");
    setup_kiln(g2);
    Log l4;
    boot(Kind::Kiln, g2, &l4);
    CHECK(kiln_view(g2) == kiln_new());
    Log l5;
    s = boot(Kind::Kiln, g2, &l5);
    CHECK_MSG(s.rolled_back == 1 && kiln_view(g2) == kiln_old(), l5.all());
    CHECK(l5.has("did not get as far as loading the mods"));
    CHECK(!there(staged(g2) / "probation" / "kiln.json"));
    Log l6;
    s = boot(Kind::Kiln, g2, &l6);
    CHECK(s.rolled_back == 0 && kiln_view(g2) == kiln_old());  // never in a loop
    fs::remove_all(g2);

    // "applied" (the start that applied it ended before it loaded the new kiln-core): no verdict yet, this start boots it
    const fs::path g6 = fresh("kiln-applied");
    setup_kiln(g6);
    boot(Kind::Kiln, g6, nullptr);
    json p = read_json(staged(g6) / "probation" / "kiln.json");
    CHECK(p.value("state", "") == "booting" && p.value("pid", 0) == 4242 && !p.value("booted", "").empty());
    p["state"] = "applied";
    put(staged(g6) / "probation" / "kiln.json", p.dump());
    Log l10;
    s = boot(Kind::Kiln, g6, &l10);
    CHECK_MSG(s.rolled_back == 0 && kiln_view(g6) == kiln_new(), l10.all());
    CHECK(read_json(staged(g6) / "probation" / "kiln.json").value("state", "") == "booting");
    fs::remove_all(g6);

    // planned on another install (kilnctl updated Kiln since): refused, nothing changes
    const fs::path g3 = fresh("kiln-base");
    setup_kiln(g3);
    put(g3 / "kiln" / ".install.json", kReceipt1 + " ");
    Log l7;
    s = boot(Kind::Kiln, g3, &l7);
    CHECK_MSG(s.failed == 1 && l7.has("Kiln changed since this update was staged"), l7.all());
    CHECK(read(g3 / "dinput8.dll") == "proxy 1" && !there(staged(g3) / "kiln"));
    fs::remove_all(g3);

    // a path outside what a Kiln update may write
    const fs::path g4 = fresh("kiln-outside");
    setup_kiln(g4);
    json doc = read_json(staged(g4) / "kiln" / "staged.json");
    doc["files"].push_back({{"path", "enshrouded_server.exe"}, {"sha256", sha("exe")}, {"size", 3}});
    put(staged(g4) / "kiln" / "files" / "enshrouded_server.exe", "exe");
    put(staged(g4) / "kiln" / "staged.json", doc.dump());
    Log l8;
    s = boot(Kind::Kiln, g4, &l8);
    CHECK_MSG(s.failed == 1 && l8.has("outside kiln/"), l8.all());
    CHECK(kiln_view(g4) == kiln_old());
    fs::remove_all(g4);

    // the server's own folders are refused too
    const fs::path g5 = fresh("kiln-data");
    setup_kiln(g5);
    doc = read_json(staged(g5) / "kiln" / "staged.json");
    doc["files"].push_back({{"path", "kiln/data/kiln-admin/admin-token"}, {"sha256", sha("mine")}, {"size", 4}});
    put(staged(g5) / "kiln" / "files" / "kiln" / "data" / "kiln-admin" / "admin-token", "mine");
    put(staged(g5) / "kiln" / "staged.json", doc.dump());
    Log l9;
    s = boot(Kind::Kiln, g5, &l9);
    CHECK_MSG(s.failed == 1 && l9.has("under kiln/data"), l9.all());
    CHECK(read(g5 / "kiln" / "data" / "kiln-admin" / "admin-token") == "token");
    fs::remove_all(g5);
}

// Plans, kept versions and journals that name kiln/data: each is refused and kiln/data stays byte for byte.
static void test_kiln_data_untouched() {
    auto expect_refused = [](const char* name, const std::function<void(const fs::path&)>& edit, const std::string& reason) {
        const fs::path g = fresh(std::string("data-") + name);
        setup_kiln(g);
        edit(g);
        Log log;
        const Summary s = boot(Kind::Kiln, g, &log);
        CHECK_MSG(s.failed == 1 && s.applied == 0 && log.has(reason), std::string(name) + ": " + log.all());
        check_data(g, name);
        fs::remove_all(g);
    };
    auto add_file = [](const fs::path& g, const std::string& path, const std::string& text) {
        json doc = read_json(staged(g) / "kiln" / "staged.json");
        doc["files"].push_back({{"path", path}, {"sha256", sha(text)}, {"size", text.size()}});
        put(staged(g) / "kiln" / "files" / path, text);
        put(staged(g) / "kiln" / "staged.json", doc.dump());
    };
    expect_refused("write", [&](const fs::path& g) { add_file(g, "kiln/data/kiln-network/server-id", "srv-hijacked\n"); }, "under kiln/data");
    expect_refused("write-case", [&](const fs::path& g) { add_file(g, "Kiln/Data/registry/entry-compat.json", "{}"); }, "under kiln/data");
    expect_refused("remove", [](const fs::path& g) {
        json doc = read_json(staged(g) / "kiln" / "staged.json");
        doc["remove"].push_back({{"path", "kiln/data/registry/entry-compat.json"}, {"sha256", sha(data_tree()["registry/entry-compat.json"])}});
        put(staged(g) / "kiln" / "staged.json", doc.dump());
    }, "under kiln/data");
    expect_refused("whole-folder", [&](const fs::path& g) { add_file(g, "kiln/data", "a file in its place"); }, "under kiln/data");
    // a kept version (previous/kiln) whose rollback would remove or replace kiln/data
    for (const char* before : {"absent", "present"}) {
        const fs::path g = fresh(std::string("data-kept-") + before);
        setup_kiln(g);
        boot(Kind::Kiln, g, nullptr);
        confirm_kiln(g);
        json kept = read_json(staged(g) / "previous" / "kiln" / "kept.json");
        kept["entries"].push_back({{"path", "kiln/data/kiln-network/server-id"}, {"before", before}});
        put(staged(g) / "previous" / "kiln" / "kept.json", kept.dump());
        put(staged(g) / "previous" / "kiln" / "files" / "kiln" / "data" / "kiln-network" / "server-id", "srv-old\n");
        put(staged(g) / "rollback" / "kiln.json", "{\"format\":1,\"unit\":\"kiln\"}");
        Log log;
        const Summary s = boot(Kind::Kiln, g, &log);
        CHECK_MSG(s.failed == 1 && s.rolled_back == 0 && log.has("under kiln/data"), log.all());
        CHECK(tree(g / "kiln" / "data") == data_tree());
        CHECK(kiln_view(g) == kiln_new());
        fs::remove_all(g);
    }
    // a hand-made journal that would move kiln/data away (and "put back" something else)
    const fs::path g = fresh("data-journal");
    write_tree(g, kiln_old());
    put_data(g);
    put(staged(g) / "tx" / "kiln" / "prev" / "files" / "kiln" / "data" / "kiln-network" / "server-id", "srv-other\n");
    const json j = {{"format", 1}, {"id", "x"}, {"unit", "kiln"}, {"op", "update"}, {"version", "2"}, {"from", "1"}, {"phase", "applying"},
                    {"steps", json::array({{{"target", "kiln/data/kiln-network/server-id"}, {"source", ""},
                                            {"backup", "kiln/staged/tx/kiln/prev/files/kiln/data/kiln-network/server-id"}, {"had", true}}})}};
    put(staged(g) / "tx" / "kiln" / "journal.json", j.dump());
    Log log;
    const Summary s = boot(Kind::Kiln, g, &log);
    CHECK_MSG(s.failed == 1 && log.has("not this unit's"), log.all());
    CHECK(tree(g / "kiln" / "data") == data_tree());
    fs::remove_all(g);
}

// kilnctl or the launcher changed Kiln after a staged change (a reinstall after a failed start, an update of their own):
// the kept version holds only the files that change changed, so putting it back now would mix two versions. Neither the
// automatic rollback nor a requested one does that; both are refused and say why, and nothing changes.
static void test_kiln_rollback_after_an_outside_change() {
    static const std::string kReceipt3 = R"({"format":1,"runtime":{"version":"0.2.0-dev.30.11"},"installer":"kilnctl","files":[]})";
    const Tree outside = {{"dinput8.dll", "proxy 3"},          {"kiln/kiln-core.dll", "core 3"}, {"kiln/host/new.dll", "new 3"},
                          {"kiln/services/b.dll", "b 1 edited"}, {"kiln/.install.json", kReceipt3}};
    const fs::path g = fresh("kiln-outside-change");
    setup_kiln(g);
    Log log;
    Summary s = boot(Kind::Kiln, g, &log);
    CHECK_MSG(s.applied == 1 && kiln_view(g) == kiln_new(), log.all());
    // the kept version says which receipt the change left in place
    CHECK(read_json(staged(g) / "previous" / "kiln" / "kept.json").value("receipt", "") == sha(kReceipt2));
    // the start crashed; the admin reinstalled Kiln with kilnctl before the next start
    for (const auto& [rel, text] : outside) put(g / rel, text);
    CHECK(kiln_view(g) == outside);
    Log l2;
    s = boot(Kind::Kiln, g, &l2);
    CHECK_MSG(s.rolled_back == 0 && s.failed == 1, l2.all());
    CHECK_MSG(kiln_view(g) == outside, "the automatic rollback mixed two versions: " + l2.all());
    CHECK_MSG(l2.has("cannot be rolled back") && l2.has("Kiln changed since"), l2.all());
    CHECK(!there(staged(g) / "probation" / "kiln.json"));  // once: never again for this update
    CHECK(!results(g).empty() && results(g).back().value("op", "") == "auto-rollback" && !results(g).back().value("ok", true));
    check_data(g, "outside change, automatic");
    // a rollback asked for by hand: refused the same way
    put(staged(g) / "rollback" / "kiln.json", "{}");
    Log l3;
    s = boot(Kind::Kiln, g, &l3);
    CHECK_MSG(s.rolled_back == 0 && s.failed == 1 && l3.has("the requested rollback is refused") && l3.has("Kiln changed since"), l3.all());
    CHECK_MSG(kiln_view(g) == outside, "the requested rollback mixed two versions: " + l3.all());
    CHECK(!there(staged(g) / "rollback" / "kiln.json"));
    // the kept version is still there for a later look; a receipt that is the change's own again allows the rollback
    put(g / "kiln" / ".install.json", kReceipt2);
    put(staged(g) / "rollback" / "kiln.json", "{}");
    Log l4;
    s = boot(Kind::Kiln, g, &l4);
    CHECK_MSG(s.rolled_back == 1 && read(g / "kiln" / ".install.json") == kReceipt1, l4.all());
    // a rollback's kept version names the receipt it put back
    CHECK(read_json(staged(g) / "previous" / "kiln" / "kept.json").value("receipt", "x") == sha(kReceipt1));
    fs::remove_all(g);

    // a server pack install (no receipt): its first update writes one; the rollback removes it again, and back
    const fs::path g2 = fresh("kiln-pack-rollback");
    setup_kiln(g2);
    fs::remove(g2 / "kiln" / ".install.json");
    json doc = read_json(staged(g2) / "kiln" / "staged.json");
    doc["base"]["receipt"] = nullptr;
    put(staged(g2) / "kiln" / "staged.json", doc.dump());
    Log l5;
    s = boot(Kind::Kiln, g2, &l5);
    CHECK_MSG(s.applied == 1 && read(g2 / "kiln" / ".install.json") == kReceipt2, l5.all());
    CHECK(read_json(staged(g2) / "previous" / "kiln" / "kept.json").value("receipt", "x") == sha(kReceipt2));
    put(staged(g2) / "rollback" / "kiln.json", "{}");
    Log l6;
    s = boot(Kind::Kiln, g2, &l6);
    CHECK_MSG(s.rolled_back == 1 && !there(g2 / "kiln" / ".install.json") && read(g2 / "dinput8.dll") == "proxy 1", l6.all());
    CHECK(read_json(staged(g2) / "previous" / "kiln" / "kept.json").value("receipt", "x").empty());
    put(staged(g2) / "rollback" / "kiln.json", "{}");
    Log l7;
    s = boot(Kind::Kiln, g2, &l7);
    CHECK_MSG(s.rolled_back == 1 && read(g2 / "kiln" / ".install.json") == kReceipt2 && read(g2 / "dinput8.dll") == "proxy 2", l7.all());
    check_data(g2, "server pack rollback");
    fs::remove_all(g2);
}

#ifdef _WIN32
// The 8.3 short name of an existing folder (CRASH-~1 for crash-reports), or "" when the volume makes none.
static std::string short_name_of(const fs::path& p) {
    wchar_t buf[4096];
    const DWORD n = GetShortPathNameW(p.c_str(), buf, 4096);
    if (n == 0 || n >= 4096) return {};
    const std::string s = fs::path(buf).filename().string();
    return _stricmp(s.c_str(), p.filename().string().c_str()) == 0 ? std::string() : s;
}
// Windows reaches a folder by its 8.3 short name too: a rule checked on names (kiln/crash-reports, a folder Kiln's receipt
// lists) must not be passed by that other name of the same folder.
static void test_short_names() {
    const fs::path g = fresh("short-names");
    write_tree(g / "kiln" / "mods" / "kiln-admin-console", mod_files("kiln-admin", "1.0.0"));
    const std::string alias = short_name_of(g / "kiln" / "mods" / "kiln-admin-console");
    if (alias.empty()) {
        std::printf("  short names: none on this volume, not checked\n");
        fs::remove_all(g);
        return;
    }
    std::printf("  short names: kiln-admin-console is also %s\n", alias.c_str());
    // a mod unit named by the short name of another folder
    stage_mod(g, alias, "kiln-admin", "2.0.0", mod_files("kiln-admin", "2.0.0"));
    Log log;
    Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(s.applied == 0 && s.failed == 1, log.all());
    CHECK_MSG(mod_view(g, "kiln-admin-console") == mod_files("kiln-admin", "1.0.0"), "replaced through its short name: " + log.all());
    CHECK_MSG(log.has("another name"), log.all());
    // a Kiln update that writes into kiln/crash-reports through its short name
    const fs::path g2 = fresh("short-names-kiln");
    setup_kiln(g2);
    put(g2 / "kiln" / "crash-reports" / "report.txt", "a report");
    const std::string cr = short_name_of(g2 / "kiln" / "crash-reports");
    CHECK(!cr.empty());
    json doc = read_json(staged(g2) / "kiln" / "staged.json");
    const std::string planted = "kiln/" + cr + "/planted.txt";
    doc["files"].push_back({{"path", planted}, {"sha256", sha("planted")}, {"size", 7}});
    put(staged(g2) / "kiln" / "files" / planted, "planted");
    put(staged(g2) / "kiln" / "staged.json", doc.dump());
    Log l2;
    s = boot(Kind::Kiln, g2, &l2);
    CHECK_MSG(s.applied == 0 && s.failed == 1 && l2.has("another name"), l2.all());
    CHECK_MSG(!there(g2 / "kiln" / "crash-reports" / "planted.txt"), "written into kiln/crash-reports through " + cr + ": " + l2.all());
    CHECK(kiln_view(g2).count("kiln/crash-reports/report.txt") == 1 && read(g2 / "dinput8.dll") == "proxy 1");
    fs::remove_all(g);
    fs::remove_all(g2);
}
#endif

// Free space: a start applies a staged update only when the volume of kiln/staged keeps 2 GiB plus the world's savegame
// folder free; otherwise the update waits, staged, with a line in kiln.log (never dropped for it). Rollbacks never wait.
static uint64_t g_fake_free = uint64_t{1} << 50;  // the test's free space (main installs the hook, children included)
static uint64_t fake_free(const fs::path&) { return g_fake_free; }
static void test_free_space() {
    constexpr uint64_t kGiB2 = uint64_t{2} << 30;
    const fs::path g = fresh("free-space");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    put(g / "enshrouded_server.json", R"({"name":"lab","saveDirectory":"./saves"})");
    put(g / "saves" / "world0.ksd", std::string(1000, 'w'));
    put(g / "saves" / "backup" / "world0.ksd", std::string(24, 'b'));
    put(g / "savegame" / "unused.ksd", std::string(5000, 'x'));  // not the save folder enshrouded_server.json names
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    g_fake_free = kGiB2 + 1023;  // one byte short of 2 GiB + 1024 bytes of saves
    for (int i = 0; i < kMaxAttempts + 1; ++i) {
        Log log;
        const Summary s = boot(Kind::Mods, g, &log);
        CHECK_MSG(s.held == 1 && s.applied == 0 && s.failed == 0, log.all());
        CHECK_MSG(log.has("mod folder m: the staged update to 2.0.0 waits for free space") && log.has("2147484671 bytes") &&
                      log.has("2147484672 bytes") && log.has("1024 bytes"),
                  log.all());
        CHECK(mod_view(g) == mod_files("m", "1.0.0") && there(staged(g) / "mods" / "m" / "staged.json"));
        CHECK(!there(staged(g) / "attempts") && !there(staged(g) / "tx") && results(g).empty());
    }
    g_fake_free = kGiB2 + 1024;  // exactly enough
    Log l2;
    Summary s = boot(Kind::Mods, g, &l2);
    CHECK_MSG(s.applied == 1 && s.held == 0 && mod_view(g) == mod_files("m", "2.0.0"), l2.all());
    // a rollback never waits: it brings back the version that worked
    g_fake_free = 0;
    put(staged(g) / "rollback" / "mod.m.json", "{}");
    Log l3;
    s = boot(Kind::Mods, g, &l3);
    CHECK_MSG(s.rolled_back == 1 && mod_view(g) == mod_files("m", "1.0.0"), l3.all());
    fs::remove_all(g);

    // a Kiln update waits the same way (the proxy's lines reach kiln.log); no enshrouded_server.json: <game>/savegame
    const fs::path g2 = fresh("free-space-kiln");
    setup_kiln(g2);
    put(g2 / "savegame" / "world0.ksd", std::string(4096, 'w'));
    auto with_save = [](Tree t) {
        t["savegame/world0.ksd"] = std::string(4096, 'w');
        return t;
    };
    g_fake_free = kGiB2 + 4095;
    Log l4;
    s = boot(Kind::Kiln, g2, &l4);
    CHECK_MSG(s.held == 1 && kiln_view(g2) == with_save(kiln_old()) && there(staged(g2) / "kiln" / "staged.json"), l4.all());
    CHECK_MSG(l4.has("Kiln: the staged update to 0.2.0-dev.30.10 waits for free space") && l4.has("4096 bytes"), l4.all());
    g_fake_free = uint64_t{1} << 50;
    Log l5;
    s = boot(Kind::Kiln, g2, &l5);
    CHECK_MSG(s.applied == 1 && kiln_view(g2) == with_save(kiln_new()), l5.all());
    fs::remove_all(g2);

    // the real free space of this volume (every runner keeps more than 2 GiB free)
    const fs::path g3 = fresh("free-space-real");
    write_tree(g3 / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g3, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    g_free_bytes_hook = nullptr;
    Log l6;
    s = boot(Kind::Mods, g3, &l6);
    g_free_bytes_hook = &fake_free;
    CHECK_MSG(s.applied == 1 && s.held == 0, l6.all());
    fs::remove_all(g3);
}

static void test_interrupted_three_times() {
    const fs::path g = fresh("thrice");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    for (int i = 0; i < 3; ++i) CHECK(run_child(Kind::Mods, g, 2) == 77);  // the 2nd change of each start
    Log log;
    const Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(log.has("was interrupted 3 times and is dropped"), log.all());
    CHECK(s.failed == 1 && s.applied == 0);
    CHECK(mod_view(g) == mod_files("m", "1.0.0"));
    CHECK(!there(staged(g) / "mods" / "m") && !there(staged(g) / "tx") && !there(staged(g) / "attempts"));
    const auto r = results(g);
    CHECK(!r.empty() && !r.back().value("ok", true) && r.back().value("error", "") == "interrupted 3 times");
    fs::remove_all(g);
}

static void test_bad_journals() {
    // damaged: the unit is left alone, with an error
    const fs::path g = fresh("journal-bad");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    put(staged(g) / "tx" / "mod.m" / "journal.json", "{\"format\":1,");
    Log log;
    Summary s = boot(Kind::Mods, g, &log);
    CHECK_MSG(s.failed == 1 && s.applied == 0 && log.has("cannot be used"), log.all());
    CHECK(mod_view(g) == mod_files("m", "1.0.0") && there(staged(g) / "mods" / "m" / "staged.json"));
    fs::remove_all(g);
    // a journal whose steps reach outside its unit is never followed
    const fs::path g2 = fresh("journal-foreign");
    write_tree(g2 / "kiln" / "mods" / "other", mod_files("other", "1.0.0"));
    const json j = {{"format", 1}, {"id", "x"}, {"unit", "mod.m"}, {"op", "update"}, {"version", "2"}, {"from", "1"},
                    {"phase", "applying"},
                    {"steps", json::array({{{"target", "kiln/mods/other"}, {"source", ""}, {"backup", "kiln/staged/tx/mod.m/prev/files"}, {"had", true}}})}};
    put(staged(g2) / "tx" / "mod.m" / "journal.json", j.dump());
    put(staged(g2) / "tx" / "mod.m" / "prev" / "files" / "kiln.mod.json", "{}");
    Log l2;
    s = boot(Kind::Mods, g2, &l2);
    CHECK_MSG(s.failed == 1 && l2.has("not this unit's"), l2.all());
    CHECK(mod_view(g2, "other") == mod_files("other", "1.0.0"));
    // a transaction folder without a journal ended before it changed anything: removed
    const fs::path g3 = fresh("journal-none");
    write_tree(g3 / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g3, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    put(staged(g3) / "tx" / "mod.m" / "prev" / "kept.json", "{}");
    Log l3;
    s = boot(Kind::Mods, g3, &l3);
    CHECK_MSG(s.applied == 1 && s.recovered == 0 && mod_view(g3) == mod_files("m", "2.0.0"), l3.all());
    fs::remove_all(g2);
    fs::remove_all(g3);
}

static void test_misc() {
    // a client applies nothing
    const fs::path g = fresh("client");
    write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
    stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
    Log log;
    const Env e = env_for(g, &log, false);
    Summary s = boot_mods(e);
    CHECK(s.applied == 0 && mod_view(g) == mod_files("m", "1.0.0") && log.has("not applied here"));
    // no kiln/staged: nothing at all
    const fs::path g2 = fresh("nothing");
    Log l2;
    s = boot(Kind::Mods, g2, &l2);
    CHECK(l2.lines.empty() && !there(staged(g2)));
    s = boot(Kind::Kiln, g2, &l2);
    CHECK(l2.lines.empty() && !there(staged(g2)));
    // stale staging folders of Kiln.Admin go; a staged folder with an unusable name is ignored
    fs::create_directories(staged(g2) / ".stage-abc" / "files");
    fs::create_directories(staged(g2) / "mods" / ".hidden");
    Log l3;
    boot(Kind::Mods, g2, &l3);
    CHECK(!there(staged(g2) / ".stage-abc") && there(staged(g2) / "mods" / ".hidden") && l3.has("ignored"));
    // the proxy's lines reach kiln-core once, and only this process's
    save_proxy_lines(g2, 7, {{Level::Warn, "staged: a"}, {Level::Info, "staged: b"}});
    CHECK(take_proxy_lines(g2, 8).empty());
    CHECK(!there(staged(g2) / "proxy-lines.json"));
    save_proxy_lines(g2, 7, {{Level::Warn, "staged: a"}, {Level::Info, "staged: b"}});
    const auto lines = take_proxy_lines(g2, 7);
    CHECK(lines.size() == 2 && lines[0].first == Level::Warn && lines[1].second == "staged: b");
    CHECK(take_proxy_lines(g2, 7).empty());
    CHECK(!kiln_update_waiting(g2));
    setup_kiln(g2);
    CHECK(kiln_update_waiting(g2));
    fs::remove_all(g);
    fs::remove_all(g2);
}

static void test_crash_points() {
    Scenario mod_update{"mod-update", Kind::Mods,
                        [](const fs::path& g) {
                            write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
                            stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
                        },
                        [](const fs::path& g) { return mod_view(g); }, mod_files("m", "1.0.0"), mod_files("m", "2.0.0"),
                        [](const fs::path& g) {
                            CHECK(tree(staged(g) / "previous" / "mods" / "m" / "files") == mod_files("m", "1.0.0"));
                            CHECK(read_json(staged(g) / "probation" / "mod.m.json").value("state", "") == "pending");
                            CHECK(!there(staged(g) / "mods" / "m"));
                        }};
    crash_everywhere(mod_update);
    Scenario mod_new{"mod-new", Kind::Mods,
                     [](const fs::path& g) { stage_mod(g, "m", "m", "1.0.0", mod_files("m", "1.0.0")); },
                     [](const fs::path& g) { return mod_view(g); }, Tree{}, mod_files("m", "1.0.0"), {}};
    crash_everywhere(mod_new);
    Scenario kiln_update{"kiln-update", Kind::Kiln, &setup_kiln, &kiln_view, kiln_old(), kiln_new(),
                         [](const fs::path& g) {
                             CHECK(read_json(staged(g) / "probation" / "kiln.json").value("state", "") == "booting");
                             CHECK(tree(staged(g) / "previous" / "kiln" / "files").size() == 4);
                             CHECK(read(g / "kiln" / "data" / "kiln-admin" / "admin-token") == "token");
                         }};
    crash_everywhere(kiln_update);
    // a requested rollback of a mod (the update applied first), and the automatic one of Kiln
    Scenario mod_rollback{"mod-rollback", Kind::Mods,
                          [](const fs::path& g) {
                              write_tree(g / "kiln" / "mods" / "m", mod_files("m", "1.0.0"));
                              stage_mod(g, "m", "m", "2.0.0", mod_files("m", "2.0.0"));
                              boot(Kind::Mods, g, nullptr);
                              put(staged(g) / "rollback" / "mod.m.json", "{\"format\":1,\"unit\":\"mod.m\"}");
                          },
                          [](const fs::path& g) { return mod_view(g); }, mod_files("m", "2.0.0"), mod_files("m", "1.0.0"),
                          [](const fs::path& g) {
                              CHECK(tree(staged(g) / "previous" / "mods" / "m" / "files") == mod_files("m", "2.0.0"));
                              CHECK(!there(staged(g) / "rollback" / "mod.m.json"));
                          }};
    crash_everywhere(mod_rollback);
    Scenario kiln_auto{"kiln-auto-rollback", Kind::Kiln,
                       [](const fs::path& g) {
                           setup_kiln(g);
                           boot(Kind::Kiln, g, nullptr);  // applied; the start "crashed" before it confirmed
                       },
                       &kiln_view, kiln_new(), kiln_old(),
                       [](const fs::path& g) { CHECK(!there(staged(g) / "probation" / "kiln.json")); }};
    crash_everywhere(kiln_auto);
}

int main(int argc, char** argv) {
    g_free_bytes_hook = &fake_free;  // hermetic: a test volume's free space never decides (test_free_space sets it)
#ifdef _WIN32
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_self = self;
    if (argc == 5 && std::strcmp(argv[1], "child") == 0)
        return child_main(std::strcmp(argv[2], "mods") == 0 ? Kind::Mods : Kind::Kiln, std::atoi(argv[3]), fs::path(argv[4]));
#else
    (void)argc;
    (void)argv;
#endif
    const char* dir = std::getenv("KILN_STAGED_SCRATCH");
    g_scratch = dir && *dir ? fs::path(dir) : fs::temp_directory_path() / "kiln-staged-test";
    fs::remove_all(g_scratch);
    fs::create_directories(g_scratch);

    test_paths();
    test_paths_fixture();
    test_mod_update_and_rollbacks();
    test_mod_loading_state_and_new_mod();
    test_disabled_carried_over();
    test_refusals();
#ifndef _WIN32
    test_link_on_the_way();
    test_rename_failure_puts_back();
#endif
    test_kiln_update();
    test_kiln_data_untouched();
    test_kiln_rollback_after_an_outside_change();
#ifdef _WIN32
    test_short_names();
#endif
    test_free_space();
    test_interrupted_three_times();
    test_bad_journals();
    test_misc();
    test_crash_points();

    fs::remove_all(g_scratch);
    if (failures) {
        std::fprintf(stderr, "staged_test: %d of %d check(s) failed\n", failures, checks);
        return 1;
    }
    std::printf("staged_test: all %d checks passed\n", checks);
    return 0;
}
