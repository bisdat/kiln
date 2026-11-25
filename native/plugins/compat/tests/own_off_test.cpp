// SPDX-License-Identifier: MIT
// Host test of a profile mod that is off while the engine's own loader runs (own_off.h, the Greeneye loader in
// greeneye/src/own_off_greeneye.h): the player's own copy in that loader's folder runs when the loader's own switch is
// on, so Kiln's status must say it runs and how to turn it off there, and "disabled" only when nothing runs. Up to
// 0.2.0-dev.31.10 kcompat::aside_report answered "disabled" without looking. tests/test-own-off.sh builds and runs it.
// Made-up mods; a temporary folder.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "../own_off.h"
#include "../../greeneye/src/own_off_greeneye.h"

namespace fs = std::filesystem;
namespace oo = kcompat::own_off;
static int failures = 0, checks = 0;

#define EXPECT(cond)                                                    \
    do {                                                                \
        ++checks;                                                       \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

static void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}
static bool has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }
static void show(const char* what, const oo::Verdict& v) {
    std::printf("%-34s found=%d runs=%d kilns=%d: %s\n", what, v.found, v.runs, v.kilns, v.reason.c_str());
}

int main() {
    const fs::path game = fs::temp_directory_path() / ("kiln-own-off-" + std::to_string(std::rand()));
    fs::remove_all(game);

    // ---- EMF's own loader -------------------------------------------------------------------------------------
    {
        const fs::path mods = game / "emf" / "mods", config = game / "emf" / "config";
        write(mods / "Big Stacks by hand" / "mod.json", R"({"id":"big_stacks","version":"1.0.0"})");
        const auto l = oo::emf(game, false);
        auto v = oo::off_entry(l, "big-stacks", {"big_stacks"});
        show("emf: own folder, no ini", v);
        EXPECT(v.found && v.runs && !v.kilns);
        EXPECT(v.where == mods / "Big Stacks by hand");
        EXPECT(v.reason.rfind("runs (your own folder ", 0) == 0);
        // the words kiln-manage's support summary looks for (finding off-but-runs, dev31/sf-own-folder-off)
        EXPECT(has(v.reason, " although it is off in the profile: "));
        EXPECT(has(v.reason, "EMF's own switch is on"));
        EXPECT(has(v.reason, "set enabled=0 in <game>\\emf\\config\\big_stacks.ini"));
        EXPECT(!has(v.reason, "disabled"));

        write(config / "big_stacks.ini", "[big_stacks]\nenabled = 0\nmultiplier=4\n");
        v = oo::off_entry(l, "big-stacks", {"big_stacks"});
        show("emf: own folder, ini enabled=0", v);
        EXPECT(v.found && !v.runs);
        EXPECT(has(v.reason, "switched off in the profile; your own folder "));
        EXPECT(has(v.reason, "big_stacks.ini says enabled=0"));

        // mod.json "enabled": false, but the ini switches it on
        write(mods / "sleep" / "mod.json", R"({"id":"sleep","version":"1.0.0","enabled":false})");
        v = oo::off_entry(l, "sleep", {"sleep"});
        show("emf: mod.json off, no ini", v);
        EXPECT(v.found && !v.runs && has(v.reason, "\"enabled\": false"));
        write(config / "sleep.ini", "[sleep]\r\nenabled=1\r\n");
        v = oo::off_entry(l, "sleep", {"sleep"});
        show("emf: mod.json off, ini enabled=1", v);
        EXPECT(v.found && v.runs && has(v.reason, "sleep.ini says enabled=1"));

        // nothing of it there: simply off
        v = oo::off_entry(l, "herald", {"herald"});
        show("emf: nothing there", v);
        EXPECT(!v.found && !v.runs && v.reason == "switched off in the profile");

        // Kiln's copy of this entry left behind (the launcher did not run since the profile changed)
        write(mods / "clock" / "mod.json", R"({"id":"clock-manifest","version":"1.0.0"})");
        write(mods / "clock" / ".kiln-library.json", R"({"format":1,"kind":"emf","id":"clock","version":"1.0.0"})");
        v = oo::off_entry(l, "clock", {"something-else"});
        show("emf: Kiln's copy still there", v);
        EXPECT(v.found && v.runs && v.kilns && has(v.reason, "Kiln's copy ") && has(v.reason, "Activate the profile again"));
        // ... but Kiln's copy of another entry is not this one, even with the same manifest id
        v = oo::off_entry(l, "other-entry", {"clock-manifest"});
        show("emf: Kiln's copy of another entry", v);
        EXPECT(!v.found);

        // a single DLL in emf\mods: its id is the file name
        write(mods / "Teleports.dll", "MZ");
        v = oo::off_entry(l, "teleports", {"teleports"});
        show("emf: single DLL", v);
        EXPECT(v.found && v.runs && v.where == mods / "Teleports.dll");

        // a client mod on a dedicated server does nothing there
        write(mods / "hud" / "mod.json", R"({"id":"hud","version":"1.0.0","side":"client"})");
        EXPECT(oo::off_entry(oo::emf(game, false), "hud", {"hud"}).runs);
        v = oo::off_entry(oo::emf(game, true), "hud", {"hud"});
        show("emf: client mod on a server", v);
        EXPECT(v.found && !v.runs && has(v.reason, "dedicated server"));

        // a linked folder is never read
        std::error_code ec;
        write(game / "elsewhere" / "linked" / "mod.json", R"({"id":"linked","version":"1.0.0"})");
        fs::create_directory_symlink(game / "elsewhere" / "linked", mods / "linked", ec);
        if (!ec) EXPECT(!oo::off_entry(l, "linked", {"linked"}).found);
    }

    // ---- Greeneye Loader's own loader -------------------------------------------------------------------------
    {
        const fs::path gd = game / "greeneye";
        write(gd / "find-by-hand" / "mod.json", R"({ "id": "find", "version": "1.4", "dll": "Find.dll" })");
        write(gd / "find-by-hand" / "Find.dll", "MZ");
        const auto client = kgreeneye::greeneye_own_loader(game, false, 2);
        auto v = oo::off_entry(client, "find", {"find"});
        show("greeneye: own folder", v);
        EXPECT(v.found && v.runs && !v.kilns && v.where == gd / "find-by-hand");
        EXPECT(has(v.reason, "Greeneye Loader's own switch is on"));
        EXPECT(has(v.reason, "set \"enabled\": false in <game>\\greeneye\\find-by-hand\\mod.json"));
        EXPECT(has(v.reason, "or move "));

        write(gd / "quick-deposit" / "mod.json", R"({"id":"quick-deposit","enabled": false})");
        write(gd / "quick-deposit" / "quick-deposit.dll", "MZ");
        v = oo::off_entry(client, "quick-deposit", {"quick-deposit"});
        show("greeneye: enabled false", v);
        EXPECT(v.found && !v.runs && has(v.reason, "\"enabled\": false"));

        write(gd / "server-tool" / "mod.json", R"({"id":"server-tool","side":"server"})");
        write(gd / "server-tool" / "server-tool.dll", "MZ");
        EXPECT(!oo::off_entry(client, "server-tool", {"server-tool"}).runs);
        EXPECT(oo::off_entry(kgreeneye::greeneye_own_loader(game, true, 2), "server-tool", {"server-tool"}).runs);

        write(gd / "no-dll" / "mod.json", R"({"id":"no-dll"})");
        v = oo::off_entry(client, "no-dll", {"no-dll"});
        EXPECT(v.found && !v.runs && has(v.reason, "missing"));

        // a running copy wins over one Greeneye Loader keeps off (folders in name order: "a-off" first)
        write(gd / "a-off" / "mod.json", R"({"id":"portals","enabled":false})");
        write(gd / "a-off" / "portals.dll", "MZ");
        write(gd / "b-on" / "mod.json", R"({"id":"portals"})");
        write(gd / "b-on" / "portals.dll", "MZ");
        v = oo::off_entry(client, "town-portals", {"portals"});
        show("greeneye: one off, one on", v);
        EXPECT(v.found && v.runs && v.where == gd / "b-on");
    }

    // ---- Shroudtopia's own loader -----------------------------------------------------------------------------
    {
        const fs::path lib = game / "library" / "minimap", mods = game / "mods";
        write(lib / "Minimap.dll", "MZ minimap build 1");
        const auto want = oo::dll_keys(lib);
        EXPECT(want.size() == 2);
        const auto l = oo::shroudtopia(game);
        EXPECT(!oo::off_entry(l, "minimap", want).found);

        write(mods / "copy-of-minimap.dll", "MZ minimap build 1");  // the same bytes under another name
        auto v = oo::off_entry(l, "minimap", want);
        show("shroudtopia: same bytes", v);
        EXPECT(v.found && v.runs && v.where == mods / "copy-of-minimap.dll");
        EXPECT(has(v.reason, "there is no <game>\\shroudtopia.json"));
        EXPECT(has(v.reason, "move it out of <game>\\mods"));
        EXPECT(!has(v.reason, "; or move"));
        fs::remove(mods / "copy-of-minimap.dll");

        write(mods / "minimap" / "MINIMAP.dll", "MZ minimap build 2");  // another build, the 1.0 folder layout
        v = oo::off_entry(l, "minimap", want);
        show("shroudtopia: same name, folder", v);
        EXPECT(v.found && v.runs && v.where == mods / "minimap");

        write(game / "shroudtopia.json", R"({"active": true, "mods": {"Minimap": {"active": false}}})");
        v = oo::off_entry(l, "minimap", want);
        EXPECT(v.runs && has(v.reason, "has \"active\" on"));
        write(game / "shroudtopia.json", R"({"active": false})");
        v = oo::off_entry(l, "minimap", want);
        show("shroudtopia: active false", v);
        EXPECT(v.found && !v.runs && has(v.reason, "loads no mod"));

        write(mods / "unrelated.dll", "MZ other");
        EXPECT(!oo::off_entry(l, "x", oo::dll_keys(game / "library" / "nothing.dll")).found);
    }

    // ---- Flame's own loader -----------------------------------------------------------------------------------
    {
        const fs::path mods = game / "mods";
        write(mods / "Creative Mode" / "flame.json", R"({"dll":"creative.dll"})");
        write(mods / "Creative Mode" / "mod.json", R"({"id":"creative","version":"3.2.0"})");
        auto v = oo::off_entry(oo::flame(game), "creative", {"creative"});
        show("flame: own folder", v);
        EXPECT(v.found && v.runs && has(v.reason, "no switch of its own") && !has(v.reason, "; or move"));
        // an EML or Shroudtopia folder without flame.json is no Flame mod
        write(mods / "creative2" / "mod.json", R"({"id":"creative2"})");
        EXPECT(!oo::off_entry(oo::flame(game), "creative2", {"creative2"}).found);
    }

    fs::remove_all(game);
    std::printf("%d of %d checks failed\n", failures, checks);
    return failures ? 1 : 0;
}
