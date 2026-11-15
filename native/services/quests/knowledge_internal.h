// SPDX-License-Identifier: MIT
// kiln.knowledge (kiln-quests.dll): what knowledge.cpp and catalog.cpp share.
#pragma once
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kiln_api.h"
#include "kiln_knowledge.h"

namespace kiln::knowledge {

extern const KilnApi* g_api;
extern KilnOwner g_owner;

void logf(int level, const char* fmt, ...);
#define KI(...) ::kiln::knowledge::logf(KILN_LOG_INFO, "knowledge: " __VA_ARGS__)
#define KW(...) ::kiln::knowledge::logf(KILN_LOG_WARN, "knowledge: " __VA_ARGS__)
#define KE(...) ::kiln::knowledge::logf(KILN_LOG_ERROR, "knowledge: " __VA_ARGS__)

inline KilnStr kstr(const char* s) { return KilnStr{s, std::strlen(s)}; }

// SEH-guarded copy from game memory (no C++ objects in the guarded frame).
bool copy_guarded(void* dst, const void* src, size_t n);
template <class T>
bool rd(uintptr_t a, T* out) {
    return a && copy_guarded(out, reinterpret_cast<const void*>(a), sizeof(T));
}
// A BlobArray / BlobString field {i32 self-relative offset, u32 count} at `field`: the element base (0 when empty).
bool blob(uintptr_t field, uintptr_t* base, uint32_t* count);

// ---- catalog (catalog.cpp): names, NPC roster, journal, query db; built on the tick thread at the world's first step.
struct CatNpc {
    KilnKnowledgeNpc npc{};
    std::string name;  // debugName
};
struct CatQuest {
    KilnJournalQuest quest{};
    std::vector<KilnJournalStep> steps;
};
struct CatQuery {
    uint32_t id = 0, index = 0, knowledge = 0;
    uint8_t type = 0, player = 0;
    std::string name;
};

// Builds the catalog from the world's registries (npcreg: the NpcRegistry singleton, journal: the JournalRegistry
// singleton) and kiln.assets (the query db). Logs what it found. Tick thread.
void catalog_build(uintptr_t npcreg, uintptr_t journal);
void catalog_clear();
bool catalog_ready();
uint32_t catalog_count(uint32_t cat);
const CatNpc* catalog_npc(uint32_t index);
const CatQuest* catalog_quest(uint32_t index);
const CatQuery* catalog_query_by_id(uint32_t id);
const CatQuery* catalog_query_at(uint32_t index);
// name -> what it means; false when unknown. Raw ids are handled by the caller.
bool catalog_resolve(const std::string& lower_name, KilnKnowledgeName* out);
// id -> catalog name ("" none)
std::string catalog_name_of(uint32_t id, uint32_t kind);
std::string catalog_entry_name(uint32_t cat, uint32_t index);
// "line:<template>/<kind><k>" through kiln.gamedata (on demand, cached); false when it cannot be resolved.
bool catalog_resolve_line(const std::string& name, uint32_t* out_id);

// Where the registry's GameKnowledgeResource lives (log text).
std::string knowledge_resource_origin(uintptr_t res);

// Mods' native quests and NPCs (native_pack.cpp): boot-time resource rebuilds.
void native_pack_install();   // kiln_plugin_main: the hook on the game's resource service init
void native_pack_ready();     // on_ready: the resource service may be up already (a client)
bool native_pack_boot();      // on_ready, first: a boot profile (chapter 08) registers the prepare with kiln.boot
std::string native_pack_manifest();
bool native_pack_resolve(const std::string& lower_name, uint32_t* id);  // "<mod>:<name>" (lower case)

// The Mother Flame's level (flame.cpp): the service functions and the program's registration (on_ready).
KilnResult KILN_CALL flame_status(KilnFlameStatus* out);
KilnResult KILN_CALL flame_set(KilnOwner owner, uint32_t level, uint64_t* out_request);
KilnResult KILN_CALL flame_result(uint64_t request, KilnFlameResult* out);
void flame_ready();

// NpcRegistry unlock state of the server world's catalog (quests.npc): pages[0] bit i = entry i unlocked, pages[1]
// bit i = entry 64 + i (only when *npages is 2: the registry's second page matches its cache); false when unreadable
// or not computed yet. *count: the registry's entries.
bool npc_unlock_pages_server(uint64_t pages[2], uint32_t* npages, uint16_t* version, uint32_t* count);

// A reason for KilnKnowledgeStatus.reason (the first one stays), from outside knowledge.cpp. Logs nothing.
void knowledge_reason(const std::string& text);

// The roster (roster.cpp; roster_core.h; docs/re/npc-roster.md).
void roster_main();   // kiln_plugin_main: what this side has for the second page; the two worlds' Post callbacks
void roster_ready();  // on_ready: the capability rows npcs.*
// The boot build's question (questpack.h GameData::roster_max): 64, or 128 with the second page installed now.
uint32_t roster_max(uint32_t needed, std::string* why);
// The boot build's roster: the collection's ids before it and the ids it added (both sorted).
void roster_built(std::vector<uint32_t> boot_ids, std::vector<uint32_t> kiln_ids);
uint32_t roster_limit();  // 64 or 128
void roster_world_event(const KilnWorldEvent* e);
// The boot build could not hold the mods' roster (more than 128 entries, or more than 64 where the second page cannot be
// installed): the start stops here, before any world loads (the error line, kiln/state/roster-refused.json, kiln.boot's
// activation fault for each mod, on a client a message box; then the process ends with KILN_KNOW_ROSTER_EXIT_CODE).
// Call with no lock held. Does not return, except when a world is present or has run in this process already (a
// start-time call only: ending the process then could lose play): then it logs that the start could not be stopped.
void roster_refuse_start(const std::string& why, const std::vector<std::string>& mods);
// One registry's pages (as npc_unlock_pages_server, for any registry).
bool npc_unlock_pages(uintptr_t registry, uint64_t pages[2], uint32_t* npages, uint16_t* version, uint32_t* count);
KilnResult KILN_CALL roster_world(uint32_t kind, uint32_t* out_generation, uint32_t* out_readiness);
KilnResult KILN_CALL roster_budget(uint32_t kind, uint32_t generation, KilnCapacityInfo* out);
KilnResult KILN_CALL npc_unlock_at(uint32_t kind, uint32_t generation, uint32_t npc_id, KilnNpcUnlockReply* out);
// knowledge.cpp: a roster change into the event ring (KILN_KNOW_EVENT_ROSTER). Any thread.
void emit_roster_change(uint32_t kind, uint32_t generation, uint32_t npc_id, uint8_t before, uint8_t after, uint16_t version,
                        uint64_t revision, uint64_t step);

}  // namespace kiln::knowledge
