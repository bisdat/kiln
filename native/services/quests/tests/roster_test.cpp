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
    sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
    Pending p;
    sc.epochs(p.epochs);
    p.span_ptr = 0x100, p.count = 70, p.pages = 2, p.masks[0] = 0x5, p.masks[1] = 0x3, p.version = 11;
    LiveCache live{0, 11, 0x5, 0x100, 70};
    const uint32_t S = KILN_WORLD_SERVER;
    CHECK(sc.bit(0xaaa, live, 64, S, 1) == Sidecars::Bit::NoPage);  // no page: the hook fails closed
    CHECK(sc.attach(0xaaa, p) == Sidecars::Attach::Attached);
    CHECK(sc.bit(0xaaa, live, 64, S, 1) == Sidecars::Bit::Unlocked && sc.bit(0xaaa, live, 65, S, 1) == Sidecars::Bit::Unlocked &&
          sc.bit(0xaaa, live, 66, S, 1) == Sidecars::Bit::Locked);
    CHECK(sc.bit(0xaaa, live, 70, S, 1) == Sidecars::Bit::Stale);   // past the collection
    CHECK(sc.bit(0xbbb, live, 64, S, 1) == Sidecars::Bit::Moved);   // not the server world's registry
    LiveCache moved = live;
    moved.version = 12;  // the game's cache was written without a paged evaluation
    CHECK(sc.bit(0xaaa, moved, 64, S, 1) == Sidecars::Bit::Stale);
    moved = live;
    moved.payload = 0x4;
    CHECK(sc.bit(0xaaa, moved, 64, S, 1) == Sidecars::Bit::Stale);
    moved = live;
    moved.status = 0x10;  // the registry was reloaded ("not computed")
    CHECK(sc.bit(0xaaa, moved, 64, S, 1) == Sidecars::Bit::Stale);
    moved = live;
    moved.span_ptr = 0x900;  // another collection at the registry
    CHECK(sc.bit(0xaaa, moved, 64, S, 1) == Sidecars::Bit::Stale);
    CHECK(sc.invalidate(0xaaa) && sc.bit(0xaaa, live, 64, S, 1) == Sidecars::Bit::NoPage);
    // Codex's regression (a7ed80ce), per world now: an evaluation can survive on a worker whose setter never arrived; a
    // new world may reuse every address and the same version/page-0 value after the stop
    {
        Sidecars stopped;
        stopped.bind(S, 1, 0xaaa);
        Pending old = p;
        stopped.epochs(old.epochs);
        old.masks[1] = 0;
        stopped.unbind(S, 1);
        stopped.bind(S, 2, 0xaaa);
        CHECK(stopped.attach(0xaaa, old) == Sidecars::Attach::Old);
        CHECK(stopped.bit(0xaaa, live, 64, S, 2) == Sidecars::Bit::NoPage);
        // a fresh evaluation in the next world attaches
        Pending fresh = p;
        stopped.epochs(fresh.epochs);
        CHECK(stopped.attach(0xaaa, fresh) == Sidecars::Attach::Attached);
        CHECK(stopped.bit(0xaaa, live, 64, S, 2) == Sidecars::Bit::Unlocked);
        // a stop on another thread while the evaluator is in flight also rejects its later attach
        std::thread worker([&] { stopped.unbind(S, 2); });
        worker.join();
        stopped.bind(S, 3, 0xaaa);
        CHECK(stopped.attach(0xaaa, fresh) == Sidecars::Attach::Old);
        CHECK(stopped.live_count() == 0);
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

// Review round 1 (17c): a page's world from its attachment, reads that cannot span a world's end, lost pages.
// The first two cases were red against the API before the fix (roster_test.cpp at a7ed80ce + these cases: "the server
// world's page survived" and "not the next world's answer" failed); here in the API after it.
Pending pend(Sidecars& sc, uint64_t mask0, uint64_t mask1, uint16_t version) {
    Pending p;
    sc.epochs(p.epochs);
    p.span_ptr = 0x100, p.count = 70, p.pages = 2, p.masks[0] = mask0, p.masks[1] = mask1, p.version = version;
    return p;
}

void review() {
    const LiveCache live{0, 11, 0x5, 0x100, 70};
    // two worlds of a hosting game, with no roster reader at all: stopping the client world keeps the server world's page
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        sc.bind(KILN_WORLD_CLIENT, 1, 0xbbb);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Attached);
        CHECK(sc.attach(0xbbb, pend(sc, 0x5, 0x0, 11)) == Sidecars::Attach::Attached);
        sc.unbind(KILN_WORLD_CLIENT, 1);
        CHECK(sc.bit(0xaaa, live, 64, KILN_WORLD_SERVER, 1) == Sidecars::Bit::Unlocked);  // the server world's page survived
        uint32_t k = 9, g = 9;
        CHECK(sc.owner(0xbbb, &k, &g) == Sidecars::Owner::None);                          // the client world's is gone
        sc.bind(KILN_WORLD_CLIENT, 2, 0xbbb);                                             // a new client world, same address
        CHECK(sc.bit(0xbbb, live, 64, KILN_WORLD_CLIENT, 2) == Sidecars::Bit::NoPage);    // nothing of the old one
        // and the other way round: the server world stops, the client world's page stays
        CHECK(sc.attach(0xbbb, pend(sc, 0x5, 0x0, 11)) == Sidecars::Attach::Attached);
        sc.unbind(KILN_WORLD_SERVER, 1);
        CHECK(sc.bit(0xbbb, live, 64, KILN_WORLD_CLIENT, 2) == Sidecars::Bit::Locked);
    }
    // a reader took the owner and the fields, the world ended, the next world's registry at the same address got a page
    // with the same tuple: the reader gets Moved, never the next world's answer
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x0, 11)) == Sidecars::Attach::Attached);
        uint32_t kind = 9, session = 9;
        CHECK(sc.owner(0xaaa, &kind, &session) == Sidecars::Owner::Found && kind == KILN_WORLD_SERVER && session == 1);
        const LiveCache taken = live;
        sc.unbind(KILN_WORLD_SERVER, 1);
        sc.bind(KILN_WORLD_SERVER, 2, 0xaaa);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Attached);
        CHECK(sc.bit(0xaaa, taken, 64, kind, session) == Sidecars::Bit::Moved);  // not the next world's answer
        CHECK(sc.bit(0xaaa, taken, 64, KILN_WORLD_SERVER, 2) == Sidecars::Bit::Unlocked);
    }
    // the hook's read protocol: the world ends, or the game's setter writes, between the fields and the answer: Moved
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Attached);
        auto index64 = [](const LiveCache&, uint64_t* i) { *i = 64; return true; };
        int reads = 0;
        auto steady = [&](LiveCache* l) { ++reads, *l = live; return true; };
        QueryAnswer a = page_query(sc, 0xaaa, steady, index64);
        CHECK(a.kind == QueryAnswer::Kind::Page && a.unlocked && reads == 3);
        auto index3 = [](const LiveCache&, uint64_t* i) { *i = 3; return true; };
        reads = 0;
        CHECK(page_query(sc, 0xaaa, steady, index3).kind == QueryAnswer::Kind::Game && reads == 1);  // page 0: the game's, no lock
        // a setter writes between the sorting read and the owner's fields, or after the page (the version moves)
        for (const int when : {2, 3}) {
            reads = 0;
            auto setter = [&](LiveCache* l) {
                *l = live;
                if (++reads == when) l->version = 12;
                return true;
            };
            a = page_query(sc, 0xaaa, setter, index64);
            CHECK(a.kind == QueryAnswer::Kind::Kept && a.why == Sidecars::Bit::Moved);
        }
        // the world stops and the next binds the same address after the owner was taken, before the page (the page
        // check sees it: the next world's page with the same fields is never this reader's answer)
        reads = 0;
        auto stop_first = [&](LiveCache* l) {
            *l = live;
            if (++reads == 2) {
                sc.unbind(KILN_WORLD_SERVER, 1);
                sc.bind(KILN_WORLD_SERVER, 2, 0xaaa);
                sc.attach(0xaaa, pend(sc, 0x5, 0x0, 11));  // the next world's page: entry 64 locked there
            }
            return true;
        };
        a = page_query(sc, 0xaaa, stop_first, index64);
        CHECK(a.kind == QueryAnswer::Kind::Kept && a.why == Sidecars::Bit::Moved);
        // ... or after the page, before the second look (the owner check sees it)
        reads = 0;
        auto stop_after = [&](LiveCache* l) {
            *l = live;
            if (++reads == 3) {
                sc.unbind(KILN_WORLD_SERVER, 2);
                sc.bind(KILN_WORLD_SERVER, 3, 0xaaa);
            }
            return true;
        };
        a = page_query(sc, 0xaaa, stop_after, index64);
        CHECK(a.kind == QueryAnswer::Kind::Kept && a.why == Sidecars::Bit::Moved);
        // no live world owns the registry: kept, counted as Unowned
        sc.unbind(KILN_WORLD_SERVER, 3);
        a = page_query(sc, 0xaaa, steady, index64);
        CHECK(a.kind == QueryAnswer::Kind::Kept && a.why == Sidecars::Bit::Unowned);
        // the loader's reset (status 0x10): the game's own answer (its "cache invalid")
        sc.bind(KILN_WORLD_SERVER, 4, 0xaaa);
        auto reset = [&](LiveCache* l) { *l = LiveCache{0x10, 0, 0, 0x100, 70}; return true; };
        CHECK(page_query(sc, 0xaaa, reset, index64).kind == QueryAnswer::Kind::Game);
        // an unreadable registry, an id that is not an entry: the game's own answer
        CHECK(page_query(sc, 0xaaa, [](LiveCache*) { return false; }, index64).kind == QueryAnswer::Kind::Game);
        CHECK(page_query(sc, 0xaaa, steady, [](const LiveCache&, uint64_t*) { return false; }).kind == QueryAnswer::Kind::Game);
    }
    // the epoch is per world: an evaluation in flight across the OTHER world's stop is attached, across its own is not
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        sc.bind(KILN_WORLD_CLIENT, 1, 0xbbb);
        const Pending before = pend(sc, 0x5, 0x1, 11);
        sc.unbind(KILN_WORLD_CLIENT, 1);
        CHECK(sc.attach(0xaaa, before) == Sidecars::Attach::Attached);
        const Pending in_flight = pend(sc, 0x5, 0x1, 11);
        sc.unbind(KILN_WORLD_SERVER, 1);
        sc.bind(KILN_WORLD_SERVER, 2, 0xaaa);  // same address, new world
        CHECK(sc.attach(0xaaa, in_flight) == Sidecars::Attach::Old);
        CHECK(sc.bit(0xaaa, live, 64, KILN_WORLD_SERVER, 2) == Sidecars::Bit::NoPage);
    }
    // a stopped session is never bound again; the context the game passed is kept per session
    {
        Sidecars sc;
        CHECK(sc.bind(KILN_WORLD_SERVER, 4, 0xaaa));
        Pending p = pend(sc, 0x5, 0x1, 11);
        p.ctx = 0x7700;
        CHECK(sc.attach(0xaaa, p) == Sidecars::Attach::Attached);
        CHECK(sc.context(KILN_WORLD_SERVER, 4) == 0x7700 && sc.context(KILN_WORLD_SERVER, 5) == 0);
        // the world's registry is replaced within the session: the old registry's page and the context go
        CHECK(sc.bind(KILN_WORLD_SERVER, 4, 0xddd));
        CHECK(sc.live_count() == 0 && sc.context(KILN_WORLD_SERVER, 4) == 0);
        uint32_t k = 0, g = 0;
        CHECK(sc.owner(0xaaa, &k, &g) == Sidecars::Owner::None && sc.owner(0xddd, &k, &g) == Sidecars::Owner::Found);
        sc.unbind(KILN_WORLD_SERVER, 4);
        CHECK(!sc.bind(KILN_WORLD_SERVER, 4, 0xaaa) && !sc.bind(KILN_WORLD_SERVER, 3, 0xaaa));  // a late Pre of a stopped world
        CHECK(sc.owner(0xaaa, &k, &g) == Sidecars::Owner::None);
        CHECK(sc.bind(KILN_WORLD_SERVER, 5, 0xaaa) && sc.owner(0xaaa, &k, &g) == Sidecars::Owner::Found && g == 5);
    }
    // a registry no live world bound: nothing is attached, a read is Unowned (the hook then answers "kept")
    {
        Sidecars sc;
        CHECK(sc.attach(0xccc, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Unowned);
        uint32_t k = 0, g = 0;
        CHECK(sc.owner(0xccc, &k, &g) == Sidecars::Owner::None);
        CHECK(sc.bit(0xccc, live, 64, kKindUnknown, 0) == Sidecars::Bit::Unowned);
    }
    // a world's session that was never seen to stop: binding the next session drops the old session's pages
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Attached);
        const Pending in_flight = pend(sc, 0x5, 0x1, 11);
        sc.bind(KILN_WORLD_SERVER, 2, 0xaaa);
        CHECK(sc.bit(0xaaa, live, 64, KILN_WORLD_SERVER, 2) == Sidecars::Bit::NoPage);
        CHECK(sc.live_count() == 0);
        CHECK(sc.attach(0xaaa, in_flight) == Sidecars::Attach::Old);  // the unseen stop moved the epoch too
        CHECK(!sc.bind(KILN_WORLD_SERVER, 1, 0xaaa));                  // and session 1 is over
    }
    // the loader's reset to "not computed" (status 0x10, version 0) while Kiln holds a page: the page says nothing
    // (Stale: the hook gives the game's own "cache invalid"), the snapshot is NotReady, no recomputation is asked for;
    // the producer's next evaluation attaches a page that matches again
    {
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        CHECK(sc.attach(0xaaa, pend(sc, 0x5, 0x1, 11)) == Sidecars::Attach::Attached);
        const LiveCache reset{0x10, 0, 0, 0x100, 70};
        CHECK(sc.bit(0xaaa, reset, 64, KILN_WORLD_SERVER, 1) == Sidecars::Bit::Stale);
        CHECK(!sc.wants_page(KILN_WORLD_SERVER, 1, 0xaaa, reset));
        Sidecar page;
        CHECK(!sc.read(KILN_WORLD_SERVER, 1, 0xaaa, reset, &page));
        Owners owners;
        std::vector<uint32_t> ids(70);
        for (uint32_t i = 0; i < 70; ++i) ids[i] = 0x1000 + i;
        CHECK(!make_snapshot(KILN_WORLD_SERVER, 1, reset, ids, nullptr, owners, 128).ready);
        const LiveCache again{0, 12, 0x7, 0x100, 70};
        CHECK(sc.attach(0xaaa, pend(sc, 0x7, 0x2, 12)) == Sidecars::Attach::Attached);
        CHECK(sc.bit(0xaaa, again, 65, KILN_WORLD_SERVER, 1) == Sidecars::Bit::Unlocked);
    }
    // a lost page is asked for again and computed again at the world's safe point, with the game's evaluator, only when
    // it agrees with the game's cache; nothing waits for the knowledge to change
    {
        Roster r(70);
        Knowledge k;
        k.unlocked.assign(70, false);
        k.unlocked[0] = k.unlocked[66] = true;
        k.version = 11;
        g_k = &k;
        Sidecars sc;
        sc.bind(KILN_WORLD_SERVER, 1, 0xaaa);
        const LiveCache cache{0, 11, 0x1, r.span().ptr, 70};  // what the producer wrote: entry 0 unlocked, version 11
        CHECK(sc.wants_page(KILN_WORLD_SERVER, 1, 0xaaa, cache));  // no page: asked for
        Pending p;
        sc.epochs(p.epochs);
        CHECK(recompute(cache, r.span(), kStride, 1, reinterpret_cast<void*>(uintptr_t{0x7700}), game_eval, &p));
        CHECK(p.ctx == 0x7700 && p.pages == 2 && p.version == 11 && p.masks[0] == 0x1 && p.masks[1] == 0x4);
        CHECK(sc.attach(0xaaa, p) == Sidecars::Attach::Attached);
        CHECK(!sc.wants_page(KILN_WORLD_SERVER, 1, 0xaaa, cache));
        CHECK(sc.bit(0xaaa, cache, 66, KILN_WORLD_SERVER, 1) == Sidecars::Bit::Unlocked);
        CHECK(sc.bit(0xaaa, cache, 65, KILN_WORLD_SERVER, 1) == Sidecars::Bit::Locked);
        CHECK(k.most <= 64);
        // lost again (lab: kiln/control/roster.drop-page), and the knowledge moved since the cache was written: not
        // attached (the producer's own evaluation in the step attaches the right one)
        sc.drop_page(KILN_WORLD_SERVER);
        CHECK(sc.wants_page(KILN_WORLD_SERVER, 1, 0xaaa, cache));
        k.version = 12;
        Pending q;
        sc.epochs(q.epochs);
        CHECK(!recompute(cache, r.span(), kStride, 1, nullptr, game_eval, &q));
        // a cache whose page 0 does not match the knowledge (the evaluator answers otherwise): not attached either
        k.version = 11;
        const LiveCache wrong{0, 11, 0x3, r.span().ptr, 70};
        CHECK(!recompute(wrong, r.span(), kStride, 1, nullptr, game_eval, &q));
        // a roster that fits in 64 asks for nothing
        const LiveCache small{0, 11, 0x1, r.span().ptr, 64};
        CHECK(!sc.wants_page(KILN_WORLD_SERVER, 1, 0xaaa, small));
    }
    // a world whose registry could not be read this step: NotReady, not the last ready snapshot
    {
        Snapshots snaps;
        Owners owners;
        std::vector<uint32_t> ids(70);
        for (uint32_t i = 0; i < 70; ++i) ids[i] = 0x1000 + i;
        Sidecar page;
        page.count = 70, page.masks[0] = 0x5, page.masks[1] = 0x1;
        snaps.publish(make_snapshot(KILN_WORLD_SERVER, 1, live, ids, &page, owners, 128));
        const WorldNow now{true, 1};
        uint32_t gen = 0, readiness = 0;
        CHECK(world(snaps, KILN_WORLD_SERVER, now, true, &gen, &readiness) == KILN_OK && readiness == KILN_CAPACITY_VALID);
        snaps.publish(unreadable_snapshot(KILN_WORLD_SERVER, 1));
        CHECK(world(snaps, KILN_WORLD_SERVER, now, true, &gen, &readiness) == KILN_OK && readiness == KILN_CAPACITY_NOT_READY);
    }
}

int main() {
    review();
    builder();
    paging();
    pairing();
    sidecars();
    snapshots();
    if (g_failures) {
        std::printf("roster_test: %d FAILED\n", g_failures);
        return 1;
    }
    std::printf("roster_test: PASS (review: ownership, reads across a stop, recovery; builder 64/65 128/129, paging, pairing, sidecars, snapshots and reads)\n");
    return 0;
}
