// SPDX-License-Identifier: MIT
#include "receiver.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>

#include "sha256.h"

namespace kmk {

namespace {
const wire::Token kZero{};

bool in_world(double x, double y, double z) {
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && x >= -1000 && x <= 12000 && z >= -1000 && z <= 12000 && y >= -1000 &&
           y <= 5000;
}
}  // namespace

Receiver::Receiver(Store& store, CatalogHolder& catalog, const Switches& switches, Inbox& inbox, LinkEnv env)
    : store_(store), catalog_(catalog), sw_(switches), inbox_(inbox), env_(std::move(env)) {}

void Receiver::log(int level, const char* fmt, ...) {
    if (!env_.log) return;
    char text[512];
    va_list v;
    va_start(v, fmt);
    std::vsnprintf(text, sizeof text, fmt, v);
    va_end(v);
    env_.log(level, text);
}

uint32_t Receiver::local_caps() const {
    uint32_t c = wire::kCapFollowPoses;  // the receiver keeps poses; the surfaces draw them
    if (sw_.compass.load()) c |= wire::kCapCompass;
    if (sw_.world.load()) c |= wire::kCapWorld;
    if (sw_.map.load() && catalog_.get() && catalog_.map_verified()) c |= wire::kCapMap;
    if (sw_.map_text.load()) c |= wire::kCapMapText;
    return c;
}

wire::Header Receiver::header(uint64_t seq) const {
    wire::Header h;
    h.server_epoch = server_epoch_;
    h.client_nonce = nonce_;
    h.sequence = seq;
    return h;
}

void Receiver::enqueue(uint32_t message, std::vector<uint8_t>&& bytes) {
    if (outbox_.size() >= 32) outbox_.pop_front();  // a client sends little; a blocked transport loses the oldest control
    Packet p;
    p.message = message;
    p.bytes = std::move(bytes);
    p.tag = ++tags_;
    outbox_.push_back(std::move(p));
}

void Receiver::mint_nonce() {
    if (env_.random) env_.random(nonce_.data(), nonce_.size());
    if (nonce_ == kZero) nonce_[0] = 1;
    have_nonce_ = true;
}

// The link starts over: its records go (one CLEARED per namespace, cause), staging and the tuple go; a new nonce when asked.
void Receiver::note_text_skips() {
    const uint64_t s = store_.received_text_skips();
    if (s == text_skips_seen_) return;
    log(KILN_LOG_WARN, "markers: %llu received marker(s) kept their last value: a server mod's new marker text is over its budget "
        "for this link (256 KiB of text per server mod, 1 MiB in all); the budget starts again with the next link",
        static_cast<unsigned long long>(s - text_skips_seen_));
    text_skips_seen_ = s;
}

void Receiver::reset_link(uint32_t cause, bool new_nonce) {
    if (store_.received_count()) store_.received_clear(cause);
    view_.clear();
    applied_ = 0;
    staging_ = Staging{};
    server_epoch_ = {};
    accepted_ = 0;
    need_catalog_ = false;
    want_resync_ = false;
    outbox_.clear();
    phase_ = Phase::Idle;
    if (new_nonce) have_nonce_ = false;
}

void Receiver::world_started() {
    uint64_t epoch = 0;
    store_.world_epoch(KILN_WORLD_CLIENT, &epoch);
    std::lock_guard g(mu_);
    world_ = epoch;
    reset_link(KILN_MARKER_WORLD_STOPPED, true);  // a new client world is a new tuple
}

void Receiver::world_stopping() {
    std::lock_guard g(mu_);
    world_ = 0;
    // the store clears the received records with the client world (one CLEARED per namespace, WORLD_STOPPED)
    view_.clear();
    applied_ = 0;
    staging_ = Staging{};
    server_epoch_ = {};
    outbox_.clear();
    phase_ = Phase::Idle;
    have_nonce_ = false;
}

KilnResult Receiver::peer_up(const KilnMarkerPeer& p) {
    if (p.role != kRoleClient || p.peer != kServerPeer) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard g(mu_);
    if (link_.up && link_.generation != p.generation) reset_link(KILN_MARKER_PEER_LEFT, true);  // another connection
    if (!link_.up || link_.generation != p.generation) have_nonce_ = false;
    link_.up = true;
    link_.peer = p.peer;
    link_.route = p.route;
    link_.flags = p.flags;
    link_.max_body = p.max_message_bytes;
    link_.generation = p.generation;
    return KILN_OK;
}

KilnResult Receiver::peer_down(uint32_t peer, uint64_t generation) {
    std::lock_guard g(mu_);
    if (!link_.up || link_.peer != peer || link_.generation != generation) return KILN_ERR_NOT_FOUND;
    reset_link(KILN_MARKER_PEER_LEFT, true);
    link_.up = false;
    return KILN_OK;
}

void Receiver::bad(uint64_t now, const char* why) {
    bad_.erase(std::remove_if(bad_.begin(), bad_.end(), [now](uint64_t t) { return now - t >= 10000; }), bad_.end());
    bad_.push_back(now);
    const bool suspend = bad_.size() >= 3 && !suspended_until_;
    if (suspend) suspended_until_ = now + 10000;  // nothing is read for 10 s; then the link starts over
    if (suspend || now - last_bad_log_ >= 1000 || !last_bad_log_) {
        last_bad_log_ = now ? now : 1;
        log(KILN_LOG_WARN, "markers: a bad marker message from the server: %s%s", why,
            suspend ? " (3 in 10 s: the marker link is suspended; it says HELLO again in 10 s)" : "");
    }
}

void Receiver::ack(uint64_t applied, uint8_t status, const std::string& reason) {
    wire::Ack a;
    a.applied = applied;
    a.status = status;
    a.reason = reason.size() > wire::kTextBytes ? reason.substr(0, wire::kTextBytes) : reason;
    enqueue(wire::kAck, wire::encode_ack(header(applied_), a));
}

void Receiver::resync(uint8_t reason, uint64_t now) {
    if (resync_at_[1] && now - resync_at_[1] < 1000) {  // at most 2 a second: once more at the next step
        want_resync_ = true;
        resync_reason_ = reason;
        return;
    }
    resync_at_[1] = resync_at_[0];
    resync_at_[0] = now ? now : 1;
    want_resync_ = false;
    staging_ = Staging{};
    wire::Resync r;
    r.last_applied = applied_;
    r.reason = reason;
    enqueue(wire::kResync, wire::encode_resync(header(applied_), r));
}

void Receiver::hello(uint64_t now) {
    if (!have_nonce_) mint_nonce();
    wire::Hello m;
    m.client_world_epoch = world_;
    wire::from_hex(env_.exe, m.exe_sha256.data(), 32);
    m.capabilities = local_caps();
    m.max_records = static_cast<uint16_t>(wire::kSnapshotRecords);
    m.max_body = static_cast<uint32_t>(std::min<size_t>(wire::kMaxBody, link_.max_body ? link_.max_body : wire::kMaxBody));
    if (const auto c = catalog_.get()) m.catalog_digest = c->digest;
    wire::Header h;
    h.client_nonce = nonce_;  // HELLO: no server epoch, sequence 0
    enqueue(wire::kHello, wire::encode_hello(h, m));
    caps_sent_ = m.capabilities;
    digest_sent_ = m.catalog_digest;
    ++hellos_;
    next_hello_ = now + hello_interval(hellos_);
}

// 1 s five times, then 5 s doubling to at most 60 s: a server that never answers (an older Kiln whose markers refuse this
// HELLO, a server world that is gone while its connection lingers) costs a HELLO a minute, not one every 5 s for ever.
// A new link, a new client world or a lapsed lease start over at 1 s.
uint64_t Receiver::hello_interval(uint32_t sent) {
    if (sent <= 5) return 1000;
    uint64_t ms = 5000;
    for (uint32_t i = 6; i < sent && ms < 60000; ++i) ms *= 2;
    return ms < 60000 ? ms : 60000;
}

void Receiver::send_catalog() {
    const auto c = catalog_.get();
    if (!c) return;
    wire::Catalog m;
    m.digest = c->digest;
    for (const IconRow& r : c->rows) {
        wire::CatalogRow w;
        w.id = r.id;
        std::memcpy(w.image.data(), r.image, 16);
        w.flags = r.flags;
        w.name = r.name.size() > wire::kTextBytes ? r.name.substr(0, wire::kTextBytes) : r.name;
        m.rows.push_back(std::move(w));
    }
    if (m.rows.size() > wire::kCatalogRows) return;
    auto bytes = wire::encode_catalog(header(applied_), m);
    if (bytes.size() > wire::kMaxBody) return;  // not the verified catalog of any build (it has 43 rows)
    enqueue(wire::kCatalog, std::move(bytes));
}

const char* Receiver::unsupported(const wire::Record& r) const {
    const uint32_t can = accepted_ & local_caps();
    uint32_t need = r.surfaces;
    if (r.label_mode == KILN_MARKER_LABEL_NATIVE_TOOLTIP) need |= wire::kCapMapText;
    if (r.followed) need |= wire::kCapFollowPoses;
    if (need & ~can) return "a record asks for a surface this client does not show";
    if ((r.surfaces & KILN_MARKER_MAP) && CatalogHolder::check(r.icon, const_cast<CatalogHolder*>(&catalog_)) != IconAnswer::Ok)
        return "a record's map icon is not one this client can draw";
    return nullptr;
}

ReceivedRecord Receiver::to_store(const wire::Record& r) const {
    ReceivedRecord o;
    o.ns = r.ns;
    o.remote_generation = r.publisher_generation;
    o.key = r.key;
    o.wire_id = r.wire_id;
    o.revision = r.revision;
    o.rec.surfaces = r.surfaces;
    o.rec.x = r.x;
    o.rec.y = r.y;
    o.rec.z = r.z;
    o.rec.height = r.height;
    o.rec.max_distance = r.max_distance;
    o.rec.icon = r.icon;
    o.rec.rgba = r.rgba;
    o.rec.priority = r.priority;
    o.rec.label_mode = r.label_mode;
    o.rec.text = r.text;  // the store interns it
    o.source_reason = r.source_reason;
    o.followed = r.followed != 0;
    return o;
}

// ---- server -> client ---------------------------------------------------------------------------------------------------

void Receiver::on_welcome(const wire::Header& h, const wire::Welcome& w, uint64_t now) {
    if (phase_ == Phase::Idle || !have_nonce_ || h.client_nonce != nonce_ || h.server_epoch == kZero) return;  // not our HELLO's answer
    const bool same_tuple = phase_ == Phase::Linked && h.server_epoch == server_epoch_;
    if (!same_tuple) {
        if (server_epoch_ != kZero && h.server_epoch != server_epoch_) {
            // another server world: its records were the last server's
            if (store_.received_count()) store_.received_clear(KILN_MARKER_PEER_LEFT);
            view_.clear();
            applied_ = 0;
            staging_ = Staging{};
        }
        server_epoch_ = h.server_epoch;
        phase_ = Phase::Linked;
        log(KILN_LOG_INFO, "markers: linked to the server's markers (accepted surfaces 0x%x%s)", w.accepted, w.need_catalog ? ", it asks for the icon catalog" : "");
    }
    accepted_ = w.accepted;
    lease_until_ = now + 10000;
    need_catalog_ = w.need_catalog != 0;
    if (need_catalog_) send_catalog();
}

void Receiver::on_heartbeat(const wire::Header& h, uint64_t now) {
    if (h.sequence > applied_ && !(staging_.active && staging_.seq >= h.sequence)) {
        resync(wire::kResyncHeartbeat, now);  // the server is ahead: a transaction was lost
    } else if (h.sequence == applied_ && applied_ && now - last_reack_ >= 2000) {
        last_reack_ = now;
        ack(applied_, wire::kAckAccepted, {});  // in case our ACK was lost and the server waits for it
    }
}

void Receiver::on_snap_begin(const wire::Header& h, const wire::SnapBegin& b, uint64_t now) {
    if (h.sequence <= applied_) return;  // an older transaction: ignored (END acknowledges it again)
    if (staging_.active && staging_.seq == h.sequence && staging_.id == b.snapshot_id) return;  // the same BEGIN again
    staging_ = Staging{};
    staging_.active = true;
    staging_.seq = h.sequence;
    staging_.id = b.snapshot_id;
    staging_.records = b.total_records;
    staging_.chunks = b.total_chunks;
    staging_.have.assign(b.total_chunks, false);
    staging_.parts.resize(b.total_chunks);
    staging_.raw.resize(b.total_chunks);
    staging_.started = now;
}

void Receiver::on_snap_chunk(const wire::Header& h, wire::SnapChunk& c, uint64_t now) {
    if (h.sequence <= applied_) return;
    if (!staging_.active || staging_.seq != h.sequence || staging_.id != c.snapshot_id) return;  // not the snapshot being staged
    if (c.index >= staging_.chunks) return bad(now, "SNAP_CHUNK's index is past the snapshot's chunks");
    const size_t expect = std::min<size_t>(wire::kChunkRecords, staging_.records - size_t{c.index} * wire::kChunkRecords);
    if (c.records.size() != expect) return bad(now, "SNAP_CHUNK's record count does not fit the snapshot");
    if (staging_.have[c.index]) {
        if (staging_.raw[c.index] == c.record_bytes) return;  // a duplicate: no change, no event
        bad(now, "two different SNAP_CHUNKs with one index");
        return resync(wire::kResyncStaging, now);
    }
    if (staging_.bytes + c.record_bytes.size() > size_t{256} << 10) {
        bad(now, "a snapshot over 256 KiB");
        return resync(wire::kResyncStaging, now);
    }
    staging_.bytes += c.record_bytes.size();
    staging_.have[c.index] = true;
    staging_.received += c.records.size();
    staging_.parts[c.index] = std::move(c.records);
    staging_.raw[c.index] = std::move(c.record_bytes);
}

void Receiver::on_snap_end(const wire::Header& h, const wire::SnapEnd& e, uint64_t) {
    if (h.sequence <= applied_) {
        ack(applied_, wire::kAckAccepted, {});  // already applied: acknowledged again, no event
        return;
    }
    if (!staging_.active || staging_.seq != h.sequence || staging_.id != e.snapshot_id) return;
    staging_.ended = true;
    staging_.digest = e.digest;
}

// Validates up to 128 staged rows per step; publishes the whole snapshot once every chunk, the END and the digest are in.
void Receiver::advance_staging(uint64_t now) {
    if (!staging_.active) return;
    if (now - staging_.started >= 5000) {  // staging expires: ask for the current state again
        log(KILN_LOG_INFO, "markers: a snapshot did not complete in 5 s: asking the server again");
        return resync(wire::kResyncStaging, now);
    }
    if (std::find(staging_.have.begin(), staging_.have.end(), false) != staging_.have.end()) return;
    std::vector<const wire::Record*> rows;
    rows.reserve(staging_.records);
    for (const auto& part : staging_.parts)
        for (const wire::Record& r : part) rows.push_back(&r);
    size_t budget = 128;
    while (staging_.validated < rows.size() && budget) {
        const wire::Record& r = *rows[staging_.validated];
        if (const char* why = wire::check_record(r)) {
            bad(now, why);
            return resync(wire::kResyncStaging, now);
        }
        ++staging_.validated;
        --budget;
    }
    if (staging_.validated < rows.size() || !staging_.ended) return;
    // the digest, the order and the keys
    Sha256 sha;
    for (const auto& raw : staging_.raw) sha.update(raw.data(), raw.size());
    if (sha.finish() != staging_.digest) {
        bad(now, "a snapshot whose digest does not match its records");
        return resync(wire::kResyncStaging, now);
    }
    std::set<uint64_t> ids;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i && !wire::record_order(*rows[i - 1], *rows[i])) {
            bad(now, "a snapshot out of order or with one key twice");
            return resync(wire::kResyncStaging, now);
        }
        if (!ids.insert(rows[i]->wire_id).second) {
            bad(now, "a snapshot with one wire id twice");
            return resync(wire::kResyncStaging, now);
        }
    }
    const uint64_t seq = staging_.seq;
    for (const wire::Record* r : rows)
        if (const char* why = unsupported(*r)) {
            staging_ = Staging{};
            ack(seq, wire::kAckUnsupported, why);  // the prior view stays
            return;
        }
    std::vector<ReceivedRecord> ups;
    ups.reserve(rows.size());
    for (const wire::Record* r : rows) ups.push_back(to_store(*r));
    std::string why;
    if (!store_.received_apply(link_.generation, ups, {}, true, KILN_MARKER_SNAPSHOT, &why)) {
        staging_ = Staging{};
        ack(seq, wire::kAckFull, why);  // the prior view stays
        return;
    }
    note_text_skips();
    view_.clear();
    for (const wire::Record* r : rows) view_[r->wire_id] = VRow{r->ns, r->publisher_generation, r->key, r->revision, r->followed != 0, r->source_reason, *r};
    applied_ = seq;
    staging_ = Staging{};
    ack(seq, wire::kAckAccepted, {});
}

Receiver::Done Receiver::on_delta(const wire::Header& h, std::vector<wire::DeltaOp>& ops, uint64_t now) {
    if (h.sequence <= applied_) {
        ack(applied_, wire::kAckAccepted, {});
        return Done::Ok;
    }
    if (h.sequence != applied_ + 1) {
        resync(wire::kResyncGap, now);
        return Done::Ok;
    }
    if (!delta_ops_.take(static_cast<double>(ops.size()), now)) return Done::Defer;  // 128 changes a second
    // the transaction on a copy of the view: all or nothing
    std::map<uint64_t, VRow> view = view_;
    std::map<std::tuple<std::string, uint64_t, std::string>, uint64_t> by_key;
    for (const auto& [id, v] : view) by_key[{v.ns, v.gen, v.key}] = id;
    std::vector<ReceivedRecord> ups;
    std::vector<std::tuple<std::string, uint64_t, std::string>> removes;
    for (const wire::DeltaOp& op : ops) {
        if (op.op == wire::kRemove) {
            auto it = view.find(op.remove_id);
            if (it == view.end()) continue;  // unknown removes are harmless
            const auto k = std::make_tuple(it->second.ns, it->second.gen, it->second.key);
            removes.push_back(k);
            ups.erase(std::remove_if(ups.begin(), ups.end(), [&](const ReceivedRecord& u) { return u.ns == std::get<0>(k) && u.remote_generation == std::get<1>(k) && u.key == std::get<2>(k); }), ups.end());
            by_key.erase(k);
            view.erase(it);
            continue;
        }
        const wire::Record& r = op.record;
        if (const char* why = wire::check_record(r)) {
            bad(now, why);
            ack(h.sequence, wire::kAckBadBaseline, why);
            return Done::Ok;
        }
        if (const char* why = unsupported(r)) {
            ack(h.sequence, wire::kAckUnsupported, why);
            return Done::Ok;
        }
        const auto k = std::make_tuple(r.ns, r.publisher_generation, r.key);
        auto it = view.find(r.wire_id);
        if (it != view.end()) {
            const VRow& v = it->second;
            if (v.ns != r.ns || v.gen != r.publisher_generation || v.key != r.key) {
                ack(h.sequence, wire::kAckBadBaseline, "an upsert names a wire id of another key");
                return Done::Ok;
            }
            if (r.revision < v.revision || (r.revision == v.revision && !(r == v.rec))) {
                ack(h.sequence, wire::kAckBadBaseline, "an upsert's revision is not after the one held");
                return Done::Ok;
            }
            if (r.revision == v.revision) continue;  // the same revision again: no change
        } else if (auto other = by_key.find(k); other != by_key.end()) {
            ack(h.sequence, wire::kAckBadBaseline, "an upsert adds a key held under another wire id");
            return Done::Ok;
        }
        removes.erase(std::remove(removes.begin(), removes.end(), k), removes.end());
        ups.erase(std::remove_if(ups.begin(), ups.end(), [&](const ReceivedRecord& u) { return u.ns == r.ns && u.remote_generation == r.publisher_generation && u.key == r.key; }), ups.end());
        ups.push_back(to_store(r));
        by_key[k] = r.wire_id;
        view[r.wire_id] = VRow{r.ns, r.publisher_generation, r.key, r.revision, r.followed != 0, r.source_reason, r};
    }
    std::string why;
    if (!store_.received_apply(link_.generation, ups, removes, false, KILN_MARKER_API, &why)) {
        ack(h.sequence, wire::kAckFull, why);
        return Done::Ok;
    }
    note_text_skips();
    view_ = std::move(view);
    applied_ = h.sequence;
    ack(applied_, wire::kAckAccepted, {});
    return Done::Ok;
}

Receiver::Done Receiver::on_pose(const wire::Header& h, const std::vector<wire::Pose>& poses, uint64_t now) {
    if (h.sequence <= applied_) {
        ack(applied_, wire::kAckAccepted, {});
        return Done::Ok;
    }
    if (h.sequence != applied_ + 1) {
        resync(wire::kResyncGap, now);
        return Done::Ok;
    }
    if (pose_rows_ + poses.size() > 128 && pose_rows_) return Done::Defer;  // 128 pose rows a step
    for (const wire::Pose& q : poses) {
        auto it = view_.find(q.wire_id);
        const bool reason_ok = q.source_reason == KILN_MARKER_READY || q.source_reason == KILN_MARKER_FOLLOW_MISSING ||
                               q.source_reason == KILN_MARKER_ANCHOR_ENDED || q.source_reason == KILN_MARKER_BAD_POSITION;
        if (it == view_.end() || !it->second.followed || it->second.revision != q.revision || !reason_ok || !in_world(q.x, q.y, q.z)) {
            // never creates a record: the view is not what the server thinks
            resync(wire::kResyncPose, now);
            return Done::Ok;
        }
    }
    pose_rows_ += static_cast<uint32_t>(poses.size());
    for (const wire::Pose& q : poses) {
        VRow& v = view_[q.wire_id];
        if (v.reason == KILN_MARKER_ANCHOR_ENDED) continue;  // a pose cannot bring an ended anchor back (a new revision can)
        v.reason = q.source_reason;
        v.rec.x = q.x;
        v.rec.y = q.y;
        v.rec.z = q.z;
        v.rec.source_reason = static_cast<uint16_t>(q.source_reason);
        store_.received_pose(v.ns, v.gen, v.key, q.x, q.y, q.z, q.source_reason);
    }
    applied_ = h.sequence;
    ack(applied_, wire::kAckAccepted, {});
    return Done::Ok;
}

Receiver::Done Receiver::handle(Inbound& m, uint64_t now) {
    const uint8_t* p = m.bytes.data();
    const size_t n = m.bytes.size();
    if (m.message >= wire::kMessages || !wire::to_client(m.message)) {
        bad(now, "a message a server may not send");
        return Done::Ok;
    }
    wire::Header h;
    const char* why = "";
    auto ours = [&] { return phase_ == Phase::Linked && h.server_epoch == server_epoch_ && h.client_nonce == nonce_; };
    switch (m.message) {
        case wire::kWelcome: {
            wire::Welcome w;
            if (!wire::decode_welcome(p, n, &h, &w, &why) || !wire::check_header(h)) {
                bad(now, *why ? why : "WELCOME's header is not schema 1");
                return Done::Ok;
            }
            on_welcome(h, w, now);
            return Done::Ok;
        }
        case wire::kSnapBegin: {
            wire::SnapBegin b;
            if (!wire::decode_snap_begin(p, n, &h, &b, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;
            on_snap_begin(h, b, now);
            return Done::Ok;
        }
        case wire::kSnapChunk: {
            wire::SnapChunk c;
            if (!wire::decode_snap_chunk(p, n, &h, &c, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;
            on_snap_chunk(h, c, now);
            return Done::Ok;
        }
        case wire::kSnapEnd: {
            wire::SnapEnd e;
            if (!wire::decode_snap_end(p, n, &h, &e, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;
            on_snap_end(h, e, now);
            return Done::Ok;
        }
        case wire::kDelta: {
            std::vector<wire::DeltaOp> ops;
            if (!wire::decode_delta(p, n, &h, &ops, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;
            return on_delta(h, ops, now);
        }
        case wire::kPose: {
            std::vector<wire::Pose> poses;
            if (!wire::decode_pose(p, n, &h, &poses, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;
            return on_pose(h, poses, now);
        }
        case wire::kHeartbeat: {
            if (!wire::decode_heartbeat(p, n, &h, &why) || !wire::check_header(h)) break;
            if (!ours()) return Done::Ok;
            lease_until_ = now + 10000;  // a matching heartbeat renews the lease
            on_heartbeat(h, now);
            return Done::Ok;
        }
        default: return Done::Ok;
    }
    bad(now, *why ? why : "a header that is not schema 1");
    return Done::Ok;
}

void Receiver::post() {
    std::lock_guard g(mu_);
    const uint64_t now = env_.now_ms ? env_.now_ms() : 0;
    pose_rows_ = 0;
    // a suspended link reads nothing; when the suspension ends, the link starts over
    if (suspended_until_ && now >= suspended_until_) {
        suspended_until_ = 0;
        bad_.clear();
        reset_link(KILN_MARKER_LINK_STALE, true);
    }
    std::deque<Inbound> q = inbox_.take();
    std::deque<Inbound> later;
    for (Inbound& m : q) {
        if (!later.empty()) {  // after the first deferred one, the rest waits too (order)
            later.push_back(std::move(m));
            continue;
        }
        if (!link_.up || m.peer != link_.peer || m.generation != link_.generation) continue;  // another connection
        if (suspended_until_) continue;
        const double size = static_cast<double>(m.bytes.size());
        if (data_msgs_.peek(now) < 1 || data_bytes_.peek(now) < size) {  // 128 messages, 256 KiB a second
            later.push_back(std::move(m));
            continue;
        }
        if (handle(m, now) == Done::Defer) {
            later.push_back(std::move(m));
            continue;
        }
        data_msgs_.take(1, now);
        data_bytes_.take(size, now);
    }
    inbox_.put_back(std::move(later));
    for (const auto& [who, n] : inbox_.take_drops())
        if (link_.up && who.first == link_.peer && who.second == link_.generation && phase_ == Phase::Linked) want_resync_ = true;  // a lost message
    if (!link_.up || !world_) {
        phase_ = Phase::Idle;
        return;
    }
    if (!(link_.flags & kNetworkReady) || !(link_.flags & kHasMarkersChannel)) return;  // the server's channel is not there (yet)
    if (link_.max_body && link_.max_body < wire::kMaxBody) {
        if (!body_warned_) log(KILN_LOG_WARN, "markers: the marker channel's limit is %u bytes, below 16384: received markers are off", link_.max_body);
        body_warned_ = true;
        return;
    }
    if (suspended_until_) return;
    if (phase_ == Phase::Idle) {
        phase_ = Phase::Hello;
        hellos_ = 0;
        next_hello_ = now;
    }
    if (phase_ == Phase::Hello) {
        if (now >= next_hello_) hello(now);
        return;
    }
    // Linked
    if (now > lease_until_) {
        log(KILN_LOG_INFO, "markers: no heartbeat from the server's markers for 10 s: received markers cleared, saying HELLO again");
        reset_link(KILN_MARKER_LINK_STALE, true);
        phase_ = Phase::Hello;
        hellos_ = 0;
        next_hello_ = now;
        hello(now);
        return;
    }
    // a change of what this client can show, or of its catalog: HELLO again with the same nonce (state kept)
    const auto c = catalog_.get();
    const wire::Digest digest = c ? c->digest : wire::Digest{};
    if (local_caps() != caps_sent_ || digest != digest_sent_) hello(now);
    advance_staging(now);
    if (want_resync_) resync(resync_reason_ ? resync_reason_ : static_cast<uint8_t>(wire::kResyncOverflow), now);
}

void Receiver::collect(uint32_t max_packets, Bucket& process, uint64_t now, std::vector<Outgoing>* out) {
    std::lock_guard g(mu_);
    if (!link_.up) return;
    uint32_t n = 0;
    for (const Packet& p : outbox_) {
        if (n >= 4 || n >= max_packets) break;
        const double size = static_cast<double>(p.bytes.size());
        if (out_bytes_.peek(now) < size || process.peek(now) < size) break;
        out_bytes_.take(size, now);
        process.take(size, now);
        Outgoing o;
        o.peer = link_.peer;
        o.generation = link_.generation;
        o.route = link_.route;
        o.message = p.message;
        o.bytes = p.bytes;
        o.tag = p.tag;
        out->push_back(std::move(o));
        ++n;
    }
}

void Receiver::sent(const Outgoing& o, bool ok, uint64_t) {
    if (!ok) return;
    std::lock_guard g(mu_);
    auto it = std::find_if(outbox_.begin(), outbox_.end(), [&](const Packet& p) { return p.tag == o.tag; });
    if (it != outbox_.end()) outbox_.erase(it);
}

Receiver::Info Receiver::info() const {
    std::lock_guard g(mu_);
    Info i;
    i.phase = phase_;
    i.suspended = suspended_until_ != 0;
    i.staging = staging_.active;
    i.need_catalog = need_catalog_;
    i.accepted = accepted_;
    i.view = static_cast<uint32_t>(view_.size());
    i.hellos = hellos_;
    i.applied = applied_;
    i.lease_until = lease_until_;
    i.nonce = nonce_;
    i.server_epoch = server_epoch_;
    return i;
}

}  // namespace kmk
