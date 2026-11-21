// SPDX-License-Identifier: MIT
// kiln.markers S3 on the host (no game, no Wine): the anchors' watch table on a fake world and storage, the explicit-world
// reader and its layout check against a fake kiln.types, and the local store following an entity. Chapter 04 §6 M05/M06
// mechanics: serials and leases, same-id recreation, a missing transform and its recovery, destroy terminal, the quotas,
// the world epoch, and the observers on several threads against a reader and a capturer.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "anchors.h"
#include "check.h"
#include "services/kiln_markers_lab.h"
#include "store.h"
#include "world_read.h"

using namespace kmk;

namespace {

constexpr uintptr_t kServerWorld = 0x10000, kClientWorld = 0x20000, kStorageOffset = 0xe179c0;
constexpr uintptr_t kServerStorage = kServerWorld + kStorageOffset, kClientStorage = kClientWorld + kStorageOffset;
constexpr uintptr_t kPredictionStorage = 0x30000 + kStorageOffset;

int64_t fx(double m) { return static_cast<int64_t>(m * 4294967296.0); }

// A fake world pair: per (world, entity) a position (present = it has a transform), faults on demand.
struct FakeWorlds {
    std::mutex mu;
    std::map<std::pair<uintptr_t, uint32_t>, std::array<int64_t, 3>> pos;
    std::set<uint32_t> faulting;
    uint32_t wrong_size = 0;  // nonzero: answer this size instead
    std::atomic<int> reads{0};
    void put(uintptr_t world, uint32_t e, double x, double y, double z) {
        std::lock_guard g(mu);
        pos[{world, e}] = {fx(x), fx(y), fx(z)};
    }
    void erase(uintptr_t world, uint32_t e) {
        std::lock_guard g(mu);
        pos.erase({world, e});
    }
};
FakeWorlds* g_fw = nullptr;

ComponentSlice* fake_get(ComponentSlice* out, uintptr_t world, uint32_t hash, uint32_t entity) {
    thread_local uint8_t bytes[176];
    g_fw->reads.fetch_add(1);
    out->data = nullptr;
    out->size = 0;
    std::lock_guard g(g_fw->mu);
    if (g_fw->faulting.count(entity)) throw 1;  // the host build's stand-in for an access violation
    auto it = g_fw->pos.find({world, entity});
    if (it == g_fw->pos.end()) return out;
    std::memset(bytes, 0, sizeof bytes);
    std::memcpy(bytes, it->second.data(), 24);
    out->data = bytes;
    out->size = g_fw->wrong_size ? g_fw->wrong_size : hash == kClientTransform.hash ? 176 : 56;
    return out;
}

TransformRead fake_read(uint32_t kind, uint32_t entity, void*) {
    return read_transform(&fake_get, kind == KILN_WORLD_CLIENT ? kClientWorld : kServerWorld, kind, entity);
}

KilnMarkerAnchorResult cap(Watches& w, KilnOwner owner, uint32_t kind, uint64_t epoch, uint32_t entity, uint64_t current = 0) {
    KilnMarkerAnchorResult r{};
    r.size = sizeof r;
    capture_into(w, owner, kind, epoch, current ? current : epoch, entity, &fake_read, nullptr, &r);
    return r;
}

uint32_t reason_of(const Watches& w, uint64_t serial) {
    uint32_t reason = 999;
    if (!w.follow(serial, nullptr, &reason)) return 1000;  // gone
    return reason;
}

// One Post's follow reads, as anchors_post does them.
void post(Watches& w, uint32_t kind) {
    std::vector<Watches::ToRead> b(kWatchSlots);
    const uint32_t n = w.to_read(kind, b.data(), kWatchSlots);
    for (uint32_t i = 0; i < n; ++i) w.read_done(b[i], fake_read(kind, b[i].entity, nullptr));
}

struct World {
    std::unique_ptr<FakeWorlds> fw = std::make_unique<FakeWorlds>();
    std::unique_ptr<Watches> w = std::make_unique<Watches>();
    uint64_t server_epoch = 1, client_epoch = 2;
    World() {
        g_fw = fw.get();
        w->world_started(KILN_WORLD_SERVER, kServerStorage, server_epoch);
        w->world_started(KILN_WORLD_CLIENT, kClientStorage, client_epoch);
    }
};

// ---- cases ------------------------------------------------------------------------------------------------------------

void serials_and_leases() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 2022, 3670, 820, 1500);
    h.fw->put(kServerWorld, 2023, 100, 50, 200);
    auto a = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022);
    CHECK_EQ(a.status, KILN_MARKER_ACCEPTED);
    CHECK(a.anchor != 0);
    CHECK_EQ(a.incarnation, 1u);
    CHECK_EQ(a.world_epoch, 1u);
    CHECK_EQ(a.entity, 2022u);
    CHECK(a.position.x == 3670 && a.position.y == 820 && a.position.z == 1500);
    CHECK(a.reason[0] == 0);
    auto b = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022);  // the same incarnation: the same serial, another lease
    CHECK_EQ(b.incarnation, a.incarnation);
    CHECK(b.anchor > a.anchor);
    auto c = cap(*h.w, 8, KILN_WORLD_SERVER, 1, 2023);
    CHECK_EQ(c.incarnation, 2u);
    CHECK(c.anchor > b.anchor);
    KilnVec3 p{};
    uint32_t reason = 99;
    CHECK(h.w->follow(a.incarnation, &p, &reason));
    CHECK_EQ(reason, KILN_MARKER_READY);
    CHECK(p.x == 3670);
    // leases are owner-bound; release is idempotent per handle
    CHECK_EQ(h.w->release(8, a.anchor), KILN_ERR_NOT_FOUND);
    CHECK_EQ(h.w->release(7, a.anchor), KILN_OK);
    CHECK_EQ(h.w->release(7, a.anchor), KILN_ERR_NOT_FOUND);
    CHECK(h.w->follow(a.incarnation, nullptr, nullptr));  // b still holds it
    CHECK_EQ(h.w->release(7, b.anchor), KILN_OK);
    CHECK(!h.w->follow(a.incarnation, nullptr, nullptr));  // no lease, no reference: gone
    CHECK(!h.w->follow(0, nullptr, nullptr));
    auto d = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022);  // watched again: a new serial, never a reused one
    CHECK_EQ(d.incarnation, 3u);
    CHECK(d.anchor > c.anchor);
    h.w->release_owner(7);
    h.w->release_owner(8);
    const auto n = h.w->counts();
    CHECK_EQ(n.leases, 0u);
    CHECK_EQ(n.slots, 0u);
    CHECK_EQ(n.live, 0u);
    CHECK_EQ(n.serials, 3u);
    kmk_test::end("M05 mechanics: serials per incarnation, owner-bound leases, handles and serials never reused");
}

void epoch_first() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 2022, 3670, 820, 1500);
    const int before = h.fw->reads.load();
    auto stale = cap(*h.w, 7, KILN_WORLD_SERVER, 9, 2022, 1);  // expected 9, the world is at 1
    CHECK_EQ(stale.status, KILN_MARKER_NOT_READY);
    CHECK(std::strncmp(stale.reason, "NotReady", 8) == 0);
    auto none = cap(*h.w, 7, KILN_WORLD_SERVER, 0, 2022, 0);
    CHECK_EQ(none.status, KILN_MARKER_NOT_READY);
    CHECK_EQ(h.fw->reads.load(), before);  // no read before the epoch is right
    CHECK_EQ(stale.anchor, 0u);
    // the service's epoch and the anchors' snapshot must agree too
    Watches::Pending p;
    char why[200];
    CHECK_EQ(h.w->begin(7, KILN_WORLD_CLIENT, 1, 2022, &p, why, sizeof why), KILN_MARKER_NOT_READY);
    CHECK_EQ(h.w->begin(7, KILN_WORLD_PREDICTION, 1, 2022, &p, why, sizeof why), KILN_MARKER_NOT_READY);
    CHECK_EQ(h.w->counts().slots, 0u);
    kmk_test::end("capture: an epoch that is not the world's now is NotReady before any read");
}

void not_found_no_lease() {
    kmk_test::begin();
    World h;
    auto missing = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 4000);
    CHECK_EQ(missing.status, KILN_MARKER_NOT_FOUND);
    CHECK_EQ(missing.anchor, 0u);
    CHECK(std::strncmp(missing.reason, "NotFound", 8) == 0);
    h.fw->put(kServerWorld, 4001, 20000, 0, 0);  // outside the world
    CHECK_EQ(cap(*h.w, 7, KILN_WORLD_SERVER, 1, 4001).status, KILN_MARKER_NOT_FOUND);
    h.fw->put(kServerWorld, 4002, 10, 10, 10);
    h.fw->faulting.insert(4002);
    auto fault = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 4002);
    CHECK_EQ(fault.status, KILN_MARKER_NOT_FOUND);
    CHECK(std::strstr(fault.reason, "faulted") != nullptr);
    h.fw->faulting.clear();
    h.fw->wrong_size = 64;
    CHECK_EQ(cap(*h.w, 7, KILN_WORLD_SERVER, 1, 4002).status, KILN_MARKER_NOT_FOUND);
    h.fw->wrong_size = 0;
    const auto n = h.w->counts();
    CHECK_EQ(n.leases, 0u);
    CHECK_EQ(n.slots, 0u);  // the fresh slots went again
    CHECK_EQ(n.live, 0u);
    // a client entity is read from the client world's render transform (176 bytes)
    h.fw->put(kClientWorld, 4003, 1, 2, 3);
    auto client = cap(*h.w, 7, KILN_WORLD_CLIENT, 2, 4003);
    CHECK_EQ(client.status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(client.world_kind, static_cast<uint32_t>(KILN_WORLD_CLIENT));
    CHECK_EQ(cap(*h.w, 7, KILN_WORLD_SERVER, 1, 4003).status, KILN_MARKER_NOT_FOUND);  // not in the server world
    kmk_test::end("capture: no entity, no transform, a bad position or a faulted read is NotFound with no lease");
}

void destroy_and_same_id() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 2022, 3670, 820, 1500);
    auto a = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022);
    uint64_t serial = 0;
    char why[256];
    CHECK_EQ(h.w->retain(7, KILN_WORLD_SERVER, a.anchor, &serial, why, sizeof why), KILN_MARKER_ACCEPTED);  // a record's reference
    CHECK_EQ(serial, a.incarnation);
    // structural attempts elsewhere change nothing
    h.w->structural(kServerStorage, 2023);
    h.w->structural(kClientStorage, 2022);
    h.w->structural(kPredictionStorage, 2022);
    h.w->structural(kServerStorage, 2022 + 65536);  // the same low bits: the lock-free test lets it through, the id does not match
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_READY);
    // destroy: ended for good, before the game reuses anything
    h.w->structural(kServerStorage, 2022);
    h.fw->erase(kServerWorld, 2022);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_ANCHOR_ENDED);
    post(*h.w, KILN_WORLD_SERVER);  // an ended incarnation is not read again
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_ANCHOR_ENDED);
    CHECK_EQ(cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022).status, KILN_MARKER_NOT_FOUND);  // no transform after the destroy
    // same-id recreation without SpawnTime: create with the explicit id (before) and its return (after)
    h.w->structural(kServerStorage, 2022);
    h.fw->put(kServerWorld, 2022, 3670, 820, 1500);
    h.w->structural(kServerStorage, 2022);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_ANCHOR_ENDED);  // the old incarnation stays ended
    auto b = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 2022);
    CHECK_EQ(b.status, KILN_MARKER_ACCEPTED);
    CHECK(b.incarnation != a.incarnation && b.incarnation > a.incarnation);
    // the old lease cannot be stored again, the new one can
    uint64_t s2 = 0;
    CHECK_EQ(h.w->retain(7, KILN_WORLD_SERVER, a.anchor, &s2, why, sizeof why), KILN_MARKER_INVALID_ANCHOR);
    CHECK(std::strstr(why, "capture it again") != nullptr);
    CHECK_EQ(h.w->retain(7, KILN_WORLD_SERVER, b.anchor, &s2, why, sizeof why), KILN_MARKER_ACCEPTED);
    CHECK_EQ(s2, b.incarnation);
    // a create that allocates (requested 0) and returns a watched id ends it too
    h.w->structural(kServerStorage, 2022);
    CHECK_EQ(reason_of(*h.w, b.incarnation), KILN_MARKER_ANCHOR_ENDED);
    h.w->drop(serial);
    h.w->drop(s2);
    h.w->release_owner(7);
    CHECK_EQ(h.w->counts().slots, 0u);
    kmk_test::end("M06 mechanics: destroy ends terminally; the same id recreated without SpawnTime is a new incarnation");
}

void missing_then_recovery() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 3000, 10, 20, 30);
    auto a = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 3000);
    uint64_t serial = 0;
    char why[256];
    // a lease alone is not read at Post: only what a record references
    std::vector<Watches::ToRead> b(8);
    CHECK_EQ(h.w->to_read(KILN_WORLD_SERVER, b.data(), 8), 0u);
    CHECK_EQ(h.w->retain(7, KILN_WORLD_SERVER, a.anchor, &serial, why, sizeof why), KILN_MARKER_ACCEPTED);
    CHECK_EQ(h.w->to_read(KILN_WORLD_SERVER, b.data(), 8), 1u);
    h.fw->put(kServerWorld, 3000, 11, 21, 31);  // it moves
    post(*h.w, KILN_WORLD_SERVER);
    KilnVec3 p{};
    uint32_t reason = 0;
    CHECK(h.w->follow(serial, &p, &reason));
    CHECK_EQ(reason, KILN_MARKER_READY);
    CHECK(p.x == 11 && p.y == 21 && p.z == 31);
    h.fw->erase(kServerWorld, 3000);  // the transform goes without a structural attempt (not replicated, a component removed)
    post(*h.w, KILN_WORLD_SERVER);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_FOLLOW_MISSING);
    h.fw->put(kServerWorld, 3000, 12, 22, 32);
    post(*h.w, KILN_WORLD_SERVER);
    CHECK(h.w->follow(serial, &p, &reason));
    CHECK_EQ(reason, KILN_MARKER_READY);  // the same serial recovers
    CHECK(p.x == 12);
    h.fw->put(kServerWorld, 3000, 12, 9000, 32);  // read, but outside the world
    post(*h.w, KILN_WORLD_SERVER);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_BAD_POSITION);
    h.fw->put(kServerWorld, 3000, 13, 23, 33);
    post(*h.w, KILN_WORLD_SERVER);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_READY);
    // dropping the lease keeps the record's reference (and the follow)
    CHECK_EQ(h.w->release(7, a.anchor), KILN_OK);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_READY);
    h.w->drop(serial);
    CHECK_EQ(reason_of(*h.w, serial), 1000u);
    kmk_test::end("FollowMissing without a structural attempt recovers with the same serial; BadPosition likewise");
}

void client_paths() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 50, 1, 1, 1);
    h.fw->put(kClientWorld, 50, 1, 1, 1);
    h.fw->put(kClientWorld, 51, 2, 2, 2);
    auto s = cap(*h.w, 7, KILN_WORLD_SERVER, 1, 50);
    auto c = cap(*h.w, 7, KILN_WORLD_CLIENT, 2, 50);
    auto d = cap(*h.w, 7, KILN_WORLD_CLIENT, 2, 51);
    CHECK(s.incarnation != c.incarnation);  // one id in two worlds: two incarnations
    h.w->client_create(50);                 // the network create path, before the game creates or skips it
    CHECK_EQ(reason_of(*h.w, c.incarnation), KILN_MARKER_ANCHOR_ENDED);
    CHECK_EQ(reason_of(*h.w, s.incarnation), KILN_MARKER_READY);
    CHECK_EQ(reason_of(*h.w, d.incarnation), KILN_MARKER_READY);
    h.w->end_all(KILN_WORLD_CLIENT);  // a guarded stack read failed
    CHECK_EQ(reason_of(*h.w, d.incarnation), KILN_MARKER_ANCHOR_ENDED);
    CHECK_EQ(reason_of(*h.w, s.incarnation), KILN_MARKER_READY);
    // a client lease cannot be stored for a server-world record and the other way round
    uint64_t serial = 0;
    char why[256];
    CHECK_EQ(h.w->retain(7, KILN_WORLD_CLIENT, s.anchor, &serial, why, sizeof why), KILN_MARKER_INVALID_ANCHOR);
    CHECK(std::strstr(why, "server world") != nullptr);
    CHECK_EQ(h.w->retain(8, KILN_WORLD_SERVER, s.anchor, &serial, why, sizeof why), KILN_MARKER_INVALID_ANCHOR);  // another owner's
    CHECK_EQ(h.w->retain(7, KILN_WORLD_SERVER, 0, &serial, why, sizeof why), KILN_MARKER_INVALID_ANCHOR);
    CHECK_EQ(serial, 0u);
    kmk_test::end("the network create path and a failed guard end client watches only; leases are per world kind and owner");
}

void quotas() {
    kmk_test::begin();
    World h;
    for (uint32_t e = 1; e <= 600; ++e) {
        h.fw->put(kServerWorld, 10000 + e, 1, 1, 1);
        h.fw->put(kClientWorld, 10000 + e, 1, 1, 1);
    }
    std::vector<KilnMarkerAnchor> mine;
    for (uint32_t i = 0; i < kLeasesPerOwner; ++i) {
        auto r = cap(*h.w, 1, KILN_WORLD_SERVER, 1, 10001 + i);
        CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
        mine.push_back(r.anchor);
    }
    auto over = cap(*h.w, 1, KILN_WORLD_SERVER, 1, 10500);
    CHECK_EQ(over.status, KILN_MARKER_FULL);
    CHECK(std::strstr(over.reason, "this publisher holds 128") != nullptr);
    CHECK_EQ(cap(*h.w, 1, KILN_WORLD_SERVER, 1, 10001).status, KILN_MARKER_FULL);  // a lease is a lease, also of a watched one
    for (uint32_t i = 0; i < kLeasesPerOwner; ++i) CHECK_EQ(cap(*h.w, 2, KILN_WORLD_SERVER, 1, 10200 + i).status, KILN_MARKER_ACCEPTED);
    auto kind = cap(*h.w, 3, KILN_WORLD_SERVER, 1, 10400);  // 256 on the server world
    CHECK_EQ(kind.status, KILN_MARKER_FULL);
    CHECK(std::strstr(kind.reason, "server world") != nullptr);
    for (uint32_t i = 0; i < kLeasesPerOwner; ++i) CHECK_EQ(cap(*h.w, 3, KILN_WORLD_CLIENT, 2, 10001 + i).status, KILN_MARKER_ACCEPTED);
    for (uint32_t i = 0; i < kLeasesPerOwner; ++i) CHECK_EQ(cap(*h.w, 4, KILN_WORLD_CLIENT, 2, 10200 + i).status, KILN_MARKER_ACCEPTED);
    auto total = cap(*h.w, 5, KILN_WORLD_CLIENT, 2, 10450);  // 512 in all
    CHECK_EQ(total.status, KILN_MARKER_FULL);
    CHECK_EQ(h.w->counts().leases, kLeasesTotal);
    CHECK_EQ(h.w->release(1, mine[0]), KILN_OK);  // room again
    CHECK_EQ(cap(*h.w, 5, KILN_WORLD_SERVER, 1, 10450).status, KILN_MARKER_ACCEPTED);
    // the table itself: every slot a record holds after its leases went, then 1024 watched entities
    World t;
    for (uint32_t e = 1; e <= 1100; ++e) {
        t.fw->put(kServerWorld, 20000 + e, 1, 1, 1);
        t.fw->put(kClientWorld, 20000 + e, 1, 1, 1);
    }
    uint32_t held = 0;
    for (uint32_t round = 0; round < 4; ++round) {
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t kind2 = round % 2 ? KILN_WORLD_CLIENT : KILN_WORLD_SERVER;
            auto r = cap(*t.w, 100 + round * 2 + i / 128, kind2, kind2 == KILN_WORLD_CLIENT ? 2 : 1, 20001 + (round / 2) * 256 + i);
            CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
            uint64_t s = 0;
            char why[200];
            if (t.w->retain(100 + round * 2 + i / 128, kind2, r.anchor, &s, why, sizeof why) == KILN_MARKER_ACCEPTED) ++held;
            CHECK_EQ(t.w->release(100 + round * 2 + i / 128, r.anchor), KILN_OK);
        }
    }
    CHECK_EQ(held, 1024u);
    CHECK_EQ(t.w->counts().slots, kWatchSlots);
    auto full = cap(*t.w, 999, KILN_WORLD_SERVER, 1, 21000);
    CHECK_EQ(full.status, KILN_MARKER_FULL);
    CHECK(std::strstr(full.reason, "watches 1024") != nullptr);
    CHECK_EQ(t.w->counts().leases, 0u);
    kmk_test::end("quotas: 128 leases per publisher, 256 per world kind, 512 in all, 1024 watched incarnations; Full, nothing evicted");
}

void world_stop() {
    kmk_test::begin();
    World h;
    h.fw->put(kClientWorld, 70, 1, 2, 3);
    auto a = cap(*h.w, 7, KILN_WORLD_CLIENT, 2, 70);
    uint64_t serial = 0;
    char why[256];
    CHECK_EQ(h.w->retain(7, KILN_WORLD_CLIENT, a.anchor, &serial, why, sizeof why), KILN_MARKER_ACCEPTED);
    h.w->world_stopping(KILN_WORLD_CLIENT);
    CHECK_EQ(reason_of(*h.w, serial), KILN_MARKER_ANCHOR_ENDED);  // ended before the storage can be reused
    CHECK_EQ(h.w->release(7, a.anchor), KILN_ERR_NOT_FOUND);       // its leases went with it
    CHECK_EQ(h.w->counts().leases, 0u);
    CHECK_EQ(h.w->counts().slots, 1u);  // the record's reference keeps the ended slot until the store drops it
    h.w->structural(kClientStorage, 70);  // the old storage is no world any more
    h.w->drop(serial);
    CHECK_EQ(h.w->counts().slots, 0u);
    h.w->world_started(KILN_WORLD_CLIENT, kClientStorage, 5);  // the same storage again, a new epoch
    CHECK_EQ(cap(*h.w, 7, KILN_WORLD_CLIENT, 2, 70).status, KILN_MARKER_NOT_READY);
    auto b = cap(*h.w, 7, KILN_WORLD_CLIENT, 5, 70);
    CHECK_EQ(b.status, KILN_MARKER_ACCEPTED);
    CHECK(b.incarnation > a.incarnation);
    h.w->world_started(KILN_WORLD_CLIENT, kClientStorage, 6);  // a start without a stop ends the old world's watches
    CHECK_EQ(h.w->release(7, b.anchor), KILN_ERR_NOT_FOUND);
    CHECK_EQ(h.w->counts().slots, 0u);
    kmk_test::end("a world stop ends its incarnations and drops its leases before the storage is reused; new world, new epoch");
}

void changed_during_read() {
    kmk_test::begin();
    World h;
    h.fw->put(kServerWorld, 80, 1, 1, 1);
    Watches::Pending p;
    char why[256];
    CHECK_EQ(h.w->begin(7, KILN_WORLD_SERVER, 1, 80, &p, why, sizeof why), KILN_MARKER_ACCEPTED);
    CHECK(p.fresh);
    h.w->structural(kServerStorage, 80);  // destroyed or recreated between the reservation and the read
    KilnMarkerAnchor handle = 1;
    CHECK_EQ(h.w->finish(7, p, fake_read(KILN_WORLD_SERVER, 80, nullptr), &handle, why, sizeof why), KILN_MARKER_NOT_FOUND);
    CHECK_EQ(handle, 0u);
    CHECK(std::strstr(why, "while it was read") != nullptr);
    CHECK_EQ(h.w->counts().slots, 0u);
    kmk_test::end("an entity destroyed or recreated while it is read gives no lease (watched from before the read)");
}

void observers_on_threads() {
    kmk_test::begin();
    World h;
    // H: the ids the observers will hit (all watched first); K: ids they never hit (some share H's low 16 bits)
    std::vector<uint32_t> hit, kept;
    for (uint32_t i = 0; i < 64; ++i) hit.push_back(30000 + i);
    for (uint32_t i = 0; i < 64; ++i) kept.push_back(30000 + i + 65536 * (1 + i % 3));
    std::vector<uint64_t> hit_serials, kept_serials;
    for (uint32_t e : hit) h.fw->put(kServerWorld, e, 5, 5, 5);
    for (uint32_t e : kept) h.fw->put(kServerWorld, e, 6, 6, 6);
    char why[256];
    for (uint32_t e : hit) {
        auto r = cap(*h.w, 7, KILN_WORLD_SERVER, 1, e);
        uint64_t s = 0;
        h.w->retain(7, KILN_WORLD_SERVER, r.anchor, &s, why, sizeof why);
        hit_serials.push_back(s);
    }
    for (uint32_t e : kept) {
        auto r = cap(*h.w, 8, KILN_WORLD_SERVER, 1, e);
        uint64_t s = 0;
        h.w->retain(8, KILN_WORLD_SERVER, r.anchor, &s, why, sizeof why);
        kept_serials.push_back(s);
    }
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> calls{0}, posts{0}, captures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(1234 + t);
            for (int i = 0; i < 200000; ++i) {
                const uint32_t pick = rng() % 4;
                const uint32_t id = pick == 0 ? hit[rng() % hit.size()] : 1 + rng() % 200000;
                if (kept.end() != std::find(kept.begin(), kept.end(), id)) continue;  // never a kept id
                h.w->structural(rng() % 2 ? kServerStorage : kPredictionStorage, id);
                if (rng() % 8 == 0) h.w->client_create(id);
                calls.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // the server world's Post: reads what the records reference
    threads.emplace_back([&] {
        while (!stop.load()) {
            post(*h.w, KILN_WORLD_SERVER);
            for (uint64_t s : kept_serials) (void)reason_of(*h.w, s);
            posts.fetch_add(1);
        }
    });
    // a capturer on ids nobody hits (the client world: its observers only see client_create of random ids)
    threads.emplace_back([&] {
        for (uint32_t e = 1; e <= 300; ++e) h.fw->put(kClientWorld, 400000 + e, 1, 1, 1);
        while (!stop.load()) {
            for (uint32_t e = 1; e <= 300 && !stop.load(); ++e) {
                auto r = cap(*h.w, 9, KILN_WORLD_CLIENT, 2, 400000 + e);
                if (r.status == KILN_MARKER_ACCEPTED) h.w->release(9, r.anchor);
                captures.fetch_add(1);
            }
        }
    });
    for (int t = 0; t < 4; ++t) threads[t].join();
    stop.store(true);
    for (size_t t = 4; t < threads.size(); ++t) threads[t].join();
    // every watched id the observers hit (on the server storage) ended; the rest still follow
    uint32_t ended = 0;
    for (uint64_t s : hit_serials) ended += reason_of(*h.w, s) == KILN_MARKER_ANCHOR_ENDED;
    CHECK_EQ(ended, static_cast<uint32_t>(hit.size()));
    for (uint64_t s : kept_serials) CHECK_EQ(reason_of(*h.w, s), KILN_MARKER_READY);
    const auto n = h.w->counts();
    CHECK_EQ(n.leases, 128u);  // owner 7 and 8; the capturer gave all of its own back
    CHECK_EQ(n.references, 128u);
    CHECK_EQ(n.live, 64u);
    CHECK(calls.load() > 100000 && posts.load() > 0 && captures.load() > 0);
    std::printf("  observers: %llu calls on 4 threads, %llu posts, %llu captures alongside\n", static_cast<unsigned long long>(calls.load()),
                static_cast<unsigned long long>(posts.load()), static_cast<unsigned long long>(captures.load()));
    kmk_test::end("observers on four threads against a Post reader and a capturer: every hit watch ended, nothing else");
}

// ---- the layout check against a fake kiln.types ------------------------------------------------------------------------

struct FT {
    std::string qname;
    uint32_t size;
    uint8_t kind;
    uint32_t hash;
    const FT* inner;
    std::vector<std::tuple<std::string, const FT*, uint64_t>> fields;
};
std::vector<std::unique_ptr<FT>> g_types;
const FT* T(const KilnType* t) { return reinterpret_cast<const FT*>(t); }
const KilnType* K(const FT* t) { return reinterpret_cast<const KilnType*>(t); }
FT* add(std::string q, uint32_t size, uint8_t kind, uint32_t hash = 0, const FT* inner = nullptr) {
    g_types.push_back(std::make_unique<FT>(FT{std::move(q), size, kind, hash, inner, {}}));
    return g_types.back().get();
}
const KilnType* KILN_CALL ft_by_hash(uint32_t h) {
    for (auto& t : g_types)
        if (t->hash == h && h) return K(t.get());
    return nullptr;
}
KilnResult KILN_CALL ft_info(const KilnType* t, KilnTypeInfo* out) {
    if (!t) return KILN_ERR_INVALID_ARGUMENT;
    out->qualified_name = KilnStr{T(t)->qname.data(), T(t)->qname.size()};
    out->name = out->qualified_name;
    out->type_size = T(t)->size;
    out->kind = T(t)->kind;
    out->hash = T(t)->hash;
    out->field_count = static_cast<uint32_t>(T(t)->fields.size());
    out->inner = K(T(t)->inner);
    return KILN_OK;
}
KilnResult KILN_CALL ft_field(const KilnType* t, uint32_t i, KilnTypeField* out) {
    if (!t || i >= T(t)->fields.size()) return KILN_ERR_NOT_FOUND;
    const auto& [n, ft, off] = T(t)->fields[i];
    out->name = KilnStr{n.data(), n.size()};
    out->type = K(ft);
    out->offset = off;
    return KILN_OK;
}

struct Layout {
    FT *s64, *pos, *quat, *f3, *u32, *wt, *component, *current, *render;
};
Layout build_types() {
    g_types.clear();
    Layout l{};
    l.s64 = add("keen::sint64", 8, KILN_TYPE_S64);
    FT* s64_alias = add("keen::WorldCoordinate", 8, KILN_TYPE_TYPEDEF, 0, l.s64);  // a typedef is looked through
    l.pos = add("keen::WorldPosition", 24, KILN_TYPE_STRUCT, 0xf99c5535u);
    l.pos->fields = {{"x", s64_alias, 0}, {"y", l.s64, 8}, {"z", l.s64, 16}};
    l.quat = add("keen::Quaternion", 16, KILN_TYPE_STRUCT);
    l.f3 = add("keen::float3", 12, KILN_TYPE_STRUCT);
    l.u32 = add("keen::uint32", 4, KILN_TYPE_U32);
    l.wt = add("keen::WorldTransform", 56, KILN_TYPE_STRUCT, 0x02968cf2u);
    l.wt->fields = {{"position", l.pos, 0}, {"orientation", l.quat, 24}, {"scale", l.f3, 40}};
    l.component = add("keen::ecs::Component", 0, KILN_TYPE_STRUCT);
    l.current = add("keen::ecs::CurrentTransform", 56, KILN_TYPE_STRUCT, 0x027ac564u, l.component);
    l.current->fields = {{"transform", l.wt, 0}};
    l.render = add("keen::ecs::RenderTransform", 176, KILN_TYPE_STRUCT, 0x3b8476ddu, l.component);
    l.render->fields = {{"transform", l.wt, 0},
                        {"interpolationSource", l.wt, 56},
                        {"interpolationTarget", l.wt, 112},
                        {"interpolationSourceTick", l.u32, 168},
                        {"interpolationNewestTick", l.u32, 172}};
    return l;
}

void layout() {
    kmk_test::begin();
    KilnTypesApi api{};
    api.header = {sizeof(KilnTypesApi), KILN_TYPES_VERSION};
    api.type_by_hash = &ft_by_hash;
    api.type_info = &ft_info;
    api.type_field = &ft_field;
    std::string why;
    Layout l = build_types();
    CHECK(layout_check(&api, KILN_WORLD_SERVER, &why));
    CHECK(layout_check(&api, KILN_WORLD_CLIENT, &why));
    CHECK(!layout_check(nullptr, KILN_WORLD_SERVER, &why));
    CHECK(why.find("kiln.types") != std::string::npos);
    l.current->size = 64;  // the component grew
    CHECK(!layout_check(&api, KILN_WORLD_SERVER, &why));
    CHECK(why.find("64 bytes") != std::string::npos);
    l = build_types();
    l.pos->fields = {{"x", l.s64, 0}, {"y", l.s64, 16}, {"z", l.s64, 8}};  // y and z swapped
    CHECK(!layout_check(&api, KILN_WORLD_SERVER, &why));
    CHECK(why.find(".position.y") != std::string::npos);
    l = build_types();
    l.s64->kind = KILN_TYPE_F64;  // a double where an s64 was
    CHECK(!layout_check(&api, KILN_WORLD_CLIENT, &why));
    l = build_types();
    l.render->fields[0] = {"transform", l.wt, 56};  // the render transform's first WorldTransform moved
    CHECK(!layout_check(&api, KILN_WORLD_CLIENT, &why));
    CHECK(layout_check(&api, KILN_WORLD_SERVER, &why));  // the server's own still matches
    l = build_types();
    l.render->fields[2] = {"interpolationTarget", l.wt, 120};
    CHECK(!layout_check(&api, KILN_WORLD_CLIENT, &why));
    CHECK(why.find("interpolationTarget") != std::string::npos);
    l = build_types();
    l.current->qname = "keen::ecs::SomethingElse";
    CHECK(!layout_check(&api, KILN_WORLD_SERVER, &why));
    l = build_types();
    l.current->hash = 0;  // no type answers the hash
    CHECK(!layout_check(&api, KILN_WORLD_SERVER, &why));
    CHECK(why.find("0x027ac564") != std::string::npos);
    g_types.clear();
    kmk_test::end("layout check through kiln.types: exact names, hashes, sizes and nested offsets, else anchors stay off");
}

void reader() {
    kmk_test::begin();
    const int64_t p[3] = {fx(3670), fx(820.5), fx(-999.25)};
    const KilnVec3 m = fixed_to_metres(p);
    CHECK(m.x == 3670 && m.y == 820.5 && m.z == -999.25);
    CHECK(position_ok({-1000, -1000, -1000}) && position_ok({12000, 5000, 12000}));
    CHECK(!position_ok({-1000.5, 0, 0}) && !position_ok({0, 5000.5, 0}) && !position_ok({0, 0, 12000.5}));
    CHECK(!position_ok({NAN, 0, 0}) && !position_ok({0, INFINITY, 0}));
    uint8_t bytes[176] = {};
    std::memcpy(bytes, p, 24);
    CHECK(decode_transform(bytes, 56, kServerTransform).kind == ReadKind::Ok);
    CHECK(decode_transform(bytes, 176, kClientTransform).kind == ReadKind::Ok);
    CHECK(decode_transform(bytes, 176, kServerTransform).kind == ReadKind::WrongSize);
    CHECK(decode_transform(nullptr, 56, kServerTransform).kind == ReadKind::Absent);
    const int64_t far[3] = {fx(20000), 0, 0};
    std::memcpy(bytes, far, 24);
    CHECK(decode_transform(bytes, 56, kServerTransform).kind == ReadKind::BadPosition);
    FakeWorlds fw;
    g_fw = &fw;
    fw.put(kServerWorld, 9, 1, 2, 3);
    CHECK(read_transform(&fake_get, kServerWorld, KILN_WORLD_SERVER, 9).kind == ReadKind::Ok);
    CHECK(read_transform(&fake_get, kServerWorld, KILN_WORLD_SERVER, 10).kind == ReadKind::Absent);
    CHECK(read_transform(nullptr, kServerWorld, KILN_WORLD_SERVER, 9).kind == ReadKind::Absent);
    CHECK(read_transform(&fake_get, 0, KILN_WORLD_SERVER, 9).kind == ReadKind::Absent);
    fw.faulting.insert(9);
    CHECK(read_transform(&fake_get, kServerWorld, KILN_WORLD_SERVER, 9).kind == ReadKind::Fault);
    kmk_test::end("the reader: 32.32 metres, world bounds, sizes per world kind, absent and faulted reads");
}

// ---- the local store following an entity -------------------------------------------------------------------------------

uint32_t acquire_cb(KilnOwner owner, uint32_t kind, KilnMarkerAnchor lease, uint64_t* serial, char* why, size_t cap, void* ctx) {
    return static_cast<Watches*>(ctx)->retain(owner, kind, lease, serial, why, cap);
}
void release_cb(uint64_t serial, void* ctx) { static_cast<Watches*>(ctx)->drop(serial); }

struct StoreSpec {
    KilnMarkerSpec s{};
    std::string key, text = "Nameplate";
    explicit StoreSpec(std::string k, KilnMarkerAnchor follow) : key(std::move(k)) {
        s.size = sizeof s;
        s.surfaces = KILN_MARKER_WORLD;
        s.position = {0, 0, 0};
        s.follow = follow;
        s.height = 2;
        s.rgba = 0xffffffffu;
    }
    const KilnMarkerSpec* get() {
        s.key = KilnStr{key.data(), key.size()};
        s.text = KilnStr{text.data(), text.size()};
        return &s;
    }
};

KilnMarkerResult set(Store& st, KilnOwner o, StoreSpec& sp, KilnResult* rc = nullptr) {
    KilnMarkerResult r{};
    r.size = sizeof r;
    const KilnResult x = st.set_local(o, sp.get(), &r);
    if (rc) *rc = x;
    return r;
}

void store_follow() {
    kmk_test::begin();
    World h;
    Arena arena;
    Switches sw;
    sw.local = true;
    sw.world = true;
    Store st(arena, sw);
    st.world_started(KILN_WORLD_CLIENT);  // the store's client epoch (1); the anchors' client world runs at epoch 2 here
    h.w->world_started(KILN_WORLD_CLIENT, kClientStorage, 1);
    CHECK_EQ(st.bind(7, KilnStr{"follow.mod", 10}), KILN_OK);
    CHECK_EQ(st.bind(8, KilnStr{"other.mod", 9}), KILN_OK);
    h.fw->put(kClientWorld, 90, 1, 2, 3);
    h.fw->put(kClientWorld, 91, 4, 5, 6);
    auto a = cap(*h.w, 7, KILN_WORLD_CLIENT, 1, 90);
    auto a2 = cap(*h.w, 7, KILN_WORLD_CLIENT, 1, 90);  // a second lease of the same incarnation
    auto b = cap(*h.w, 7, KILN_WORLD_CLIENT, 1, 91);
    StoreSpec sp("plate", a.anchor);
    KilnResult rc = 0;
    // without the anchors' row: unavailable, nothing stored
    auto off = set(st, 7, sp, &rc);
    CHECK_EQ(rc, KILN_ERR_UNAVAILABLE);
    CHECK(std::strcmp(off.reason, "markers.anchors.client") == 0);
    sw.anchors_client = true;
    off = set(st, 7, sp, &rc);  // the row is on but no callbacks are registered yet
    CHECK_EQ(rc, KILN_ERR_UNAVAILABLE);
    st.set_follow(&acquire_cb, &release_cb, h.w.get());
    auto r = set(st, 7, sp, &rc);
    CHECK_EQ(rc, KILN_OK);
    CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(r.affected, 1u);
    CHECK_EQ(st.local_follows(), 1u);
    CHECK_EQ(h.w->counts().references, 1u);
    StoreSpec same("plate", a2.anchor);  // another lease, the same incarnation: the same value
    same.s.position = {500, 500, 500};   // ignored with follow
    r = set(st, 7, same);
    CHECK_EQ(r.status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(r.affected, 0u);
    CHECK_EQ(h.w->counts().references, 1u);  // no second reference kept
    // the caller's leases go; the stored record keeps following
    CHECK_EQ(h.w->release(7, a.anchor), KILN_OK);
    CHECK_EQ(h.w->release(7, a2.anchor), KILN_OK);
    CHECK_EQ(h.w->counts().slots, 2u);
    std::vector<ClientRow> rows;
    st.snapshot_client(&rows);
    CHECK_EQ(rows.size(), 1u);
    uint32_t reason = 0;
    if (rows.size() == 1) {
        CHECK_EQ(rows[0].rec.follow, a.incarnation);
        CHECK(rows[0].rec.x == 0 && rows[0].rec.y == 0);
        CHECK(h.w->follow(rows[0].rec.follow, nullptr, &reason) && reason == KILN_MARKER_READY);
    }
    // a released, foreign or ended lease is refused, nothing committed
    StoreSpec stale("plate2", a.anchor);
    r = set(st, 7, stale);
    CHECK_EQ(r.status, KILN_MARKER_INVALID_ANCHOR);
    CHECK_EQ(r.committed, 0u);
    CHECK(std::strncmp(r.reason, "InvalidAnchor", 13) == 0);
    StoreSpec foreign("plate3", b.anchor);
    CHECK_EQ(set(st, 8, foreign).status, KILN_MARKER_INVALID_ANCHOR);
    CHECK_EQ(st.local_count(), 1u);
    CHECK_EQ(h.w->counts().references, 1u);
    // moving the record to another entity: one Updated, the reference moves
    StoreSpec moved("plate", b.anchor);
    r = set(st, 7, moved);
    CHECK_EQ(r.affected, 1u);
    CHECK_EQ(h.w->counts().references, 1u);
    CHECK_EQ(h.w->counts().slots, 1u);  // 90's slot had nothing left
    // the entity is destroyed: the record stays (no automatic Removed) and reports AnchorEnded
    h.w->structural(kClientStorage, 91);
    st.snapshot_client(&rows);
    CHECK_EQ(rows.size(), 1u);
    if (rows.size() == 1) CHECK(h.w->follow(rows[0].rec.follow, nullptr, &reason) && reason == KILN_MARKER_ANCHOR_ENDED);
    // ... and the ended lease cannot follow a new record
    StoreSpec again("plate4", b.anchor);
    r = set(st, 7, again);
    CHECK_EQ(r.status, KILN_MARKER_INVALID_ANCHOR);
    CHECK(std::strstr(r.reason, "capture it again") != nullptr);
    // remove gives the reference back
    KilnMarkerResult rr{};
    rr.size = sizeof rr;
    CHECK_EQ(st.remove_local(7, KilnStr{"plate", 5}, &rr), KILN_OK);
    CHECK_EQ(st.local_follows(), 0u);
    CHECK_EQ(h.w->counts().references, 0u);
    CHECK_EQ(h.w->release(7, b.anchor), KILN_OK);
    CHECK_EQ(h.w->counts().slots, 0u);
    // clear, release_owner and the world's stop give theirs back too
    std::vector<KilnMarkerAnchor> leases;
    for (uint32_t e = 0; e < 3; ++e) {
        h.fw->put(kClientWorld, 100 + e, 1, 1, 1);
        leases.push_back(cap(*h.w, 7, KILN_WORLD_CLIENT, 1, 100 + e).anchor);
        StoreSpec s("c" + std::to_string(e), leases.back());
        CHECK_EQ(set(st, 7, s).status, KILN_MARKER_ACCEPTED);
    }
    CHECK_EQ(st.clear_local(7, &rr), KILN_OK);
    CHECK_EQ(h.w->counts().references, 0u);
    StoreSpec r1("r1", leases[0]);
    CHECK_EQ(set(st, 7, r1).status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(st.release(7), KILN_OK);
    CHECK_EQ(h.w->counts().references, 0u);
    CHECK_EQ(st.bind(7, KilnStr{"follow.mod", 10}), KILN_OK);
    StoreSpec r2("r2", leases[1]);
    CHECK_EQ(set(st, 7, r2).status, KILN_MARKER_ACCEPTED);
    h.w->world_stopping(KILN_WORLD_CLIENT);  // the glue's order: anchors first, then the store
    st.world_stopping(KILN_WORLD_CLIENT);
    CHECK_EQ(h.w->counts().references, 0u);
    CHECK_EQ(h.w->counts().slots, 0u);
    CHECK_EQ(st.local_follows(), 0u);
    kmk_test::end("the local store: a followed record holds one reference; two leases of one incarnation are one value; refs go back");
}

void store_follow_limit() {
    kmk_test::begin();
    World h;
    Arena arena;
    Switches sw;
    sw.local = true;
    sw.world = true;
    sw.anchors_client = true;
    Store st(arena, sw);
    st.world_started(KILN_WORLD_CLIENT);
    h.w->world_started(KILN_WORLD_CLIENT, kClientStorage, 1);
    st.set_follow(&acquire_cb, &release_cb, h.w.get());
    CHECK_EQ(st.bind(7, KilnStr{"a.mod", 5}), KILN_OK);
    CHECK_EQ(st.bind(8, KilnStr{"b.mod", 5}), KILN_OK);
    h.fw->put(kClientWorld, 500, 1, 1, 1);
    auto la = cap(*h.w, 7, KILN_WORLD_CLIENT, 1, 500);
    auto lb = cap(*h.w, 8, KILN_WORLD_CLIENT, 1, 500);
    uint32_t ok = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        StoreSpec s("k" + std::to_string(i), la.anchor);
        ok += set(st, 7, s).status == KILN_MARKER_ACCEPTED;
        // the per-step pending limit (64): run a "client step" now and then
        if (i % 50 == 49) (void)st.take(kRoleClient);
    }
    (void)st.take(kRoleClient);
    for (uint32_t i = 0; i < 28; ++i) {
        StoreSpec s("k" + std::to_string(i), lb.anchor);
        ok += set(st, 8, s).status == KILN_MARKER_ACCEPTED;
    }
    (void)st.take(kRoleClient);
    CHECK_EQ(ok, 128u);
    CHECK_EQ(st.local_follows(), kLocalFollows);
    StoreSpec over("k99", lb.anchor);
    auto r = set(st, 8, over);
    CHECK_EQ(r.status, KILN_MARKER_FULL);
    CHECK(std::strstr(r.reason, "follow an entity") != nullptr);
    // a fixed marker still fits
    StoreSpec fixed("fixed", 0);
    fixed.s.position = {1, 2, 3};
    CHECK_EQ(set(st, 8, fixed).status, KILN_MARKER_ACCEPTED);
    // turning a followed record into a fixed one gives a follow back
    StoreSpec unf("k0", 0);
    unf.s.position = {1, 2, 3};
    (void)st.take(kRoleClient);
    CHECK_EQ(set(st, 8, unf).status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(st.local_follows(), kLocalFollows - 1);
    CHECK_EQ(set(st, 8, over).status, KILN_MARKER_ACCEPTED);
    CHECK_EQ(h.w->counts().references, 128u);
    kmk_test::end("the local store: at most 128 records follow an entity (Full); fixed markers keep working");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // every line out before a sanitizer stops the run
    serials_and_leases();
    epoch_first();
    not_found_no_lease();
    destroy_and_same_id();
    missing_then_recovery();
    client_paths();
    quotas();
    world_stop();
    changed_during_read();
    observers_on_threads();
    layout();
    reader();
    store_follow();
    store_follow_limit();
    return kmk_test::finish("anchors_test");
}
