// SPDX-License-Identifier: MIT
// Host test of the ShroudForge mods the profile has off whose folder ShroudForge would still run (src/own_skip.h):
// kiln-shroudforge names them to the runtime it hosts, which skips them this start. A player's report (0.2.0-dev.31.9):
// a folder he put into ShroudForge's mods folder himself ran while the profile had the mod off. tests/test-own-skip.sh
// builds and runs it. Made-up mods; a temporary folder.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "own_skip.h"

namespace fs = std::filesystem;
static int failures = 0;

#define EXPECT(cond)                                                       \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

static void write(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

// a ShroudForge package: mod.json with `id`, and ShroudForge's switch when `ext` is given
static fs::path package(const fs::path& dir, const std::string& id, const char* ext) {
    write(dir / "mod.json", "{\"id\":\"" + id + "\",\"name\":\"N\",\"version\":\"1.0.0\"}");
    if (ext) write(dir / "extended.mod.json", ext);
    return dir;
}

static const char* ON = R"({"schemaVersion":1,"enabled":true,"targets":["client","server"]})";
static const char* OFF = R"({"schemaVersion":1,"enabled":false})";

int main() {
    const fs::path root = fs::temp_directory_path() / ("kiln-sf-own-skip-" + std::to_string(std::rand()));
    fs::remove_all(root);
    const fs::path mods = root / "mods", lib = root / "library", kiln = root / "kiln";
    // the Library's copies (the profile's paths)
    for (const char* id : {"climb-tweaks", "glide-boost", "shut-own", "stack-copy", "both-kinds", "client-only", "kept-on"})
        package(lib / id, id, nullptr);
    fs::create_directories(lib / "dll-only");
    write(lib / "dll-only/dll-only.dll", "MZ");
    // ShroudForge's mods folder: the player's own folders, one under another name, and Kiln's copy
    package(mods / "climb-tweaks", "climb-tweaks", ON);
    package(mods / "Glide Boost Manual", "glide-boost", ON);
    package(mods / "shut-own", "shut-own", OFF);
    package(mods / "stack-copy", "stack-copy", ON);
    write(mods / "stack-copy/.kiln-library.json", R"({"format":1,"kind":"shroudforge","id":"stack-copy","version":"1.0.0","hash":"h"})");
    package(mods / "both-kinds", "both-kinds", ON);
    package(mods / "client-only", "client-only", R"({"schemaVersion":1,"enabled":true,"targets":["client"]})");
    package(mods / "kept-on", "kept-on", ON);
    write(mods / "dll-only/dll-only.dll", "MZ");
    write(mods / "dll-only/native-plugin.ini", "[Plugin]\nEnabled=true\nDll=dll-only.dll\n");
    const auto entry = [&](const std::string& id, bool enabled) {
        return nlohmann::json{{"id", id}, {"path", (lib / id).string()}, {"enabled", enabled}};
    };
    const nlohmann::json profile = {
        {"mods", {{"shroudforge", {entry("climb-tweaks", false), entry("glide-boost", false), entry("shut-own", false),
                                   entry("stack-copy", false), entry("both-kinds", false), entry("client-only", false),
                                   entry("kept-on", true), entry("dll-only", false), entry("gone", false)}},
                  // the same package on as an EML entry: not off
                  {"eml", {entry("both-kinds", true)}}}}};

    const auto client = ksf::profile_off_folders(profile, mods, kiln, false);
    const auto find = [](const std::vector<ksf::OffFolder>& v, const std::string& id) -> const ksf::OffFolder* {
        for (const auto& f : v)
            if (f.id == id) return &f;
        return nullptr;
    };
    EXPECT(client.size() == 5);
    const auto* own = find(client, "climb-tweaks");
    EXPECT(own && own->package == "climb-tweaks" && own->folder == mods / "climb-tweaks" && !own->kilns);
    const auto* renamed = find(client, "glide-boost");
    EXPECT(renamed && renamed->package == "glide-boost" && renamed->folder == mods / "Glide Boost Manual");
    const auto* copy = find(client, "stack-copy");
    EXPECT(copy && copy->kilns);
    EXPECT(find(client, "client-only") != nullptr);
    const auto* dll = find(client, "dll-only");
    EXPECT(dll && dll->package == "dll-only" && dll->folder == mods / "dll-only");
    for (const char* id : {"shut-own", "both-kinds", "kept-on", "gone"}) EXPECT(find(client, id) == nullptr);
    // a dedicated server: the client-only package does not run there
    const auto server = ksf::profile_off_folders(profile, mods, kiln, true);
    EXPECT(server.size() == 4 && find(server, "client-only") == nullptr);
    // the lines
    EXPECT(ksf::off_folder_line(*own, ksf::Skip::skipped, "F", "M", false) ==
           "climb-tweaks is off in the profile: your own folder F is skipped this start; Kiln leaves its files as they are");
    const auto runs = ksf::off_folder_line(*own, ksf::Skip::no_export, "F", "M", false);
    EXPECT(runs.find("climb-tweaks is off in the profile, but your own folder F runs this start") == 0);
    EXPECT(runs.find("Mods > ShroudForge") != std::string::npos && runs.find("Let Kiln manage this folder") != std::string::npos &&
           runs.find("move the folder out of M") != std::string::npos);
    EXPECT(runs.find("takes no skip list from Kiln") != std::string::npos);
    EXPECT(ksf::off_folder_line(*own, ksf::Skip::no_export, "F", "M", true).find("\"enabled\": false") != std::string::npos);
    EXPECT(ksf::off_folder_line(*own, ksf::Skip::self_hosted, "F", "M", false).find("ShroudForge's own loader runs this start, so Kiln cannot have it skipped") != std::string::npos);
    EXPECT(ksf::off_folder_line(*copy, ksf::Skip::skipped, "F", "M", false).find("Kiln's copy F has ShroudForge's switch on and is skipped") != std::string::npos);
    // Review 0.2.0-dev.31.12: a folder without mod.json that the launcher placed as `mods/XHL-Auto-Loot` for the
    // Library id `xhl-auto-loot`. Windows (and Proton) open `mods/xhl-auto-loot` all the same, but ShroudForge's DLL
    // Manager matches the folder's own name exactly: the runtime must be given the name on disk.
    {
        const fs::path cased = root / "cased";
        fs::create_directories(lib / "xhl-auto-loot");
        write(lib / "xhl-auto-loot/xhl-auto-loot.dll", "MZ");
        write(cased / "XHL-Auto-Loot/XHL-Auto-Loot.dll", "MZ");
        write(cased / "XHL-Auto-Loot/native-plugin.ini", "[Plugin]\nEnabled=true\nDll=XHL-Auto-Loot.dll\n");
        const nlohmann::json p = {{"mods", {{"shroudforge", {entry("xhl-auto-loot", false)}}}}};
        const auto off = ksf::profile_off_folders(p, cased, kiln, false);
        EXPECT(off.size() == 1);
        EXPECT(!off.empty() && off[0].package == "XHL-Auto-Loot" && off[0].folder == cased / "XHL-Auto-Loot");
    }
    // Review 0.2.0-dev.31.12: one call per package. The runtime refuses a whole call for one name it does not take; one
    // such name must not keep the others running, and its line must not say the runtime takes no skip list.
    {
        std::vector<std::string> asked;
        const auto results = ksf::ask_each(client, true, [&](const std::string& package) {
            asked.push_back(package);
            return package != "glide-boost";
        });
        EXPECT(asked.size() == client.size());
        EXPECT(results.size() == client.size());
        for (size_t i = 0; i < client.size() && i < results.size(); ++i)
            EXPECT(results[i] == (client[i].package == "glide-boost" ? ksf::Skip::refused : ksf::Skip::skipped));
        const auto none = ksf::ask_each(client, true, {});
        for (const auto r : none) EXPECT(r == ksf::Skip::no_export);
        const auto self = ksf::ask_each(client, false, {});
        for (const auto r : self) EXPECT(r == ksf::Skip::self_hosted);
        const auto refused = ksf::off_folder_line(*renamed, ksf::Skip::refused, "F", "M", false);
        EXPECT(refused.find("glide-boost is off in the profile, but your own folder F runs this start") == 0);
        EXPECT(refused.find("takes no skip list") == std::string::npos);
        EXPECT(refused.find("did not take its name (glide-boost)") != std::string::npos);
    }
    // nothing in the mods folder changed
    std::ifstream ext(mods / "climb-tweaks/extended.mod.json");
    EXPECT(std::string((std::istreambuf_iterator<char>(ext)), std::istreambuf_iterator<char>()) == ON);
    // no profile lists, no mods folder: nothing
    EXPECT(ksf::profile_off_folders(nlohmann::json::object(), mods, kiln, false).empty());
    EXPECT(ksf::profile_off_folders(profile, root / "missing", kiln, false).empty());
    fs::remove_all(root);
    if (failures) return 1;
    std::printf("own_skip: all passed\n");
    return 0;
}
