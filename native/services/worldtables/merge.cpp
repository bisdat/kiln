// SPDX-License-Identifier: MIT
// See merge.h. The resources are the game's blob format: a struct at the start, BlobArray {int32 offset from the field itself,
// uint32 count} pointing further in. A table only grows by appending a new copy of its array to the end and pointing the
// header at it; what the old array's records point at stays where it is, so inner offsets are recomputed for the new
// address. The old array stays behind as dead bytes.
#include "merge.h"

#include "surfacecheck.h"
#include "terrain.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace kwt {

namespace {

template <class T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}
template <class T>
void wr(uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof v);
}

void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    Bytes m(data, data + len);
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<uint8_t>(bits >> (i * 8)));
    auto rol = [](uint32_t x, int n) { return x << n | x >> (32 - n); };
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[80];
        for (int t = 0; t < 16; ++t)
            w[t] = uint32_t{m[o + t * 4]} << 24 | uint32_t{m[o + t * 4 + 1]} << 16 | uint32_t{m[o + t * 4 + 2]} << 8 | uint32_t{m[o + t * 4 + 3]};
        for (int t = 16; t < 80; ++t) w[t] = rol(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int t = 0; t < 80; ++t) {
            uint32_t f, k;
            if (t < 20) f = (b & c) | (~b & d), k = 0x5A827999u;
            else if (t < 40) f = b ^ c ^ d, k = 0x6ED9EBA1u;
            else if (t < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDCu;
            else f = b ^ c ^ d, k = 0xCA62C1D6u;
            const uint32_t x = rol(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = x;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<uint8_t>(h[i] >> (24 - j * 8));
}

void pad4(Bytes* b) {
    while (b->size() % 4) b->push_back(0);
}

int enum_index(const std::string& v, const char* const* names, int count) {
    for (int i = 0; i < count; ++i)
        if (v == names[i]) return i;
    return -1;
}

std::string join_names(const char* const* names, int count) {
    std::string s;
    for (int i = 0; i < count; ++i) s += (i ? ", " : "") + std::string(names[i]);
    return s;
}

bool valid_name(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s)
        if (!std::isalnum(c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

}  // namespace

bool Guid::zero() const {
    for (uint8_t x : b)
        if (x) return false;
    return true;
}

// .NET order: the first three groups are little-endian.
std::string Guid::text() const {
    char s[37];
    std::snprintf(s, sizeof s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12],
                  b[13], b[14], b[15]);
    return s;
}

bool Guid::parse(const std::string& s, Guid* out) {
    if (s.size() != 36 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    static constexpr int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int at = 0;
    Guid g;
    for (int i = 0; i < 16; ++i) {
        if (s[at] == '-') ++at;
        auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
        const int hi = hex(s[at]), lo = hex(s[at + 1]);
        if (hi < 0 || lo < 0) return false;
        g.b[order[i]] = static_cast<uint8_t>(hi << 4 | lo);
        at += 2;
    }
    *out = g;
    return true;
}

uint32_t fnv1a(const void* data, size_t n) {
    uint32_t h = 0x811c9dc5u;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x01000193u;
    return h;
}

Guid derive_guid(const std::string& mod, const std::string& kind, const std::string& name) {
    static constexpr uint8_t ns[16] = {0x6f, 0x6d, 0x2a, 0x44, 0x53, 0xc1, 0x5c, 0x52, 0x9d, 0x51, 0x0a, 0x3c, 0x6e, 0x1f, 0x2b, 0x77};  // big-endian
    const std::string text = mod + ":" + kind + ":" + name;
    Bytes all(ns, ns + 16);
    all.insert(all.end(), text.begin(), text.end());
    uint8_t h[20];
    sha1(all.data(), all.size(), h);
    h[6] = static_cast<uint8_t>((h[6] & 0x0f) | 0x50);
    h[8] = static_cast<uint8_t>((h[8] & 0x3f) | 0x80);
    Guid g;
    static constexpr int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; ++i) g.b[order[i]] = h[i];
    return g;
}

uint32_t item_id_of(const Guid& g) { return fnv1a(g.b.data(), 16); }

ojson parse_relaxed(const std::string& text) {
    // Match BlockJson: JSON comments, trailing commas and at most 64 nested containers. Strip comments first so a comma can see a closing token
    // across them, while braces and quotes in comments never affect strings or the depth guard. Whitespace keeps tokens from joining together.
    std::string clean = text;
    bool quoted = false, escaped = false;
    unsigned depth = 0;
    for (size_t i = 0; i < clean.size(); ++i) {
        const char c = clean[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
            continue;
        }
        if (c == '"') { quoted = true; continue; }
        if (c == '/' && i + 1 < clean.size() && (clean[i + 1] == '/' || clean[i + 1] == '*')) {
            const bool line = clean[i + 1] == '/';
            clean[i++] = ' ';
            clean[i] = ' ';
            bool closed = line;
            while (++i < clean.size()) {
                if (line && (clean[i] == '\n' || clean[i] == '\r')) break;
                if (!line && clean[i] == '*' && i + 1 < clean.size() && clean[i + 1] == '/') {
                    clean[i++] = ' ';
                    clean[i] = ' ';
                    closed = true;
                    break;
                }
                if (clean[i] != '\n' && clean[i] != '\r') clean[i] = ' ';
            }
            if (!closed) return ojson(ojson::value_t::discarded);
            continue;
        }
        if (c == '[' || c == '{') {
            if (++depth > 64) return ojson(ojson::value_t::discarded);
        } else if ((c == ']' || c == '}') && depth) --depth;
    }
    quoted = escaped = false;
    char previous = 0;
    for (size_t i = 0; i < clean.size(); ++i) {
        const char c = clean[i];
        if (quoted) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == ',') {
            const size_t next = clean.find_first_not_of(" \t\r\n", i + 1);
            // A real value must precede it: never turn [,], {,} or a double comma into valid JSON.
            const bool value = previous == '"' || previous == ']' || previous == '}' || (previous >= '0' && previous <= '9') || previous == 'e' || previous == 'l';
            if (value && next != std::string::npos && (clean[next] == ']' || clean[next] == '}')) clean[i] = ' ';
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') previous = c;
    }
    return ojson::parse(clean, nullptr, false);
}

// ---- blocks ------------------------------------------------------------------------------------------------------
void read_strength(const ojson& j, const std::string& where, const std::string& label, Strength* s, std::vector<std::string>* errors) {
    auto fail = [&](const std::string& field, const std::string& why) { errors->push_back(label + ", " + where + "." + field + ": " + why); };
    if (!j.is_object()) {
        errors->push_back(label + ", " + where + ": expected an object");
        return;
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
        const std::string& k = it.key();
        const ojson& v = it.value();
        if (k == "hardness") {
            const int i = v.is_string() ? enum_index(v.get<std::string>(), kHardnessJson, 6) : -1;
            if (i < 0) fail(k, v.dump() + " is not a hardness; there are " + join_names(kHardnessJson, 6));
            else s->hardness = i;
        } else if (k == "health") {
            if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 1000000) fail(k, v.dump() + ": takes a whole number, 1 to 1000000");
            else s->health = static_cast<int32_t>(v.get<int64_t>());
        } else if (k == "breaks") {
            const int i = v.is_string() ? enum_index(v.get<std::string>(), kBreaksJson, 4) : -1;
            if (i < 0) fail(k, v.dump() + " is not a class; there are " + join_names(kBreaksJson, 4));
            else s->breaks = i;
        } else if (k == "itemsPerBlocks") {
            if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 100000) fail(k, v.dump() + ": takes a whole number, 1 to 100000");
            else s->items_per_blocks = static_cast<uint32_t>(v.get<int64_t>());
        } else if (k == "drops") {
            if (!v.is_string() || v.get<std::string>().empty()) fail(k, "takes an item: a GameItems name or a GUID");
            else s->drops = v.get<std::string>();
        } else {
            fail(k, "is not a field of a strength; there are hardness, health, breaks, itemsPerBlocks, drops");
        }
    }
}

namespace {
// "strength": { ... }, or the name of a block in the mod's "strengths" section.
void read_strength_or_name(const ojson& j, const ModFile& mf, const std::string& label, Strength* s, std::vector<std::string>* errors) {
    if (!j.is_string()) {
        read_strength(j, "strength", label, s, errors);
        return;
    }
    const auto it = mf.strengths.find(j.get<std::string>());
    if (it == mf.strengths.end()) {
        errors->push_back(label + ", strength: no strength \"" + j.get<std::string>() + "\" in this mod's \"strengths\" sections");
        return;
    }
    read_strength(it->second, "strength", label, s, errors);
}
// "surface": { ... }, or the name of a block in the mod's "surfaces" sections. The surface it means, or null with an error.
const ojson* read_surface_or_name(const ojson& j, const ModFile& mf, const std::string& label, bool tweak, std::vector<std::string>* errors) {
    const ojson* s = &j;
    if (j.is_string()) {
        const std::string name = j.get<std::string>();
        const auto it = mf.surfaces.find(name);
        if (it == mf.surfaces.end()) {
            errors->push_back(label + ", surface: " +
                              (name.find(':') != std::string::npos ? "\"" + name + "\" is another mod's: a data file names a surface of its own mod, or writes it here"
                                                                   : "no surface \"" + name + "\" in this mod's \"surfaces\" sections"));
            return nullptr;
        }
        s = &it->second;
    }
    const size_t before = errors->size();
    ksf::check_surface(*s, label, errors, tweak);
    return errors->size() == before ? s : nullptr;
}

// The block as a writer reads it: a surface named by a block is written out in it.
std::string with_surface(const ojson& j, const ojson* surface) {
    if (!surface || !j.contains("surface") || !j["surface"].is_string()) return j.dump();
    ojson copy = j;
    copy["surface"] = *surface;
    return copy.dump();
}
}  // namespace

void read_blocks(const ojson& doc, ModFile* out) {
    auto err = [&](const std::string& t) { out->errors.push_back(out->file + ": " + t); };
    if (!doc.is_object()) {
        err("expected an object");
        return;
    }
    if (auto it = doc.find("materialTweaks"); it != doc.end()) {
        if (!it->is_object()) err("materialTweaks: expected block name -> block");
        else
            for (auto b = it->begin(); b != it->end(); ++b) {
                const std::string label = "block materialTweak " + b.key();
                const ojson& j = b.value();
                if (!valid_name(b.key())) { err(label + ": a block name takes letters, digits, '-', '_' and '.'"); continue; }
                if (!j.is_object()) { err(label + ": expected an object"); continue; }
                Tweak t;
                t.mod = out->mod;
                t.dir = out->dir;
                t.block = b.key();
                t.definition = j.dump();
                std::vector<std::string> errors;
                for (auto f = j.begin(); f != j.end(); ++f) {
                    if (f.key() == "material") {
                        if (!f->is_string() || f->get<std::string>().empty()) errors.push_back(label + ", material: takes a game material: a GameMaterials name");
                        else t.material = f->get<std::string>();
                    } else if (f.key() == "strength") {
                        read_strength_or_name(*f, *out, label, &t.strength, &errors);
                    } else if (f.key() == "surface") {
                        // what kiln.surfaces cooks on a client; checked here on every side, so a wrong one is refused alike everywhere
                        if (const ojson* sf = read_surface_or_name(*f, *out, label, true, &errors)) t.definition = with_surface(j, sf);
                    } else if (f.key() == "feel" || f.key() == "comment" || f.key() == "was") {
                        // a table writer's fields; nothing the merge here has to read
                    } else {
                        errors.push_back(label + ", " + f.key() + ": is not a field of a material tweak; there are material, strength, surface");
                    }
                }
                if (t.material.empty() && errors.empty()) errors.push_back(label + ", material: is required: which game material it changes");
                if (!t.strength.any() && !j.contains("surface") && errors.empty()) errors.push_back(label + ": changes nothing: it needs a strength or a surface");
                if (!errors.empty()) {
                    for (const auto& e : errors) err(e);
                    continue;
                }
                out->tweaks.push_back(std::move(t));
            }
    }
    if (auto it = doc.find("buildingMaterials"); it != doc.end()) {
        if (!it->is_object()) err("buildingMaterials: expected block name -> block");
        else
            for (auto b = it->begin(); b != it->end(); ++b) {
                const std::string label = "block buildingMaterial " + b.key();
                const ojson& j = b.value();
                if (!valid_name(b.key())) { err(label + ": a block name takes letters, digits, '-', '_' and '.'"); continue; }
                if (!j.is_object()) { err(label + ": expected an object"); continue; }
                std::vector<std::string> errors;
                Material m;
                m.mod = out->mod;
                m.dir = out->dir;
                m.block = b.key();
                m.definition = j.dump();
                std::string was;
                for (auto f = j.begin(); f != j.end(); ++f) {
                    const std::string& k = f.key();
                    if (k == "strength") read_strength_or_name(*f, *out, label, &m.strength, &errors);
                    else if (k == "roof") {
                        if (!f->is_boolean()) errors.push_back(label + ", roof: takes true or false");
                        else m.roof = f->get<bool>();
                    } else if (k == "was") {
                        if (!f->is_string()) errors.push_back(label + ", was: takes the block's old name");
                        else was = f->get<std::string>();
                    } else if (k == "name") {
                        if (!f->is_string() || f->get<std::string>().empty()) errors.push_back(label + ", name: takes the name players read");
                    } else if (k == "surface") {
                        if (const ojson* sf = read_surface_or_name(*f, *out, label, false, &errors)) m.definition = with_surface(j, sf);
                    } else if (k == "recipe") {
                        // the block item's; its main input is also what digging gives back. The rest is Kiln.Blocks' to check
                        if (f->is_object()) {
                            if (f->contains("makes") && (*f)["makes"].is_number_integer() && (*f)["makes"].get<int64_t>() > 0)
                                m.loot_makes = static_cast<uint32_t>((*f)["makes"].get<int64_t>());
                            if (f->contains("needs") && (*f)["needs"].is_object() && !(*f)["needs"].empty()) {
                                const auto first = (*f)["needs"].begin();
                                if (first.value().is_number_integer() && first.value().get<int64_t>() > 0) {
                                    m.loot_from = first.key();
                                    m.loot_count = static_cast<uint32_t>(first.value().get<int64_t>());
                                }
                            }
                        }
                    } else if (k == "description" || k == "icon" || k == "feel" || k == "loot" || k == "comment" || k == "tweak") {
                        // the block item's and the table writers' fields
                    } else {
                        errors.push_back(label + ", " + k + ": is not a field of a building material; there are name, description, strength, roof, surface, icon, feel, recipe, loot, was");
                    }
                }
                if (!j.contains("name") && errors.empty()) errors.push_back(label + ", name: is required: what players read");
                if (out->clients != "mod" && errors.empty())
                    errors.push_back(label + ": mod " + out->mod + " says \"clients\": \"" + out->clients + "\", and a new material has to be on every player's game. Set \"clients\": \"mod\" in kiln.mod.json");
                if (!errors.empty()) {
                    for (const auto& e : errors) err(e);
                    continue;
                }
                const std::string key = was.empty() ? b.key() : was;
                m.guid = derive_guid(out->mod, "material", key);
                m.item = true;
                m.item_guid = derive_guid(out->mod, "item", key);
                m.item_id = item_id_of(m.item_guid);
                out->materials.push_back(std::move(m));
            }
    }
    read_terrain_blocks(doc, out);
    if (auto it = doc.find("blueprints"); it != doc.end()) {
        if (!it->is_object()) err("blueprints: expected block name -> block");
        else
            for (auto b = it->begin(); b != it->end(); ++b) {
                std::vector<std::string> errors;
                Shape sh;
                read_shape(b.key(), b.value(), *out, &sh, &errors);
                if (!errors.empty()) {
                    for (const auto& e : errors) err(e);
                    continue;
                }
                out->shapes.push_back(std::move(sh));
            }
    }
    if (auto section=doc.find("buildingMaterialPools"); section!=doc.end()) {
        if (!section->is_object()) err("buildingMaterialPools: expected block name -> block");
        else for (auto it=section->begin();it!=section->end();++it) {
            PoolDefinition p; p.mod=out->mod; p.block=it.key(); p.dependencies=out->dependencies;
            const auto& j=it.value(); p.definition=j.dump();
            auto bad=[&](const std::string& text){ p.valid=false; err("block buildingMaterialPool "+p.block+": "+text); };
            std::string was;
            if (!valid_name(p.block) || !j.is_object()) bad("expected a named pool object");
            if (j.is_object()) {
                for(auto f=j.begin();f!=j.end();++f) {
                    if(f.key()=="name") { if(!f->is_string() || f->get<std::string>().empty()) bad("name is required"); }
                    else if(f.key()=="was") { if(!f->is_string() || !valid_name(f->get<std::string>())) bad("invalid was"); else was=f->get<std::string>(); }
                    else if(f.key()=="roof") { if(!f->is_boolean()) bad("roof takes true or false"); else p.roof=f->get<bool>(); }
                    else if(f.key()!="materials") bad("unknown field "+f.key());
                }
                if(!j.contains("name")) bad("name is required");
                if(!j.contains("materials") || !j["materials"].is_array()) bad("materials takes 1..64 entries");
                else {
                    // the count is resolve_pools' to name (an argument error with the count), not a parse error
                    for(const auto& e:j["materials"]) {
                        PoolEntry entry;
                        if(!e.is_object()) bad("entry takes exactly one of material/gameMaterial");
                        else {
                            if(e.contains("material") && e["material"].is_string()) entry.material=e["material"].get<std::string>();
                            if(e.contains("gameMaterial") && e["gameMaterial"].is_string()) entry.game_material=e["gameMaterial"].get<std::string>();
                            if(e.size()!=1 || (entry.material.empty()==entry.game_material.empty())) bad("entry takes exactly one of material/gameMaterial");
                        }
                        p.entries.push_back(std::move(entry));
                    }
                }
            }
            if(out->clients!="mod") bad("requires clients: mod");
            p.guid=derive_guid(p.mod,"materialpool",was.empty()?p.block:was);
            nlohmann::json canonical=nlohmann::json::object();
            if(j.is_object()) canonical=nlohmann::json::parse(j.dump());
            canonical["roof"]=p.roof;
            if(canonical.contains("materials") && canonical["materials"].is_array()) for(auto& e:canonical["materials"]) {
                if(e.is_object() && e.contains("material") && e["material"].is_string()) {
                    auto ref=e["material"].get<std::string>();
                    if(ref.find(':')==std::string::npos) e["material"]=p.mod+":"+ref;
                }
            }
            p.definition=canonical.dump();
            out->pools.push_back(std::move(p));
        }
    }
    if (auto section = doc.find("toolMaterialPools"); section != doc.end()) {
        if (!section->is_object()) err("toolMaterialPools: expected block name -> block");
        else for (auto it = section->begin(); it != section->end(); ++it) {
            PoolDefinition p; p.mod = out->mod; p.block = it.key(); p.tool = true; p.dependencies = out->dependencies;
            const auto& j = it.value();
            auto bad = [&](const std::string& why) { p.valid = false; err("block toolMaterialPool " + p.block + ": " + why); };
            if (!valid_name(p.block) || !j.is_object()) bad("expected a named pool object");
            std::string was;
            if (j.is_object()) {
                for (auto f = j.begin(); f != j.end(); ++f) {
                    if (f.key() == "name") { if (!f->is_string() || f->get<std::string>().empty()) bad("name is required"); }
                    else if (f.key() == "was") { if (!f->is_string() || !valid_name(f->get<std::string>())) bad("invalid was"); else was = f->get<std::string>(); }
                    else if (f.key() != "entries") bad("unknown field " + f.key());
                }
                if (!j.contains("name")) bad("name is required");
                if (!j.contains("entries") || !j["entries"].is_array() || j["entries"].empty() || j["entries"].size() > kPoolCapacity)
                    bad("entries takes 1..64 terrain items");
                else for (const auto& e : j["entries"]) {
                    PoolEntry entry;
                    if (!e.is_object() || e.size() != 3 || !e.contains("item") || !e["item"].is_string() || e["item"].get<std::string>().empty() ||
                        !e.contains("terrainMaterial") || !e["terrainMaterial"].is_number_integer() || e["terrainMaterial"].get<int64_t>() < 1 || e["terrainMaterial"].get<int64_t>() > 127 ||
                        !e.contains("voxelsPerItem") || !e["voxelsPerItem"].is_number_integer() || e["voxelsPerItem"].get<int64_t>() < 1 || e["voxelsPerItem"].get<int64_t>() > 1000000)
                        bad("each entry takes item, terrainMaterial (1..127), voxelsPerItem (1..1000000)");
                    else { entry.item = e["item"].get<std::string>(); Guid guid; if (Guid::parse(entry.item, &guid)) entry.item = std::to_string(item_id_of(guid)); entry.terrain_material = e["terrainMaterial"].get<uint32_t>(); entry.voxels_per_item = e["voxelsPerItem"].get<uint32_t>(); }
                    p.entries.push_back(std::move(entry));
                }
            }
            if (out->clients != "mod") bad("requires clients: mod");
            p.guid = derive_guid(p.mod, "toolmaterialpool", was.empty() ? p.block : was);
            p.definition = nlohmann::json::parse(j.dump()).dump();
            out->pools.push_back(std::move(p));
        }
    }
    // Computed brush descriptors emitted by Kiln.Blocks. These do not make ordinary BlueprintBlock
    // accept terrain data: a separate section owns the raw-fill shape contract.
    if (auto section = doc.find("toolBrushShapes"); section != doc.end()) {
        if (!section->is_object()) err("toolBrushShapes: expected block name -> shape");
        else for (auto it = section->begin(); it != section->end(); ++it) {
            const auto& j = it.value();
            auto bad = [&](const std::string& why) { err("block terrainBrush " + it.key() + ": " + why); };
            Shape shape; shape.mod = out->mod; shape.block = it.key(); shape.terrain_brush = true;
            if (!valid_name(it.key()) || !j.is_object() || j.size() != 3 || !j.contains("item") || !j["item"].is_string() ||
                !Guid::parse(j["item"].get<std::string>(), &shape.item) || shape.item.zero() || !j.contains("shape") || j["shape"] != "sphere" ||
                !j.contains("radius") || !j["radius"].is_number()) { bad("expected item GUID, sphere shape and radius"); continue; }
            const double radius = j["radius"].get<double>();
            if (!std::isfinite(radius) || radius < 0.25 || radius > 8 || out->clients != "mod") { bad("radius is 0.25..8 metres; requires clients: mod"); continue; }
            const auto side = static_cast<uint32_t>(std::ceil(radius * 4)); // half-metre cells, diameter 2r
            shape.x = shape.y = shape.z = side; shape.item_id = item_id_of(shape.item);
            shape.bits.resize(size_t(side) * side * side, 0);
            // Sample each voxel centre; a terrain brush's registry payload contains fill bytes, not material ids.
            for (uint32_t z = 0; z < side; ++z) for (uint32_t y = 0; y < side; ++y) for (uint32_t x = 0; x < side; ++x) {
                const double dx = (double(x) + 0.5 - double(side) / 2) * 0.5;
                const double dy = (double(y) + 0.5 - double(side) / 2) * 0.5;
                const double dz = (double(z) + 0.5 - double(side) / 2) * 0.5;
                if (dx * dx + dy * dy + dz * dz <= radius * radius) shape.bits[x + side * (y + side * z)] = 255;
            }
            shape.definition = nlohmann::json::parse(j.dump()).dump();
            out->shapes.push_back(std::move(shape));
        }
    }
    read_crafting_props(doc, out);
}

// The same cells Kiln.Blocks' ShapeCells makes: layers from the bottom up, rows from the back, cells from the left; # a cell, . or a space none. Or, from code,
// the cells already made: {"item": GUID, "size": [x, y, z], "cells": base64}.
void read_shape(const std::string& name, const ojson& j, const ModFile& mf, Shape* sh, std::vector<std::string>* errors) {
    const std::string label = "block blueprint " + name;
    auto fail = [&](const std::string& field, const std::string& why) { errors->push_back(label + (field.empty() ? "" : ", " + field) + ": " + why); };
    if (!valid_name(name)) return fail("", "a block name takes letters, digits, '-', '_' and '.'");
    if (!j.is_object()) return fail("", "expected an object");
    sh->mod = mf.mod;
    sh->block = name;
    sh->dependencies = mf.dependencies;
    std::string was;
    const ojson* cells = nullptr;
    const ojson* size = nullptr;
    std::string item;
    for (auto f = j.begin(); f != j.end(); ++f) {
        const std::string& k = f.key();
        if (k == "materialPool") {
            if (!f->is_string() || f->get<std::string>().empty()) fail(k,"takes a pool reference");
            else { sh->material_pool=f->get<std::string>(); Guid pg; if(sh->material_pool.find(':')==std::string::npos && !Guid::parse(sh->material_pool,&pg)) sh->material_pool=mf.mod+":"+sh->material_pool; }
        } else if (k == "cells") cells = &f.value();
        else if (k == "size") size = &f.value();
        else if (k == "item") {
            if (!f->is_string() || !Guid::parse(f->get<std::string>(), &sh->item)) fail(k, "takes the blueprint item's GUID");
            item = f->is_string() ? f->get<std::string>() : "";
        } else if (k == "roof") {
            if (!f->is_boolean()) fail(k, "takes true or false");
            else sh->roof = f->get<bool>();
        } else if (k == "was") {
            if (!f->is_string()) fail(k, "takes the block's old name");
            else was = f->get<std::string>();
        } else if (k == "name") {
            if (!f->is_string() || f->get<std::string>().empty()) fail(k, "takes the name players read");
        } else if (k == "cost") {
            if (!f->is_number_integer() || f->get<int64_t>() < 1 || f->get<int64_t>() > 9999) fail(k, f->dump() + " is outside 1..9999");
        } else if (k == "description" || k == "recipe" || k == "tweak" || k == "comment") {
            // the blueprint item's fields, Kiln.Blocks' to read
        } else {
            fail(k, "is not a field of a shape; there are name, description, cells, roof, cost, recipe, was");
        }
    }
    if (!j.contains("name") && item.empty()) fail("name", "is required: what players read");
    if (!cells) fail("cells", "a shape needs its cells: layers of rows of # and .");
    if (!errors->empty()) return;
    if (cells->is_string()) {
        // the computed form code sends
        if (!size || !size->is_array() || size->size() != 3) return fail("size", "takes [x, y, z] with the cells");
        uint32_t dims[3];
        for (int i = 0; i < 3; ++i) {
            if (!(*size)[i].is_number_integer() || (*size)[i].get<int64_t>() < 1 || (*size)[i].get<int64_t>() > kMaxShapeCells) return fail("size", "each side is 1.." + std::to_string(kMaxShapeCells));
            dims[i] = static_cast<uint32_t>((*size)[i].get<int64_t>());
        }
        sh->x = dims[0], sh->y = dims[1], sh->z = dims[2];
        if (!base64_decode(cells->get<std::string>(), &sh->bits) || sh->bits.size() != (sh->x * sh->y * sh->z + 7) / 8) return fail("cells", "doesn't hold one bit per cell of its size");
    } else {
        if (!cells->is_array() || cells->empty()) return fail("cells", "a shape needs its cells: layers of rows of # and .");
        uint32_t x = 0, y = static_cast<uint32_t>(cells->size()), z = 0;
        for (size_t ly = 0; ly < cells->size(); ++ly) {
            const ojson& rows = (*cells)[ly];
            if (!rows.is_array()) return fail("cells[" + std::to_string(ly) + "]", "a layer is a list of rows");
            z = std::max<uint32_t>(z, static_cast<uint32_t>(rows.size()));
            for (size_t lz = 0; lz < rows.size(); ++lz) {
                if (!rows[lz].is_string()) return fail("cells[" + std::to_string(ly) + "][" + std::to_string(lz) + "]", "a row is a string of # and .");
                std::string r = rows[lz].get<std::string>();
                while (!r.empty() && std::isspace(static_cast<unsigned char>(r.back()))) r.pop_back();
                x = std::max<uint32_t>(x, static_cast<uint32_t>(r.size()));
            }
        }
        if (x == 0 || z == 0) return fail("cells", "the shape has no cells: # is a cell");
        if (x > kMaxShapeCells || y > kMaxShapeCells || z > kMaxShapeCells)
            return fail("cells", std::to_string(x) + " x " + std::to_string(y) + " x " + std::to_string(z) + " cells: a shape fits in " + std::to_string(kMaxShapeCells) + " each way (" +
                                     std::to_string(kMaxShapeCells / 2) + " m)");
        sh->x = x, sh->y = y, sh->z = z;
        sh->bits.assign((x * y * z + 7) / 8, 0);
        uint32_t full = 0;
        for (uint32_t ly = 0; ly < y; ++ly) {
            const ojson& rows = (*cells)[ly];
            for (uint32_t lz = 0; lz < rows.size(); ++lz) {
                const std::string r = rows[lz].get<std::string>();
                for (uint32_t lx = 0; lx < r.size(); ++lx) {
                    const char c = r[lx];
                    if (c == '.' || c == ' ') continue;
                    if (c != '#') return fail("cells[" + std::to_string(ly) + "][" + std::to_string(lz) + "]", std::string("'") + c + "' at " + std::to_string(lx) + ": a cell is # and none is . or a space");
                    const uint32_t i = lx + x * (ly + y * lz);
                    sh->bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
                    ++full;
                }
            }
        }
        if (full == 0) return fail("cells", "the shape has no cells: # is a cell");
    }
    if (mf.clients != "mod")
        return fail("", "mod " + mf.mod + " says \"clients\": \"" + mf.clients + "\", and a new shape has to be on every player's game. Set \"clients\": \"mod\" in kiln.mod.json");
    if (item.empty()) sh->item = derive_guid(mf.mod, "item", was.empty() ? name : was);
    sh->item_id = item_id_of(sh->item);
    std::string hex;
    for (uint8_t b : sh->bits) {
        char h[3];
        std::snprintf(h, sizeof h, "%02x", b);
        hex += h;
    }
    ojson n = ojson::object();
    n["item"] = sh->item.text();
    n["size"] = ojson::array({sh->x, sh->y, sh->z});
    n["cells"] = hex;
    n["roof"] = sh->roof;
    if(!sh->material_pool.empty()) n["materialPool"]=sh->material_pool;
    sh->definition = n.dump();
}

// ---- ids ---------------------------------------------------------------------------------------------------------
Allocation allocate(std::vector<Material> live, std::vector<Material> kept, const std::vector<int>& free_ids, const std::vector<int>& deprecated,
                    const std::vector<Material>& remembered) {
    if (!remembered.empty()) {
        std::vector<std::string> retained;
        for (;;) {
            auto a = allocate(live, kept, free_ids, deprecated);
            bool changed = false;
            std::erase_if(live, [&](const Material& m) {
                const auto old = std::find_if(remembered.begin(), remembered.end(), [&](const Material& k) { return !k.terrain && k.guid == m.guid; });
                if (old == remembered.end() || std::any_of(a.placed.begin(), a.placed.end(), [&](const Placed& p) { return p.m.guid == m.guid; })) return false;
                auto k = *old;k.live = false;k.item = false;
                kept.push_back(std::move(k));
                retained.push_back("block buildingMaterial " + m.block + " (mod " + m.mod + "): refused: no material id remains; a world has cells of it, so it is kept as an absent mod's material is");
                changed = true;
                return true;
            });
            if (!changed) {
                a.refused.insert(a.refused.end(), retained.begin(), retained.end());
                return a;
            }
        }
    }
    Allocation a;
    auto by_guid = [](const Material& x, const Material& y) { return x.guid.text() < y.guid.text(); };
    std::sort(live.begin(), live.end(), by_guid);
    std::sort(kept.begin(), kept.end(), by_guid);
    std::vector<int> room = free_ids;
    room.insert(room.end(), deprecated.begin(), deprecated.end());
    a.room = room.size();
    const size_t holds = std::min(kept.size(), room.size());
    const size_t for_live = room.size() - holds;
    size_t at = 0;
    const size_t free_count = free_ids.size();
    auto place = [&](Material&& m) {
        Placed p;
        p.reused = at >= free_count;
        p.cell = room[at++];
        p.m = std::move(m);
        a.placed.push_back(std::move(p));
    };
    for (size_t i = 0; i < live.size(); ++i) {
        if (i >= for_live) {
            char b[256];
            std::snprintf(b, sizeof b, "the game has room for %zu new building materials across all mods%s", room.size(),
                          kept.empty() ? "" : (", " + std::to_string(holds) + " of them kept for absent mods (WorldTables.Forget frees one)").c_str());
            a.refused.push_back("block buildingMaterial " + live[i].block + " (mod " + live[i].mod + "): refused: " + b);
            continue;
        }
        place(std::move(live[i]));
    }
    for (size_t i = 0; i < kept.size(); ++i) {
        if (at >= room.size()) {
            a.refused.push_back("kept material " + kept[i].guid.text() + " (" + kept[i].block + ", mod " + kept[i].mod + "): no room left: a world's cells of it are lost when it loads");
            continue;
        }
        place(std::move(kept[i]));
    }
    return a;
}

uint32_t loot_rate(uint32_t makes, uint32_t count) {
    if (!count) return 1;
    return std::max<uint32_t>(1, (makes + count / 2) / count);
}

std::string handshake_hash(const std::vector<Placed>& placed) {
    uint64_t h = 0xcbf29ce484222325ull;
    bool any = false;
    auto add = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    };
    for (const Placed& p : placed) {
        if (!p.m.live) continue;
        any = true;
        const uint8_t id = static_cast<uint8_t>(p.cell);
        add(&id, 1);
        add(p.m.guid.b.data(), 16);
    }
    if (!any) return kEmptyHash;
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
    return s;
}

// ---- crafting props ----------------------------------------------------------------------------------------------
// kiln.content derives a clone's name tag from its GUID (newcontent.cpp derive_guid, purpose "kiln.item.name"): the first four bytes XOR the purpose's hash, the last byte 'K'.
uint32_t name_tag_of_item(const Guid& item) {
    static const char purpose[] = "kiln.item.name";
    uint8_t g[16];
    std::memcpy(g, item.b.data(), 16);
    const uint32_t h = fnv1a(purpose, sizeof purpose - 1);
    for (int i = 0; i < 4; ++i) g[i] ^= static_cast<uint8_t>(h >> (8 * i));
    g[15] ^= 0x4B;
    return fnv1a(g, 16);
}

namespace {
void make_crafting_prop(CraftingProp* p, const std::string& key, const Guid* item) {
    p->guid = derive_guid(p->mod, "craftingprop", key);
    p->prop_id = item_id_of(p->guid);
    p->item = item ? *item : derive_guid(p->mod, "item", key);
    p->item_id = item_id_of(p->item);
    p->name_tag = name_tag_of_item(p->item);
    ojson d = ojson::object();
    d["key"] = key;
    d["item"] = p->item.text();
    d["name"] = p->name;
    p->definition = d.dump();
}
}  // namespace

void read_crafting_props(const ojson& doc, ModFile* out) {
    auto err = [&](const std::string& t) { out->errors.push_back(out->file + ": " + t); };
    const auto need_mod = [&](const std::string& label) {
        if (out->clients == "mod") return true;
        err(label + ", provides: mod " + out->mod + " says \"clients\": \"" + out->clients + "\", and a crafting prop has its place in a table every game merges at its start, so it has to be on every player's game. Set \"clients\": \"mod\" in kiln.mod.json");
        return false;
    };
    // a data file: the "provides" of a prop block (the block's own name is what players read)
    static const char* const kSections[] = {"props", "stations", "containers", "doors", "beds", "seats"};
    static const char* const kKinds[] = {"prop", "station", "container", "door", "bed", "seat"};
    for (size_t k = 0; k < std::size(kSections); ++k) {
        const auto it = doc.find(kSections[k]);
        if (it == doc.end() || !it->is_object()) continue;
        for (auto b = it->begin(); b != it->end(); ++b) {
            if (!b.value().is_object() || !b.value().contains("provides")) continue;
            const std::string label = std::string("block ") + kKinds[k] + " " + b.key();
            const ojson& v = b.value()["provides"];
            if (!v.is_boolean()) { err(label + ", provides: takes true or false"); continue; }
            if (!v.get<bool>()) continue;
            if (!valid_name(b.key())) { err(label + ": a block name takes letters, digits, '-', '_' and '.'"); continue; }
            if (!need_mod(label)) continue;
            const ojson& j = b.value();
            if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) { err(label + ", name: is required: what players read"); continue; }
            CraftingProp p;
            p.mod = out->mod;
            p.block = b.key();
            p.name = j["name"].get<std::string>();
            const std::string key = j.contains("was") && j["was"].is_string() && !j["was"].get<std::string>().empty() ? j["was"].get<std::string>() : b.key();
            make_crafting_prop(&p, key, nullptr);
            out->craft_props.push_back(std::move(p));
        }
    }
    // code's define: {"craftingProps": {"<block>": {"key": ..., "item": GUID, "name": ...}}}
    if (const auto it = doc.find("craftingProps"); it != doc.end()) {
        if (!it->is_object()) { err("craftingProps: expected block name -> block"); return; }
        for (auto b = it->begin(); b != it->end(); ++b) {
            const std::string label = "block craftingProp " + b.key();
            if (!valid_name(b.key())) { err(label + ": a block name takes letters, digits, '-', '_' and '.'"); continue; }
            if (!b.value().is_object()) { err(label + ": expected an object"); continue; }
            const ojson& j = b.value();
            CraftingProp p;
            p.mod = out->mod;
            p.block = b.key();
            std::string key = b.key();
            Guid item;
            bool has_item = false, bad = false;
            for (auto f = j.begin(); f != j.end(); ++f) {
                if (f.key() == "key") {
                    if (!f->is_string() || f->get<std::string>().empty()) { err(label + ", key: takes the block's name or its \"was\""); bad = true; }
                    else key = f->get<std::string>();
                } else if (f.key() == "item") {
                    if (!f->is_string() || !Guid::parse(f->get<std::string>(), &item)) { err(label + ", item: takes the GUID of the item that places it"); bad = true; }
                    else has_item = true;
                } else if (f.key() == "name") {
                    if (!f->is_string() || f->get<std::string>().empty()) { err(label + ", name: takes what players read"); bad = true; }
                    else p.name = f->get<std::string>();
                } else {
                    err(label + ", " + f.key() + ": is not a field of a crafting prop; there are key, item, name");
                    bad = true;
                }
            }
            if (bad || !need_mod(label)) continue;
            if (p.name.empty()) { err(label + ", name: is required: what players read"); continue; }
            make_crafting_prop(&p, key, has_item ? &item : nullptr);
            out->craft_props.push_back(std::move(p));
        }
    }
}

bool read_crafting_prop_table(const Bytes& res, const CraftingPropLayout& C, uint64_t* used, std::set<uint32_t>* ids, std::string* why) {
    ArrayView v;
    if (!C.ok || !array_at(res, C.array, C.stride, &v)) {
        *why = "the crafting prop table doesn't read as its layout says";
        return false;
    }
    *used = 0;
    ids->clear();
    for (uint32_t i = 0; i < v.count; ++i) {
        const uint8_t* e = v.base + size_t{i} * C.stride;
        const uint8_t index = e[C.index];
        if (index >= kCraftingPropPlaces) {
            *why = "a crafting prop of the game has place " + std::to_string(index) + ": past the " + std::to_string(kCraftingPropPlaces) + " a mask holds";
            return false;
        }
        *used |= uint64_t{1} << index;
        ids->insert(rd<uint32_t>(e + C.id));
    }
    return true;
}

void allocate_crafting_props(std::vector<CraftingProp>* props, uint64_t used, std::vector<std::string>* refused) {
    // in the order of the GUIDs as text, as the materials' ids are given
    std::sort(props->begin(), props->end(), [](const CraftingProp& a, const CraftingProp& b) { return a.guid.text() < b.guid.text(); });
    std::vector<int> free_places;
    for (uint32_t i = 0; i < kCraftingPropPlaces; ++i)
        if (!(used >> i & 1)) free_places.push_back(static_cast<int>(i));
    std::vector<CraftingProp> kept;
    for (CraftingProp& p : *props) {
        if (kept.size() >= free_places.size()) {
            refused->push_back("block craftingProp " + p.block + " (mod " + p.mod + "): refused: the game has room for " + std::to_string(free_places.size()) + " new crafting props across all mods, and " +
                               std::to_string(kept.size()) + " are taken");
            continue;
        }
        p.index = free_places[kept.size()];
        kept.push_back(std::move(p));
    }
    *props = std::move(kept);
}

bool merge_crafting_props(const Bytes& game, const CraftingPropLayout& C, const std::vector<CraftingProp>& props, Bytes* out, std::string* why) {
    *out = game;
    if (props.empty()) return true;
    ArrayView v;
    if (!C.ok || !array_at(game, C.array, C.stride, &v)) {
        *why = "the crafting prop table doesn't read as its layout says";
        return false;
    }
    const size_t old_at = v.base ? static_cast<size_t>(v.base - game.data()) : 0;
    while (out->size() % 16) out->push_back(0);
    const size_t at = out->size();
    out->resize(at + (size_t{v.count} + props.size()) * C.stride, 0);
    if (v.count) std::memcpy(out->data() + at, game.data() + old_at, size_t{v.count} * C.stride);
    for (size_t i = 0; i < props.size(); ++i) {
        const CraftingProp& p = props[i];
        uint8_t* e = out->data() + at + (size_t{v.count} + i) * C.stride;
        std::memcpy(e + C.guid, p.guid.b.data(), 16);
        wr<uint32_t>(e + C.id, p.prop_id);
        e[C.index] = static_cast<uint8_t>(p.index);
        wr<uint32_t>(e + C.name, p.name_tag);
        wr<uint32_t>(e + C.item, p.item_id);
        std::memcpy(e + C.item_ref, p.item.b.data(), 16);
    }
    wr<int32_t>(out->data() + C.array, static_cast<int32_t>(int64_t(at) - int64_t(C.array)));
    wr<uint32_t>(out->data() + C.array + 4, static_cast<uint32_t>(v.count + props.size()));
    return true;
}

std::string handshake_hash(const std::vector<Placed>& placed, const std::vector<CraftingProp>& props) {
    const std::string materials = handshake_hash(placed);
    if (props.empty()) return materials;
    uint64_t h = 0xcbf29ce484222325ull;
    const auto add = [&](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 0x100000001b3ull;
    };
    add(materials.data(), materials.size());
    for (const CraftingProp& p : props) {
        const uint8_t index = static_cast<uint8_t>(p.index);
        add("p", 1);
        add(&index, 1);
        add(p.guid.b.data(), 16);
    }
    char s[17];
    std::snprintf(s, sizeof s, "%016llx", static_cast<unsigned long long>(h));
    return s;
}

std::string handshake_hash(const std::vector<Placed>& placed, const std::vector<CraftingProp>& props,
                           const std::vector<MaterialPool>& pools, const std::vector<Shape>& shapes) {
    const std::string base=handshake_hash(placed,props);
    if(pools.empty()) return base; // Preserve every pre-pool profile's identity, byte for byte.
    uint64_t h=0xcbf29ce484222325ull;
    auto bytes=[&](const void* ptr,size_t n){auto p=static_cast<const uint8_t*>(ptr);for(size_t i=0;i<n;++i)h=(h^p[i])*0x100000001b3ull;};
    auto u32=[&](uint32_t n){uint8_t b[4]={uint8_t(n),uint8_t(n>>8),uint8_t(n>>16),uint8_t(n>>24)};bytes(b,4);};
    bytes(base.data(),base.size());
    static constexpr char version[]="kiln.materialpools.v1";bytes(version,sizeof version);
    auto sorted=pools;
    std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.guid.text()<b.guid.text();});
    u32(uint32_t(sorted.size()));
    for(const auto& pool:sorted) {
        bytes(pool.guid.b.data(),16);u32(item_id_of(pool.guid));u32((pool.roof?1u:0u) | (pool.tool?2u:0u));u32(uint32_t(pool.items.size()));
        for(uint32_t item:pool.items)u32(item);
        if (pool.tool) { for (uint32_t cell : pool.terrain_materials) u32(cell); for (uint32_t rate : pool.voxels_per_item) u32(rate); }
    }
    std::vector<Shape> bindings;
    for(const auto& shape:shapes)if(!shape.pool.zero())bindings.push_back(shape);
    std::sort(bindings.begin(),bindings.end(),[](const auto& a,const auto& b){return a.item.text()<b.item.text();});
    u32(uint32_t(bindings.size()));
    for(const auto& shape:bindings){bytes(shape.item.b.data(),16);bytes(shape.pool.b.data(),16);}
    char out[17];std::snprintf(out,sizeof out,"%016llx",static_cast<unsigned long long>(h));return out;
}

// ---- reading -----------------------------------------------------------------------------------------------------
bool array_at(const Bytes& res, uint32_t field, uint32_t stride, ArrayView* out) {
    if (static_cast<uint64_t>(field) + 8 > res.size()) return false;
    const int32_t off = rd<int32_t>(res.data() + field);
    const uint32_t count = rd<uint32_t>(res.data() + field + 4);
    out->count = count;
    if (!count) {
        out->base = nullptr;
        return true;
    }
    const int64_t at = int64_t{field} + off;
    if (off == 0 || at < 0 || static_cast<uint64_t>(at) + uint64_t{count} * stride > res.size()) return false;
    out->base = res.data() + at;
    return true;
}

bool read_registry(const Bytes& res, const Layouts& L, RegistryInfo* out, std::string* why) {
    ArrayView t, b;
    if (res.size() < L.reg_size || !array_at(res, L.reg_terrain, L.terrain.stride, &t) || !array_at(res, L.reg_building, L.building.stride, &b)) {
        *why = "the terraforming registry doesn't read as its layout says";
        return false;
    }
    for (uint32_t i = 0; i < t.count; ++i) {
        const uint8_t* c = t.base + size_t{i} * L.terrain.stride;
        out->terrain_at[c[L.terrain.id]] = static_cast<uint32_t>(c - res.data());
    }
    for (uint32_t i = 0; i < b.count; ++i) {
        const uint8_t* c = b.base + size_t{i} * L.building.stride;
        out->building_at[128 + c[L.building.id]] = static_cast<uint32_t>(c - res.data());
    }
    if (out->building_at.empty()) {
        *why = "the terraforming registry has no building material";
        return false;
    }
    return true;
}

bool world_has_table(const Bytes& world, const Layouts& L) {
    if (world.size() < L.world_size || world.size() < size_t{L.world_guids} + 256 * 16) return false;
    for (size_t i = 0; i < 256 * 16; ++i)
        if (world[L.world_guids + i]) return true;
    return false;
}

std::vector<int> free_building_ids(const Bytes& world, const Layouts& L) {
    std::vector<int> out;
    for (int id = 128; id < 256; ++id) {
        bool any = false;
        for (int i = 0; i < 16; ++i) any = any || world[L.world_guids + size_t(id) * 16 + i];
        if (!any) out.push_back(id);
    }
    return out;
}

std::vector<int> deprecated_ids(const Bytes& reg, const RegistryInfo& info, const Layouts& L) {
    std::vector<int> out;
    for (const auto& [cell, at] : info.building_at) {
        bool any = false;
        for (int i = 0; i < 16; ++i) any = any || reg[at + L.building.item + i];
        if (!any) out.push_back(cell);
    }
    return out;
}

// ---- writing -----------------------------------------------------------------------------------------------------
namespace {

std::string hardness_text(const Layouts& L, int v) { return v >= 0 && static_cast<size_t>(v) < L.hardness_names.size() ? L.hardness_names[v] : std::to_string(v); }
std::string class_text(const Layouts& L, int v) { return v >= 0 && static_cast<size_t>(v) < L.class_names.size() ? L.class_names[v] : std::to_string(v); }

// Writes a strength into one config. Returns what changed, for the log; empty: nothing did.
std::string apply_strength(uint8_t* c, const ConfigLayout& K, const Layouts& L, const Strength& s) {
    std::string out;
    auto note = [&](const std::string& t) { out += (out.empty() ? "" : ", ") + t; };
    if (s.hardness && c[K.hardness] != *s.hardness) {
        note("hardness " + hardness_text(L, c[K.hardness]) + " -> " + hardness_text(L, *s.hardness));
        c[K.hardness] = static_cast<uint8_t>(*s.hardness);
    }
    if (s.health && rd<int32_t>(c + K.health) != *s.health) {
        note("health " + std::to_string(rd<int32_t>(c + K.health)) + " -> " + std::to_string(*s.health));
        wr<int32_t>(c + K.health, *s.health);
    }
    if (s.breaks && c[K.klass] != *s.breaks) {
        note("breaks " + class_text(L, c[K.klass]) + " -> " + class_text(L, *s.breaks));
        c[K.klass] = static_cast<uint8_t>(*s.breaks);
    }
    if (s.items_per_blocks && rd<uint32_t>(c + K.rate) != *s.items_per_blocks) {
        note("itemsPerBlocks " + std::to_string(rd<uint32_t>(c + K.rate)) + " -> " + std::to_string(*s.items_per_blocks));
        wr<uint32_t>(c + K.rate, *s.items_per_blocks);
    }
    if (s.drops) {
        Guid g;
        if (Guid::parse(*s.drops, &g) && std::memcmp(c + K.item, g.b.data(), 16) != 0) {
            note("drops " + *s.drops);
            std::memcpy(c + K.item, g.b.data(), 16);
        }
    }
    return out;
}

// The config a new material starts from: the game's plainest of that class. A material's feel (sounds, friction, footsteps, what an
// NPC's path thinks of it) stays this one's until the block says otherwise.
const uint8_t* template_config(const Bytes& reg, const RegistryInfo& info, const Layouts& L, int klass, bool roof) {
    const ConfigLayout& K = L.building;
    const uint8_t* fallback = nullptr;
    for (const auto& [cell, at] : info.building_at) {
        const uint8_t* c = reg.data() + at;
        bool has_item = false;
        for (int i = 0; i < 16; ++i) has_item = has_item || c[K.item + i];
        if (!has_item || (K.roof != UINT32_MAX && (c[K.roof] != 0) != roof)) continue;
        if (!fallback) fallback = c;
        if (c[K.klass] == klass) return c;
    }
    return fallback ? fallback : reg.data() + info.building_at.begin()->second;
}

}  // namespace

bool merge_registry(const Bytes& game, const Layouts& L, const std::vector<Tweak>& tweaks, const std::vector<Placed>& placed, Bytes* out,
                    std::vector<RegistryChange>* changes, std::string* why) {
    RegistryInfo info;
    if (!read_registry(game, L, &info, why)) return false;
    *out = game;
    const ConfigLayout& B = L.building;
    const ConfigLayout& T = L.terrain;

    for (const Tweak& t : tweaks) {
        if (t.cell < 0 || !t.strength.any()) continue;
        const bool building = t.cell >= 128;
        const auto& map = building ? info.building_at : info.terrain_at;
        const auto it = map.find(t.cell);
        if (it == map.end()) {
            changes->push_back({"tweak " + t.block + " (mod " + t.mod + "): " + t.material + " has no config: nothing changed"});
            continue;
        }
        const std::string text = apply_strength(out->data() + it->second, building ? B : T, L, t.strength);
        changes->push_back({"tweak " + t.block + " (mod " + t.mod + "): " + t.material + " (" + std::to_string(t.cell) + "): " + (text.empty() ? "already so" : text)});
    }

    // New configs. A deprecated slot's config is replaced where it is; the rest make the array grow.
    std::vector<Bytes> added;
    for (const Placed& p : placed) {
        const int klass = p.m.strength.breaks.value_or(2);
        const uint8_t* tpl = template_config(game, info, L, klass, p.m.roof);
        Bytes c(tpl, tpl + B.stride);
        c[B.id] = static_cast<uint8_t>(p.cell - 128);
        std::memset(c.data() + B.conditional, 0, 8);
        if (B.roof != UINT32_MAX) c[B.roof] = p.m.roof ? 1 : 0;
        apply_strength(c.data(), B, L, p.m.strength);
        Guid loot;
        std::memcpy(loot.b.data(), c.data() + B.item, 16);
        changes->push_back({"material " + std::to_string(p.cell) + " " + p.m.guid.text() + " " + p.m.block + " (mod " + p.m.mod + ", " + (p.m.live ? "live" : "kept") + (p.reused ? ", reusing a deprecated slot" : "") +
                            "): hardness " + hardness_text(L, c[B.hardness]) + ", health " + std::to_string(rd<int32_t>(c.data() + B.health)) + ", breaks " + class_text(L, c[B.klass]) +
                            ", drops " + loot.text() + " per " + std::to_string(rd<uint32_t>(c.data() + B.rate)) + " blocks"});
        const auto at = info.building_at.find(p.cell);
        if (at != info.building_at.end()) std::memcpy(out->data() + at->second, c.data(), c.size());
        else added.push_back(std::move(c));
    }
    if (added.empty()) return true;

    ArrayView old;
    if (!array_at(*out, L.reg_building, B.stride, &old)) {
        *why = "the terraforming registry's building list doesn't read";
        return false;
    }
    const uint32_t old_count = old.count;
    const size_t old_at = static_cast<size_t>(old.base - out->data());
    pad4(out);
    const size_t at = out->size();
    out->resize(at + (size_t{old_count} + added.size()) * B.stride);
    std::memcpy(out->data() + at, out->data() + old_at, size_t{old_count} * B.stride);
    for (uint32_t i = 0; i < old_count; ++i) {
        const size_t field_old = old_at + size_t{i} * B.stride + B.conditional;
        const size_t field_new = at + size_t{i} * B.stride + B.conditional;
        const int32_t off = rd<int32_t>(out->data() + field_old);
        if (off) wr<int32_t>(out->data() + field_new, static_cast<int32_t>(int64_t(field_old) + off - int64_t(field_new)));
    }
    for (size_t i = 0; i < added.size(); ++i) std::memcpy(out->data() + at + (old_count + i) * B.stride, added[i].data(), B.stride);
    wr<int32_t>(out->data() + L.reg_building, static_cast<int32_t>(at - L.reg_building));
    wr<uint32_t>(out->data() + L.reg_building + 4, static_cast<uint32_t>(old_count + added.size()));
    return true;
}

bool merge_world(const Bytes& game, const Layouts& L, const std::vector<Placed>& placed, Bytes* out, std::string* why) {
    if (game.size() < size_t{L.world_guids} + 256 * 16) {
        *why = "a world resource is smaller than its material table";
        return false;
    }
    *out = game;
    for (const Placed& p : placed) std::memcpy(out->data() + L.world_guids + size_t(p.cell) * 16, p.m.guid.b.data(), 16);
    return true;
}

// ---- the build menu's pools ------------------------------------------------------------------------------------------
std::string pool_of(const Material& m) {
    if (!m.item) return "";
    if (m.terrain) return kTerrainPool;
    return m.roof ? kRoofPool : kWallPool;
}

std::string pool_of(const Material& m,const std::set<Guid>& grouped) {
    return !m.terrain && grouped.count(m.guid)?"":pool_of(m);
}

bool pool_counts(const Bytes& game, const Layouts& L, std::map<std::string, uint32_t>* out, std::string* why) {
    ArrayView pools;
    if (!array_at(game, L.pools, L.pool_stride, &pools)) {
        *why = "the material pools don't read as their layout says";
        return false;
    }
    out->clear();
    for (uint32_t i = 0; i < pools.count; ++i) {
        const size_t rec = static_cast<size_t>(pools.base - game.data()) + size_t{i} * L.pool_stride;
        const int32_t off = rd<int32_t>(game.data() + rec + L.pool_name);
        const uint32_t len = rd<uint32_t>(game.data() + rec + L.pool_name + 4);
        const int64_t at = int64_t(rec) + L.pool_name + off;
        ArrayView items;
        if (!off || at <= 0 || static_cast<uint64_t>(at) + len > game.size() || !array_at(game, static_cast<uint32_t>(rec + L.pool_materials), 4, &items)) {
            *why = "a material pool's name or list doesn't read";
            return false;
        }
        (*out)[std::string(reinterpret_cast<const char*>(game.data() + at), len)] = items.count;
    }
    return true;
}

std::string pool_room_text(const std::string& pool, uint32_t game_count) {
    const uint32_t room = game_count < kPoolCapacity ? kPoolCapacity - game_count : 0;
    const std::string cap = std::to_string(kPoolCapacity), have = std::to_string(game_count), fit = std::to_string(room);
    if (pool == kTerrainPool)
        return "the game's terrain shapes list at most " + cap + " placeable terrain materials and have " + have + " of their own: " + fit + " new ones fit across all mods";
    return "the game's build menu lists at most " + cap + (pool == kRoofPool ? " roof" : " wall") + " materials and has " + have + " of its own: " + fit + " new ones fit across all mods";
}

void fit_pools(std::vector<Material>* live, const std::map<std::string, uint32_t>& game_counts, const std::set<Guid>& remembered, std::vector<Material>* held,
               std::vector<std::string>* refused) {
    std::vector<size_t> order(live->size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t x, size_t y) { return (*live)[x].guid.text() < (*live)[y].guid.text(); });
    std::map<std::string, uint32_t> used;
    std::vector<bool> out(live->size(), false);
    for (const size_t i : order) {
        const Material& m = (*live)[i];
        const std::string pool = pool_of(m);
        if (pool.empty()) continue;
        const auto it = game_counts.find(pool);
        const uint32_t have = it == game_counts.end() ? 0 : it->second;
        if (have < kPoolCapacity && used[pool] < kPoolCapacity - have) {
            ++used[pool];
            continue;
        }
        out[i] = true;
        std::string text = std::string("block ") + (m.terrain ? "terrainMaterial " : "buildingMaterial ") + m.block + " (mod " + m.mod + "): refused: " + pool_room_text(pool, have);
        if (remembered.count(m.guid)) {
            text += ". A world has cells of it: it's kept as an absent mod's material is, so they stay";
            Material h = m;
            h.live = false;
            h.item = false;
            held->push_back(std::move(h));
        }
        refused->push_back(std::move(text));
    }
    std::vector<Material> keep;
    for (size_t i = 0; i < live->size(); ++i)
        if (!out[i]) keep.push_back(std::move((*live)[i]));
    *live = std::move(keep);
}

namespace {
bool pool_ref(const std::string& text, const std::string& owner, const std::string& mod, const std::string& key, const Guid& guid) {
    Guid parsed;
    if(Guid::parse(text,&parsed)) return parsed==guid;
    return text==(mod==owner?key:mod+":"+key) || text==mod+":"+key;
}
}
bool terrain_pool_items(const Bytes& pools, const Layouts& L, std::set<uint32_t>* items, std::string* why) {
    items->clear();
    const auto fail=[&](const char* reason){*why=reason;return false;};
    if(L.pool_stride!=24 || L.pool_id!=4 || L.pool_materials!=8 || L.pool_name!=16)
        return fail("vanilla terrain pool: reflected row layout is unavailable or unsupported");
    ArrayView rows;if(!array_at(pools,L.pools,L.pool_stride,&rows))return fail("vanilla terrain pool: registry does not read");
    bool found=false;std::set<uint32_t> members;
    for(uint32_t i=0;i<rows.count;++i) {
        const size_t rec=size_t(rows.base-pools.data())+size_t(i)*L.pool_stride;
        const int32_t rel=rd<int32_t>(pools.data()+rec+L.pool_name);
        const uint32_t len=rd<uint32_t>(pools.data()+rec+L.pool_name+4);
        const int64_t at=int64_t(rec+L.pool_name)+rel;
        if(!rel || at<=0 || uint64_t(at)+len>pools.size())return fail("vanilla terrain pool: unreadable pool name");
        if(std::string(reinterpret_cast<const char*>(pools.data()+at),len)!="TerrainMaterials")continue;
        // FNV of 256c43ba-dd20-4369-b3bf-f49c671a59ec, qualified against three shipped terrain blueprints.
        if(found || rd<uint32_t>(pools.data()+rec+L.pool_id)!=851579436u)return fail("vanilla terrain pool: duplicate row or changed pool identity");
        found=true;ArrayView list;
        if(!array_at(pools,uint32_t(rec+L.pool_materials),4,&list) || !list.count || list.count>kPoolCapacity)
            return fail("vanilla terrain pool: unreadable or oversized material list");
        for(uint32_t n=0;n<list.count;++n) {
            const auto item=rd<uint32_t>(list.base+size_t(n)*4);
            if(!item || !members.insert(item).second)return fail("vanilla terrain pool: null or duplicate material item");
        }
    }
    if(!found)return fail("vanilla terrain pool: TerrainMaterials is unavailable");
    *items=std::move(members);return true;
}
std::map<std::string, PoolGameMaterial> pool_game_materials(const std::vector<GameBlockItem>& items, const Bytes& registry,
                                                              const RegistryInfo& info, const Layouts& L, bool include_tools,
                                                              const std::set<uint32_t>& vanilla_terrain) {
    // The member is the item that places the material: the one the game's own build-menu pools list. Not the config's
    // materialItem: that is the material's loot (Wood Logs for Rough Wood, the Stone terrain item for Rough Stone), and
    // materials share it.
    std::map<std::string, PoolGameMaterial> out;
    for (const auto& it : items) {
        if (!it.item_id) continue;
        const auto terrain = info.terrain_at.find(it.cell), building = info.building_at.find(it.cell);
        if (terrain == info.terrain_at.end() && building == info.building_at.end()) continue;
        if (building == info.building_at.end() && !include_tools) continue;
        const bool roof = building != info.building_at.end() && L.building.roof != UINT32_MAX && registry[building->second + L.building.roof] != 0;
        const PoolGameMaterial m{it.item_id, roof, false, it.cell, vanilla_terrain.count(it.item_id)!=0};
        auto add = [&](const std::string& key) {
            if (key.empty()) return;
            const auto [have, fresh] = out.emplace(key, m);
            if (!fresh && (have->second.item != m.item || have->second.cell != m.cell)) have->second = {0, m.roof, true, -1};
        };
        add(material_name(it.debug_name));
        if (include_tools && terrain != info.terrain_at.end()) { add(it.debug_name); add(std::to_string(it.item_id)); }
    }
    return out;
}
PoolResolution resolve_pools(const std::vector<PoolDefinition>& definitions, const std::vector<Material>& materials,
                             const std::map<std::string, PoolGameMaterial>& game) {
    PoolResolution r;
    std::set<uint32_t> ids;
    std::map<uint32_t,size_t> counts;for(const auto& d:definitions)++counts[item_id_of(d.guid)];
    for(const auto& d:definitions) {
        MaterialPool p{d.guid,d.mod+":"+d.block,d.roof,{}}; p.tool = d.tool;
        bool ok=true;
        std::vector<std::string> problems;  // every one, each a sentence naming its entry: an author fixes a file once
        std::set<Guid> mine;                // what this pool takes out of the game's lists, if it is accepted
        std::map<uint32_t,size_t> members;  // item -> the entry that named it first
        const auto fail=[&](std::string text){ok=false;problems.push_back(std::move(text));};
        const auto entry=[&](size_t i){
            const auto& e=d.entries[i];
            return "materials["+std::to_string(i)+"] ("+(e.material.empty()?"gameMaterial "+e.game_material:"material "+e.material)+")";
        };
        const auto argument=[&](const std::string& text){
            fail(text);
            if(!r.arguments_invalid) r.argument_reason=std::string(d.tool?"block toolMaterialPool ":"block buildingMaterialPool ")+d.block+" (mod "+d.mod+"): "+text;
            r.arguments_invalid=true;
        };
        if(!d.valid) fail("its definition has errors (each named on its own line)");
        if(d.guid.zero()) fail("it has no GUID");
        if(d.entries.empty()) argument("it has no entries; a pool lists 1 to 64 materials");
        else if(d.entries.size()>kPoolCapacity) argument("it has "+std::to_string(d.entries.size())+" entries; a pool lists 1 to 64 materials");
        if(counts[item_id_of(d.guid)]>1) fail("its pool id is another pool's too (two keys or was values that derive one GUID)");
        if(d.entries.size()<=kPoolCapacity) for(size_t i=0;i<d.entries.size();++i) {
            const auto& e=d.entries[i];
            uint32_t item=0; bool roof=false;
            if (d.tool) {
                const auto found = game.find(e.item);
                if (found == game.end() || found->second.ambiguous || !found->second.item)
                    fail("entries[" + std::to_string(i) + "]: no unambiguous placeable terrain item " + e.item);
                else if (found->second.cell < 1 || found->second.cell >= 128 || uint32_t(found->second.cell) != e.terrain_material)
                    fail("entries[" + std::to_string(i) + "]: item does not place the declared terrain material");
                else if (!found->second.vanilla_terrain)
                    argument("entries[" + std::to_string(i) + "].item: " + e.item + " is not a member of the qualified vanilla TerrainMaterials pool; tool pools are policy subsets, not new game rows");
                else if (!e.voxels_per_item || e.voxels_per_item > 1000000)
                    argument("entries[" + std::to_string(i) + "]: voxelsPerItem must be 1..1000000");
                else item = found->second.item;
                if (std::find(p.terrain_materials.begin(), p.terrain_materials.end(), e.terrain_material) != p.terrain_materials.end())
                    argument("entries[" + std::to_string(i) + "]: duplicate terrain material");
                p.terrain_materials.push_back(e.terrain_material); p.voxels_per_item.push_back(e.voxels_per_item);
            }
            else if(!e.material.empty() && !e.game_material.empty()) fail(entry(i)+": it takes exactly one of material and gameMaterial");
            else if(!e.material.empty()) {
                const Material* found=nullptr;
                for(const auto& m:materials) if(pool_ref(e.material,d.mod,m.mod,m.block,m.guid)) { found=&m; break; }
                if(!found) fail(entry(i)+": it names no building material of this mod or of a declared dependency");
                else if(found->terrain) fail(entry(i)+": it is a terrain material; a pool lists building materials");
                else if(found->mod!=d.mod && !d.dependencies.count(found->mod)) fail(entry(i)+": the material belongs to mod "+found->mod+", which mod "+d.mod+" does not declare as a dependency");
                else if(!found->live) fail(entry(i)+": the material is not live this session (its mod is absent, or the material was refused)");
                else if(!found->item || !found->item_id) fail(entry(i)+": the material has no block item");
                else { item=found->item_id; roof=found->roof; mine.insert(found->guid); }
            } else {
                auto it=game.find(e.game_material);
                if(it==game.end()) fail(entry(i)+": the game has no building material of that name");
                else if(it->second.cell > 0 && it->second.cell < 128) fail(entry(i)+": it is a terrain material; a pool lists building materials");
                else if(it->second.ambiguous) fail(entry(i)+": the game has more than one block item for that material");
                else if(!it->second.item) fail(entry(i)+": the game's material has no block item");
                else { item=it->second.item; roof=it->second.roof; }
            }
            if(item && roof!=d.roof) argument(entry(i)+(roof?" is a roof material and the pool is not a roof pool":" is not a roof material and the pool is a roof pool"));
            if(item) {
                const auto [first,fresh]=members.emplace(item,i);
                if(!fresh) argument(entry(first->second)+" and "+entry(i)+" are the same building item "+std::to_string(item));
            }
            p.items.push_back(item);
        }
        ids.insert(item_id_of(d.guid));
        if(ok) {
            // Only an accepted pool takes materials out of the game's lists, and only its own mod's or a declared
            // dependency's: a refused pool, or a reference without a dependency, leaves every material where it was.
            r.grouped.insert(mine.begin(),mine.end());
            r.pools.push_back(std::move(p));
        } else {
            std::string why;
            for(size_t i=0;i<problems.size();++i) why+=(i?"; ":"")+problems[i];
            r.errors.push_back(std::string(d.tool?"block toolMaterialPool ":"block buildingMaterialPool ")+d.block+" (mod "+d.mod+"): refused: "+why);
        }
    }
    return r;
}
void bind_shape_pools(std::vector<Shape>* shapes, const std::vector<PoolDefinition>& definitions,
                      const std::vector<MaterialPool>& pools, std::vector<std::string>* errors) {
    std::vector<Shape> accepted;
    for(auto sh:*shapes) {
        if(sh.material_pool.empty()) { accepted.push_back(std::move(sh)); continue; }
        const PoolDefinition* d=nullptr;
        for(const auto& candidate:definitions) if(pool_ref(sh.material_pool,sh.mod,candidate.mod,candidate.block,candidate.guid)) { d=&candidate; break; }
        const std::string head="block blueprint "+sh.block+" (mod "+sh.mod+"): refused: its materialPool \""+sh.material_pool+"\"";
        std::string why;
        if(!d) why=head+" names no pool of this mod or of a declared dependency";
        else if(d->tool) why=head+" names a tool policy, not a game-visible building material pool";
        else if(d->mod!=sh.mod && !sh.dependencies.count(d->mod)) why=head+" belongs to mod "+d->mod+", which mod "+sh.mod+" does not declare as a dependency";
        else if(d->roof!=sh.roof) why=head+(sh.roof?": the shape is a roof shape and the pool a wall pool; Roof must agree":": the shape is a wall shape and the pool a roof pool; Roof must agree");
        else if(std::none_of(pools.begin(),pools.end(),[&](const auto& p){return p.guid==d->guid;})) why=head+" was refused (its own line says why)";
        if(!why.empty()) { errors->push_back(why); continue; }
        sh.pool=d->guid;
        accepted.push_back(std::move(sh));
    }
    *shapes=std::move(accepted);
}

void fit_pools(std::vector<Material>* live, const std::map<std::string, uint32_t>& game_counts,
               const std::set<Guid>& remembered, std::vector<Material>* held,
               std::vector<std::string>* refused, const std::set<Guid>& grouped) {
    if (grouped.empty()) return fit_pools(live, game_counts, remembered, held, refused);
    std::vector<Material> defaults, custom;
    for (const auto& m : *live)
        (m.terrain || !grouped.count(m.guid) ? defaults : custom).push_back(m);
    fit_pools(&defaults, game_counts, remembered, held, refused);
    defaults.insert(defaults.end(), custom.begin(), custom.end());
    *live = std::move(defaults);
}

bool merge_pools(const Bytes& game, const Layouts& L, const std::vector<Placed>& placed, Bytes* out, std::string* why) {
    ArrayView pools;
    if (!array_at(game, L.pools, L.pool_stride, &pools)) {
        *why = "the material pools don't read as their layout says";
        return false;
    }
    *out = game;
    const auto pool_named = [&](const std::string& want) -> size_t {
        for (uint32_t i = 0; i < pools.count; ++i) {
            const size_t rec = static_cast<size_t>(pools.base - game.data()) + size_t{i} * L.pool_stride;
            const int32_t off = rd<int32_t>(game.data() + rec + L.pool_name);
            const uint32_t len = rd<uint32_t>(game.data() + rec + L.pool_name + 4);
            const int64_t at = int64_t(rec) + L.pool_name + off;
            if (off && at > 0 && static_cast<uint64_t>(at) + len <= game.size() && std::string(reinterpret_cast<const char*>(game.data() + at), len) == want) return rec;
        }
        return SIZE_MAX;
    };
    for (const bool roof : {false, true}) {
        std::vector<uint32_t> ids;
        for (const Placed& p : placed)
            if (p.m.live && p.m.item && p.m.roof == roof) ids.push_back(p.m.item_id);
        if (ids.empty()) continue;
        const size_t rec = pool_named(roof ? "BuildingRoofMaterials" : "BuildingDefaultMaterials");
        if (rec == SIZE_MAX) {
            *why = std::string("the game has no material pool ") + (roof ? "BuildingRoofMaterials" : "BuildingDefaultMaterials");
            return false;
        }
        const size_t field = rec + L.pool_materials;
        ArrayView cur;
        if (!array_at(*out, static_cast<uint32_t>(field), 4, &cur)) {
            *why = "a material pool's list doesn't read";
            return false;
        }
        const uint32_t count = cur.count;
        if (size_t{count} + ids.size() > kPoolCapacity) {
            *why = std::string("the material pool ") + (roof ? kRoofPool : kWallPool) + " would list " + std::to_string(size_t{count} + ids.size()) + " items, past the " +
                   std::to_string(kPoolCapacity) + " a player's game holds";
            return false;
        }
        const size_t cur_at = cur.base ? static_cast<size_t>(cur.base - out->data()) : 0;
        pad4(out);
        const size_t at = out->size();
        out->resize(at + (size_t{count} + ids.size()) * 4);
        if (count) std::memcpy(out->data() + at, out->data() + cur_at, size_t{count} * 4);
        for (size_t i = 0; i < ids.size(); ++i) wr<uint32_t>(out->data() + at + (count + i) * 4, ids[i]);
        wr<int32_t>(out->data() + field, static_cast<int32_t>(at - field));
        wr<uint32_t>(out->data() + field + 4, static_cast<uint32_t>(count + ids.size()));
    }
    return true;
}

namespace {
struct PoolRows { ArrayView rows; std::optional<uint32_t> wall, roof, terrain; };
// The writer's checks of the game's pool table and of the accepted pools, before anything is written.
std::string pool_rows(const Bytes& game, const Layouts& L, const std::vector<MaterialPool>& groups, PoolRows* out) {
    // This is the measured row contract. Do not guess new fields on another executable.
    if (L.pool_stride != 24 || L.pool_slot != 0 || L.pool_id != 4 || L.pool_materials != 8 || L.pool_name != 16)
        return "custom material pools: reflected row layout is unavailable or unsupported";
    ArrayView& rows = out->rows;
    if (!array_at(game, L.pools, L.pool_stride, &rows)) return "custom material pools: registry does not read";
    std::set<uint32_t> ids;
    std::set<std::string> names;
    uint64_t projected = uint64_t(game.size()) + 4;
    for (uint32_t i = 0; i < rows.count; ++i) {
        const size_t rec = size_t(rows.base - game.data()) + size_t(i) * L.pool_stride;
        if (!ids.insert(rd<uint32_t>(game.data() + rec + L.pool_id)).second) return "custom material pools: duplicate existing pool id";
        ArrayView items;
        if (!array_at(game, uint32_t(rec + L.pool_materials), 4, &items) || items.count > kPoolCapacity)
            return "custom material pools: existing pool is unreadable or exceeds 64 entries";
        const int32_t rel = rd<int32_t>(game.data() + rec + L.pool_name);
        const uint32_t len = rd<uint32_t>(game.data() + rec + L.pool_name + 4);
        const int64_t at = int64_t(rec + L.pool_name) + rel;
        if (!rel || at <= 0 || uint64_t(at) + len > game.size()) return "custom material pools: existing name is unreadable";
        const std::string name(reinterpret_cast<const char*>(game.data() + at), len);
        if (!names.insert(name).second) return "custom material pools: duplicate existing pool name";
        if (name == kWallPool) out->wall = rd<uint32_t>(game.data() + rec + L.pool_slot);
        if (name == kRoofPool) out->roof = rd<uint32_t>(game.data() + rec + L.pool_slot);
        if (name == "TerrainMaterials") out->terrain = rd<uint32_t>(game.data() + rec + L.pool_slot);
        projected += 24 + 4 * uint64_t(kPoolCapacity) + 3;  // its row again, and room for every appended item
    }
    for (const auto& g : groups) {
        if (g.guid.zero() || !ids.insert(item_id_of(g.guid)).second) return "custom material pools: null GUID or pool id collision";
        const auto colon = g.name.find(':');
        if (colon == std::string::npos || !valid_name(g.name.substr(0, colon)) || !valid_name(g.name.substr(colon + 1)) || !names.insert(g.name).second)
            return "custom material pools: malformed or duplicate canonical name";
        if (g.items.empty() || g.items.size() > kPoolCapacity) return "custom material pools: expected 1..64 entries";
        std::set<uint32_t> unique(g.items.begin(), g.items.end());
        if (unique.count(0) || unique.size() != g.items.size()) return "custom material pools: null or duplicate item id";
        if (!(g.tool ? out->terrain : g.roof ? out->roof : out->wall)) return "custom material pools: equipment slot unavailable";
        if (g.tool) {
            if(g.terrain_materials.size()!=g.items.size() || g.voxels_per_item.size()!=g.items.size())
                return "tool material policy: entry metadata mismatch";
            std::set<uint32_t> vanilla;std::string why;
            if(!terrain_pool_items(game,L,&vanilla,&why))return why;
            for(size_t i=0;i<g.items.size();++i)
                if(!vanilla.count(g.items[i]) || !g.terrain_materials[i] || g.terrain_materials[i]>=128 || !g.voxels_per_item[i] || g.voxels_per_item[i]>1000000)
                    return "tool material policy: entry is not a valid vanilla TerrainMaterials member";
            continue; // Policy metadata only. Never append a row or repeat the game's magic-stock enumeration.
        }
        projected += 24 + 3 + 4 * uint64_t(g.items.size()) + 3 + g.name.size() + 1;
    }
    if (projected > INT32_MAX) return "custom material pools: resource exceeds relative-offset range";
    return "";
}
}  // namespace

std::string check_custom_pools(const Bytes& game, const Layouts& L, const std::vector<MaterialPool>& groups) {
    PoolRows rows;
    return groups.empty() ? std::string() : pool_rows(game, L, groups, &rows);
}
void admit_custom_pools(const Bytes& game, const Layouts& L, PoolResolution* r) {
    if (r->pools.empty()) return;
    const std::string why = check_custom_pools(game, L, r->pools);
    if (why.empty()) return;
    refuse_custom_pools(r, "this game build's material pool table fails a check of the pool writer (" + why + ")");
}
void refuse_custom_pools(PoolResolution* r, const std::string& reason) {
    for (const auto& p : r->pools) {
        const auto colon = p.name.find(':');
        r->errors.push_back("block buildingMaterialPool " + p.name.substr(colon + 1) + " (mod " + p.name.substr(0, colon) + "): refused: " +
                            reason + "; materials use the ordinary selectors; shapes bound to this pool are refused");
    }
    r->pools.clear();
    r->grouped.clear();
}

bool retain_allocated_pools(PoolResolution* r, const std::vector<Material>& materials, const std::vector<Placed>& placed,
                            const std::map<std::string, PoolGameMaterial>& game) {
    std::set<uint32_t> live_items;
    for (const auto& p : placed) if (p.m.live && p.m.item) live_items.insert(p.m.item_id);
    for (const auto& [name, m] : game) if (m.item) live_items.insert(m.item);
    const auto before = r->pools.size();
    std::erase_if(r->pools, [&](const auto& p) {
        if (std::all_of(p.items.begin(), p.items.end(), [&](uint32_t i) { return live_items.count(i); })) return false;
        const auto colon = p.name.find(':');
        r->errors.push_back("block buildingMaterialPool " + p.name.substr(colon + 1) + " (mod " + p.name.substr(0, colon) + "): refused: a member did not receive a material id");
        return true;
    });
    if (r->pools.size() == before) return false;
    std::set<uint32_t> members;
    for (const auto& p : r->pools) members.insert(p.items.begin(), p.items.end());
    std::set<Guid> grouped;
    for (const auto& m : materials)
        if (r->grouped.count(m.guid) && members.count(m.item_id)) grouped.insert(m.guid);
    r->grouped = std::move(grouped);
    return true;
}

// Budgets from what the game's own bytes held before Kiln's merge: a game update that adds materials or pool
// entries is counted as the game's, not as unknown room.
BudgetCounts material_budget_counts(uint32_t category, uint32_t game_used, uint64_t live, uint64_t retained) {
    BudgetCounts b;
    b.capacity = category == 0 ? 128 : 127;
    b.ready = uint64_t(game_used) + live + retained <= b.capacity;
    if (b.ready) b.game = game_used, b.kiln = uint32_t(live), b.reserved = uint32_t(retained);
    return b;
}
BudgetCounts pool_budget_counts(const std::string&, uint32_t original, uint32_t served) {
    BudgetCounts b;
    b.capacity = kPoolCapacity;
    b.ready = served >= original && served <= kPoolCapacity;
    if (b.ready) b.game = original, b.kiln = served - original;
    return b;
}
bool stored_file_goes(const std::string& stem, const std::string& block, bool owner_loaded, const std::set<std::string>& defined) {
    return owner_loaded && !defined.count(stem.rfind("@pool-",0)==0?"@pool/"+block:block);
}
std::string kept_file_text(const std::string& path) {
    return "stored definition " + path + " kept: its mod is not loaded in this profile, so nothing of it is applied; it applies again when the mod is";
}

bool merge_pools(const Bytes& game, const Layouts& L, const std::vector<Placed>& placed,
                 const std::vector<MaterialPool>& groups, Bytes* out, std::string* why,
                 const std::set<Guid>& grouped) {
    std::vector<Placed> vanilla;
    for (const auto& p : placed)
        if (p.m.terrain || !grouped.count(p.m.guid)) vanilla.push_back(p);
    Bytes next;
    if (const auto bad=check_custom_pools(game,L,groups);!bad.empty()) {*why=bad;return false;}
    if (std::none_of(groups.begin(),groups.end(),[](const auto& g){return !g.tool;})) {
        if (!merge_pools(game, L, vanilla, &next, why)) return false;
        *out = std::move(next);
        return true;
    }
    const auto fail = [&](const std::string& text) { *why = text; return false; };
    PoolRows table;
    if (const std::string bad = pool_rows(game, L, groups, &table); !bad.empty()) return fail(bad);
    const ArrayView& rows = table.rows;
    const std::optional<uint32_t>& wall_slot = table.wall;
    const std::optional<uint32_t>& roof_slot = table.roof;
    std::vector<MaterialPool> sorted;
    for(const auto& group:groups)if(!group.tool)sorted.push_back(group);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.guid.text() < b.guid.text(); });
    if (!merge_pools(game, L, vanilla, &next, why)) return false;
    // Keep the original payload in place; clone the row array and rebase both inner pointers.
    // Bound all resource offsets, including backward references, before converting to int32.
    const uint64_t total = uint64_t(rows.count) + sorted.size();
    if (total > UINT32_MAX || uint64_t(next.size()) + 3 + total*24 > INT32_MAX)
        return fail("custom material pools: resource exceeds relative-offset range");
    const size_t old_rows = rows.count ? size_t(rows.base - game.data()) : 0;
    pad4(&next);
    const size_t at = next.size();
    next.resize(at + size_t(total)*24, 0);
    for (size_t i=0; i<rows.count; ++i) {
        const size_t src=old_rows+i*24, dst=at+i*24;
        std::memcpy(next.data()+dst,next.data()+src,24);
        for (uint32_t field : {L.pool_materials,L.pool_name}) {
            const int32_t rel=rd<int32_t>(next.data()+src+field);
            if (rel) {
                const int64_t rebased=int64_t(src)+rel-int64_t(dst);
                if (rebased<INT32_MIN || rebased>INT32_MAX) return fail("custom material pools: relative-offset overflow");
                wr<int32_t>(next.data()+dst+field,int32_t(rebased));
            }
        }
    }
    for (size_t i=0; i<sorted.size(); ++i) {
        const auto& g=sorted[i]; const size_t rec=at+(rows.count+i)*24;
        wr<uint32_t>(next.data()+rec+L.pool_slot,*(g.roof?roof_slot:wall_slot));
        wr<uint32_t>(next.data()+rec+L.pool_id,item_id_of(g.guid));
        pad4(&next); size_t list=next.size();
        next.resize(list+g.items.size()*4);
        std::memcpy(next.data()+list,g.items.data(),g.items.size()*4);
        wr<int32_t>(next.data()+rec+L.pool_materials,int32_t(list-rec-L.pool_materials));
        wr<uint32_t>(next.data()+rec+L.pool_materials+4,uint32_t(g.items.size()));
        size_t name=next.size(); next.insert(next.end(),g.name.begin(),g.name.end()); next.push_back(0);
        if (next.size()>INT32_MAX) return fail("custom material pools: resource exceeds relative-offset range");
        wr<int32_t>(next.data()+rec+L.pool_name,int32_t(name-rec-L.pool_name));
        wr<uint32_t>(next.data()+rec+L.pool_name+4,uint32_t(g.name.size()));
    }
    wr<int32_t>(next.data()+L.pools,int32_t(at-L.pools));
    wr<uint32_t>(next.data()+L.pools+4,uint32_t(total));
    *out=std::move(next);
    return true;
}

uint32_t first_pool_item(const Bytes& pools, const Layouts& L) {
    ArrayView v;
    if (!array_at(pools, L.pools, L.pool_stride, &v) || !v.count) return 0;
    const size_t rec = static_cast<size_t>(v.base - pools.data());
    ArrayView items;
    if (!array_at(pools, static_cast<uint32_t>(rec + L.pool_materials), 4, &items) || !items.count) return 0;
    return rd<uint32_t>(items.base);
}

bool merge_blueprints(const Bytes& game, const Layouts& L, const std::vector<Placed>& placed, uint32_t template_item, Bytes* out, std::string* why) {
    ArrayView v;
    if (!array_at(game, L.blueprints, L.bp_stride, &v)) {
        *why = "the blueprint items don't read as their layout says";
        return false;
    }
    *out = game;
    std::vector<uint32_t> ids;
    for (const Placed& p : placed)
        if (p.m.live && p.m.item) ids.push_back(p.m.item_id);
    if (ids.empty()) return true;
    const size_t old_at = static_cast<size_t>(v.base - game.data());
    size_t tpl = SIZE_MAX;
    for (uint32_t i = 0; i < v.count; ++i)
        if (rd<uint32_t>(game.data() + old_at + size_t{i} * L.bp_stride + L.bp_item) == template_item) tpl = i;
    if (tpl == SIZE_MAX) {
        *why = "the first pool item has no blueprint entry to start from";
        return false;
    }
    pad4(out);
    const size_t at = out->size();
    const size_t total = size_t{v.count} + ids.size();
    out->resize(at + total * L.bp_stride);
    auto copy = [&](size_t from_index, size_t to_index) {
        const size_t from = old_at + from_index * L.bp_stride, to = at + to_index * L.bp_stride;
        std::memcpy(out->data() + to, out->data() + from, L.bp_stride);
        const int32_t off = rd<int32_t>(out->data() + from + L.bp_data);
        if (off) wr<int32_t>(out->data() + to + L.bp_data, static_cast<int32_t>(int64_t(from) + off - int64_t(to)));
    };
    for (uint32_t i = 0; i < v.count; ++i) copy(i, i);
    for (size_t i = 0; i < ids.size(); ++i) {
        copy(tpl, v.count + i);
        wr<uint32_t>(out->data() + at + (v.count + i) * L.bp_stride + L.bp_item, ids[i]);
    }
    wr<int32_t>(out->data() + L.blueprints, static_cast<int32_t>(at - L.blueprints));
    wr<uint32_t>(out->data() + L.blueprints + 4, static_cast<uint32_t>(total));
    return true;
}

// ---- names -------------------------------------------------------------------------------------------------------
std::string material_name(const std::string& item_debug_name) {
    std::string s = item_debug_name;
    while (!s.empty() && s[0] == '_') s.erase(0, 1);
    std::string prefix;
    const std::string terrain = "Build_TerrainMaterial_", block = "Block_";
    if (s.rfind(terrain, 0) == 0) {
        s.erase(0, terrain.size());
        prefix = "Terrain";
    } else if (s.rfind(block, 0) == 0) {
        s.erase(0, block.size());
    } else {
        return "";
    }
    std::string out = prefix;
    size_t at = 0;
    while (at <= s.size()) {
        const size_t end = std::min(s.find('_', at), s.size());
        std::string tok = s.substr(at, end - at);
        at = end + 1;
        const bool tier = tok.size() >= 2 && tok[0] == 'T' && std::all_of(tok.begin() + 1, tok.end(), [](unsigned char c) { return std::isdigit(c); });
        std::string up = tok;
        std::transform(up.begin(), up.end(), up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (tok.empty() || tier || up == "CRAFTING" || up == "REWARD") continue;
        tok[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(tok[0])));
        out += tok;
    }
    return out;
}

std::string stored_refusal_text(const std::string& path, const std::string& why) {
    const auto detail = why.rfind("unsafe: ", 0) == 0 || why.rfind("unavailable: ", 0) == 0 ? why : "unsafe: " + why;
    return "stored definition " + path + " refused: " + detail + (detail.find("left in place") == std::string::npos ? " (left in place)" : "");
}
std::vector<std::string> merge_error_lines(const std::vector<std::string>& errors, const std::set<std::string>& logged) {
    std::vector<std::string> out;
    for (const auto& e : errors)
        if (!logged.count(e)) out.push_back(e);
    return out;
}

bool base64_decode(const std::string& text, Bytes* out) {
    out->clear();
    uint32_t acc = 0;
    int bits = 0;
    for (const char c : text) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+') v = 62;
        else if (c == '/') v = 63;
        else if (c == '=') break;
        else return false;
        acc = acc << 6 | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out->push_back(static_cast<uint8_t>(acc >> bits));
        }
    }
    return true;
}

bool merge_shapes(const Bytes& table, const Layouts& L, const std::vector<Shape>& shapes, Bytes* out, std::vector<std::string>* refused, std::string* why) {
    ArrayView v;
    if (!array_at(table, L.blueprints, L.bp_stride, &v)) {
        *why = "the blueprint items don't read as their layout says";
        return false;
    }
    *out = table;
    std::set<uint32_t> have;
    const size_t old_at = v.base ? static_cast<size_t>(v.base - table.data()) : 0;
    for (uint32_t i = 0; i < v.count; ++i) have.insert(rd<uint32_t>(table.data() + old_at + size_t{i} * L.bp_stride + L.bp_item));
    std::vector<const Shape*> in;
    for (const Shape& s : shapes) {
        if (!have.insert(s.item_id).second) {
            refused->push_back("block blueprint " + s.block + " (mod " + s.mod + "): its item's id is taken in the shapes table: two shapes with one name?");
            continue;
        }
        in.push_back(&s);
    }
    if (in.empty()) return true;
    // the entries: the table's, then the shapes'
    pad4(out);
    const size_t at = out->size();
    const size_t total = size_t{v.count} + in.size();
    out->resize(at + total * L.bp_stride);
    for (uint32_t i = 0; i < v.count; ++i) {
        const size_t from = old_at + size_t{i} * L.bp_stride, to = at + size_t{i} * L.bp_stride;
        std::memcpy(out->data() + to, out->data() + from, L.bp_stride);
        const int32_t off = rd<int32_t>(out->data() + from + L.bp_data);
        if (off) wr<int32_t>(out->data() + to + L.bp_data, static_cast<int32_t>(int64_t(from) + off - int64_t(to)));
    }
    // each shape's cells after the entries
    for (size_t k = 0; k < in.size(); ++k) {
        const Shape& s = *in[k];
        const size_t cells_at = out->size();
        out->insert(out->end(), s.bits.begin(), s.bits.end());
        const size_t e = at + (v.count + k) * L.bp_stride;
        std::memset(out->data() + e, 0, L.bp_stride);
        wr<uint32_t>(out->data() + e + L.bp_item, s.item_id);
        wr<uint32_t>(out->data() + e + L.bp_size, s.x);
        wr<uint32_t>(out->data() + e + L.bp_size + 4, s.y);
        wr<uint32_t>(out->data() + e + L.bp_size + 8, s.z);
        wr<int32_t>(out->data() + e + L.bp_data, static_cast<int32_t>(int64_t(cells_at) - int64_t(e + L.bp_data)));
        wr<uint32_t>(out->data() + e + L.bp_data + 4, static_cast<uint32_t>(s.bits.size()));
        (*out)[e + L.bp_compressed] = s.terrain_brush ? 0 : 1;
        pad4(out);
    }
    wr<int32_t>(out->data() + L.blueprints, static_cast<int32_t>(at - L.blueprints));
    wr<uint32_t>(out->data() + L.blueprints + 4, static_cast<uint32_t>(total));
    return true;
}

Bytes voxel_model(const Shape& s, const ModelLayout& M) {
    Bytes b(M.size, 0);
    wr<uint32_t>(b.data() + M.size_field, s.x);
    wr<uint32_t>(b.data() + M.size_field + 4, s.y);
    wr<uint32_t>(b.data() + M.size_field + 8, s.z);
    b[M.terrain] = s.terrain_brush ? 1 : 0;
    pad4(&b);
    const size_t at = b.size();
    const uint32_t n = s.x * s.y * s.z;
    b.resize(at + n, 0);
    for (uint32_t i = 0; i < n; ++i)
        if (s.terrain_brush) b[at + i] = s.bits[i];
        else if ((s.bits[i / 8] >> (i % 8)) & 1) b[at + i] = s.roof ? kRoofCell : kFullCell;
    wr<int32_t>(b.data() + M.data, static_cast<int32_t>(at - M.data));
    wr<uint32_t>(b.data() + M.data + 4, n);
    return b;
}

}  // namespace kwt
