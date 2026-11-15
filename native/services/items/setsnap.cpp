// SPDX-License-Identifier: MIT
// kiln.items v6: the canonical snapshot and the snapshot store (setsnap.h). Plain data, no game call, no Windows.
#include "setsnap.h"

#include <algorithm>
#include <cstring>

namespace kitems::sets {
namespace {

template <typename T>
T rd(const uint8_t* p, uint32_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// An InventorySlotId as a KilnSlotId: entity | index << 32, 0 for no entity. The padding after the index is not read.
uint64_t slot_id(const uint8_t* p, const SetLayout& L) {
    const uint32_t e = rd<uint32_t>(p, L.slot_entity);
    return e ? KILN_SLOT_ID(e, p[L.slot_index]) : 0;
}

struct Walk {
    SnapReader& r;
    const SetLayout& L;
    SetModel* m;
    std::vector<uint32_t> path;       // nodes on the current path (cycle check)
    std::vector<uint32_t> seen_nodes;  // every node read (a node reached twice is read once)
    std::vector<uint32_t> leaf_ids;    // leaf inventories, in the order found

    SnapStatus node(uint32_t entity, uint8_t depth) {
        if (m->nodes.size() >= kMaxNodes) return SnapStatus::TooManyNodes;
        const uint8_t* s = r.get(Part::Setup, entity);
        if (!s) return depth == 0 ? SnapStatus::NoSetup : SnapStatus::MissingNode;
        NodeRec n{};
        n.entity = entity;
        n.depth = depth;
        for (uint32_t k = 0; k < 16; ++k) {
            const uint32_t link = rd<uint32_t>(s, L.setup_links + 4 * k);
            if (link == 0) break;
            n.link_entity[k] = link;
            n.link_cat[k] = s[L.setup_cats + k];
            n.link_count = static_cast<uint8_t>(k + 1);
        }
        n.flags = s[L.setup_flags];
        n.generic = s[L.setup_generic];
        n.available = s[L.setup_available];
        n.initialized = s[L.setup_initialized];
        m->nodes.push_back(n);
        seen_nodes.push_back(entity);
        path.push_back(entity);
        for (uint32_t k = 0; k < n.link_count; ++k) {
            const uint32_t link = n.link_entity[k];
            if (n.link_cat[k] == kCatVirtual) {
                if (std::find(path.begin(), path.end(), link) != path.end()) return SnapStatus::Cycle;
                if (std::find(seen_nodes.begin(), seen_nodes.end(), link) != seen_nodes.end()) continue;  // read already
                if (depth >= kMaxDepth) return SnapStatus::TooDeep;
                if (SnapStatus st = node(link, static_cast<uint8_t>(depth + 1)); st != SnapStatus::Ok) return st;
                continue;
            }
            if (std::find(leaf_ids.begin(), leaf_ids.end(), link) != leaf_ids.end()) continue;
            if (leaf_ids.size() >= kMaxLeaves) return SnapStatus::TooManyLeaves;
            leaf_ids.push_back(link);
        }
        path.pop_back();
        return SnapStatus::Ok;
    }
};

void put8(std::vector<uint8_t>* o, uint8_t v) { o->push_back(v); }
void put32(std::vector<uint8_t>* o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o->push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void put64(std::vector<uint8_t>* o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o->push_back(static_cast<uint8_t>(v >> (8 * i)));
}

}  // namespace

const char* snap_status_text(SnapStatus s) {
    switch (s) {
        case SnapStatus::Ok: return "";
        case SnapStatus::NoSetup: return "the entity has no inventory (InventorySetup)";
        case SnapStatus::NotInitialized: return "the player's inventory is not initialized yet";
        case SnapStatus::NoSelection: return "the entity has no equipment (SlotSelection): not a character";
        case SnapStatus::NotPlayer: return "the entity's equipment has no action bar: not a player character";
        case SnapStatus::TooDeep: return "the player's inventory nests Virtual links deeper than 3: Kiln does not snapshot it";
        case SnapStatus::TooManyNodes: return "the player's inventory has more than 64 inventory setups: Kiln does not snapshot it";
        case SnapStatus::TooManyLeaves: return "the player's inventory has more than 32 inventories: Kiln does not snapshot it";
        case SnapStatus::Cycle: return "the player's inventory links back into itself (a Virtual cycle): Kiln does not snapshot it";
        case SnapStatus::MissingNode: return "a Virtual link of the player's inventory names an entity without an inventory setup";
        case SnapStatus::MissingInventory: return "a link of the player's inventory names an entity without an inventory";
    }
    return "unknown";
}

const LeafRec* SetModel::leaf(uint32_t entity) const {
    auto it = std::lower_bound(leaves.begin(), leaves.end(), entity, [](const LeafRec& l, uint32_t e) { return l.entity < e; });
    return it != leaves.end() && it->entity == entity ? &*it : nullptr;
}

const Stack* SetModel::stack_at(uint64_t slot) const {
    if (!slot || (slot >> 40) != 0 || KILN_SLOT_INDEX(slot) >= kSlotsPerPage) return nullptr;
    const LeafRec* l = leaf(KILN_SLOT_INVENTORY(slot));
    return l ? &l->s[KILN_SLOT_INDEX(slot)] : nullptr;
}

const ItemRec* SetModel::item(uint32_t pide) const {
    auto it = std::lower_bound(items.begin(), items.end(), pide, [](const ItemRec& i, uint32_t p) { return i.pide < p; });
    return it != items.end() && it->pide == pide ? &*it : nullptr;
}

const Stack* SetModel::stack_of(uint32_t pide) const {
    if (!pide) return nullptr;
    for (const LeafRec& l : leaves)
        for (const Stack& s : l.s)
            if (s.pide == pide) return &s;
    return nullptr;
}

SnapStatus read_model(SnapReader& r, const SetLayout& L, uint32_t player, SetModel* m) {
    *m = SetModel{};
    m->player = player;
    Walk w{r, L, m, {}, {}, {}};
    if (SnapStatus st = w.node(player, 0); st != SnapStatus::Ok) return st;
    if (m->nodes[0].initialized == 0) return SnapStatus::NotInitialized;
    const uint8_t* sel = r.get(Part::Selection, player);
    if (!sel) return SnapStatus::NoSelection;
    for (uint32_t i = 0; i < kEquipSlots; ++i) m->equipment[i] = slot_id(sel + L.sel_equipment + i * L.slot_size, L);
    m->actionbar[0] = rd<uint32_t>(sel, L.sel_actionbar);
    m->actionbar[1] = rd<uint32_t>(sel, L.sel_actionbar + 4);
    if (m->actionbar[0] == 0) return SnapStatus::NotPlayer;  // a player's selection always has its first action-bar page
    std::sort(w.leaf_ids.begin(), w.leaf_ids.end());
    for (uint32_t id : w.leaf_ids) {
        const uint8_t* inv = r.get(Part::Inventory, id);
        if (!inv) return SnapStatus::MissingInventory;
        LeafRec l{};
        l.entity = id;
        for (uint32_t k = 0; k < kSlotsPerPage; ++k) {
            Stack& s = l.s[k];
            s.slot = KILN_SLOT_ID(id, k);
            const uint32_t item = rd<uint32_t>(inv, 12 * k), count = rd<uint32_t>(inv, 12 * k + 4), pide = rd<uint32_t>(inv, 12 * k + 8);
            if (item == 0 || count == 0) continue;  // an empty stack is all zero, whatever its tail holds
            s.item = item;
            s.count = count;
            s.pide = pide == 0xFFFFFFFFu ? 0 : pide;
        }
        m->leaves.push_back(l);
    }
    for (const LeafRec& l : m->leaves)
        for (const Stack& s : l.s) {
            if (!s.pide) continue;
            ItemRec it{};
            it.pide = s.pide;
            it.item = s.item;
            m->items.push_back(it);
        }
    std::sort(m->items.begin(), m->items.end(), [](const ItemRec& a, const ItemRec& b) { return a.pide < b.pide; });
    // one record per item entity (a corrupt tree could list one entity in two slots: both stay in the leaves)
    m->items.erase(std::unique(m->items.begin(), m->items.end(), [](const ItemRec& a, const ItemRec& b) { return a.pide == b.pide; }),
                   m->items.end());
    for (ItemRec& it : m->items) {
        it.stamp = r.stamp(it.pide, it.item);
        if (const uint8_t* st = r.get(Part::ItemState, it.pide)) {
            it.has_state = 1;
            it.contained = slot_id(st + L.state_slot, L);
            it.state_item = rd<uint32_t>(st, L.state_item);
        }
        if (const uint8_t* ow = r.get(Part::Owner, it.pide)) {
            it.has_owner = 1;
            it.owner = rd<uint32_t>(ow, L.owner_entity);
        }
    }
    return SnapStatus::Ok;
}

void encode(const SnapPrefix& p, const SetModel& m, std::vector<uint8_t>* o) {
    o->clear();
    o->reserve(128 + m.nodes.size() * 96 + m.leaves.size() * 100 + m.items.size() * 40);
    // 5. the prefix
    put32(o, kSchema);
    for (uint8_t b : p.session) put8(o, b);
    put64(o, p.world_epoch);
    put32(o, p.player);
    put32(o, p.identity.entity);
    put32(o, p.identity.flags);
    put64(o, p.identity.spawn_time);
    put64(o, p.identity.life_serial);
    put64(o, p.identity.player_handle);
    put64(o, p.identity.joined_step);
    // 1. the setup nodes
    put32(o, static_cast<uint32_t>(m.nodes.size()));
    for (const NodeRec& n : m.nodes) {
        put32(o, n.entity);
        put8(o, n.link_count);
        for (uint32_t k = 0; k < n.link_count; ++k) {
            put32(o, n.link_entity[k]);
            put8(o, n.link_cat[k]);
        }
        put8(o, n.flags);
        put8(o, n.generic);
        put8(o, n.available);
        put8(o, n.initialized);
    }
    // 2. the leaf inventories
    put32(o, static_cast<uint32_t>(m.leaves.size()));
    for (const LeafRec& l : m.leaves) {
        put32(o, l.entity);
        for (const Stack& s : l.s) {
            put32(o, s.item);
            put32(o, s.count);
            put32(o, s.pide);
        }
    }
    // 3. the selection
    for (uint64_t e : m.equipment) put64(o, e);
    put32(o, m.actionbar[0]);
    put32(o, m.actionbar[1]);
    // 4. the item entities
    put32(o, static_cast<uint32_t>(m.items.size()));
    for (const ItemRec& it : m.items) {
        put32(o, it.pide);
        put32(o, it.item);
        put64(o, it.stamp);
        put8(o, it.has_state);
        put64(o, it.contained);
        put32(o, it.state_item);
        put8(o, it.has_owner);
        put32(o, it.owner);
    }
}

// ---- the store ----

long SnapStore::find(uint64_t token) const {
    for (size_t i = 0; i < live_.size(); ++i)
        if (live_[i].token == token) return static_cast<long>(i);
    return -1;
}

uint32_t SnapStore::count_of(KilnOwner owner) const {
    uint32_t n = 0;
    for (const SnapEntry& e : live_) n += e.owner == owner;
    return n;
}

size_t SnapStore::expire(uint64_t now) {
    const size_t before = live_.size();
    live_.erase(std::remove_if(live_.begin(), live_.end(), [now](const SnapEntry& e) { return now > e.captured_step + kTtl; }),
                live_.end());
    return before - live_.size();
}

SnapStore::Put SnapStore::put(SnapEntry&& e, uint64_t now, uint64_t* token) {
    *token = 0;
    expire(now);
    if (live_.size() >= kGlobal) return Put::GlobalFull;
    if (count_of(e.owner) >= kPerOwner) return Put::OwnerFull;
    e.token = next_++;
    *token = e.token;
    live_.push_back(std::move(e));
    return Put::Ok;
}

SnapStore::Find SnapStore::peek(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t stamp_epoch, uint64_t now,
                                const SnapEntry** out) {
    *out = nullptr;
    const long i = token ? find(token) : -1;
    if (i < 0) return Find::Unknown;
    const SnapEntry& e = live_[static_cast<size_t>(i)];
    if (e.owner != owner) return Find::OtherOwner;  // left in place: it is someone else's
    Find why = Find::Ok;
    if (now > e.captured_step + kTtl) why = Find::Expired;
    else if (e.prefix.world_epoch != epoch) why = Find::OtherWorld;
    else if (e.stamp_epoch != stamp_epoch) why = Find::StampsReset;
    if (why != Find::Ok) {
        live_.erase(live_.begin() + i);
        return why;
    }
    *out = &e;
    return Find::Ok;
}

SnapStore::Find SnapStore::take(uint64_t token, KilnOwner owner, uint64_t epoch, uint64_t stamp_epoch, uint64_t now, SnapEntry* out) {
    const SnapEntry* e = nullptr;
    const Find f = peek(token, owner, epoch, stamp_epoch, now, &e);
    if (f != Find::Ok) return f;
    const long i = find(token);
    *out = std::move(live_[static_cast<size_t>(i)]);
    live_.erase(live_.begin() + i);
    return Find::Ok;
}

bool SnapStore::release(uint64_t token, KilnOwner owner) {
    const long i = token ? find(token) : -1;
    if (i < 0 || live_[static_cast<size_t>(i)].owner != owner) return false;
    live_.erase(live_.begin() + i);
    return true;
}

size_t SnapStore::clear() {
    const size_t n = live_.size();
    live_.clear();
    return n;
}

size_t SnapStore::release_owner(KilnOwner owner) {
    const size_t before = live_.size();
    live_.erase(std::remove_if(live_.begin(), live_.end(), [owner](const SnapEntry& e) { return e.owner == owner; }), live_.end());
    return before - live_.size();
}

}  // namespace kitems::sets
