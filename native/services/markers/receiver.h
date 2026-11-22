// SPDX-License-Identifier: MIT
// kiln.markers' client role of the link (dossier chapter 04 §4.4, §4.5): the built-in receiver. It says HELLO to its
// server (route 1 Network, route 2 the hosted loopback), sends the icon catalog when asked, stages snapshots, applies
// deltas and poses as whole transactions into the store's received records (virtual publishers under the process owner;
// the author's mod is never needed), acknowledges each, and keeps a lease on the link.
//
// The client role's state machine (one link: its server):
//   Idle (no link, no client world, no marker channel)
//   Hello: HELLO now, again every 1 s five times, then every 5 s; a new nonce per link generation, client world and restart
//   Linked (WELCOME with our nonce and a nonzero server epoch): data of that tuple only; old tuples are ignored.
//     SNAP_BEGIN/CHUNK/END stage a whole snapshot (512 rows, 256 KiB, 5 s), validated 128 rows per step, published at
//     once when complete and its digest matches; DELTA and POSE need applied + 1 (a gap asks for RESYNC); a transaction
//     at or below the applied sequence is ignored and acknowledged again. ACK status: accepted, full (a limit), unsupported
//     (a surface or icon this client cannot show), bad baseline. HEARTBEAT renews a 10 s lease and asks for RESYNC when it
//     names a later sequence; a lapsed lease clears the received records and says HELLO again with a new nonce.
//     A change of the local capabilities or catalog says HELLO again with the same nonce (state kept).
//   3 bad messages in 10 s: nothing is read for 10 s, then a new HELLO with a new nonce (the records clear with the lease).
// Threads: post, peer_up, peer_down, collect, sent on the client's main thread (its world's Post and Client.Tick).
#ifndef KILN_MARKERS_RECEIVER_H
#define KILN_MARKERS_RECEIVER_H
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bridge.h"
#include "catalog.h"
#include "linkenv.h"
#include "store.h"
#include "wire.h"

namespace kmk {

class Receiver : public Role {
public:
    Receiver(Store& store, CatalogHolder& catalog, const Switches& switches, Inbox& inbox, LinkEnv env);

    void world_started();
    void world_stopping();

    KilnResult peer_up(const KilnMarkerPeer& peer) override;
    KilnResult peer_down(uint32_t peer, uint64_t generation) override;
    void collect(uint32_t max_packets, Bucket& process, uint64_t now_ms, std::vector<Outgoing>* out) override;
    void sent(const Outgoing& packet, bool ok, uint64_t now_ms) override;

    // The client step: inbound messages, staging, timers, HELLO.
    void post();

    // What this client can show now (HELLO's capability bits).
    uint32_t local_caps() const;

    // ---- tests and logs ----
    enum class Phase { Idle, Hello, Linked };
    struct Info {
        Phase phase = Phase::Idle;
        bool suspended = false, staging = false, need_catalog = false;
        uint32_t accepted = 0, view = 0, hellos = 0;
        uint64_t applied = 0, lease_until = 0;
        wire::Token nonce{}, server_epoch{};
    };
    Info info() const;

private:
    struct VRow {
        std::string ns;
        uint64_t gen = 0;
        std::string key;
        uint64_t revision = 0;
        bool followed = false;
        uint32_t reason = 0;
        wire::Record rec;  // as last applied (an identical resend is no change)
    };
    struct Packet {
        uint32_t message = 0;
        std::vector<uint8_t> bytes;
        uint64_t tag = 0;
    };
    struct Staging {
        bool active = false, ended = false;
        uint64_t seq = 0, id = 0, started = 0;
        uint16_t records = 0, chunks = 0;
        std::vector<bool> have;
        std::vector<std::vector<wire::Record>> parts;
        std::vector<std::vector<uint8_t>> raw;
        wire::Digest digest{};
        size_t bytes = 0, validated = 0, received = 0;
    };
    enum class Done { Ok, Defer };

    // under mu_
    void reset_link(uint32_t cause, bool new_nonce);
    void mint_nonce();
    Done handle(Inbound& m, uint64_t now);
    void bad(uint64_t now, const char* why);
    void on_welcome(const wire::Header& h, const wire::Welcome& w, uint64_t now);
    Done on_delta(const wire::Header& h, std::vector<wire::DeltaOp>& ops, uint64_t now);
    Done on_pose(const wire::Header& h, const std::vector<wire::Pose>& poses, uint64_t now);
    void on_heartbeat(const wire::Header& h, uint64_t now);
    void on_snap_begin(const wire::Header& h, const wire::SnapBegin& b, uint64_t now);
    void on_snap_chunk(const wire::Header& h, wire::SnapChunk& c, uint64_t now);
    void on_snap_end(const wire::Header& h, const wire::SnapEnd& e, uint64_t now);
    void advance_staging(uint64_t now);
    // A record this client can show: nullptr, else why not (unsupported surface or icon)
    const char* unsupported(const wire::Record& r) const;
    ReceivedRecord to_store(const wire::Record& r) const;
    void ack(uint64_t applied, uint8_t status, const std::string& reason);
    void resync(uint8_t reason, uint64_t now);
    void hello(uint64_t now);
    void send_catalog();
    void enqueue(uint32_t message, std::vector<uint8_t>&& bytes);
    wire::Header header(uint64_t seq) const;
    void log(int level, const char* fmt, ...);

    Store& store_;
    CatalogHolder& catalog_;
    const Switches& sw_;
    Inbox& inbox_;
    LinkEnv env_;

    mutable std::mutex mu_;
    struct {
        bool up = false;
        uint32_t peer = kServerPeer, route = kRouteNetwork, flags = 0, max_body = 0;
        uint64_t generation = 0;
    } link_;
    uint64_t world_ = 0;  // the client world's epoch (the store's)
    Phase phase_ = Phase::Idle;
    wire::Token nonce_{}, server_epoch_{};
    bool have_nonce_ = false;
    uint32_t caps_sent_ = 0;
    wire::Digest digest_sent_{};
    uint32_t hellos_ = 0;
    uint64_t next_hello_ = 0;
    uint32_t accepted_ = 0;
    bool need_catalog_ = false;
    uint64_t applied_ = 0;
    std::map<uint64_t, VRow> view_;  // wire id -> row
    Staging staging_;
    uint64_t lease_until_ = 0;
    std::vector<uint64_t> bad_;
    uint64_t suspended_until_ = 0;
    uint64_t last_bad_log_ = 0;
    uint64_t text_skips_seen_ = 0;  // the store's received_text_skips() at the last check (note_text_skips)
    void note_text_skips();         // logs once per new skip that a received record kept its last value (text over budget)
    uint64_t resync_at_[2] = {0, 0};
    bool want_resync_ = false;
    uint8_t resync_reason_ = 0;
    uint64_t last_reack_ = 0;
    uint32_t pose_rows_ = 0;  // pose rows applied in this step (128 at most)
    Bucket data_msgs_{128, 128};
    Bucket data_bytes_{256.0 * 1024, 256.0 * 1024};
    Bucket delta_ops_{128, 128};
    Bucket out_bytes_{128.0 * 1024, 64.0 * 1024};
    std::deque<Packet> outbox_;
    uint64_t tags_ = 0;
    bool body_warned_ = false;
};

}  // namespace kmk
#endif
