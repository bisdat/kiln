// SPDX-License-Identifier: MIT
// kiln.markers wire v1 (dossier chapter 04 §4.4): byte-exact golden frames for each of the eleven messages and for a
// RECORD, written out field by field from the chapter's table (not from the encoder), the round trips, and the strict
// reads: short and long bodies, counts and lengths over their limits, text that is not strict UTF-8 or has a control
// character, and the meaning checks of a record.
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "sha256.h"
#include "wire.h"

using namespace kmk;
using namespace kmk::wire;

namespace {

std::vector<uint8_t> unhex(const std::string& s) {
    std::vector<uint8_t> out;
    std::string digits;
    for (char c : s)
        if (c != ' ') digits += c;
    out.resize(digits.size() / 2);
    from_hex(digits, out.data(), out.size());
    return out;
}

std::string hexs(const std::vector<uint8_t>& b) { return hex(b.data(), b.size()); }

#define CHECK_HEX(bytes, expect)                                                                         \
    do {                                                                                                 \
        const std::string got_ = hexs(bytes);                                                            \
        std::string want_;                                                                               \
        for (char c_ : std::string(expect))                                                              \
            if (c_ != ' ') want_ += c_;                                                                  \
        if (got_ != want_) {                                                                             \
            std::printf("  golden mismatch (%s:%d)\n    got  %s\n    want %s\n", __FILE__, __LINE__, got_.c_str(), want_.c_str()); \
            ++kmk_test::g_failures;                                                                      \
            ++kmk_test::g_case_failures;                                                                 \
        }                                                                                                \
    } while (0)

// schema 1, flags 0, server epoch 00..0f, client nonce 10..1f, sequence 0x0102030405060708
const char* kHead = "0100 0000 000102030405060708090a0b0c0d0e0f 101112131415161718191a1b1c1d1e1f 0807060504030201";
// a HELLO's header: no server epoch, sequence 0
const char* kHelloHead = "0100 0000 00000000000000000000000000000000 101112131415161718191a1b1c1d1e1f 0000000000000000";

Header head() {
    Header h;
    for (int i = 0; i < 16; ++i) {
        h.server_epoch[i] = static_cast<uint8_t>(i);
        h.client_nonce[i] = static_cast<uint8_t>(0x10 + i);
    }
    h.sequence = 0x0102030405060708ull;
    return h;
}

// wire 5, generation 2, "ns", "k", revision 3, surfaces COMPASS|WORLD, priority -2, rgba 0x11223344, icon 0, label 0,
// followed, FOLLOW_MISSING, (1.5, -2, 100.25), height 2, max distance 0.5, "hi"
const char* kRecord =
    "0500000000000000 0200000000000000 0200 6e73 0100 6b 0300000000000000 06000000 feff 44332211 00000000 00 01 0600"
    " 000000000000f83f 00000000000000c0 0000000000105940 00000040 0000003f 0200 6869";

Record record() {
    Record r;
    r.wire_id = 5;
    r.publisher_generation = 2;
    r.ns = "ns";
    r.key = "k";
    r.revision = 3;
    r.surfaces = 6;
    r.priority = -2;
    r.rgba = 0x11223344u;
    r.icon = 0;
    r.label_mode = 0;
    r.followed = 1;
    r.source_reason = 6;
    r.x = 1.5;
    r.y = -2.0;
    r.z = 100.25;
    r.height = 2.0f;
    r.max_distance = 0.5f;
    r.text = "hi";
    return r;
}

void golden_frames() {
    kmk_test::begin();
    const Header h = head();
    Header hh;
    hh.client_nonce = h.client_nonce;

    // 0 HELLO
    Hello hello;
    hello.client_world_epoch = 7;
    hello.exe_sha256.fill(0xaa);
    hello.capabilities = 0x17;
    hello.max_records = 512;
    hello.max_body = 16384;
    hello.catalog_digest.fill(0xbb);
    const auto b0 = encode_hello(hh, hello);
    CHECK_HEX(b0, std::string(kHelloHead) + " 0700000000000000 " + std::string(64, 'a') + " 17000000 0002 00400000 " + std::string(64, 'b'));
    CHECK_EQ(b0.size(), size_t{44 + 8 + 32 + 4 + 2 + 4 + 32});
    Header rh;
    Hello rhello;
    const char* why = nullptr;
    CHECK(decode_hello(b0.data(), b0.size(), &rh, &rhello, &why));
    CHECK(rhello.client_world_epoch == 7 && rhello.capabilities == 0x17 && rhello.max_records == 512 && rhello.max_body == 16384);
    CHECK(rhello.exe_sha256 == hello.exe_sha256 && rhello.catalog_digest == hello.catalog_digest && rh.client_nonce == h.client_nonce);

    // 1 WELCOME
    Welcome w;
    w.accepted = 0x17;
    w.max_records = 512;
    w.need_catalog = 1;
    const auto b1 = encode_welcome(h, w);
    CHECK_HEX(b1, std::string(kHead) + " 17000000 0002 01 00");
    Welcome rw;
    CHECK(decode_welcome(b1.data(), b1.size(), &rh, &rw, &why) && rw.accepted == 0x17 && rw.need_catalog == 1 && rh.sequence == h.sequence);

    // 2 CATALOG (a synthetic row: no game data)
    Catalog cat;
    cat.digest.fill(0xcc);
    CatalogRow row;
    row.id = 0xffe5e444u;
    for (int i = 0; i < 16; ++i) row.image[i] = static_cast<uint8_t>(0x20 + i);
    row.flags = 25;
    row.name = "icon_x";
    cat.rows.push_back(row);
    const auto b2 = encode_catalog(h, cat);
    CHECK_HEX(b2, std::string(kHead) + " " + std::string(64, 'c') + " 0100 44e4e5ff 202122232425262728292a2b2c2d2e2f 19000000 0600 69636f6e5f78");
    Catalog rc;
    CHECK(decode_catalog(b2.data(), b2.size(), &rh, &rc, &why) && rc.rows.size() == 1 && rc.rows[0].id == row.id && rc.rows[0].name == "icon_x" &&
          rc.rows[0].image == row.image && rc.rows[0].flags == 25 && rc.digest == cat.digest);

    // 3 SNAP_BEGIN
    SnapBegin sb{9, 33, 2};
    const auto b3 = encode_snap_begin(h, sb);
    CHECK_HEX(b3, std::string(kHead) + " 0900000000000000 2100 0200");
    SnapBegin rsb;
    CHECK(decode_snap_begin(b3.data(), b3.size(), &rh, &rsb, &why) && rsb.snapshot_id == 9 && rsb.total_records == 33 && rsb.total_chunks == 2);

    // 4 SNAP_CHUNK
    const auto b4 = encode_snap_chunk(h, 9, 1, {record()});
    CHECK_HEX(b4, std::string(kHead) + " 0900000000000000 0100 0100 " + kRecord);
    SnapChunk rch;
    CHECK(decode_snap_chunk(b4.data(), b4.size(), &rh, &rch, &why) && rch.snapshot_id == 9 && rch.index == 1 && rch.records.size() == 1 &&
          rch.records[0] == record());
    CHECK_HEX(rch.record_bytes, kRecord);

    // 5 SNAP_END: the digest is SHA-256 over the records' bytes in order
    SnapEnd se;
    se.snapshot_id = 9;
    se.digest = snapshot_digest({record()});
    Sha256 sha;
    const auto rb = unhex(kRecord);
    sha.update(rb.data(), rb.size());
    CHECK(se.digest == sha.finish());
    const auto empty = snapshot_digest({});
    CHECK(hex(empty.data(), 32) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const auto b5 = encode_snap_end(h, se);
    CHECK_HEX(b5, std::string(kHead) + " 0900000000000000 " + hex(se.digest.data(), 32));
    SnapEnd rse;
    CHECK(decode_snap_end(b5.data(), b5.size(), &rh, &rse, &why) && rse.digest == se.digest && rse.snapshot_id == 9);

    // 6 DELTA: an upsert and a remove
    DeltaOp up;
    up.record = record();
    DeltaOp rm;
    rm.op = kRemove;
    rm.remove_id = 7;
    const auto b6 = encode_delta(h, {up, rm});
    CHECK_HEX(b6, std::string(kHead) + " 0200 00 " + kRecord + " 01 0700000000000000");
    std::vector<DeltaOp> rops;
    CHECK(decode_delta(b6.data(), b6.size(), &rh, &rops, &why) && rops.size() == 2 && rops[0].op == kUpsert && rops[0].record == record() &&
          rops[1].op == kRemove && rops[1].remove_id == 7);

    // 7 POSE
    Pose q{5, 3, 1.5, -2.0, 100.25, 6};
    const auto b7 = encode_pose(h, {q});
    CHECK_HEX(b7, std::string(kHead) + " 0100 0500000000000000 0300000000000000 000000000000f83f 00000000000000c0 0000000000105940 06000000");
    std::vector<Pose> rq;
    CHECK(decode_pose(b7.data(), b7.size(), &rh, &rq, &why) && rq.size() == 1 && rq[0].wire_id == 5 && rq[0].revision == 3 && rq[0].z == 100.25 &&
          rq[0].source_reason == 6);

    // 8 ACK
    Ack a;
    a.applied = 4;
    a.status = kAckFull;
    a.reason = "full";
    const auto b8 = encode_ack(h, a);
    CHECK_HEX(b8, std::string(kHead) + " 0400000000000000 01 0400 66756c6c");
    Ack ra;
    CHECK(decode_ack(b8.data(), b8.size(), &rh, &ra, &why) && ra.applied == 4 && ra.status == 1 && ra.reason == "full");

    // 9 RESYNC
    const auto b9 = encode_resync(h, Resync{4, 1});
    CHECK_HEX(b9, std::string(kHead) + " 0400000000000000 01");
    Resync rr;
    CHECK(decode_resync(b9.data(), b9.size(), &rh, &rr, &why) && rr.last_applied == 4 && rr.reason == 1);

    // 10 HEARTBEAT: the header only
    const auto b10 = encode_heartbeat(h);
    CHECK_HEX(b10, kHead);
    CHECK(decode_heartbeat(b10.data(), b10.size(), &rh, &why) && rh.sequence == h.sequence && rh.server_epoch == h.server_epoch);
    CHECK(check_header(rh));
    CHECK_EQ(b10.size(), kHeaderBytes);

    // directions
    for (uint32_t m : {kHello, kCatalog, kAck, kResync}) CHECK(to_server(m) && !to_client(m));
    for (uint32_t m : {kWelcome, kSnapBegin, kSnapChunk, kSnapEnd, kDelta, kPose, kHeartbeat}) CHECK(to_client(m) && !to_server(m));
    CHECK(!to_client(11) && !to_server(11));
    kmk_test::end("wire.golden_frames");
}

std::vector<uint8_t> cut(std::vector<uint8_t> b, int delta, uint8_t fill = 0xA5) {
    if (delta < 0) b.resize(b.size() - static_cast<size_t>(-delta));
    else b.insert(b.end(), static_cast<size_t>(delta), fill);
    return b;
}

// kiln.net's game transport pads every payload with zeros to a multiple of 4 (net/game.cpp send). A 31.10 server
// refused a same-build client's 126-byte HELLO that arrived as 128 bytes ("bytes after its end", every 5 s): every
// message decodes with that padding, and with nothing else after its end.
template <class Decode>
void padded(const std::vector<uint8_t>& b, Decode decode) {
    const size_t pad = (4 - b.size() % 4) % 4;
    auto p = cut(b, static_cast<int>(pad), 0);
    CHECK(decode(p.data(), p.size()));                       // the transport's padding: accepted
    if (pad) {
        auto nz = cut(b, static_cast<int>(pad), 0);
        nz.back() = 1;
        CHECK(!decode(nz.data(), nz.size()));                // a nonzero byte in it: refused
    }
    auto four = cut(b, static_cast<int>(pad + 4), 0);
    CHECK(!decode(four.data(), four.size()));                // more than the padding: refused
    if (b.size() % 4 != 3) {
        auto odd = cut(b, 1, 0);
        CHECK(!decode(odd.data(), odd.size()));              // a zero byte that leaves the length unaligned: refused
    }
}

void transport_padding() {
    kmk_test::begin();
    const Header h = head();
    Header rh;
    const char* why = nullptr;
    Hello hello;
    const auto hb = encode_hello(h, hello);
    CHECK_EQ(hb.size() % 4, size_t{2});  // the live case: 126 bytes, 128 on the wire
    padded(hb, [&](const uint8_t* p, size_t n) { Hello x; return decode_hello(p, n, &rh, &x, &why); });
    Welcome w;
    padded(encode_welcome(h, w), [&](const uint8_t* p, size_t n) { Welcome x; return decode_welcome(p, n, &rh, &x, &why); });
    Catalog c;
    padded(encode_catalog(h, c), [&](const uint8_t* p, size_t n) { Catalog x; return decode_catalog(p, n, &rh, &x, &why); });
    SnapBegin sb;
    sb.snapshot_id = 9;
    sb.total_records = 1;
    sb.total_chunks = 1;
    padded(encode_snap_begin(h, sb), [&](const uint8_t* p, size_t n) { SnapBegin x; return decode_snap_begin(p, n, &rh, &x, &why); });
    const auto chunk = encode_snap_chunk(h, 9, 0, {record()});
    padded(chunk, [&](const uint8_t* p, size_t n) { SnapChunk x; return decode_snap_chunk(p, n, &rh, &x, &why); });
    {   // the snapshot digest covers the records' bytes only, never the padding
        SnapChunk a, b;
        auto p = cut(chunk, static_cast<int>((4 - chunk.size() % 4) % 4), 0);
        CHECK(decode_snap_chunk(chunk.data(), chunk.size(), &rh, &a, &why) && decode_snap_chunk(p.data(), p.size(), &rh, &b, &why));
        CHECK(a.record_bytes == b.record_bytes);
    }
    SnapEnd se;
    se.snapshot_id = 9;
    padded(encode_snap_end(h, se), [&](const uint8_t* p, size_t n) { SnapEnd x; return decode_snap_end(p, n, &rh, &x, &why); });
    DeltaOp op;
    op.record = record();
    padded(encode_delta(h, {op}), [&](const uint8_t* p, size_t n) { std::vector<DeltaOp> x; return decode_delta(p, n, &rh, &x, &why); });
    Pose q;
    q.wire_id = 5;
    q.revision = 3;
    padded(encode_pose(h, {q}), [&](const uint8_t* p, size_t n) { std::vector<Pose> x; return decode_pose(p, n, &rh, &x, &why); });
    Ack a;
    a.applied = 4;
    a.status = kAckFull;
    a.reason = "full";
    padded(encode_ack(h, a), [&](const uint8_t* p, size_t n) { Ack x; return decode_ack(p, n, &rh, &x, &why); });
    Resync rs;
    rs.last_applied = 4;
    rs.reason = kResyncGap;
    padded(encode_resync(h, rs), [&](const uint8_t* p, size_t n) { Resync x; return decode_resync(p, n, &rh, &x, &why); });
    padded(encode_heartbeat(h), [&](const uint8_t* p, size_t n) { return decode_heartbeat(p, n, &rh, &why); });
    kmk_test::end("wire.transport_padding");
}

void strict_reads() {
    kmk_test::begin();
    const Header h = head();
    Header rh;
    const char* why = nullptr;
    // every message: one byte short and one byte long fail
    Hello hello;
    auto b = encode_hello(h, hello);
    Hello x0;
    CHECK(!decode_hello(cut(b, -1).data(), b.size() - 1, &rh, &x0, &why));
    CHECK(!decode_hello(cut(b, 1).data(), b.size() + 1, &rh, &x0, &why));
    b = encode_heartbeat(h);
    CHECK(!decode_heartbeat(cut(b, -1).data(), b.size() - 1, &rh, &why));
    CHECK(!decode_heartbeat(cut(b, 1).data(), b.size() + 1, &rh, &why));
    b = encode_snap_chunk(h, 1, 0, {record()});
    SnapChunk sc;
    CHECK(!decode_snap_chunk(cut(b, -1).data(), b.size() - 1, &rh, &sc, &why));
    CHECK(!decode_snap_chunk(cut(b, 1).data(), b.size() + 1, &rh, &sc, &why));
    CHECK(!decode_header(b.data(), 43, &rh, &why));
    // counts over their limits
    std::vector<Record> many(33, record());
    b = encode_snap_chunk(h, 1, 0, many);
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = encode_snap_chunk(h, 1, 16, {record()});
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = encode_snap_chunk(h, 1, 0, {});
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    std::vector<DeltaOp> ops(33);
    for (auto& o : ops) o.record = record();
    b = encode_delta(h, ops);
    std::vector<DeltaOp> rops;
    CHECK(!decode_delta(b.data(), b.size(), &rh, &rops, &why));
    b = encode_delta(h, {});
    CHECK(!decode_delta(b.data(), b.size(), &rh, &rops, &why));
    DeltaOp odd;
    odd.op = 2;
    b = encode_delta(h, {odd});  // op 2 then a remove id's bytes
    CHECK(!decode_delta(b.data(), b.size(), &rh, &rops, &why));
    std::vector<Pose> poses(65);
    b = encode_pose(h, poses);
    std::vector<Pose> rq;
    CHECK(!decode_pose(b.data(), b.size(), &rh, &rq, &why));
    Catalog cat;
    cat.rows.resize(129);
    for (auto& r : cat.rows) r.name = "x";
    b = encode_catalog(h, cat);
    Catalog rc;
    CHECK(!decode_catalog(b.data(), b.size(), &rh, &rc, &why));
    cat.rows.resize(1);
    cat.rows[0].name = "";
    b = encode_catalog(h, cat);
    CHECK(!decode_catalog(b.data(), b.size(), &rh, &rc, &why));
    SnapBegin sb{1, 513, 17};
    b = encode_snap_begin(h, sb);
    SnapBegin rsb;
    CHECK(!decode_snap_begin(b.data(), b.size(), &rh, &rsb, &why));
    sb = SnapBegin{1, 33, 1};
    b = encode_snap_begin(h, sb);
    CHECK(!decode_snap_begin(b.data(), b.size(), &rh, &rsb, &why));
    sb = SnapBegin{1, 0, 0};
    b = encode_snap_begin(h, sb);
    CHECK(decode_snap_begin(b.data(), b.size(), &rh, &rsb, &why));
    Ack a;
    a.status = 4;
    b = encode_ack(h, a);
    Ack ra;
    CHECK(!decode_ack(b.data(), b.size(), &rh, &ra, &why));
    Welcome w;
    w.need_catalog = 2;
    b = encode_welcome(h, w);
    Welcome rw;
    CHECK(!decode_welcome(b.data(), b.size(), &rh, &rw, &why));
    // texts: too long, not UTF-8, a control character, an overlong form
    auto rec_with = [&](auto f) {
        Record r = record();
        f(r);
        return encode_snap_chunk(h, 1, 0, {r});
    };
    b = rec_with([](Record& r) { r.ns = std::string(65, 'a'); });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.key = std::string(65, 'a'); });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = std::string(129, 'a'); });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = std::string("a\xff", 2); });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = "two\nlines"; });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = std::string("\xc0\xaf", 2); });
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = std::string("\xc2\x85", 2); });  // U+0085, a C1 control
    CHECK(!decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    b = rec_with([](Record& r) { r.text = "Raid entrance \xe2\x9a\x94"; });  // UTF-8 text is fine
    CHECK(decode_snap_chunk(b.data(), b.size(), &rh, &sc, &why));
    // a body over 16384 bytes
    std::vector<uint8_t> big(kMaxBody + 1, 0);
    std::memcpy(big.data(), encode_heartbeat(h).data(), kHeaderBytes);
    CHECK(!decode_heartbeat(big.data(), big.size(), &rh, &why));
    // the largest record is 336 bytes, so 32 of them fit one chunk under 16384
    Record largest = record();
    largest.ns = std::string(64, 'n');
    largest.key = std::string(64, 'k');
    largest.text = std::string(128, 't');
    Writer wr;
    encode_record(wr, largest);
    CHECK_EQ(wr.bytes.size(), size_t{336});
    CHECK_EQ(record_size(largest), size_t{336});
    CHECK(wr.bytes.size() <= kMaxRecordBytes);
    const auto full = encode_snap_chunk(h, 1, 0, std::vector<Record>(32, largest));
    CHECK(full.size() <= kMaxBody);
    CHECK(decode_snap_chunk(full.data(), full.size(), &rh, &sc, &why) && sc.records.size() == 32);
    std::vector<DeltaOp> dops(32);
    for (auto& o : dops) o.record = largest;
    CHECK(encode_delta(h, dops).size() <= kMaxBody);
    // schema and flags
    Header other = h;
    other.schema = 2;
    CHECK(!check_header(other));
    other = h;
    other.flags = 1;
    CHECK(!check_header(other));
    kmk_test::end("wire.strict_reads");
}

void record_meaning() {
    kmk_test::begin();
    CHECK(check_record(record()) == nullptr);
    auto bad = [](auto f) {
        Record r = record();
        f(r);
        return check_record(r) != nullptr;
    };
    CHECK(bad([](Record& r) { r.wire_id = 0; }));
    CHECK(bad([](Record& r) { r.revision = 0; }));
    CHECK(bad([](Record& r) { r.ns = "has space"; }));
    CHECK(bad([](Record& r) { r.key = ""; }));
    CHECK(bad([](Record& r) { r.surfaces = 0; }));
    CHECK(bad([](Record& r) { r.surfaces = 8; }));
    CHECK(bad([](Record& r) { r.label_mode = 1; }));  // a tooltip needs MAP
    CHECK(bad([](Record& r) { r.label_mode = 2; }));
    CHECK(bad([](Record& r) { r.followed = 2; }));
    CHECK(bad([](Record& r) { r.source_reason = 3; }));
    CHECK(bad([](Record& r) {
        r.followed = 0;
        r.source_reason = 6;  // a fixed record is READY
    }));
    CHECK(bad([](Record& r) { r.x = std::nan(""); }));
    CHECK(bad([](Record& r) { r.z = 12000.5; }));
    CHECK(bad([](Record& r) { r.y = 5000.5; }));
    CHECK(bad([](Record& r) { r.height = 128.5f; }));
    CHECK(bad([](Record& r) { r.max_distance = -1; }));
    CHECK(bad([](Record& r) { r.max_distance = INFINITY; }));
    CHECK(bad([](Record& r) { r.icon = 7; }));  // an icon without MAP
    CHECK(bad([](Record& r) { r.surfaces = 1; }));  // MAP without an icon
    Record map = record();
    map.surfaces = 1;
    map.icon = 0xffe5e444u;
    map.label_mode = 1;
    CHECK(check_record(map) == nullptr);
    for (uint16_t reason : {0, 6, 7, 11}) {
        Record r = record();
        r.source_reason = reason;
        CHECK(check_record(r) == nullptr);
    }
    // the snapshot order: namespace, then publisher generation, then key (ordinal)
    Record a = record(), b = record();
    a.ns = "B";
    b.ns = "a";
    CHECK(record_order(a, b));
    a.ns = b.ns = "x";
    a.publisher_generation = 10;
    b.publisher_generation = 9;
    CHECK(record_order(b, a));
    a.publisher_generation = b.publisher_generation;
    a.key = "k1";
    b.key = "k2";
    CHECK(record_order(a, b) && !record_order(b, a) && !record_order(a, a));
    kmk_test::end("wire.record_meaning");
}

}  // namespace

int main() {
    golden_frames();
    strict_reads();
    transport_padding();
    record_meaning();
    return kmk_test::finish("wire_test");
}
