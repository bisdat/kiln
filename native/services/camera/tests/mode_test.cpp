// SPDX-License-Identifier: MIT
// Host tests of kiln.camera's SERVER_MODE backend (mode.h) with the real lease model (state.h) on a fake kiln.world:
// requests run in the fake's step after a fake "game writer" put the stock bytes back, and their results come in the
// next Pre, before the backend's own step, as kiln.world does (kiln_world.h, tracked writes). run.sh builds it.
#include <array>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../mode.h"

static int g_checks = 0;
#define CHECK(x)                                                              \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(x)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::exit(1);                                                     \
        }                                                                     \
    } while (0)

using namespace kcam;
using Bytes = std::array<uint8_t, 3>;

namespace {

constexpr uint32_t kNcc = 0xd2c927b5u;
constexpr KilnEntity A = 1, B = 2;
constexpr KilnOwner kMod = 101, kOther = 102;

std::vector<std::string> g_log;

void KILN_CALL on_done(const KilnCameraResult* r, void*) {
    char line[160];
    std::snprintf(line, sizeof line, "done %llu ok=%u reason=%u rev=%llu flags=%u scope=%u", static_cast<unsigned long long>(r->lease), r->ok, r->reason_code,
                  static_cast<unsigned long long>(r->revision), r->flags, r->commit_scope);
    g_log.push_back(line);
}

void KILN_CALL on_changed(const KilnCameraChange* c, void*) {
    static const char* const kKind[] = {"?", "applied", "updated", "ended"};
    char line[160];
    std::snprintf(line, sizeof line, "%s %llu rev=%llu end=%u", kKind[c->kind < 4 ? c->kind : 0], static_cast<unsigned long long>(c->lease),
                  static_cast<unsigned long long>(c->revision), c->end_reason);
    g_log.push_back(line);
}

struct TestEnv : Env {
    bool call_done(KilnCameraDone fn, const KilnCameraResult& r, void* user) override { return fn(&r, user), true; }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& c, void* user) override { return fn(&c, user), true; }
    void log(int32_t, const std::string&) override {}
};

// kiln.world's tracked writes as the backend sees them, and the game around them.
struct FakeWorld : ModeWorld {
    struct Req {
        uint64_t id = 0;
        KilnWorldWriteRequest q{};
        bool cancelled = false, ran = false, delivered = false, suppress_result = false;
        bool current_at_cancel = false;  // the ticket's guard still named it when cancel came
        uint32_t status = 0;
        uint64_t step = 0;
    };
    std::vector<Req> reqs;
    uint64_t next_id = 1, now = 1000, steps = 0, fraction_ns = 0;
    std::map<KilnEntity, Incarnation> players{{A, {1, 1}}, {B, {2, 1}}};
    std::map<KilnEntity, Bytes> ncc, stock{{A, {0, 7, 0}}, {B, {0, 9, 1}}};
    uint32_t state = KILN_WORLD_WRITES_READY;  // write_info's state of NCC for kiln.camera
    bool cancel_wrong_thread = false;      // write_cancel answers WRONG_THREAD (inside the write program, off the tick thread)
    bool in_update = false;                // the write program's update runs: a cancel now drops the result only
    std::function<void()> mid_update;      // runs inside the update, after it took its batch (a program on the tick thread)
    std::function<void()> on_now;          // runs once inside the next now_ns() (another thread's call at that point)
    bool game_writes = true;               // the game's writer puts the stock bytes back every step
    bool component_gone = false;           // NO_COMPONENT for every request
    uint32_t force_status = 0xffffffffu;   // answer every request with this status, without running it
    int cancels_while_current = 0;
    std::vector<std::string> logs;

    KilnResult submit(const KilnWorldWriteRequest& q, uint64_t* out) override {
        Req r;
        r.id = next_id++;
        r.q = q;
        reqs.push_back(r);
        *out = r.id;
        return KILN_OK;
    }
    KilnResult cancel(uint64_t ticket) override {
        for (Req& r : reqs)
            if (r.id == ticket) {
                r.current_at_cancel = ModeBackend::ticket_current(r.q.user);
                if (r.current_at_cancel) ++cancels_while_current;
                if (r.delivered) return KILN_ERR_NOT_FOUND;
                if (cancel_wrong_thread) return KILN_ERR_WRONG_THREAD;
                if (in_update && !r.ran) {
                    // write_queue's cancelled_running_: OK, the result is dropped, but the change callback still runs
                    r.suppress_result = true;
                    return KILN_OK;
                }
                r.cancelled = true;
                return KILN_OK;
            }
        return KILN_ERR_NOT_FOUND;
    }
    bool player(KilnEntity e, Incarnation* out) override {
        const auto it = players.find(e);
        if (it == players.end()) return false;
        *out = it->second;
        return true;
    }
    uint64_t now_ms() override { return now; }
    uint64_t now_ns() override {
        if (on_now) {
            auto fn = std::move(on_now);
            on_now = nullptr;
            fn();
        }
        return now * 1000000 + fraction_ns;
    }
    uint64_t step() override { return steps; }
    void log(int32_t, const std::string& t) override { logs.push_back(t); }
    uint32_t write_state(uint32_t component, const std::string& who) override {
        CHECK(component == kNcc && who == "kiln.camera");
        return state;
    }

    // The sim step: the game's writer, then kiln_world_writes with every request not yet run.
    void sim() {
        ++steps;
        const uint64_t batch_end = next_id;  // the update takes what was queued before it began
        if (game_writes)
            for (auto& [e, b] : stock) ncc[e] = b;
        in_update = true;
        if (mid_update) mid_update();
        for (size_t i = 0; i < reqs.size(); ++i) {
            Req& r = reqs[i];
            if (r.ran || r.cancelled || r.id >= batch_end) continue;
            r.ran = true;
            r.step = steps;
            if (force_status != 0xffffffffu) {
                r.status = force_status;
                continue;
            }
            if (component_gone) {
                r.status = KILN_WORLD_WRITE_NO_COMPONENT;
                continue;
            }
            Bytes copy = ncc[r.q.entity];
            if (!r.q.change(r.q.entity, copy.data(), 3, r.q.user)) {
                r.status = KILN_WORLD_WRITE_REFUSED;
                continue;
            }
            r.status = copy == ncc[r.q.entity] ? KILN_WORLD_WRITE_UNCHANGED : KILN_WORLD_WRITE_DONE;
            ncc[r.q.entity] = copy;
        }
        in_update = false;
    }
    // kiln.world's Pre: the results of the last step, oldest first (a callback may submit or cancel).
    void deliver() {
        for (size_t i = 0; i < reqs.size(); ++i) {
            Req& r = reqs[i];
            if (!r.ran || r.cancelled || r.delivered) continue;
            r.delivered = true;
            if (r.suppress_result) continue;
            KilnWorldWriteResult res{};
            res.size = sizeof res;
            res.status = r.status;
            res.ticket = r.id;
            res.entity = r.q.entity;
            res.component = r.q.component;
            KilnWorldWriteFn fn = r.q.callback;
            void* user = r.q.user;
            fn(&res, user);  // may push into reqs: `r` is not used after this
        }
    }
    uint32_t open_for(KilnEntity e) const {
        uint32_t n = 0;
        for (const Req& r : reqs)
            if (r.q.entity == e && !r.cancelled && !r.delivered) ++n;
        return n;
    }
    uint32_t submitted_for(KilnEntity e) const {
        uint32_t n = 0;
        for (const Req& r : reqs)
            if (r.q.entity == e) ++n;
        return n;
    }
};

struct Rig {
    TestEnv env;
    Core core{&env};
    FakeWorld world;
    ModeBackend mode{&core, &world, kNcc, "kiln.camera"};
    KilnHandle sub = 0;

    Rig() {
        g_log.clear();
        core.set_backend(KILN_CAMERA_SERVER_MODE, &mode);
        core.set_writes(true);
        mode.set_ready(true);
        core.world_started(kServer, 1);
        CHECK(core.subscribe(999, &on_changed, nullptr, &sub) == KILN_OK);
    }
    ~Rig() {
        core.world_stopping(kServer, core.epoch(kServer));
        mode.step();  // cancel retained service-owned tickets before the fake world goes away
    }
    // One server tick: kiln.world's Pre (results), the backend's step, the core's Pre (answers, events), the sim step.
    void tick(int n = 1) {
        for (int i = 0; i < n; ++i) {
            world.deliver();
            mode.step();
            core.pre(kServer);
            world.sim();
        }
    }
    // Pre only (a test acts between the Pre and the step).
    void pre() {
        world.deliver();
        mode.step();
        core.pre(kServer);
    }
    KilnCameraLease apply(KilnOwner owner, KilnEntity player, uint32_t camera, uint32_t transition, int32_t priority = 0, uint32_t duration = 120000,
                          KilnResult want = KILN_OK) {
        KilnCameraRequest q{};
        q.size = sizeof q;
        q.parts = KILN_CAMERA_MODE;
        q.endpoint = KILN_CAMERA_SERVER_MODE;
        q.player_entity = player;
        q.input_policy = KILN_CAMERA_STOCK_MODE;
        q.duration_ms = duration;
        q.priority = priority;
        q.camera_id = static_cast<uint8_t>(camera);
        q.transition_override = static_cast<uint8_t>(transition);
        KilnCameraLease id = 0;
        CHECK(core.apply(owner, &q, &id, &on_done, nullptr, kOnServer) == want);
        return id;
    }
    void update(KilnOwner owner, KilnCameraLease id, uint32_t camera, uint32_t transition, uint32_t duration, uint32_t timing = KILN_CAMERA_RESTART_TIMELINE) {
        KilnCameraRequest q{};
        q.size = sizeof q;
        q.parts = KILN_CAMERA_MODE;
        q.endpoint = KILN_CAMERA_SERVER_MODE;
        q.player_entity = A;
        q.input_policy = KILN_CAMERA_STOCK_MODE;
        q.duration_ms = duration;
        q.camera_id = static_cast<uint8_t>(camera);
        q.transition_override = static_cast<uint8_t>(transition);
        CHECK(core.update(owner, id, timing, &q, &on_done, nullptr, kOnServer) == KILN_OK);
    }
    void end(KilnOwner owner, KilnCameraLease id) { CHECK(core.end(owner, id, &on_done, nullptr, kOnServer) == KILN_OK); }
    Bytes at(KilnEntity e) { return world.ncc[e]; }
};

std::string L(const char* fmt, unsigned long long a, unsigned long long b = 0, unsigned long long c = 0, unsigned long long d = 0) {
    char line[160];
    std::snprintf(line, sizeof line, fmt, a, b, c, d);
    return line;
}

size_t count(const std::string& prefix) {
    size_t n = 0;
    for (const std::string& l : g_log)
        if (l.rfind(prefix, 0) == 0) ++n;
    return n;
}

const Bytes kHeld{1, 34, 2};

// ---- 1-2: the write every step, one Applied, nothing for B ----
void test_maintained_and_isolated() {
    Rig r;
    const KilnCameraLease id = r.apply(kMod, A, 34, 2);
    r.tick();  // Pre: submitted; step: written
    CHECK(r.at(A) == kHeld);
    CHECK(g_log.empty());  // the answer comes with the result, one Pre later
    r.tick();
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=1 reason=0 rev=1 flags=0 scope=1", id), L("applied %llu rev=1 end=0", id)}));
    for (int i = 0; i < 20; ++i) {
        r.tick();
        CHECK(r.at(A) == kHeld);             // after the game's writer, every step
        CHECK(r.world.open_for(A) == 1);     // one write out at a time
        CHECK(r.at(B) == r.world.stock[B]);  // B as the game left it
    }
    CHECK(r.world.submitted_for(B) == 0);
    CHECK(r.world.submitted_for(A) == 22);  // one per step, never two
    CHECK(count("applied") == 1 && g_log.size() == 2);
    CHECK(r.mode.counters().results[KILN_WORLD_WRITE_DONE] == 21);
}

// ---- 3: update, AlreadyOwned, Busy, a higher priority supersedes ----
void test_update_and_supersede() {
    Rig r;
    const KilnCameraLease id = r.apply(kMod, A, 34, 2);
    r.tick(2);
    g_log.clear();
    r.update(kMod, id, 34, 0, 60000);
    r.tick();
    CHECK(r.at(A) == (Bytes{1, 34, 0}));
    r.tick();
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=1 reason=0 rev=2 flags=0 scope=1", id), L("updated %llu rev=2 end=0", id)}));
    g_log.clear();
    const KilnCameraLease again = r.apply(kMod, A, 34, 2, 5);
    const KilnCameraLease equal = r.apply(kOther, A, 34, 2, 0);
    r.tick();
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=0 reason=2 rev=0 flags=0 scope=0", again), L("done %llu ok=0 reason=1 rev=0 flags=0 scope=0", equal)}));
    g_log.clear();
    const KilnCameraLease winner = r.apply(kOther, A, 20, 1, 5);
    r.tick();  // the holder's last result sends the candidate's first write, in this step
    CHECK(r.at(A) == (Bytes{1, 20, 1}));
    r.tick();  // its result: the holder ends superseded, then the candidate is applied
    CHECK(r.at(A) == (Bytes{1, 20, 1}));
    r.tick();
    CHECK((g_log == std::vector<std::string>{L("ended %llu rev=2 end=6", id), L("done %llu ok=1 reason=0 rev=1 flags=0 scope=1", winner),
                                             L("applied %llu rev=1 end=0", winner)}));
}

// ---- 4: End between the Pre that queued the write and the step: tombstone, then cancel; the stock comes back ----
void test_end_cancels_queued_and_no_late_revision() {
    Rig r;
    const KilnCameraLease id = r.apply(kMod, A, 34, 2);
    r.tick(5);
    g_log.clear();
    r.pre();                                  // this step's write is queued
    CHECK(r.world.open_for(A) == 1);
    const size_t before = r.world.reqs.size();
    r.end(kMod, id);
    CHECK(r.world.reqs[before - 1].cancelled);
    CHECK(!r.world.reqs[before - 1].current_at_cancel);  // the guard was cleared before write_cancel
    CHECK(r.world.cancels_while_current == 0);
    r.world.sim();
    CHECK(!r.world.reqs[before - 1].ran);     // the cancelled write never ran
    CHECK(r.at(A) == r.world.stock[A]);       // the game's own bytes, not a boot snapshot
    r.tick(3);
    CHECK(r.world.reqs.size() == before);     // nothing queued for an ended lease
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=1 reason=0 rev=1 flags=0 scope=0", id), L("ended %llu rev=1 end=1", id)}));
    r.end(kMod, id);
    r.tick();
    CHECK(g_log.back() == L("done %llu ok=1 reason=0 rev=0 flags=2 scope=0", id));  // AlreadyEnded, no second event
    CHECK(count("ended") == 1);
}

// ---- 5: End before the first write; cancel a queued revision while a successor waits ----
void test_pending_cancel() {
    Rig r;
    const KilnCameraLease id = r.apply(kMod, A, 34, 2);
    r.end(kMod, id);
    r.tick(3);
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=0 reason=24 rev=0 flags=0 scope=0", id), L("done %llu ok=1 reason=0 rev=0 flags=0 scope=0", id),
                                             L("ended %llu rev=0 end=13", id)}));
    CHECK(r.world.submitted_for(A) == 0);
    CHECK(r.at(A) == r.world.stock[A]);

    g_log.clear();
    const KilnCameraLease holder = r.apply(kMod, A, 34, 2);
    r.tick(2);
    r.pre();  // the holder's write of this step is queued
    const size_t queued = r.world.reqs.size() - 1;
    const KilnCameraLease successor = r.apply(kOther, A, 30, 1, 10);
    r.end(kMod, holder);  // cancels the holder's queued write; the successor's goes out at once
    CHECK(r.world.reqs[queued].cancelled && !r.world.reqs[queued].current_at_cancel);
    CHECK(r.world.open_for(A) == 1);
    r.world.sim();
    CHECK(r.at(A) == (Bytes{1, 30, 1}));
    r.tick(2);
    CHECK(count(L("ended %llu", holder).c_str()) == 1);
    CHECK(count(L("ended %llu rev=1 end=1", holder).c_str()) == 1);  // released, not superseded
    CHECK(count(L("applied %llu rev=1", successor).c_str()) == 1);
    CHECK(r.core.active_revision(successor) == 1);
}

// ---- 6: expiry, player left, a new incarnation, owner retiring, world stop ----
void test_owner_epoch_incarnation() {
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2, 0, 1000);
        r.tick(3);
        r.world.now += 1000;  // the server clock reaches the deadline
        r.tick(2);
        CHECK(g_log.back() == L("ended %llu rev=1 end=2", id));
        CHECK(r.at(A) == r.world.stock[A]);
        CHECK(r.world.open_for(A) == 0);
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.tick(3);
        r.world.players[A] = Incarnation{1, 2};  // the id names someone else now
        r.tick(2);
        CHECK(g_log.back() == L("ended %llu rev=1 end=4", id));
        CHECK(r.world.open_for(A) == 0);
        const KilnCameraLease next = r.apply(kMod, A, 34, 2);  // the new player: a new lease works
        r.tick(2);
        CHECK(g_log.back() == L("applied %llu rev=1 end=0", next));
        r.world.players.erase(A);
        r.tick(2);
        CHECK(g_log.back() == L("ended %llu rev=1 end=4", next));
        CHECK(r.apply(kMod, A, 34, 2, 0, 120000, KILN_ERR_NOT_FOUND) == 0);  // not a player: the immediate NOT_FOUND
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.tick(3);
        r.pre();
        const size_t queued = r.world.reqs.size() - 1;
        r.core.owner_retiring(kMod);
        r.pre();  // the core drops the owner's lease at its next Pre (drop: guard, then cancel)
        CHECK(r.world.reqs[queued].cancelled && !r.world.reqs[queued].current_at_cancel);
        CHECK(g_log.back() == L("ended %llu rev=1 end=8", id));
        r.world.sim();
        CHECK(r.at(A) == r.world.stock[A]);
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.tick(3);
        r.pre();
        FakeWorld::Req& queued = r.world.reqs.back();
        r.core.world_stopping(kServer, 1);  // any thread: the change callback refuses from here
        CHECK(g_log.back() == L("ended %llu rev=1 end=5", id));
        r.world.sim();  // a step of the dying world still runs the queued write: refused
        CHECK(queued.status == KILN_WORLD_WRITE_REFUSED);
        CHECK(r.at(A) == r.world.stock[A]);
        r.core.world_started(kServer, 2);
        const size_t before = r.world.reqs.size();
        r.tick(3);
        CHECK(r.world.reqs.size() == before);  // nothing of the old epoch goes out again
        const KilnCameraLease next = r.apply(kMod, A, 34, 2);
        r.tick(2);
        CHECK(g_log.back() == L("applied %llu rev=1 end=0", next));
    }
}

// ---- 7: already matching bytes are a commit of the server state ----
void test_unchanged_scope() {
    Rig r;
    r.world.game_writes = false;  // nothing puts the stock back
    const KilnCameraLease first = r.apply(kMod, A, 34, 2);
    r.tick(2);
    r.end(kMod, first);
    r.tick();
    g_log.clear();
    const uint64_t unchanged = r.mode.counters().results[KILN_WORLD_WRITE_UNCHANGED];
    const KilnCameraLease same = r.apply(kOther, A, 34, 2);  // the component holds these bytes already
    r.tick(2);
    CHECK(r.mode.counters().results[KILN_WORLD_WRITE_UNCHANGED] > unchanged);
    CHECK((g_log == std::vector<std::string>{L("done %llu ok=1 reason=0 rev=1 flags=0 scope=1", same), L("applied %llu rev=1 end=0", same)}));
}

// ---- results that end or refuse ----
void test_failures() {
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.world.component_gone = true;
        r.tick(2);
        CHECK((g_log == std::vector<std::string>{L("done %llu ok=0 reason=8 rev=0 flags=0 scope=0", id)}));  // PeerUnavailable
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.tick(3);
        r.world.component_gone = true;
        r.tick(2);
        CHECK(g_log.back() == L("ended %llu rev=1 end=4", id));
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.world.force_status = KILN_WORLD_WRITE_NOT_DECLARED;
        r.tick(2);
        CHECK(g_log.back() == L("done %llu ok=0 reason=18 rev=0 flags=0 scope=0", id));  // FrameFault
        CHECK(r.mode.caps() == 0);  // the path does not work: the row goes off
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2);
        r.tick(3);
        r.world.force_status = KILN_WORLD_WRITE_DROPPED;
        r.tick(static_cast<int>(2 * ModeBackend::kMaxDrops) + 2);
        CHECK(g_log.back() == L("ended %llu rev=1 end=7", id));  // CapabilityLost after the drops in a row
    }
    {
        Rig r;
        const KilnCameraLease id = r.apply(kMod, A, 34, 2, 0, 5000);
        r.tick(3);
        r.world.now += 4000;
        g_log.clear();
        r.update(kMod, id, 34, 0, 3000, KILN_CAMERA_PRESERVE_TIMELINE);  // 4 s gone of a new 3 s: over already
        r.tick();
        CHECK((g_log == std::vector<std::string>{L("done %llu ok=0 reason=13 rev=1 flags=0 scope=0", id)}));  // ExpiredUpdate, revision 1 stays
        CHECK(r.core.active_revision(id) == 1);
    }
}

void test_retire_before_next_pre() {
    Rig r;
    r.apply(kMod, A, 34, 2);
    r.pre();
    r.core.owner_retiring(kMod);  // any thread: no next Pre is needed to invalidate a queued write
    r.world.sim();
    CHECK(r.at(A) == r.world.stock[A]);
    r.pre();
    CHECK(r.world.logs.empty());
}

void test_expiry_inside_write() {
    Rig r;
    r.apply(kMod, A, 34, 2, 0, 1000);
    r.tick(3);
    r.pre();
    r.world.now += 1001;  // the server stalls after Pre, before its tracked write runs
    r.world.sim();
    CHECK(r.at(A) == r.world.stock[A]);
}

void test_timeline_begins_at_write() {
    Rig r;
    r.apply(kMod, A, 34, 2, 0, 1000);
    r.pre();
    r.world.now += 500;  // queue/step latency must not consume the first revision's lifetime
    r.world.sim();
    r.tick();
    r.world.now += 600;
    r.tick();
    CHECK(r.at(A) == kHeld);
    CHECK(count("ended") == 0);
}

void test_submillisecond_origin() {
    Rig r;
    r.world.fraction_ns = 500000;
    r.apply(kMod, A, 34, 2, 0, 1);
    r.tick();  // first write at 1000.5 ms
    r.world.now = 1001;
    r.world.fraction_ns = 100000;  // elapsed 0.6 ms, not the authored 1 ms
    r.tick();
    CHECK(r.at(A) == kHeld);
    r.world.fraction_ns = 600000;
    r.tick();
    CHECK(r.at(A) == r.world.stock[A]);
}

// M1: End from a program on the tick thread while the write program's update holds the lease's job. kiln.world's
// cancel answers OK (it only drops the result) and the change callback still runs in that update with the ticket as
// `user`; or the cancel fails (WRONG_THREAD) and the job runs and is answered as if never cancelled. The ticket must
// outlive both (ASan), the write must not land, and nothing of it is reported.
void test_cancel_while_change_can_still_run() {
    for (bool wrong_thread : {false, true}) {
        Rig r;
        const auto id = r.apply(kMod, A, 34, 2);
        r.tick(2);
        CHECK(count("applied") == 1);
        r.pre();  // the next write is out
        CHECK(r.mode.tickets() == 1);
        const auto before = r.mode.counters();
        r.world.cancel_wrong_thread = wrong_thread;
        r.world.mid_update = [&] {
            r.end(kMod, id);
            CHECK(r.mode.tickets() == 0);
        };
        r.world.sim();
        r.world.mid_update = nullptr;
        r.world.cancel_wrong_thread = false;
        CHECK(r.mode.retired() == (wrong_thread ? 0u : 1u));      // kept until the next step(): the update did call it
        CHECK(r.at(A) == r.world.stock[A]);                       // the guard refused: the stock bytes stay
        CHECK(r.mode.counters().refusals == before.refusals + 1);  // the callback did run, on the live ticket
        CHECK(r.mode.counters().writes == before.writes);
        r.pre();  // the retired ticket is freed; a WRONG_THREAD one is freed by its own result
        CHECK(r.mode.tickets() == 0);
        CHECK(r.mode.retired() == 0);
        CHECK(count("ended") == 1);
        CHECK(count("applied") == 1);
        r.tick(3);
        CHECK(r.at(A) == r.world.stock[A]);
        CHECK(r.mode.counters().submits == before.submits);  // no maintenance for an ended lease
    }
}

// A cancel between two steps (a Pre) frees nothing at once either: the ticket waits for the next step(), and a
// lease ended there writes nothing more.
void test_cancel_in_pre_retires_until_next_step() {
    Rig r;
    const auto id = r.apply(kMod, A, 34, 2);
    r.tick(2);
    r.pre();
    r.end(kMod, id);
    CHECK(r.mode.retired() == 1);
    r.world.sim();
    CHECK(r.at(A) == r.world.stock[A]);
    r.pre();
    CHECK(r.mode.retired() == 0);
    CHECK(count("ended") == 1);
}

// kiln.world's write program goes off at run time (refused, faulted, skipped): every request comes back DROPPED. The
// row goes off at the first such answer and the held lease ends CapabilityLost at once, not after 20 drops; the next
// server world asks again, and a world whose program is off from its start turns the row off in its first Pre.
void test_program_off_turns_the_row_off() {
    Rig r;
    const auto id = r.apply(kMod, A, 34, 2);
    r.tick(2);
    CHECK(count("applied") == 1);
    CHECK(r.mode.caps() == KILN_CAMERA_CAP_SERVER_MODE);
    r.world.state = KILN_WORLD_WRITES_OFF;
    r.world.force_status = KILN_WORLD_WRITE_DROPPED;
    r.tick(2);
    CHECK(r.mode.caps() == 0);
    CHECK(r.mode.down() == ModeBackend::kProgramOff);
    CHECK(count("ended") == 1);
    CHECK(g_log.back() == L("ended %llu rev=1 end=%llu", id, KILN_CAMERA_END_CAPABILITY_LOST));
    r.apply(kOther, A, 34, 2, 0, 120000, KILN_ERR_UNAVAILABLE);  // the row is off: refused at the call
    r.tick(3);
    CHECK(r.mode.tickets() == 0);
    // the next server world has it again
    r.world.state = KILN_WORLD_WRITES_READY;
    r.world.force_status = 0xffffffffu;
    r.core.world_stopping(kServer, 1);
    r.core.world_started(kServer, 2);
    r.tick();
    CHECK(r.mode.caps() == KILN_CAMERA_CAP_SERVER_MODE);
    const auto again = r.apply(kMod, A, 34, 2);
    r.tick(2);
    CHECK(g_log.back() == L("applied %llu rev=1 end=0", again));
    // one whose program is off from its start: off in its first Pre, before any request
    r.world.state = KILN_WORLD_WRITES_OFF;
    r.core.world_stopping(kServer, 2);
    r.core.world_started(kServer, 3);
    r.pre();
    CHECK(r.mode.caps() == 0);
    // a staged request that meets an off program is refused FrameFault, not StaleWorld
    r.world.state = KILN_WORLD_WRITES_READY;
    r.core.world_stopping(kServer, 3);
    r.core.world_started(kServer, 4);
    r.tick();
    const auto late = r.apply(kMod, A, 34, 2);
    r.world.state = KILN_WORLD_WRITES_OFF;
    r.world.force_status = KILN_WORLD_WRITE_DROPPED;
    r.tick(2);
    CHECK(g_log.back() == L("done %llu ok=0 reason=%llu rev=0 flags=0 scope=0", late, KILN_CAMERA_REASON_FRAME_FAULT));
}

// world_stopping may come on any thread. One that lands inside step(), after it read the core's epoch and before it
// re-armed the change callbacks' epoch guard, must not bring the stopped world's epoch back: the step forgets that
// world's work instead of keeping its ticket and guard for one more step.
void test_stop_inside_step_keeps_the_world_stopped() {
    Rig r;
    const auto id = r.apply(kMod, A, 34, 2);
    r.tick(3);
    CHECK(r.at(A) == kHeld);
    r.world.deliver();  // the result submits the next write
    CHECK(r.mode.tickets() == 1);
    r.world.on_now = [&] { r.core.world_stopping(kServer, 1); };  // fires inside step(), after its epoch read
    r.mode.step();
    CHECK(!r.world.on_now);
    CHECK(g_log.back() == L("ended %llu rev=1 end=%llu", id, KILN_CAMERA_END_WORLD_STOPPED));
    CHECK(r.mode.tickets() == 0);        // that world's ticket is cancelled and forgotten
    CHECK(r.mode.guard(A).lease == 0);   // no guard is armed for it
    r.world.sim();
    CHECK(r.at(A) == r.world.stock[A]);
    // the next world arms again
    r.core.world_started(kServer, 2);
    const auto next = r.apply(kMod, A, 34, 2);
    r.tick(2);
    CHECK(g_log.back() == L("applied %llu rev=1 end=0", next));
    CHECK(r.at(A) == kHeld);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        if (std::strcmp(argv[1], "retire") == 0) test_retire_before_next_pre();
        if (std::strcmp(argv[1], "expiry") == 0) test_expiry_inside_write();
        if (std::strcmp(argv[1], "timeline") == 0) test_timeline_begins_at_write();
        if (std::strcmp(argv[1], "precision") == 0) test_submillisecond_origin();
        if (std::strcmp(argv[1], "running-cancel") == 0) test_cancel_while_change_can_still_run();
        if (std::strcmp(argv[1], "stop-inside-step") == 0) test_stop_inside_step_keeps_the_world_stopped();
        return 0;
    }
    test_cancel_while_change_can_still_run();
    test_cancel_in_pre_retires_until_next_step();
    test_program_off_turns_the_row_off();
    test_stop_inside_step_keeps_the_world_stopped();
    test_retire_before_next_pre();
    test_expiry_inside_write();
    test_timeline_begins_at_write();
    test_submillisecond_origin();
    test_maintained_and_isolated();
    test_update_and_supersede();
    test_end_cancels_queued_and_no_late_revision();
    test_pending_cancel();
    test_owner_epoch_incarnation();
    test_unchanged_scope();
    test_failures();
    std::printf("camera mode: PASS (%d checks)\n", g_checks);
    return 0;
}
