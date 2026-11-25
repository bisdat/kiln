// SPDX-License-Identifier: MIT
// kiln-emf v32 buffs and v35 player_view without the game (host tests: tests/buffs_test.cpp).
//
// What EMF's core does (EMF 0.6.22 src/core/buffs.cpp, buff_service.cpp, player_view.cpp; MIT, the EMF contributors),
// put onto what Kiln's services give:
//  - a buff type is a 184-byte keen::BuffType record of the one buff collection (the BuffRegistry's +0x20, sigdb
//    gamedata/buffreg.collection; docs/re/gamedata.md), read at EMF's offsets. Kiln's catalog of the game's reflection
//    lists the record's fields in that order (buffTypeId, slot, applyType, defaultLifeTime, name, description,
//    isFoodPoison, hideInUi, showAsTag, ..., despawnOnDeath, isSaved, impact, factionExcludeMask, onCreate,
//    whileApplied, onTimeout, onTimeoutIsSet), and Kiln's own RE has id +0, isSaved +0x4a and onTimeout +0x88 where
//    EMF reads them (docs/re/emf-compat.md, "v32 buffs").
//  - an entity's buffs come from kiln.playerstate buffs() (BuffContainer + BuffNetworkData, any entity, server world):
//    Row is one of its records.
//  - giving and removing run the game's own applyBuff / removeBuff natives in a small Impact program kiln-emf
//    registers with kiln.abilities (the way Kiln.Abilities' ApplyBuff and RemoveBuff do), on an impact entity spawned
//    through kiln.spawn; the program keeps the native's answer and a done flag readable until Kiln reads them.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "emf/emf_api.h"

namespace kemf::buffs {

constexpr size_t kTypeRecordSize = 0xB8;  // keen::BuffType, 184 bytes
// The game's 274 buffs and Kiln's room of 4,096 buff clones (kiln.gamedata), with room to spare; EMF stops at 4,096.
constexpr uint32_t kMaxTypes = 8192;
constexpr int64_t kCollectionOffset = 0x20;  // BuffRegistry -> the collection resource (gamedata/buffreg.collection)
constexpr size_t kBuffsArray = 0x18;         // the collection's buffs BlobArray {s32 self-relative, u32 count}
constexpr int64_t kRefreshMinNs = 1'000'000'000LL;  // EMF: an end that moved later by more than this is REFRESHED
constexpr size_t kMaxPending = 64;    // EMF: "64 waiting"
constexpr size_t kStartsPerStep = 16;  // EMF's emf_buffs program takes 16 requests a step
constexpr uint64_t kTimeoutSteps = 120;  // Kiln.Abilities' removal timeout (about 5 s on a dedicated server)
constexpr uint32_t kWaitSteps = 30;      // the program keeps its answer readable this many steps, then ends itself
constexpr uint64_t kRegistryWaitSteps = 60;  // a request waits this long for the world's ImpactRegistry
constexpr uint32_t kMaxWatchers = 16;

uint32_t fnv1a(const uint8_t* p, size_t n);
uint32_t guid_id(const uint8_t* guid16);  // FNV-1a of a 16-byte GUID; 0 for an all-zero GUID (EMF's rule)

// One buff of an entity, as kiln.playerstate buffs() gives it.
struct Row {
    uint32_t type = 0, entity = 0;
    uint8_t slot = 0, index = 0, cleansed = 0;
    int64_t life_ns = 0, end_ns = 0;  // end 0 = not known (yet)
    uint32_t level = 0;
};

// A 184-byte record copy -> EmfBuffType (EMF's buff_type_of).
bool type_of(const uint8_t* record, EmfBuffType* out);
// The rows -> EmfBuff, in the order given (slot order), `now` the world's ECS time: the number there is; fills `out`
// up to `cap` (EMF's read_buffs).
EmfBuff buff_of(const Row& r, int64_t now);
uint32_t fill_buffs(const Row* rows, uint32_t n, int64_t now, EmfBuff* out, uint32_t cap);
const char* slot_name(uint8_t slot);  // "food" .. "terrain"

// ---- events: a player's buffs against the step before (EMF's BuffDiff) ----
class Diff {
public:
    // The events of one entity this step (its rows in slot order); the first sight records only.
    std::vector<EmfBuffEvent> step(uint32_t entity, const Row* rows, uint32_t n, int64_t now);
    void forget(uint32_t entity) { last_.erase(entity); }
    void retain(const std::vector<uint32_t>& present);
    void reset() { last_.clear(); }
    size_t size() const { return last_.size(); }

private:
    struct Inst {
        uint32_t buff_entity, type;
        uint8_t slot, cleansed;
        int64_t end;  // -1: not known
        int64_t life;
    };
    std::unordered_map<uint32_t, std::vector<Inst>> last_;
};

// ---- giving / removing ----
// 16 bytes whose FNV-1a is `id`: the game's natives hash a BuffTypeReference's GUID and look the hash up, so any GUID
// with the id's hash names that buff (Kiln.Abilities.GuidWithId: the last four bytes met in the middle).
std::array<uint8_t, 16> guid_with_id(uint32_t id);

extern const uint8_t kApplyProgram[16];   // kiln-emf's own program GUIDs (game byte order)
extern const uint8_t kRemoveProgram[16];
extern const uint8_t kGenericImpact[16];  // the game's generic impact template (Kiln.Abilities.GenericImpactTemplate)
constexpr uint32_t kApplyBuffNative = 0x91f30b26u;   // applyBuff, argc 5
constexpr uint32_t kRemoveBuffNative = 0xca687904u;  // removeBuff, argc 3
// Program variables, in layout order (instance_read's index).
constexpr uint32_t kApplyVarBuff = 0, kApplyVarTarget = 1, kApplyVarDuration = 2, kApplyVarApplied = 3, kApplyVarDone = 4;
constexpr uint32_t kRemoveVarBuff = 0, kRemoveVarTarget = 1, kRemoveVarWas = 2, kRemoveVarDone = 3;
// keen::ImpactProgram blobs (Kiln.Abilities' ProgramBlob layout).
std::vector<uint8_t> apply_program();
std::vector<uint8_t> remove_program();

// What a running request's program says now.
enum class Wait { Waiting, Answered, Gone, TimedOut, Closed };
Wait judge(bool world_running, bool impact_exists, int done /* -1 not readable, 0, 1 */, uint64_t age_steps);
const char* wait_why(Wait w);  // the REFUSED reason of Gone / TimedOut / Closed

// After a give: the game's native answer and the target's buffs read back (EMF's check_applied). Empty = applied
// (*buff_entity = the new instance); else why not. faction -1: not read.
std::string apply_check(bool native_ok, const Row* rows, uint32_t n, uint32_t type, uint8_t slot,
                        uint16_t faction_exclude, int faction, uint32_t* buff_entity);
// The instance a removal takes: the first of `type` not being removed yet (EMF's buff_cleanse order), 0 = none.
// `taken`: instances other removals already took and the game has not cleansed yet (the same step, or still running).
uint32_t removal_target(const Row* rows, uint32_t n, uint32_t type, const std::vector<uint32_t>* taken = nullptr);

}  // namespace kemf::buffs

namespace kemf::view {

constexpr float kDefaultWorldSize = 10240.0f;  // Embervale's map, EMF's default before its fog grid is read
// Where (0, 0, 1) turned by the quaternion points: 0 north (+z), 90 east (+x), 0..360 (EMF's heading_from_quat).
float heading_from_quat(float x, float y, float z, float w);
inline double metres(int64_t fixed) { return static_cast<double>(fixed) / 4294967296.0; }  // keen::WorldPosition 32.32
// EMF's copy rule: out->size 0 or at least the struct = the whole struct; else that many bytes; out->size = bytes.
void copy_view(const EmfPlayerView& v, EmfPlayerView* out);

}  // namespace kemf::view
