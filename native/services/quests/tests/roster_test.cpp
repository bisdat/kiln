// SPDX-License-Identifier: MIT
// The roster (package 17c) on the host: the builder's admission at 64/65 and 128/129, the paged evaluation against a
// model of the game's evaluator (it takes an index modulo 64, as the game's BTS does), the pairing of an evaluation with
// its setter on one thread, the sidecar's validity, and the snapshots with their reads. No game memory, no Wine.
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "../questbuild.h"
#include "../roster_core.h"

using namespace kiln::roster;

static int g_failures = 0;
#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                \
            ++g_failures;                                                            \
        }                                                                            \
    } while (0)

// ---- a model of the game: an NpcResource array (136 bytes, the query's id at +0x10) and the evaluator
constexpr uint64_t kStride = 136;
struct Roster {
    std::vector<uint8_t> bytes;
    explicit Roster(size_t n) : bytes(n * kStride) {
        for (size_t i = 0; i < n; ++i) {
            const uint32_t id = 0x1000 + static_cast<uint32_t>(i);
            std::memcpy(&bytes[i * kStride], &id, 4);
            const uint32_t query = 0x2000 + static_cast<uint32_t>(i);  // every entry has a requirement
            std::memcpy(&bytes[i * kStride + 0x10], &query, 4);
        }
    }
    Span span() const { return Span{reinterpret_cast<uintptr_t>(bytes.data()), bytes.size() / kStride}; }
};

struct Knowledge {
    uint16_t version = 1;
    std::vector<bool> unlocked;        // by query id - 0x2000
    std::vector<bool> fails;           // the query fails (status 1 of the whole evaluation)
    uint32_t calls = 0, most = 0;      // evaluator calls, the most entries one call saw
    int bump_after_call = -1;          // the knowledge moves after that many calls (versions disagree)
};
Knowledge* g_k = nullptr;

// The game's NpcEvalUnlocks: unchanged -> 6; else a bit per entry with an index modulo 64; a failed query -> 1.
void game_eval(Result16* out, void*, uint16_t* seen, const Span* span) {
    Knowledge& k = *g_k;
    ++k.calls;
    k.most = std::max<uint32_t>(k.most, static_cast<uint32_t>(span->count));
    if (*seen == k.version) {
        *out = Result16{6, {}, 0, 0};
        return;
    }
    uint64_t bits = 0;
    bool ok = true;
    for (uint64_t i = 0; i < span->count; ++i) {
        uint32_t q = 0;
        std::memcpy(&q, reinterpret_cast<const uint8_t*>(span->ptr + i * kStride + 0x10), 4);
        const size_t qi = q - 0x2000;
        if (qi < k.fails.size() && k.fails[qi]) ok = false;
        else if (qi < k.unlocked.size() && k.unlocked[qi]) bits |= 1ull << (i & 63);  // the game's BTS
    }
    if (!ok) {
        *out = Result16{1, {}, 0, 0};
        return;
    }
    *seen = k.version;
    *out = Result16{0, {}, 0, bits};
    if (k.bump_after_call >= 0 && static_cast<int>(k.calls) == k.bump_after_call) ++k.version;  // moves after this call
}

void builder() {
    using namespace kiln::qb;
    // a collection of `n` game entries: BlobArray at +0 pointing behind the header
    auto collection = [](uint32_t n) {
        std::vector<uint8_t> b(8 + n * 136);
        const uint32_t rel = 8;
        std::memcpy(b.data(), &rel, 4);
        std::memcpy(b.data() + 4, &n, 4);
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t id = 0x5000 + i;
            std::memcpy(b.data() + 8 + i * 136, &id, 4);
        }
        return b;
    };
    auto defs = [](size_t n) {
        std::vector<NpcDef> v(n);
        for (size_t i = 0; i < n; ++i) v[i].id = 0x9000 + static_cast<uint32_t>(i), v[i].debug_name = "n" + std::to_string(i);
        return v;
    };
    const auto game = collection(35);
    Bytes out;
    std::string err;
    CHECK(npc_count(game.data(), game.size()) == 35);
    CHECK(npc_ids(game.data(), game.size()).size() == 35);
    // 64: 35 + 29 accepted, 35 + 30 refused with the shipped reason, word for word
    CHECK(rebuild_npcs(game.data(), game.size(), defs(29), &out, &err));
    CHECK(npc_count(out.data(), out.size()) == 64);
    CHECK(!rebuild_npcs(game.data(), game.size(), defs(30), &out, &err));
    CHECK(err == "the roster holds at most 64 NPCs (a 64-bit unlock mask): 35 of the game's + 30 new");
    CHECK(err == refusal_text(64, 35, 30));
    // 128: 35 + 30 and 35 + 93 accepted, 35 + 94 refused
    err.clear();
    CHECK(rebuild_npcs(game.data(), game.size(), defs(30), &out, &err, kMaxNpcsPaged));
    CHECK(npc_count(out.data(), out.size()) == 65);
    CHECK(rebuild_npcs(game.data(), game.size(), defs(93), &out, &err, kMaxNpcsPaged));
    CHECK(npc_count(out.data(), out.size()) == 128);
    CHECK(!rebuild_npcs(game.data(), game.size(), defs(94), &out, &err, kMaxNpcsPaged));
    CHECK(err == "the roster holds at most 128 NPCs (two 64-bit unlock pages): 35 of the game's + 94 new");
    CHECK(err == refusal_text(128, 35, 94));
    // any other maximum is 64 (the builder knows two)
    CHECK(!rebuild_npcs(game.data(), game.size(), defs(30), &out, &err, 4096));
}

void paging() {
    for (const size_t n : {size_t{35}, size_t{64}, size_t{65}, size_t{100}, size_t{128}}) {
        Roster r(n);
        Knowledge k;
        k.unlocked.assign(n, false);
        for (size_t i = 0; i < n; i += 3) k.unlocked[i] = true;  // 0, 3, 6, ...: entry 64 is unlocked when 0 is not? 64 % 3 == 1: locked
        if (n > 64) k.unlocked[64] = !k.unlocked[0];              // contradicts entry 0
        g_k = &k;
        Result16 out{0xee, {}, 0xeeee, 0xeeee};
        uint16_t seen = 0;
        const PageOutcome o = eval_pages(&out, nullptr, &seen, r.span(), kStride, 1, game_eval);
        CHECK(k.most <= 64);  // never more than 64 entries reach the game's evaluator
        CHECK(out.status == 0 && seen == k.version && out.zero == 0);
        if (n <= 64) {
            CHECK(!o.paged && k.calls == 1);
            continue;
        }
        CHECK(o.paged && o.complete && o.pages == (n + 63) / 64 && k.calls == o.pages && o.version == k.version);
        CHECK(out.payload == o.masks[0]);
        for (size_t i = 0; i < n; ++i) CHECK(((o.masks[i / 64] >> (i % 64)) & 1) == (k.unlocked[i] ? 1u : 0u));
        CHECK(((o.masks[1] >> 0) & 1) != ((o.masks[0] >> 0) & 1));  // 0 and 64 differ: no aliasing
    }
    // unchanged: page 0 answers 6, nothing is computed further, *seen is left alone
    {
        Roster r(65);
        Knowledge k;
        k.unlocked.assign(65, true);
        g_k = &k;
        Result16 out{};
        uint16_t seen = k.version;
        const PageOutcome o = eval_pages(&out, nullptr, &seen, r.span(), kStride, 1, game_eval);
        CHECK(out.status == 6 && out.payload == 0 && !o.complete && k.calls == 1 && seen == k.version);
    }
    // a failed query on page 2: the whole evaluation fails (status 1), *seen untouched: the game keeps its old cache
    {
        Roster r(128);
        Knowledge k;
        k.unlocked.assign(128, true);
        k.fails.assign(128, false);
        k.fails[100] = true;
        g_k = &k;
        Result16 out{};
        uint16_t seen = 0;
        const PageOutcome o = eval_pages(&out, nullptr, &seen, r.span(), kStride, 1, game_eval);
        CHECK(out.status == 1 && out.payload == 0 && !o.complete && seen == 0 && k.calls == 2);
    }
    // the knowledge moves between page 1 and page 2: the evaluator's own error, *seen untouched, computed next step
    {
        Roster r(70);
        Knowledge k;
        k.unlocked.assign(70, true);
        k.bump_after_call = 1;
        g_k = &k;
        Result16 out{};
        uint16_t seen = 0;
        const PageOutcome o = eval_pages(&out, nullptr, &seen, r.span(), kStride, 1, game_eval);
        CHECK(out.status == 1 && !o.complete && seen == 0);
    }
    // a collection longer than 128 (not Kiln's): one call as the game asked
    {
        Roster r(130);
        Knowledge k;
        k.unlocked.assign(130, true);
        g_k = &k;
        Result16 out{};
        uint16_t seen = 0;
        const PageOutcome o = eval_pages(&out, nullptr, &seen, r.span(), kStride, 1, game_eval);
        CHECK(!o.paged && k.calls == 1 && k.most == 130);
    }
}

void pairing() {
    PendingStack s;
    Pending a;
    a.span_ptr = 0x100, a.count = 65, a.pages = 2, a.masks[0] = 1, a.masks[1] = 2, a.version = 5;
    Pending b = a;
    b.span_ptr = 0x200, b.masks[0] = 7, b.masks[1] = 8, b.version = 9;
    Pending got;
    s.push(a);
    s.push(b);  // nested: b's setter comes first
    CHECK(s.take(0x200, 65, 7, 9, &got) && got.masks[1] == 8 && s.size() == 1);
    CHECK(s.take(0x100, 65, 1, 5, &got) && got.masks[1] == 2 && s.size() == 0);
    // a setter that belongs to no paged evaluation finds nothing; a stale entry never pairs with another value
    s.push(a);
    CHECK(!s.take(0x100, 65, 1, 6, &got));   // another version
    CHECK(!s.take(0x100, 65, 3, 5, &got));   // another page-0 value
    CHECK(!s.take(0x100, 66, 1, 5, &got));   // another collection
    CHECK(s.size() == 1);
    for (int i = 0; i < 10; ++i) s.push(b);  // bounded: the oldest go
    CHECK(s.size() == PendingStack::kDepth);
    // per thread: another thread's stack is its own
    std::thread([&] {
        thread_local PendingStack t;
        Pending x;
        CHECK(!t.take(0x200, 65, 7, 9, &x));
    }).join();
}

void sidecars() {
    Sidecars sc;
    Pending p;
    p.span_ptr = 0x100, p.count = 70, p.pages = 2, p.masks[0] = 0x5, p.masks[1] = 0x3, p.version = 11;
    LiveCache live{0, 11, 0x5, 0x100, 70};
    CHECK(sc.bit(0xaaa, live, 64) == Sidecars::Bit::NoPage);  // no page: the hook fails closed
    CHECK(sc.attach(0xaaa, p));
    CHECK(sc.bit(0xaaa, live, 64) == Sidecars::Bit::Unlocked && sc.bit(0xaaa, live, 65) == Sidecars::Bit::Unlocked &&
          sc.bit(0xaaa, live, 66) == Sidecars::Bit::Locked);
    CHECK(sc.bit(0xaaa, live, 70) == Sidecars::Bit::Stale);   // past the collection
    CHECK(sc.bit(0xbbb, live, 64) == Sidecars::Bit::NoPage);  // another registry (another world) has no page
    LiveCache moved = live;
    moved.version = 12;  // the game's cache was written without a paged evaluation
    CHECK(sc.bit(0xaaa, moved, 64) == Sidecars::Bit::Stale);
    moved = live;
    moved.payload = 0x4;
    CHECK(sc.bit(0xaaa, moved, 64) == Sidecars::Bit::Stale);
    moved = live;
    moved.status = 0x10;  // the registry was reloaded ("not computed")
    CHECK(sc.bit(0xaaa, moved, 64) == Sidecars::Bit::Stale);
    moved = live;
    moved.span_ptr = 0x900;  // another collection at the registry
    CHECK(sc.bit(0xaaa, moved, 64) == Sidecars::Bit::Stale);
    CHECK(sc.invalidate(0xaaa) && sc.bit(0xaaa, live, 64) == Sidecars::Bit::NoPage);
    // a world's stop takes its registries' pages and the ones no world was seen to own
    CHECK(sc.attach(0xaaa, p) && sc.attach(0xbbb, p) && sc.attach(0xccc, p));
    sc.adopt(0xaaa, KILN_WORLD_SERVER, 1);
    sc.adopt(0xbbb, KILN_WORLD_CLIENT, 1);
    sc.drop_world(KILN_WORLD_CLIENT);
    CHECK(sc.bit(0xaaa, live, 64) == Sidecars::Bit::Unlocked);
    CHECK(sc.bit(0xbbb, live, 64) == Sidecars::Bit::NoPage && sc.bit(0xccc, live, 64) == Sidecars::Bit::NoPage);
    // An evaluation can survive on a worker whose setter never arrived. A new world
    // may reuse every address and the same version/page-0 value after stop.
    {
        Sidecars stopped;
        Pending old = p;
        old.epoch = stopped.epoch();
        old.masks[1] = 0;
        stopped.drop_world(KILN_WORLD_SERVER);
        CHECK(!stopped.attach(0xaaa, old));
        CHECK(stopped.bit(0xaaa, live, 64) == Sidecars::Bit::NoPage);
        // A fresh evaluation in the next world can still attach.
        Pending fresh = p;
        fresh.epoch = stopped.epoch();
        CHECK(stopped.attach(0xaaa, fresh));
        CHECK(stopped.bit(0xaaa, live, 64) == Sidecars::Bit::Unlocked);
        // A stop racing with an in-flight evaluator also rejects its later attach.
        std::thread worker([&] { stopped.drop_world(KILN_WORLD_SERVER); });
        worker.join();
        CHECK(!stopped.attach(0xbbb, fresh));
    }
    // a held lock: a hook does not wait for it (bounded, then the caller fails closed)
    {
        BoundedLock l;
        std::atomic<bool> done{false};
        CHECK(l.try_for());
        std::thread t([&] {
            CHECK(!l.try_for(std::chrono::microseconds(200)));
            done = true;
        });
        t.join();
        l.unlock();
        CHECK(done && l.busy() == 1);
    }
}

void snapshots() {
    Snapshots snaps;
    Owners owners;
    owners.known = true;
    for (uint32_t i = 0; i < 35; ++i) owners.boot_ids.push_back(0x1000 + i);
    for (uint32_t i = 35; i < 70; ++i) owners.kiln_ids.push_back(0x1000 + i);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < 70; ++i) ids.push_back(0x1000 + i);
    Sidecar page;
    page.count = 70, page.masks[0] = 0x1, page.masks[1] = 0x2;
    LiveCache live{0, 4, 0x1, 0x100, 70};
    const WorldNow server{true, 3}, client{true, 2}, none{false, 0};

    uint32_t gen = 99, readiness = 99;
    // no world: NoWorld; a world without a snapshot: NotReady, never a token
    CHECK(world(snaps, KILN_WORLD_SERVER, none, true, &gen, &readiness) == KILN_OK && gen == 0 && readiness == KILN_CAPACITY_NO_WORLD);
    CHECK(world(snaps, KILN_WORLD_SERVER, server, true, &gen, &readiness) == KILN_OK && gen == 0 && readiness == KILN_CAPACITY_NOT_READY);
    CHECK(world(snaps, KILN_WORLD_PREDICTION, server, true, &gen, &readiness) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(world(snaps, 7, server, true, &gen, &readiness) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(world(snaps, KILN_WORLD_CLIENT, client, false, &gen, &readiness) == KILN_ERR_UNAVAILABLE);  // a dedicated server

    // a page the sidecar does not have: not ready (never a truncated roster)
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, nullptr, owners, 128));
    CHECK(world(snaps, KILN_WORLD_SERVER, server, true, &gen, &readiness) == KILN_OK && readiness == KILN_CAPACITY_NOT_READY);
    // at a maximum of 64 a 70-entry roster is not answered (an unsupported roster)
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, &page, owners, 64));
    CHECK(world(snaps, KILN_WORLD_SERVER, server, true, &gen, &readiness) == KILN_OK && readiness == KILN_CAPACITY_NOT_READY);
    // ready
    std::vector<Change> changes;
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, &page, owners, 128), &changes);
    CHECK(changes.empty());  // the first ready snapshot of a world is its baseline
    CHECK(world(snaps, KILN_WORLD_SERVER, server, true, &gen, &readiness) == KILN_OK && gen == 3 && readiness == KILN_CAPACITY_VALID);
    const uint64_t rev = snaps.get(KILN_WORLD_SERVER)->revision;
    // an unchanged snapshot keeps the revision
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, &page, owners, 128), &changes);
    CHECK(snaps.get(KILN_WORLD_SERVER)->revision == rev && changes.empty());

    KilnNpcUnlockReply r{};
    r.size = sizeof r;
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000, server, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_VALID &&
          r.unlocked == 1 && r.knowledge_version == 4 && r.world_kind == 0 && r.generation == 3 && r.revision == rev);
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000 + 64, server, true, &r) == KILN_OK && r.unlocked == 0);
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000 + 65, server, true, &r) == KILN_OK && r.unlocked == 1);
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x4242, server, true, &r) == KILN_OK && r.readiness == KILN_NPC_UNKNOWN_ID &&
          r.unlocked == 0 && r.knowledge_version == 0 && r.npc_id == 0x4242);
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 0, 0x1000, server, true, &r) == KILN_ERR_INVALID_ARGUMENT);   // generation 0
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0, server, true, &r) == KILN_ERR_INVALID_ARGUMENT);        // NpcId 0
    CHECK(unlock_at(snaps, KILN_WORLD_PREDICTION, 3, 1, server, true, &r) == KILN_ERR_INVALID_ARGUMENT);
    KilnNpcUnlockReply small{};
    small.size = 8;
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000, server, true, &small) == KILN_ERR_INVALID_ARGUMENT);
    // the client token is the client world's: the server's snapshot never answers it
    CHECK(unlock_at(snaps, KILN_WORLD_CLIENT, 2, 0x1000, client, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_NOT_READY);
    CHECK(unlock_at(snaps, KILN_WORLD_CLIENT, 3, 0x1000, client, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_WORLD_ENDED);

    // the budget
    KilnCapacityInfo b{};
    b.size = sizeof b;
    CHECK(budget(snaps, KILN_WORLD_SERVER, 3, server, true, 128, true, &b) == KILN_OK && b.readiness == KILN_CAPACITY_VALID &&
          b.capacity == 128 && b.game_used == 35 && b.kiln_used == 35 && b.reserved == 0 && (b.flags & KILN_CAPACITY_EXPANDED) &&
          (b.flags & KILN_CAPACITY_WORLD) && b.world_kind == 0 && b.generation == 3);
    CHECK(budget(snaps, KILN_WORLD_SERVER, 0, server, true, 128, true, &b) == KILN_ERR_INVALID_ARGUMENT);
    // entries neither the game's collection nor Kiln's build put there: reserved
    Owners partial = owners;
    partial.kiln_ids.resize(20);
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, &page, partial, 128));
    CHECK(budget(snaps, KILN_WORLD_SERVER, 3, server, true, 128, true, &b) == KILN_OK && b.game_used == 35 && b.kiln_used == 20 &&
          b.reserved == 15);
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, live, ids, &page, owners, 128));

    // a change of one entry's state: one event, with the new snapshot's version and revision
    LiveCache next = live;
    next.version = 5;
    Sidecar page2 = page;
    page2.masks[1] = 0x3;  // entry 64 unlocked (65 was)
    changes.clear();
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, next, ids, &page2, owners, 128), &changes);
    CHECK(changes.size() == 1 && changes[0].npc_id == 0x1000 + 64 && changes[0].before == 0 && changes[0].after == 1 &&
          changes[0].version == 5 && changes[0].revision == snaps.get(KILN_WORLD_SERVER)->revision);
    // a version change alone is no event
    next.version = 6;
    changes.clear();
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 3, next, ids, &page2, owners, 128), &changes);
    CHECK(changes.empty());

    // a worker thread reads both tokens without any game memory
    Sidecar client_page = page;
    client_page.masks[1] = 0;  // the client world's own registry: entry 65 locked there
    snaps.publish(make_snapshot(KILN_WORLD_CLIENT, 2, live, ids, &client_page, owners, 128));
    std::thread([&] {
        KilnNpcUnlockReply s{}, c{};
        s.size = c.size = sizeof s;
        CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000 + 65, server, true, &s) == KILN_OK && s.readiness == KILN_CAPACITY_VALID &&
              s.unlocked == 1);
        CHECK(unlock_at(snaps, KILN_WORLD_CLIENT, 2, 0x1000 + 65, client, true, &c) == KILN_OK && c.readiness == KILN_CAPACITY_VALID &&
              c.unlocked == 0);  // the client world's own page
    }).join();

    // the server world ends: its token reads WorldEnded with nulls, never false-success, never the next world's data
    snaps.end(KILN_WORLD_SERVER, 3);
    const WorldNow gone{false, 3}, next_world{true, 4};
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000, gone, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_WORLD_ENDED &&
          r.unlocked == 0 && r.knowledge_version == 0 && r.generation == 3);
    CHECK(budget(snaps, KILN_WORLD_SERVER, 3, gone, true, 128, true, &b) == KILN_OK && b.readiness == KILN_CAPACITY_WORLD_ENDED &&
          b.capacity == 0 && !(b.flags & KILN_CAPACITY_READY));
    CHECK(world(snaps, KILN_WORLD_SERVER, gone, true, &gen, &readiness) == KILN_OK && readiness == KILN_CAPACITY_NO_WORLD && gen == 0);
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 4, live, ids, &page, owners, 128));
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 3, 0x1000, next_world, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_WORLD_ENDED);
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 4, 0x1000, next_world, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_VALID);
    // a missed stop: the session's counter moved on, the old token still ends
    CHECK(unlock_at(snaps, KILN_WORLD_CLIENT, 2, 0x1000, WorldNow{true, 3}, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_WORLD_ENDED);
    // a cache that is not computed (status 0x10 after the registry's load): not ready
    LiveCache fresh = live;
    fresh.status = 0x10;
    snaps.publish(make_snapshot(KILN_WORLD_SERVER, 4, fresh, ids, &page, owners, 128));
    CHECK(unlock_at(snaps, KILN_WORLD_SERVER, 4, 0x1000, next_world, true, &r) == KILN_OK && r.readiness == KILN_CAPACITY_NOT_READY);
}

int main() {
    builder();
    paging();
    pairing();
    sidecars();
    snapshots();
    if (g_failures) {
        std::printf("roster_test: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("roster_test: PASS (builder 64/65 128/129, paging, pairing, sidecars, snapshots and reads)\n");
    return 0;
}
