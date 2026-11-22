// SPDX-License-Identifier: MIT
// kiln.content: Kiln's base rules (since 0.2.0-dev.31; docs/kiln/services/build.md "Base rules and base limits",
// native/core/abi/services/kiln_base_rules.h). A mod states in Kiln's own names how many Flame Altars a world may have
// and how large an altar's build zone is; the game's names (keen::BalancingTable, altarsPerFlameLevel,
// buildzoneSizesPerAltarLevel) stay here. Included by gamedata_bootpatch.cpp only.
//
// One rule (blocks/*.json "baseRules": {"<name>": {...}}, or BaseRulesBlock staged in OnBoot):
//   {"minimumAltars": 1..16      at least this many Flame Altars at every Flame level
//    "extraAltars": 0..15        this many more than the game's number at every Flame level (the result at most 16)
//    "buildAreaPercent": 100..200   every altar level's build zone at this share of the game's, at most 256 m an axis
//    "sizes": [{"altarLevel": 1..5, "size": [x, y, z]}]   at least this full size (metres; each axis finite, 0.5..256,
//                                                         a multiple of 0.5) for an altar of that level}
// Every member may be left out (or null). A rule's result at a level never goes below the game's value.
//
// Composition (every mod's rules, any order): at each Flame level the count is the largest any rule asks for, each
// size axis at each altar level the largest. Since every rule is monotone in the game's value, the largest of the rules
// is the rule of the largest minimum, the largest extra, the largest percentage and the largest size per level and axis:
// compose() keeps exactly that. It is applied after every mod's own resource patches of the table, as a raise-only
// pass: each element ends at max(what the patches left, the composed rule of the game's own value). Base rules claim no
// field, so they never refuse another mod and no mod's patch refuses them.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace kc::baserules {

constexpr char kKind[] = "baseRules";          // the boot kind (blocks/*.json section, BootDefinitions)
constexpr char kPrefix[] = "baseRules@";       // a rule's document key in the patches collector: "baseRules@<name>"
constexpr char kTable[] = "keen::BalancingTable";
constexpr char kAltars[] = "altarsPerFlameLevel";        // BlobArray<u8>, one per Flame level
constexpr char kSizes[] = "buildzoneSizesPerAltarLevel"; // BlobArray<float3>, one per altar level (full sizes)
constexpr int kMaxAltars = 16;   // the game counts at most 16 (its getter clamps); above 16 is not a base rule
constexpr int kMaxExtra = 15, kMinPercent = 100, kMaxPercent = 200, kMaxLevel = 5;
constexpr double kMinSize = 0.5, kMaxSize = 256.0;  // Kiln's policy bounds (dossier chapter 18, D4)

struct Size {
    int level = 0;
    double xyz[3] = {};
};
struct Rule {
    std::string mod, name;
    int minimum = 0;  // 0: left out
    int extra = 0;
    int percent = 0;  // 0: left out
    std::vector<Size> sizes;
};

inline bool whole(const nlohmann::json& v, int lo, int hi, int* out) {
    if (!v.is_number()) return false;
    const double d = v.get<double>();
    if (!std::isfinite(d) || d != std::floor(d) || d < lo || d > hi) return false;
    *out = static_cast<int>(d);
    return true;
}

// One rule's document; false + *err (naming the member) when it is not one.
inline bool parse(const nlohmann::json& doc, Rule* r, std::string* err) {
    *r = Rule{r->mod, r->name, 0, 0, 0, {}};
    if (!doc.is_object())
        return *err = "a base rule is an object: {\"minimumAltars\": 1..16, \"extraAltars\": 0..15, \"buildAreaPercent\": 100..200, "
                      "\"sizes\": [{\"altarLevel\": 1..5, \"size\": [x, y, z]}]}",
               false;
    for (const auto& [k, v] : doc.items()) {
        if (k == "format" || v.is_null()) continue;
        if (k == "minimumAltars") {
            if (v.is_number() && std::isfinite(v.get<double>()) && v.get<double>() > kMaxAltars)
                return *err = "\"minimumAltars\": " + v.dump() + ": at most 16 Flame Altars (the game counts no more than 16; above 16 is not a base rule)", false;
            if (!whole(v, 1, kMaxAltars, &r->minimum)) return *err = "\"minimumAltars\" is a whole number from 1 to 16, not " + v.dump(), false;
        } else if (k == "extraAltars") {
            if (!whole(v, 0, kMaxExtra, &r->extra)) return *err = "\"extraAltars\" is a whole number from 0 to 15, not " + v.dump(), false;
        } else if (k == "buildAreaPercent") {
            if (!whole(v, kMinPercent, kMaxPercent, &r->percent))
                return *err = "\"buildAreaPercent\" is a whole number from 100 to 200, not " + v.dump(), false;
        } else if (k == "sizes") {
            if (!v.is_array()) return *err = "\"sizes\" is a list of {\"altarLevel\": 1..5, \"size\": [x, y, z]}", false;
            for (size_t i = 0; i < v.size(); ++i) {
                const nlohmann::json& e = v[i];
                const std::string at = "\"sizes\"[" + std::to_string(i) + "]";
                if (!e.is_object()) return *err = at + " is {\"altarLevel\": 1..5, \"size\": [x, y, z]}", false;
                for (const auto& [m, x] : e.items())
                    if (m != "altarLevel" && m != "size") return *err = at + ": unknown member \"" + m + "\" (altarLevel, size)", false;
                Size s;
                if (!e.contains("altarLevel") || !whole(e["altarLevel"], 1, kMaxLevel, &s.level))
                    return *err = at + ": \"altarLevel\" is a whole number from 1 to 5 (an altar level the game has, not 0)", false;
                for (const Size& o : r->sizes)
                    if (o.level == s.level) return *err = at + ": altar level " + std::to_string(s.level) + " is named twice", false;
                if (!e.contains("size") || !e["size"].is_array() || e["size"].size() != 3)
                    return *err = at + ": \"size\" is [x, y, z], the full size in metres", false;
                for (int a = 0; a < 3; ++a) {
                    const nlohmann::json& x = e["size"][a];
                    const double d = x.is_number() ? x.get<double>() : NAN;
                    if (!std::isfinite(d) || d < kMinSize || d > kMaxSize)
                        return *err = at + ": \"size\"[" + std::to_string(a) + "] is " + x.dump() + ": each axis is from 0.5 to 256 metres", false;
                    if (d * 2 != std::floor(d * 2))
                        return *err = at + ": \"size\"[" + std::to_string(a) + "] is " + x.dump() + ": each axis is a multiple of 0.5 metres", false;
                    s.xyz[a] = d;
                }
                r->sizes.push_back(s);
            }
        } else {
            return *err = "unknown member \"" + k + "\" (minimumAltars, extraAltars, buildAreaPercent, sizes)", false;
        }
    }
    return true;
}

struct Composed {
    int minimum = 0, extra = 0, percent = 0;
    double size[kMaxLevel + 1][3] = {};  // 0: none
    bool counts() const { return minimum > 1 || extra > 0; }
    bool sizes() const {
        if (percent > kMinPercent) return true;
        for (int l = 1; l <= kMaxLevel; ++l)
            for (double a : size[l])
                if (a > 0) return true;
        return false;
    }
    bool any() const { return counts() || sizes(); }
};

inline Composed compose(const std::vector<Rule>& rules) {
    Composed c;
    for (const Rule& r : rules) {
        c.minimum = std::max(c.minimum, r.minimum);
        c.extra = std::max(c.extra, r.extra);
        c.percent = std::max(c.percent, r.percent);
        for (const Size& s : r.sizes)
            for (int a = 0; a < 3; ++a) c.size[s.level][a] = std::max(c.size[s.level][a], s.xyz[a]);
    }
    return c;
}

// The composed rule's count for a Flame level whose game value is "game": never below the game's, never above 16 by a rule.
inline int altars(int game, const Composed& c) {
    if (!c.counts()) return game;
    return std::max(game, std::min(kMaxAltars, std::max(c.minimum, game + c.extra)));
}

// The composed rule's full size on one axis of an altar level whose game value is "game" (metres).
inline double size(double game, int level, int axis, const Composed& c) {
    double t = game;
    if (level < 1 || level > kMaxLevel) return t;
    if (c.percent > kMinPercent) t = std::max(t, std::min(kMaxSize, game * c.percent / 100.0));
    if (c.size[level][axis] > 0) t = std::max(t, c.size[level][axis]);
    return t;
}

inline nlohmann::json sizes_json(const std::vector<Size>& sizes) {
    nlohmann::json a = nlohmann::json::array();
    for (const Size& s : sizes) a.push_back({{"altarLevel", s.level}, {"size", {s.xyz[0], s.xyz[1], s.xyz[2]}}});
    return a;
}
inline nlohmann::json rule_json(const Rule& r) {
    nlohmann::json j = {{"mod", r.mod}, {"name", r.name}};
    if (r.minimum) j["minimumAltars"] = r.minimum;
    j["extraAltars"] = r.extra;
    if (r.percent) j["buildAreaPercent"] = r.percent;
    j["sizes"] = sizes_json(r.sizes);
    return j;
}
inline nlohmann::json composed_json(const Composed& c) {
    nlohmann::json j = {{"extraAltars", c.extra}};
    if (c.minimum) j["minimumAltars"] = c.minimum;
    if (c.percent) j["buildAreaPercent"] = c.percent;
    std::vector<Size> sizes;
    for (int l = 1; l <= kMaxLevel; ++l)
        if (c.size[l][0] > 0 || c.size[l][1] > 0 || c.size[l][2] > 0) sizes.push_back({l, {c.size[l][0], c.size[l][1], c.size[l][2]}});
    j["sizes"] = sizes_json(sizes);
    return j;
}

// ---- the game's table (kiln.content's reflection walk) ------------------------------------------------------------------

// The table's two lists as numbers: altars per Flame level, full sizes per altar level.
struct Lists {
    std::vector<double> altars;
    std::vector<std::array<double, 3>> sizes;
};

// The leaves of one list path below the table at "base" (resolved through the game's reflection).
inline bool leaves(const gd::TType* type, uintptr_t base, const std::string& path, std::vector<gd::Resolved>* out, std::vector<Field>* fields,
                   std::string* err) {
    out->clear();
    fields->clear();
    std::string e;
    if (!gd::resolve(gd::Root::Plain, gd::Node{type, base}, path, out, &e)) return *err = "\"" + path + "\": " + (e.empty() ? "no such member" : e), false;
    for (const gd::Resolved& r : *out) {
        Field f;
        if (!r.node.t || !gd::leaf_of(r.node.t, &f) || f.ref || (f.vt != KILN_CV_INT && f.vt != KILN_CV_FLOAT))
            return *err = "\"" + path + "\": " + r.path + " is not a number in this game build", false;
        fields->push_back(f);
    }
    return true;
}

inline double number(const Field& f, uintptr_t addr) {
    const gjson v = gd::leaf_json(f, reinterpret_cast<const uint8_t*>(addr));
    return v.is_number() ? v.get<double>() : 0;
}

// Reads the table at "base" (the game's bytes or a copy). false + *err: this game build's table is not shaped so.
inline bool read(const gd::TType* type, uintptr_t base, Lists* out, std::string* err) {
    std::vector<gd::Resolved> r;
    std::vector<Field> f;
    if (!leaves(type, base, std::string(kAltars) + "[*]", &r, &f, err)) return false;
    out->altars.clear();
    for (size_t i = 0; i < r.size(); ++i) out->altars.push_back(number(f[i], r[i].node.addr));
    out->sizes.clear();
    for (int a = 0; a < 3; ++a) {
        if (!leaves(type, base, std::string(kSizes) + "[*]." + "xyz"[a], &r, &f, err)) return false;
        if (a == 0) out->sizes.resize(r.size());
        if (r.size() != out->sizes.size()) return *err = std::string("\"") + kSizes + "\": its axes have different lengths", false;
        for (size_t i = 0; i < r.size(); ++i) out->sizes[i][a] = number(f[i], r[i].node.addr);
    }
    return true;
}

inline nlohmann::json lists_json(const Lists& l) {
    nlohmann::json j = {{"altars", nlohmann::json::array()}, {"sizes", nlohmann::json::array()}};
    for (double a : l.altars) j["altars"].push_back(static_cast<int64_t>(std::llround(a)));
    for (const auto& s : l.sizes) j["sizes"].push_back({s[0], s[1], s[2]});
    return j;
}

// Kiln's pass over a copy of the table, after every mod's patches: each element raised to the composed rule of the game's
// own value (game: the game's bytes), never lowered. *written: values changed. false + *err: the table cannot be read or
// written so in this game build (nothing of the pass is kept: the caller builds the copy again without it).
inline bool raise(const gd::TType* type, const uint8_t* game, std::vector<uint8_t>* copy, const Composed& c, size_t* written,
                  std::vector<std::string>* notes, std::string* err) {
    *written = 0;
    if (!c.any()) return true;
    Lists g;
    if (!read(type, reinterpret_cast<uintptr_t>(game), &g, err)) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(copy->data()), hi = lo + copy->size();
    size_t counts = 0, sizes = 0;
    // one leaf: raised to "want" when it is below
    auto put = [&](const gd::Resolved& r, const Field& f, double want, size_t* n) {
        if (r.node.addr < lo || r.node.addr + f.bytes > hi) return *err = r.path + " lies outside the table's own bytes", false;
        const double now = number(f, r.node.addr);
        if (!(want > now)) return true;
        uint8_t out[16] = {};
        bool reset = false;
        std::string e;
        const gjson v = f.vt == KILN_CV_INT ? gjson(static_cast<int64_t>(std::llround(want))) : gjson(want);
        if (!gd::parse_leaf(f, v, reinterpret_cast<const uint8_t*>(r.node.addr), out, &reset, &e)) return *err = r.path + ": " + e, false;
        std::memcpy(reinterpret_cast<void*>(r.node.addr), out, f.bytes);
        ++*n;
        return true;
    };
    std::vector<gd::Resolved> r;
    std::vector<Field> f;
    if (c.counts()) {
        if (!leaves(type, lo, std::string(kAltars) + "[*]", &r, &f, err)) return false;
        for (size_t i = 0; i < r.size(); ++i) {
            // a level the game's list has: its value; one another mod's patch added: the value as that patch left it
            const double base = i < g.altars.size() ? g.altars[i] : number(f[i], r[i].node.addr);
            if (!put(r[i], f[i], altars(static_cast<int>(std::llround(base)), c), &counts)) return false;
        }
    }
    if (c.sizes())
        for (int a = 0; a < 3; ++a) {
            if (!leaves(type, lo, std::string(kSizes) + "[*]." + "xyz"[a], &r, &f, err)) return false;
            for (size_t i = 0; i < r.size(); ++i) {
                const double base = i < g.sizes.size() ? g.sizes[i][a] : number(f[i], r[i].node.addr);
                if (!put(r[i], f[i], size(base, static_cast<int>(i), a, c), &sizes)) return false;
            }
        }
    *written = counts + sizes;
    notes->push_back("boot: base rules: " + std::string(kTable) + ": " + std::to_string(counts) + " Flame Altar count(s) and " + std::to_string(sizes) +
                     " build-zone size(s) raised by the composed rule " + composed_json(c).dump());
    return true;
}

}  // namespace kc::baserules
