// SPDX-License-Identifier: MIT
// Player storage's bounded, engine-independent state and KPS1 encoding. All callers serialize Model access.
#pragma once
#include "services/kiln_items_storage.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#define XXH_INLINE_ALL
#include "xxhash/xxhash.h"
namespace kitems::storage {
inline void append_le(std::vector<uint8_t>& out, uint64_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline std::string hex(const std::vector<uint8_t>& b) {
    std::string s;
    for (auto v : b) {
        s += "0123456789abcdef"[v >> 4];
        s += "0123456789abcdef"[v & 15];
    }
    return s;
}
struct Record {
    uint8_t slot = 0;
    uint32_t item = 0;
    uint16_t count = 0;
    std::array<bool, 6> present{};
    std::array<std::vector<uint8_t>, 6> options;
};
inline constexpr unsigned option_sizes[]{1, 8, 8, 4, 2, 5};
inline std::vector<uint8_t> wire_record(const Record& r) {
    std::vector<uint8_t> b;
    append_le(b, r.slot, 1);
    append_le(b, r.item, 4);
    append_le(b, r.count, 2);
    unsigned flags = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (r.present[i])
            flags |= 1u << i;
    append_le(b, flags, 1);
    for (unsigned i = 0; i < 6; ++i)
        if (r.present[i])
            b.insert(b.end(), r.options[i].begin(), r.options[i].end());
    return b;
}
inline bool decode_record(const uint8_t* b, size_t n, Record* r) {
    if (!b || !r || n < 8 || (b[7] & ~63u))
        return false;
    *r = {};
    r->slot = b[0];
    for (unsigned i = 0; i < 4; ++i)
        r->item |= uint32_t(b[1 + i]) << (8 * i);
    r->count = uint16_t(b[5] | uint16_t(b[6]) << 8);
    size_t at = 8;
    for (unsigned i = 0; i < 6; ++i)
        if (b[7] & (1u << i)) {
            if (n - at < option_sizes[i])
                return false;
            r->present[i] = true;
            r->options[i].assign(b + at, b + at + option_sizes[i]);
            at += option_sizes[i];
        }
    return at == n;
}
inline bool canonical_record(const Record& r, std::vector<uint8_t>* b) {
    append_le(*b, r.slot, 1);
    append_le(*b, r.item, 4);
    append_le(*b, r.count, 2);
    for (unsigned i = 0; i < 6; ++i) {
        b->push_back(r.present[i] ? 1 : 0);
        if (r.present[i]) {
            if (r.options[i].size() != option_sizes[i])
                return false;
            b->insert(b->end(), r.options[i].begin(), r.options[i].end());
        }
    }
    return true;
}
struct Inventory {
    std::array<std::optional<Record>, 16> equipment;
    std::array<std::optional<uint16_t>, 4> refs;
    uint8_t main_pages = 0, extra_pages = 0;
    std::vector<Record> main, extra;
};
inline bool encode(const Inventory& in, std::vector<uint8_t>* out) {
    if (!out || in.main_pages > 16 || in.extra_pages > 16 || in.main.size() + in.extra.size() > 248)
        return false;
    std::vector<uint8_t> b{'K', 'P', 'S', '1'};
    uint32_t mask = 0;
    for (unsigned i = 0; i < 16; ++i)
        if (in.equipment[i])
            mask |= 1u << i;
    for (unsigned i = 0; i < 4; ++i)
        if (in.refs[i])
            mask |= 1u << (16 + i);
    append_le(b, mask, 4);
    for (const auto& r : in.equipment) {
        b.push_back(r ? 1 : 0);
        if (r && !canonical_record(*r, &b))
            return false;
    }
    for (const auto& r : in.refs) {
        b.push_back(r ? 1 : 0);
        if (r)
            append_le(b, *r, 2);
    }
    b.insert(b.end(), {in.main_pages, static_cast<uint8_t>(in.main.size()), in.extra_pages,
                       static_cast<uint8_t>(in.extra.size())});
    for (const auto& r : in.main)
        if (!canonical_record(r, &b))
            return false;
    for (const auto& r : in.extra)
        if (!canonical_record(r, &b))
            return false;
    *out = std::move(b);
    return true;
}
inline uint64_t witness(const std::vector<uint8_t>& b) { return XXH3_64bits(b.data(), b.size()); }
inline std::string witness_text(const std::vector<uint8_t>& b) {
    char s[23];
    std::snprintf(s, sizeof s, "xxh3:%016llx", static_cast<unsigned long long>(witness(b)));
    return s;
}
struct Layout {
    int64_t equip0 = 0, equip_stride = 0, refs = 0, main_pages = 0, extra_pages = 0, list = 0, record_stride = 0;
};
inline bool derive_layout(const std::array<int64_t, 8>& v, Layout* l, std::string* why) {
    const char* bad = nullptr;
    if (v[0] < 0 || v[0] > 0x100000)
        bad = "characters/struct.equip0";
    else if (v[1] <= v[0] || v[1] - v[0] < 57 || v[1] - v[0] > 256)
        bad = "characters/struct.equip1";
    else if (v[2] != v[0] + 15 * (v[1] - v[0]))
        bad = "characters/struct.equip_last";
    else if (v[3] < v[2] + 57 || v[3] > 0x100000)
        bad = "characters/struct.refs";
    else if (v[4] < v[3] + 16 || v[4] > 0x100000)
        bad = "characters/struct.main_pages";
    else if (v[5] != v[4] + 12)
        bad = "characters/struct.extra_pages";
    else if (v[6] < v[5] + 12 || v[6] > 0x100000)
        bad = "characters/struct.list";
    else if (v[7] != 56)
        bad = "items/record.stride";
    if (bad) {
        if (why)
            *why = std::string("invalid derived layout: ") + bad;
        return false;
    }
    *l = {v[0], v[1] - v[0], v[3], v[4], v[5], v[6], v[7]};
    return true;
}
inline bool key_ok(const std::string& key) {
    auto colon = key.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 64 || key.size() - colon - 1 == 0 ||
        key.size() - colon - 1 > 32)
        return false;
    for (size_t i = 0; i < key.size(); ++i) {
        if (i == colon)
            continue;
        char c = key[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!alnum && ((i == 0 || i == colon + 1) || (c != '.' && c != '_' && c != '-')))
            return false;
    }
    return true;
}
struct Filter {
    uint32_t categories = 0, flags = 0;
    std::vector<uint32_t> ids;
    bool assign(const KilnStorageFilter& f) {
        if (f.size < sizeof f || (f.flags & ~7u) || f.id_count > KIS_MAX_FILTER_IDS || (f.id_count && !f.ids))
            return false;
        std::vector<uint32_t> v;
        if (f.id_count)
            v.assign(f.ids, f.ids + f.id_count);
        if (!std::is_sorted(v.begin(), v.end()) || std::adjacent_find(v.begin(), v.end()) != v.end())
            return false;
        categories = f.categories;
        flags = f.flags;
        ids = std::move(v);
        return true;
    }
    bool allows(uint32_t item, uint32_t category, uint32_t equip, bool pide) const {
        return (!categories || (category < 32 && (categories & (1u << category)))) &&
               (!(flags & KIS_FILTER_NO_PIDE) || !pide) &&
               (!(flags & 3u) || ((flags & 1u) && equip == 6) || ((flags & 2u) && equip == 7)) &&
               (ids.empty() || std::binary_search(ids.begin(), ids.end(), item));
    }
};
inline bool death_takes(uint32_t rule, uint32_t mode, uint32_t category, bool tombstone, bool restored) {
    if (!restored || !tombstone || rule == KIS_DEATH_KEEP)
        return false;
    if (rule == KIS_DEATH_GRAVE)
        return true;
    return rule == KIS_DEATH_FOLLOW_GAME && (mode == 1 || (mode == 0 && (category == 9 || category == 12)));
}
struct Entry {
    KilnStorageEntry row{};
    std::vector<KilnStorageRecord> records;
};
struct Registration {
    KilnOwner owner = 0;
    uint32_t handle = 0, slots = 0, death = 0;
    std::string key;
    Filter filter;
};
struct Holder {
    uint32_t entity = 0, slots = 0;
    std::vector<uint32_t> pages;
    std::array<uint16_t, 4> refs{KIS_REF_NONE, KIS_REF_NONE, KIS_REF_NONE, KIS_REF_NONE};
    uint32_t stacks = 0;
};
inline uint64_t entries_hash(const std::vector<Entry>& entries) {
    std::vector<uint8_t> bytes;
    for (const auto& e : entries) {
        auto n = std::strlen(e.row.key);
        bytes.insert(bytes.end(), e.row.key, e.row.key + n);
        bytes.push_back(0);
        append_le(bytes, e.row.kind, 4);
        append_le(bytes, e.row.slots, 4);
        append_le(bytes, e.row.reason, 4);
        for (auto ref : e.row.refs)
            append_le(bytes, ref, 2);
        for (const auto& r : e.records) {
            append_le(bytes, r.slot, 2);
            append_le(bytes, r.length, 1);
            append_le(bytes, r.reason, 1);
            if (r.length <= KIS_RECORD_MAX)
                bytes.insert(bytes.end(), r.data, r.data + r.length);
        }
    }
    return witness(bytes);
}
struct Player {
    KilnStorageWitness witness{};
    uint64_t contents_hash = 0;
    bool restored = false, locked = false, applied = false, leaving = false, build_pass_completed = false;
    uint64_t leave_step = 0;
    uint32_t source = 0;
    bool paired = false;
    uint64_t revision = 0, changes = 0;
    std::vector<Entry> entries;
    std::map<std::string, Holder> holders;
};
struct Frozen {
    bool taken = false;
    KilnStorageFrozenBatch header{};
    std::vector<KilnStorageCapture> captures;
    std::vector<KilnStorageEntry> entries;
    std::vector<KilnStorageRecord> records;
};
class Model {
    std::map<uint64_t, Frozen> batches_;
    uint64_t next_join_ = 1, next_tag_ = 1, seq_ = 0, event_seq_ = 0, active_ = 0;
    uint32_t next_handle_ = 1;
    bool bound_ = false;
    KilnOwner persist_ = 0;

  public:
    std::map<uint32_t, Player> players;
    std::map<std::string, Registration> registrations;
    std::deque<KilnStorageEvent> events;
    std::set<KilnOwner> retired;
    void event(uint32_t kind, const Player* p = nullptr, const std::string& key = {}, uint64_t tag = 0,
               uint32_t reason = 0) {
        KilnStorageEvent e{};
        e.size = sizeof e;
        e.kind = kind;
        e.seq = ++event_seq_;
        e.tag = tag;
        e.reason = reason;
        if (p) {
            e.step = p->witness.step;
            e.join = p->witness.join;
            e.slot = p->witness.slot;
            e.player = p->witness.player;
            e.account = p->witness.account;
            std::memcpy(e.character, p->witness.character, 16);
            e.revision = p->revision;
            e.source = p->source;
            e.flags = p->paired ? KIS_EVENT_PAIRED : 0;
        }
        std::snprintf(e.key, sizeof e.key, "%s", key.c_str());
        auto r = registrations.find(key);
        if (r != registrations.end())
            e.handle = r->second.handle;
        events.push_back(e);
        if (events.size() > 256)
            events.pop_front();
    }
    uint64_t last_event() const { return event_seq_; }
    KilnResult bind(KilnOwner owner) {
        if (retired.count(owner))
            return KILN_ERR_REVOKED;
        if (bound_ && persist_ != owner)
            return KILN_ERR_UNAVAILABLE;
        bound_ = true;
        persist_ = owner;
        return KILN_OK;
    }
    bool bound() const { return bound_; }
    bool owns_persist(KilnOwner owner) const { return bound_ && persist_ == owner && !retired.count(owner); }
    void revoke(KilnOwner owner) {
        if (!owner)
            return;
        retired.insert(owner);
        if (bound_ && persist_ == owner)
            bound_ = false;
        for (auto i = registrations.begin(); i != registrations.end();)
            i = i->second.owner == owner ? registrations.erase(i) : std::next(i);
    }
    KilnResult register_def(KilnOwner owner, const KilnStorageDef& d, uint32_t* out) {
        if (out)
            *out = 0;
        if (retired.count(owner))
            return KILN_ERR_REVOKED;
        if (!out || d.size < sizeof d || !d.key.ptr || !d.key.len || d.key.len >= KIS_KEY_MAX || !d.slots ||
            d.slots > KIS_MAX_SLOTS || d.death > 2 || d.flags)
            return KILN_ERR_INVALID_ARGUMENT;
        Registration r;
        r.key.assign(d.key.ptr, d.key.len);
        if (!key_ok(r.key) || registrations.count(r.key) || !r.filter.assign(d.filter))
            return KILN_ERR_INVALID_ARGUMENT;
        uint32_t total = 0, own = 0;
        for (const auto& [k, v] : registrations) {
            total += (v.slots + 7u) / 8u * 8u;
            own += v.owner == owner;
        }
        if (total + (d.slots + 7u) / 8u * 8u > KIS_MAX_SLOTS_PLAYER || own >= KIS_MAX_PER_MOD ||
            registrations.size() >= KIS_MAX_STORAGES)
            return KILN_ERR_UNAVAILABLE;
        r.owner = owner;
        r.handle = next_handle_++;
        r.slots = d.slots;
        r.death = d.death;
        *out = r.handle;
        registrations.emplace(r.key, std::move(r));
        return KILN_OK;
    }
    const Registration* registration(KilnOwner owner, uint32_t handle) const {
        for (const auto& [k, r] : registrations)
            if (r.owner == owner && r.handle == handle)
                return &r;
        return nullptr;
    }
    uint64_t join(uint32_t slot, uint64_t account, const std::array<uint8_t, 16>& id, uint64_t w, uint64_t step,
                  uint32_t flags = 0) {
        if (slot >= 16)
            return 0;
        Player p;
        p.witness.size = sizeof p.witness;
        p.witness.flags = KIS_WITNESS_READ | (flags & KIS_WITNESS_EXTRA_MAIN_PAGES);
        p.witness.join = next_join_++;
        p.witness.witness = w;
        p.witness.step = step;
        p.witness.account = account;
        p.witness.slot = slot;
        std::copy(id.begin(), id.end(), p.witness.character);
        players[slot] = std::move(p);
        event(KIS_EVENT_JOINED, &players[slot]);
        return players[slot].witness.join;
    }
    void applied(uint32_t slot, uint32_t player) {
        auto i = players.find(slot);
        if (i == players.end())
            return;
        i->second.applied = true;
        i->second.witness.player = player;
        i->second.witness.flags |= KIS_WITNESS_APPLIED;
    }
    bool pending_build(uint32_t slot) const {
        auto i = players.find(slot);
        return i != players.end() && i->second.restored && !i->second.locked &&
               !(i->second.witness.flags & KIS_WITNESS_LEFT);
    }
    bool build_ready(uint32_t slot) const {
        auto i = players.find(slot);
        return pending_build(slot) && i->second.applied;
    }
    KilnResult restore(uint32_t slot, uint64_t join, const KilnStorageDoc& d) {
        auto i = players.find(slot);
        if (i == players.end() || i->second.witness.join != join || (i->second.witness.flags & KIS_WITNESS_LEFT))
            return KILN_ERR_NOT_FOUND;
        auto& p = i->second;
        if (p.restored || d.size < sizeof d || (d.flags & ~3u) || d.source < 1 || d.source > 3 || d.reserved ||
            d.entry_count > KIS_MAX_ENTRIES || d.record_count > KIS_MAX_RECORDS || (d.entry_count && !d.entries) ||
            (d.record_count && !d.records) || d.account != p.witness.account ||
            std::memcmp(d.character, p.witness.character, 16))
            return KILN_ERR_INVALID_ARGUMENT;
        std::vector<Entry> entries;
        std::set<std::string> keys, live;
        std::vector<bool> used(d.record_count, false);
        for (uint32_t n = 0; n < d.entry_count; ++n) {
            const auto& e = d.entries[n];
            const char* end = static_cast<const char*>(std::memchr(e.key, 0, sizeof e.key));
            if (!end)
                return KILN_ERR_INVALID_ARGUMENT;
            std::string key(e.key, end);
            if (e.size < sizeof e || !key_ok(key) || e.kind < KIS_ENTRY_LIVE || e.kind > KIS_ENTRY_QUARANTINE || !e.slots ||
                e.slots > 256 || e.first > d.record_count || e.count > d.record_count - e.first)
                return KILN_ERR_INVALID_ARGUMENT;
            if (e.kind == KIS_ENTRY_LIVE && !live.insert(key).second)
                return KILN_ERR_INVALID_ARGUMENT;
            keys.insert(key);
            if (keys.size() > KIS_MAX_STORAGES)
                return KILN_ERR_INVALID_ARGUMENT;
            for (uint32_t k = e.first; k < e.first + e.count; ++k) {
                if (used[k])
                    return KILN_ERR_INVALID_ARGUMENT;
                used[k] = true;
            }
            Entry copy;
            copy.row = e;
            copy.row.first = 0;
            if (e.count)
                copy.records.assign(d.records + e.first, d.records + e.first + e.count);
            entries.push_back(std::move(copy));
        }
        if (std::count(used.begin(), used.end(), true) != d.record_count)
            return KILN_ERR_INVALID_ARGUMENT;
        p.entries = std::move(entries);
        p.contents_hash = entries_hash(p.entries);
        p.restored = true;
        p.locked = d.flags & KIS_DOC_LOCKED;
        p.source = d.source;
        p.paired = d.flags & KIS_DOC_PAIRED;
        p.revision = d.revision;
        p.witness.flags |= KIS_WITNESS_RESTORED;
        return KILN_OK;
    }
    void mark_built(uint32_t slot, const std::string& key, uint32_t entity, std::vector<uint32_t> pages) {
        auto i = players.find(slot);
        if (i == players.end())
            return;
        auto& h = i->second.holders[key];
        h.entity = entity;
        h.pages = std::move(pages);
        auto r = registrations.find(key);
        h.slots = r == registrations.end() ? static_cast<uint32_t>(h.pages.size() * 8) : r->second.slots;
    }
    uint32_t holder(KilnOwner owner, uint32_t handle, uint32_t player) const {
        const auto* r = registration(owner, handle);
        if (!r)
            return 0;
        for (const auto& [s, p] : players)
            if (p.witness.player == player) {
                auto h = p.holders.find(r->key);
                return h == p.holders.end() ? 0 : h->second.entity;
            }
        return 0;
    }
    uint32_t entity_owner(uint32_t entity) const {
        if (!entity)
            return 0;
        for (const auto& [s, p] : players)
            for (const auto& [k, h] : p.holders)
                if (h.entity == entity || std::find(h.pages.begin(), h.pages.end(), entity) != h.pages.end())
                    return p.witness.player;
        return 0;
    }
    bool is_holder(uint32_t entity) const {
        for (const auto& [s, p] : players)
            for (const auto& [k, h] : p.holders)
                if (h.entity == entity)
                    return true;
        return false;
    }
    bool find_kind(uint32_t entity, uint32_t flags) const {
        return !is_holder(entity) || bool(flags & KILN_ITEMS_FIND_PLAYER_STORAGE);
    }
    bool transfer_denied(uint32_t from, uint32_t to) const { return entity_owner(from) || entity_owner(to); }
    bool actor_allowed(uint32_t entity, uint32_t actor) const {
        auto owner = entity_owner(entity);
        return !owner || owner == actor;
    }
    uint64_t begin_batch(uint64_t saves_seq, uint32_t revision, uint64_t step) {
        if (active_)
            complete();
        Frozen f;
        f.header.size = sizeof f.header;
        f.header.tag = next_tag_++;
        f.header.saves_seq = saves_seq;
        f.header.revision = revision;
        f.header.first_step = f.header.last_step = step;
        active_ = f.header.tag;
        batches_.emplace(active_, std::move(f));
        while (batches_.size() > 16)
            batches_.erase(batches_.begin());
        return active_;
    }
    uint64_t active_tag() const { return active_; }
    void metadata(uint64_t tag, int64_t unix_ms, const uint8_t guid[16]) {
        auto i = batches_.find(tag);
        if (i == batches_.end())
            return;
        i->second.header.unix_ms = unix_ms;
        std::memcpy(i->second.header.world_guid, guid, 16);
    }
    void capture(uint32_t slot, uint64_t w, const std::vector<Entry>& entries, bool refused, uint32_t reason) {
        auto i = players.find(slot);
        if (i == players.end() || !active_)
            return;
        auto& p = i->second;
        auto& f = batches_.at(active_);
        if (!refused && p.restored && !p.locked) {
            auto hash = entries_hash(entries);
            if (hash != p.contents_hash) {
                p.contents_hash = hash;
                ++p.changes;
            }
        }
        KilnStorageCapture c{};
        c.size = sizeof c;
        c.seq = ++seq_;
        c.join = p.witness.join;
        c.step = p.witness.step;
        c.witness = w;
        c.revision = p.revision;
        c.changes = p.changes;
        c.account = p.witness.account;
        std::memcpy(c.character, p.witness.character, 16);
        c.player = p.witness.player;
        c.slot = slot;
        c.first_entry = static_cast<uint32_t>(f.entries.size());
        if (refused) {
            c.flags = KIS_CAPTURE_REFUSED;
            c.reason = reason;
            f.header.flags |= KIS_BATCH_REFUSED;
        } else if (!p.restored)
            c.flags = KIS_CAPTURE_UNRESTORED;
        else if (p.locked)
            c.flags = KIS_CAPTURE_LOCKED;
        else {
            size_t count = 0;
            for (const auto& e : entries)
                count += e.records.size();
            if (entries.size() > KIS_MAX_ENTRIES || count > KIS_MAX_RECORDS || f.captures.size() >= 16) {
                c.flags = KIS_CAPTURE_REFUSED;
                c.reason = 7;
                f.header.flags |= KIS_BATCH_REFUSED;
            } else
                for (auto& e : entries) {
                    auto row = e.row;
                    row.first = static_cast<uint32_t>(f.records.size());
                    row.count = static_cast<uint32_t>(e.records.size());
                    f.entries.push_back(row);
                    f.records.insert(f.records.end(), e.records.begin(), e.records.end());
                    ++c.entry_count;
                }
        }
        if (f.captures.size() < 16)
            f.captures.push_back(c);
        p.witness.captured = w;
        p.witness.capture_seq = c.seq;
        event(KIS_EVENT_CAPTURED, &p, {}, active_);
        f.header.capture_count = static_cast<uint32_t>(f.captures.size());
        f.header.entry_count = static_cast<uint32_t>(f.entries.size());
        f.header.record_count = static_cast<uint32_t>(f.records.size());
        f.header.last_step = c.step;
    }
    bool flag_leaving(uint64_t tag, uint32_t slot, uint64_t join) {
        auto i = batches_.find(tag);
        if (i == batches_.end())
            return false;
        for (auto& c : i->second.captures)
            if (c.slot == slot && c.join == join) {
                c.flags |= KIS_CAPTURE_LEFT;
                return true;
            }
        return false;
    }
    void complete() {
        if (!active_)
            return;
        batches_.at(active_).header.flags |= KIS_BATCH_COMPLETE;
        event(KIS_EVENT_BATCH, nullptr, {}, active_);
        active_ = 0;
    }
    KilnResult take(uint64_t tag, Frozen* out, bool mark = true) {
        auto i = batches_.find(tag);
        if (i == batches_.end())
            return KILN_ERR_NOT_FOUND;
        if (mark)
            i->second.taken = true;
        *out = i->second;
        return KILN_OK;
    }
    void mark_taken(uint64_t tag) {
        auto i = batches_.find(tag);
        if (i != batches_.end())
            i->second.taken = true;
    }
    KilnResult release(uint64_t tag) {
        if (tag == active_)
            return KILN_ERR_UNAVAILABLE;
        return batches_.erase(tag) ? KILN_OK : KILN_ERR_NOT_FOUND;
    }
    size_t batch_count() const { return batches_.size(); }
    void reset_session() {
        players.clear();
        batches_.clear();
        events.clear();
        active_ = 0;
    }
    uint32_t drop_complete() {
        uint32_t dropped = 0;
        for (auto i = batches_.begin(); i != batches_.end();)
            if ((i->second.header.flags & KIS_BATCH_COMPLETE) && !i->second.taken) {
                i = batches_.erase(i);
                ++dropped;
            } else
                ++i;
        return dropped;
    }
    void leave(uint32_t slot) {
        auto i = players.find(slot);
        if (i == players.end())
            return;
        auto& p = i->second;
        p.holders.clear();
        p.witness.flags |= KIS_WITNESS_LEFT;
        for (auto& [t, f] : batches_)
            for (auto& c : f.captures)
                if (c.join == p.witness.join)
                    c.flags |= KIS_CAPTURE_LEFT;
        event(KIS_EVENT_LEFT, &p);
    }
};
} // namespace kitems::storage
