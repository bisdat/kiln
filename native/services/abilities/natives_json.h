// SPDX-License-Identifier: MIT
// kiln.abilities: a mod's impact/natives.json (schema/impact-natives.schema.json, format 1) read at the service's
// on_ready, before any program is verified. Portable (host-tested in tests/natives_test.cpp).
//  - An entry with "sync": true is a typed declaration: every argument's "type" (one impact type name, or "any") and
//    "size" are binding; the verifier refuses a program whose variables do not fit (E_ARGTYPE), the thunk checks
//    every call (0x2C, 0x18). The generator of [ImpactNative] methods writes such entries.
//  - Any other entry declares the argc only (what an older file means): its argument types stay notes for the cook.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nlohmann/json.hpp"
#include "services/kiln_abilities.h"

namespace kab {

constexpr size_t kNativesJsonBytes = 256 * 1024;

struct NativeDecl {
    std::string name;
    uint32_t hash = 0;
    uint32_t argc = 0;
    bool typed = false;
    std::vector<KilnNativeArg> args;  // typed only: args[0..argc)
};

inline uint32_t fnv1a_text(std::string_view s) {
    uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193u;
    return h;
}

// The size of an impact value type by name (tools/impact/data/types.json), 0 when Kiln does not know it.
inline uint16_t impact_type_size(std::string_view t) {
    struct T { const char* name; uint16_t size; };
    static constexpr T kSizes[] = {
        {"sint32", 4}, {"uint32", 4}, {"float32", 4}, {"bool", 1}, {"Time", 8}, {"ecs.EntityId", 4},
        {"WorldTransform", 56}, {"float3", 12}, {"float2", 8}, {"float4", 16}, {"impact.DamageSetup", 52},
        {"uint8", 1}, {"sint8", 1}, {"uint16", 2}, {"sint16", 2}, {"uint64", 8}, {"sint64", 8},
    };
    for (const auto& k : kSizes)
        if (t == k.name) return k.size;
    return 0;
}

// Parses one file of mod `mod_id`. Entries that are not the mod's own ("<mod_id>.<Name>"), that name no valid native or
// whose hash is not FNV-1a of the name are skipped with a note. Throws std::runtime_error when the file is not a
// natives.json at all (not JSON, too large, another format).
inline std::vector<NativeDecl> parse_natives_json(std::string_view text, std::string_view mod_id, std::vector<std::string>* notes) {
    if (text.size() > kNativesJsonBytes) throw std::runtime_error("impact/natives.json exceeds 256 KiB");
    const auto doc = nlohmann::json::parse(text, nullptr, true, false);
    if (!doc.is_object()) throw std::runtime_error("impact/natives.json is not a JSON object");
    if (doc.contains("format") && doc["format"] != 1) throw std::runtime_error("impact/natives.json: format is not 1");
    std::vector<NativeDecl> out;
    if (!doc.contains("natives")) return out;
    if (!doc["natives"].is_array()) throw std::runtime_error("impact/natives.json: \"natives\" is not an array");
    auto note = [&](const std::string& s) { if (notes) notes->push_back(s); };
    const std::string prefix = std::string(mod_id) + ".";
    for (const auto& n : doc["natives"]) {
        if (!n.is_object() || !n.contains("name") || !n["name"].is_string()) { note("an entry without a name"); continue; }
        NativeDecl d;
        d.name = n["name"].get<std::string>();
        if (d.name.rfind(prefix, 0) != 0) { note(d.name + ": not this mod's (" + prefix + "<Name>)"); continue; }
        d.hash = fnv1a_text(d.name);
        if (n.contains("hash")) {
            uint32_t h = 0;
            const auto& v = n["hash"];
            try {
                if (v.is_number_unsigned()) h = v.get<uint32_t>();
                else if (v.is_string()) h = static_cast<uint32_t>(std::stoul(v.get<std::string>(), nullptr, 0));
            } catch (const std::exception&) { h = 0; }
            if (h != d.hash) { note(d.name + ": its hash is not FNV-1a-32 of the name"); continue; }
        }
        const auto* args = n.contains("args") && n["args"].is_array() ? &n["args"] : nullptr;
        if (n.contains("argc") && n["argc"].is_number_unsigned()) d.argc = n["argc"].get<uint32_t>();
        else d.argc = args ? static_cast<uint32_t>(args->size()) : 0;
        if (d.argc > 31) { note(d.name + ": argc over 31"); continue; }
        d.typed = n.contains("sync") && n["sync"].is_boolean() && n["sync"].get<bool>();
        if (d.typed) {
            const size_t count = args ? args->size() : 0;
            if (count != d.argc) { note(d.name + ": \"sync\" needs one \"args\" entry per argument"); continue; }
            bool bad = false;
            for (size_t i = 0; i < count && !bad; ++i) {
                const auto& a = (*args)[i];
                KilnNativeArg k{};
                std::string type = "any";
                if (a.contains("type") && a["type"].is_string()) type = a["type"].get<std::string>();
                else if (a.contains("types") && a["types"].is_array() && a["types"].size() == 1 && a["types"][0].is_string())
                    type = a["types"][0].get<std::string>();
                k.type = type == "any" ? 0 : fnv1a_text(type);
                k.size = type == "any" ? 0 : impact_type_size(type);
                if (a.contains("size")) {
                    if (!a["size"].is_number_unsigned() || a["size"].get<uint64_t>() > 0xFFFF) { bad = true; break; }
                    k.size = static_cast<uint16_t>(a["size"].get<uint32_t>());
                }
                const std::string dir = a.contains("dir") && a["dir"].is_string() ? a["dir"].get<std::string>() : "in";
                k.dir = dir == "out" ? KILN_NATIVE_ARG_OUT : dir == "inout" ? KILN_NATIVE_ARG_INOUT : KILN_NATIVE_ARG_IN;
                if (a.contains("optional") && a["optional"].is_boolean() && a["optional"].get<bool>()) k.flags |= KILN_NATIVE_ARG_OPTIONAL;
                if (a.contains("raw") && a["raw"].is_boolean() && a["raw"].get<bool>()) k.flags |= KILN_NATIVE_ARG_RAW;
                d.args.push_back(k);
            }
            if (bad) { note(d.name + ": an argument's \"size\" is not 0..65535"); continue; }
        }
        out.push_back(std::move(d));
    }
    return out;
}

}  // namespace kab
