// SPDX-License-Identifier: MIT
// kiln-dllmod's host (src/session.cpp) with test packages from tests/test_mod.cpp, one scenario per process (the
// packages stay loaded): tests/run_host.sh runs it under plain Wine, no game.
//   dllmod_host_test.exe <folder with the test DLLs> <scratch folder> <scenario>
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../src/session.h"

namespace fs = std::filesystem;
using namespace kdllmod;

static int g_failed = 0;
static std::vector<std::string> g_log;
#define CHECK(c)                                                       \
    do {                                                               \
        if (!(c)) {                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);   \
            ++g_failed;                                                \
        }                                                              \
    } while (0)

static fs::path g_bin, g_root;
static const std::string kWorld = "0123456789abcdef0123456789abcdef";
static const std::string kOther = "fedcba9876543210fedcba9876543210";

// Lays a package out as <root>/<folder>/<dll> with its manifest beside it, and returns its Mod (status waiting).
static Mod package(const std::string& folder, const std::string& built, const std::string& dll, const std::string& manifest) {
    const fs::path dir = g_root / folder;
    fs::create_directories(dir);
    fs::copy_file(g_bin / built, dir / dll, fs::copy_options::overwrite_existing);
    std::ofstream(dir / (dll + ".mod"), std::ios::binary) << manifest;
    Mod m;
    m.id = folder;
    m.name = folder;
    m.dll = dir / dll;
    m.root = g_root;
    m.package = package_id(folder + "/" + dll);
    m.origin = "profile";
    std::string e;
    if (!read_manifest(m.dll, &m.manifest, &e)) std::printf("manifest %s: %s\n", folder.c_str(), e.c_str());
    m.version = m.manifest.version;
    return m;
}

static const char* kWithSettings =
    "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=1.0.0\nabi=1\nsetting=b_mode|choice|Mode|slow|slow|fast\nsetting=a_on|bool|On|true\n";
static const char* kPlain = "ENSHROUDED_PACKAGE_V1\r\nkind=mod\r\nversion=0.1.29-dev\r\nabi=1\r\n";

static std::string report(const Mod& m) {
    HMODULE h = GetModuleHandleW(m.dll.filename().c_str());
    auto fn = h ? reinterpret_cast<const char* (*)()>(reinterpret_cast<void*>(GetProcAddress(h, "test_mod_report"))) : nullptr;
    return fn ? fn() : std::string("<not loaded>");
}

static void make(Session& s, std::vector<Mod> mods) {
    s.mods = std::move(mods);
    s.log = [](int level, const std::string& mod, const std::string& text) {
        const std::string line = std::to_string(level) + " [" + mod + "] " + text;
        std::printf("  log %s\n", line.c_str());
        g_log.push_back(line);
    };
    s.settings = [](const Mod& m, const std::string&, Values* v, std::string* from, std::string*) {
        *v = defaults(m.manifest);
        if (v->count("b_mode")) (*v)["b_mode"] = "fast";
        *from = "test";
        return true;
    };
}

static bool logged(const std::string& part) {
    for (const auto& l : g_log)
        if (l.find(part) != std::string::npos) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    g_bin = argv[1];
    g_root = fs::path(argv[2]) / "Mods" / "EnshroudedModLoader";
    const std::string scenario = argv[3];
    fs::remove_all(argv[2]);
    std::string why;

    if (scenario == "start") {
        // a mod with settings, one without, and a mod that needs a library package
        Session s;
        make(s, {package("carry", "dllmod_test_settings.dll", "carry.dll", kWithSettings),
                          package("lantern", "dllmod_test_plain.dll", "lantern.dll", kPlain),
                          package("user", "dllmod_test_uses_lib.dll", "user.dll",
                                  "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=2\nabi=1\nrequire=shared/dllmod_test_lib.dll@1.5\n")});
        package("shared", "dllmod_test_lib.dll", "dllmod_test_lib.dll", "ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1.5\nabi=1\n");
        CHECK(s.may_join(&why));
        CHECK(s.start_world(kWorld, &why));
        std::printf("  why: %s\n", why.c_str());
        CHECK(s.mods[0].status == "started" && s.mods[1].status == "started" && s.mods[2].status == "started");
        // Configure first with every value sorted by key, then Initialize with the 24-byte ABI 1 context, same world
        CHECK(report(s.mods[0]) == "C" + kWorld + "|a_on=true\nb_mode=fast\n;I24,1," + kWorld + ";");
        CHECK(report(s.mods[1]) == "I24,1," + kWorld + ";");
        CHECK(report(s.mods[2]) == "I24,1," + kWorld + ";");
        CHECK(s.mods[0].settings.at("b_mode") == "fast" && s.mods[0].settings_from == "test");
        // each mod's lines under its own id, line breaks trimmed
        CHECK(logged("3 [carry] test mod started") && !logged("started\r"));
        CHECK(logged("3 [user] the library answered 42"));
        CHECK(logged("[shared/dllmod_test_lib.dll] library shared/dllmod_test_lib.dll 1.5 loaded"));
        CHECK(s.started() == 3 && s.failed() == 0 && s.resident() && s.world() == kWorld);
        // the same world again: nothing runs twice
        CHECK(s.start_world(kWorld, &why));
        CHECK(report(s.mods[1]) == "I24,1," + kWorld + ";");
        // another world in the same game session: refused, the mods were made for the first
        CHECK(!s.start_world(kOther, &why) && why.find("another world") != std::string::npos);
        // joining another player's world while mods are loaded: refused
        CHECK(!s.may_join(&why) && why.find("Restart Enshrouded") != std::string::npos);
    } else if (scenario == "fail") {
        Session s;
        make(s, {package("bad", "dllmod_test_fail.dll", "bad.dll", kPlain)});
        CHECK(!s.start_world(kWorld, &why) && why.find("failed to start") != std::string::npos);
        CHECK(why.find("returned 0") != std::string::npos);
        CHECK(s.mods[0].status == "failed" && s.failed() == 1);
        CHECK(logged("3 [bad] test mod refuses to start"));
        // half started: every later start is refused until the game restarts
        CHECK(!s.start_world(kWorld, &why) && why.find("Restart Enshrouded") != std::string::npos);
        CHECK(!s.may_join(&why));
    } else if (scenario == "fault") {
        Session s;
        make(s, {package("crash", "dllmod_test_fault.dll", "crash.dll", kPlain)});
        CHECK(!s.start_world(kWorld, &why) && why.find("faulted (0xC0000005)") != std::string::npos);
        CHECK(s.mods[0].status == "failed");
    } else if (scenario == "rejected") {
        Mod good = package("good", "dllmod_test_plain.dll", "good.dll", kPlain);
        Mod bad = package("broken", "dllmod_test_plain.dll", "broken.dll", kPlain);
        bad.status = "rejected";
        bad.reason = "its manifest is invalid";
        Session s;
        make(s, {good, bad});
        // fail closed: one enabled mod that cannot run keeps the world (and every other mod) from starting
        CHECK(!s.start_world(kWorld, &why) && why.find("its manifest is invalid") != std::string::npos);
        CHECK(!s.resident() && report(s.mods[0]) == "<not loaded>");
        // switched off, it no longer holds the world back
        s.mods[1].enabled = false;
        CHECK(s.start_world(kWorld, &why) && s.mods[0].status == "started");
    } else if (scenario == "missing-lib") {
        Session s;
        make(s, {package("user", "dllmod_test_uses_lib.dll", "user.dll",
                                  "ENSHROUDED_PACKAGE_V1\nkind=mod\nversion=2\nabi=1\nrequire=shared/dllmod_test_lib.dll@1.5\n")});
        CHECK(!s.start_world(kWorld, &why) && why.find("not there") != std::string::npos);
        CHECK(!s.resident());
        package("shared", "dllmod_test_lib.dll", "dllmod_test_lib.dll", "ENSHROUDED_PACKAGE_V1\nkind=library\nversion=1.4\nabi=1\n");
        CHECK(!s.start_world(kWorld, &why) && why.find("version 1.5") != std::string::npos);
        CHECK(!s.resident());
    } else if (scenario == "already-loaded") {
        Mod m = package("dup", "dllmod_test_plain.dll", "dup.dll", kPlain);
        CHECK(LoadLibraryW(m.dll.c_str()) != nullptr);  // as if a loader, or a native mod, had loaded it before
        Session s;
        make(s, {m});
        CHECK(!s.start_world(kWorld, &why) && why.find("already loaded") != std::string::npos);
        CHECK(report(s.mods[0]) == "");  // never called
    } else if (scenario == "no-world") {
        Session s;
        make(s, {package("lantern", "dllmod_test_plain.dll", "lantern.dll", kPlain)});
        CHECK(!s.start_world("", &why) && why.find("world's id") != std::string::npos);
        CHECK(!s.resident());
        CHECK(s.start_world(kWorld, &why) && s.mods[0].status == "started");
    } else if (scenario == "settings-invalid") {
        Session s;
        make(s, {package("carry", "dllmod_test_settings.dll", "carry.dll", kWithSettings)});
        s.settings = [](const Mod&, const std::string&, Values*, std::string*, std::string* err) {
            *err = "its settings file x.ini is not valid (mode=1 is not a value this setting takes)";
            return false;
        };
        CHECK(!s.start_world(kWorld, &why) && why.find("x.ini is not valid") != std::string::npos);
        CHECK(!s.resident() && s.mods[0].status == "rejected");
    } else if (scenario == "no-export") {
        // a "mod" without EnshroudedModInitialize: refused before it is loaded
        Session s;
        make(s, {package("lib-as-mod", "dllmod_test_lib.dll", "libmod.dll", kPlain)});
        CHECK(!s.start_world(kWorld, &why) && why.find("does not export EnshroudedModInitialize") != std::string::npos);
        CHECK(!s.resident());
    } else if (scenario == "none") {
        Mod off = package("off", "dllmod_test_plain.dll", "off.dll", kPlain);
        off.enabled = false;
        Session s;
        make(s, {off});
        CHECK(s.start_world(kWorld, &why) && !s.resident() && s.may_join(&why));
        CHECK(report(s.mods[0]) == "<not loaded>");
    } else {
        std::printf("unknown scenario %s\n", scenario.c_str());
        return 2;
    }
    std::printf("RESULT %s %s\n", scenario.c_str(), g_failed ? "FAILED" : "passed");
    return g_failed ? 1 : 0;
}
