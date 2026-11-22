// SPDX-License-Identifier: MIT
// kiln.content: the registry's live back-end (registry P1 slice S9; P1-PLAN.md sections 1 and 7).
//
// The registry stage (S11, at resources-ready) composes every mod's registry edits over the base, Kiln's older paths below
// (owner decision D1). Fields that apply "now" or "worldLoad" are written here, as store layers of one owner ("registry") in
// the seq band kRegistrySeq: above every data-pack layer, below code. Each field's registry rows are one evaluated layer
// per game slot: its value is computed from the value below it (the game's, or an old pack's) at every flush, so a pack hot
// reload re-evaluates relative edits over the pack's new value. Fields that apply "restart" are the boot back-end's (S10)
// and are not touched here. Compare-before-write (sfi R3) is the store's: a slot another writer wrote after Kiln's layer is
// skipped and reported, never silently overwritten.
//
// Kiln translates: mods name Kiln ids and Kiln fields only; the regmap rows (S2) say which game members a field is and how
// its value converts (int, float, bool, time, enum, formula, list, ...). Every game entry is found by the GUID / 32-bit id
// the stage's model gave its Kiln id; an entry a mod added by the GUID RegistryLive::added_guid gives it (model.h's rule).
//
// Hot reload (apply_all_packs with reload): the "registry" sections of the pack files are lowered again; when one changed,
// the stage is composed again over the kept inputs (base, old writes, every mod's batches with the changed data batches
// replaced), every registry layer is replaced (a removed pack's edits leave nothing behind) and the report callback
// rewrites registry.json. A changed edit naming a field the start never decoded is refused ("needs a restart").
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "content.h"
#include "registry/compose.h"
#include "registry/regmap.h"

namespace kc {

struct RegistryLive {
    std::shared_ptr<const registry::Schema> schema;
    std::shared_ptr<const registry::map::RegMap> regmap;
    registry::Result result;
    std::map<std::string, std::string> ids;  // "<registry>|<kiln id>" -> the game entry's GUID text (guid_text's form)
    std::map<std::string, uint32_t> nums;    // the same key -> the game's 32-bit id (items, recipes, loot, drops, inputCategories)
    // Kept for a hot reload's recomposition (ModInputs: code batches recorded, code unset).
    registry::BaseModel base;
    std::vector<registry::OldWrite> old;
    std::vector<registry::ModInput> mods;
    std::set<std::string> decoded;  // "<registry>.<field>" the base holds values for
    // Kept across a hot reload's recomposition (review finding 6): the refusals the stage made outside compose (its mapping
    // check, skipped mods, unusable sections, faulted callbacks, failed boot writes) and the restart fields' outcomes as the
    // boot back-end wrote them (they change only at the next start).
    std::vector<registry::Refusal> pre_refused;
    std::map<registry::FieldRef, registry::FieldOutcome> boot_fields;
    // The stage's mapping check (refuse_unmapped), run on every reloaded batch.
    std::function<void(registry::Batch*, std::vector<registry::Refusal>*)> check;
    // The GUID of an entry a mod added (S11 sets registry::model::added_guid: UUIDv5 of "registry:<registry>:<kiln id>").
    std::function<std::string(const std::string& registry, const std::string& kiln_id)> added_guid;
    // registry.json (S11 writes it): called after every application of the live layers (world load, pack reload) with the
    // stage's result plus the live back-end's refusals and skipped writes as refused rows.
    std::function<void(const registry::Result&)> report;
};

// One pack file's "registry" section, lowered (packs.cpp registry_sections).
struct RegistrySection {
    std::string mod, version, rel, source;  // source: "content/<file>.json" (the batches' source)
    std::string side;                       // the mod's kiln.mod.json "side"
    uint64_t hash = 0;                     // of the section's text: what a reload compares
    std::vector<registry::Batch> batches;       // add, alter, final (those present)
    std::vector<registry::Refusal> refused;
    std::string error;                     // the section itself unusable
};
// The registry sections of the files (in their order) checked against the schema (lower_section). Files without one are
// left out.
std::vector<RegistrySection> registry_sections(const std::vector<PackFile>& files, const registry::Schema& schema);
const registry::Schema& registry_schema();  // the generated schema (schema_ir.h), built once

// The stage hands its result over (S11, resources-ready). Replaces what an earlier start installed.
void registry_live_install(RegistryLive in);
bool registry_live_installed();
// apply_all_packs, after the packs (store_mutex held): puts the live layers, flushes, reports. files: the pack files of this
// application (a reload compares their registry sections with the last ones).
// publish: report at once (registry_live_publish); apply_all_packs passes false and publishes after it released the store's lock.
void registry_live_apply(const std::vector<PackFile>& files, bool publish = true);
// The last application's registry.json, through the stage's report callback, with no lock of kiln.content's held (review
// finding 10: game job threads wait on the store's lock for drop tables). Call it with no store lock held.
void registry_live_publish();
// A drop table seen for the first time, or whose stacks moved (drops_apply_rules after newcontent repointed them): its
// registry layers come off the stacks they were on and go on the stacks the table has now.
void registry_live_table(const Entry& table);
// The last application's result as reported (the stage's plus the live refusals), a copy; none before any.
std::optional<registry::Result> registry_live_result();
// How the live back-end sizes a game type (type_size; tests replace it).
extern uint32_t (*g_registry_type_size)(const char* qname);
// The owner of the registry's layers (created on first use).
KilnOwner registry_owner();

}  // namespace kc
