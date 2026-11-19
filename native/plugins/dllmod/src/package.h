// SPDX-License-Identifier: MIT
// kiln-dllmod: the parts of hosting Enshrouded ModLoader packages that need no Windows API, so they are tested on the
// Linux host as well (tests/logic_test.cpp): the package manifest (ENSHROUDED_PACKAGE_V1), the settings a mod is given,
// the settings file format, package ids, world ids, the load order of required packages and SHA-256.
//
// Written for Kiln from the package format's public description (the loader's SDK text STANDARD_V1 and the shipped
// manifests of CarryOn and Belt Lantern): what a package file says and what a mod receives. No code of that loader is
// used (its repository has no licence). docs/kiln/engines/dllmod.md lists which facts come from where.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace kdllmod {

constexpr std::string_view kPackageHeader = "ENSHROUDED_PACKAGE_V1";
constexpr std::string_view kSettingsHeader = "ENSHROUDED_SETTINGS_V1";
constexpr size_t kManifestMax = 16 * 1024;  // a manifest is at most 16 KiB
constexpr size_t kSettingsMax = 16 * 1024;
constexpr size_t kMaxSettings = 16;
constexpr size_t kMaxRequires = 64;
constexpr size_t kMaxDepth = 64;
constexpr size_t kMaxModules = 1024;

enum class SettingType { Bool, Choice, Percent };

struct Setting {
    std::string key, label, def;
    SettingType type = SettingType::Bool;
    std::vector<std::string> choices;  // Choice
    int min = 0, max = 0, step = 1;    // Percent
};

struct Manifest {
    bool is_mod = false;  // kind=mod (else kind=library)
    std::string version;
    std::vector<Setting> settings;
    std::vector<std::pair<std::string, std::string>> requires_;       // (package id, exact version)
    std::map<std::string, std::map<std::string, std::string>> texts;  // language -> key -> label
};

using Values = std::map<std::string, std::string>;

namespace detail {

inline std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out.push_back(line);
        if (end == text.size()) break;
        at = end + 1;
    }
    return out;
}

inline std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t end = s.find(sep, at);
        out.emplace_back(s.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        if (end == std::string_view::npos) break;
        at = end + 1;
    }
    return out;
}

inline bool only(std::string_view s, std::string_view allowed) {
    for (char c : s)
        if (allowed.find(c) == std::string_view::npos) return false;
    return true;
}

// 0..10000, no sign, no leading zero
inline bool integer(std::string_view s, int* out) {
    if (s.empty() || s.size() > 5 || (s.size() > 1 && s[0] == '0')) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    if (v > 10000) return false;
    *out = v;
    return true;
}

// a setting field: at most 100 bytes, none of = tab CR LF NUL
inline bool field(std::string_view s) {
    if (s.size() > 100) return false;
    for (char c : s)
        if (c == '=' || c == '\t' || c == '\r' || c == '\n' || c == '\0') return false;
    return true;
}

// well-formed UTF-8 sequences (no overlong or surrogate check: the bytes are only compared and copied)
inline bool utf8(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t n = 0;
        if (c < 0x80) n = 0;
        else if ((c >> 5) == 6) n = 1;
        else if ((c >> 4) == 14) n = 2;
        else if ((c >> 3) == 30) n = 3;
        else return false;
        if (n > s.size() - i - 1) return false;
        for (size_t k = 1; k <= n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n + 1;
    }
    return true;
}

}  // namespace detail

// A package version: 1 to 64 of A-Z a-z 0-9 . _ -
inline bool valid_version(std::string_view v) {
    return !v.empty() && v.size() <= 64 &&
           detail::only(v, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
}

inline bool valid_setting_key(std::string_view k) {
    return !k.empty() && k.size() <= 40 && detail::only(k, "abcdefghijklmnopqrstuvwxyz0123456789_");
}

// A package id: the DLL's path below the package root, lower case, '/' between folders, ending in ".dll"; no empty,
// "." or ".." part, no part ending in '.' or ' ', none of < > " | ? * : tab CR LF NUL. Empty when the path is no such path.
inline std::string package_id(std::string_view rel) {
    std::string out;
    for (char c : rel) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        out.push_back(c);
    }
    if (out.empty() || out.size() > 1024 || out.front() == '/') return {};
    for (const auto& part : detail::split(out, '/')) {
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return {};
        for (char c : part)
            if (std::string_view("<>\"|?*:\t\r\n").find(c) != std::string_view::npos || c == '\0') return {};
    }
    if (out.size() < 5 || out.compare(out.size() - 4, 4, ".dll") != 0) return {};
    return out;
}

// Parses a .dll.mod manifest. False with a short reason when the package format refuses it: the first line must be
// the header; kind (mod or library), version and abi (1) once each; setting, text and require lines as the format
// gives them; any other key, a duplicate, or settings on a library is an error. A UTF-8 BOM and CRLF are fine.
inline bool parse_manifest(std::string_view text, Manifest* out, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    *out = Manifest{};
    if (text.size() > kManifestMax) return fail("the manifest is larger than 16 KiB");
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    if (!detail::utf8(text)) return fail("the manifest is not UTF-8");
    const auto lines = detail::lines(text);
    if (lines.empty() || lines[0] != kPackageHeader) return fail("the first line is not ENSHROUDED_PACKAGE_V1");
    bool kind = false, version = false, abi = false;
    for (size_t n = 1; n < lines.size(); ++n) {
        const std::string_view line = lines[n];
        if (line.empty()) continue;
        const std::string where = " (line " + std::to_string(n + 1) + ")";
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return fail("a line without '='" + where);
        const std::string_view k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "kind") {
            if (kind || (v != "mod" && v != "library")) return fail("kind must be given once, as mod or library" + where);
            kind = true;
            out->is_mod = v == "mod";
        } else if (k == "version") {
            if (version || !valid_version(v)) return fail("version must be given once (1-64 of A-Z a-z 0-9 . _ -)" + where);
            version = true;
            out->version = std::string(v);
        } else if (k == "abi") {
            if (abi || v != "1") return fail("abi must be given once, and Kiln knows abi 1 only" + where);
            abi = true;
        } else if (k == "setting") {
            if (out->settings.size() >= kMaxSettings) return fail("more than 16 settings" + where);
            const auto parts = detail::split(v, '|');
            if (parts.size() < 4 || !valid_setting_key(parts[0]) || parts[2].empty())
                return fail("a setting line is not key|type|label|default..." + where);
            for (const auto& p : parts)
                if (!detail::field(p)) return fail("a setting field is longer than 100 bytes or holds = tab or a line break" + where);
            for (const auto& s : out->settings)
                if (s.key == parts[0]) return fail("the setting " + parts[0] + " is given twice" + where);
            Setting s;
            s.key = parts[0];
            s.label = parts[2];
            s.def = parts[3];
            if (parts[1] == "bool" && parts.size() == 4 && (s.def == "true" || s.def == "false")) {
                s.type = SettingType::Bool;
            } else if (parts[1] == "choice" && parts.size() >= 6 && parts.size() <= 12) {
                s.type = SettingType::Choice;
                std::set<std::string> distinct;
                for (size_t i = 4; i < parts.size(); ++i)
                    if (parts[i].empty() || !distinct.insert(parts[i]).second)
                        return fail("the choices of " + s.key + " are empty or repeat" + where);
                s.choices.assign(parts.begin() + 4, parts.end());
                if (!distinct.count(s.def)) return fail("the default of " + s.key + " is not one of its choices" + where);
            } else if (parts[1] == "percent" && parts.size() == 7) {
                s.type = SettingType::Percent;
                int def = 0;
                if (!detail::integer(parts[3], &def) || !detail::integer(parts[4], &s.min) || !detail::integer(parts[5], &s.max) ||
                    !detail::integer(parts[6], &s.step) || s.step < 1 || s.max <= s.min || (s.max - s.min) % s.step != 0 ||
                    (s.max - s.min) / s.step > 31 || def < s.min || def > s.max || (def - s.min) % s.step != 0)
                    return fail("the percent setting " + s.key + " has a bad default, range or step" + where);
            } else {
                return fail("the setting " + s.key + " has an unknown type or the wrong number of fields" + where);
            }
            out->settings.push_back(std::move(s));
        } else if (k == "text") {
            const auto parts = detail::split(v, '|');
            if (parts.size() != 3 || parts[0].empty() || parts[0].size() > 16 ||
                !detail::only(parts[0], "abcdefghijklmnopqrstuvwxyz0123456789-") || parts[1].empty() || parts[1].size() > 100 ||
                !detail::only(parts[1], "abcdefghijklmnopqrstuvwxyz0123456789_.") || parts[2].empty() || !detail::field(parts[2]) ||
                !out->texts[parts[0]].emplace(parts[1], parts[2]).second)
                return fail("a text line is not language|key|label, or repeats" + where);
        } else if (k == "require") {
            if (out->requires_.size() >= kMaxRequires) return fail("more than 64 required packages" + where);
            const size_t at = v.rfind('@');
            if (at == std::string_view::npos) return fail("a require line is not <package>@<version>" + where);
            const std::string id = package_id(v.substr(0, at));
            const std::string_view ver = v.substr(at + 1);
            if (id.empty() || !valid_version(ver)) return fail("a require line names no package path or no version" + where);
            for (const auto& r : out->requires_)
                if (r.first == id) return fail("the package " + id + " is required twice" + where);
            out->requires_.emplace_back(id, std::string(ver));
        } else {
            return fail("unknown key \"" + std::string(k) + "\"" + where);
        }
    }
    if (!kind || !version || !abi) return fail("kind, version and abi are all needed");
    if (!out->is_mod && !out->settings.empty()) return fail("a library package has no settings");
    // a text names a setting or one of its choices: setting.<key>, choice.<key>.<index>
    for (const auto& [lang, entries] : out->texts)
        for (const auto& [key, label] : entries) {
            bool known = false;
            for (const auto& s : out->settings) {
                if (key == "setting." + s.key) known = true;
                for (size_t i = 0; i < s.choices.size(); ++i)
                    if (key == "choice." + s.key + "." + std::to_string(i)) known = true;
            }
            if (!known) return fail("the text " + key + " (" + lang + ") names no setting");
        }
    return true;
}

inline bool valid_value(const Setting& s, std::string_view v) {
    switch (s.type) {
    case SettingType::Bool: return v == "true" || v == "false";
    case SettingType::Choice:
        for (const auto& c : s.choices)
            if (c == v) return true;
        return false;
    case SettingType::Percent: {
        int n = 0;
        return detail::integer(v, &n) && n >= s.min && n <= s.max && (n - s.min) % s.step == 0;
    }
    }
    return false;
}

inline Values defaults(const Manifest& m) {
    Values v;
    for (const auto& s : m.settings) v.emplace(s.key, s.def);
    return v;
}

// What Configure receives: every value, defaults included, as key=value lines sorted by key, each ended by LF.
inline std::string settings_text(const Values& v) {
    std::string out;
    for (const auto& [k, val] : v) out += k + "=" + val + "\n";
    return out;
}

// A settings file: ENSHROUDED_SETTINGS_V1, then key=value lines (a UTF-8 BOM and CRLF are fine, blank lines skipped).
// Starts from the manifest's defaults; a key the manifest does not have, a key given twice or a value its setting does
// not take makes the whole file invalid (false with the reason).
inline bool parse_settings(std::string_view text, const Manifest& m, Values* out, std::string* error) {
    auto fail = [&](std::string why) {
        if (error) *error = std::move(why);
        return false;
    };
    if (text.size() > kSettingsMax) return fail("larger than 16 KiB");
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    const auto lines = detail::lines(text);
    if (lines.empty() || lines[0] != kSettingsHeader) return fail("the first line is not ENSHROUDED_SETTINGS_V1");
    Values v = defaults(m);
    std::set<std::string> seen;
    for (size_t n = 1; n < lines.size(); ++n) {
        const std::string_view line = lines[n];
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        const std::string key(line.substr(0, eq));
        const Setting* s = nullptr;
        for (const auto& x : m.settings)
            if (x.key == key) s = &x;
        if (eq == std::string_view::npos || !s) return fail("line " + std::to_string(n + 1) + ": no setting named \"" + key + "\"");
        if (!seen.insert(key).second) return fail(key + " is given twice");
        const std::string_view value = line.substr(eq + 1);
        if (!valid_value(*s, value)) return fail(key + "=" + std::string(value) + " is not a value this setting takes");
        v[key] = std::string(value);
    }
    *out = std::move(v);
    return true;
}

// The 32 lower-case hex digits a mod is given as the world's id; empty when all 16 bytes are zero (no world).
inline std::string world_id_hex(const uint8_t bytes[16]) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    bool any = false;
    for (int i = 0; i < 16; ++i) {
        out.push_back(hex[bytes[i] >> 4]);
        out.push_back(hex[bytes[i] & 15]);
        any |= bytes[i] != 0;
    }
    return any ? out : std::string();
}

inline bool valid_world_id(std::string_view id) {
    return id.size() == 32 && detail::only(id, "0123456789abcdef") && id != std::string(32, '0');
}

// ---- load order ----
// One package as the plan sees it: its id, its manifest, or why it cannot be read.
struct PlanItem {
    std::string id;
    Manifest manifest;
    std::string error;  // why the package cannot be read (missing, bad manifest)
};

// The packages to load for the wanted kind=mod package ids, each after the packages it requires, every one once.
// lookup(id, item) reads a package. Refuses (false with the reason): a package that cannot be read, a version that is
// not the exact one required, a cycle, a required kind=mod package that is not wanted itself, a library asked for on
// its own, depth over 64 or more than 1024 packages.
inline bool make_plan(const std::vector<std::string>& wanted, const std::function<bool(const std::string&, PlanItem*)>& lookup,
                      std::vector<PlanItem>* ordered, std::string* error) {
    ordered->clear();
    std::set<std::string> want(wanted.begin(), wanted.end()), visiting, done;
    std::map<std::string, PlanItem> seen;
    std::function<bool(const std::string&, size_t)> visit = [&](const std::string& id, size_t depth) -> bool {
        if (done.count(id)) return true;
        if (depth > kMaxDepth || visiting.count(id) || seen.size() >= kMaxModules) {
            *error = "the packages " + id + " requires form a cycle, or go deeper than 64 or past 1024 packages";
            return false;
        }
        PlanItem item;
        if (!lookup(id, &item) || !item.error.empty()) {
            *error = id + ": " + (item.error.empty() ? std::string("cannot be read") : item.error);
            return false;
        }
        if (item.manifest.is_mod && !want.count(id)) {
            *error = id + " is a mod another package requires: switch it on as well";
            return false;
        }
        if (!item.manifest.is_mod && depth == 0) {
            *error = id + " is a library: it loads only when a mod requires it";
            return false;
        }
        visiting.insert(id);
        seen[id] = item;
        for (const auto& [dep, ver] : item.manifest.requires_) {
            if (!visit(dep, depth + 1)) return false;
            if (seen[dep].manifest.version != ver) {
                *error = id + " requires " + dep + " version " + ver + ", but that package is version " + seen[dep].manifest.version;
                return false;
            }
        }
        visiting.erase(id);
        done.insert(id);
        ordered->push_back(seen[id]);
        return true;
    };
    for (const auto& id : wanted)
        if (!visit(id, 0)) return false;
    return true;
}

// ---- SHA-256 (the loader's per-world settings file is named by the hash of the package id) ----
inline std::string sha256_hex(std::string_view data) {
    static const uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
        0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
        0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg(data);
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(msg[off + i * 4])) << 24) | (uint32_t(uint8_t(msg[off + i * 4 + 1])) << 16) |
                   (uint32_t(uint8_t(msg[off + i * 4 + 2])) << 8) | uint32_t(uint8_t(msg[off + i * 4 + 3]));
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint32_t v : h)
        for (int i = 28; i >= 0; i -= 4) out.push_back(hex[(v >> i) & 15]);
    return out;
}

// ---- the game build this engine knows ----
// Enshrouded client build 26430625 (enshrouded.exe SHA-256 5d9ef2f1...0c77): the one build the mods for this loader are
// made for. The two functions Kiln hooks, their first bytes as the exe on disk has them (checked before hooking), and
// where a world slot keeps its id. Facts of the game's code; where they come from: docs/kiln/engines/dllmod.md.
struct GameBinding {
    const char* sha256;
    uint32_t start_local_rva;  // starts a local world (single player or hosting): fn(world slot*)
    std::array<uint8_t, 16> start_local_bytes;
    uint32_t start_session_rva;  // starts a session: fn(request*); joining another machine's world has 0 in its first field
    std::array<uint8_t, 14> start_session_bytes;
    uint32_t world_valid;  // byte: the slot holds a world
    uint32_t world_guid;   // 16 bytes: the world's id
};

inline const GameBinding kClient26430625{
    "5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77",
    0xd8ed80,
    {0x48, 0x89, 0x5c, 0x24, 0x18, 0x56, 0x48, 0x83, 0xec, 0x20, 0x4c, 0x8b, 0x0d, 0x17, 0x07, 0xab},
    0xd8efb0,
    {0x40, 0x56, 0x48, 0x83, 0xec, 0x30, 0x48, 0x8b, 0xf1, 0x48, 0x8d, 0x54, 0x24, 0x20},
    0xe0,
    0x14,
};

inline const GameBinding* binding_for(std::string_view sha256) {
    std::string low(sha256);
    for (char& c : low)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return low == kClient26430625.sha256 ? &kClient26430625 : nullptr;
}

}  // namespace kdllmod
