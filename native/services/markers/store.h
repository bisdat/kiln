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
#include <tuple>
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
    // Received followed rows (the server follows the entity and sends poses): the last two poses and when they arrived
    // (the store's clock, steady milliseconds). rec.x/y/z is pose_last. The surfaces interpolate pose_prev -> pose_last
    // over 100 ms after pose_last_ms, clamped, no extrapolation. SOURCE_STALE above: no pose for 750 ms. Before a second
    // pose, pose_prev == pose_last.
    bool followed = false;
    KilnVec3 pose_prev{}, pose_last{};
    uint64_t pose_prev_ms = 0, pose_last_ms = 0;
};

// The spec's own rules (chapter 04 §4.5 schema) for any store (local or the server layers): nullptr when it is valid;
// `out` then holds the normalized record (its text still the caller's bytes) and `key` the key.
const char* validate_spec(const KilnMarkerSpec& s, Record* out, std::string_view* key);

// One received record as the receiver hands it to the store (wire v1, the link part). rec.text is the receiver's bytes
// until the store interns it.
struct ReceivedRecord {
    std::string ns;
    uint64_t remote_generation = 0;  // the server's publisher generation
    std::string key;
    uint64_t wire_id = 0, revision = 0;
    Record rec;
    uint32_t source_reason = KILN_MARKER_READY;
    bool followed = false;
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

    // ---- the server layers' notes (the link part, server role) ----
    // The bound publisher of `owner`: its namespace and generation. False when it is not bound.
    bool publisher(KilnOwner owner, std::string* ns, uint64_t* generation) const;
    // Queues one server logical change (role 0) of a bound publisher. counted: it takes a pending slot (an API call),
    // false and *why when the publisher's or the role's limit is reached (nothing queued then).
    bool server_change(KilnOwner owner, uint32_t kind, uint32_t audience, uint32_t recipient, uint64_t peer_generation, std::string_view key,
                       uint32_t affected, uint32_t cause, bool counted, const char** why);

    // ---- received records (the link part, client role): virtual publishers under the process owner ----
    // Applies one received transaction at once or not at all. replace: the upserts are the whole new view (a snapshot);
    // else they and the removes patch it (a delta). One logical note per changed key (audience RECEIVED, peer_generation):
    // ADDED, UPDATED (another wire id or revision), REMOVED; the same wire id and revision again is no change. Limits:
    // 512 received, 1024 with the local ones, 128 per namespace; received text 1 MiB of new arena bytes in all and 256 KiB
    // per namespace. False and *why (nothing changed) when a limit would be passed.
    bool received_apply(uint64_t peer_generation, const std::vector<ReceivedRecord>& upserts,
                        const std::vector<std::tuple<std::string, uint64_t, std::string>>& removes, bool replace, uint32_t cause, std::string* why);
    // A pose of a received followed record (no logical note).
    void received_pose(std::string_view ns, uint64_t remote_generation, std::string_view key, double x, double y, double z, uint32_t source_reason);
    // Every received record goes: one CLEARED per nonempty namespace.
    void received_clear(uint32_t cause);
    uint32_t received_count() const;
    // The store's clock (steady milliseconds; tests set their own).
    void set_clock(uint64_t (*clock)()) { clock_ = clock; }

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
    void clear_received(uint32_t cause);  // under mu_
    void enqueue_note(uint32_t role, std::string_view ns, uint64_t publisher_generation, uint32_t kind, uint32_t audience, uint32_t recipient,
                      uint64_t peer_generation, std::string_view key, uint32_t affected, uint32_t cause);  // under mu_, uncounted

    // received records, by (namespace, the server's publisher generation, key)
    using RKey = std::tuple<std::string, uint64_t, std::string>;
    struct RRow {
        uint64_t virtual_generation = 0;  // the virtual publisher's generation (this process's numbering)
        uint64_t wire_id = 0, revision = 0;
        Record rec;
        uint32_t source_reason = KILN_MARKER_READY;
        bool followed = false;
        KilnVec3 pose_prev{}, pose_last{};
        uint64_t pose_prev_ms = 0, pose_last_ms = 0;
    };
    std::map<RKey, RRow> received_;
    std::map<std::pair<std::string, uint64_t>, uint64_t, std::less<>> virtual_;  // (namespace, server generation) -> virtual generation
    std::map<std::string, uint64_t, std::less<>> received_charged_;  // new arena bytes per received namespace, ever
    uint64_t received_text_ = 0;   // ... in all
    uint64_t received_link_ = 0;   // the peer generation of the received view
    uint64_t (*clock_)() = nullptr;

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
