// SPDX-License-Identifier: MIT
#include "store.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "text.h"

namespace kmk {

namespace {

constexpr uint32_t kSurfaceBits = KILN_MARKER_MAP | KILN_MARKER_COMPASS | KILN_MARKER_WORLD;

void reset(KilnMarkerResult* r) {
    const uint32_t size = r->size;
    std::memset(r, 0, sizeof(KilnMarkerResult));
    r->size = size;
}

void say(KilnMarkerResult* r, const char* fmt, va_list v) { std::vsnprintf(r->reason, sizeof r->reason, fmt, v); }

KilnResult bad(KilnMarkerResult* r, const char* fmt, ...) {
    va_list v;
    va_start(v, fmt);
    say(r, fmt, v);
    va_end(v);
    return KILN_ERR_INVALID_ARGUMENT;
}

KilnResult off(KilnMarkerResult* r, const char* capability) {
    std::snprintf(r->reason, sizeof r->reason, "%s", capability);
    return KILN_ERR_UNAVAILABLE;
}

// A refusal by the current state: KILN_OK, nothing committed. The reason starts with the status's name.
KilnResult refuse(KilnMarkerResult* r, uint32_t status, const char* fmt, ...) {
    r->status = status;
    r->committed = 0;
    va_list v;
    va_start(v, fmt);
    say(r, fmt, v);
    va_end(v);
    return KILN_OK;
}

KilnResult accepted(KilnMarkerResult* r, uint32_t affected) {
    r->status = KILN_MARKER_ACCEPTED;
    r->committed = 1;
    r->affected = affected;
    r->reason[0] = 0;
    return KILN_OK;
}

bool result_ok(const KilnMarkerResult* r) { return r && r->size >= sizeof(KilnMarkerResult); }

bool finite_in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }

void copy_field(char* dst, size_t cap, std::string_view s) {
    const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
    if (n) std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

// The spec's own rules (chapter 04 §4.5 schema). nullptr when it is valid; `out` then holds the normalized record (its
// text still the caller's bytes) and `key` the key.
const char* validate(const KilnMarkerSpec& s, Record* out, std::string_view* key) {
    if (!s.surfaces || (s.surfaces & ~kSurfaceBits)) return "surfaces must be a nonzero mask of MAP (1), COMPASS (2) and WORLD (4)";
    if (!valid_identifier(s.key.ptr, s.key.len)) return "key must be 1 to 64 bytes of [A-Za-z0-9_.:-]";
    if (!std::isfinite(s.position.x) || !std::isfinite(s.position.y) || !std::isfinite(s.position.z)) return "position must be finite";
    if (!s.follow && (!finite_in(s.position.x, -1000, 12000) || !finite_in(s.position.z, -1000, 12000) || !finite_in(s.position.y, -1000, 5000)))
        return "position is outside the world (x and z in [-1000, 12000], y in [-1000, 5000])";
    if (!finite_in(s.height, -128, 128)) return "height must be finite, in [-128, 128] metres";
    if (!finite_in(s.max_distance, 0, 100000)) return "max_distance must be finite, in [0, 100000] metres (0 = unlimited)";
    if (s.priority < -32768 || s.priority > 32767) return "priority must be in [-32768, 32767]";
    if (s.map_label_mode != KILN_MARKER_LABEL_NONE && s.map_label_mode != KILN_MARKER_LABEL_NATIVE_TOOLTIP) return "map_label_mode must be NONE (0) or NATIVE_TOOLTIP (1)";
    if (s.map_label_mode == KILN_MARKER_LABEL_NATIVE_TOOLTIP && !(s.surfaces & KILN_MARKER_MAP)) return "map_label_mode NATIVE_TOOLTIP needs the MAP surface";
    if (const char* why = text_problem(s.text.ptr, s.text.len, kTextMax)) return why;
    if (s.surfaces & KILN_MARKER_MAP) {
        if (!s.icon_type) return "a MAP marker needs an icon of the catalog (icon_type 0 is for compass and world only)";
    } else if (s.icon_type) {
        return "icon_type must be 0 without the MAP surface (icons are drawn on the map only)";
    }
    Record r;
    r.surfaces = s.surfaces;
    if (!s.follow) {  // a followed record's position is ignored: normalized to zero
        r.x = s.position.x + 0.0;  // + 0.0: -0 becomes +0
        r.y = s.position.y + 0.0;
        r.z = s.position.z + 0.0;
    }
    r.height = s.height + 0.0f;
    r.max_distance = s.max_distance + 0.0f;
    r.icon = s.icon_type;
    r.rgba = s.rgba;
    r.priority = s.priority;
    r.label_mode = s.map_label_mode;
    r.text = s.text.len ? std::string_view(s.text.ptr, s.text.len) : std::string_view();
    *out = r;
    *key = std::string_view(s.key.ptr, s.key.len);
    return nullptr;
}

}  // namespace

// ---- publishers ---------------------------------------------------------------------------------------------------------

Store::Binding* Store::bound(KilnOwner owner) {
    auto it = bindings_.find(owner);
    return it == bindings_.end() ? nullptr : &it->second;
}

KilnResult Store::bind(KilnOwner owner, KilnStr ns) {
    if (!owner || !valid_identifier(ns.ptr, ns.len)) return KILN_ERR_INVALID_ARGUMENT;
    const std::string_view name(ns.ptr, ns.len);
    std::lock_guard g(mu_);
    if (Binding* b = bound(owner)) return ledger_[b->ledger].ns == name ? KILN_OK : KILN_ERR_INVALID_ARGUMENT;
    uint32_t index = 0;
    for (; index < ledger_.size(); ++index)
        if (ledger_[index].ns == name) break;
    if (index < ledger_.size()) {
        if (ledger_[index].bound_to) return KILN_ERR_INVALID_ARGUMENT;  // another live owner holds it
    } else {
        if (ledger_.size() >= kNamespaces) return KILN_ERR_BUFFER_TOO_SMALL;
        ledger_.push_back(LedgerEntry{std::string(name), 0, 0});
    }
    ledger_[index].bound_to = owner;
    Binding& b = bindings_[owner];
    b.owner = owner;
    b.ledger = index;
    b.generation = ++next_generation_;
    by_generation_[b.generation] = owner;
    return KILN_OK;
}

KilnResult Store::release(KilnOwner owner) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard g(mu_);
    Binding* b = bound(owner);
    if (!b) return KILN_OK;
    if (const uint32_t n = static_cast<uint32_t>(b->local.size())) {
        local_total_ -= n;
        b->local.clear();
        enqueue_change(kRoleClient, *b, KILN_MARKER_CLEARED, {}, n, KILN_MARKER_OWNER_RETIRED, false);
    }
    ledger_[b->ledger].bound_to = 0;
    by_generation_.erase(b->generation);
    bindings_.erase(owner);
    return KILN_OK;
}

// ---- notifications ------------------------------------------------------------------------------------------------------

void Store::enqueue_change(uint32_t role, const Binding& b, uint32_t kind, std::string_view key, uint32_t affected, uint32_t cause, bool counted) {
    Note n;
    n.kind = Note::Change;
    n.role = role;
    n.counted = counted;
    n.publisher_generation = b.generation;
    KilnMarkerChange& c = n.change;
    c.size = sizeof(KilnMarkerChange);
    c.kind = kind;
    c.audience = KILN_MARKER_LOCAL;
    c.recipient = 0;
    c.world_epoch = epochs_[role == kRoleClient ? KILN_WORLD_CLIENT : KILN_WORLD_SERVER].load();
    c.peer_generation = 0;
    c.publisher_generation = b.generation;
    c.sequence = ++sequence_[role];
    c.affected = affected;
    c.cause = cause;
    copy_field(c.owner, sizeof c.owner, ledger_[b.ledger].ns);
    copy_field(c.key, sizeof c.key, key);
    queue_[role].push_back(n);
}

void Store::post_catalog(uint32_t role, const KilnMarkerCatalogChange& change) {
    Note n;
    n.kind = Note::Catalog;
    n.role = role;
    n.catalog = change;
    n.catalog.size = sizeof(KilnMarkerCatalogChange);
    n.catalog.role = role;
    std::lock_guard g(mu_);
    queue_[role].push_back(n);
}

std::vector<Note> Store::take(uint32_t role) {
    std::lock_guard g(mu_);
    std::vector<Note> out;
    out.swap(queue_[role]);
    for (const Note& n : out) {
        if (!n.counted) continue;
        --pending_[role];
        if (auto it = by_generation_.find(n.publisher_generation); it != by_generation_.end())
            if (Binding* b = bound(it->second)) --b->pending;
    }
    return out;
}

// ---- the local store ----------------------------------------------------------------------------------------------------

KilnResult Store::set_local(KilnOwner owner, const KilnMarkerSpec* spec, KilnMarkerResult* r) {
    if (!result_ok(r)) return KILN_ERR_INVALID_ARGUMENT;
    reset(r);
    if (!spec || spec->size < sizeof(KilnMarkerSpec)) return bad(r, "spec.size is smaller than KilnMarkerSpec v1");
    if (!sw_.local.load()) return off(r, "markers.local");
    Record rec;
    std::string_view key;
    if (const char* why = validate(*spec, &rec, &key)) return bad(r, "%s", why);
    // what the spec asks for must all be there: nothing is drawn partly
    if (spec->follow && !sw_.anchors_client.load()) return off(r, "markers.anchors.client");
    if ((rec.surfaces & KILN_MARKER_MAP) && !sw_.map.load()) return off(r, "markers.map");
    if ((rec.surfaces & KILN_MARKER_COMPASS) && !sw_.compass.load()) return off(r, "markers.compass");
    if ((rec.surfaces & KILN_MARKER_WORLD) && !sw_.world.load()) return off(r, "markers.world");
    if (rec.label_mode == KILN_MARKER_LABEL_NATIVE_TOOLTIP && !sw_.map_text.load()) return off(r, "markers.map_text");
    if (rec.surfaces & KILN_MARKER_MAP) {
        switch (icon_check_ ? icon_check_(rec.icon, icon_ctx_) : IconAnswer::NotReady) {
            case IconAnswer::Ok: break;
            case IconAnswer::NotReady: return refuse(r, KILN_MARKER_CATALOG_NOT_READY, "CatalogNotReady: the icon catalog is still warming up");
            case IconAnswer::Unknown: return bad(r, "icon_type 0x%08x is not in the icon catalog", rec.icon);
            case IconAnswer::NotForMap:
                return bad(r, "icon_type 0x%08x is listed without the MAP surface (fast-travel types are refused on the map)", rec.icon);
        }
    }
    if (spec->follow) return off(r, "markers.anchors.client");  // anchors are not built in this kiln.markers yet

    std::lock_guard g(mu_);
    Binding* b = bound(owner);
    if (!b) return bad(r, "owner %llu is not bound (bind_owner)", static_cast<unsigned long long>(owner));
    if (!epochs_[KILN_WORLD_CLIENT].load()) return refuse(r, KILN_MARKER_NOT_READY, "NotReady: there is no client world");
    LedgerEntry& ledger = ledger_[b->ledger];
    auto it = b->local.find(key);
    if (it != b->local.end() && it->second.same(rec)) return accepted(r, 0);  // identical: nothing to do, no event
    if (it == b->local.end()) {
        if (b->local.size() >= kPerNamespace)
            return refuse(r, KILN_MARKER_FULL, "Full: namespace '%s' has %u local markers (the most)", ledger.ns.c_str(), kPerNamespace);
        if (local_total_ >= kLocalRecords) return refuse(r, KILN_MARKER_FULL, "Full: the process has %u local markers (the most)", kLocalRecords);
        if (local_total_ + received_total_ >= kClientRecords)
            return refuse(r, KILN_MARKER_FULL, "Full: this client holds %u markers (local and received; the most)", kClientRecords);
    }
    if (b->pending >= kPendingPerPublisher)
        return refuse(r, KILN_MARKER_FULL, "Full: %u changes of '%s' wait for the next client step", kPendingPerPublisher, ledger.ns.c_str());
    if (pending_[kRoleClient] >= kPendingPerRole)
        return refuse(r, KILN_MARKER_FULL, "Full: %u marker changes wait for the next client step", kPendingPerRole);
    const char* why = nullptr;
    std::string_view interned;
    if (!arena_.intern(rec.text, &ledger.text_charged, &interned, &why)) return refuse(r, KILN_MARKER_FULL, "Full: %s", why);
    rec.text = interned;
    rec.sequence = sequence_[kRoleClient] + 1;  // the sequence enqueue_change gives this change
    uint32_t kind = KILN_MARKER_UPDATED;
    if (it == b->local.end()) {
        b->local.emplace(std::string(key), rec);
        ++local_total_;
        kind = KILN_MARKER_ADDED;
    } else {
        it->second = rec;
    }
    ++b->pending;
    ++pending_[kRoleClient];
    enqueue_change(kRoleClient, *b, kind, key, 1, KILN_MARKER_API, true);
    return accepted(r, 1);
}

KilnResult Store::remove_local(KilnOwner owner, KilnStr key, KilnMarkerResult* r) {
    if (!result_ok(r)) return KILN_ERR_INVALID_ARGUMENT;
    reset(r);
    if (!sw_.local.load()) return off(r, "markers.local");
    if (!valid_identifier(key.ptr, key.len)) return bad(r, "key must be 1 to 64 bytes of [A-Za-z0-9_.:-]");
    const std::string_view k(key.ptr, key.len);
    std::lock_guard g(mu_);
    Binding* b = bound(owner);
    if (!b) return bad(r, "owner %llu is not bound (bind_owner)", static_cast<unsigned long long>(owner));
    if (!epochs_[KILN_WORLD_CLIENT].load()) return refuse(r, KILN_MARKER_NOT_READY, "NotReady: there is no client world");
    auto it = b->local.find(k);
    if (it == b->local.end()) return refuse(r, KILN_MARKER_NOT_FOUND, "NotFound: '%s' has no local marker '%.*s'", ledger_[b->ledger].ns.c_str(),
                                            static_cast<int>(k.size()), k.data());
    if (b->pending >= kPendingPerPublisher)
        return refuse(r, KILN_MARKER_FULL, "Full: %u changes of '%s' wait for the next client step", kPendingPerPublisher, ledger_[b->ledger].ns.c_str());
    if (pending_[kRoleClient] >= kPendingPerRole)
        return refuse(r, KILN_MARKER_FULL, "Full: %u marker changes wait for the next client step", kPendingPerRole);
    b->local.erase(it);
    --local_total_;
    ++b->pending;
    ++pending_[kRoleClient];
    enqueue_change(kRoleClient, *b, KILN_MARKER_REMOVED, k, 1, KILN_MARKER_API, true);
    return accepted(r, 1);
}

KilnResult Store::clear_local(KilnOwner owner, KilnMarkerResult* r) {
    if (!result_ok(r)) return KILN_ERR_INVALID_ARGUMENT;
    reset(r);
    if (!sw_.local.load()) return off(r, "markers.local");
    std::lock_guard g(mu_);
    Binding* b = bound(owner);
    if (!b) return bad(r, "owner %llu is not bound (bind_owner)", static_cast<unsigned long long>(owner));
    if (!epochs_[KILN_WORLD_CLIENT].load()) return refuse(r, KILN_MARKER_NOT_READY, "NotReady: there is no client world");
    const uint32_t n = static_cast<uint32_t>(b->local.size());
    if (!n) return accepted(r, 0);
    if (b->pending >= kPendingPerPublisher)
        return refuse(r, KILN_MARKER_FULL, "Full: %u changes of '%s' wait for the next client step", kPendingPerPublisher, ledger_[b->ledger].ns.c_str());
    if (pending_[kRoleClient] >= kPendingPerRole)
        return refuse(r, KILN_MARKER_FULL, "Full: %u marker changes wait for the next client step", kPendingPerRole);
    b->local.clear();
    local_total_ -= n;
    ++b->pending;
    ++pending_[kRoleClient];
    enqueue_change(kRoleClient, *b, KILN_MARKER_CLEARED, {}, n, KILN_MARKER_API, true);
    return accepted(r, n);
}

// ---- worlds -------------------------------------------------------------------------------------------------------------

void Store::clear_world(uint32_t kind) {
    if (kind != KILN_WORLD_CLIENT) return;  // the server's sent records arrive with the server layers
    for (auto& [owner, b] : bindings_) {
        const uint32_t n = static_cast<uint32_t>(b.local.size());
        if (!n) continue;
        local_total_ -= n;
        b.local.clear();
        enqueue_change(kRoleClient, b, KILN_MARKER_CLEARED, {}, n, KILN_MARKER_WORLD_STOPPED, false);
    }
}

void Store::world_started(uint32_t kind) {
    if (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT) return;
    std::lock_guard g(mu_);
    if (epochs_[kind].load()) clear_world(kind);  // a start without a stop: the old world's records end here
    epochs_[kind].store(++epoch_counter_);
}

void Store::world_stopping(uint32_t kind) {
    if (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT) return;
    std::lock_guard g(mu_);
    if (!epochs_[kind].load()) return;
    clear_world(kind);        // its notes carry the stopping world's epoch
    epochs_[kind].store(0);   // from here (same lock) a mutation sees no world
}

KilnResult Store::world_epoch(uint32_t kind, uint64_t* out) const {
    if (!out || (kind != KILN_WORLD_SERVER && kind != KILN_WORLD_CLIENT)) return KILN_ERR_INVALID_ARGUMENT;
    *out = epochs_[kind].load();
    return KILN_OK;
}

void Store::snapshot_client(std::vector<ClientRow>* out) const {
    out->clear();
    std::lock_guard g(mu_);
    out->reserve(local_total_ + received_total_);
    for (const auto& [owner, b] : bindings_) {
        for (const auto& [key, rec] : b.local) {
            ClientRow row;
            row.source = KILN_MARKER_LOCAL;
            row.publisher_generation = b.generation;
            row.ns = ledger_[b.ledger].ns;
            row.key = key;
            row.rec = rec;
            out->push_back(std::move(row));
        }
    }
}

uint32_t Store::local_count() const {
    std::lock_guard g(mu_);
    return local_total_;
}

uint32_t Store::pending(uint32_t role) const {
    std::lock_guard g(mu_);
    return pending_[role];
}

uint64_t Store::text_charged(std::string_view ns) const {
    std::lock_guard g(mu_);
    for (const LedgerEntry& e : ledger_)
        if (e.ns == ns) return e.text_charged;
    return 0;
}

}  // namespace kmk
