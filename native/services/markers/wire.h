// SPDX-License-Identifier: MIT
// kiln.markers wire v1 (dossier chapter 04 §4.4): the codecs of the eleven messages of channel "kiln:markers" version 1.
// Pure: no state, no clock, no allocation beyond the decoded values. Every read is bounds-checked against the message; a
// decode fails (false and a reason) on a short or long body, a count or length over its limit, or text that is not strict
// UTF-8 without control characters. The meaning of the values (identifier grammar, ranges, reasons) is checked by
// check_record, apart from the bytes.
//
// Every body is little-endian and at most 16384 bytes including the 44-byte common header:
//   u16 schema (1), u16 flags (0), 16-byte server epoch, 16-byte client nonce, u64 sequence.
// Epochs and nonces are session tokens in .NET Guid.ToByteArray order. str16 is a u16 BYTE length and that many bytes of
// strict UTF-8 with no NUL or control character. Message ids (NetChannel's message id) and directions:
//   0 HELLO C->S, 1 WELCOME S->C, 2 CATALOG C->S, 3 SNAP_BEGIN S->C, 4 SNAP_CHUNK S->C, 5 SNAP_END S->C, 6 DELTA S->C,
//   7 POSE S->C, 8 ACK C->S, 9 RESYNC C->S, 10 HEARTBEAT S->C.
// A DELTA operation is a u8 op: 0 = upsert (a RECORD follows), 1 = remove (a u64 wire id follows).
#ifndef KILN_MARKERS_WIRE_H
#define KILN_MARKERS_WIRE_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kmk::wire {

constexpr uint16_t kSchema = 1;
constexpr size_t kHeaderBytes = 44;
constexpr size_t kMaxBody = 16384;         // whole body, header included
constexpr size_t kMaxRecordBytes = 344;    // one encoded RECORD (the largest legal one is 336)
constexpr uint32_t kChunkRecords = 32;     // records per SNAP_CHUNK
constexpr uint32_t kSnapshotRecords = 512; // records per snapshot
constexpr uint32_t kSnapshotChunks = 16;
constexpr uint32_t kDeltaOps = 32;
constexpr uint32_t kPoses = 64;
constexpr uint32_t kCatalogRows = 128;     // parser ceiling (the verified catalog has 43)
constexpr size_t kNameBytes = 64;          // namespace, key
constexpr size_t kTextBytes = 128;         // marker text, catalog names, ACK reason

enum Message : uint32_t {
    kHello = 0, kWelcome = 1, kCatalog = 2, kSnapBegin = 3, kSnapChunk = 4, kSnapEnd = 5, kDelta = 6, kPose = 7,
    kAck = 8, kResync = 9, kHeartbeat = 10,
};
constexpr uint32_t kMessages = 11;
// Client to server: HELLO, CATALOG, ACK, RESYNC. Everything else goes server to client.
constexpr bool to_server(uint32_t m) { return m == kHello || m == kCatalog || m == kAck || m == kResync; }
constexpr bool to_client(uint32_t m) { return m < kMessages && !to_server(m); }

// HELLO capability bits. Compass means Kiln's drawn compass strip, never the game's own.
enum Capability : uint32_t { kCapMap = 1, kCapCompass = 2, kCapWorld = 4, kCapMapText = 8, kCapFollowPoses = 16 };

// ACK status
enum AckStatus : uint8_t { kAckAccepted = 0, kAckFull = 1, kAckUnsupported = 2, kAckBadBaseline = 3 };

// DELTA op
enum DeltaOpKind : uint8_t { kUpsert = 0, kRemove = 1 };

using Token = std::array<uint8_t, 16>;
using Digest = std::array<uint8_t, 32>;

struct Header {
    uint16_t schema = kSchema;
    uint16_t flags = 0;
    Token server_epoch{};
    Token client_nonce{};
    uint64_t sequence = 0;
};

struct Record {
    uint64_t wire_id = 0;
    uint64_t publisher_generation = 0;
    std::string ns, key;
    uint64_t revision = 0;
    uint32_t surfaces = 0;
    int16_t priority = 0;
    uint32_t rgba = 0xffffffffu;
    uint32_t icon = 0;
    uint8_t label_mode = 0;
    uint8_t followed = 0;
    uint16_t source_reason = 0;
    double x = 0, y = 0, z = 0;
    float height = 0, max_distance = 0;
    std::string text;
    bool operator==(const Record&) const = default;
};

struct Hello {
    uint64_t client_world_epoch = 0;
    Digest exe_sha256{};
    uint32_t capabilities = 0;
    uint16_t max_records = kSnapshotRecords;
    uint32_t max_body = kMaxBody;
    Digest catalog_digest{};
};

struct Welcome {
    uint32_t accepted = 0;
    uint16_t max_records = kSnapshotRecords;
    uint8_t need_catalog = 0;
    uint8_t reserved = 0;
};

struct CatalogRow {
    uint32_t id = 0;
    std::array<uint8_t, 16> image{};
    uint32_t flags = 0;
    std::string name;
};

struct Catalog {
    Digest digest{};
    std::vector<CatalogRow> rows;
};

struct SnapBegin {
    uint64_t snapshot_id = 0;
    uint16_t total_records = 0;
    uint16_t total_chunks = 0;
};

struct SnapChunk {
    uint64_t snapshot_id = 0;
    uint16_t index = 0;
    std::vector<Record> records;
    std::vector<uint8_t> record_bytes;  // decode: the exact bytes of the records, in order (the snapshot digest's input)
};

struct SnapEnd {
    uint64_t snapshot_id = 0;
    Digest digest{};
};

struct DeltaOp {
    uint8_t op = kUpsert;
    Record record;          // kUpsert
    uint64_t remove_id = 0; // kRemove
};

struct Pose {
    uint64_t wire_id = 0;
    uint64_t revision = 0;
    double x = 0, y = 0, z = 0;
    uint32_t source_reason = 0;
};

struct Ack {
    uint64_t applied = 0;
    uint8_t status = kAckAccepted;
    std::string reason;
};

struct Resync {
    uint64_t last_applied = 0;
    uint8_t reason = 0;
};

// RESYNC reasons (informational)
enum ResyncReason : uint8_t { kResyncGap = 1, kResyncStaging = 2, kResyncHeartbeat = 3, kResyncOverflow = 4, kResyncPose = 5 };

// ---- bytes ----

class Writer {
public:
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void f32(float v);
    void f64(double v);
    void raw(const void* p, size_t n);
    void str16(std::string_view s);  // the caller keeps s within its limit
};

class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool ok() const { return ok_; }
    size_t left() const { return ok_ ? n_ - at_ : 0; }
    size_t at() const { return at_; }
    bool done() const { return ok_ && at_ == n_; }
    // kiln.net's game transport pads every payload with zeros to a multiple of 4 bytes (net/game.cpp send: the game's
    // bit writers flush whole 32-bit words), so a message can arrive with up to 3 zero bytes after its end. Those are
    // the transport's: true when only such padding is left.
    bool only_padding() const;
    uint8_t u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    int16_t i16() { return static_cast<int16_t>(u16()); }
    float f32();
    double f64();
    void raw(void* out, size_t n);
    // A str16 of at most max bytes of strict UTF-8 without control characters; fails the reader otherwise.
    std::string str16(size_t max);
    void fail(const char* why) {
        if (ok_) why_ = why;
        ok_ = false;
    }
    const char* why() const { return why_; }

private:
    const uint8_t* need(size_t n);
    const uint8_t* p_;
    size_t n_;
    size_t at_ = 0;
    bool ok_ = true;
    const char* why_ = "";
};

// ---- messages ----
// encode_*: the whole body (header first). decode_*: the whole body; false and *why on anything malformed (the header is
// read and returned for every message that decodes; its schema and flags are checked by the caller: check_header).

std::vector<uint8_t> encode_hello(const Header& h, const Hello& m);
std::vector<uint8_t> encode_welcome(const Header& h, const Welcome& m);
std::vector<uint8_t> encode_catalog(const Header& h, const Catalog& m);
std::vector<uint8_t> encode_snap_begin(const Header& h, const SnapBegin& m);
std::vector<uint8_t> encode_snap_chunk(const Header& h, uint64_t snapshot_id, uint16_t index, const std::vector<Record>& records);
std::vector<uint8_t> encode_snap_end(const Header& h, const SnapEnd& m);
std::vector<uint8_t> encode_delta(const Header& h, const std::vector<DeltaOp>& ops);
std::vector<uint8_t> encode_pose(const Header& h, const std::vector<Pose>& poses);
std::vector<uint8_t> encode_ack(const Header& h, const Ack& m);
std::vector<uint8_t> encode_resync(const Header& h, const Resync& m);
std::vector<uint8_t> encode_heartbeat(const Header& h);

bool decode_header(const uint8_t* p, size_t n, Header* h, const char** why);
bool decode_hello(const uint8_t* p, size_t n, Header* h, Hello* m, const char** why);
bool decode_welcome(const uint8_t* p, size_t n, Header* h, Welcome* m, const char** why);
bool decode_catalog(const uint8_t* p, size_t n, Header* h, Catalog* m, const char** why);
bool decode_snap_begin(const uint8_t* p, size_t n, Header* h, SnapBegin* m, const char** why);
bool decode_snap_chunk(const uint8_t* p, size_t n, Header* h, SnapChunk* m, const char** why);
bool decode_snap_end(const uint8_t* p, size_t n, Header* h, SnapEnd* m, const char** why);
bool decode_delta(const uint8_t* p, size_t n, Header* h, std::vector<DeltaOp>* ops, const char** why);
bool decode_pose(const uint8_t* p, size_t n, Header* h, std::vector<Pose>* poses, const char** why);
bool decode_ack(const uint8_t* p, size_t n, Header* h, Ack* m, const char** why);
bool decode_resync(const uint8_t* p, size_t n, Header* h, Resync* m, const char** why);
bool decode_heartbeat(const uint8_t* p, size_t n, Header* h, const char** why);

// schema 1 and flags 0
bool check_header(const Header& h);

void encode_record(Writer& w, const Record& r);
bool decode_record(Reader& r, Record* out);
size_t record_size(const Record& r);

// A record's meaning (chapter 04 §4.4, §4.5 schema): nullptr when it is good, else the reason. Identifiers, a nonzero wire
// id and revision, the surface mask, the label mode (NATIVE_TOOLTIP needs MAP), followed 0/1, the source reason (READY,
// FOLLOW_MISSING, ANCHOR_ENDED, BAD_POSITION; a fixed record is READY), finite in-bounds coordinates, height, distance,
// and an icon only with MAP. Whether the icon is in a catalog is the receiver's question.
const char* check_record(const Record& r);

// The snapshot digest: SHA-256 of the concatenated encoded records, in (namespace, publisher generation, key) order.
Digest snapshot_digest(const std::vector<Record>& sorted);
// The order snapshots use (namespace ordinal, publisher generation, key ordinal).
bool record_order(const Record& a, const Record& b);

// Hex helpers (logs, the exe hash in HELLO)
std::string hex(const uint8_t* p, size_t n);
bool from_hex(std::string_view s, uint8_t* out, size_t n);

}  // namespace kmk::wire
#endif
