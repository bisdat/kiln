// SPDX-License-Identifier: MIT
// 16f S3 authored-body admission (bodyadmit.h). No resource is published here.
#include "bodyadmit.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

namespace kiln::qb::bodyadmit {
namespace {
using Json = nlohmann::json;  // key-order-free comparison of the cooker's sorted JSON and the authored blocks

bool hex64(const ojson& value) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
std::string digest(const Bytes& bytes) {
    const char* data = bytes.empty() ? "" : reinterpret_cast<const char*>(bytes.data());
    return sha256_hex(std::string_view(data, bytes.size()));
}
const ojson& member(const ojson& object, const char* key) {
    static const ojson null;
    const auto it = object.find(key);
    return it == object.end() ? null : *it;
}
std::string text(const ojson& object, const char* key) {
    const ojson& value = member(object, key);
    return value.is_string() ? value.get<std::string>() : std::string();
}

// The local cook cache, read and checked once per mod: its sources and cooked outputs still hash as the cook recorded.
struct Cache {
    ojson doc;
    std::string source_hash;
    ojson entries = ojson::array();  // the kiln.assets entries (paths relative to assets/) this machine publishes
    std::set<std::string> files;     // every cooked output file the cache lists (mod-relative)
};
bool load_cache(const Env& env, Cache* cache, std::string* why) {
    const auto fail = [&](const std::string& detail) { *why = std::string(kCache) + ": " + detail; return false; };
    Bytes raw;
    std::string reason;
    if (!env.read || !env.read(kCache, kMaxCache, &raw, &reason))
        return fail("this machine has no readable cook of the mod's authored bodies (" + reason + "); cook the mod");
    // a cooked file like any other: kiln.assets' verdict from this game's cook receipts and kiln.json "cooked" (review3 S4)
    if (!env.cooked) return fail("kiln.assets cannot check whether this game's cook made it; no body is built from it");
    switch (env.cooked(raw)) {
    case 0:
        break;
    case 1:
        if (env.warn) env.warn(std::string(kCache) + " was not made by this game's cook: loaded (kiln.json \"cooked\": \"warn\"); cook the mod here");
        break;
    default:
        return fail("not made by this game's cook, so it is not loaded (kiln.json \"cooked\": \"local\"); cook the mod here");
    }
    cache->doc = ojson::parse(raw.begin(), raw.end(), nullptr, false);
    const ojson& doc = cache->doc;
    if (doc.is_discarded() || !doc.is_object() || member(doc, "format") != 1 || !hex64(member(doc, "sourceHash")) ||
        !member(doc, "sources").is_array() || !member(doc, "entries").is_array() || !member(doc, "bodies").is_object())
        return fail("not a format-1 body cache with sourceHash, sources, entries and bodies; cook the mod again");
    cache->source_hash = doc["sourceHash"].get<std::string>();
    if (doc["sources"].size() > 4096 || doc["entries"].size() > 4096) return fail("too many sources or entries");
    // Authored sources (model, clips, glTF buffers and images, material blocks and their textures): the same bytes on
    // a server and a client, so a changed source means the cook is stale on this machine.
    for (const auto& row : doc["sources"]) {
        const std::string file = row.is_object() ? text(row, "file") : std::string();
        if (!row.is_object() || !contained(file) || !hex64(member(row, "sha256"))) return fail("a source row is malformed; cook the mod again");
        Bytes bytes;
        if (!env.read(file, kMaxFile, &bytes, &reason)) return fail(file + ": " + reason + "; cook the mod again");
        if (digest(bytes) != row["sha256"].get<std::string>()) return fail(file + " changed after the last cook; cook the mod again");
    }
    std::set<std::string> blobs;
    for (const auto& entry : doc["entries"]) {
        if (!entry.is_object()) return fail("an entry is not an object; cook the mod again");
        const std::string kind = text(entry, "kind"), file = text(entry, "file");
        if ((kind != "resource" && kind != "blob") || !contained(file) || file.rfind("assets/cooked/", 0) != 0 || !member(entry, "producer").is_string())
            return fail("an entry is not a cooked resource or blob of this mod; cook the mod again");
        if (text(entry, "sourceHash") != cache->source_hash) return fail(file + " belongs to an older cook of the bodies; cook the mod again");
        if (kind == "resource" && (text(entry, "mode") != "new" || text(entry, "type").empty() || text(entry, "guid").empty()))
            return fail(file + ": a body resource must be a new keyed resource");
        cache->files.insert(file);
        if (kind == "blob") blobs.insert(file);
        const bool skipped = member(entry, "skippedOnServer") == true;
        if (skipped && env.client) return fail(file + " was skipped by a dedicated server's cook; cook the mod on this game client");
        if (!skipped) {
            if (!hex64(member(entry, "sha256"))) return fail(file + " has no recorded hash; cook the mod again");
            Bytes bytes;
            if (!env.read(file, kMaxFile, &bytes, &reason)) return fail(file + ": " + reason + "; cook the mod again");
            if (digest(bytes) != entry["sha256"].get<std::string>()) return fail(file + " changed after the last cook; cook the mod again");
        }
        if (skipped) continue;  // nothing was made here; a dedicated server draws no model
        ojson out = {{"kind", kind}, {"file", file.substr(std::strlen("assets/"))}};
        if (kind == "resource") {
            out["guid"] = entry["guid"];
            out["type"] = entry["type"];
            out["part"] = entry.value("part", 0);
            out["mode"] = "new";
            ojson patches = ojson::array();
            for (const auto& patch : member(entry, "patch").is_array() ? entry["patch"] : ojson::array()) {
                const std::string blob = patch.is_object() ? text(patch, "blob") : std::string();
                if (!contained(blob) || blob.rfind("assets/cooked/", 0) != 0 || !member(patch, "offset").is_number_unsigned())
                    return fail(file + ": a patch must name a cooked blob of this mod and an offset");
                patches.push_back({{"offset", patch["offset"]}, {"blob", blob.substr(std::strlen("assets/"))}});
            }
            if (!patches.empty()) out["patch"] = std::move(patches);
        }
        cache->entries.push_back(std::move(out));
    }
    // A patch's blob is itself one of the cook's checked outputs, never another file.
    for (const auto& entry : cache->entries)
        for (const auto& patch : member(entry, "patch").is_array() ? entry["patch"] : ojson::array())
            if (!blobs.count("assets/" + patch["blob"].get<std::string>())) return fail(text(entry, "file") + ": its patch blob is not a cooked body blob");
    return true;
}

// The cook's RenderModel entry for this body: <out>/<model name, '/' as '_'>.bin (tools/modelcook steps.model_entries).
bool model_entry(const Cache& cache, const ojson& spec, Guid* guid, std::string* why) {
    const std::string stem = [&] { std::string s = text(spec, "modelName"); std::replace(s.begin(), s.end(), '/', '_'); return s; }();
    size_t found = 0;
    for (const auto& entry : cache.doc["entries"]) {
        const std::string file = text(entry, "file");
        if (text(entry, "kind") != "resource" || text(entry, "type") != kModelType || stem.empty() ||
            file.size() < stem.size() + 5 || file.compare(file.size() - stem.size() - 5, std::string::npos, "/" + stem + ".bin") != 0)
            continue;
        if (!parse_guid(text(entry, "guid"), guid) || guid->zero()) { *why = file + ": the cooked model has no GUID"; return false; }
        ++found;
    }
    if (found != 1) { *why = "the cook lists " + std::to_string(found) + " RenderModel entries for model " + text(spec, "modelName") + ", not one"; return false; }
    return true;
}

bool admit_block(const ModInput& mod, const Env& env, const std::string& name, ojson* body, Cache* cache, bool* cache_read,
                 std::string* why) {
    const auto fail = [&](const std::string& detail) { *why = "npcs." + name + ": " + detail; return false; };
    if (!*cache_read) {
        if (!load_cache(env, cache, why)) { *why = "npcs." + name + ": " + *why; return false; }
        *cache_read = true;
    }
    const ojson& cooked = member(cache->doc["bodies"], name.c_str());
    if (!cooked.is_object()) return fail(std::string(kCache) + " has no body for this NPC; cook the mod");
    if (text(cooked, "sourceHash") != cache->source_hash) return fail("its cooked body belongs to an older cook; cook the mod again");
    const ojson& spec = member(cooked, "spec");
    const ojson& wanted = member(*body, "spec");
    if (!spec.is_object() || !wanted.is_object()) return fail("its cooked body has no spec; cook the mod again");
    for (const char* part : {"body", "animation", "motions"})
        if (Json::parse(member(spec, part).dump()) != Json::parse(member(wanted, part).dump()))
            return fail(std::string(part) + " changed after the last cook; cook the mod again");
    Guid graph, model;
    if (!parse_guid(text(cooked, "graph"), &graph) || graph.zero()) return fail("the cook made no graph for it; cook the mod again");
    const ojson& bindings = member(cooked, "bindings");
    if (!bindings.is_array() || bindings.size() != 27 || !std::all_of(bindings.begin(), bindings.end(), [](const ojson& b) { return b.is_object(); }))
        return fail("the cook bound no complete motion vector (27 rows); cook the mod again");
    std::string reason;
    if (!model_entry(*cache, spec, &model, &reason)) return fail(reason);
    if (env.client) {
        Guid recorded;
        if (member(cooked, "complete") != true) return fail("its cook is not complete on this game client; cook the mod again");
        if (!parse_guid(text(cooked, "model"), &recorded) || !(recorded == model)) return fail("its cooked model is not the cook's RenderModel; cook the mod again");
        if (!check_body_materials(mod.id, member(cooked, "materialBlocks"), env.materials, &reason)) return fail(reason);
    }
    // The graph must be one of this mod's own cooked resources: never a foreign or game graph by accident.
    bool own_graph = false;
    for (const auto& entry : cache->doc["entries"]) {
        Guid g;
        own_graph |= text(entry, "kind") == "resource" && text(entry, "type") == kGraphType && parse_guid(text(entry, "guid"), &g) && g == graph;
    }
    if (!own_graph) return fail("its graph is not one of the cook's body resources; cook the mod again");
    const ojson size = member(member(wanted, "body"), "size");
    *body = {{"model", guid_text(model)}, {"graph", guid_text(graph)}, {"size", size}, {"bindings", bindings},
             {"closure", cache->source_hash}, {"block", name}};
    return true;
}

}  // namespace

bool contained(const std::string& file) {
    if (file.empty() || file.size() > 1024 || file.front() == '/' || file.find('\\') != std::string::npos ||
        file.find('\0') != std::string::npos || file.find(':') != std::string::npos)
        return false;
    for (size_t at = 0;;) {
        const size_t end = file.find('/', at);
        const std::string part = file.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (end == std::string::npos) return true;
        at = end + 1;
    }
}

ojson resolve_spec(const std::string& mod, const ojson& npc, const ojson& bodies, const ojson& animations, const ojson& motions) {
    const auto part = [&](const char* field, const ojson& section) -> ojson {
        const ojson& value = member(npc, field);
        if (value.is_object()) return value;
        if (!value.is_string()) return nullptr;
        std::string local = value.get<std::string>();
        if (const size_t colon = local.find(':'); colon != std::string::npos) {
            if (local.substr(0, colon) != mod) return nullptr;  // another mod's part: refused by the managed definition
            local = local.substr(colon + 1);
        }
        const ojson& found = member(section, local.c_str());
        return found.is_object() ? found : ojson(nullptr);
    };
    ojson body = part("body", bodies);
    if (!body.is_object() || !body.contains("model")) return nullptr;
    return {{"body", body}, {"animation", part("animation", animations)}, {"motions", part("motions", motions)}};
}

bool admit(ModInput* mod, const Env& env, ojson* assets, std::string* why) {
    *assets = {{"entries", ojson::array()}};
    Cache cache;
    bool cache_read = false, layout_checked = false, from_blocks = false;
    try {
        for (auto& document : mod->documents) {
            if (!document.is_object() || !member(document, "npcs").is_object()) continue;
            for (auto& [name, npc] : document["npcs"].items()) {
                if (!npc.is_object() || !npc.contains("body")) continue;
                ojson& body = npc["body"];
                if (!body.is_object()) { *why = "npcs." + name + ": body must be an object"; return false; }
                if (!layout_checked) {
                    std::string reason;
                    if (!env.layout || !env.layout(&reason)) {
                        *why = "npcs." + name + ": " + (reason.empty() ? "the game's type registry cannot be checked" : reason);
                        return false;
                    }
                    layout_checked = true;
                }
                if (!body.contains("block")) {  // review3 M2: no cooked source identity and no G2 material check for these in v1
                    *why = "npcs." + name + ": " + kCodeBody;
                    return false;
                }
                if (!admit_block(*mod, env, name, &body, &cache, &cache_read, why)) return false;
                from_blocks = true;
            }
        }
    } catch (const std::exception& error) {
        *why = std::string("authored body admission failed: ") + error.what();
        return false;
    }
    if (from_blocks) (*assets)["entries"] = std::move(cache.entries);
    why->clear();
    return true;
}
}  // namespace kiln::qb::bodyadmit
