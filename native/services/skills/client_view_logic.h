// SPDX-License-Identifier: MIT
// kiln.skills.client (native/core/abi/services/kiln_skills_client.h, since 0.2.0-dev.31.13): the pure part, host-tested
// (tests/test_client_view.cpp): one keen::SkillTreeNode and one link decoded from the loaded resource, the screen's
// record of a node merged into a level, the records map's probe (the engine's open-addressing HashMap), the point
// triple's check, and the bounded caller buffers. client_view.cpp reads the client world's memory on its thread.
// Evidence: docs/re/progression.md section 8.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../../core/abi/services/kiln_skills_client.h"

namespace kiln::skills::client {

// keen::SkillTreeNode (0x8c bytes; tree.h's F:: has the same offsets, asserted in client_view.cpp).
struct NodeOffsets {
    static constexpr size_t size = 0x8c, id = 0x00, pos = 0x04, in = 0x74, out = 0x7c, cost = 0x84, type = 0x86,
                            cls = 0x87, max_level = 0x88, level_index = 0x89;
};
constexpr size_t kLinkSize = 0x10;  // keen::SkillTreeNodeLink {id, source, target, bidirectional}

template <class T>
T rd(const uint8_t* p, size_t off) {
    T v;
    std::memcpy(&v, p + off, sizeof v);
    return v;
}

// The screen's record of one node (G38UiPlayData's map, 0x44 bytes): level +0x41, unlocked +0x43.
struct Record {
    bool found = false;
    uint8_t level = 0;
    uint8_t unlocked = 0;
};

// A node of the resource and the player's record of it. The record's level byte is meaningful only for a node with
// levels (skilltree_ui decodes PlayerSkillsUi.unlockLevel[levelIndex] for every node, levelIndex 0 for the plain ones):
// a plain node is level 1 when unlocked. A node with levels that is unlocked is at least level 1, at most max_level.
inline KilnSkillsClientNode node_of(const uint8_t* n, uint16_t index, const Record& rec, bool shadow) {
    using F = NodeOffsets;
    KilnSkillsClientNode o{};
    o.id = rd<uint32_t>(n, F::id);
    o.index = index;
    o.cost = rd<uint16_t>(n, F::cost);
    o.type = n[F::type];
    o.class_alignment = n[F::cls];
    o.max_level = n[F::max_level];
    o.level_index = n[F::level_index];
    o.x = rd<float>(n, F::pos);
    o.y = rd<float>(n, F::pos + 4);
    o.incoming = rd<uint32_t>(n, F::in + 4);
    o.outgoing = rd<uint32_t>(n, F::out + 4);
    if (rec.found) o.flags |= KILN_SKILLS_CLIENT_RECORD;
    if (shadow) o.flags |= KILN_SKILLS_CLIENT_SHADOW;
    if (rec.found && rec.unlocked) {
        o.level = o.max_level ? static_cast<uint8_t>(std::clamp<int>(rec.level, 1, o.max_level)) : 1;
        o.flags |= KILN_SKILLS_CLIENT_UNLOCKED;
    }
    return o;
}

inline KilnSkillsClientLink link_of(const uint8_t* l) {
    KilnSkillsClientLink o{};
    o.id = rd<uint32_t>(l, 0);
    o.from = rd<uint32_t>(l, 4);
    o.to = rd<uint32_t>(l, 8);
    o.bidirectional = l[12] ? 1 : 0;
    return o;
}

// The engine's HashMap probe as Kiln's screen hook walks it (shadow.cpp write_records): slot = id & (cap - 1), on while
// the slot's bit is set, until the key matches. -1: not there, or a map that is not one (cap 0 or not a power of two).
template <class BitAt, class KeyAt>
int64_t find_slot(uint32_t id, uint64_t cap, BitAt bit, KeyAt key) {
    if (!cap || (cap & (cap - 1))) return -1;
    uint64_t slot = id & (cap - 1);
    for (uint64_t n = 0; n < cap && bit(slot); ++n) {
        if (key(slot) == id) return static_cast<int64_t>(slot);
        slot = (slot + 1) & (cap - 1);
    }
    return -1;
}

// skilltree_ui's triple: available = total - spent when total > spent, else 0. A triple that is not one (a read between
// two writes, a moved offset) is refused and the copy keeps the last good one.
inline bool points_ok(int32_t total, int32_t spent, int32_t available) {
    if (total < 0 || spent < 0 || total > 100000 || spent > 100000) return false;
    return available == (total > spent ? total - spent : 0);
}

// The resource's header (keen::SkillTreeResource: nodes rel32 +0 / count +4, links rel32 +8 / count +12): the arrays'
// offsets from the resource's start, or false for counts out of range.
struct Arrays {
    int64_t nodes = 0, links = 0;
    uint32_t node_count = 0, link_count = 0;
};
inline bool arrays_of(const uint8_t header[16], Arrays* a) {
    const int32_t no = rd<int32_t>(header, 0), lo = rd<int32_t>(header, 8);
    a->node_count = rd<uint32_t>(header, 4);
    a->link_count = rd<uint32_t>(header, 12);
    a->nodes = 0 + static_cast<int64_t>(no);
    a->links = 8 + static_cast<int64_t>(lo);
    return a->node_count > 0 && a->node_count <= KILN_SKILLS_CLIENT_MAX_NODES && a->link_count <= KILN_SKILLS_CLIENT_MAX_LINKS &&
           a->nodes >= 16 && (a->link_count == 0 || a->links >= 16);
}

template <class Row>
KilnResult copy_out(const std::vector<Row>& rows, Row* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    *out_count = static_cast<uint32_t>(rows.size());
    const uint32_t n = std::min<uint32_t>(capacity, static_cast<uint32_t>(rows.size()));
    if (n) std::memcpy(out, rows.data(), n * sizeof(Row));
    return n < rows.size() ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

inline bool demanded(uint64_t last_read_ms, uint64_t now_ms) {
    return last_read_ms != 0 && now_ms >= last_read_ms && now_ms - last_read_ms <= KILN_SKILLS_CLIENT_DEMAND_MS;
}

}  // namespace kiln::skills::client
