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
inline void refuse_unmapped_edits(const map::RegMap& rm, const Schema& schema, Batch* b, std::vector<Refusal>* refused) {
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
        if (e.entry != EntryOp::None) {
            bad = true;
            reason = e.registry + " " + (e.entry == EntryOp::Add ? "add " + e.add_id : "remove " + e.select.id) +
                     ": entry add and remove have no back-end in this release (no game entry is removed and no new entry is created); this edit was not applied";
        } else if (e.entry == EntryOp::Add) {
            for (const auto& [k, v] : e.add_fields) bad = bad || unmapped(k);
        } else if (e.raw.empty() && e.entry != EntryOp::Remove) {
            bad = unmapped(e.element.on ? (e.element.leaf.empty() ? e.element.list : e.element.list + "[]." + e.element.leaf) : e.field);
        }
        if (!bad) {
            kept.push_back(std::move(e));
            continue;
        }
        Refusal r{b->mod, b->source, b->stage, e.n, reason, ""};
        if (e.entry == EntryOp::Add) r.entry = e.registry + "|" + unmapped_lower(e.add_id);
        refused->push_back(std::move(r));
    }
    b->edits = std::move(kept);
}

}  // namespace kc::registry
