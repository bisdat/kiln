// SPDX-License-Identifier: MIT
// Stable entry authoring lowers into kiln.content's existing definition/placeholder/sync path.
#include "registry_live.h"
#include <algorithm>
#include <cstdio>

namespace kc {
registry::json registry_entry_definition(const registry::EntryRef& ref, const registry::EntryDefinition& d,
                                         const RegistryLive& in, std::string* why) {
    using registry::json;
    auto fail = [&](const std::string& reason) -> json { *why = reason; return nullptr; };
    if (ref.registry != "items" && ref.registry != "recipes") return fail(ref.registry + " does not support entry add");
    if (!in.schema || !in.regmap || !in.added_guid) return fail("the entry definition backend is unavailable");
    if (ref.id.rfind(d.mod + ":", 0) != 0) return fail("an added entry belongs to its bound mod " + d.mod);
    if (d.like.empty() || d.base.rfind("game:", 0) != 0) return fail("an entry add needs like: a present Kiln entry whose clone chain ends at a game entry");
    const auto base = in.ids.find(ref.registry + "|" + d.base);
    if (base == in.ids.end() || base->second.empty()) return fail("like " + d.base + " has no base GUID on this build");
    const std::string guid = in.added_guid(ref.registry, ref.id);
    uint8_t bytes[16]{};
    if (!guid_parse(guid, bytes)) return fail("the stable entry GUID is invalid");
    const uint32_t num = fnv1a(bytes, 16);
    if (!num) return fail("the stable entry GUID hashes to reserved id zero");
    for (const auto& [key, value] : in.nums) {
        if (!key.starts_with(ref.registry + "|") || value != num) continue;
        const auto old = in.ids.find(key);
        if (key != ref.registry + "|" + ref.id || old == in.ids.end() || old->second != guid)
            return fail("stable entry id collision with " + key.substr(ref.registry.size() + 1) + "; choose a new Kiln id");
    }
    std::string name = "Kiln_Registry_" + ref.registry + "_" + guid;
    std::erase(name, '-');
    json body = {{"guid", guid}, {"name", name}, {"base", base->second}, {"registryId", ref.id},
                 {"registryKind", ref.registry}, {"registryFields", json::object()}};
    std::set<std::string> dependencies;
    if (!d.like.starts_with("game:")) dependencies.insert(ref.registry + "|" + d.like);
    const auto* spec = in.schema->registry(ref.registry);
    if (!spec) return fail("unknown registry " + ref.registry);
    for (const auto& [field, value] : d.values) {
        const auto* f = spec->field(field);
        if (!f || f->read_only || field.find('[') != std::string::npos || value.kind == registry::Value::None) continue;
        std::string reason;
        // An inherited unproven field stays as the base's bytes. Explicit unproven writes were already refused by the stage.
        if (in.regmap->mapping(ref.registry, field, &reason) != registry::map::Mapping::Mapped) continue;
        body["registryFields"][field] = value.to_json();
    }
    if (ref.registry == "items") body["rolledLoot"] = true; // the stable effective values, not newcontent's gear defaults
    else {
        for (const char* list : {"inputs", "outputs"}) {
            const auto value = d.values.find(list);
            if (value == d.values.end() || value->second.kind != registry::Value::List || value->second.list.empty())
                return fail(std::string("recipes.") + list + " needs at least one stack for an added recipe");
            json stacks = json::array();
            for (const auto& stack : value->second.list) {
                json out = json::object();
                const auto* item = stack.leaf("item");
                const auto* category = stack.leaf("category");
                const auto* count = stack.leaf("count");
                if (!count || count->kind != registry::Value::Int || count->i < 1 || count->i > 65535)
                    return fail(std::string("recipes.") + list + " has an invalid stack count");
                out["count"] = count->i;
                if (item && item->kind == registry::Value::Text) {
                    auto found = in.ids.find("items|" + item->t);
                    if (found == in.ids.end() || found->second.empty()) return fail("item " + item->t + " has no definition GUID");
                    out["item"] = found->second;
                    if (!item->t.starts_with("game:")) dependencies.insert("items|" + item->t);
                } else if (list[0] == 'i' && category && category->kind == registry::Value::Text) {
                    const auto found = in.nums.find("inputCategories|" + category->t);
                    if (found == in.nums.end() || !found->second) return fail("input category " + category->t + " is unknown");
                    char hex[16]; std::snprintf(hex, sizeof hex, "0x%08x", found->second); out["category"] = hex;
                } else return fail(std::string("recipes.") + list + " needs an item or input category");
                stacks.push_back(std::move(out));
            }
            body[list] = std::move(stacks);
        }
        if (auto it = d.values.find("duration"); it != d.values.end()) body["duration"] = it->second.to_json();
        const auto unlock = d.values.find("unlock");
        body["unlock"] = unlock != d.values.end() && unlock->second.t == "always" ? "always" : "base";
        // Recipe construction already owns these lists/values, including during early registration before RL is populated.
        for (const char* field : {"inputs", "outputs", "duration", "unlock"}) body["registryFields"].erase(field);
    }
    if (!dependencies.empty()) body["registryDependencies"] = dependencies;
    return body;
}

namespace {
void refuse_definition(RegistryLive& in, const registry::EntryRef& ref, const std::string& why) {
    const auto d = in.result.definitions.at(ref);
    registry::Refusal r{d.mod, d.source, registry::Stage::Add, d.n, ref.registry + " " + ref.id + " refused: " + why,
                        ref.registry + "|" + ref.id, ref.registry, ref.id, ""};
    for (auto& mod : in.mods) {
        if (mod.id != d.mod) continue;
        mod.refused.push_back(r);
        for (auto& batch : mod.batches) std::erase_if(batch.edits, [&](const auto& e) {
            return e.entry == registry::EntryOp::Add && e.registry == ref.registry && e.add_id == ref.id;
        });
    }
    in.pre_refused.push_back(r);
}
std::map<registry::EntryRef, KilnOwner> entry_owners; // process-owned definitions; resources-ready thread, then store_mutex
}

void registry_entries_prepare(RegistryLive* target) {
    auto& in = *target;
    const auto base_ids = in.ids;
    const auto base_nums = in.nums;
    std::set<registry::EntryRef> defined;
    for (;;) {
        in.ids = base_ids; in.nums = base_nums;
        for (const auto& [ref, definition] : in.result.definitions) {
            if (!in.added_guid) continue;
            const std::string guid = in.added_guid(ref.registry, ref.id);
            uint8_t bytes[16]{};
            if (!guid_parse(guid, bytes)) continue;
            in.ids[ref.registry + "|" + ref.id] = guid;
            in.nums[ref.registry + "|" + ref.id] = fnv1a(bytes, 16);
        }
        std::map<registry::EntryRef, std::string> failures;
        std::map<registry::EntryRef, registry::json> bodies;
        for (const auto& [ref, definition] : in.result.definitions) {
            std::string why;
            auto body = registry_entry_definition(ref, definition, in, &why);
            if (body.is_null()) failures[ref] = why;
            else if (!in.pin_definition || !in.pin_definition(ref, body, &why))
                failures[ref] = why.empty() ? "entry compatibility storage is unavailable" : why;
            else bodies[ref] = std::move(body);
        }
        if (failures.empty()) {
            for (const auto& ref : in.result.added) {
                const auto& body = bodies.at(ref); // stage order, so a clone's like entry is queued before its dependents
                if (defined.count(ref)) continue;
                std::string why;
                if (!in.define_entry || !in.define_entry(ref, body, &why))
                    failures[ref] = why.empty() ? "the content definition backend is unavailable" : why;
                else defined.insert(ref);
            }
        }
        if (failures.empty()) { in.entry_bodies = std::move(bodies); break; }
        for (const auto& [ref, why] : failures) refuse_definition(in, ref, why);
        in.result = registry::compose(*in.schema, in.base, in.old, in.mods);
        for (const auto& [ref, field] : in.boot_fields) in.result.fields[ref] = field;
        in.result.refused.insert(in.result.refused.begin(), in.pre_refused.begin(), in.pre_refused.end());
        for (auto it = defined.begin(); it != defined.end();) {
            if (in.result.definitions.count(*it)) { ++it; continue; }
            if (in.forget_entry) in.forget_entry(*it);
            it = defined.erase(it);
        }
    }
}

bool registry_entry_define_native(const registry::EntryRef& ref, const registry::json& body, std::string* why) {
    std::lock_guard lock(store_mutex());
    auto& owner = entry_owners[ref];
    if (!owner) {
        const std::string label = "registry:" + ref.registry + ":" + ref.id;
        owner = g_api->owner_create(KilnStr{label.data(), label.size()});
        if (!owner) return *why = "the entry definition owner could not be created", false;
        set_owner_label(owner, "mod " + ref.id.substr(0, ref.id.find(':')));
    }
    std::string report;
    const auto rc = nc_define(owner, ref.registry == "items" ? KILN_CONTENT_ITEMS : KILN_CONTENT_RECIPES,
                              body.dump(), false, "registry:" + ref.id, &report);
    const auto parsed = registry::json::parse(report, nullptr, false);
    if (rc != KILN_OK) return *why = parsed.is_object() ? parsed.value("error", "content definition refused") : "content definition refused", false;
    return true;
}
void registry_entry_forget_native(const registry::EntryRef& ref) {
    std::lock_guard lock(store_mutex());
    if (auto it = entry_owners.find(ref); it != entry_owners.end()) nc_forget_unbound(it->second);
}
} // namespace kc
