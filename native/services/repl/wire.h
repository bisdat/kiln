// SPDX-License-Identifier: MIT
// kiln.repl: the game's replication messages and state blobs read as plain data (the dossier's chapter 11 §2.3), shared
// by the lab harness, the stats and watch hooks and the host tests. Pure functions over bytes: no game call, no
// allocation, no global state. Callers on the game's threads run them inside their guard.
#pragma once
#include <cstdint>
#include <cstring>

#include "layout.h"
#include "services/kiln_repl.h"

namespace repl {

inline uint32_t u32_at(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}
inline uint16_t u16_at(const uint8_t* p) {
    uint16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// An entity message (a MsgEntry of type 4): `u32 (tick << 3) | kind`, `u32 seq`, `u32 entity`; a create then `u32 parent,
// GUID[16], u8 0`; an update `u32 baseline` (bit 31: the acknowledged state A) and the GUID only when the baseline is 0;
// then `u16 length` and the payload.
struct Header {
    uint32_t kind, tick, seq, entity, base;
    bool has_guid;
    uint8_t guid[16];
    uint32_t payload_len;
    const uint8_t* payload;
};
// False when it is not a create or update, or is cut short.
inline bool parse_header(const uint8_t* d, uint64_t n, Header* h) {
    if (n < 12) return false;
    const uint32_t w0 = u32_at(d);
    h->kind = w0 & 7;
    h->tick = w0 >> 3;
    h->seq = u32_at(d + 4);
    h->entity = u32_at(d + 8);
    h->has_guid = false;
    h->base = 0;
    uint64_t o = 12;
    if (h->kind == kKindCreate) {
        if (n < o + 4 + 16 + 1 + 2) return false;
        o += 4;  // parent
        std::memcpy(h->guid, d + o, 16);
        o += 16 + 1;
        h->has_guid = true;
    } else if (h->kind == kKindUpdate) {
        if (n < o + 4 + 2) return false;
        h->base = u32_at(d + o) & 0x7fffffffu;
        o += 4;
        if (h->base == 0) {
            if (n < o + 16 + 2) return false;
            std::memcpy(h->guid, d + o, 16);
            o += 16;
            h->has_guid = true;
        }
    } else {
        return false;
    }
    const uint16_t len = u16_at(d + o);
    o += 2;
    if (o + len > n) return false;
    h->payload_len = len;
    h->payload = d + o;
    return true;
}

// A destroy list (a MsgEntry of type 5, header kind 4): `u32 (tick << 3) | 4`, `u32 seq`, `u8 nA`, `u8 nB`, then the ids:
// list A (destroyed) first, then list B (left the peer's relevance).
struct DestroyList {
    uint32_t tick, seq;
    uint32_t na, nb;       // the ids present (cut to what the message holds)
    const uint8_t* ids;    // (na + nb) u32
};
inline bool parse_destroy(const uint8_t* d, uint64_t n, DestroyList* l) {
    if (n < 10 || (u32_at(d) & 7) != kKindDestroyList) return false;
    l->tick = u32_at(d) >> 3;
    l->seq = u32_at(d + 4);
    const uint64_t room = (n - 10) / 4;
    uint64_t a = d[8], b = d[9];
    if (a > room) a = room;
    if (a + b > room) b = room - a;
    l->na = static_cast<uint32_t>(a);
    l->nb = static_cast<uint32_t>(b);
    l->ids = d + 10;
    return true;
}

// What one send-callback batch counts for (KilnReplPeerStats): the session takes its messages in order and, at the first
// one it cannot allocate, hands the rest back (they are "refused"), so the accepted ones are msgs[0 .. sent).
struct BatchCount {
    uint64_t messages = 0, bytes = 0, refused = 0, creates = 0, updates = 0, destroy_lists = 0, destroyed_ids = 0;
};
inline BatchCount count_batch(const MsgEntry* msgs, uint32_t count, uint32_t sent) {
    BatchCount c;
    const uint32_t ok = sent < count ? sent : count;
    c.refused = count - ok;
    c.messages = ok;
    for (uint32_t i = 0; i < ok; ++i) {
        const MsgEntry& m = msgs[i];
        c.bytes += m.size;
        if (!m.data) continue;
        if (m.type == kMsgEntity && m.size >= 4) {
            const uint32_t kind = m.data[0] & 7;
            c.creates += kind == kKindCreate;
            c.updates += kind == kKindUpdate;
        } else if (m.type == kMsgList) {
            DestroyList l;
            if (parse_destroy(m.data, m.size, &l)) {
                ++c.destroy_lists;
                c.destroyed_ids += l.na + l.nb;
            }
        }
    }
    return c;
}

// The components of a state blob that carry data (its list 1): registry index, the data's offset in the blob, its size.
// `reg` has `count` and `at[i].size` / `at[i].flags` (KILN_REPL_COMPONENT_VERSIONED: a version byte follows the index);
// index(buf, state) fills the game's StateIndex result into a zeroed buffer of kIndexBytes (false: it faulted), which
// gives each component's data offset at buf + kIndexOffsets (u16 per index; data at state + 8 + offset).
struct Part {
    uint16_t index;
    uint32_t off;
    uint16_t size;
};
template <class Reg, class IndexFn>
inline bool state_parts(const Reg& reg, const uint8_t* s, IndexFn&& index, Part* out, uint32_t cap, uint32_t* n) {
    *n = 0;
    alignas(16) uint8_t buf[kIndexBytes];
    std::memset(buf, 0, sizeof buf);
    if (!index(buf, s)) return false;
    const uint16_t total = u16_at(s), count = u16_at(s + 2);
    uint32_t o = kStateHeader;
    for (uint32_t k = 0; k < count; ++k) {
        if (o + 2 > total || *n >= cap) return false;
        const uint16_t i = u16_at(s + o);
        o += 2;
        if (i >= reg.count) return false;
        if (reg.at[i].flags & KILN_REPL_COMPONENT_VERSIONED) o += 1;
        const uint16_t off = u16_at(buf + kIndexOffsets + i * 2);
        const uint32_t at = static_cast<uint32_t>(kStateHeader) + off;
        if (at + reg.at[i].size > total) return false;
        out[(*n)++] = Part{i, at, reg.at[i].size};
    }
    return true;
}

}  // namespace repl
