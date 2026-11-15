// SPDX-License-Identifier: MIT
// kiln.items v6, equipment sets: the canonical snapshot of a player's inventory and the snapshot store (chapter 10
// §4.1, §4.2). Nothing here touches game memory by itself: a reader hands over the bytes of one component of one entity
// (GetComponent in Pre, the program's declared accessors inside kiln_items_equip), so both readers produce the same
// bytes from the same state, and a host test runs it on made-up memory (tests/setsnap_test.cpp).
//
// The snapshot is not a revision counter and not a hash: it is the copied, normalised bytes that decide what a set
// selects and whether it can be applied, compared byte for byte.
//   1. The root InventorySetup and every Virtual one it reaches: entity, link count, the ordered (entity, category)
//      links up to the first zero, flags, generic and available slot counts, initialised. At most 64 setup nodes,
//      Virtual links down to depth 3 and 32 leaf inventories: a tree beyond that, a cycle or a node that cannot be read
//      is a refusal (the baseline walk in engine.cpp stops there silently; a set never runs on a cut tree).
//   2. The leaf inventories in ascending entity order, eight {item, count, pide} each; an empty stack (item 0 or count
//      0) is all zero, pide 0xFFFFFFFF is 0. Currency, equipment and nested pages included.
//   3. SlotSelection's 20 equipment slots and its two action-bar entity ids, slot ids normalised (entity | index << 32,
//      0 for none; the padding is not read). The selected action-bar slot, the item in hand and the material
//      selection are left out: a change of the item in hand does not stale a plan, and a set never changes them.
//   4. Every distinct item entity of those inventories in ascending id order: its item id, its kiln.items stamp
//      (zero-extended), ItemState's containedInSlotId (normalised) and itemId, OwnerRelationship's owner. Durability,
//      fuel, perks and level are left out: they do not change which item a set selects.
//   5. A prefix: schema 1, the process nonce, the world epoch, the player, the player's identity (kiln.decisions'
//      KilnEventEntityKey). Little-endian, explicit widths, no C padding.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "moves.h"
#include "services/kiln_decisions.h"
#include "services/kiln_items.h"

namespace kitems::sets {

constexpr uint32_t kSchema = 1;
constexpr uint32_t kMaxNodes = 64;   // InventorySetup nodes (the root and the Virtual ones it reaches)
constexpr uint32_t kMaxDepth = 3;    // a Virtual link inside a node of depth 3 is refused
constexpr uint32_t kMaxLeaves = 32;  // leaf inventories (KILN_ITEMS_MAX_INVENTORIES)
constexpr uint32_t kSelectionSlots = 20; // SlotSelection.equipment
constexpr uint8_t kCatGeneric = 4, kCatVirtual = 5, kCatEquipment = 2;

// Where the fields are in this build's components (kiln.types; sets.cpp resolves and checks them). The defaults are the
// layouts of the checked-in dumps of server 26430253 and client 26430625 (sigdb/types/), used by the host tests.
struct SetLayout {
    uint32_t setup_size = 84, setup_links = 0, setup_cats = 64, setup_flags = 80, setup_generic = 81, setup_available = 82,
             setup_initialized = 83;
    uint32_t inv_size = 96;  // eight ItemStack {u32 id; ItemData {u32 count; u32 pide}}
    uint32_t sel_size = 216, sel_equipment = 0, sel_actionbar = 160;
    uint32_t slot_size = 8, slot_entity = 0, slot_index = 4;  // InventorySlotId
    uint32_t state_size = 20, state_slot = 0, state_item = 8;
    uint32_t owner_size = 4, owner_entity = 0;
};

// The parts a reader supplies.
enum class Part : uint32_t { Setup, Inventory, Selection, ItemState, Owner };

// Hands over a component's bytes: a pointer to at least the layout's size of that part, or null when the entity has
// none (or a smaller one). The pointer is read at once and never kept.
class SnapReader {
public:
    virtual ~SnapReader() = default;
    virtual const uint8_t* get(Part part, uint32_t entity) = 0;
    // kiln.items' stamp of an item entity holding `item` (instances.h stamp_for: thread-safe); 0 = none.
    virtual uint32_t stamp(uint32_t pide, uint32_t item) = 0;
};

struct NodeRec {
    uint32_t entity = 0;
    uint8_t depth = 0;
    uint8_t link_count = 0;
    uint32_t link_entity[16] = {};
    uint8_t link_cat[16] = {};
    uint8_t flags = 0, generic = 0, available = 0, initialized = 0;
};

struct LeafRec {
    uint32_t entity = 0;
    Stack s[kSlotsPerPage];  // normalised: an empty stack is all zero; slot is always set
};

struct ItemRec {
    uint32_t pide = 0, item = 0, stamp = 0;
    uint8_t has_state = 0, has_owner = 0;
    uint64_t contained = 0;  // ItemState.containedInSlotId, normalised
    uint32_t state_item = 0; // ItemState.itemId
    uint32_t owner = 0;      // OwnerRelationship.ownerEntityId
};

struct SetModel {
    uint32_t player = 0;
    std::vector<NodeRec> nodes;    // nodes[0] is the root; depth-first in link order
    std::vector<LeafRec> leaves;   // ascending entity
    uint64_t equipment[kSelectionSlots] = {};
    uint32_t actionbar[2] = {};
    std::vector<ItemRec> items;    // ascending pide

    const LeafRec* leaf(uint32_t entity) const;
    const Stack* stack_at(uint64_t slot) const;  // null: not a slot of these inventories
    const ItemRec* item(uint32_t pide) const;
    // The stack holding item entity `pide`; null when none does.
    const Stack* stack_of(uint32_t pide) const;
};

enum class SnapStatus : uint32_t {
    Ok = 0,
    NoSetup,           // the entity has no InventorySetup
    NotInitialized,    // its inventory is not initialised yet
    NoSelection,       // no SlotSelection: not a character with equipment
    NotPlayer,         // a SlotSelection without an action bar: not a player character
    TooDeep,           // a Virtual link below depth 3
    TooManyNodes,      // more than 64 InventorySetup nodes
    TooManyLeaves,     // more than 32 leaf inventories
    Cycle,             // a Virtual link back to a node on its own path
    MissingNode,       // a Virtual link to an entity without an InventorySetup
    MissingInventory,  // a link to an entity without an Inventory
};
const char* snap_status_text(SnapStatus s);

// Reads a player's tree into `m` (cleared first). Not Ok: `m` is partial and must not be used.
SnapStatus read_model(SnapReader& r, const SetLayout& layout, uint32_t player, SetModel* m);

// The prefix (part 5).
struct SnapPrefix {
    uint8_t session[16] = {};
    uint64_t world_epoch = 0;
    uint32_t player = 0;
    KilnEventEntityKey identity{};
};

// The canonical bytes of a model under a prefix (out is replaced).
void encode(const SnapPrefix& prefix, const SetModel& m, std::vector<uint8_t>* out);

// ---- the store ----
// One captured snapshot. The store owns its bytes and model until it is released, expires, is taken by an accepted
// set, or the world stops.
struct SnapEntry {
    uint64_t token = 0;
    KilnOwner owner = 0;
    uint64_t captured_step = 0;
    uint64_t stamp_epoch = 0;  // instances_epoch() at the capture: a stamp reset (an id counter regression) stales it
    SnapPrefix prefix;
    SetModel model;
    std::vector<uint8_t> bytes;
};

// Tokens are nonzero, only grow within the process and are never handed out twice, also after a clear. At most 256
// live snapshots in all and 16 per owner; a full store refuses (a soft result), it never evicts. A snapshot can be used
// through captured_step + 250 and is gone after that. Not thread-safe: sets.cpp holds a mutex around every call.
class SnapStore {
public:
    static constexpr uint32_t kGlobal = 256;
    static constexpr uint32_t kPerOwner = 16;
    static constexpr uint64_t kTtl = 250;

    enum class Put { Ok, GlobalFull, OwnerFull };
    // Drops expired snapshots first (`now`), then stores `e` under a new token (*token).
    Put put(SnapEntry&& e, uint64_t now, uint64_t* token);

    enum class Find { Ok, Unknown, OtherOwner, Expired, OtherWorld, StampsReset, OtherPlayer };
    // The snapshot `token` for `owner` in world `epoch` with stamp epoch `stamp_epoch` at step `now`. Ok: *out points at
    // it (valid until the next call). An expired one is removed and reported Expired; one of another world or stamp
    // epoch is removed and reported so.
    Find peek(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t stamp_epoch, uint64_t now, const SnapEntry** out);
    // As peek, and on Ok moves the snapshot out of the store (an accepted set takes it).
    Find take(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t stamp_epoch, uint64_t now, SnapEntry* out);
    // Releases `token` if `owner` captured it. False: unknown, or another owner's.
    bool release(uint64_t token, KilnOwner owner);
    size_t expire(uint64_t now);
    size_t clear();
    size_t release_owner(KilnOwner owner);

    size_t size() const { return live_.size(); }
    uint32_t count_of(KilnOwner owner) const;
    uint64_t issued() const { return next_ - 1; }

private:
    long find(uint64_t token) const;
    std::vector<SnapEntry> live_;
    uint64_t next_ = 1;
};

}  // namespace kitems::sets
