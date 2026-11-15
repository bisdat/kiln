// SPDX-License-Identifier: MIT
// Host test of kiln.component_changes' core (watch_core.h, chapter 07 §4.6) on a fake world: budgets and refusals,
// link expansion, baselines and compares, tokens and the origin rule, the two-frame queue and its batches, world stop,
// another life of an id, players, owners that go away. tests/run.sh builds it with ASan/UBSan.
#include <cstdio>
#include <map>
#include <set>
#include <thread>

#include "../watch_core.h"

using namespace kiln::decisions::watch;

static int g_pass = 0, g_fail = 0;
#define CHECK(x) ((x) ? ++g_pass : (++g_fail, std::printf("FAIL line %d: %s\n", __LINE__, #x)))

struct World : Ports {
    std::map<std::pair<uint32_t, uint32_t>, std::vector<uint8_t>> comps;
    std::map<uint32_t, std::array<uint8_t, kSetupSize>> setups;
    std::map<uint32_t, uint64_t> spawn;        // entity -> SpawnTime (its life); absent: no identity
    std::set<uint32_t> gone;
    std::vector<uint32_t> in_game;
    std::map<std::pair<uint32_t, uint32_t>, std::pair<int32_t, int32_t>> typed_values;
    std::set<KilnOwner> dead;
    uint64_t sess = 7;
    uint64_t serial = 0;
    std::map<std::pair<uint32_t, uint64_t>, uint64_t> serials;  // (entity, life) -> life serial

    bool read(uint32_t e, uint32_t c, uint8_t* out, uint32_t* size) override {
        auto it = comps.find({e, c});
        if (it == comps.end() || gone.count(e)) return *size = 0, false;
        *size = static_cast<uint32_t>(it->second.size());
        if (it->second.size() > KILN_WATCH_MAX_BYTES) return false;
        std::memcpy(out, it->second.data(), it->second.size());
        return true;
    }
    bool setup(uint32_t e, uint8_t* out) override {
        auto it = setups.find(e);
        if (it == setups.end() || gone.count(e)) return false;
        std::memcpy(out, it->second.data(), kSetupSize);
        return true;
    }
    bool key(uint32_t e, KilnEventEntityKey* out, bool* exists) override {
        *out = KilnEventEntityKey{};
        *exists = !gone.count(e) && (spawn.count(e) || comps.lower_bound({e, 0}) != comps.end());
        auto it = spawn.find(e);
        if (gone.count(e) || it == spawn.end()) return false;
        auto& s = serials[{e, it->second}];
        if (!s) s = ++serial;
        out->entity = e;
        out->flags = KILN_EVENT_KEY_SPAWN_TIME;
        out->spawn_time = it->second;
        out->life_serial = s;
        return true;
    }
    uint32_t players(uint32_t* out, uint32_t cap) override {
        uint32_t n = 0;
        for (uint32_t p : in_game)
            if (n < cap) out[n++] = p;
        return n;
    }
    bool typed(uint32_t e, uint32_t c, int32_t* cur, int32_t* max) override {
        auto it = typed_values.find({e, c});
        if (it == typed_values.end()) return false;
        *cur = it->second.first;
        *max = it->second.second;
        return true;
    }
    bool owner_alive(KilnOwner o) override { return !dead.count(o); }
    uint64_t session() override { return sess; }

    void put(uint32_t e, uint32_t c, std::vector<uint8_t> bytes) { comps[{e, c}] = std::move(bytes); }
    void entity(uint32_t e, uint64_t life = 1000) { spawn[e] = life; }
    void set_byte(uint32_t e, uint32_t c, size_t at, uint8_t v) { comps[{e, c}][at] = v; }
    // An InventorySetup: links (entity, category) in order.
    void link(uint32_t e, std::vector<std::pair<uint32_t, uint8_t>> links) {
        std::array<uint8_t, kSetupSize> s{};
        for (size_t k = 0; k < links.size(); ++k) {
            std::memcpy(s.data() + 4 * k, &links[k].first, 4);
            s[0x40 + k] = links[k].second;
        }
        setups[e] = s;
    }
    // An Inventory: up to 8 (item, count, pide) stacks.
    void stacks(uint32_t inv, std::vector<std::array<uint32_t, 3>> st) {
        std::vector<uint8_t> b(kInventorySize, 0);
        for (size_t k = 0; k < st.size(); ++k) std::memcpy(b.data() + 12 * k, st[k].data(), 12);
        put(inv, KILN_WATCH_INVENTORY, b);
    }
};

static std::vector<uint8_t> bytes40(uint8_t fill) { return std::vector<uint8_t>(40, fill); }
static std::vector<uint8_t> c_of(uint8_t fill) { return bytes40(fill); }

struct Run {
    World w;
    Watcher c{&w};
    uint64_t epoch = 1, step = 10;
    const KilnOwner mod = 101, host = 900;
    Run() {
        c.set_on(true);
        c.attach(host);
    }
    // Pre (the caller's code), then window_begin, the "sim" (fn), window_end.
    template <typename F>
    void tick(F sim) {
        ++step;
        c.window_begin(epoch, step);
        sim();
        c.window_end(epoch, step);
    }
    void tick() {
        tick([] {});
    }
    uint64_t watch(std::vector<uint32_t> roots, uint32_t components, uint32_t expansion = 0, KilnComponentWatchStatusV1* out = nullptr,
                   uint32_t scope = KILN_WATCH_SCOPE_ENTITIES, KilnOwner owner = 0) {
        KilnComponentWatchRequestV1 rq{};
        rq.size = sizeof rq;
        rq.scope = scope;
        rq.components = components;
        rq.expansion = expansion;
        rq.entities = roots.empty() ? nullptr : roots.data();
        rq.entity_count = static_cast<uint32_t>(roots.size());
        KilnComponentWatchStatusV1 st{};
        const KilnResult rc = c.create(owner ? owner : mod, rq, epoch, step, &st);
        if (out) *out = st;
        return rc == KILN_OK ? st.watch_id : ~0ull;
    }
    std::vector<KilnComponentChangeV1> drain() {
        uint64_t batch = 0;
        uint32_t n = 0;
        std::vector<KilnComponentChangeV1> out;
        if (c.begin_events(host, &batch, &n) != KILN_OK || batch == 0) return out;
        out.resize(n);
        uint32_t req = 0;
        if (c.read_events(host, batch, out.data(), n, sizeof(KilnComponentChangeV1), &req) != KILN_OK) out.clear();
        c.ack_events(host, batch);
        return out;
    }
    void write(uint32_t e, uint32_t comp, const std::vector<uint8_t>& before, const std::vector<uint8_t>& after, const char* source,
               uint64_t token = 0, uint64_t serial = 0, uint64_t ep = 0) {
        KilnKnownWriteV1 k{};
        k.size = sizeof k;
        k.component = comp;
        k.world_epoch = ep;
        k.operation_token = token;
        k.entity.entity = e;
        k.entity.life_serial = serial;
        k.before = before.data();
        k.after = after.data();
        k.bytes = static_cast<uint32_t>(before.size());
        k.source_mod = KilnStr{source, source ? std::strlen(source) : 0};
        c.record_write(mod, k, true);
    }
};

static uint64_t status_of(Run& r, uint64_t id, KilnComponentWatchStatusV1* st = nullptr) {
    KilnComponentWatchStatusV1 s{};
    if (r.c.status(id, &s) != KILN_OK) return 99;
    if (st) *st = s;
    return s.status;
}

// ---- requests and budgets ----
static void test_requests() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    CHECK(r.c.window_wanted() == 0);  // nothing watched: no window calls
    CHECK(r.watch({}, 1) == ~0ull);                     // Entities with no root
    CHECK(r.watch({2000}, 0) == ~0ull);                 // no component
    CHECK(r.watch({2000}, 64) == ~0ull);                // a component outside the six
    CHECK(r.watch({2000, 2000}, 1) == ~0ull);           // the same root twice
    CHECK(r.watch({0}, 1) == ~0ull);                    // entity 0
    CHECK(r.watch({2000}, 1, 2) == ~0ull);              // no such expansion
    CHECK(r.watch({2000}, 1, 0, nullptr, KILN_WATCH_SCOPE_PLAYERS) == ~0ull);  // Players with a list
    std::vector<uint32_t> many;
    for (uint32_t i = 0; i < 129; ++i) many.push_back(3000 + i);
    CHECK(r.watch(many, 1) == ~0ull);                   // 129 roots
    KilnComponentWatchStatusV1 st{};
    CHECK(r.watch({4444}, 1, 0, &st) == 0 && st.status == KILN_WATCH_SUSPENDED_METADATA);  // no such entity: refused, no handle
    r.w.put(4445, KILN_WATCH_HEALTH, bytes40(1));  // exists, no SpawnTime, not a player
    CHECK(r.watch({4445}, 1, 0, &st) == 0 && st.status == KILN_WATCH_SUSPENDED_METADATA && std::strstr(st.reason, "identity"));
    CHECK(r.c.window_wanted() == 0);
    const uint64_t id = r.watch({2000}, 1, 0, &st);
    CHECK(id != 0 && id != ~0ull && st.status == KILN_WATCH_READY && st.pairs == 1 && st.limit == 4096);
    CHECK(r.c.window_wanted() == 1);
    // 32 watches, then the 33rd is refused with a reason and no handle
    for (int i = 1; i < 32; ++i) CHECK(r.watch({2000}, 1) > 0);
    CHECK(r.watch({2000}, 1, 0, &st) == 0 && st.status == KILN_WATCH_SUSPENDED_BUDGET && std::strstr(st.reason, "32 watches"));
}

static void test_pair_budget() {
    Run r;
    // 4096 present pairs exactly: 1024 entities x 4 present components, plus absent ones that do not count
    std::vector<uint32_t> roots;
    for (uint32_t e = 10000; e < 10000 + 1024; ++e) {
        r.w.entity(e);
        for (uint32_t c = 1; c <= 4; ++c) r.w.put(e, c, bytes40(1));
        roots.push_back(e);
    }
    KilnComponentWatchStatusV1 st{};
    uint32_t total = 0;
    for (size_t at = 0; at < roots.size(); at += 128) {
        std::vector<uint32_t> part(roots.begin() + at, roots.begin() + at + 128);
        const uint64_t id = r.watch(part, KILN_WATCH_COMPONENTS_ALL, 0, &st);  // Inventory and SlotSelection absent
        CHECK(id > 0 && id != ~0ull);
        total += st.pairs;
    }
    CHECK(total == 4096);
    // overlap is free: the same pairs again
    CHECK(r.watch({10000}, 15, 0, &st) > 0);
    r.w.entity(20000);
    r.w.put(20000, KILN_WATCH_HEALTH, bytes40(1));
    CHECK(r.watch({20000}, 1, 0, &st) == 0 && st.status == KILN_WATCH_SUSPENDED_BUDGET && st.pairs == 1);  // one more: refused
    // an absent component costs nothing
    CHECK(r.watch({20000}, 2, 0, &st) > 0 && st.pairs == 0);
    r.tick();
    KilnComponentChangesCountersV1 k{};
    k.size = sizeof k;
    r.c.counters(&k);
    CHECK(k.present_pairs == 4096 && k.watches == 10);
}

// ---- compares ----
static void test_compare() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, KILN_WATCH_COMPONENTS_ALL);
    r.tick();  // baseline
    CHECK(r.drain().empty());
    r.tick([&] { r.w.set_byte(2000, KILN_WATCH_HEALTH, 8, 9); });
    auto ch = r.drain();
    CHECK(ch.size() == 1);
    if (ch.size() == 1) {
        CHECK(ch[0].component == KILN_WATCH_HEALTH && ch[0].kind == 1 && ch[0].version == 1 && ch[0].size == sizeof(KilnComponentChangeV1));
        CHECK(ch[0].before[8] == 1 && ch[0].after[8] == 9 && ch[0].before_bytes == 40 && ch[0].after_bytes == 40);
        CHECK(ch[0].flags == (KILN_CHANGE_BEFORE_PRESENT | KILN_CHANGE_AFTER_PRESENT));
        CHECK(ch[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN && ch[0].source_mod[0] == 0);  // no token: Unknown, never Game
        CHECK(ch[0].watch_count == 1 && ch[0].watches[0] == id && ch[0].root_count == 1 && ch[0].roots[0] == 2000);
        CHECK(ch[0].entity.entity == 2000 && ch[0].entity.flags == KILN_EVENT_KEY_SPAWN_TIME && ch[0].world_kind == 0);
        CHECK(ch[0].step == r.step && ch[0].since_step == r.step - 1 && ch[0].world_session == 7);
    }
    // A -> B -> A in one interval: nothing
    r.tick([&] {
        r.w.set_byte(2000, KILN_WATCH_HEALTH, 8, 3);
        r.w.set_byte(2000, KILN_WATCH_HEALTH, 8, 9);
    });
    CHECK(r.drain().empty());
    // absent -> present, present -> absent; absent on both sides: nothing
    r.tick([&] { r.w.put(2000, KILN_WATCH_MANA, bytes40(5)); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].component == KILN_WATCH_MANA && ch[0].flags == KILN_CHANGE_AFTER_PRESENT && ch[0].before_bytes == 0);
    r.tick([&] { r.w.comps.erase({2000, KILN_WATCH_MANA}); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].flags == KILN_CHANGE_BEFORE_PRESENT && ch[0].after_bytes == 0);
    // a change between two steps (Post or Pre: Kiln's tick-thread writers) belongs to the next interval
    r.w.set_byte(2000, KILN_WATCH_HEALTH, 9, 4);
    r.tick();
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].after[9] == 4);
    // too large a component is never present
    r.w.put(2000, KILN_WATCH_SLOT_SELECTION, std::vector<uint8_t>(300, 1));
    r.tick();
    CHECK(r.drain().empty());
}

// ---- tokens and origin ----
static void test_origin() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    r.watch({2000}, 1);
    r.tick();
    auto a = bytes40(1), b = bytes40(2), c = bytes40(3), d = bytes40(4);
    auto change = [&](const std::vector<uint8_t>& to) { r.w.put(2000, KILN_WATCH_HEALTH, to); };
    // one mod's full chain: Kiln, with its id and token
    r.tick([&] {
        change(b);
        r.write(2000, 1, a, b, "my-mod", 77);
    });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_KILN && std::strcmp(ch[0].source_mod, "my-mod") == 0 &&
          ch[0].token_count == 1 && ch[0].tokens[0] == 77);
    // a chain of two writes of the same mod, and a nested one recorded after the write inside it
    r.tick([&] {
        change(d);
        r.write(2000, 1, b, c, "my-mod");
        r.write(2000, 1, b, d, "my-mod");  // the outer write: from b to d, recorded last
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_KILN && ch[0].token_count == 2);
    // two mods: Mixed, both named
    r.tick([&] {
        change(b);
        r.write(2000, 1, d, c, "mod-a");
        r.write(2000, 1, c, b, "mod-b");
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_MIXED && std::strcmp(ch[0].source_mod, "mod-a, mod-b") == 0);
    // a gap: a token, then something without one changed it further
    r.tick([&] {
        change(a);
        r.write(2000, 1, b, c, "my-mod");
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_MIXED);
    // a token that does not start at the baseline (the game wrote first)
    r.tick([&] {
        change(c);
        r.write(2000, 1, b, c, "my-mod");
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_MIXED);
    // a source Kiln cannot vouch for: Unknown (empty, too long, not UTF-8)
    r.tick([&] {
        change(d);
        r.write(2000, 1, c, d, "");
    });
    CHECK(r.drain()[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
    std::string long_id(128, 'x');
    r.tick([&] {
        change(a);
        r.write(2000, 1, d, a, long_id.c_str());
    });
    CHECK(r.drain()[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
    r.tick([&] {
        change(b);
        r.write(2000, 1, a, b, "\xff\xfe");
    });
    CHECK(r.drain()[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
    // A -> B -> A with tokens: no record at all
    r.tick([&] {
        r.write(2000, 1, b, c, "my-mod");
        r.write(2000, 1, c, b, "my-mod");
    });
    CHECK(r.drain().empty());
    // a write between two steps (a decision's survival health in Pre) counts for the next interval
    change(c);
    r.write(2000, 1, b, c, "kiln.combat");
    r.tick();
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_KILN && std::strcmp(ch[0].source_mod, "kiln.combat") == 0);
    // more than 32 tokens: truncated and Mixed, the change itself still reported
    r.tick([&] {
        for (int i = 0; i < 40; ++i) r.write(2000, 1, c, c, "my-mod");
        change(d);
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_MIXED && (ch[0].flags & KILN_CHANGE_TOKENS_TRUNCATED) && ch[0].token_count == 32);
    // a token of another world epoch or another life is refused; one for an entity nobody watches is ignored
    KilnKnownWriteV1 k{};
    k.size = sizeof k;
    k.component = 1;
    k.entity.entity = 2000;
    k.world_epoch = r.epoch + 1;
    k.before = d.data();
    k.after = a.data();
    k.bytes = 40;
    k.source_mod = KilnStr{"my-mod", 6};
    CHECK(r.c.record_write(r.mod, k, true) == KILN_ERR_NOT_FOUND);
    k.world_epoch = 0;
    k.entity.life_serial = 999999;
    CHECK(r.c.record_write(r.mod, k, true) == KILN_ERR_NOT_FOUND);
    k.entity.life_serial = 0;
    k.entity.entity = 5555;
    CHECK(r.c.record_write(r.mod, k, true) == KILN_OK);
    k.entity.entity = 2000;
    k.bytes = 0;
    CHECK(r.c.record_write(r.mod, k, true) == KILN_ERR_INVALID_ARGUMENT);
    k.bytes = 40;
    k.component = 7;
    CHECK(r.c.record_write(r.mod, k, true) == KILN_ERR_INVALID_ARGUMENT);
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.tokens_refused == 2 && n.tokens_unwatched == 1 && n.tokens_truncated == 8);
    r.tick();
    CHECK(r.drain().empty());  // the refused tokens changed nothing
}

static void test_worker_tokens() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_FUEL, bytes40(1));
    r.watch({2000}, KILN_WATCH_COMPONENTS_ALL);
    r.tick();
    auto a = bytes40(1), b = bytes40(2);
    r.tick([&] {
        // a writer on a worker in the forced-parallel window (kiln.world's write program, a program wrap's stage)
        std::thread t([&] {
            r.w.put(2000, KILN_WATCH_FUEL, b);
            KilnKnownWriteV1 k{};
            k.size = sizeof k;
            k.component = KILN_WATCH_FUEL;
            k.entity.entity = 2000;
            k.before = a.data();
            k.after = b.data();
            k.bytes = 40;
            k.source_mod = KilnStr{"wrap-mod", 8};
            r.c.record_write(r.mod, k, false);
        });
        t.join();
    });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_KILN && std::strcmp(ch[0].source_mod, "wrap-mod") == 0);
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.tokens_off_world_thread == 1 && n.tokens_recorded == 1);
}

static void test_tokens_before_baseline() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    // a write recorded before any watch: not counted, and a new pair's baseline starts after it
    auto a = bytes40(1), b = bytes40(2), c = bytes40(3);
    r.write(2000, 1, a, b, "my-mod");
    r.w.put(2000, 1, b);
    r.watch({2000}, 1);
    r.write(2000, 1, b, c, "my-mod");  // made in the same Pre as the watch, before its baseline
    r.w.put(2000, 1, c);
    r.tick([&] { r.w.put(2000, 1, a); });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].before[0] == 3 && ch[0].after[0] == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
}

static void test_token_store_full() {
    Run r;
    std::vector<uint32_t> roots;
    for (uint32_t e = 3000; e < 3000 + 128; ++e) {
        r.w.entity(e);
        r.w.put(e, 1, bytes40(1));
        r.w.put(e, 2, bytes40(1));
        r.w.put(e, 3, bytes40(1));
        roots.push_back(e);
    }
    r.watch(roots, 7);
    r.tick();
    auto a = bytes40(1), b = bytes40(2);
    r.tick([&] {
        for (uint32_t e = 3000; e < 3000 + 128; ++e)
            for (uint32_t c = 1; c <= 3; ++c)
                for (int i = 0; i < 31; ++i) r.write(e, c, a, a, "my-mod");  // 11904 tokens: the store takes 8192
        r.w.put(3127, 3, b);
        r.write(3127, 3, a, b, "my-mod");
    });
    auto ch = r.drain();
    // every token of that pair was lost: the record says so and claims no origin
    CHECK(ch.size() == 1 && (ch[0].flags & KILN_CHANGE_TOKENS_LOST) && ch[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN && ch[0].token_count == 0);
    // a pair with tokens and a lost one: Mixed
    r.tick([&] {
        r.write(3000, 3, a, b, "my-mod");
        for (uint32_t e = 3000; e < 3000 + 128; ++e)
            for (uint32_t c = 1; c <= 2; ++c)
                for (int i = 0; i < 32; ++i) r.write(e, c, a, a, "my-mod");
        r.w.put(3000, 3, c_of(3));
        r.write(3000, 3, b, c_of(3), "my-mod");  // lost: the chain cannot be shown whole
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && (ch[0].flags & KILN_CHANGE_TOKENS_LOST) && ch[0].origin == KILN_CHANGE_ORIGIN_MIXED && ch[0].token_count == 1);
}

// ---- queue and batches ----
static void test_batches() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, 1);
    r.tick();
    r.tick([&] { r.w.set_byte(2000, 1, 0, 2); });
    uint64_t batch = 0, again = 0;
    uint32_t n = 0, m = 0, req = 0;
    CHECK(r.c.begin_events(r.mod, &batch, &n) == KILN_ERR_UNAVAILABLE);  // not the consumer
    CHECK(r.c.begin_events(r.host, &batch, &n) == KILN_OK && batch != 0 && n == 1);
    CHECK(r.c.begin_events(r.host, &again, &m) == KILN_OK && again == batch && m == 1);  // the same batch until acked
    CHECK(r.c.read_events(r.host, batch, nullptr, 0, sizeof(KilnComponentChangeV1), &req) == KILN_ERR_BUFFER_TOO_SMALL && req == 1);
    std::vector<KilnComponentChangeV1> one(1), two(1);
    CHECK(r.c.read_events(r.host, batch, one.data(), 1, 8, &req) == KILN_ERR_INVALID_ARGUMENT);  // stride below the record
    CHECK(r.c.read_events(r.host, batch, one.data(), 1, sizeof(KilnComponentChangeV1), &req) == KILN_OK);
    CHECK(r.c.read_events(r.host, batch, two.data(), 1, sizeof(KilnComponentChangeV1), &req) == KILN_OK);
    CHECK(std::memcmp(one.data(), two.data(), sizeof(KilnComponentChangeV1)) == 0);  // a read consumes nothing
    // a bigger stride (a newer consumer's record)
    std::vector<uint8_t> wide(4096, 0xCC);
    CHECK(r.c.read_events(r.host, batch, reinterpret_cast<KilnComponentChangeV1*>(wide.data()), 1, 4096, &req) == KILN_OK);
    // what arrives now belongs to the next batch
    r.tick([&] { r.w.set_byte(2000, 1, 0, 3); });
    CHECK(r.c.begin_events(r.host, &again, &m) == KILN_OK && again == batch && m == 1);
    CHECK(r.c.ack_events(r.host, batch) == KILN_OK && r.c.ack_events(r.host, batch) == KILN_OK);  // acked twice: fine
    CHECK(r.c.begin_events(r.host, &again, &m) == KILN_OK && again != batch && m == 1);
    CHECK(r.c.ack_events(r.host, again) == KILN_OK);
    (void)id;
}

static void test_queue_overflow() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, 1);
    r.tick();
    const uint64_t first = r.step;
    r.tick([&] { r.w.set_byte(2000, 1, 0, 2); });  // frame 1
    r.tick([&] { r.w.set_byte(2000, 1, 0, 3); });  // frame 2
    const uint64_t complete = r.step;
    r.tick([&] { r.w.set_byte(2000, 1, 0, 4); });  // a third: nobody read
    KilnComponentWatchStatusV1 st{};
    CHECK(status_of(r, id, &st) == KILN_WATCH_SUSPENDED_BUDGET && std::strstr(st.reason, "queue") && st.last_complete_step == complete);
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.queue_overflows == 1 && n.frames_dropped == 1 && n.queued_frames == 2);
    CHECK(r.drain().size() == 2);  // the two frames that fit are delivered
    // suspended: no more records, until resumed; resumed: a fresh baseline and no catch-up change
    r.tick([&] { r.w.set_byte(2000, 1, 0, 5); });
    CHECK(r.drain().empty());
    CHECK(r.c.resume(r.mod, id, r.step, &st) == KILN_OK && st.status == KILN_WATCH_READY);
    CHECK(r.c.resume(r.mod + 1, id, r.step, &st) == KILN_ERR_NOT_FOUND);  // not its owner
    r.tick();
    CHECK(r.drain().empty());
    r.tick([&] { r.w.set_byte(2000, 1, 0, 6); });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].before[0] == 5 && ch[0].after[0] == 6);
    (void)first;
}

// ---- lives, worlds, owners ----
static void test_lives() {
    Run r;
    r.w.entity(2000, 1000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, 1);
    r.tick();
    // the id is another entity now (another SpawnTime): the watch never follows it
    r.w.entity(2000, 2000);
    r.tick([&] { r.w.set_byte(2000, 1, 0, 9); });
    KilnComponentWatchStatusV1 st{};
    CHECK(status_of(r, id, &st) == KILN_WATCH_SUSPENDED_METADATA && std::strstr(st.reason, "another entity"));
    CHECK(r.drain().empty());
    CHECK(r.c.resume(r.mod, id, r.step, &st) == KILN_OK && st.status == KILN_WATCH_SUSPENDED_METADATA);  // still another life
    // gone: suspended; its last compare reported the component's end first
    Run g;
    g.w.entity(2000);
    g.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t gid = g.watch({2000}, 1);
    g.tick();
    g.tick([&] { g.w.gone.insert(2000); });
    auto ch = g.drain();
    CHECK(ch.size() == 1 && ch[0].flags == KILN_CHANGE_BEFORE_PRESENT);
    g.tick();
    CHECK(status_of(g, gid, &st) == KILN_WATCH_SUSPENDED_METADATA && std::strstr(st.reason, "gone"));
}

static void test_tree_exact_bounds() {
    World w;
    std::vector<uint32_t> leaves;
    std::string why;
    w.link(1, {{2, 5}}); w.link(2, {{3, 5}}); w.link(3, {{4, 5}}); w.link(4, {{5, 4}});
    CHECK(walk_tree(w, 1, &leaves, &why) && leaves.size() == 1); // depth exactly 3
    std::vector<std::pair<uint32_t, uint8_t>> root;
    uint32_t id = 100;
    for (uint32_t k = 0; k < 16; ++k) {
        root.push_back({10 + k, 5});
        std::vector<std::pair<uint32_t, uint8_t>> children;
        for (uint32_t j = 0; j < (k == 15 ? 2u : 3u); ++j) {
            children.push_back({id, 5}); w.link(id++, {});
        }
        w.link(10 + k, children);
    }
    w.link(1, root);
    CHECK(walk_tree(w, 1, &leaves, &why)); // 1 + 16 + 47 == 64 setups
    w.link(25, {{145, 5}, {146, 5}, {147, 5}}); w.link(147, {});
    CHECK(!walk_tree(w, 1, &leaves, &why) && why.find("64") != std::string::npos);
    std::vector<std::pair<uint32_t, uint8_t>> a, b;
    for (uint32_t k = 0; k < 16; ++k) { a.push_back({200 + k, 4}); b.push_back({300 + k, 4}); }
    w.link(1, {{2, 5}, {3, 5}}); w.link(2, a); w.link(3, b);
    CHECK(walk_tree(w, 1, &leaves, &why) && leaves.size() == 32);
}

static void test_invalid_unicode_sources() {
    for (const char* source : {"\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80"}) {
        Run r;
        r.w.entity(2000); r.w.put(2000, 1, bytes40(1));
        r.watch({2000}, 1); r.tick();
        r.tick([&] { r.write(2000, 1, bytes40(1), bytes40(2), source); r.w.put(2000, 1, bytes40(2)); });
        auto ch = r.drain();
        CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
    }
}

static void test_link_reentry() {
    Run r;
    r.w.entity(5);
    r.w.link(5, {{7001, 4}});
    r.w.stacks(7001, {{13, 1, 8001}});
    r.w.put(8001, 4, bytes40(1));
    r.watch({5}, bit(4), 1);
    r.tick();
    r.tick([&] { r.w.put(8001, 4, bytes40(2)); });
    auto first = r.drain();
    r.w.stacks(7001, {});
    r.tick(); // observed outside every tree
    r.w.stacks(7001, {{13, 1, 8001}});
    r.tick();
    r.tick([&] { r.w.put(8001, 4, bytes40(3)); });
    auto again = r.drain();
    CHECK(first.size() == 1 && again.size() == 1 && first[0].entity.life_serial != again[0].entity.life_serial);
}

static void test_observation_limits() {
    Run r;
    r.w.entity(2000);
    auto a = bytes40(1), b = bytes40(2), c = bytes40(3);
    r.w.put(2000, 1, a);
    r.watch({2000}, 1);
    r.tick();
    // Names are owned by the token even when the writer unloads before delivery.
    r.tick([&] {
        std::string name = "unloaded-mod";
        r.write(2000, 1, a, b, name.c_str());
        name.assign("xxxxxxxxxxxx");
        // An unreported writer overwrites b with c and restores b: net attribution only.
        r.w.put(2000, 1, c);
        r.w.put(2000, 1, b);
    });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_KILN &&
          std::strcmp(ch[0].source_mod, "unloaded-mod") == 0);
    // A rolled back operation has no net bytes and must not invent an event.
    r.tick([&] { r.w.put(2000, 1, c); r.w.put(2000, 1, b); });
    CHECK(r.drain().empty());
    // The next unrelated change cannot inherit that operation's attribution.
    r.tick([&] { r.w.put(2000, 1, a); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].origin == KILN_CHANGE_ORIGIN_UNKNOWN);
}

static void test_reuse_during_sim() {
    Run r;
    r.w.entity(2000, 1000);
    r.w.put(2000, 1, bytes40(1));
    const auto id = r.watch({2000}, 1);
    r.tick();
    r.tick([&] { r.w.entity(2000, 2000); r.w.put(2000, 1, bytes40(9)); });
    CHECK(r.drain().empty());
    CHECK(status_of(r, id) == KILN_WATCH_SUSPENDED_METADATA);
}

static void test_world_stop() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, 1);
    r.tick();
    r.tick([&] { r.w.set_byte(2000, 1, 0, 2); });
    r.c.world_stopped("the server world stopped");
    KilnComponentWatchStatusV1 st{};
    CHECK(status_of(r, id, &st) == KILN_WATCH_DISPOSED && std::strstr(st.reason, "stopped"));
    CHECK(r.c.window_wanted() == 0);
    CHECK(r.drain().empty());  // the old world's records are not delivered into the next one
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.frames_dropped == 1);
    // a resume of a watch of the stopped world: Disposed, with the reason
    CHECK(r.c.resume(r.mod, id, r.step, &st) == KILN_OK && st.status == KILN_WATCH_DISPOSED);
    // the next world: the same entity ids mean nothing to the old watch
    r.epoch = 2;
    r.w.sess = 8;
    r.tick([&] { r.w.set_byte(2000, 1, 0, 3); });
    CHECK(r.drain().empty());
    // a watch made while the session already moved on (no stop seen): ended at the next window
    Run s;
    s.w.entity(2000);
    s.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t sid = s.watch({2000}, 1);
    s.w.sess = 9;
    s.tick();
    CHECK(status_of(s, sid) == KILN_WATCH_DISPOSED);
}

static void test_owners() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t a = r.watch({2000}, 1);
    const uint64_t b = r.watch({2000}, 1, 0, nullptr, KILN_WATCH_SCOPE_ENTITIES, 202);
    CHECK(r.c.remove(202, a) == KILN_ERR_NOT_FOUND);  // not its watch
    r.w.dead.insert(r.mod);
    r.tick();
    CHECK(status_of(r, a) == KILN_WATCH_DISPOSED && status_of(r, b) == KILN_WATCH_READY);
    CHECK(r.c.release_owner(202) == 1 && status_of(r, b) == KILN_WATCH_DISPOSED);
    CHECK(r.c.window_wanted() == 0);
}

static void test_players() {
    Run r;
    r.w.entity(1);
    r.w.put(1, KILN_WATCH_HEALTH, bytes40(1));
    r.w.entity(2);
    r.w.put(2, KILN_WATCH_HEALTH, bytes40(1));
    r.w.in_game = {1};
    KilnComponentWatchStatusV1 st{};
    const uint64_t id = r.watch({}, 1, 0, &st, KILN_WATCH_SCOPE_PLAYERS);
    CHECK(id > 0 && st.pairs == 1);
    r.tick();
    // a player joins: watched from its first window, no change for joining
    r.w.in_game = {1, 2};
    r.tick();
    CHECK(r.drain().empty() && status_of(r, id, &st) == KILN_WATCH_READY && st.pairs == 2);
    r.tick([&] { r.w.set_byte(2, 1, 0, 7); });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].entity.entity == 2 && ch[0].roots[0] == 2);
    // leaves: no more records for it
    r.w.in_game = {1};
    r.tick([&] { r.w.set_byte(2, 1, 0, 8); });
    CHECK(r.drain().empty());
    // a player id that comes back as another session is another life: a new baseline, nothing carried over
    r.w.entity(2, 5000);
    r.w.in_game = {1, 2};
    r.tick();
    CHECK(r.drain().empty());
}

// ---- links ----
static void test_links() {
    Run r;
    const uint32_t P = 5, A = 7001, B = 7002, V = 7100, C = 7003;
    r.w.entity(P);
    r.w.put(P, KILN_WATCH_HEALTH, bytes40(1));
    r.w.link(P, {{A, 2}, {V, 5}});   // equipment, then a Virtual setup
    r.w.link(V, {{B, 4}, {C, 4}});
    r.w.stacks(A, {{11, 1, 8001}, {12, 3, 0xFFFFFFFFu}});  // a torch (item entity 8001), a stack without one
    r.w.stacks(B, {{13, 1, 8002}});
    r.w.stacks(C, {});
    r.w.put(8001, KILN_WATCH_FUEL, bytes40(64));
    r.w.put(8002, KILN_WATCH_FUEL, bytes40(64));
    KilnComponentWatchStatusV1 st{};
    const uint64_t id = r.watch({P}, KILN_WATCH_COMPONENTS_ALL, KILN_WATCH_EXPAND_INVENTORY, &st);
    CHECK(id > 0 && st.pairs == 6);  // Health on P; Inventory on A, B, C; Fuel on 8001, 8002
    r.tick();
    // a linked child changes: its record names the root, so a filter on the root sees it
    r.tick([&] { r.w.set_byte(8002, KILN_WATCH_FUEL, 8, 50); });
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].entity.entity == 8002 && ch[0].root_count == 1 && ch[0].roots[0] == P && ch[0].entity.flags == 0 &&
          (ch[0].entity.life_serial & kLinkedSerialBit));
    const uint64_t old_serial = ch.empty() ? 0 : ch[0].entity.life_serial;
    // the item entity in the slot is replaced (another pide): the old one is no longer watched, the new one starts
    r.w.put(8003, KILN_WATCH_FUEL, bytes40(64));
    r.tick([&] { r.w.stacks(B, {{13, 1, 8003}}); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].component == KILN_WATCH_INVENTORY && ch[0].entity.entity == B);
    r.tick([&] {
        r.w.set_byte(8002, KILN_WATCH_FUEL, 8, 40);  // the old item: not watched any more
        r.w.set_byte(8003, KILN_WATCH_FUEL, 8, 41);
    });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].entity.entity == 8003);
    // the old item entity's id back in the tree holding another item: a new life, not the old watch's
    r.tick([&] { r.w.stacks(B, {{14, 1, 8002}}); });
    r.drain();
    r.tick([&] { r.w.set_byte(8002, KILN_WATCH_FUEL, 8, 39); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].entity.entity == 8002 && ch[0].entity.life_serial != old_serial);
    // a cycle: suspended whole, never in part
    r.w.link(V, {{B, 4}, {P, 5}});
    r.tick();
    CHECK(status_of(r, id, &st) == KILN_WATCH_SUSPENDED_METADATA && std::strstr(st.reason, "cycle"));
    // deeper than 3, a missing setup, 33 inventories: refused at the watch
    Run d;
    d.w.entity(5);
    d.w.link(5, {{6001, 5}});
    d.w.link(6001, {{6002, 5}});
    d.w.link(6002, {{6003, 5}});
    d.w.link(6003, {{6004, 5}});
    d.w.link(6004, {{6005, 4}});
    CHECK(d.watch({5}, 16, 1, &st) == 0 && std::strstr(st.reason, "deeper"));
    d.w.link(5, {{6100, 5}});
    CHECK(d.watch({5}, 16, 1, &st) == 0 && std::strstr(st.reason, "missing"));
    std::vector<std::pair<uint32_t, uint8_t>> sixteen;
    for (uint32_t i = 0; i < 16; ++i) sixteen.push_back({6200 + i, 4});
    std::vector<std::pair<uint32_t, uint8_t>> more;
    for (uint32_t i = 0; i < 16; ++i) more.push_back({6300 + i, 4});
    d.w.link(5, {{6400, 5}, {6401, 5}, {6402, 5}});
    d.w.link(6400, sixteen);
    d.w.link(6401, more);
    d.w.link(6402, {{6500, 4}});
    CHECK(d.watch({5}, 16, 1, &st) == 0 && std::strstr(st.reason, "32"));
    // no expansion: only the root
    CHECK(d.watch({5}, 16, 0, &st) > 0 && st.pairs == 0);
}

static void test_dynamic_budget() {
    Run r;
    // a watch near the budget; a link grows its tree past 4096: suspended whole before that step's compare
    std::vector<uint32_t> roots;
    for (uint32_t e = 10000; e < 10000 + 1023; ++e) {
        r.w.entity(e);
        for (uint32_t c = 1; c <= 4; ++c) r.w.put(e, c, bytes40(1));
    }
    for (size_t at = 0; at < 1023; at += 128) {
        std::vector<uint32_t> part;
        for (uint32_t e = 10000 + static_cast<uint32_t>(at); e < 10000 + std::min<uint32_t>(1023, static_cast<uint32_t>(at) + 128); ++e) part.push_back(e);
        CHECK(r.watch(part, 15) > 0);
    }
    const uint32_t P = 5;
    r.w.entity(P);
    r.w.put(P, KILN_WATCH_HEALTH, bytes40(1));
    r.w.link(P, {{7001, 4}});
    r.w.stacks(7001, {});
    KilnComponentWatchStatusV1 st{};
    const uint64_t id = r.watch({P}, KILN_WATCH_HEALTH | bit(KILN_WATCH_INVENTORY), 1, &st);
    CHECK(id > 0 && st.pairs == 2);  // 4092 + 2 = 4094
    r.tick();
    const uint64_t complete = r.step;
    r.w.link(P, {{7001, 4}, {7002, 4}, {7003, 4}, {7004, 4}});  // 4092 + 1 + 4 = 4097
    r.w.stacks(7002, {});
    r.w.stacks(7003, {});
    r.w.stacks(7004, {});
    r.tick([&] { r.w.set_byte(P, 1, 0, 2); });
    CHECK(status_of(r, id, &st) == KILN_WATCH_SUSPENDED_BUDGET && std::strstr(st.reason, "4096") && st.last_complete_step == complete);
    CHECK(r.drain().empty());  // nothing of it is compared once suspended
    // resume while it still does not fit: stays suspended, says why
    CHECK(r.c.resume(r.mod, id, r.step, &st) == KILN_OK && st.status == KILN_WATCH_SUSPENDED_BUDGET);
    r.w.link(P, {{7001, 4}});
    CHECK(r.c.resume(r.mod, id, r.step, &st) == KILN_OK && st.status == KILN_WATCH_READY);
}

// steady state (no replan): a component that appears still counts against the budget; the last watch ends: nothing kept
static void test_steady() {
    Run r;
    for (uint32_t e = 10000; e < 10000 + 1024; ++e) {
        r.w.entity(e);
        for (uint32_t c = 1; c <= 4; ++c) r.w.put(e, c, bytes40(1));
    }
    std::vector<uint64_t> ids;
    for (uint32_t at = 0; at < 1024; at += 128) {
        std::vector<uint32_t> part;
        for (uint32_t e = 10000 + at; e < 10000 + at + 128; ++e) part.push_back(e);
        ids.push_back(r.watch(part, 15));  // 512 present pairs each: 4096
    }
    r.w.entity(20000);
    const uint64_t late = r.watch({20000}, bit(KILN_WATCH_MANA));  // absent: free
    CHECK(late > 0 && late != ~0ull);
    r.tick();
    r.tick();  // steady
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.present_pairs == 4096);
    r.tick([&] { r.w.put(20000, KILN_WATCH_MANA, bytes40(2)); });  // 4097 present: suspend before reporting
    CHECK(r.drain().empty());  // growth is refused before this compare, not the following one
    CHECK(status_of(r, late) == KILN_WATCH_SUSPENDED_BUDGET);
    r.tick();
    KilnComponentWatchStatusV1 st{};
    CHECK(status_of(r, late, &st) == KILN_WATCH_SUSPENDED_BUDGET && std::strstr(st.reason, "4096"));
    CHECK(status_of(r, ids[0]) == KILN_WATCH_READY);
    for (uint64_t id : ids) r.c.remove(r.mod, id);
    r.c.remove(r.mod, late);
    n = KilnComponentChangesCountersV1{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.watches == 0 && n.pairs == 0 && n.present_pairs == 0 && r.c.window_wanted() == 0);
}

static void test_split() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t a = r.watch({2000}, 1);
    r.tick();
    // in Pre: the component changes, then a second watch is made on it
    r.w.set_byte(2000, 1, 0, 2);
    const uint64_t b = r.watch({2000}, 1);
    r.tick();
    auto ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].watch_count == 1 && ch[0].watches[0] == a && ch[0].after[0] == 2);  // only the old watch
    r.tick([&] { r.w.set_byte(2000, 1, 0, 3); });
    ch = r.drain();
    CHECK(ch.size() == 1 && ch[0].watch_count == 2 && ch[0].before[0] == 2);  // both, one record: overlap is not duplicated
    (void)b;
}

static void test_typed() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    r.w.typed_values[{2000, 1}] = {100, 120};
    r.w.put(2000, KILN_WATCH_STAMINA, bytes40(1));  // no typed values known for Stamina here
    r.watch({2000}, 3);
    r.tick();
    r.tick([&] {
        r.w.set_byte(2000, 1, 9, 2);
        r.w.typed_values[{2000, 1}] = {80, 120};
        r.w.set_byte(2000, 2, 9, 2);
    });
    auto ch = r.drain();
    CHECK(ch.size() == 2);
    for (const auto& c : ch) {
        if (c.component == 1) CHECK(c.attribute == KILN_CHANGE_ATTRIBUTE_HEALTH && c.value_before == 100 && c.value_after == 80 && c.maximum_before == 120 && c.maximum_after == 120);
        else CHECK(c.attribute == KILN_CHANGE_ATTRIBUTE_NONE);
    }
}

static void test_stop() {
    Run r;
    r.w.entity(2000);
    r.w.put(2000, KILN_WATCH_HEALTH, bytes40(1));
    const uint64_t id = r.watch({2000}, 1);
    r.tick();
    r.c.stop("a window call faulted");
    CHECK(r.c.stopped() && r.c.window_wanted() == 0 && status_of(r, id) == KILN_WATCH_DISPOSED);
    KilnComponentWatchStatusV1 st{};
    CHECK(r.watch({2000}, 1, 0, &st) == 0 && std::strstr(st.reason, "fault"));
    auto a = bytes40(1);
    r.write(2000, 1, a, a, "my-mod");  // returns at once
    KilnComponentChangesCountersV1 n{};
    n.size = sizeof n;
    r.c.counters(&n);
    CHECK(n.state == 2 && n.tokens_recorded == 0);
}

static void test_origin_rule() {
    // the pure rule: absent states never connect
    std::vector<Token> t(1);
    t[0].valid = true;
    t[0].source = "m";
    t[0].bytes = 4;
    uint8_t base[4] = {1, 1, 1, 1}, fin[4] = {2, 2, 2, 2};
    std::memcpy(t[0].before.data(), base, 4);
    std::memcpy(t[0].after.data(), fin, 4);
    CHECK(origin_of(base, 4, true, fin, 4, true, t, false, false).origin == KILN_CHANGE_ORIGIN_KILN);
    CHECK(origin_of(base, 4, false, fin, 4, true, t, false, false).origin == KILN_CHANGE_ORIGIN_MIXED);  // was absent
    CHECK(origin_of(base, 4, true, fin, 4, false, t, false, false).origin == KILN_CHANGE_ORIGIN_MIXED);  // is gone
    CHECK(origin_of(base, 4, true, fin, 4, true, t, true, false).origin == KILN_CHANGE_ORIGIN_MIXED);   // truncated
    CHECK(origin_of(base, 4, true, fin, 4, true, t, false, true).origin == KILN_CHANGE_ORIGIN_MIXED);   // a token lost
    CHECK(origin_of(base, 4, true, fin, 4, true, {}, false, false).origin == KILN_CHANGE_ORIGIN_UNKNOWN);
}

int main() {
    test_requests();
    test_pair_budget();
    test_compare();
    test_origin();
    test_worker_tokens();
    test_tokens_before_baseline();
    test_token_store_full();
    test_batches();
    test_queue_overflow();
    test_lives();
    test_reuse_during_sim();
    test_observation_limits();
    test_link_reentry();
    test_invalid_unicode_sources();
    test_tree_exact_bounds();
    test_world_stop();
    test_owners();
    test_players();
    test_links();
    test_dynamic_budget();
    test_split();
    test_steady();
    test_typed();
    test_stop();
    test_origin_rule();
    std::printf("watch_test: %d PASS, %d FAIL\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
