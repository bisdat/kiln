// SPDX-License-Identifier: MIT
// kiln.content: the registry's mapping check (review2 N7): the stage (stage.cpp refuse_unmapped) and the live back-end
// (registry_live.cpp, when no stage check is installed) refuse the same edits for the same reasons. Header-only so the
// live back-end's host tests run production's guard without the stage.
#pragma once
#include <string>
#include <vector>

#include "ir.h"
#include "regmap.h"

namespace kc::registry {

inline std::string unmapped_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return s;
}

// Every edit of b naming a field this build has no verified mapping for, and every entry add/remove (no back-end in this
// release), is moved from b into *refused with its reason; the rest stays in b in order.
inline void refuse_unmapped_edits(const map::RegMap& rm, const Schema& schema, Batch* b, std::vector<Refusal>* refused, bool entry_backend = false) {
    std::vector<Edit> kept;
    for (Edit& e : b->edits) {
        std::string reason;
        auto unmapped = [&](const std::string& field) {
            if (field.empty() || is_raw_field(field)) return false;
            const RegistrySpec* rs = schema.registry(e.registry);
            const FieldSpec* f = rs ? rs->field(field) : nullptr;
            if (!f || f->read_only) return false;  // compose names an unknown or read-only field itself
            return rm.mapping(e.registry, generic_path(field), &reason) != map::Mapping::Mapped;
        };
        bool bad = false;
        // review finding 1 (ruling 2026-11-21): no back-end removes a game entry or creates a mod's in this release (P2 builds it):
        // every entry add and remove is refused here, before composition, so nothing is reported as done and nothing is removed
        // with it (a recipe needing a removed item stays); a reference to a refused add names this refusal
        if (e.entry != EntryOp::None && !entry_backend) {
            bad = true;
            reason = e.registry + " " + (e.entry == EntryOp::Add ? "add " + e.add_id : "remove " + e.select.id) +
                     ": entry add and remove have no back-end in this release (no game entry is removed and no new entry is created); this edit was not applied";
        } else if (e.entry == EntryOp::Add) {
            if ((e.registry != "items" && e.registry != "recipes") || e.like.empty()) {
                bad = true; reason = e.registry + " add requires a supported registry and an explicit like Kiln id";
            }
            for (const auto& [k, v] : e.add_fields) {
                if (bad) break;
                bad = unmapped(k);
                const auto* rs = schema.registry(e.registry);
                const auto* field = rs ? rs->field(k) : nullptr;
                if (!bad && field && field->element == FieldType::Struct && v.kind == Value::List)
                    for (const auto& element : v.list) for (const auto& leaf : element.keys) {
                        if (bad) break;
                        bad = unmapped(k + "[]." + leaf);
                    }
            }
        } else if (e.entry == EntryOp::Remove && e.registry == "items") {
            for (const char* field : {"entries[].chance", "entries[].weight"}) {
                if (rm.mapping("drops", field, &reason) != map::Mapping::Mapped) {
                    bad = true; reason = "drop tables not proven on this build; item removal was not applied"; break;
                }
            }
            if (!bad) for (const char* field : {"inputs", "outputs", "inputs[].item", "outputs[].item"}) {
                if (rm.mapping("recipes", field, &reason) != map::Mapping::Mapped) {
                    bad = true; reason = "recipe dependencies not proven on this build; item removal was not applied"; break;
                }
            }
        } else if (e.raw.empty() && e.entry != EntryOp::Remove) {
            bad = unmapped(e.element.on ? (e.element.leaf.empty() ? e.element.list : e.element.list + "[]." + e.element.leaf) : e.field);
        }
        if (!bad) {
            kept.push_back(std::move(e));
            continue;
        }
        Refusal r{b->mod, b->source, b->stage, e.n, reason, ""};
        // the place, as compose's refusals name it (the join's registry part asks which field a refusal hit: stage.cpp finish)
        r.registry = e.registry;
        r.raw = !e.element.on && e.field.empty() && !e.raw.empty();
        r.field = e.element.on || !e.field.empty() ? e.field : r.raw ? e.raw.front().path : "";
        if (e.entry == EntryOp::Add) r.entry = e.registry + "|" + unmapped_lower(e.add_id);
        refused->push_back(std::move(r));
    }
    b->edits = std::move(kept);
}

}  // namespace kc::registry
