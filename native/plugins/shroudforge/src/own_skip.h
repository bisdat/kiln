// SPDX-License-Identifier: MIT
// kiln-shroudforge: the ShroudForge mods the active profile has off whose folder ShroudForge would still run
// (0.2.0-dev.31.12, docs/kiln/engines.md "A player's own ShroudForge folder"). Kiln never writes into, switches,
// replaces or removes a folder the player put into ShroudForge's mods folder himself, so ShroudForge's own switch there
// (`enabled` in its extended.mod.json) can be on while the profile has the mod off. When Kiln hosts ShroudForge, it names
// those packages to the runtime (shroudforge_host_skip_packages, the fork's kiln/abi/host_skip.h) and ShroudForge
// skips them this start, their files untouched. EML entries are kiln-eml's (it names its own the same way).
//
// Header only and free of Windows calls, so tests/own_skip_test.cpp checks it on the host (test-own-skip.sh).
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"

namespace ksf {

// One profile entry the profile has off and whose folder ShroudForge would run.
struct OffFolder {
    std::string id;                 // the profile entry's id (the Library id)
    std::string package;            // what the runtime matches: the folder's mod.json id, or its folder name without one
    std::filesystem::path folder;   // the folder in ShroudForge's mods folder
    bool kilns = false;             // Kiln's copy (its .kiln-library.json names the entry), else the player's own folder
};

namespace own_skip_detail {

// UTF-8 in and out (profile.json and kiln.log are UTF-8; a plain std::string path would be the ANSI code page on Windows)
inline std::filesystem::path u8path(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }
inline std::string utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

inline std::optional<nlohmann::json> json_file(const std::filesystem::path& file) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec) || std::filesystem::file_size(file, ec) > 1024 * 1024) return std::nullopt;
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        return nlohmann::json::parse(in);
    } catch (...) {
        return std::nullopt;
    }
}

// mod.json's id in `dir`, "" without one
inline std::string mod_id(const std::filesystem::path& dir) {
    const auto j = json_file(dir / "mod.json");
    if (!j || !j->is_object()) return {};
    const auto it = j->find("id");
    return it != j->end() && it->is_string() ? it->get<std::string>() : std::string{};
}

inline bool same_name(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

// Does ShroudForge run the package folder `dir` on this side? A folder with mod.json: its extended.mod.json says
// enabled: true, and its targets name this side (none: an EML package runs on both, another on the client), as
// kiln-eml's discover::shroudforge_switch reads it. A folder without mod.json is the DLL Manager's: Kiln cannot tell
// from the folder alone, so it counts as running (skipping one that does not run changes nothing).
inline bool runs(const std::filesystem::path& dir, bool server) {
    std::error_code ec;
    if (!std::filesystem::exists(dir / "mod.json", ec)) return true;
    const auto j = json_file(dir / "extended.mod.json");
    if (!j || !j->is_object()) return false;
    const auto enabled = j->find("enabled");
    if (enabled == j->end() || !enabled->is_boolean() || !enabled->get<bool>()) return false;
    const auto launcher = j->find("launcher");
    const bool eml = launcher != j->end() && launcher->is_string() && launcher->get<std::string>() == "EML";
    const auto targets = j->find("targets");
    if (targets == j->end() || !targets->is_array()) return !server || eml;
    bool client = false, on_server = false;
    for (const auto& t : *targets) {
        if (!t.is_string()) continue;
        client |= t.get<std::string>() == "client";
        on_server |= t.get<std::string>() == "server";
    }
    return server ? on_server : (client || !on_server);
}

// Kiln's copy of the entry `id` (its marker; kiln-manage's receipt decides what Kiln may change, this only how to say it)
inline bool kilns_copy(const std::filesystem::path& dir, const std::string& id) {
    const auto j = json_file(dir / ".kiln-library.json");
    return j && j->is_object() && j->value("kind", std::string{}) == "shroudforge" && j->value("id", std::string{}) == id;
}

// The folder `<mods>/<name>` as the folder itself spells it: the exact name first, else the one whose name differs only
// in case (the one Windows opens). Empty when there is none.
inline std::filesystem::path on_disk(const std::filesystem::path& mods, const std::string& name) {
    std::error_code ec;
    std::filesystem::path other;
    for (auto it = std::filesystem::directory_iterator(mods, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto here = utf8(it->path().filename());
        if (here == name) return it->path();
        if (other.empty() && same_name(here, name)) other = it->path();
    }
    return other;
}

inline std::filesystem::path entry_path(const nlohmann::json& e, const std::filesystem::path& kiln) {
    if (!e.contains("path") || !e["path"].is_string()) return {};
    std::filesystem::path p = u8path(e["path"].get<std::string>());
    return p.is_absolute() ? p : kiln / p;
}

}  // namespace own_skip_detail

// The active profile's ShroudForge entries that are off (`mods.shroudforge`, enabled false) whose folder in `mods`
// (ShroudForge's mods folder) ShroudForge would run this start (`server`: a dedicated server). The folder that serves an
// entry is `<mods>/<id>`, or one whose mod.json holds the entry's package id under another name (as the launcher finds
// the player's own folder). Left out: a package that an enabled entry of the profile (ShroudForge or EML) or the EML
// runtime list (`emlRuntime`) also stands for. `kiln`: <game>/kiln, the base of relative profile paths.
inline std::vector<OffFolder> profile_off_folders(const nlohmann::json& profile, const std::filesystem::path& mods,
                                                  const std::filesystem::path& kiln, bool server) {
    using namespace own_skip_detail;
    std::vector<OffFolder> out;
    if (!profile.is_object()) return out;
    const auto lists = profile.find("mods");
    if (lists == profile.end() || !lists->is_object()) return out;
    const auto list_of = [&](const char* kind) {
        const auto it = lists->find(kind);
        return it != lists->end() && it->is_array() ? *it : nlohmann::json::array();
    };
    const auto sf = list_of("shroudforge");
    // what is on: the ids and package ids of enabled entries, and the EML runtime list
    std::vector<std::string> on;
    for (const char* kind : {"shroudforge", "eml"})
        for (const auto& e : list_of(kind)) {
            if (!e.is_object() || !e.value("enabled", true)) continue;
            if (e.contains("id") && e["id"].is_string()) on.push_back(e["id"].get<std::string>());
            if (const auto p = entry_path(e, kiln); !p.empty())
                if (const auto id = mod_id(p); !id.empty()) on.push_back(id);
        }
    if (const auto r = profile.find("emlRuntime"); r != profile.end() && r->is_array())
        for (const auto& id : *r)
            if (id.is_string()) on.push_back(id.get<std::string>());
    const auto is_on = [&](const std::string& id) { return std::find(on.begin(), on.end(), id) != on.end(); };
    std::error_code ec;
    if (!std::filesystem::is_directory(mods, ec)) return out;
    for (const auto& e : sf) {
        if (!e.is_object() || e.value("enabled", true) || !e.contains("id") || !e["id"].is_string()) continue;
        const std::string id = e["id"].get<std::string>();
        if (id.empty() || id.find_first_of("/\\:") != std::string::npos || id == "." || id == "..") continue;
        const auto library = entry_path(e, kiln);
        const std::string package = library.empty() ? std::string{} : mod_id(library);
        if (is_on(id) || (!package.empty() && is_on(package))) continue;
        // <mods>/<id> when it holds this package (or, for a package without mod.json, has none either), named as on disk:
        // Windows (and Proton) open `mods/xhl-auto-loot` for a folder `mods/XHL-Auto-Loot`, but ShroudForge's DLL Manager
        // matches a folder without mod.json by its own name, exactly
        std::filesystem::path folder;
        const auto named = on_disk(mods, id);
        if (!named.empty() && std::filesystem::is_directory(named, ec) && !std::filesystem::is_symlink(named, ec)) {
            const auto there = mod_id(named);
            if (package.empty() ? there.empty() || there == id : there == package) folder = named;
        }
        // or the player's folder of the same package under another name (sorted, the first one)
        if (folder.empty() && !package.empty()) {
            std::vector<std::filesystem::path> dirs;
            for (auto it = std::filesystem::directory_iterator(mods, ec); !ec && it != std::filesystem::directory_iterator();
                 it.increment(ec)) {
                const auto name = utf8(it->path().filename());
                if (name.empty() || name[0] == '.' || !it->is_directory(ec) || it->is_symlink(ec)) continue;
                if (mod_id(it->path()) == package) dirs.push_back(it->path());
            }
            std::sort(dirs.begin(), dirs.end());
            if (!dirs.empty()) folder = dirs.front();
        }
        if (folder.empty() || !runs(folder, server)) continue;
        const std::string there = mod_id(folder);
        out.push_back(OffFolder{id, there.empty() ? utf8(folder.filename()) : there, folder, kilns_copy(folder, id)});
    }
    return out;
}

// What became of one folder: the runtime skips it (`skipped`), it refused its name (`refused`: ShroudForge takes ids of
// letters, digits, '.', '-' and '_' and refuses a whole call for one it does not), the runtime Kiln hosts has no skip
// list (`no_export`), or ShroudForge's own loader runs this start (`self_hosted`).
enum class Skip { skipped, refused, no_export, self_hosted };

// Asks the runtime to skip each folder's package, one call per package (a refused name keeps only its own package
// running). `hosted`: Kiln hosts the runtime; `skip`: its export shroudforge_host_skip_packages, empty without one.
inline std::vector<Skip> ask_each(const std::vector<OffFolder>& folders, bool hosted, const std::function<bool(const std::string&)>& skip) {
    std::vector<Skip> out;
    for (const auto& f : folders)
        out.push_back(!hosted ? Skip::self_hosted : !skip ? Skip::no_export : skip(f.package) ? Skip::skipped : Skip::refused);
    return out;
}

// The kiln.log line of one folder: skipped, or it runs and why (`what`). `mods`: ShroudForge's mods folder. Kiln never
// writes into the player's own folder.
inline std::string off_folder_line(const OffFolder& f, Skip what, const std::string& folder, const std::string& mods, bool server) {
    if (what == Skip::skipped)
        return f.kilns ? f.id + " is off in the profile: Kiln's copy " + folder + " has ShroudForge's switch on and is skipped this "
                             "start; press Play in the Kiln launcher (or activate the profile again) and Kiln switches it off"
                       : f.id + " is off in the profile: your own folder " + folder + " is skipped this start; Kiln leaves its files "
                             "as they are";
    const std::string why = what == Skip::refused
        ? "the ShroudForge runtime did not take its name (" + f.package + ") into its skip list (it takes ids of letters, digits, "
          "'.', '-' and '_')"
        : what == Skip::no_export ? "this ShroudForge runtime takes no skip list from Kiln (Kiln's bundled runtime takes one from "
                                    "0.2.0-dev.31.12 on, an installed ShroudForge does not: launcher Settings, \"Use my installed "
                                    "ShroudForge\")"
                                  : "ShroudForge's own loader runs this start, so Kiln cannot have it skipped";
    if (f.kilns)
        return f.id + " is off in the profile, but Kiln's copy " + folder + " runs this start: ShroudForge's switch is on in it, and " +
               why + ". Press Play in the Kiln launcher (or activate the profile again): Kiln then switches it off";
    return f.id + " is off in the profile, but your own folder " + folder + " runs this start: Kiln never writes into it, "
           "ShroudForge's own switch for it is on, and " + why + ". To turn it off, " +
           std::string(server ? "put \"enabled\": false into its extended.mod.json (then restart the server)"
                              : "switch it off in Kiln's in-game menu (Mods > ShroudForge) or in ShroudForge's Modloader") +
           ", choose \"Let Kiln manage this folder\" in the Kiln launcher, or move the folder out of " + mods;
}

}  // namespace ksf
