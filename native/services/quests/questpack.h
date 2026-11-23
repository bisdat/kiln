// SPDX-License-Identifier: MIT
// Compiles mods' quests/*.json (every quest and dialogue unless it says "journal": "chat" or "native": false, NPCs, new
// knowledge) into the game's resources (questbuild.h). Portable: tools/kfc/tests runs it on Linux. docs/kiln/services/npcs.md.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "nlohmann/json.hpp"
#include "questbuild.h"

namespace kiln::qb {

using ojson = nlohmann::ordered_json;

constexpr size_t kMaxBlockFile = 16u << 20;
constexpr int kMaxBlockDepth = 64;
// blocks/*.json accepts comments and trailing commas, like the managed block loader. Invalid or oversized input is discarded.
ojson parse_blocks_json(const std::string& text);

struct ModInput {
    std::string id;                  // the manifest's id
    std::vector<ojson> documents;    // quests/*.json, then quests/generated/*.json and what code defined (kiln/data/kiln-quests/native/<id>.json)
    std::vector<std::string> files;  // parallel: names for messages
    ojson audio;                     // assets/cooked/audio.json (null when the mod has none)
    std::string guid_namespace;      // assets/cook.json's guidNamespace ("" when none)
    std::vector<std::string> required_mods;  // native dependencies: a refused dependency contributes no shared tables
    bool authored_body = false;      // do not fall back to publishing the ordinary part of a body-dependent mod
    std::string dir;                 // the mod's folder (UTF-8): authored-body admission reads its cooked body cache there
    std::string body_refusal;        // set at collection: this mod's authored bodies cannot be admitted (the whole mod is refused)
    std::vector<std::string> refused_bodies;  // NPC blocks collection left out for body_refusal (another chassis): failed rows still
};
// The one verified roster-body chassis (chapter 16f section 5.1): Bodies.Fisher and Bodies.Villager both lower to it.
constexpr const char* kBodyChassis = "NPC_Workshop_Fisher_Trader";

// Read access to the game's resources.
struct GameData {
    std::function<bool(const Guid&, uint32_t type, const uint8_t** data, size_t* len)> read;
    // template name -> GUID (false: unknown). Names resolve in the same data the game loads.
    std::function<bool(const std::string& name, Guid* out)> template_by_name;
    // The roster's maximum on this side (since 0.2.0-dev.29): asked only when the mods' NPCs and the collection's own
    // entries together are more than kMaxNpcs, with that total. Returns kMaxNpcs or kMaxNpcsPaged; `why` says why it is
    // kMaxNpcs (the build's refusal carries it). Not set: kMaxNpcs.
    std::function<uint32_t(uint32_t needed, std::string* why)> roster_max;
    // 16f: the running game's type registry still has the verified body layout (bodypack.h check_body_layout). Not set:
    // an NPC with an admitted "body" refuses its mod. Asked only when such an NPC is built.
    std::function<bool(std::string* why)> body_layout;
};

struct TemplateOut {
    Guid guid;
    std::string name;
    bool is_new = false;
    Bytes data;
};
struct ResourceOut {  // 16f S4: a new resource the build adds beside its templates (cloned NPC motion sequences)
    Guid guid;
    uint32_t type = 0;
    std::string name;
    Bytes data;
};
struct TextOut {
    uint32_t tag = 0;
    std::string json;  // {"en": "...", "de": "..."}
};
struct PackOut {
    bool any = false;          // something native to build
    bool ok = true;
    std::vector<std::string> errors, warnings;
    std::string shared_failure;  // missing shared inputs stop startup; one mod's template/build failure does not
    std::vector<KnowledgeDef> knowledge;
    std::vector<JournalQuestDef> quests;
    std::vector<NpcDef> npcs;
    Bytes res_knowledge, res_journal, res_npcs;
    size_t knowledge_source_size = 0;  // input length before this compile appended tables; private compaction provenance
    std::vector<TemplateOut> templates;
    std::vector<ResourceOut> resources;  // new keys only, published with the templates
    std::vector<TextOut> texts;
    std::vector<std::string> content_items, content_recipes;  // definitions for kiln.content's define()
    // The roster as built (since 0.2.0-dev.29; empty when no mod adds an NPC): the collection's NpcIds before the
    // build (sorted), the ids the build added (sorted), and the maximum it was built against.
    std::vector<uint32_t> roster_boot_ids, roster_kiln_ids;
    uint32_t roster_max = kMaxNpcs;
    // The roster could not be held (more entries than roster_max): the build failed with roster_refusal, and the
    // start must stop before any world loads (native_pack.cpp; a world without these mods' data drops their progress).
    // roster_mods: the mods that add roster entries.
    bool roster_refused = false;
    std::string roster_refusal;
    std::vector<std::string> roster_mods;
    ojson manifest;            // what Kiln.Quests/Native needs at run time (schema: npcs.md)
};

constexpr const char* kGuidKnowledge = "cb1eeb8b-f58a-4785-bf0f-6dd5437e9a5b";
constexpr const char* kGuidJournal = "33701b26-ec1d-423f-8e06-49f023b91b7f";
constexpr const char* kGuidNpcs = "377f61dc-c7b8-4e4f-830f-7d2c3e8b7fd8";

// The deterministic id of a mod's named thing (knowledge object, line, tag, ...).
Guid derive_guid(const std::string& mod, const std::string& kind, const std::string& name);
inline uint32_t derive_id(const std::string& mod, const std::string& kind, const std::string& name) {
    return fnv(derive_guid(mod, kind, name));
}

// Compiles and builds. Errors are collected: when any is reported, nothing of that mod is used (the others are built).
PackOut compile_pack(const std::vector<ModInput>& mods, const GameData& game);

}  // namespace kiln::qb
