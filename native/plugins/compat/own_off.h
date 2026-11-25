// SPDX-License-Identifier: MIT
// A profile mod that is switched off, while the engine's own loader runs instead of Kiln's adapter (0.2.0-dev.31.12,
// docs/kiln/engines/README.md "Off in the profile, the player's own folder"). That loader reads only its own folder
// (<game>/emf/mods, <game>/greeneye, <game>/mods ...) and its own switch there, never Kiln's profile. Kiln never
// writes into, switches, replaces or removes a folder the player put there, so the player's own copy of a mod the
// profile has off still runs when that loader's own switch is on. Up to dev.31.10 kcompat::aside_report said
// "disabled" for every such entry without looking. off_entry() looks: it finds the copy that loader would run (the
// player's folder holding the same mod, or a Kiln copy of that entry still in place) and reads that loader's switch.
//
// Header only and free of Windows calls (std::filesystem, nlohmann::json), so tests/own_off_test.cpp checks it on the
// host (tests/test-own-off.sh). compat.h's aside_off() turns a Verdict into an AsideReport and plugs in Kiln's link
// checks (CheckedPath) and marker reader.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"

namespace kcompat::own_off {

namespace fs = std::filesystem;

inline std::string low(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
// UTF-8 in and out (profile.json, mod.json and kiln.log are UTF-8)
inline std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}
inline fs::path u8path(std::string_view s) { return fs::path(std::u8string(s.begin(), s.end())); }

inline bool read_text(const fs::path& file, std::string* out, size_t cap = 1024 * 1024) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return false;
    const auto n = fs::file_size(file, ec);
    if (ec || n > cap) return false;
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    out->assign(static_cast<size_t>(n), '\0');
    return static_cast<bool>(in.read(out->data(), static_cast<std::streamsize>(n))) || n == 0;
}

// A JSON file as its loader reads it: a UTF-8 BOM and comments allowed; nullopt when missing or not JSON.
inline std::optional<nlohmann::json> json_file(const fs::path& file) {
    std::string text;
    if (!read_text(file, &text)) return std::nullopt;
    if (text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);
    try {
        return nlohmann::json::parse(text, nullptr, true, true);
    } catch (...) {
        return std::nullopt;
    }
}

// What the own loader sees in one entry of its folder (a folder, or a file where the loader takes single files).
struct Seen {
    std::vector<std::string> keys;  // what identifies the mod, lower case (its manifest id; Shroudtopia: its DLLs)
    bool runs = true;               // the loader's own switch (and side, API ...) lets it run this start
    std::string why_not;            // !runs: why that loader keeps it off ("its mod.json says \"enabled\": false")
    std::string on_text;            // runs: what says so; default "<loader>'s own switch is on"
    std::string turn_off;           // how to turn it off in that loader (a clause: "set ... in ...")
    bool move_too = true;           // the reason adds "or move <folder> out of <game>\\<folder>" (turn_off says no move)
};

// One engine's own loader.
struct Loader {
    std::string name;         // "Greeneye Loader", "EMF", "Shroudtopia", "Flame"
    std::string kind;         // the engine's kind in Kiln's copy marker (.kiln-library.json "kind")
    fs::path folder;          // the folder it reads (<game>/greeneye ...)
    std::string folder_text;  // the same for sentences, below <game> ("greeneye", "emf\\mods")
    std::string own_text;     // the loader in sentences; default "<name>'s own loader"
    bool files = false;       // single files in the folder are mods too (EMF's <name>.dll, Shroudtopia's mods\*.dll)
    std::function<std::optional<Seen>(const fs::path& entry, bool dir)> read;  // nullopt: no mod of this loader
    // A folder entry the loader's folder may be read through: compat.h passes CheckedPath (no links, no aliases).
    std::function<bool(const fs::path& root, const fs::path& entry)> contained;
    // Kiln's copy marker in a folder (.kiln-library.json: kind, id); compat.h passes read_library_marker.
    std::function<bool(const fs::path& dir, std::string* kind, std::string* id)> marker;
};

// What a profile entry that is off does under that loader.
struct Verdict {
    bool found = false;  // a copy of it is in the loader's folder
    bool runs = false;   // and the loader runs it this start
    bool kilns = false;  // that copy is Kiln's (its marker names this entry), not the player's own
    fs::path where;      // the copy
    std::string reason;  // one sentence for the state file and kiln.log ("switched off in the profile" when !found)
};

inline bool default_contained(const fs::path&, const fs::path& entry) {
    std::error_code ec;
    return !fs::is_symlink(entry, ec);
}

inline bool default_marker(const fs::path& dir, std::string* kind, std::string* id) {
    const auto j = json_file(dir / ".kiln-library.json");
    if (!j || !j->is_object() || !j->contains("kind") || !(*j)["kind"].is_string() || !j->contains("id") ||
        !(*j)["id"].is_string())
        return false;
    *kind = (*j)["kind"].get<std::string>();
    *id = (*j)["id"].get<std::string>();
    return !kind->empty() && !id->empty();
}

namespace detail {

inline std::string own(const Loader& l) { return l.own_text.empty() ? l.name + "'s own loader" : l.own_text; }
inline std::string yours(const Verdict& v) {
    std::error_code ec;
    return fs::is_directory(v.where, ec) ? "your own folder " : "your own file ";
}

inline std::string reason_runs(const Loader& l, const Seen& s, const Verdict& v) {
    const std::string where = utf8(v.where);
    const std::string on = s.on_text.empty() ? l.name + "'s own switch is on" : s.on_text;
    if (v.kilns)
        return "runs (Kiln's copy " + where + " is still there; " + on + ") although it is off in the profile: " + own(l) +
               " runs this start and reads only <game>\\" + l.folder_text +
               ", not the profile. Activate the profile again in the Kiln launcher to take Kiln's copy out, or " + s.turn_off;
    return "runs (" + yours(v) + where + "; " + on + ") although it is off in the profile: " + own(l) +
           " runs this start and reads only <game>\\" + l.folder_text +
           ", not the profile, and Kiln never changes a folder you put there. To turn it off in " + l.name + ": " +
           s.turn_off + (s.move_too ? "; or move " + where + " out of <game>\\" + l.folder_text : std::string());
}

inline std::string reason_kept_off(const Loader& l, const Seen& s, const Verdict& v) {
    return std::string("switched off in the profile; ") + (v.kilns ? std::string("Kiln's copy ") : yours(v)) + utf8(v.where) +
           " is in <game>\\" + l.folder_text + " too, and " + own(l) + " keeps it off as well (" + s.why_not +
           ")";
}

}  // namespace detail

// The entry `profile_id` (its Library id) is off in the profile; `want` identifies its mod (the keys the loader's
// read() gives for the same mod: the manifest id, lower case). The first copy the loader would run wins (entries in
// name order); else the first copy it keeps off; else nothing is there and the entry is simply off.
inline Verdict off_entry(const Loader& l, std::string_view profile_id, const std::vector<std::string>& want) {
    Verdict none;
    none.reason = "switched off in the profile";
    std::error_code ec;
    if (!l.read || l.folder.empty() || !fs::is_directory(l.folder, ec)) return none;
    std::vector<fs::path> entries;
    for (const auto& d : fs::directory_iterator(l.folder, ec)) {
        const auto name = d.path().filename().native();
        if (name.empty() || name[0] == '.') continue;
        entries.push_back(d.path());
        if (entries.size() > 4096) break;
    }
    std::sort(entries.begin(), entries.end());
    const auto contained = l.contained ? l.contained : default_contained;
    const auto marker = l.marker ? l.marker : default_marker;
    const std::string pid = low(profile_id);
    std::optional<Verdict> kept;
    for (const auto& e : entries) {
        const bool dir = fs::is_directory(e, ec);
        if (!dir && !(l.files && fs::is_regular_file(e, ec))) continue;
        if (!contained(l.folder, e)) continue;  // a linked folder is refused before anything in it is read
        bool kilns = false;
        if (dir) {
            std::string kind, id;
            if (marker(e, &kind, &id) && kind == l.kind) {
                if (pid.empty() || low(id) != pid) continue;  // Kiln's copy of another entry
                kilns = true;
            }
        }
        const auto seen = l.read(e, dir);
        if (!seen) continue;
        bool same = kilns;
        for (const auto& k : seen->keys)
            if (!k.empty() && std::find(want.begin(), want.end(), k) != want.end()) same = true;
        if (!same) continue;
        Verdict v;
        v.found = true;
        v.kilns = kilns;
        v.where = e;
        v.runs = seen->runs;
        if (v.runs) {
            v.reason = detail::reason_runs(l, *seen, v);
            return v;
        }
        v.reason = detail::reason_kept_off(l, *seen, v);
        if (!kept) kept = v;
    }
    return kept ? *kept : none;
}

// ---- the loaders -------------------------------------------------------------------------------------------------

// EMF's own loader (docs/kiln/emf.md "When EMF itself is installed"): <game>/emf/mods/<folder>/ with mod.json, or a
// single <game>/emf/mods/<name>.dll (id: the file name, lower case). Its switch is enabled=0/1 in
// <game>/emf/config/<id>.ini; without a value there, mod.json's "enabled" (default on). A "side": "client" mod does
// nothing on a dedicated server.
inline Loader emf(const fs::path& game, bool server) {
    Loader l;
    l.name = "EMF";
    l.kind = "emf";
    l.folder = game / "emf" / "mods";
    l.folder_text = "emf\\mods";
    l.files = true;
    const fs::path config = game / "emf" / "config";
    l.read = [config, server](const fs::path& e, bool dir) -> std::optional<Seen> {
        Seen s;
        std::string id;
        bool manifest_on = true;
        std::string side;
        if (dir) {
            const auto j = json_file(e / "mod.json");
            if (!j || !j->is_object() || !j->contains("id") || !(*j)["id"].is_string()) return std::nullopt;
            id = (*j)["id"].get<std::string>();
            if (j->contains("enabled") && (*j)["enabled"].is_boolean()) manifest_on = (*j)["enabled"].get<bool>();
            if (j->contains("side") && (*j)["side"].is_string()) side = low((*j)["side"].get<std::string>());
        } else {
            if (low(utf8(e.extension())) != ".dll") return std::nullopt;
            id = low(utf8(e.stem()));
        }
        if (id.empty()) return std::nullopt;
        s.keys = {low(id)};
        const std::string ini_text = "emf\\config\\" + id + ".ini";
        s.turn_off = "set enabled=0 in <game>\\" + ini_text + " (EMF's own switch)";
        std::string ini, flat;
        int switch_value = -1;
        if (read_text(config / u8path(id + ".ini"), &ini, 256 * 1024)) {
            for (char c : ini)
                if (c != ' ' && c != '\t' && c != '\r') flat += c;
            flat = "\n" + flat;
            if (flat.find("\nenabled=0") != std::string::npos) switch_value = 0;
            else if (flat.find("\nenabled=1") != std::string::npos) switch_value = 1;
        }
        if (switch_value == 0) {
            s.runs = false;
            s.why_not = ini_text + " says enabled=0";
        } else if (switch_value < 0 && !manifest_on) {
            s.runs = false;
            s.why_not = "its mod.json says \"enabled\": false and " + ini_text + " sets no enabled=";
        } else if (server && side == "client") {
            s.runs = false;
            s.why_not = "a client mod (side \"client\"), which does nothing on a dedicated server";
        } else {
            s.on_text = switch_value == 1 ? "EMF's own switch is on: " + ini_text + " says enabled=1"
                                          : "EMF's own switch is on: no enabled=0 in " + ini_text;
        }
        return s;
    };
    return l;
}

// What identifies a Shroudtopia mod: its DLL files (Shroudtopia has no manifest id). Each DLL gives two keys, its
// bytes (size and FNV-1a hash: the very same build) and its file name (another build of the same mod).
inline std::vector<std::string> dll_keys_of_file(const fs::path& f) {
    std::vector<std::string> keys;
    std::string bytes;
    if (low(utf8(f.extension())) != ".dll" || !read_text(f, &bytes, 512u * 1024 * 1024)) return keys;
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) h = (h ^ c) * 1099511628211ull;
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    keys.push_back("bytes:" + std::to_string(bytes.size()) + ":" + hex);
    keys.push_back("name:" + low(utf8(f.filename())));
    return keys;
}

// A Shroudtopia profile entry (a DLL, or a folder with its DLLs) or an entry of <game>/mods.
inline std::vector<std::string> dll_keys(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return dll_keys_of_file(p);
    std::vector<fs::path> files;
    for (const auto& f : fs::directory_iterator(p, ec))
        if (f.is_regular_file(ec)) files.push_back(f.path());
    std::sort(files.begin(), files.end());
    std::vector<std::string> keys;
    for (const auto& f : files)
        for (auto& k : dll_keys_of_file(f)) keys.push_back(std::move(k));
    return keys;
}

// Shroudtopia's own loader (docs/kiln/engines/shroudtopia.md): every DLL in <game>/mods and in <game>/mods/<name>/
// (the 1.0 layout). It loads each mod's DLL and calls Load while its settings file <game>/shroudtopia.json has
// "active" on; the per-mod "active" under the mod's own name in "mods" only activates or deactivates it after that.
inline Loader shroudtopia(const fs::path& game) {
    Loader l;
    l.name = "Shroudtopia";
    l.kind = "shroudtopia";
    l.folder = game / "mods";
    l.folder_text = "mods";
    l.files = true;
    const fs::path settings = game / "shroudtopia.json";
    l.read = [settings](const fs::path& e, bool) -> std::optional<Seen> {
        Seen s;
        s.keys = dll_keys(e);
        if (s.keys.empty()) return std::nullopt;
        s.move_too = false;
        s.turn_off = "it has no switch that keeps a DLL in <game>\\mods from loading (\"active\" under the mod's name in "
                     "<game>\\shroudtopia.json \"mods\" only deactivates it after its DLL loaded), so move it out of "
                     "<game>\\mods";
        std::error_code ec;
        if (!fs::exists(settings, ec)) {
            s.on_text = "Shroudtopia's own switch is on: there is no <game>\\shroudtopia.json, so its loader starts with "
                        "its defaults";
            return s;
        }
        const auto j = json_file(settings);
        if (j && j->is_object() && j->contains("active") && (*j)["active"].is_boolean() && !(*j)["active"].get<bool>()) {
            s.runs = false;
            s.why_not = "<game>\\shroudtopia.json says \"active\": false, so its loader loads no mod";
            return s;
        }
        s.on_text = "Shroudtopia's own switch is on: <game>\\shroudtopia.json has \"active\" on";
        return s;
    };
    return l;
}

// Flame's own loader (docs/kiln/engines/flame.md): every <game>/mods/<folder>/ with a flame.json. It has no switch of
// its own: whatever is there starts.
inline Loader flame(const fs::path& game) {
    Loader l;
    l.name = "Flame";
    l.kind = "flame";
    l.folder = game / "mods";
    l.folder_text = "mods";
    l.read = [](const fs::path& e, bool dir) -> std::optional<Seen> {
        std::error_code ec;
        if (!dir || !fs::is_regular_file(e / "flame.json", ec)) return std::nullopt;
        Seen s;
        std::string id = utf8(e.filename());
        if (const auto j = json_file(e / "mod.json"); j && j->is_object() && j->contains("id") && (*j)["id"].is_string() &&
                                                       !(*j)["id"].get<std::string>().empty())
            id = (*j)["id"].get<std::string>();
        s.keys = {low(id)};
        s.on_text = "Flame's loader has no switch of its own and starts every folder with a flame.json in <game>\\mods";
        s.move_too = false;
        s.turn_off = "it has no switch, so move the folder out of <game>\\mods, or switch Flame's loader off (';' in front "
                     "of its line in kiln\\chain.ini)";
        return s;
    };
    return l;
}

}  // namespace kcompat::own_off
