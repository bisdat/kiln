// SPDX-License-Identifier: MIT
// kiln.repl host tests (no game; built with clang-cl, run under plain Wine by tests/run.sh, so the guard's SEH is the
// real thing): the ABI layout of kiln_repl.h; the fault rule of guard.h, the one shape of every observer detour (a
// forced access violation in an observer leaves the original's result intact, the original runs exactly once, the
// hook is disabled and says so once); the record rings (rings.h: overflow counts `dropped`, the next record carries
// gap = 1, wrap, the record limit, a second producer); what a send-callback batch counts for (wire.h).
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../guard.h"
#include "../layout.h"
#include "../rings.h"
#include "../wire.h"
#include "services/kiln_repl.h"

namespace {

int g_checks = 0, g_failed = 0;
void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("FAIL %s\n", what);
    }
}
#define CHECK(x) check((x), #x)

// ---- ABI ----
void abi() {
    static_assert(sizeof(KilnReplPeerStats) == 16 + 13 * 8);
    static_assert(offsetof(KilnReplPeerStats, peer) == 4 && offsetof(KilnReplPeerStats, channel) == 8 &&
                  offsetof(KilnReplPeerStats, flags) == 9 && offsetof(KilnReplPeerStats, frames) == 16 &&
                  offsetof(KilnReplPeerStats, refused) == 40 && offsetof(KilnReplPeerStats, unchanged) == 80 &&
                  offsetof(KilnReplPeerStats, dropped) == 112);
    static_assert(sizeof(KilnReplComponent) == 24 && offsetof(KilnReplComponent, hash) == 4 &&
                  offsetof(KilnReplComponent, custom_codec) == 8 && offsetof(KilnReplComponent, data) == 16);
    static_assert(sizeof(KilnReplWatchEvent) == 48 && offsetof(KilnReplWatchEvent, direction) == 8 &&
                  offsetof(KilnReplWatchEvent, gap) == 11 && offsetof(KilnReplWatchEvent, entity) == 12 &&
                  offsetof(KilnReplWatchEvent, payload_bytes) == 28 && offsetof(KilnReplWatchEvent, count) == 32 &&
                  offsetof(KilnReplWatchEvent, components) == 40);
    static_assert(sizeof(KilnReplComponentInfo) == 16 && offsetof(KilnReplComponentInfo, index) == 8 &&
                  offsetof(KilnReplComponentInfo, data_size) == 10 && offsetof(KilnReplComponentInfo, flags) == 12);
    // v1's slots in order; a table only grows at its end
    static_assert(offsetof(KilnReplApi, capabilities) == 8 && offsetof(KilnReplApi, stats_enable) == 16 &&
                  offsetof(KilnReplApi, peer_stats) == 24 && offsetof(KilnReplApi, watch) == 32 &&
                  offsetof(KilnReplApi, remove) == 40 && offsetof(KilnReplApi, component_info) == 48 &&
                  offsetof(KilnReplApi, capture) == 56);
    static_assert(KILN_REPL_CAP_OBSERVE == 1 && KILN_REPL_CAP_CLIENT == 2 && KILN_REPL_CAP_LOOPBACK == 4 &&
                  KILN_REPL_CAP_FILTER == 8 && KILN_REPL_CAP_TOUCH == 16 && KILN_REPL_CAP_MESSAGES == 32 &&
                  KILN_REPL_CAP_SCHEMA == 64);
    // the game's structures the hooks read
    static_assert(sizeof(repl::Item12) == 12 && sizeof(repl::MsgEntry) == 32 && sizeof(repl::Batch) == 24 &&
                  sizeof(repl::Holder) == 48 && offsetof(repl::WriterOut, skipped) == 16);
    CHECK(sizeof(KilnReplApi) >= 64);
    // dev.28's lab slots, then dev.31's at the end
    static_assert(offsetof(KilnReplApi, lab_add_fakes) == 64 && offsetof(KilnReplApi, lab_decoded) == 96 &&
                  offsetof(KilnReplApi, lab_payload) == 104 && offsetof(KilnReplApi, lab_hook_cost) == 112 &&
                  sizeof(KilnReplApi) == 120);
    static_assert(sizeof(KilnReplHookCost) == 8 + 17 * 8 && offsetof(KilnReplHookCost, callback_calls) == 48 &&
                  offsetof(KilnReplHookCost, allocations) == 128);
    static_assert(offsetof(KilnReplWatchEvent, reason) == 36);  // dev.28's pad2: same place
    static_assert(KILN_REPL_KIND_CREATE == 2 && KILN_REPL_KIND_UPDATE == 3 && KILN_REPL_KIND_DESTROYED == 4 &&
                  KILN_REPL_KIND_ENDED == 5 && KILN_REPL_PEER_LOCAL == 4);
}

// ---- the fault rule ----
int g_original_calls = 0;
int g_before_runs = 0, g_after_runs = 0;
bool g_fault_before = false, g_fault_after = false;

// A stand-in for a game function: a value the caller must get back unchanged.
__declspec(noinline) int64_t original(int64_t a, int64_t b) {
    ++g_original_calls;
    return a * 1000 + b;
}
__declspec(noinline) void original_void(int64_t* out) {
    ++g_original_calls;
    *out += 1;
}

struct Call {
    int64_t a, b;
    int64_t result = 0;
    int64_t seen_result = -1;
};

void fault_now() {
    volatile int* p = nullptr;
    *p = 1;  // a real access violation, as an observer reading a stale pointer would raise
}
void before(void* p) {
    ++g_before_runs;
    if (g_fault_before) fault_now();
    (void)p;
}
void after(void* p) {
    ++g_after_runs;
    auto* c = static_cast<Call*>(p);
    c->seen_result = c->result;
    if (g_fault_after) fault_now();
}

repl::Guard g_guard{"test-hook"};
int64_t detoured(int64_t a, int64_t b) {
    Call c{a, b};
    return repl::detour(g_guard, &c, &before, &after, &original, a, b);
}

std::vector<std::string> g_lines;
int flush(repl::Guard& g) {
    repl::Guard* list[] = {&g};
    return repl::flush_faults(list, 1, [](uintptr_t w, char* out, size_t cap) { std::snprintf(out, cap, "%s", w > 1 ? "addr" : "?"); },
                              [](const char* line) { g_lines.emplace_back(line); });
}

void fault_rule() {
    // a call without a fault: both observers run, the original once, the result through
    CHECK(detoured(2, 3) == 2003);
    CHECK(g_original_calls == 1 && g_before_runs == 1 && g_after_runs == 1);
    CHECK(!g_guard.disabled.load() && g_guard.faults.load() == 0);
    CHECK(flush(g_guard) == 0 && g_lines.empty());

    // a fault in the observer before the original: the original still runs once, its result is returned untouched
    g_fault_before = true;
    CHECK(detoured(7, 9) == 7009);
    CHECK(g_original_calls == 2);
    CHECK(g_guard.disabled.load() && g_guard.faults.load() == 1 && g_guard.where.load() > 1);
    CHECK(g_after_runs == 1);  // disabled: the after observer does not run in the same call either

    // disabled: pass-through, no observer runs, the original once per call
    g_fault_before = false;
    CHECK(detoured(4, 5) == 4005);
    CHECK(g_original_calls == 3 && g_before_runs == 2 && g_after_runs == 1);

    // one log line per hook per session
    CHECK(flush(g_guard) == 1 && g_lines.size() == 1);
    CHECK(!g_lines.empty() && g_lines[0] == "repl: observer test-hook faulted at addr; disabled");
    CHECK(flush(g_guard) == 0 && g_lines.size() == 1);

    // a fault after the original: the result is the original's, the after observer saw it
    repl::Guard g2{"after-hook"};
    g_fault_after = true;
    Call c{11, 12};
    const int64_t r = repl::detour(g2, &c, nullptr, &after, &original, int64_t{11}, int64_t{12});
    CHECK(r == 11012 && c.result == 11012 && c.seen_result == 11012);
    CHECK(g_original_calls == 4 && g2.disabled.load() && g2.faults.load() == 1);
    g_fault_after = false;

    // a void original: runs once, faulting observer or not
    repl::Guard g3{"void-hook"};
    int64_t v = 0;
    g_fault_before = true;
    struct VoidCall {
    } vc;
    repl::detour(g3, &vc, &before, nullptr, &original_void, &v);
    repl::detour(g3, &vc, &before, nullptr, &original_void, &v);
    g_fault_before = false;
    CHECK(v == 2 && g_original_calls == 6 && g3.disabled.load() && g3.faults.load() == 1);

    // guards fault independently
    CHECK(!repl::Guard{"fresh"}.disabled.load());
}

// ---- the record rings ----
uint32_t drain_all(repl::Ring& r, std::vector<std::pair<uint32_t, uint8_t>>* got) {
    return r.drain([&](const repl::RecordHeader& h, const uint8_t* p) {
        uint32_t v = 0;
        if (h.len >= 4) std::memcpy(&v, p, 4);
        got->emplace_back(v, h.gap);
    });
}

void rings() {
    repl::Ring r;
    CHECK(!r.ready() && r.reserve(1, 4) == nullptr);  // no buffer: nothing
    CHECK(r.init(256, 1000) && r.capacity() == 256);
    // 24-byte records (8 header + 16 payload): 10 fit in 256 bytes, the 11th is dropped and counted
    uint8_t payload[16] = {};
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < 12; ++i) {
        std::memcpy(payload, &i, 4);
        pushed += r.push(7, payload, sizeof payload);
    }
    CHECK(pushed == 10 && r.dropped() == 2 && r.written() == 10);
    std::vector<std::pair<uint32_t, uint8_t>> got;
    CHECK(drain_all(r, &got) == 10);
    CHECK(got.size() == 10 && got[0].first == 0 && got[9].first == 9);
    bool no_gap = true;
    for (auto& g : got) no_gap = no_gap && g.second == 0;
    CHECK(no_gap);  // records written before the drop carry no gap
    // the next record that fits carries gap = 1, the one after it not
    uint32_t v = 100;
    std::memcpy(payload, &v, 4);
    CHECK(r.push(7, payload, sizeof payload));
    v = 101;
    std::memcpy(payload, &v, 4);
    CHECK(r.push(7, payload, sizeof payload));
    got.clear();
    CHECK(drain_all(r, &got) == 2 && got[0] == std::make_pair(100u, uint8_t{1}) && got[1] == std::make_pair(101u, uint8_t{0}));

    // 30 records through the ring one at a time cross its end several times and come back in order
    got.clear();
    for (uint32_t i = 0; i < 30; ++i) {
        std::memcpy(payload, &i, 4);
        CHECK(r.push(7, payload, sizeof payload));
        CHECK(drain_all(r, &got) == 1);
    }
    bool in_order = got.size() == 30;
    for (uint32_t i = 0; i < got.size(); ++i) in_order = in_order && got[i].first == i && got[i].second == 0;
    CHECK(in_order);

    // a record that does not fit before the end leaves a wrap marker and starts at 0
    repl::Ring w;
    CHECK(w.init(64, 100));
    uint8_t big[40] = {};
    big[0] = 1;
    CHECK(w.push(1, big, 32));       // 40 bytes at 0
    got.clear();
    CHECK(drain_all(w, &got) == 1);  // read: the space is free again, the write position is 40
    big[0] = 2;
    CHECK(w.push(1, big, 24));       // 32 bytes: 24 left before the end -> marker at 40, record at 0
    got.clear();
    CHECK(drain_all(w, &got) == 1 && got[0].first == 2 && w.dropped() == 0);

    // the record limit: at most `records` unread records, whatever the bytes
    repl::Ring n;
    CHECK(n.init(4096, 3));
    for (int i = 0; i < 5; ++i) n.push(1, payload, 4);
    CHECK(n.written() == 3 && n.dropped() == 2);
    got.clear();
    CHECK(drain_all(n, &got) == 3 && got[0].second == 0);

    // reserve / commit shorter / abort
    repl::Ring c;
    CHECK(c.init(256, 10));
    uint8_t* p = c.reserve(3, 64);
    CHECK(p != nullptr && c.reserved());
    // a second producer while the reservation is open: dropped, never written into the open one
    CHECK(c.reserve(3, 8) == nullptr && c.dropped() == 1);
    v = 55;
    std::memcpy(p, &v, 4);
    c.commit(4);  // only 4 of the 64 bytes
    CHECK(!c.reserved());
    p = c.reserve(3, 16);
    CHECK(p != nullptr);
    c.abort();    // nothing published
    got.clear();
    CHECK(drain_all(c, &got) == 1 && got[0].first == 55 && got[0].second == 1);  // after the second producer's drop
    // discard drops what is unread
    c.push(3, payload, 4);
    c.discard();
    got.clear();
    CHECK(drain_all(c, &got) == 0);
    // a record larger than the ring is dropped
    CHECK(c.reserve(1, 1000) == nullptr && c.dropped() == 2);
    c.clear_counters();
    CHECK(c.dropped() == 0);
}

// ---- what a batch counts for ----
std::vector<uint8_t> entity_msg(uint32_t kind, uint32_t tick, uint32_t seq, uint32_t entity, uint16_t payload) {
    std::vector<uint8_t> m(12);
    const uint32_t w0 = (tick << 3) | kind;
    std::memcpy(m.data(), &w0, 4);
    std::memcpy(m.data() + 4, &seq, 4);
    std::memcpy(m.data() + 8, &entity, 4);
    if (kind == repl::kKindCreate) {
        m.resize(12 + 4 + 16 + 1, 0);
        m[16] = 0xAB;  // GUID's first byte
    } else {
        const uint32_t base = 0x80000000u | (tick - 4);
        m.resize(16);
        std::memcpy(m.data() + 12, &base, 4);
    }
    m.push_back(static_cast<uint8_t>(payload));
    m.push_back(static_cast<uint8_t>(payload >> 8));
    m.resize(m.size() + payload, 0x5A);
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

void batches() {
    std::vector<std::vector<uint8_t>> bodies = {entity_msg(repl::kKindCreate, 100, 1, 2542, 9), entity_msg(repl::kKindUpdate, 101, 2, 2542, 5),
                                               destroy_msg(102, 3, {2542}, {7, 8}), entity_msg(repl::kKindUpdate, 103, 4, 99, 3),
                                               std::vector<uint8_t>(6, 0)};
    repl::MsgEntry msgs[5] = {};
    const uint8_t types[5] = {repl::kMsgEntity, repl::kMsgEntity, repl::kMsgList, repl::kMsgEntity, repl::kMsgHandshake};
    uint64_t all = 0;
    for (int i = 0; i < 5; ++i) {
        msgs[i].type = types[i];
        msgs[i].seq = static_cast<uint32_t>(i + 1);
        msgs[i].data = bodies[i].data();
        msgs[i].size = bodies[i].size();
        all += msgs[i].size;
    }
    // everything accepted
    repl::BatchCount n = repl::count_batch(msgs, 5, 5);
    CHECK(n.messages == 5 && n.bytes == all && n.refused == 0);
    CHECK(n.creates == 1 && n.updates == 2 && n.destroy_lists == 1 && n.destroyed_ids == 3);
    // the session took the first 2: the rest is refused and not counted as sent
    n = repl::count_batch(msgs, 5, 2);
    CHECK(n.messages == 2 && n.refused == 3 && n.bytes == msgs[0].size + msgs[1].size && n.creates == 1 && n.updates == 1 &&
          n.destroy_lists == 0);
    // a sent count above the batch's (a stale field) never reads past it
    n = repl::count_batch(msgs, 3, 9);
    CHECK(n.messages == 3 && n.refused == 0);

    // the destroy list's two lists; one cut short keeps only what it holds
    repl::DestroyList l{};
    CHECK(repl::parse_destroy(bodies[2].data(), bodies[2].size(), &l) && l.na == 1 && l.nb == 2 && repl::u32_at(l.ids) == 2542 &&
          repl::u32_at(l.ids + 8) == 8 && l.tick == 102 && l.seq == 3);
    CHECK(repl::parse_destroy(bodies[2].data(), bodies[2].size() - 4, &l) && l.na == 1 && l.nb == 1);
    CHECK(!repl::parse_destroy(bodies[0].data(), bodies[0].size(), &l));  // an entity message is not a destroy list
    // headers
    repl::Header h{};
    CHECK(repl::parse_header(bodies[0].data(), bodies[0].size(), &h) && h.kind == repl::kKindCreate && h.tick == 100 &&
          h.entity == 2542 && h.has_guid && h.guid[0] == 0xAB && h.payload_len == 9);
    CHECK(repl::parse_header(bodies[1].data(), bodies[1].size(), &h) && h.kind == repl::kKindUpdate && h.base == 97 && !h.has_guid &&
          h.payload_len == 5);
    CHECK(!repl::parse_header(bodies[1].data(), bodies[1].size() - 1, &h));  // cut short
}

// ---- observer depth: kiln-repl.dll counts heap allocations made inside observer code ----
int g_depth_seen = -1;
void depth_probe(void*) { g_depth_seen = repl::t_in_observer; }
void observer_depth() {
    repl::Guard g{"depth"};
    CHECK(repl::t_in_observer == 0);
    repl::observe(g, &depth_probe, nullptr);
    CHECK(g_depth_seen == 1 && repl::t_in_observer == 0);
    repl::Guard f{"depth-fault"};
    repl::observe(f, [](void*) { fault_now(); }, nullptr);
    CHECK(f.disabled.load() && repl::t_in_observer == 0);  // a fault leaves the depth right
}

}  // namespace

int main() {
    abi();
    fault_rule();
    rings();
    batches();
    observer_depth();
    std::printf("repl host tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
