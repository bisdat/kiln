// SPDX-License-Identifier: MIT
#include "wire.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "services/kiln_markers.h"
#include "sha256.h"
#include "text.h"

namespace kmk::wire {

// ---- bytes --------------------------------------------------------------------------------------------------------------

void Writer::u16(uint16_t v) {
    bytes.push_back(static_cast<uint8_t>(v));
    bytes.push_back(static_cast<uint8_t>(v >> 8));
}
void Writer::u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Writer::u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Writer::f32(float v) {
    uint32_t b;
    std::memcpy(&b, &v, 4);
    u32(b);
}
void Writer::f64(double v) {
    uint64_t b;
    std::memcpy(&b, &v, 8);
    u64(b);
}
void Writer::raw(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    bytes.insert(bytes.end(), b, b + n);
}
void Writer::str16(std::string_view s) {
    u16(static_cast<uint16_t>(s.size()));
    raw(s.data(), s.size());
}

const uint8_t* Reader::need(size_t n) {
    if (!ok_) return nullptr;
    if (n > n_ - at_) {
        fail("the message ends early");
        return nullptr;
    }
    const uint8_t* p = p_ + at_;
    at_ += n;
    return p;
}
uint8_t Reader::u8() {
    const uint8_t* p = need(1);
    return p ? p[0] : 0;
}
uint16_t Reader::u16() {
    const uint8_t* p = need(2);
    return p ? static_cast<uint16_t>(p[0] | (p[1] << 8)) : 0;
}
uint32_t Reader::u32() {
    const uint8_t* p = need(4);
    if (!p) return 0;
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
uint64_t Reader::u64() {
    const uint8_t* p = need(8);
    if (!p) return 0;
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}
float Reader::f32() {
    const uint32_t b = u32();
    float v;
    std::memcpy(&v, &b, 4);
    return v;
}
double Reader::f64() {
    const uint64_t b = u64();
    double v;
    std::memcpy(&v, &b, 8);
    return v;
}
void Reader::raw(void* out, size_t n) {
    const uint8_t* p = need(n);
    if (p) std::memcpy(out, p, n);
    else std::memset(out, 0, n);
}
std::string Reader::str16(size_t max) {
    const uint16_t len = u16();
    if (!ok_) return {};
    if (len > max) {
        fail("a text is longer than its limit");
        return {};
    }
    const uint8_t* p = need(len);
    if (!p) return {};
    const char* s = reinterpret_cast<const char*>(p);
    if (text_problem(s, len, max)) {
        fail("a text is not strict UTF-8 without control characters");
        return {};
    }
    return std::string(s, len);
}

// ---- header -------------------------------------------------------------------------------------------------------------

namespace {

void put_header(Writer& w, const Header& h) {
    w.bytes.reserve(256);
    w.u16(h.schema);
    w.u16(h.flags);
    w.raw(h.server_epoch.data(), 16);
    w.raw(h.client_nonce.data(), 16);
    w.u64(h.sequence);
}

void get_header(Reader& r, Header* h) {
    h->schema = r.u16();
    h->flags = r.u16();
    r.raw(h->server_epoch.data(), 16);
    r.raw(h->client_nonce.data(), 16);
    h->sequence = r.u64();
}

// The end of a decode: every byte read, the body within 16384.
bool finish(Reader& r, size_t n, const char** why) {
    if (r.ok() && !r.done()) r.fail("the message has bytes after its end");
    if (r.ok() && n > kMaxBody) r.fail("the message is longer than 16384 bytes");
    if (!r.ok() && why) *why = r.why();
    return r.ok();
}

bool start(const uint8_t* p, size_t n, Reader& r, Header* h, const char** why) {
    if (!p && n) {
        if (why) *why = "no bytes";
        return false;
    }
    if (n > kMaxBody) {
        if (why) *why = "the message is longer than 16384 bytes";
        return false;
    }
    get_header(r, h);
    if (!r.ok() && why) *why = r.why();
    return r.ok();
}

}  // namespace

bool check_header(const Header& h) { return h.schema == kSchema && h.flags == 0; }

bool decode_header(const uint8_t* p, size_t n, Header* h, const char** why) {
    Reader r(p, n);
    return start(p, n, r, h, why);
}

// ---- records ------------------------------------------------------------------------------------------------------------

void encode_record(Writer& w, const Record& r) {
    w.u64(r.wire_id);
    w.u64(r.publisher_generation);
    w.str16(r.ns);
    w.str16(r.key);
    w.u64(r.revision);
    w.u32(r.surfaces);
    w.i16(r.priority);
    w.u32(r.rgba);
    w.u32(r.icon);
    w.u8(r.label_mode);
    w.u8(r.followed);
    w.u16(r.source_reason);
    w.f64(r.x);
    w.f64(r.y);
    w.f64(r.z);
    w.f32(r.height);
    w.f32(r.max_distance);
    w.str16(r.text);
}

bool decode_record(Reader& r, Record* out) {
    const size_t at = r.at();
    out->wire_id = r.u64();
    out->publisher_generation = r.u64();
    out->ns = r.str16(kNameBytes);
    out->key = r.str16(kNameBytes);
    out->revision = r.u64();
    out->surfaces = r.u32();
    out->priority = r.i16();
    out->rgba = r.u32();
    out->icon = r.u32();
    out->label_mode = r.u8();
    out->followed = r.u8();
    out->source_reason = r.u16();
    out->x = r.f64();
    out->y = r.f64();
    out->z = r.f64();
    out->height = r.f32();
    out->max_distance = r.f32();
    out->text = r.str16(kTextBytes);
    if (r.ok() && r.at() - at > kMaxRecordBytes) r.fail("a record is longer than 344 bytes");
    return r.ok();
}

size_t record_size(const Record& r) { return 74 + 2 + r.ns.size() + 2 + r.key.size() + 2 + r.text.size(); }

namespace {
bool in(double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; }
}  // namespace

const char* check_record(const Record& r) {
    if (!r.wire_id) return "a record has wire id 0";
    if (!r.revision) return "a record has revision 0";
    if (!valid_identifier(r.ns.data(), r.ns.size())) return "a record's namespace is not 1 to 64 bytes of [A-Za-z0-9_.:-]";
    if (!valid_identifier(r.key.data(), r.key.size())) return "a record's key is not 1 to 64 bytes of [A-Za-z0-9_.:-]";
    constexpr uint32_t all = KILN_MARKER_MAP | KILN_MARKER_COMPASS | KILN_MARKER_WORLD;
    if (!r.surfaces || (r.surfaces & ~all)) return "a record's surfaces are not a nonzero mask of MAP, COMPASS and WORLD";
    if (r.label_mode > KILN_MARKER_LABEL_NATIVE_TOOLTIP) return "a record's map label mode is unknown";
    if (r.label_mode == KILN_MARKER_LABEL_NATIVE_TOOLTIP && !(r.surfaces & KILN_MARKER_MAP)) return "a record's native tooltip has no MAP surface";
    if (r.followed > 1) return "a record's followed flag is not 0 or 1";
    switch (r.source_reason) {
        case KILN_MARKER_READY: break;
        case KILN_MARKER_FOLLOW_MISSING:
        case KILN_MARKER_ANCHOR_ENDED:
        case KILN_MARKER_BAD_POSITION:
            if (!r.followed) return "a fixed record's source reason is not READY";
            break;
        default: return "a record's source reason is not READY, FOLLOW_MISSING, ANCHOR_ENDED or BAD_POSITION";
    }
    if (!in(r.x, -1000, 12000) || !in(r.z, -1000, 12000) || !in(r.y, -1000, 5000)) return "a record's position is not finite inside the world";
    if (!in(r.height, -128, 128)) return "a record's height is not finite in [-128, 128]";
    if (!in(r.max_distance, 0, 100000)) return "a record's max distance is not finite in [0, 100000]";
    if ((r.surfaces & KILN_MARKER_MAP) ? r.icon == 0 : r.icon != 0) return "a record's icon does not match its MAP surface";
    return nullptr;
}

bool record_order(const Record& a, const Record& b) {
    if (a.ns != b.ns) return a.ns < b.ns;
    if (a.publisher_generation != b.publisher_generation) return a.publisher_generation < b.publisher_generation;
    return a.key < b.key;
}

Digest snapshot_digest(const std::vector<Record>& sorted) {
    Sha256 h;
    for (const Record& r : sorted) {
        Writer w;
        encode_record(w, r);
        h.update(w.bytes.data(), w.bytes.size());
    }
    return h.finish();
}

// ---- messages -----------------------------------------------------------------------------------------------------------

std::vector<uint8_t> encode_hello(const Header& h, const Hello& m) {
    Writer w;
    put_header(w, h);
    w.u64(m.client_world_epoch);
    w.raw(m.exe_sha256.data(), 32);
    w.u32(m.capabilities);
    w.u16(m.max_records);
    w.u32(m.max_body);
    w.raw(m.catalog_digest.data(), 32);
    return std::move(w.bytes);
}

bool decode_hello(const uint8_t* p, size_t n, Header* h, Hello* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->client_world_epoch = r.u64();
    r.raw(m->exe_sha256.data(), 32);
    m->capabilities = r.u32();
    m->max_records = r.u16();
    m->max_body = r.u32();
    r.raw(m->catalog_digest.data(), 32);
    return finish(r, n, why);
}

std::vector<uint8_t> encode_welcome(const Header& h, const Welcome& m) {
    Writer w;
    put_header(w, h);
    w.u32(m.accepted);
    w.u16(m.max_records);
    w.u8(m.need_catalog);
    w.u8(m.reserved);
    return std::move(w.bytes);
}

bool decode_welcome(const uint8_t* p, size_t n, Header* h, Welcome* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->accepted = r.u32();
    m->max_records = r.u16();
    m->need_catalog = r.u8();
    m->reserved = r.u8();
    if (r.ok() && m->need_catalog > 1) r.fail("WELCOME's needCatalog is not 0 or 1");
    return finish(r, n, why);
}

std::vector<uint8_t> encode_catalog(const Header& h, const Catalog& m) {
    Writer w;
    put_header(w, h);
    w.raw(m.digest.data(), 32);
    w.u16(static_cast<uint16_t>(m.rows.size()));
    for (const CatalogRow& row : m.rows) {
        w.u32(row.id);
        w.raw(row.image.data(), 16);
        w.u32(row.flags);
        w.str16(row.name);
    }
    return std::move(w.bytes);
}

bool decode_catalog(const uint8_t* p, size_t n, Header* h, Catalog* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    r.raw(m->digest.data(), 32);
    const uint16_t count = r.u16();
    if (r.ok() && count > kCatalogRows) r.fail("CATALOG has more than 128 rows");
    m->rows.clear();
    for (uint32_t i = 0; r.ok() && i < count; ++i) {
        CatalogRow row;
        row.id = r.u32();
        r.raw(row.image.data(), 16);
        row.flags = r.u32();
        row.name = r.str16(kTextBytes);
        if (r.ok() && row.name.empty()) r.fail("a CATALOG row has no name");
        m->rows.push_back(std::move(row));
    }
    return finish(r, n, why);
}

std::vector<uint8_t> encode_snap_begin(const Header& h, const SnapBegin& m) {
    Writer w;
    put_header(w, h);
    w.u64(m.snapshot_id);
    w.u16(m.total_records);
    w.u16(m.total_chunks);
    return std::move(w.bytes);
}

bool decode_snap_begin(const uint8_t* p, size_t n, Header* h, SnapBegin* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->snapshot_id = r.u64();
    m->total_records = r.u16();
    m->total_chunks = r.u16();
    if (r.ok() && m->total_records > kSnapshotRecords) r.fail("SNAP_BEGIN has more than 512 records");
    if (r.ok() && m->total_chunks != (m->total_records + kChunkRecords - 1) / kChunkRecords)
        r.fail("SNAP_BEGIN's chunk count does not fit its record count (32 per chunk)");
    return finish(r, n, why);
}

std::vector<uint8_t> encode_snap_chunk(const Header& h, uint64_t snapshot_id, uint16_t index, const std::vector<Record>& records) {
    Writer w;
    put_header(w, h);
    w.u64(snapshot_id);
    w.u16(index);
    w.u16(static_cast<uint16_t>(records.size()));
    for (const Record& rec : records) encode_record(w, rec);
    return std::move(w.bytes);
}

bool decode_snap_chunk(const uint8_t* p, size_t n, Header* h, SnapChunk* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->snapshot_id = r.u64();
    m->index = r.u16();
    const uint16_t count = r.u16();
    if (r.ok() && m->index >= kSnapshotChunks) r.fail("SNAP_CHUNK's index is 16 or more");
    if (r.ok() && (count == 0 || count > kChunkRecords)) r.fail("SNAP_CHUNK has no record or more than 32");
    m->records.clear();
    const size_t first = r.at();
    for (uint32_t i = 0; r.ok() && i < count; ++i) {
        Record rec;
        if (decode_record(r, &rec)) m->records.push_back(std::move(rec));
    }
    if (r.ok()) m->record_bytes.assign(p + first, p + r.at());
    return finish(r, n, why);
}

std::vector<uint8_t> encode_snap_end(const Header& h, const SnapEnd& m) {
    Writer w;
    put_header(w, h);
    w.u64(m.snapshot_id);
    w.raw(m.digest.data(), 32);
    return std::move(w.bytes);
}

bool decode_snap_end(const uint8_t* p, size_t n, Header* h, SnapEnd* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->snapshot_id = r.u64();
    r.raw(m->digest.data(), 32);
    return finish(r, n, why);
}

std::vector<uint8_t> encode_delta(const Header& h, const std::vector<DeltaOp>& ops) {
    Writer w;
    put_header(w, h);
    w.u16(static_cast<uint16_t>(ops.size()));
    for (const DeltaOp& op : ops) {
        w.u8(op.op);
        if (op.op == kUpsert) encode_record(w, op.record);
        else w.u64(op.remove_id);
    }
    return std::move(w.bytes);
}

bool decode_delta(const uint8_t* p, size_t n, Header* h, std::vector<DeltaOp>* ops, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    const uint16_t count = r.u16();
    if (r.ok() && (count == 0 || count > kDeltaOps)) r.fail("DELTA has no operation or more than 32");
    ops->clear();
    for (uint32_t i = 0; r.ok() && i < count; ++i) {
        DeltaOp op;
        op.op = r.u8();
        if (op.op == kUpsert) decode_record(r, &op.record);
        else if (op.op == kRemove) op.remove_id = r.u64();
        else r.fail("a DELTA operation is neither upsert (0) nor remove (1)");
        if (r.ok()) ops->push_back(std::move(op));
    }
    return finish(r, n, why);
}

std::vector<uint8_t> encode_pose(const Header& h, const std::vector<Pose>& poses) {
    Writer w;
    put_header(w, h);
    w.u16(static_cast<uint16_t>(poses.size()));
    for (const Pose& q : poses) {
        w.u64(q.wire_id);
        w.u64(q.revision);
        w.f64(q.x);
        w.f64(q.y);
        w.f64(q.z);
        w.u32(q.source_reason);
    }
    return std::move(w.bytes);
}

bool decode_pose(const uint8_t* p, size_t n, Header* h, std::vector<Pose>* poses, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    const uint16_t count = r.u16();
    if (r.ok() && (count == 0 || count > kPoses)) r.fail("POSE has no pose or more than 64");
    poses->clear();
    for (uint32_t i = 0; r.ok() && i < count; ++i) {
        Pose q;
        q.wire_id = r.u64();
        q.revision = r.u64();
        q.x = r.f64();
        q.y = r.f64();
        q.z = r.f64();
        q.source_reason = r.u32();
        if (r.ok()) poses->push_back(q);
    }
    return finish(r, n, why);
}

std::vector<uint8_t> encode_ack(const Header& h, const Ack& m) {
    Writer w;
    put_header(w, h);
    w.u64(m.applied);
    w.u8(m.status);
    w.str16(m.reason.size() > kTextBytes ? std::string_view(m.reason).substr(0, kTextBytes) : std::string_view(m.reason));
    return std::move(w.bytes);
}

bool decode_ack(const uint8_t* p, size_t n, Header* h, Ack* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->applied = r.u64();
    m->status = r.u8();
    m->reason = r.str16(kTextBytes);
    if (r.ok() && m->status > kAckBadBaseline) r.fail("ACK's status is unknown");
    return finish(r, n, why);
}

std::vector<uint8_t> encode_resync(const Header& h, const Resync& m) {
    Writer w;
    put_header(w, h);
    w.u64(m.last_applied);
    w.u8(m.reason);
    return std::move(w.bytes);
}

bool decode_resync(const uint8_t* p, size_t n, Header* h, Resync* m, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    m->last_applied = r.u64();
    m->reason = r.u8();
    return finish(r, n, why);
}

std::vector<uint8_t> encode_heartbeat(const Header& h) {
    Writer w;
    put_header(w, h);
    return std::move(w.bytes);
}

bool decode_heartbeat(const uint8_t* p, size_t n, Header* h, const char** why) {
    Reader r(p, n);
    if (!start(p, n, r, h, why)) return false;
    return finish(r, n, why);
}

// ---- hex ----------------------------------------------------------------------------------------------------------------

std::string hex(const uint8_t* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

bool from_hex(std::string_view s, uint8_t* out, size_t n) {
    if (s.size() != 2 * n) return false;
    auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
    for (size_t i = 0; i < n; ++i) {
        const int hi = v(s[2 * i]), lo = v(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    return true;
}

}  // namespace kmk::wire
