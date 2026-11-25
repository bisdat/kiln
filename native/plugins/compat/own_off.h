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

// `s` with every byte that is not part of valid UTF-8, and every control character, as '?': a reason holds
// values from the player's files (Greeneye's lenient reader takes any bytes), and the state files' JSON writer
// (nlohmann dump) throws on invalid UTF-8.
inline std::string clean(std::string_view s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = n != 0 && i + n <= s.size() && !(n == 2 && c < 0xC2) && !(n == 4 && c > 0xF4);
        for (size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok && n == 3) {
            const auto c1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(c == 0xE0 && c1 < 0xA0) && !(c == 0xED && c1 >= 0xA0);  // overlong, surrogates
        }
        if (ok && n == 4) {
            const auto c1 = static_cast<unsigned char>(s[i + 1]);
            ok = !(c == 0xF0 && c1 < 0x90) && !(c == 0xF4 && c1 >= 0x90);
        }
        if (!ok || (n == 1 && c < 0x20)) {
            o += '?';
            ++i;
            continue;
        }
        o.append(s.substr(i, n));
        i += n;
    }
    return o;
}

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
    std::vector<std::string> keys;  // what identifies the mod, lower case (its manifest id; Shroudtopia: its DLLs' bytes)
    // what only hints at the mod (Shroudtopia: its DLLs' file names, which an unrelated DLL or another build can share):
    // a match on these alone never reads as "runs" (a false "runs" is worse than a false "disabled")
    std::vector<std::string> weak_keys;
    bool runs = true;               // the loader's own switch (and side, API ...) lets it run this start
    std::string why_not;            // !runs: why that loader keeps it off ("its mod.json says \"enabled\": false")
    bool unsure = false;            // !runs because Kiln cannot read that loader's switch (why_not says why), not "off"
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
    bool maybe = false;  // only a weak key matched (Shroudtopia: a DLL of the same file name, other bytes): not "runs"
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

inline std::string reason_maybe(const Loader& l, const Seen& s, const Verdict& v) {
    std::error_code ec;
    const bool dir = fs::is_directory(v.where, ec);
    return "switched off in the profile; " + yours(v) + utf8(v.where) + " in <game>\\" + l.folder_text +
           (dir ? " holds a DLL of the same file name as this mod's DLL" : " has the same file name as this mod's DLL") +
           ", but other bytes: if that is another build of this mod, " + own(l) +
           " loads it this start (it reads only <game>\\" + l.folder_text + ", not the profile), and to stop that: " +
           s.turn_off;
}

inline std::string reason_kept_off(const Loader& l, const Seen& s, const Verdict& v) {
    return std::string("switched off in the profile; ") + (v.kilns ? std::string("Kiln's copy ") : yours(v)) + utf8(v.where) +
           " is in <game>\\" + l.folder_text + " too, and " +
           (s.unsure ? "Kiln cannot tell whether " + own(l) + " runs it (" + s.why_not + "); to be sure it does not: " + s.turn_off
                     : own(l) + " keeps it off as well (" + s.why_not + ")");
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
    std::optional<Verdict> kept, maybe;
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
        auto any = [&](const std::vector<std::string>& keys) {
            for (const auto& k : keys)
                if (!k.empty() && std::find(want.begin(), want.end(), k) != want.end()) return true;
            return false;
        };
        const bool same = kilns || any(seen->keys);
        if (!same) {
            // only the weak keys match: maybe another build of this mod, maybe an unrelated DLL of the same name
            if (!maybe && seen->runs && any(seen->weak_keys)) {
                Verdict v;
                v.found = v.maybe = true;
                v.where = e;
                v.reason = clean(detail::reason_maybe(l, *seen, v));
                maybe = v;
            }
            continue;
        }
        Verdict v;
        v.found = true;
        v.kilns = kilns;
        v.where = e;
        v.runs = seen->runs;
        if (v.runs) {
            v.reason = clean(detail::reason_runs(l, *seen, v));
            return v;
        }
        v.reason = clean(detail::reason_kept_off(l, *seen, v));
        if (!kept) kept = v;
    }
    return kept ? *kept : maybe ? *maybe : none;
}

// EMF's own switch in <game>/emf/config/<id>.ini: every `enabled=` line (key in any case, around it spaces, before
// any section or in one; ';' and '#' start a comment line). 0: any of them is off (0, false, no, off: the spellings
// kiln-emf's settings take); 1: all of them on (1, true, yes, on); 2: a value that is neither; -1: none. `value` gets
// the value that decided. An off value anywhere wins, so a file EMF might read either way never reads as "runs".
inline int ini_switch(std::string_view text, std::string* value) {
    auto trim = [](std::string_view v) {
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t' || v.front() == '\r' || v.front() == '\0')) v.remove_prefix(1);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r' || v.back() == '\0')) v.remove_suffix(1);
        return v;
    };
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    int out = -1;
    for (size_t i = 0; i < text.size();) {
        size_t j = text.find('\n', i);
        if (j == std::string_view::npos) j = text.size();
        const std::string_view line = trim(text.substr(i, j - i));
        i = j + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || low(trim(line.substr(0, eq))) != "enabled") continue;
        const std::string v = low(trim(line.substr(eq + 1)));
        if (v == "0" || v == "false" || v == "no" || v == "off") {
            *value = v;
            return 0;
        }
        if (v == "1" || v == "true" || v == "yes" || v == "on") {
            if (out < 0) {
                out = 1;
                *value = v;
            }
        } else if (out != 2) {
            out = 2;
            *value = v.substr(0, 32);
        }
    }
    return out;
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
        std::string ini, value;
        int switch_value = -1;
        std::error_code ec;
        const fs::path ini_file = config / u8path(id + ".ini");
        if (read_text(ini_file, &ini, 256 * 1024)) switch_value = ini_switch(ini, &value);
        else if (fs::exists(ini_file, ec)) switch_value = 3;  // there, but over 256 KiB, not a file or unreadable
        if (switch_value == 0) {
            s.runs = false;
            s.why_not = ini_text + " says enabled=" + value;
        } else if (switch_value == 2 || switch_value == 3) {
            // Kiln cannot read EMF's switch: it does not claim the mod runs (a false "runs" is worse)
            s.runs = false;
            s.unsure = true;
            s.why_not = switch_value == 2 ? ini_text + " says enabled=" + value + ", which Kiln cannot read as on or off"
                                          : ini_text + " is there, but Kiln cannot read it (over 256 KiB or unreadable)";
        } else if (switch_value < 0 && !manifest_on) {
            s.runs = false;
            s.why_not = "its mod.json says \"enabled\": false and " + ini_text + " sets no enabled=";
        } else if (server && side == "client") {
            s.runs = false;
            s.why_not = "a client mod (side \"client\"), which does nothing on a dedicated server";
        } else {
            s.on_text = switch_value == 1 ? "EMF's own switch is on: " + ini_text + " says enabled=" + value
                                          : "EMF's own switch is on: no enabled=0 in " + ini_text;
        }
        return s;
    };
    return l;
}

// What identifies a Shroudtopia mod: its DLL files (Shroudtopia has no manifest id). Each DLL gives two keys, its
// bytes (size and FNV-1a hash: the very same build) and its file name (another build of the same mod).
// The file is hashed in 64 KiB blocks: it is never held in memory whole (a game process; a DLL up to 512 MiB counts).
inline std::vector<std::string> dll_keys_of_file(const fs::path& f) {
    std::vector<std::string> keys;
    std::error_code ec;
    if (low(utf8(f.extension())) != ".dll" || !fs::is_regular_file(f, ec)) return keys;
    const auto n = fs::file_size(f, ec);
    if (ec || n > 512ull * 1024 * 1024) return keys;
    std::ifstream in(f, std::ios::binary);
    if (!in) return keys;
    uint64_t h = 1469598103934665603ull, total = 0;
    std::vector<char> block(64 * 1024);
    while (in) {
        in.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto got = static_cast<size_t>(in.gcount());
        for (size_t i = 0; i < got; ++i) h = (h ^ static_cast<unsigned char>(block[i])) * 1099511628211ull;
        total += got;
        if (total > n) return {};  // it grew while read
    }
    if (total != n) return {};
    char hex[17];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    keys.push_back("bytes:" + std::to_string(n) + ":" + hex);
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
        for (auto& k : dll_keys(e)) (k.rfind("name:", 0) == 0 ? s.weak_keys : s.keys).push_back(std::move(k));
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
        if (!j || !j->is_object()) {
            // Shroudtopia keeps its last good settings (at start: its defaults, "active" on) on a file it cannot parse,
            // but Kiln reads the file with another parser and cannot be sure: no "runs" (a false "runs" is worse)
            s.runs = false;
            s.unsure = true;
            s.why_not = "<game>\\shroudtopia.json cannot be read as a JSON object: over 1 MiB, unreadable or not JSON";
            return s;
        }
        // Its loader loads mods only while "active" is true: a missing or non-boolean "active" reads as off (the same
        // rule as kiln-shroudtopia's port of that loop, json_bool("active", false)).
        if (!j->contains("active") || !(*j)["active"].is_boolean() || !(*j)["active"].get<bool>()) {
            s.runs = false;
            s.why_not = !j->contains("active") || !(*j)["active"].is_boolean()
                            ? "<game>\\shroudtopia.json has no \"active\": true, which its loader reads as off, so it loads no mod"
                            : "<game>\\shroudtopia.json says \"active\": false, so its loader loads no mod";
            return s;
        }
        s.on_text = "Shroudtopia's own switch is on: <game>\\shroudtopia.json says \"active\": true";
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
