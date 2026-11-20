// SPDX-License-Identifier: MIT
// kiln.markers' logical store (chapter 04 §4.1, §4.5): publishers and their namespaces, the local (client) records, the
// count limits, the pending-notification reservations and the world epochs. Pure: no game call, no callback; the glue
// (markers.cpp) feeds it the world lifecycle and takes its queued notes at each world's Post.
//
// A mutation runs under one short lock: it validates, checks every limit on the projected counts, reserves its
// notification slot, interns its text and commits, in that order; a refusal changes nothing. Nothing is evicted.
#ifndef KILN_MARKERS_STORE_H
#define KILN_MARKERS_STORE_H
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "arena.h"
#include "events.h"
#include "services/kiln_markers.h"

namespace kmk {

constexpr uint32_t kRoleServer = 0, kRoleClient = 1;

// Count limits (policy; chapter 04 §4.5).
constexpr uint32_t kPerNamespace = 128;         // live records per namespace and source
constexpr uint32_t kLocalRecords = 512;         // local records in the process
constexpr uint32_t kClientRecords = 1024;       // local + received on a client
constexpr uint32_t kNamespaces = 64;            // distinct namespaces ever bound in the process (the ledger)
constexpr uint32_t kPendingPerPublisher = 64;   // accepted changes of one publisher waiting for its dispatch
constexpr uint32_t kPendingPerRole = 256;       // ... of all publishers of a role

// What may be offered now; written by the glue (rows built, the client gate, sigdb), read without the lock.
struct Switches {
    std::atomic<bool> local{false};
    std::atomic<bool> map{false};
    std::atomic<bool> compass{false};
    std::atomic<bool> world{false};
    std::atomic<bool> map_text{false};
    std::atomic<bool> anchors_client{false};
};

// The icon catalog's answer for a map icon (catalog.h implements it).
enum class IconAnswer { NotReady, Unknown, NotForMap, Ok };
using IconCheck = IconAnswer (*)(uint32_t id, void* ctx);

// One record, as compared: two specs that normalize to equal records are the same logical value.
struct Record {
    uint32_t surfaces = 0;
    double x = 0, y = 0, z = 0;
    float height = 0, max_distance = 0;
    uint32_t icon = 0, rgba = 0xffffffffu;
    int32_t priority = 0;
    uint32_t label_mode = 0;
    std::string_view text;  // interned (arena), or the caller's bytes before commit
    uint64_t follow = 0;    // the followed incarnation (the anchors' watch serial, never reused); 0 = fixed at x, y, z
    uint64_t sequence = 0;  // not part of the value: the logical sequence of the change that stored it
    bool same(const Record& o) const {
        return surfaces == o.surfaces && x == o.x && y == o.y && z == o.z && height == o.height && max_distance == o.max_distance &&
               icon == o.icon && rgba == o.rgba && priority == o.priority && label_mode == o.label_mode && text == o.text &&
               follow == o.follow;
    }
};

// One row of a client's records as the surfaces see it (S5, S6): a copy taken under the store lock at a client Post.
// Local records and received ones (the receiver's virtual publishers) alike.
struct ClientRow {
    uint32_t source = KILN_MARKER_LOCAL;       // KILN_MARKER_LOCAL or KILN_MARKER_RECEIVED
    uint64_t publisher_generation = 0;
    uint64_t peer_generation = 0;              // received: the link's generation; 0 for local records
    std::string ns, key;
    Record rec;                                // rec.text lives in the arena (process lifetime)
    uint32_t source_reason = KILN_MARKER_READY; // received: the sender's FOLLOW_MISSING, ANCHOR_ENDED or BAD_POSITION,
                                                // or SOURCE_STALE; local: READY (follow state comes from the anchors)
};

class Store {
public:
    Store(Arena& arena, const Switches& switches) : arena_(arena), sw_(switches) {}
    void set_icon_check(IconCheck fn, void* ctx) {
        icon_check_ = fn;
        icon_ctx_ = ctx;
    }

    // ---- publishers ----
    KilnResult bind(KilnOwner owner, KilnStr ns);
    KilnResult release(KilnOwner owner);  // the owner's records (one CLEARED per nonempty scope) and its binding

    // ---- the local store (header: set_local, remove_local, clear_local) ----
    KilnResult set_local(KilnOwner owner, const KilnMarkerSpec* spec, KilnMarkerResult* result);
    KilnResult remove_local(KilnOwner owner, KilnStr key, KilnMarkerResult* result);
    KilnResult clear_local(KilnOwner owner, KilnMarkerResult* result);

    // ---- worlds: STARTED takes the next epoch; STOPPING sets it to 0 first, then clears that world's records ----
    void world_started(uint32_t kind);
    void world_stopping(uint32_t kind);
    KilnResult world_epoch(uint32_t kind, uint64_t* out) const;

    // ---- notifications ----
    void post_catalog(uint32_t role, const KilnMarkerCatalogChange& change);  // not counted against the change limits
    std::vector<Note> take(uint32_t role);  // the queued notes of a role, in order; frees their reservations

    // ---- for tests and logs ----
    // The client's rows now (local, then received), for the surfaces at a client Post.
    void snapshot_client(std::vector<ClientRow>* out) const;

    uint32_t local_count() const;
    uint32_t pending(uint32_t role) const;
    uint64_t text_charged(std::string_view ns) const;

private:
    struct Binding {
        KilnOwner owner = 0;
        uint32_t ledger = 0;
        uint64_t generation = 0;
        uint32_t pending = 0;
        std::map<std::string, Record, std::less<>> local;
    };
    struct LedgerEntry {
        std::string ns;
        uint64_t text_charged = 0;  // new text bytes this namespace added to the arena, ever
        KilnOwner bound_to = 0;
    };

    Binding* bound(KilnOwner owner);
    void enqueue_change(uint32_t role, const Binding& b, uint32_t kind, std::string_view key, uint32_t affected, uint32_t cause, bool counted);
    void clear_world(uint32_t kind);  // under mu_

    Arena& arena_;
    const Switches& sw_;
    IconCheck icon_check_ = nullptr;
    void* icon_ctx_ = nullptr;

    mutable std::mutex mu_;
    std::map<KilnOwner, Binding> bindings_;
    std::unordered_map<uint64_t, KilnOwner> by_generation_;
    std::vector<LedgerEntry> ledger_;
    uint64_t next_generation_ = 0;
    uint32_t local_total_ = 0;
    uint32_t received_total_ = 0;  // the receiver's records (wire v1)
    uint32_t pending_[2] = {0, 0};
    uint64_t sequence_[2] = {0, 0};
    std::vector<Note> queue_[2];
    uint64_t epoch_counter_ = 0;
    std::atomic<uint64_t> epochs_[KILN_WORLD_KINDS] = {};
};

}  // namespace kmk
#endif
