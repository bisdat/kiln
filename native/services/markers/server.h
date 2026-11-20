// SPDX-License-Identifier: MIT
// kiln.markers' server role of the link (dossier chapter 04 §4.4, §4.5): the sent records in two layers per publisher, B
// (broadcast, recipient 0) and P (per connection: an override or a suppression), and one link per bridge peer that carries
// each recipient's EFFECTIVE view (P's override, nothing for a suppression, else B) over wire v1.
//
// Per link (server role) the state machine is:
//   registered (peer_up) --HELLO--> welcomed (tuple: server epoch, client nonce, client world epoch; WELCOME sent)
//   welcomed --needCatalog--> CATALOG verified or refused (map on or off for this link; WELCOME needCatalog 0)
//   welcomed + HasPlayer + a recipient slot (16) = a data recipient:
//     idle --need_full--> SNAPSHOT in flight --ACK--> idle;  idle --dirty keys--> DELTA in flight --ACK--> idle;
//     idle --followed rows due--> POSE in flight --ACK--> idle.  One transaction in flight; its ACK timer (2 s) starts
//     when its last packet went; 3 resends, then Stalled (game connection untouched). RESYNC or a new HELLO abandons the
//     flight and sends a full snapshot. ACK Full / Unsupported: refused until a HELLO, a smaller view or an identical
//     retry; ACK bad baseline: full snapshot. HEARTBEAT every 2 s with the last transaction's sequence.
//   3 bad messages in 10 s: suspended (only a HELLO is read) until a new valid handshake.
//   peer_down / another client world epoch: the connection's P layers go (one CLEARED per publisher scope).
// Wire ids are never reused in a server epoch; a record's revision rises with every change of the effective value sent to
// that link (also when an override's removal reveals the broadcast again); positions alone do not raise it.
//
// Threads: the API (send ...) and peer_up, peer_down, post, collect, sent on the server's tick thread; release_owner and
// the world stop on any thread. One lock; no callback, no game call under it.
#ifndef KILN_MARKERS_SERVER_H
#define KILN_MARKERS_SERVER_H
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "bridge.h"
#include "catalog.h"
#include "linkenv.h"
#include "store.h"
#include "wire.h"

namespace kmk {

// Limits (policy, chapter 04 §4.5)
constexpr uint32_t kServerDefinitions = 1024;   // stored definitions (B and P, suppressions included)
constexpr uint32_t kBroadcastRecords = 512;
constexpr uint32_t kPerPublisherSent = 128;     // per publisher, B and every P together
constexpr uint32_t kPeerEffective = 512;        // each link's effective records
constexpr uint32_t kPeerFollowed = 64;          // each link's effective followed records (also the broadcast's)
constexpr uint32_t kServerFollowAnchors = 128;  // distinct followed incarnations
constexpr uint32_t kBridgePeers = 32;           // registered server-role peers
constexpr uint32_t kDataRecipients = 16;        // welcomed links with a player that get data

class Server : public Role {
public:
    Server(Store& store, CatalogHolder& catalog, Inbox& inbox, LinkEnv env);

    // markers.send: off until the bridge attaches (and the world lifecycle is there).
    void set_available(bool on) { available_.store(on); }
    bool available() const { return available_.load(); }
    // Called (outside the lock, at the end of post) once a verified client's catalog became the process's catalog.
    std::function<void()> on_catalog_published;

    void world_started();
    void world_stopping();

    KilnResult send(KilnOwner owner, KilnEntity recipient, const KilnMarkerSpec* spec, KilnMarkerResult* r);
    KilnResult unsend(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r);
    KilnResult clear_sent(KilnOwner owner, KilnEntity recipient, KilnMarkerResult* r);
    KilnResult suppress_sent(KilnOwner owner, KilnEntity recipient, KilnStr key, KilnMarkerResult* r);
    void release_owner(KilnOwner owner);

    // Role
    KilnResult peer_up(const KilnMarkerPeer& peer) override;
    KilnResult peer_down(uint32_t peer, uint64_t generation) override;
    void collect(uint32_t max_packets, Bucket& process, uint64_t now_ms, std::vector<Outgoing>* out) override;
    void sent(const Outgoing& packet, bool ok, uint64_t now_ms) override;

    // The server step: inbound messages, timers, the next transaction of each link.
    void post();

    // ---- tests and logs ----
    struct LinkInfo {
        bool welcomed = false, map_ok = false, need_catalog = false, suspended = false, stalled = false, in_flight = false;
        uint8_t refused = 0;
        uint32_t accepted = 0, effective = 0, followed = 0, view = 0, dirty = 0, outbox = 0;
        uint64_t sequence = 0, acked = 0, identity = 0;
    };
    bool link_info(uint32_t peer, LinkInfo* out) const;
    uint32_t definitions() const;
    wire::Token epoch() const;

    using EKey = std::tuple<std::string, uint64_t, std::string>;  // (namespace, publisher generation, key)

    struct Spec1 {
        uint32_t surfaces = 0;
        double x = 0, y = 0, z = 0;
        float height = 0, max_distance = 0;
        uint32_t icon = 0, rgba = 0xffffffffu;
        int32_t priority = 0;
        uint32_t label_mode = 0;
        uint64_t follow = 0;  // incarnation serial; 0 = fixed
        std::string text;
        bool operator==(const Spec1&) const = default;
    };

private:
    struct Override {
        bool suppress = false;
        Spec1 spec;
    };
    struct Pub {
        KilnOwner owner = 0;
        std::string ns;
        uint64_t generation = 0;
        std::map<std::string, Spec1, std::less<>> B;
        std::map<uint64_t, std::map<std::string, Override, std::less<>>> P;  // connection identity -> layer
        uint32_t count = 0;  // B and every P entry
    };
    struct ViewRow {
        uint64_t wire_id = 0, revision = 0;
        Spec1 spec;
        double px = 0, py = 0, pz = 0;  // the last position sent (followed rows)
        uint32_t reason = 0;            // the last source reason sent
        uint64_t pose_ms = 0;
    };
    struct Packet {
        uint32_t message = 0;
        std::vector<uint8_t> bytes;
        uint64_t seq = 0;  // transaction sequence (0: control)
        bool last = false; // the flight's last packet
        uint64_t tag = 0;
    };
    struct Link {
        uint32_t peer = 0, route = kRouteNetwork, entity = 0, flags = 0, max_body = 0;
        uint64_t generation = 0, identity = 0;
        // handshake
        bool hello = false;
        wire::Token nonce{};
        uint64_t world_epoch = 0;
        uint32_t caps = 0, client_body = 0;
        uint16_t max_records = 0;
        wire::Digest exe{}, digest{};
        uint32_t accepted = 0;
        bool map_ok = false, need_catalog = false, suspended = false;
        std::vector<uint64_t> bad;  // times of recent bad messages
        // data
        uint64_t seq = 0, acked = 0;
        struct Flight {
            uint64_t seq = 0;
            std::vector<Packet> packets;
            uint64_t sent_at = 0;  // when its last packet went (0: not yet)
            uint32_t tries = 0;
        };
        std::optional<Flight> flight;
        bool need_full = true, stalled = false;
        uint64_t stalled_seq = 0;
        uint8_t refused = 0;  // 1 Full, 2 Unsupported (ACK status)
        std::set<EKey> dirty;
        std::map<EKey, ViewRow> view;
        uint32_t eff = 0, followed = 0;
        uint64_t next_heartbeat = 0;
        uint64_t resync_at[2] = {0, 0};
        Bucket delta_ops{128, 128};
        Bucket controls{8, 16};
        Bucket bytes{128.0 * 1024, 64.0 * 1024};
        std::deque<Packet> outbox;
        bool deferring = false;
    };
    enum class Class { None, Ready, NoKiln, NotReady, Full };

    // under mu_
    Link* find(uint32_t peer, uint64_t generation);
    Link* find_peer(uint32_t peer);
    Link* find_identity(uint64_t identity);
    Link* target(KilnEntity entity);
    Pub* pub_of(std::string_view ns, uint64_t generation);
    bool shadowed(const Link& l) const;
    Class classify(const Link& l) const;
    bool compatible(const Link& l, const Spec1& s) const;
    const Spec1* effective(const Pub& p, const Link& l, const std::string& key) const;
    const Spec1* effective_identity(const Pub& p, uint64_t identity, const std::string& key) const;
    void recount(Link& l);
    void count_follow(const Spec1* before, const Spec1* after);
    void drop_targets(Link& l, uint32_t cause);
    void reset_data(Link& l);
    void handle(Link& l, Inbound& m, uint64_t now);
    void bad(Link& l, uint64_t now, const char* why);
    void on_hello(Link& l, const wire::Header& h, const wire::Hello& m, uint64_t now);
    void on_catalog(Link& l, const wire::Catalog& m, uint64_t now);
    void on_ack(Link& l, const wire::Ack& m, uint64_t now);
    void on_resync(Link& l, const wire::Resync& m, uint64_t now);
    void decide_catalog(Link& l);
    uint32_t accepted_mask(const Link& l) const;
    void welcome(Link& l);
    void tick(Link& l, uint64_t now);
    void build_snapshot(Link& l, uint64_t now);
    void build_delta(Link& l, uint64_t now);
    void build_poses(Link& l, uint64_t now);
    void start_flight(Link& l, std::vector<Packet>&& packets);
    void enqueue(Link& l, Packet&& p);
    wire::Header header(const Link& l, uint64_t seq) const;
    wire::Record to_wire(const EKey& k, const ViewRow& v) const;
    void pose_of(const Spec1& s, ViewRow* v) const;
    void log(int level, const char* fmt, ...);
    // the effect of a mutation on every link: dirty marks, counts, the result's queued/refused
    struct Tally {
        uint32_t queued = 0, refused = 0, requires_client = 0, not_ready = 0, full = 0;
    };
    void finish(KilnMarkerResult* r, const Tally& t, uint32_t affected);

    Store& store_;
    CatalogHolder& catalog_;
    Inbox& inbox_;
    LinkEnv env_;
    std::atomic<bool> available_{false};

    mutable std::mutex mu_;
    wire::Token epoch_{};
    bool live_ = false;  // a server world runs
    uint64_t wire_ids_ = 0, snapshots_ = 0, identities_ = 0, tags_ = 0;
    std::map<KilnOwner, Pub> pubs_;
    uint32_t defs_ = 0, broadcast_ = 0, broadcast_followed_ = 0;
    std::map<uint64_t, uint32_t> follow_refs_;  // serial -> stored definitions following it
    std::vector<std::unique_ptr<Link>> links_;
    size_t rr_ = 0;  // collect's round-robin start
    uint64_t last_bad_log_ = 0;
    bool catalog_published_ = false;
};

}  // namespace kmk
#endif
