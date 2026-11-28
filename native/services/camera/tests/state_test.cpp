// SPDX-License-Identifier: MIT
// Host tests of kiln.camera's lease model (state.h) on a fake backend (run.sh). Each test says in its name what it
// proves; a CHECK that fails prints its line and ends the run with 1.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../state.h"

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

namespace {

// ---- what the callbacks saw, in order ----
std::vector<std::string> g_log;
bool g_fault_next = false;         // the callback that runs next "faults"
int g_fault_changed = 0;           // n > 0: the n-th change callback from now "faults"
bool g_in_table_call = false;      // true while apply/update/end runs: no callback may come then
std::thread::id g_callback_thread;
Core* g_core = nullptr;
KilnHandle g_unsubscribe_in_callback = 0;

std::string str(KilnStr s) { return std::string(s.ptr ? s.ptr : "", s.len); }

void KILN_CALL on_done(const KilnCameraResult* r, void* user) {
    CHECK(!g_in_table_call);
    CHECK(r->size == sizeof(KilnCameraResult));
    g_callback_thread = std::this_thread::get_id();
    char line[200];
    std::snprintf(line, sizeof line, "done %s lease=%llu ok=%u reason=%u rev=%llu flags=%u scope=%u", static_cast<const char*>(user),
                  static_cast<unsigned long long>(r->lease), r->ok, r->reason_code, static_cast<unsigned long long>(r->revision), r->flags, r->commit_scope);
    g_log.push_back(line);
    if (!r->ok) CHECK(!str(r->reason).empty());  // a refusal always has words
}

void KILN_CALL on_changed(const KilnCameraChange* c, void* user) {
    CHECK(!g_in_table_call);
    CHECK(c->size == sizeof(KilnCameraChange));
    g_callback_thread = std::this_thread::get_id();
    static const char* const kKind[] = {"?", "applied", "updated", "ended"};
    char line[200];
    std::snprintf(line, sizeof line, "event %s %s lease=%llu rev=%llu end=%u gen=%u", static_cast<const char*>(user), kKind[c->kind < 4 ? c->kind : 0],
                  static_cast<unsigned long long>(c->lease), static_cast<unsigned long long>(c->revision), c->end_reason, c->local_generation);
    g_log.push_back(line);
    if (g_unsubscribe_in_callback) {
        CHECK(g_core->unsubscribe(g_unsubscribe_in_callback) == KILN_OK);
        g_unsubscribe_in_callback = 0;
    }
}

struct TestEnv : Env {
    std::vector<std::string> lines;
    bool call_done(KilnCameraDone fn, const KilnCameraResult& r, void* user) override {
        if (g_fault_next) return g_fault_next = false, false;
        fn(&r, user);
        return true;
    }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& c, void* user) override {
        if (g_fault_next) return g_fault_next = false, false;
        if (g_fault_changed && --g_fault_changed == 0) return false;
        fn(&c, user);
        return true;
    }
    void log(int32_t, const std::string& text) override { lines.push_back(text); }
};

struct FakeBackend : Backend {
    uint64_t bits = 0;
    std::vector<std::string> calls;
    std::vector<Staged> staged;  // request pointers are not kept valid: only the numbers are read
    KilnResult admit_rc = KILN_OK;
    uint32_t admit_reason = 0, stage_reason = 0;
    bool release_now = true;
    uint32_t local_player = 0;

    uint64_t caps() const override { return bits; }
    KilnResult admit(Stored& request, uint32_t* reason, const char**) override {
        if (request.req.endpoint == KILN_CAMERA_LOCAL_VISUAL && !request.req.player_entity) request.req.player_entity = local_player;
        *reason = admit_reason;
        return admit_rc;
    }
    bool stage(const Staged& s, uint32_t* reason, const char**) override {
        if (stage_reason) return *reason = stage_reason, false;
        Staged copy = s;
        copy.request = nullptr;
        staged.push_back(copy);
        calls.push_back("stage " + std::to_string(s.lease) + "/" + std::to_string(s.revision));
        return true;
    }
    void cancel_update(KilnCameraLease lease, uint64_t revision) override { calls.push_back("cancel_update " + std::to_string(lease) + "/" + std::to_string(revision)); }
    bool release(KilnCameraLease lease, uint64_t revision) override {
        calls.push_back("release " + std::to_string(lease) + "/" + std::to_string(revision));
        return release_now;
    }
    void drop(KilnCameraLease lease) override { calls.push_back("drop " + std::to_string(lease)); }
    void world_stopped(uint32_t epoch) override { calls.push_back("world_stopped " + std::to_string(epoch)); }
};

char kA[] = "A", kB[] = "B", kSub[] = "sub", kSub2[] = "sub2";
constexpr KilnOwner kOwnerA = 11, kOwnerB = 22, kOwnerWatch = 33;

struct Rig {
    TestEnv env;
    Core core{&env};
    FakeBackend mode, local;
    Rig() {
        g_log.clear();
        g_fault_next = false;
        g_fault_changed = 0;
        g_core = &core;
        g_unsubscribe_in_callback = 0;
        mode.bits = KILN_CAMERA_CAP_SERVER_MODE;
        local.bits = KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE;
        local.local_player = 7;
        core.set_backend(KILN_CAMERA_SERVER_MODE, &mode);
        core.set_backend(KILN_CAMERA_LOCAL_VISUAL, &local);
        core.set_writes(true);
        core.world_started(kServer, 1);
        core.world_started(kClient, 1);
    }
    KilnResult apply(KilnOwner owner, const KilnCameraRequest& r, KilnCameraLease* lease, char* name, uint32_t threads = kOnServer | kOnClient) {
        g_in_table_call = true;
        const KilnResult rc = core.apply(owner, &r, lease, &on_done, name, threads);
        g_in_table_call = false;
        return rc;
    }
    KilnResult update(KilnOwner owner, KilnCameraLease lease, const KilnCameraRequest& r, char* name, uint32_t timing = 0) {
        g_in_table_call = true;
        const KilnResult rc = core.update(owner, lease, timing, &r, &on_done, name, kOnServer | kOnClient);
        g_in_table_call = false;
        return rc;
    }
    KilnResult end(KilnOwner owner, KilnCameraLease lease, char* name, uint32_t threads = kOnServer | kOnClient) {
        g_in_table_call = true;
        const KilnResult rc = core.end(owner, lease, &on_done, name, threads);
        g_in_table_call = false;
        return rc;
    }
    KilnHandle watch(char* name = kSub, KilnOwner owner = kOwnerWatch) {
        KilnHandle h = 0;
        CHECK(core.subscribe(owner, &on_changed, name, &h) == KILN_OK && h);
        return h;
    }
    // An applied mode lease of `owner` for `player`.
    KilnCameraLease applied(KilnOwner owner, uint32_t player, int32_t priority, char* name);
};

KilnCameraRequest mode_request(uint32_t player, int32_t priority = 0, uint8_t transition = KILN_CAMERA_TRANSITION_LERP) {
    KilnCameraRequest r{};
    r.size = sizeof r;
    r.parts = KILN_CAMERA_MODE;
    r.endpoint = KILN_CAMERA_SERVER_MODE;
    r.player_entity = player;
    r.input_policy = KILN_CAMERA_STOCK_MODE;
    r.duration_ms = 120000;
    r.priority = priority;
    r.camera_id = 34;
    r.transition_override = transition;
    return r;
}

KilnCameraRequest lens_request(float degrees = 40) {
    KilnCameraRequest r{};
    r.size = sizeof r;
    r.parts = KILN_CAMERA_LENS;
    r.endpoint = KILN_CAMERA_LOCAL_VISUAL;
    r.duration_ms = 10000;
    r.lens_source = KILN_CAMERA_LENS_CONSTANT;
    r.fov_y_degrees = degrees;
    return r;
}

KilnCameraKey key(uint32_t at, double x) {
    KilnCameraKey k{};
    k.at_ms = at;
    k.pose.position[0] = x;
    k.pose.rotation[3] = 1;
    return k;
}

KilnCameraRequest profile_request() {
    KilnCameraRequest r{};
    r.size = sizeof r;
    r.parts = KILN_CAMERA_PROFILE;
    r.endpoint = KILN_CAMERA_LOCAL_VISUAL;
    r.duration_ms = 10000;
    r.profile.size = sizeof r.profile;
    r.profile.distance = 8;
    r.profile.rotation[3] = 1;
    return r;
}

KilnCameraLease Rig::applied(KilnOwner owner, uint32_t player, int32_t priority, char* name) {
    KilnCameraLease lease = 0;
    CHECK(apply(owner, mode_request(player, priority), &lease, name) == KILN_OK && lease);
    CHECK(core.committed(lease, 1, 100));
    core.pre(kServer);
    g_log.clear();
    mode.calls.clear();
    return lease;
}

bool valid(const KilnCameraRequest& r) {
    Stored s;
    return validate(&r, &s);
}

// ---- the request's shape and numbers ----------------------------------------------------------------------------------

void test_validation() {
    CHECK(valid(mode_request(1)));
    CHECK(valid(lens_request()));
    CHECK(valid(profile_request()));
    Stored s;
    CHECK(!validate(nullptr, &s));
    {
        auto r = lens_request();
        r.size = sizeof r - 1;
        CHECK(!valid(r));  // smaller than v1
        r.size = sizeof r + 16;
        CHECK(valid(r));   // a later, larger struct is read as v1
    }
    {
        auto r = lens_request();
        r.parts = 0;
        CHECK(!valid(r));
        r.parts = KILN_CAMERA_LENS | 32;
        CHECK(!valid(r));  // an unknown part bit
        r = lens_request();
        r.endpoint = 0;
        CHECK(!valid(r));
        r.endpoint = 4;
        CHECK(!valid(r));
        r = lens_request();
        r.reserved[1] = 1;
        CHECK(!valid(r));
    }
    {   // time, priority
        auto r = lens_request();
        r.duration_ms = 0;
        CHECK(!valid(r));
        r.duration_ms = 120001;
        CHECK(!valid(r));
        r.duration_ms = 120000;
        CHECK(valid(r));
        r.blend_in_ms = 60000, r.blend_out_ms = 60001;
        CHECK(!valid(r));
        r.blend_out_ms = 60000;
        CHECK(valid(r));
        r.priority = 1001;
        CHECK(!valid(r));
        r.priority = -1001;
        CHECK(!valid(r));
        r.priority = -1000;
        CHECK(valid(r));
    }
    {   // a player is required where the endpoint is somebody else's camera
        auto r = mode_request(0);
        CHECK(!valid(r));
        r = lens_request();
        r.endpoint = KILN_CAMERA_REMOTE_VISUAL;
        CHECK(!valid(r));
        r.player_entity = 3;
        CHECK(valid(r));
    }
    {   // mode stands alone, with the stock policy and no blends
        auto r = mode_request(1);
        r.parts |= KILN_CAMERA_SHAKE;
        CHECK(!valid(r));
        r = mode_request(1);
        r.input_policy = KILN_CAMERA_VISUAL_ONLY;
        CHECK(!valid(r));
        r.input_policy = KILN_CAMERA_AIM_TOO;
        CHECK(valid(r));  // reserved: accepted here, refused INPUT_POLICY by the core
        r = mode_request(1);
        r.camera_id = 46;
        CHECK(!valid(r));
        r.camera_id = 45;
        CHECK(valid(r));
        r.transition_override = 3;
        CHECK(!valid(r));
        r = mode_request(1);
        r.blend_in_ms = 1;
        CHECK(!valid(r));
        r = lens_request();
        r.input_policy = KILN_CAMERA_STOCK_MODE;
        CHECK(!valid(r));  // a visual part with the stock policy
    }
    {   // lens: 1 to 179 degrees, the whole degrees the projection takes (0.01 rad from 0 and from 180)
        auto r = lens_request(0.99f);
        CHECK(!valid(r));
        r.fov_y_degrees = 179.01f;
        CHECK(!valid(r));
        r.fov_y_degrees = NAN;
        CHECK(!valid(r));
        r.fov_y_degrees = INFINITY;
        CHECK(!valid(r));
        r.fov_y_degrees = 0;
        CHECK(!valid(r));
        for (float d : {1.0f, 2.5f, 4.99f, 160.01f, 179.0f}) {
            r.fov_y_degrees = d;
            CHECK(valid(r));
        }
        r.fov_y_degrees = 160;
        CHECK(valid(r));
        r.lens_source = KILN_CAMERA_LENS_NONE;
        CHECK(!valid(r));  // the LENS part without a source
        r = profile_request();
        r.fov_y_degrees = 40;
        CHECK(!valid(r));  // a lens value without the LENS part
        r = lens_request();
        r.lens_source = KILN_CAMERA_LENS_KEYS;
        r.fov_y_degrees = 0;
        CHECK(!valid(r));  // keyed lens without a path
    }
    {   // path
        KilnCameraKey keys[3] = {key(0, 1), key(500, 2), key(1000, 3)};
        KilnCameraRequest r{};
        r.size = sizeof r;
        r.parts = KILN_CAMERA_PATH;
        r.endpoint = KILN_CAMERA_LOCAL_VISUAL;
        r.duration_ms = 1000;
        r.keys = keys;
        r.key_count = 3;
        CHECK(valid(r));
        r.key_count = 0;
        CHECK(!valid(r));
        r.key_count = 257;
        CHECK(!valid(r));
        r.key_count = 3;
        r.keys = nullptr;
        CHECK(!valid(r));
        r.keys = keys;
        keys[0].at_ms = 1;
        CHECK(!valid(r));  // the first key is at 0
        keys[0].at_ms = 0;
        keys[2].at_ms = 500;
        CHECK(!valid(r));  // strictly increasing
        keys[2].at_ms = 1001;
        CHECK(!valid(r));  // the last key is inside the duration
        keys[2].at_ms = 1000;
        keys[1].pose.position[1] = 1000001;
        CHECK(!valid(r));
        keys[1].pose.position[1] = INFINITY;
        CHECK(!valid(r));
        keys[1].pose.position[1] = 0;
        keys[1].pose.rotation[3] = 1.01f;
        CHECK(!valid(r));  // not a unit quaternion
        keys[1].pose.rotation[3] = 1.0004f;
        CHECK(valid(r));
        keys[1].pose.rotation[3] = 1;
        keys[1].reserved2 = 1;
        CHECK(!valid(r));
        keys[1].reserved2 = 0;
        keys[1].fov_y_degrees = 50;
        CHECK(!valid(r));  // a key lens without LENS_KEYS
        r.parts |= KILN_CAMERA_LENS;
        r.lens_source = KILN_CAMERA_LENS_KEYS;
        CHECK(!valid(r));  // every key has a lens, or none
        keys[0].fov_y_degrees = keys[2].fov_y_degrees = 50;
        CHECK(valid(r));
        keys[1].fov_y_degrees = 175;
        CHECK(valid(r));  // keyed lenses: the same 1 to 179 degrees
        keys[1].fov_y_degrees = 179.5f;
        CHECK(!valid(r));
        keys[1].fov_y_degrees = 50;
        r.fov_y_degrees = 40;
        CHECK(!valid(r));  // a static lens and keyed lenses
        r.fov_y_degrees = 0;
        r.parts |= KILN_CAMERA_PROFILE;
        r.profile = profile_request().profile;
        CHECK(!valid(r));  // path and profile exclude each other
        // the stored copy is normalized and owns its keys
        r.parts = KILN_CAMERA_PATH | KILN_CAMERA_LENS;
        keys[1].pose.rotation[3] = 1.0004f;
        Stored kept;
        CHECK(validate(&r, &kept));
        CHECK(kept.keys.size() == 3 && kept.req.keys == kept.keys.data() && kept.req.keys != keys);
        CHECK(std::fabs(kept.keys[1].pose.rotation[3] - 1.0f) < 1e-6f);
        r = lens_request();
        r.key_count = 1;
        CHECK(!valid(r));  // keys without the PATH part
    }
    {   // profile
        auto r = profile_request();
        r.profile.size = sizeof r.profile - 4;
        CHECK(!valid(r));
        r = profile_request();
        // no size caps (0.2.0-dev.31.25): any finite distance of 0 or more, any finite offsets; the render path checks
        // the camera that comes out against the world bound
        for (float d : {0.0f, 0.24f, 200.5f, 300.0f, 5000.0f}) {
            r.profile.distance = d;
            CHECK(valid(r));
        }
        r.profile.distance = -0.01f;
        CHECK(!valid(r));  // a distance is not negative
        r.profile.distance = INFINITY;
        CHECK(!valid(r));
        r.profile.distance = NAN;
        CHECK(!valid(r));
        r.profile.distance = 200;
        r.profile.offset_local[0] = 80, r.profile.offset_local[1] = 80;
        CHECK(valid(r));  // more than 100 m long
        r.profile.offset_local[2] = 4000;
        CHECK(valid(r));
        r.profile.offset_local[2] = NAN;
        CHECK(!valid(r));
        r.profile.offset_local[2] = -INFINITY;
        CHECK(!valid(r));
        r.profile.offset_local[0] = r.profile.offset_local[1] = r.profile.offset_local[2] = 0;
        CHECK(valid(r));
        for (float y : {-100.5f, 277.0f, -2000.0f}) {
            r.profile.offset_world_y = y;
            CHECK(valid(r));
        }
        r.profile.offset_world_y = INFINITY;
        CHECK(!valid(r));
        r.profile.offset_world_y = 0;
        r.profile.collision_policy = 1;
        CHECK(!valid(r));
        r.profile.collision_policy = 0;
        r.profile.rotation_mode = 3;
        CHECK(!valid(r));
        r.profile.rotation_mode = KILN_CAMERA_FREE_ORBIT;
        r.profile.rotation[0] = 0.6f, r.profile.rotation[3] = 0.8f;
        CHECK(!valid(r));  // a free orbit takes only identity
        r.profile.rotation_mode = KILN_CAMERA_FOLLOW_HEADING;
        CHECK(valid(r));
        // what no part selects is not read, and is zero in the stored copy
        r = lens_request();
        r.profile.distance = NAN;
        r.shake.amplitude_metres = 99;
        r.camera_id = 200;
        Stored kept;
        CHECK(validate(&r, &kept));
        CHECK(kept.req.profile.distance == 0 && kept.req.profile.rotation[3] == 1 && kept.req.shake.amplitude_metres == 0 && kept.req.camera_id == 0);
    }
    {   // shake
        auto r = lens_request();
        r.parts |= KILN_CAMERA_SHAKE;
        r.shake.amplitude_metres = 0.5f, r.shake.frequency_hz = 30, r.shake.falloff_ms = 10000;
        CHECK(valid(r));
        // no caps on strength or speed (0.2.0-dev.31.25): finite, 0 or more
        r.shake.amplitude_metres = 0.51f;
        CHECK(valid(r));
        r.shake.amplitude_metres = 5, r.shake.frequency_hz = 60;
        CHECK(valid(r));
        r.shake.amplitude_metres = 0.1f, r.shake.frequency_hz = 0.09f;
        CHECK(valid(r));
        r.shake.frequency_hz = 0;
        CHECK(valid(r));
        r.shake.frequency_hz = -1;
        CHECK(!valid(r));
        r.shake.frequency_hz = INFINITY;
        CHECK(!valid(r));
        r.shake.frequency_hz = 1, r.shake.amplitude_metres = -0.01f;
        CHECK(!valid(r));
        r.shake.amplitude_metres = NAN;
        CHECK(!valid(r));
        r.shake.amplitude_metres = 0.1f, r.shake.falloff_ms = 10001;
        CHECK(!valid(r));
    }
    // the rows a request needs
    CHECK(required_caps(mode_request(1)) == KILN_CAMERA_CAP_SERVER_MODE);
    CHECK(required_caps(lens_request()) == KILN_CAMERA_CAP_LOCAL_FOV);  // a lens needs no pose row
    auto both = profile_request();
    both.parts |= KILN_CAMERA_SHAKE | KILN_CAMERA_LENS;
    CHECK(required_caps(both) == (KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE | KILN_CAMERA_CAP_LOCAL_FOV));
    auto remote = lens_request();
    remote.endpoint = KILN_CAMERA_REMOTE_VISUAL;
    CHECK(required_caps(remote) == KILN_CAMERA_CAP_REMOTE);
    for (uint32_t code = 1; code <= KILN_CAMERA_REASON_BODY_TOO_LARGE; ++code) CHECK(*reason_text(code));
}

// ---- immediate returns ------------------------------------------------------------------------------------------------

void test_immediate_returns() {
    Rig t;
    KilnCameraLease lease = 0;
    auto bad = mode_request(1);
    bad.camera_id = 99;
    CHECK(t.apply(kOwnerA, bad, &lease, kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.core.apply(kOwnerA, nullptr, &lease, &on_done, kA, kOnServer) == KILN_ERR_INVALID_ARGUMENT);
    // the endpoint's world thread
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA, kOnClient) == KILN_ERR_WRONG_THREAD);
    CHECK(t.apply(kOwnerA, lens_request(), &lease, kA, kOnServer) == KILN_ERR_WRONG_THREAD);
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA, 0) == KILN_ERR_WRONG_THREAD);
    // a missing row: the lens row is off, a profile still works
    t.local.bits &= ~KILN_CAMERA_CAP_LOCAL_FOV;
    CHECK(t.apply(kOwnerA, lens_request(), &lease, kA) == KILN_ERR_UNAVAILABLE);
    CHECK(t.apply(kOwnerA, profile_request(), &lease, kA) == KILN_OK);
    // an endpoint without a backend
    auto remote = lens_request();
    remote.endpoint = KILN_CAMERA_REMOTE_VISUAL, remote.player_entity = 2;
    CHECK(t.apply(kOwnerA, remote, &lease, kA) == KILN_ERR_UNAVAILABLE);
    // no world of that kind
    t.core.world_stopping(kServer, 1);
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_ERR_NOT_FOUND);
    // the backend's immediate answer (no such player)
    t.core.world_started(kServer, 2);
    t.mode.admit_rc = KILN_ERR_NOT_FOUND;
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_ERR_NOT_FOUND);
    CHECK(t.core.pending() == 1);  // only the profile apply waits
    CHECK(g_log.empty());

    // without the owner-retiring observer nothing that holds a callback is admitted
    Rig off;
    off.core.set_writes(false);
    KilnHandle h = 0;
    uint64_t mask = 99;
    CHECK(off.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_ERR_UNAVAILABLE);
    CHECK(off.end(kOwnerA, 1, kA) == KILN_ERR_UNAVAILABLE);
    CHECK(off.core.subscribe(kOwnerA, &on_changed, kSub, &h) == KILN_ERR_UNAVAILABLE);
    CHECK(off.core.capabilities(KILN_CAMERA_SERVER_MODE, 0, &mask) == KILN_OK && mask == 0);
    off.core.set_local_read_caps(KILN_CAMERA_CAP_RENDERED_SNAPSHOT);
    CHECK(off.core.capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &mask) == KILN_OK && mask == KILN_CAMERA_CAP_RENDERED_SNAPSHOT);  // the read survives
    off.core.set_writes(true);
    CHECK(off.core.capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &mask) == KILN_OK && mask == (off.local.bits | KILN_CAMERA_CAP_RENDERED_SNAPSHOT));
    CHECK(off.core.capabilities(KILN_CAMERA_SERVER_MODE, 0, &mask) == KILN_OK && mask == KILN_CAMERA_CAP_SERVER_MODE);
    CHECK(off.core.capabilities(0, 0, &mask) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(off.core.capabilities(KILN_CAMERA_SERVER_MODE, 0, nullptr) == KILN_ERR_INVALID_ARGUMENT);
}

// ---- apply, the answer and the event ----------------------------------------------------------------------------------

void test_apply_done_then_changed() {
    Rig t;
    t.watch();
    KilnCameraLease lease = 0;
    CHECK(t.apply(kOwnerA, mode_request(5), &lease, kA) == KILN_OK);
    CHECK(lease == 1);  // known at once, before anything is applied
    CHECK(t.mode.calls == std::vector<std::string>{"stage 1/1"});
    CHECK(t.mode.staged[0].owner == kOwnerA && t.mode.staged[0].epoch == 1 && !t.mode.staged[0].update && t.mode.staged[0].replaces == 0);
    t.core.pre(kServer);
    CHECK(g_log.empty());  // accepted is not applied: nothing is said until the backend commits
    CHECK(t.core.active_revision(lease) == 0 && t.core.pending() == 1);
    CHECK(t.core.committed(lease, 1, 77));
    CHECK(g_log.empty());  // and never inside the backend's report: on the world's Pre
    t.core.pre(kClient);
    CHECK(g_log.empty());  // the server world's answers do not go out on the client world's Pre
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=1 ok=1 reason=0 rev=1 flags=0 scope=1", "event sub applied lease=1 rev=1 end=0 gen=1"}));
    CHECK(t.core.pending() == 0 && t.core.active_revision(lease) == 1);
    t.core.pre(kServer);
    CHECK(g_log.size() == 2);  // once
    CHECK(!t.core.committed(lease, 1, 78));  // a second report of the same revision is stale
    t.core.pre(kServer);
    CHECK(g_log.size() == 2);

    // a visual lease answers with the client-frame scope, on the client world
    KilnCameraLease lens = 0;
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 1, 5));
    g_log.clear();
    t.core.pre(kServer);
    CHECK(g_log.empty());
    t.core.pre(kClient);
    CHECK((g_log == std::vector<std::string>{"done A lease=2 ok=1 reason=0 rev=1 flags=0 scope=2", "event sub applied lease=2 rev=1 end=0 gen=1"}));
}

void test_refusals_through_done() {
    Rig t;
    t.watch();
    KilnCameraLease lease = 0;
    // a mode request on a visual endpoint, and a visual one on the mode endpoint
    auto wrong = mode_request(1);
    wrong.endpoint = KILN_CAMERA_LOCAL_VISUAL;
    CHECK(t.apply(kOwnerA, wrong, &lease, kA) == KILN_OK);
    auto wrong2 = lens_request();
    wrong2.endpoint = KILN_CAMERA_SERVER_MODE, wrong2.player_entity = 1;
    CHECK(t.apply(kOwnerA, wrong2, &lease, kA) == KILN_OK);
    // the reserved policy
    auto aim = lens_request();
    aim.input_policy = KILN_CAMERA_AIM_TOO;
    CHECK(t.apply(kOwnerA, aim, &lease, kA) == KILN_OK);
    // the backend's own refusals: before staging and at staging
    t.mode.admit_reason = KILN_CAMERA_REASON_PEER_UNAVAILABLE;
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_OK);
    t.mode.admit_reason = 0;
    t.mode.stage_reason = KILN_CAMERA_REASON_TARGET_UNAVAILABLE;
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_OK);
    CHECK(t.mode.calls.empty() && t.local.calls.empty() && t.core.live_leases() == 0);
    t.core.pre(kClient);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=0 reason=5 rev=0 flags=0 scope=0",
                       "done A lease=3 ok=0 reason=6 rev=0 flags=0 scope=0",
                       "done A lease=2 ok=0 reason=5 rev=0 flags=0 scope=0",
                       "done A lease=4 ok=0 reason=8 rev=0 flags=0 scope=0",
                       "done A lease=5 ok=0 reason=10 rev=0 flags=0 scope=0",
                   }));
    // a refused apply never was a lease: its owner's end() is answered "already ended", and nobody hears of it
    g_log.clear();
    CHECK(t.end(kOwnerA, 4, kA) == KILN_OK);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=4 ok=1 reason=0 rev=0 flags=2 scope=0"}));
    // a backend that rejects a staged apply
    g_log.clear();
    t.mode.stage_reason = 0;
    CHECK(t.apply(kOwnerA, mode_request(1), &lease, kA) == KILN_OK);
    CHECK(t.core.rejected(lease, 1, KILN_CAMERA_REASON_FRAME_FAULT, KILN_CAMERA_MAY_HAVE_APPLIED, "a later writer changed the frame"));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=6 ok=0 reason=18 rev=0 flags=1 scope=0"}));
    CHECK(t.core.live_leases() == 0 && !t.core.committed(lease, 1, 1));
}

// ---- one owner, two owners, priorities --------------------------------------------------------------------------------

void test_already_owned_and_busy() {
    Rig t;
    t.watch();
    KilnCameraLease a = 0, again = 0, other = 0;
    CHECK(t.apply(kOwnerA, mode_request(5, 10), &a, kA) == KILN_OK);
    // while its own apply still waits, and after it holds: use update
    CHECK(t.apply(kOwnerA, mode_request(5, 500), &again, kA) == KILN_OK);
    // another owner while an apply is in flight on that player: busy, whatever its priority
    CHECK(t.apply(kOwnerB, mode_request(5, 1000), &other, kB) == KILN_OK);
    CHECK(t.core.committed(a, 1, 1));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=2 ok=0 reason=2 rev=0 flags=0 scope=0",
                       "done B lease=3 ok=0 reason=1 rev=0 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=1 rev=1 end=0 gen=1",
                   }));
    g_log.clear();
    CHECK(t.apply(kOwnerA, mode_request(5, 500), &again, kA) == KILN_OK);   // holds it: already owned
    CHECK(t.apply(kOwnerB, mode_request(5, 10), &other, kB) == KILN_OK);    // equal priority: busy
    CHECK(t.apply(kOwnerB, mode_request(5, 9), &other, kB) == KILN_OK);     // lower: busy
    // another player is another slot
    KilnCameraLease elsewhere = 0;
    CHECK(t.apply(kOwnerB, mode_request(6, -5), &elsewhere, kB) == KILN_OK);
    CHECK(t.core.committed(elsewhere, 1, 2));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=4 ok=0 reason=2 rev=0 flags=0 scope=0",
                       "done B lease=5 ok=0 reason=1 rev=0 flags=0 scope=0",
                       "done B lease=6 ok=0 reason=1 rev=0 flags=0 scope=0",
                       "done B lease=7 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=7 rev=1 end=0 gen=1",
                   }));
    CHECK(t.core.active_revision(a) == 1);  // the refusals left the holder alone
    // the mode domain and the visual domain are separate: the same owner may hold one of each
    KilnCameraLease lens = 0;
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 1, 3));
    CHECK(t.core.live_leases() == 3);
}

void test_supersede_old_ended_before_new_applied() {
    Rig t;
    t.watch();
    const KilnCameraLease a = t.applied(kOwnerA, 5, 10, kA);
    KilnCameraLease b = 0;
    CHECK(t.apply(kOwnerB, mode_request(5, 11), &b, kB) == KILN_OK);
    CHECK(t.mode.staged.back().replaces == a);
    t.core.pre(kServer);
    CHECK(g_log.empty() && t.core.active_revision(a) == 1);  // while the candidate is decided the old lease stays
    // the holder cannot update under a staged candidate: one apply or update in flight per endpoint
    CHECK(t.update(kOwnerA, a, mode_request(5, 10, 0), kA) == KILN_OK);
    CHECK(t.core.committed(b, 1, 9));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=0 reason=1 rev=1 flags=0 scope=0",
                       "event sub ended lease=1 rev=1 end=6 gen=1",
                       "done B lease=2 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=2 rev=1 end=0 gen=1",
                   }));
    // the old lease's id can do nothing to its successor
    g_log.clear();
    CHECK(!t.core.ended(a, 1, KILN_CAMERA_END_EXPIRED, 0, 0, nullptr));
    CHECK(!t.core.released(a, 0));
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=1 ok=1 reason=0 rev=0 flags=2 scope=0"}));
    CHECK(t.core.active_revision(b) == 1);

    // a candidate that fails leaves the old lease as it was
    Rig u;
    u.watch();
    const KilnCameraLease keep = u.applied(kOwnerA, 5, 10, kA);
    CHECK(u.apply(kOwnerB, mode_request(5, 11), &b, kB) == KILN_OK);
    CHECK(u.core.rejected(b, 1, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, nullptr));
    u.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done B lease=2 ok=0 reason=8 rev=0 flags=0 scope=0"}));
    CHECK(u.core.active_revision(keep) == 1);
}

// ---- update ------------------------------------------------------------------------------------------------------------

void test_update_and_stale_revisions() {
    Rig t;
    t.watch();
    const KilnCameraLease a = t.applied(kOwnerA, 5, 0, kA);
    CHECK(t.update(kOwnerA, a, mode_request(5, 0, 0), kA, KILN_CAMERA_RESTART_TIMELINE) == KILN_OK);
    CHECK(t.mode.staged.back().update && t.mode.staged.back().revision == 2 && t.mode.staged.back().timing == KILN_CAMERA_RESTART_TIMELINE);
    CHECK(t.update(kOwnerA, a, mode_request(5), kA) == KILN_OK);  // a second update while one waits: busy
    CHECK(!t.core.committed(a, 1, 1));  // the old revision cannot commit again
    CHECK(!t.core.committed(a, 3, 1));  // nor one that was never staged
    CHECK(t.core.active_revision(a) == 1);
    CHECK(t.core.committed(a, 2, 50));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=0 reason=1 rev=1 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=2 flags=0 scope=1",
                       "event sub updated lease=1 rev=2 end=0 gen=1",
                   }));  // exactly one Updated, no new Applied
    // a refused update keeps the old revision, and the next one gets a new number
    g_log.clear();
    CHECK(t.update(kOwnerA, a, mode_request(5), kA) == KILN_OK);
    CHECK(t.mode.staged.back().revision == 3);
    CHECK(t.core.rejected(a, 3, KILN_CAMERA_REASON_INSUFFICIENT_LIFETIME, 0, nullptr));
    CHECK(!t.core.ended(a, 3, KILN_CAMERA_END_EXPIRED, 0, 0, nullptr));  // an end of a revision that never applied is stale
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=1 ok=0 reason=26 rev=2 flags=0 scope=0"}));
    CHECK(t.core.active_revision(a) == 2);
    CHECK(t.update(kOwnerA, a, mode_request(5), kA) == KILN_OK);
    CHECK(t.mode.staged.back().revision == 4);
    CHECK(t.core.committed(a, 4, 51));

    // who may update what
    g_log.clear();
    t.core.pre(kServer);
    g_log.clear();
    CHECK(t.update(kOwnerB, a, mode_request(5), kB) == KILN_OK);        // another owner's lease
    CHECK(t.update(kOwnerA, 999, mode_request(5), kA) == KILN_OK);      // no such lease
    CHECK(t.update(kOwnerA, a, mode_request(6), kA) == KILN_OK);        // another player
    auto visual = lens_request();
    visual.endpoint = KILN_CAMERA_SERVER_MODE, visual.player_entity = 5;
    CHECK(t.update(kOwnerA, a, visual, kA) == KILN_OK);                 // a mode lease stays a mode lease
    auto aim = mode_request(5);
    aim.input_policy = KILN_CAMERA_AIM_TOO;
    CHECK(t.update(kOwnerA, a, aim, kA) == KILN_OK);
    CHECK(t.core.update(kOwnerA, a, 2, &aim, &on_done, kA, kOnServer) == KILN_ERR_INVALID_ARGUMENT);  // no such timing
    CHECK(t.core.update(kOwnerA, a, 0, &aim, &on_done, kA, kOnClient) == KILN_ERR_WRONG_THREAD);       // the lease's world
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done B lease=1 ok=0 reason=4 rev=0 flags=0 scope=0",
                       "done A lease=999 ok=0 reason=3 rev=0 flags=0 scope=0",
                       "done A lease=1 ok=0 reason=5 rev=4 flags=0 scope=0",
                       "done A lease=1 ok=0 reason=23 rev=4 flags=0 scope=0",
                       "done A lease=1 ok=0 reason=6 rev=4 flags=0 scope=0",
                   }));
    CHECK(t.core.active_revision(a) == 4);

    // an update needs the rows of its new, whole request: a missing one is the immediate answer and changes nothing
    KilnCameraLease lens = 0;
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 1, 1));
    t.local.bits &= ~KILN_CAMERA_CAP_LOCAL_SHAKE;
    auto shaky = lens_request();
    shaky.parts |= KILN_CAMERA_SHAKE;
    shaky.shake.amplitude_metres = 0.1f, shaky.shake.frequency_hz = 2;
    CHECK(t.update(kOwnerA, lens, shaky, kA) == KILN_ERR_UNAVAILABLE);
    CHECK(t.core.active_revision(lens) == 1);
    // the local player's own id, or 0, names the local endpoint; another id does not
    auto mine = lens_request(50);
    mine.player_entity = 7;
    CHECK(t.update(kOwnerA, lens, mine, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 2, 2));
    auto theirs = lens_request(50);
    theirs.player_entity = 8;
    g_log.clear();
    t.core.pre(kClient);
    g_log.clear();
    CHECK(t.update(kOwnerA, lens, theirs, kA) == KILN_OK);
    t.core.pre(kClient);
    CHECK((g_log == std::vector<std::string>{"done A lease=2 ok=0 reason=5 rev=2 flags=0 scope=0"}));
}

// ---- end ---------------------------------------------------------------------------------------------------------------

void test_end_is_idempotent() {
    Rig t;
    t.watch();
    const KilnCameraLease a = t.applied(kOwnerA, 5, 0, kA);
    CHECK(t.end(kOwnerB, a, kB) == KILN_OK);   // not its lease
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);
    CHECK(t.mode.calls == std::vector<std::string>{"release 1/1"});
    CHECK(g_log.empty());                      // the answer and the event wait for the world's Pre
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);   // again, at once
    t.core.pre(kServer);
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);   // and later
    CHECK(t.end(kOwnerB, a, kB) == KILN_OK);   // a tombstone keeps its owner
    CHECK(t.end(kOwnerA, 4242, kA) == KILN_OK);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done B lease=1 ok=0 reason=4 rev=0 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=1 flags=0 scope=0",
                       "event sub ended lease=1 rev=1 end=1 gen=1",
                       "done A lease=1 ok=1 reason=0 rev=0 flags=2 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=0 flags=2 scope=0",
                       "done B lease=1 ok=0 reason=4 rev=0 flags=0 scope=0",
                       "done A lease=4242 ok=0 reason=3 rev=0 flags=0 scope=0",
                   }));  // one Ended(Released), however often it is ended
    CHECK(t.mode.calls.size() == 1 && t.core.live_leases() == 0 && t.core.pending() == 0);
    CHECK(t.core.end(kOwnerA, 0, &on_done, kA, kOnServer) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.core.end(kOwnerA, 4242, &on_done, kA, 0) == KILN_ERR_WRONG_THREAD);
}

void test_end_waits_for_the_release() {
    Rig t;
    t.watch();
    KilnCameraLease lens = 0;
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 1, 1));
    t.core.pre(kClient);
    g_log.clear();
    t.local.release_now = false;  // a blend out: the release comes frames later
    CHECK(t.end(kOwnerA, lens, kA) == KILN_OK);
    CHECK(t.end(kOwnerA, lens, kA) == KILN_OK);  // a second end() joins the first
    t.core.pre(kClient);
    CHECK(g_log.empty());  // not on queueing: only after the release frame
    // while it is ending the lease takes no update, and another owner still has to outrank it
    CHECK(t.update(kOwnerA, lens, lens_request(60), kA) == KILN_OK);
    KilnCameraLease other = 0;
    CHECK(t.apply(kOwnerB, lens_request(), &other, kB) == KILN_OK);
    CHECK(t.core.released(lens, 321));
    CHECK(!t.core.released(lens, 322));
    t.core.pre(kClient);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=0 reason=3 rev=1 flags=0 scope=0",
                       "done B lease=2 ok=0 reason=1 rev=0 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=1 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=1 flags=2 scope=0",
                       "event sub ended lease=1 rev=1 end=1 gen=1",
                   }));
}

void test_end_cancels_pending_work() {
    Rig t;
    t.watch();
    // an apply that still waits: its own callback says Cancelled, the lease never shows as applied
    KilnCameraLease a = 0;
    CHECK(t.apply(kOwnerA, mode_request(5), &a, kA) == KILN_OK);
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);
    CHECK((t.mode.calls == std::vector<std::string>{"stage 1/1", "drop 1"}));
    CHECK(!t.core.committed(a, 1, 1));  // the backend's late report finds nothing
    CHECK(!t.core.rejected(a, 1, KILN_CAMERA_REASON_BUSY, 0, nullptr));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=0 reason=24 rev=0 flags=0 scope=0",
                       "done A lease=1 ok=1 reason=0 rev=0 flags=0 scope=0",
                       "event sub ended lease=1 rev=0 end=13 gen=1",
                   }));
    // an update that still waits: cancelled first, then the release; its revision can never apply
    const KilnCameraLease b = t.applied(kOwnerA, 5, 0, kA);
    CHECK(t.update(kOwnerA, b, mode_request(5, 0, 0), kA) == KILN_OK);
    t.mode.calls.clear();
    t.mode.release_now = false;
    CHECK(t.end(kOwnerA, b, kA) == KILN_OK);
    CHECK((t.mode.calls == std::vector<std::string>{"cancel_update 2/2", "release 2/1"}));  // cancelled before the release is asked for
    CHECK(!t.core.committed(b, 2, 1));
    CHECK(t.core.active_revision(b) == 1);
    CHECK(t.core.released(b, 9));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=2 ok=0 reason=24 rev=1 flags=0 scope=0",
                       "done A lease=2 ok=1 reason=0 rev=1 flags=0 scope=0",
                       "event sub ended lease=2 rev=1 end=1 gen=1",
                   }));  // no late Updated
    // ending the holder leaves another owner's candidate for the slot staged: it applies without a Superseded
    t.mode.release_now = true;
    const KilnCameraLease c = t.applied(kOwnerA, 5, 0, kA);
    KilnCameraLease d = 0;
    CHECK(t.apply(kOwnerB, mode_request(5, 1), &d, kB) == KILN_OK);
    CHECK(t.end(kOwnerA, c, kA) == KILN_OK);
    CHECK(t.core.committed(d, 1, 4));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=3 ok=1 reason=0 rev=1 flags=0 scope=0",
                       "event sub ended lease=3 rev=1 end=1 gen=1",
                       "done B lease=4 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=4 rev=1 end=0 gen=1",
                   }));
}

// ---- ends the backend reports ------------------------------------------------------------------------------------------

void test_backend_ends() {
    Rig t;
    t.watch();
    const KilnCameraLease a = t.applied(kOwnerA, 5, 0, kA);
    CHECK(!t.core.ended(a, 2, KILN_CAMERA_END_EXPIRED, 0, 0, nullptr));  // not the applied revision: ignored
    CHECK(!t.core.ended(a, 1, KILN_CAMERA_END_NONE, 0, 0, nullptr));     // not an end reason
    CHECK(t.core.active_revision(a) == 1);
    CHECK(t.update(kOwnerA, a, mode_request(5), kA) == KILN_OK);         // waits when the lease expires
    CHECK(t.core.ended(a, 1, KILN_CAMERA_END_EXPIRED, 0, 60, nullptr));
    CHECK(!t.core.ended(a, 1, KILN_CAMERA_END_EXPIRED, 0, 61, nullptr)); // once
    CHECK(!t.core.committed(a, 2, 61));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=1 ok=0 reason=13 rev=1 flags=0 scope=0", "event sub ended lease=1 rev=1 end=2 gen=1"}));
    g_log.clear();
    const KilnCameraLease b = t.applied(kOwnerA, 6, 0, kA);
    CHECK(t.core.ended(b, 1, KILN_CAMERA_END_PLAYER_LEFT, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, "the player left"));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"event sub ended lease=2 rev=1 end=4 gen=1"}));
    // a staged lease does not "end": the backend rejects it
    KilnCameraLease c = 0;
    CHECK(t.apply(kOwnerA, mode_request(7), &c, kA) == KILN_OK);
    CHECK(!t.core.ended(c, 1, KILN_CAMERA_END_PLAYER_LEFT, 0, 0, nullptr));
    CHECK(t.core.live_leases() == 1);
}

// ---- the queue ---------------------------------------------------------------------------------------------------------

void test_queue_64_backpressure() {
    Rig t;
    KilnCameraLease lease = 0;
    for (uint32_t i = 0; i < KILN_CAMERA_MAX_PENDING; ++i) CHECK(t.apply(kOwnerA, mode_request(1 + (i % 16)), &lease, kA) == KILN_OK);
    CHECK(t.core.pending() == KILN_CAMERA_MAX_PENDING);
    const KilnCameraLease last = lease;
    lease = 0;
    CHECK(t.apply(kOwnerB, mode_request(1), &lease, kB) == KILN_ERR_BUFFER_TOO_SMALL);  // the 65th: refused before it is accepted
    CHECK(lease == 0 && t.core.pending() == KILN_CAMERA_MAX_PENDING);
    CHECK(t.update(kOwnerA, 1, mode_request(1), kA) == KILN_ERR_BUFFER_TOO_SMALL);
    // end() still gets through, so the queue can be cleared; it has its own bound
    CHECK(t.end(kOwnerA, 1, kA) == KILN_OK);
    for (uint32_t i = t.core.pending(); i < kMaxPendingWithEnds; ++i) CHECK(t.end(kOwnerA, last, kA) == KILN_OK);
    CHECK(t.end(kOwnerA, last, kA) == KILN_ERR_BUFFER_TOO_SMALL);
    CHECK(g_log.empty());  // no callback came out of a refused admission
    t.core.pre(kServer);
    CHECK(t.core.pending() == 15 && g_log.size() == kMaxPendingWithEnds - 15 + 0);  // the 15 staged applies of players 2..16 still wait
    CHECK(t.apply(kOwnerB, mode_request(1), &lease, kB) == KILN_OK);  // room again

    // 16 players may have a mode slot at a time
    Rig u;
    for (uint32_t p = 1; p <= kMaxPlayerSlots; ++p) CHECK(u.apply(kOwnerA, mode_request(p), &lease, kA) == KILN_OK);
    CHECK(u.apply(kOwnerA, mode_request(17), &lease, kA) == KILN_ERR_BUFFER_TOO_SMALL);
    CHECK(u.end(kOwnerA, lease, kA) == KILN_OK);  // player 16's slot frees
    CHECK(u.apply(kOwnerA, mode_request(17), &lease, kA) == KILN_OK);
}

// ---- owners that go ----------------------------------------------------------------------------------------------------

void test_owner_retiring_cancels_its_work() {
    Rig t;
    t.watch();
    t.watch(kSub2, kOwnerA);  // the retiring owner listens too
    const KilnCameraLease held = t.applied(kOwnerA, 5, 0, kA);
    KilnCameraLease staged = 0, others = 0, lens = 0;
    CHECK(t.update(kOwnerA, held, mode_request(5), kA) == KILN_OK);          // an update in flight
    CHECK(t.apply(kOwnerA, mode_request(6), &staged, kA) == KILN_OK);        // an apply in flight
    CHECK(t.apply(kOwnerA, mode_request(5), &others, kA) == KILN_OK);        // a refusal already queued for it
    CHECK(t.apply(kOwnerB, mode_request(7), &others, kB) == KILN_OK);        // somebody else's work
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);           // and a lease in the other world
    CHECK(t.core.committed(lens, 1, 1));
    t.core.pre(kClient);
    g_log.clear();
    t.mode.calls.clear();

    std::thread([&] { t.core.owner_retiring(kOwnerA); }).join();  // any thread; it only marks the owner
    CHECK(t.mode.calls.empty());                                  // no backend, no callback from the observer
    KilnHandle h = 0;
    CHECK(t.apply(kOwnerA, mode_request(8), &others, kA) == KILN_ERR_REVOKED);
    CHECK(t.end(kOwnerA, held, kA) == KILN_ERR_REVOKED);
    CHECK(t.core.subscribe(kOwnerA, &on_changed, kSub2, &h) == KILN_ERR_REVOKED);
    CHECK(t.core.committed(others, 1, 2));
    t.core.pre(kServer);
    // nothing reaches A: not its queued refusal, not its updates, not its subscriber. The others hear that A's
    // applied lease ended; A's staged lease never showed and ends without a word.
    CHECK((g_log == std::vector<std::string>{
                       "done B lease=4 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=4 rev=1 end=0 gen=1",
                       "event sub ended lease=1 rev=1 end=8 gen=1",
                   }));
    CHECK((t.mode.calls == std::vector<std::string>{"drop 1", "drop 2"}));
    CHECK(!t.core.committed(held, 2, 3) && !t.core.committed(staged, 1, 3));  // its work cannot complete later
    CHECK(t.core.pending() == 0);
    // the client world drops its part at its own Pre
    CHECK(t.core.live_leases() == 2 && t.local.calls.back() != "drop 5");
    g_log.clear();
    t.core.pre(kClient);
    CHECK((g_log == std::vector<std::string>{"event sub ended lease=5 rev=1 end=8 gen=1"}));
    CHECK(t.local.calls.back() == "drop 5" && t.core.live_leases() == 1);
    t.core.owner_retiring(0);  // owner 0 never retires (and writes refuse it: test_owner_zero_writes_are_refused)
}

// Owner 0 is "no mod": it never retires, and every owner-0 caller would share one identity (one could update or end
// another's lease). Writes refuse it as malformed before anything is queued; a subscription of Kiln's own stays.
void test_owner_zero_writes_are_refused() {
    Rig t;
    const KilnCameraLease held = t.applied(kOwnerA, 5, 0, kA);
    KilnCameraLease lease = 0;
    CHECK(t.apply(0, mode_request(9), &lease, kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(lease == 0);
    CHECK(t.apply(0, lens_request(), &lease, kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.update(0, held, mode_request(5), kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.end(0, held, kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.end(0, 987654, kA) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(t.core.pending() == 0 && t.mode.calls.empty() && t.local.calls.empty());
    KilnHandle h = 0;
    CHECK(t.core.subscribe(0, &on_changed, kSub, &h) == KILN_OK && h);
    t.core.pre(kServer);
    CHECK(g_log.empty());
    CHECK(t.core.active_revision(held) == 1);  // the mod's lease is untouched
}

void test_callback_fault_retires_the_owner() {
    Rig t;
    t.watch();
    const KilnCameraLease held = t.applied(kOwnerA, 5, 0, kA);
    KilnCameraLease bad = 0, later = 0;
    CHECK(t.apply(kOwnerA, mode_request(5), &bad, kA) == KILN_OK);     // answered AlreadyOwned: this callback faults
    CHECK(t.apply(kOwnerA, mode_request(6), &later, kA) == KILN_OK);   // staged work of the same owner
    g_fault_next = true;
    t.core.pre(kServer);
    // the fault is contained: the owner's lease ends, its staged apply is dropped, it is never called again
    CHECK((g_log == std::vector<std::string>{"event sub ended lease=1 rev=1 end=8 gen=1"}));
    CHECK(t.env.lines.size() == 1 && t.env.lines[0].find("owner 11") != std::string::npos);
    CHECK((t.mode.calls == std::vector<std::string>{"stage 3/1", "drop 1", "drop 3"}));
    CHECK(t.core.live_leases() == 0 && t.core.pending() == 0);
    CHECK(t.apply(kOwnerA, mode_request(7), &later, kA) == KILN_ERR_REVOKED);
    (void)held;

    // a faulting subscriber loses its subscription and its leases; the other subscribers still get every event
    Rig u;
    u.watch(kSub, kOwnerB);
    u.watch(kSub2, kOwnerWatch);
    u.applied(kOwnerB, 5, 0, kB);
    KilnCameraLease first = 0, second = 0;
    CHECK(u.apply(kOwnerA, mode_request(6), &first, kA) == KILN_OK);
    CHECK(u.apply(kOwnerA, mode_request(7), &second, kA) == KILN_OK);
    CHECK(u.core.committed(first, 1, 1) && u.core.committed(second, 1, 2));
    g_fault_changed = 1;  // B's subscriber is the first one called
    u.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=2 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub2 applied lease=2 rev=1 end=0 gen=1",
                       "done A lease=3 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub2 applied lease=3 rev=1 end=0 gen=1",
                       "event sub2 ended lease=1 rev=1 end=8 gen=1",
                   }));
    CHECK(u.env.lines.size() == 1 && u.env.lines[0].find("owner 22") != std::string::npos);
    CHECK(u.mode.calls.back() == "drop 1" && u.core.live_leases() == 2);
}

void test_unsubscribe() {
    Rig t;
    const KilnHandle first = t.watch(kSub);
    t.watch(kSub2);
    const KilnCameraLease a = t.applied(kOwnerA, 5, 0, kA);
    // from inside its own callback: it does not wait, and the subscriber is not called again
    g_unsubscribe_in_callback = first;
    CHECK(t.update(kOwnerA, a, mode_request(5), kA) == KILN_OK);
    CHECK(t.core.committed(a, 2, 1));
    CHECK(t.end(kOwnerA, a, kA) == KILN_OK);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=1 reason=0 rev=2 flags=0 scope=1",
                       "event sub updated lease=1 rev=2 end=0 gen=1",
                       "event sub2 updated lease=1 rev=2 end=0 gen=1",
                       "done A lease=1 ok=1 reason=0 rev=2 flags=0 scope=0",
                       "event sub2 ended lease=1 rev=2 end=1 gen=1",
                   }));
    CHECK(t.core.unsubscribe(first) == KILN_ERR_NOT_FOUND);
    KilnHandle h = 0;
    CHECK(t.core.subscribe(kOwnerA, nullptr, nullptr, &h) == KILN_ERR_INVALID_ARGUMENT);
    for (uint32_t i = 1; i < kMaxSubscribers; ++i) CHECK(t.core.subscribe(kOwnerA, &on_changed, kSub, &h) == KILN_OK);
    CHECK(t.core.subscribe(kOwnerA, &on_changed, kSub, &h) == KILN_ERR_BUFFER_TOO_SMALL);

    // a subscriber removed while an event is going round is not called for that event either
    Rig u;
    u.watch(kSub);
    const KilnHandle second = u.watch(kSub2);
    const KilnCameraLease b = u.applied(kOwnerA, 5, 0, kA);
    g_unsubscribe_in_callback = second;
    CHECK(u.update(kOwnerA, b, mode_request(5), kA) == KILN_OK);
    CHECK(u.core.committed(b, 2, 1));
    u.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"done A lease=1 ok=1 reason=0 rev=2 flags=0 scope=1", "event sub updated lease=1 rev=2 end=0 gen=1"}));
}

// ---- worlds ------------------------------------------------------------------------------------------------------------

void test_world_stop_is_terminal_and_epochs_are_separate() {
    Rig t;
    t.watch();
    const KilnCameraLease held = t.applied(kOwnerA, 5, 0, kA);
    KilnCameraLease staged = 0, lens = 0, ending = 0;
    CHECK(t.update(kOwnerA, held, mode_request(5), kA) == KILN_OK);
    CHECK(t.apply(kOwnerB, mode_request(6), &staged, kB) == KILN_OK);
    CHECK(t.apply(kOwnerB, mode_request(7), &ending, kB) == KILN_OK);
    CHECK(t.core.committed(ending, 1, 1));
    t.mode.release_now = false;
    CHECK(t.end(kOwnerB, ending, kB) == KILN_OK);
    CHECK(t.apply(kOwnerA, lens_request(), &lens, kA) == KILN_OK);
    CHECK(t.core.committed(lens, 1, 1));
    t.core.pre(kClient);
    g_log.clear();
    t.mode.calls.clear();

    // the world is destroyed on another thread, between two steps: no Pre comes any more, so what is open is
    // answered there and then
    const std::thread::id here = std::this_thread::get_id();
    std::thread([&] { t.core.world_stopping(kServer, 1); }).join();
    CHECK(g_callback_thread != here);
    CHECK((g_log == std::vector<std::string>{
                       "done B lease=3 ok=1 reason=0 rev=1 flags=0 scope=1",   // queued before the stop, still delivered first
                       "event sub applied lease=3 rev=1 end=0 gen=1",
                       "done A lease=1 ok=0 reason=12 rev=1 flags=0 scope=0",  // the waiting update
                       "event sub ended lease=1 rev=1 end=5 gen=1",
                       "done B lease=2 ok=0 reason=12 rev=0 flags=0 scope=0",  // the waiting apply: it never was applied, no event
                       "done B lease=3 ok=1 reason=0 rev=1 flags=0 scope=0",   // the waiting end
                       "event sub ended lease=3 rev=1 end=5 gen=1",
                   }));
    CHECK(t.mode.calls == std::vector<std::string>{"world_stopped 1"});  // the backend is told first and only that
    CHECK(t.core.epoch(kServer) == 0 && t.core.epoch(kClient) == 1);
    CHECK(t.core.live_leases() == 1 && t.core.active_revision(lens) == 1);  // the client world's lease is not touched
    CHECK(t.core.pending() == 0);
    // nothing of the old world completes into the new one
    CHECK(!t.core.committed(held, 2, 5) && !t.core.committed(staged, 1, 5) && !t.core.released(ending, 5));
    KilnCameraLease next = 0;
    CHECK(t.apply(kOwnerA, mode_request(5), &next, kA) == KILN_ERR_NOT_FOUND);
    CHECK(t.end(kOwnerA, held, kA) == KILN_ERR_NOT_FOUND);  // no Pre would answer it; the lease ended with its world
    t.core.world_stopping(kServer, 1);  // a second stop of the same world: nothing
    g_log.clear();
    t.core.world_started(kServer, 2);
    CHECK(t.end(kOwnerA, held, kA) == KILN_OK);  // in the next world the old lease is a tombstone of its owner
    CHECK(t.apply(kOwnerA, mode_request(5), &next, kA) == KILN_OK);
    CHECK(next > held && t.mode.staged.back().epoch == 2);  // ids are never reused; the lease carries its world's epoch
    CHECK(t.core.committed(next, 1, 6));
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{
                       "done A lease=1 ok=1 reason=0 rev=0 flags=2 scope=0",
                       "done A lease=5 ok=1 reason=0 rev=1 flags=0 scope=1",
                       "event sub applied lease=5 rev=1 end=0 gen=2",
                   }));
    // a stop for an epoch that is not the live one is ignored; a start without a stop ends the old world's leases
    t.core.world_stopping(kServer, 1);
    CHECK(t.core.epoch(kServer) == 2 && t.core.active_revision(next) == 1);
    g_log.clear();
    t.core.world_started(kServer, 3);
    CHECK(t.core.epoch(kServer) == 3 && t.core.active_revision(next) == 0);
    t.core.pre(kServer);
    CHECK((g_log == std::vector<std::string>{"event sub ended lease=5 rev=1 end=5 gen=2"}));
    CHECK(t.core.epoch(kClient) == 1 && t.core.active_revision(lens) == 1);
}

}  // namespace

int main() {
    test_validation();
    test_immediate_returns();
    test_apply_done_then_changed();
    test_refusals_through_done();
    test_already_owned_and_busy();
    test_supersede_old_ended_before_new_applied();
    test_update_and_stale_revisions();
    test_end_is_idempotent();
    test_end_waits_for_the_release();
    test_end_cancels_pending_work();
    test_backend_ends();
    test_queue_64_backpressure();
    test_owner_retiring_cancels_its_work();
    test_owner_zero_writes_are_refused();
    test_callback_fault_retires_the_owner();
    test_unsubscribe();
    test_world_stop_is_terminal_and_epochs_are_separate();
    std::printf("camera state: PASS (%d checks)\n", g_checks);
    return 0;
}
