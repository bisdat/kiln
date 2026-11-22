// SPDX-License-Identifier: MIT
// kiln.repl.net's sidecar tracking (track.h) without the game (built with clang-cl, run under plain Wine by tests/run.sh):
// the events of a send-callback batch of a client channel (creates, both lists of a destroy list, nothing for messages
// the session refused, nothing on the prediction channel), one record per batch through a ring, GAP after records a full
// ring dropped, the client's per-message records, and the table of tracks (an owner's revoke removes its tracks).
#include <cstdio>
#include <cstring>
#include <vector>

#include "../track.h"

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(x) check((x), #x, __LINE__)

std::vector<uint8_t> entity_msg(uint32_t kind, uint32_t tick, uint32_t seq, uint32_t entity) {
    std::vector<uint8_t> m(12);
    const uint32_t w0 = (tick << 3) | kind;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    std::memcpy(m.data() + 8, &entity, 4);
    if (kind == repl::kKindCreate) {
        m.resize(12 + 4 + 16 + 1, 0);
    } else {
        const uint32_t base = 0x80000000u | (tick - 4);
        m.resize(16);
        std::memcpy(m.data() + 12, &base, 4);
    }
    m.push_back(3);
    m.push_back(0);
    m.resize(m.size() + 3, 0x5A);
    return m;
}
std::vector<uint8_t> destroy_msg(uint32_t tick, uint32_t seq, std::vector<uint32_t> a, std::vector<uint32_t> b) {
    std::vector<uint8_t> m(10);
    const uint32_t w0 = (tick << 3) | repl::kKindDestroyList;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    m[8] = static_cast<uint8_t>(a.size());
    m[9] = static_cast<uint8_t>(b.size());
    for (auto* l : {&a, &b})
        for (uint32_t id : *l) {
            m.resize(m.size() + 4);
            std::memcpy(m.data() + m.size() - 4, &id, 4);
        }
    return m;
}

struct Ev {
    uint8_t kind;
    uint32_t entity, tick;
    bool operator==(const Ev& o) const { return kind == o.kind && entity == o.entity && tick == o.tick; }
};
std::vector<Ev> drain(repl::Ring& r) {
    std::vector<Ev> out;
    repl::track::drain(r, [&](uint8_t k, uint32_t e, uint32_t t) { out.push_back(Ev{k, e, t}); });
    return out;
}

// A batch as the send callback sees it: create, update, destroy list, handshake, create; the session took "sent" of them.
struct TestBatch {
    std::vector<std::vector<uint8_t>> bodies;
    repl::MsgEntry msgs[5] = {};
    repl::Batch b{};
    TestBatch(uint32_t sent, uint8_t pred) {
        bodies = {entity_msg(repl::kKindCreate, 100, 1, 2542), entity_msg(repl::kKindUpdate, 101, 2, 2542),
                  destroy_msg(102, 3, {2542}, {7, 8}), std::vector<uint8_t>(6, 0), entity_msg(repl::kKindCreate, 103, 5, 99)};
        const uint8_t types[5] = {repl::kMsgEntity, repl::kMsgEntity, repl::kMsgList, repl::kMsgHandshake, repl::kMsgEntity};
        for (int i = 0; i < 5; ++i) {
            msgs[i].type = types[i];
            msgs[i].seq = static_cast<uint32_t>(i + 1);
            msgs[i].data = bodies[i].data();
            msgs[i].size = bodies[i].size();
        }
        b.msgs = msgs;
        b.count = 5;
        b.machine = 3;
        b.pred = pred;
        b.sent = sent;
    }
};

void batches() {
    using repl::track::Rec;
    // every message accepted: the two creates, list A as DESTROYED, list B as LEFT; the update and the handshake give none
    TestBatch all(5, 0);
    Rec out[16] = {};
    const uint32_t n = repl::track::scan_batch(all.b.msgs, all.b.count, all.b.sent, out, 16);
    CHECK(n == 5);
    CHECK(out[0].entity == 2542 && out[0].tick == 100 && out[0].kind == KILN_REPL_TRACK_CREATED);
    CHECK(out[1].entity == 2542 && out[1].tick == 102 && out[1].kind == KILN_REPL_TRACK_DESTROYED);
    CHECK(out[2].entity == 7 && out[2].kind == KILN_REPL_TRACK_LEFT && out[3].entity == 8 && out[3].kind == KILN_REPL_TRACK_LEFT);
    CHECK(out[4].entity == 99 && out[4].tick == 103 && out[4].kind == KILN_REPL_TRACK_CREATED);
    // the session took the first 3: the create of 99 was refused, so it is no event
    TestBatch three(3, 0);
    CHECK(repl::track::scan_batch(three.b.msgs, three.b.count, three.b.sent, nullptr, 0) == 4);
    // a stale sent above the count never reads past the batch
    TestBatch stale(9, 0);
    stale.b.count = 2;
    CHECK(repl::track::scan_batch(stale.b.msgs, stale.b.count, stale.b.sent, nullptr, 0) == 1);
    // a cap smaller than the events: counted, written only up to the cap
    Rec two[2] = {};
    CHECK(repl::track::scan_batch(all.b.msgs, 5, 5, two, 2) == 5 && two[1].kind == KILN_REPL_TRACK_DESTROYED);

    // through a ring: one record per batch; the prediction channel gives nothing
    repl::Ring r;
    CHECK(r.init(4096, 64));
    CHECK(repl::track::push_batch(r, &three.b));
    TestBatch pred(5, 1);
    CHECK(!repl::track::push_batch(r, &pred.b));
    const std::vector<Ev> got = drain(r);
    CHECK(got.size() == 4);
    CHECK(got.size() == 4 && got[0] == (Ev{KILN_REPL_TRACK_CREATED, 2542, 100}) && got[1] == (Ev{KILN_REPL_TRACK_DESTROYED, 2542, 102}) &&
          got[2] == (Ev{KILN_REPL_TRACK_LEFT, 7, 102}) && got[3] == (Ev{KILN_REPL_TRACK_LEFT, 8, 102}));
    CHECK(r.written() == 1);
    // a batch with no create and no list: no record at all
    TestBatch none(5, 0);
    none.b.msgs = none.msgs + 1;  // the update only
    none.b.count = 1;
    none.b.sent = 1;
    CHECK(!repl::track::push_batch(r, &none.b) && r.written() == 1);
}

void gaps() {
    // a ring with room for one record: the second batch is dropped, the next one that fits comes with GAP first
    repl::Ring r;
    CHECK(r.init(128, 64));  // a 5-event record is 8 + 60 = 68 -> 72 bytes: one fits, a second does not
    TestBatch all(5, 0);
    CHECK(repl::track::push_batch(r, &all.b));
    CHECK(!repl::track::push_batch(r, &all.b) && r.dropped() == 1);
    std::vector<Ev> got = drain(r);
    CHECK(got.size() == 5 && got[0].kind == KILN_REPL_TRACK_CREATED);  // the first record: no gap before it
    CHECK(repl::track::push_batch(r, &all.b));
    got = drain(r);
    CHECK(got.size() == 6 && got[0] == (Ev{KILN_REPL_TRACK_GAP, 0, 0}) && got[1] == (Ev{KILN_REPL_TRACK_CREATED, 2542, 100}));
    CHECK(repl::track::push_batch(r, &all.b));
    got = drain(r);
    CHECK(got.size() == 5 && got[0].kind != KILN_REPL_TRACK_GAP);  // the gap was said once
}

void client_messages() {
    repl::Ring r;
    CHECK(r.init(1024, 16));
    const auto create = entity_msg(repl::kKindCreate, 500, 1, 3000);
    const auto update = entity_msg(repl::kKindUpdate, 501, 2, 3000);
    const auto list = destroy_msg(502, 3, {3000, 3001}, {3002});
    CHECK(repl::track::push_message(r, create.data(), create.size()));
    CHECK(!repl::track::push_message(r, update.data(), update.size()));
    CHECK(repl::track::push_message(r, list.data(), list.size()));
    CHECK(!repl::track::push_message(r, create.data(), 20));  // cut short: no event
    const std::vector<Ev> got = drain(r);
    CHECK(got.size() == 4 && got[0] == (Ev{KILN_REPL_TRACK_CREATED, 3000, 500}) && got[1] == (Ev{KILN_REPL_TRACK_DESTROYED, 3000, 502}) &&
          got[2] == (Ev{KILN_REPL_TRACK_DESTROYED, 3001, 502}) && got[3] == (Ev{KILN_REPL_TRACK_LEFT, 3002, 502}));
}

int g_calls[3] = {};
void KILN_CALL fn_a(const KilnReplTrackEvent*, uint32_t, void*) { ++g_calls[0]; }
void KILN_CALL fn_b(const KilnReplTrackEvent*, uint32_t, void*) { ++g_calls[1]; }
void KILN_CALL fn_c(const KilnReplTrackEvent*, uint32_t, void*) { ++g_calls[2]; }

void table() {
    repl::track::Table<3> t;
    const uint64_t tag = 0x544bull << 48;
    const uint64_t a = t.add(7, KILN_REPL_SEND, &fn_a, nullptr, tag);
    const uint64_t b = t.add(8, KILN_REPL_SEND, &fn_b, nullptr, tag);
    const uint64_t c = t.add(7, KILN_REPL_RECEIVE, &fn_c, nullptr, tag);
    CHECK(a && b && c && a != b && (a >> 48) == 0x544b);
    CHECK(t.add(9, KILN_REPL_SEND, &fn_a, nullptr, tag) == 0);  // full
    // owner 7 goes (any thread): both its tracks stop at once, the other owner's stays
    CHECK(t.retire(7) == (KILN_REPL_SEND | KILN_REPL_RECEIVE));
    CHECK(t.retire(7) == 0);  // said once
    t.each(KILN_REPL_SEND, [](repl::track::Slot& s) { s.fn(nullptr, 0, nullptr); });
    t.each(KILN_REPL_RECEIVE, [](repl::track::Slot& s) { s.fn(nullptr, 0, nullptr); });
    CHECK(g_calls[0] == 0 && g_calls[1] == 1 && g_calls[2] == 0);
    // the threads free the slots between deliveries
    CHECK(t.apply(KILN_REPL_SEND) == 1 && t.apply(KILN_REPL_RECEIVE) == 0);
    const uint64_t d = t.add(9, KILN_REPL_SEND, &fn_a, nullptr, tag);
    CHECK(d && d != a);
    // remove: by handle, once; the direction it had
    CHECK(t.remove(b) == KILN_REPL_SEND && t.remove(b) == 0 && t.remove(0) == 0 && t.remove(a) == 0);
    CHECK(t.apply(KILN_REPL_SEND) == 1);
}

}  // namespace

int main() {
    batches();
    gaps();
    client_messages();
    table();
    std::printf("repl track tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
