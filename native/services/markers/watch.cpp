// SPDX-License-Identifier: MIT
// kiln.markers' watch table (anchors.h).
#include "anchors.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace kmk {

namespace {

constexpr uint32_t kFilterMask = 0xffffu;

void say(char* why, size_t cap, const char* fmt, ...) {
    if (!why || !cap) return;
    va_list v;
    va_start(v, fmt);
    std::vsnprintf(why, cap, fmt, v);
    va_end(v);
}

bool kind_ok(uint32_t kind) { return kind == KILN_WORLD_SERVER || kind == KILN_WORLD_CLIENT; }

}  // namespace

Watches::Watches() {
    for (uint32_t i = 0; i < kWatchSlots; ++i) free_slots_[i] = kWatchSlots - 1 - i;
    free_slot_count_ = kWatchSlots;
    for (uint32_t i = 0; i < kLeasesTotal; ++i) free_leases_[i] = kLeasesTotal - 1 - i;
    free_lease_count_ = kLeasesTotal;
    for (auto& f : filter_) f.store(0, std::memory_order_relaxed);
    // sized once: inserts made by captures do not rehash
    by_serial_.reserve(kWatchSlots * 2);
    by_entity_.reserve(kWatchSlots * 2);
    by_handle_.reserve(kLeasesTotal * 2);
    per_owner_.reserve(512);
}

// ---- private helpers (under mu_) ----------------------------------------------------------------------------------------

void Watches::end_slot(Slot& s) {
    if (!s.serial || !s.live) return;
    s.live = false;
    s.reason = KILN_MARKER_ANCHOR_ENDED;
    filter_[s.entity & kFilterMask].fetch_sub(1, std::memory_order_seq_cst);
    live_.fetch_sub(1, std::memory_order_seq_cst);
    ended_.fetch_add(1, std::memory_order_relaxed);
}

void Watches::end_matching(uint32_t kind, uint32_t id) {
    for (Slot& s : slots_)
        if (s.serial && s.live && s.kind == kind && s.entity == id) end_slot(s);
}

void Watches::maybe_free(uint32_t index) {
    Slot& s = slots_[index];
    if (!s.serial || s.leases || s.refs) return;
    if (s.live) {
        filter_[s.entity & kFilterMask].fetch_sub(1, std::memory_order_seq_cst);
        live_.fetch_sub(1, std::memory_order_seq_cst);
    }
    by_serial_.erase(s.serial);
    if (auto it = by_entity_.find(key(s.epoch, s.entity)); it != by_entity_.end() && it->second == index) by_entity_.erase(it);
    s = Slot{};
    free_slots_[free_slot_count_++] = index;
}

void Watches::drop_lease(uint32_t index) {
    Lease& l = leases_[index];
    if (!l.handle) return;
    Slot& s = slots_[l.slot];
    if (s.leases) --s.leases;
    if (per_kind_[l.kind]) --per_kind_[l.kind];
    if (auto it = per_owner_.find(l.owner); it != per_owner_.end() && --it->second == 0) per_owner_.erase(it);
    by_handle_.erase(l.handle);
    const uint32_t slot = l.slot;
    l = Lease{};
    free_leases_[free_lease_count_++] = index;
    maybe_free(slot);
}

void Watches::stop_locked(uint32_t kind) {
    storage_[kind] = 0;
    epoch_[kind] = 0;
    for (Slot& s : slots_)
        if (s.serial && s.kind == kind) end_slot(s);
    for (uint32_t i = 0; i < kLeasesTotal; ++i)
        if (leases_[i].handle && leases_[i].kind == kind) drop_lease(i);
}

uint32_t Watches::room(KilnOwner owner, uint32_t kind, char* why, size_t cap) const {
    const auto it = per_owner_.find(owner);
    if (it != per_owner_.end() && it->second >= kLeasesPerOwner) {
        say(why, cap, "Full: this publisher holds %u anchor leases (the most); release some (MarkerAnchor.Dispose)", kLeasesPerOwner);
        return KILN_MARKER_FULL;
    }
    if (per_kind_[kind] >= kLeasesPerKind) {
        say(why, cap, "Full: %u anchor leases are held on the %s world (the most)", kLeasesPerKind, kind == KILN_WORLD_CLIENT ? "client" : "server");
        return KILN_MARKER_FULL;
    }
    if (free_lease_count_ == 0) {
        say(why, cap, "Full: %u anchor leases are held in this process (the most)", kLeasesTotal);
        return KILN_MARKER_FULL;
    }
    return KILN_MARKER_ACCEPTED;
}

// ---- worlds -------------------------------------------------------------------------------------------------------------

void Watches::world_started(uint32_t kind, uintptr_t storage, uint64_t epoch) {
    if (!kind_ok(kind)) return;
    std::lock_guard g(mu_);
    if (storage_[kind] || epoch_[kind]) stop_locked(kind);  // a start without a stop: the old world's watches end here
    storage_[kind] = storage;
    epoch_[kind] = epoch;
}

void Watches::world_stopping(uint32_t kind) {
    if (!kind_ok(kind)) return;
    std::lock_guard g(mu_);
    stop_locked(kind);
}

// ---- observers ----------------------------------------------------------------------------------------------------------

void Watches::structural(uintptr_t storage, uint32_t id) {
    if (!id || !storage) return;
    if (live_.load(std::memory_order_seq_cst) == 0) return;
    if (filter_[id & kFilterMask].load(std::memory_order_seq_cst) == 0) return;
    std::lock_guard g(mu_);
    for (uint32_t kind : {static_cast<uint32_t>(KILN_WORLD_SERVER), static_cast<uint32_t>(KILN_WORLD_CLIENT)})
        if (storage_[kind] == storage) end_matching(kind, id);
}

void Watches::client_create(uint32_t id) {
    if (!id) return;
    if (live_.load(std::memory_order_seq_cst) == 0) return;
    if (filter_[id & kFilterMask].load(std::memory_order_seq_cst) == 0) return;
    std::lock_guard g(mu_);
    end_matching(KILN_WORLD_CLIENT, id);
}

void Watches::end_all(uint32_t kind) {
    std::lock_guard g(mu_);
    for (Slot& s : slots_)
        if (s.serial && s.kind == kind) end_slot(s);
}

// ---- capture ------------------------------------------------------------------------------------------------------------

uint32_t Watches::begin(KilnOwner owner, uint32_t kind, uint64_t epoch, uint32_t entity, Pending* p, char* why, size_t cap) {
    *p = Pending{};
    if (!kind_ok(kind)) {
        say(why, cap, "NotReady: no world of kind %u", kind);
        return KILN_MARKER_NOT_READY;
    }
    std::lock_guard g(mu_);
    if (!epoch_[kind] || epoch_[kind] != epoch) {
        say(why, cap, "NotReady: the %s world of epoch %llu is not running (its epoch is %llu now)", kind == KILN_WORLD_CLIENT ? "client" : "server",
            static_cast<unsigned long long>(epoch), static_cast<unsigned long long>(epoch_[kind]));
        return KILN_MARKER_NOT_READY;
    }
    if (epoch >> 32) {  // the (epoch, entity) key holds 32 bits of epoch: past that, anchors stop (chapter: overflow disables)
        say(why, cap, "NotReady: world epochs ran out for anchors in this process");
        return KILN_MARKER_NOT_READY;
    }
    if (const uint32_t st = room(owner, kind, why, cap); st != KILN_MARKER_ACCEPTED) return st;
    p->kind = kind;
    p->entity = entity;
    p->epoch = epoch;
    if (auto it = by_entity_.find(key(epoch, entity)); it != by_entity_.end()) {
        const Slot& s = slots_[it->second];
        if (s.serial && s.live && s.kind == kind && s.epoch == epoch && s.entity == entity) {
            p->slot = it->second;
            p->serial = s.serial;
            return KILN_MARKER_ACCEPTED;
        }
    }
    if (!free_slot_count_) {
        say(why, cap, "Full: kiln.markers watches %u entities (the most)", kWatchSlots);
        return KILN_MARKER_FULL;
    }
    if (next_serial_ == UINT64_MAX) {
        say(why, cap, "Full: anchor serials ran out in this process");
        return KILN_MARKER_FULL;
    }
    const uint32_t index = free_slots_[--free_slot_count_];
    Slot& s = slots_[index];
    s = Slot{};
    s.serial = ++next_serial_;
    s.epoch = epoch;
    s.entity = entity;
    s.kind = kind;
    s.live = true;
    // watched from here on, before the read: a structural attempt meanwhile ends it and finish sees that
    filter_[entity & kFilterMask].fetch_add(1, std::memory_order_seq_cst);
    live_.fetch_add(1, std::memory_order_seq_cst);
    by_serial_[s.serial] = index;
    by_entity_[key(epoch, entity)] = index;
    p->slot = index;
    p->serial = s.serial;
    p->fresh = true;
    return KILN_MARKER_ACCEPTED;
}

uint32_t Watches::finish(KilnOwner owner, const Pending& p, const TransformRead& read, KilnMarkerAnchor* handle, char* why, size_t cap) {
    *handle = 0;
    std::lock_guard g(mu_);
    Slot& s = slots_[p.slot];
    const bool same = s.serial == p.serial && s.serial != 0;
    if (!same || !s.live) {
        if (same && p.fresh) maybe_free(p.slot);
        say(why, cap, "NotFound: entity %u was destroyed or recreated while it was read; capture it again", p.entity);
        return KILN_MARKER_NOT_FOUND;
    }
    if (read.kind != ReadKind::Ok) {
        if (p.fresh) maybe_free(p.slot);
        switch (read.kind) {
            case ReadKind::BadPosition:
                say(why, cap, "NotFound: entity %u is outside the world (x and z in [-1000, 12000], y in [-1000, 5000] metres)", p.entity);
                break;
            case ReadKind::Fault:
                say(why, cap, "NotFound: reading entity %u's position faulted", p.entity);
                break;
            case ReadKind::WrongSize:
                say(why, cap, "NotFound: entity %u's transform is not the layout this build was checked for", p.entity);
                break;
            default:
                say(why, cap, "NotFound: no entity %u with a position in the %s world (not created yet, not replicated here, or gone)", p.entity,
                    p.kind == KILN_WORLD_CLIENT ? "client" : "server");
                break;
        }
        return KILN_MARKER_NOT_FOUND;
    }
    if (const uint32_t st = room(owner, p.kind, why, cap); st != KILN_MARKER_ACCEPTED) {
        if (p.fresh) maybe_free(p.slot);
        return st;
    }
    const uint32_t li = free_leases_[--free_lease_count_];
    Lease& l = leases_[li];
    l.handle = ++next_handle_;
    l.owner = owner;
    l.slot = p.slot;
    l.kind = p.kind;
    by_handle_[l.handle] = li;
    ++per_owner_[owner];
    ++per_kind_[p.kind];
    ++s.leases;
    s.reason = KILN_MARKER_READY;  // the newest read
    s.position = read.position;
    *handle = l.handle;
    return KILN_MARKER_ACCEPTED;
}

KilnResult Watches::release(KilnOwner owner, KilnMarkerAnchor handle) {
    std::lock_guard g(mu_);
    const auto it = by_handle_.find(handle);
    if (!handle || it == by_handle_.end() || leases_[it->second].owner != owner) return KILN_ERR_NOT_FOUND;
    drop_lease(it->second);
    return KILN_OK;
}

void Watches::release_owner(KilnOwner owner) {
    std::lock_guard g(mu_);
    if (!per_owner_.count(owner)) return;
    for (uint32_t i = 0; i < kLeasesTotal; ++i)
        if (leases_[i].handle && leases_[i].owner == owner) drop_lease(i);
}

// ---- references ---------------------------------------------------------------------------------------------------------

uint32_t Watches::retain(KilnOwner owner, uint32_t kind, KilnMarkerAnchor handle, uint64_t* serial, char* why, size_t cap) {
    *serial = 0;
    std::lock_guard g(mu_);
    const auto it = by_handle_.find(handle);
    if (!handle || it == by_handle_.end() || leases_[it->second].owner != owner) {
        say(why, cap, "InvalidAnchor: the follow lease %016llx is not a live lease of this publisher (released, another publisher's, or of a world "
                      "that stopped)",
            static_cast<unsigned long long>(handle));
        return KILN_MARKER_INVALID_ANCHOR;
    }
    const Lease& l = leases_[it->second];
    if (l.kind != kind) {
        say(why, cap, "InvalidAnchor: the follow lease is of the %s world; this marker follows entities of the %s world",
            l.kind == KILN_WORLD_CLIENT ? "client" : "server", kind == KILN_WORLD_CLIENT ? "client" : "server");
        return KILN_MARKER_INVALID_ANCHOR;
    }
    Slot& s = slots_[l.slot];
    if (!epoch_[kind] || s.epoch != epoch_[kind]) {
        say(why, cap, "InvalidAnchor: the follow lease is of a world that stopped");
        return KILN_MARKER_INVALID_ANCHOR;
    }
    if (!s.live) {
        say(why, cap, "InvalidAnchor: entity %u, which the lease followed, was destroyed, unloaded or recreated; capture it again", s.entity);
        return KILN_MARKER_INVALID_ANCHOR;
    }
    ++s.refs;
    ++refs_;
    *serial = s.serial;
    return KILN_MARKER_ACCEPTED;
}

void Watches::drop(uint64_t serial) {
    if (!serial) return;
    std::lock_guard g(mu_);
    const auto it = by_serial_.find(serial);
    if (it == by_serial_.end()) return;
    Slot& s = slots_[it->second];
    if (!s.refs) return;
    --s.refs;
    --refs_;
    maybe_free(it->second);
}

// ---- follow reads -------------------------------------------------------------------------------------------------------

uint32_t Watches::to_read(uint32_t kind, ToRead* out, uint32_t cap) const {
    std::lock_guard g(mu_);
    uint32_t n = 0;
    for (uint32_t i = 0; i < kWatchSlots && n < cap; ++i) {
        const Slot& s = slots_[i];
        if (s.serial && s.live && s.kind == kind && s.refs) out[n++] = ToRead{i, s.entity, s.serial};
    }
    return n;
}

void Watches::read_done(const ToRead& t, const TransformRead& r) {
    if (t.slot >= kWatchSlots) return;
    std::lock_guard g(mu_);
    Slot& s = slots_[t.slot];
    if (s.serial != t.serial || !s.live) return;  // ended (or gone) meanwhile: an end is terminal
    switch (r.kind) {
        case ReadKind::Ok:
            s.reason = KILN_MARKER_READY;
            s.position = r.position;
            break;
        case ReadKind::BadPosition:
            s.reason = KILN_MARKER_BAD_POSITION;
            break;
        default:
            s.reason = KILN_MARKER_FOLLOW_MISSING;
            break;
    }
}

bool Watches::follow(uint64_t serial, KilnVec3* position, uint32_t* reason) const {
    if (!serial) return false;
    std::lock_guard g(mu_);
    const auto it = by_serial_.find(serial);
    if (it == by_serial_.end()) return false;
    const Slot& s = slots_[it->second];
    if (reason) *reason = s.live ? s.reason : static_cast<uint32_t>(KILN_MARKER_ANCHOR_ENDED);
    if (position) *position = s.position;
    return true;
}

bool Watches::lease(KilnOwner owner, KilnMarkerAnchor handle, ToRead* t, uint32_t* kind, uint64_t* epoch) const {
    std::lock_guard g(mu_);
    const auto it = by_handle_.find(handle);
    if (!handle || it == by_handle_.end() || leases_[it->second].owner != owner) return false;
    const Lease& l = leases_[it->second];
    const Slot& s = slots_[l.slot];
    if (t) *t = ToRead{l.slot, s.entity, s.serial};
    if (kind) *kind = l.kind;
    if (epoch) *epoch = s.epoch;
    return true;
}

Watches::Counts Watches::counts() const {
    std::lock_guard g(mu_);
    Counts c;
    c.slots = kWatchSlots - free_slot_count_;
    c.live = live_.load();
    c.leases = kLeasesTotal - free_lease_count_;
    c.references = refs_;
    c.serials = next_serial_;
    c.handles = next_handle_;
    c.ended = ended_.load();
    return c;
}

// ---- capture_anchor's body ----------------------------------------------------------------------------------------------

uint32_t capture_into(Watches& w, KilnOwner owner, uint32_t kind, uint64_t expected, uint64_t current, uint32_t entity, CaptureReadFn read,
                      void* ctx, KilnMarkerAnchorResult* out) {
    const uint32_t size = out->size;
    std::memset(out, 0, sizeof(KilnMarkerAnchorResult));
    out->size = size;
    out->world_kind = kind;
    out->entity = entity;
    out->world_epoch = current;
    if (!current || current != expected) {  // before any read
        out->status = KILN_MARKER_NOT_READY;
        std::snprintf(out->reason, sizeof out->reason, "NotReady: the %s world of epoch %llu is not running (now: %llu)",
                      kind == KILN_WORLD_CLIENT ? "client" : "server", static_cast<unsigned long long>(expected),
                      static_cast<unsigned long long>(current));
        return out->status;
    }
    Watches::Pending p;
    out->status = w.begin(owner, kind, expected, entity, &p, out->reason, sizeof out->reason);
    if (out->status != KILN_MARKER_ACCEPTED) return out->status;
    const TransformRead r = read(kind, entity, ctx);  // no lock held
    KilnMarkerAnchor handle = 0;
    out->status = w.finish(owner, p, r, &handle, out->reason, sizeof out->reason);
    if (out->status != KILN_MARKER_ACCEPTED) return out->status;
    out->anchor = handle;
    out->incarnation = p.serial;
    out->position = r.position;
    out->reason[0] = 0;
    return out->status;
}

}  // namespace kmk
