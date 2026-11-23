// SPDX-License-Identifier: MIT
// Host tests of kiln.camera's REMOTE_VISUAL backend (bridge.h) with the real lease model (state.h) and a fake transport
// bridge (plain C functions, as Kiln.Camera's are): what is sent and when, how the bridge's answers become `done` and
// change events, refusals with their reasons, End's priority, a client's own end, owners that retire, unbind and world
// stop. No game, no Wine, no managed code.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../bridge.h"

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

constexpr KilnOwner kBridge = 700, kAuthorA = 11, kAuthorB = 22, kWatch = 33;
constexpr KilnEntity kGuest = 5, kVanilla = 6;

// ---- the fake bridge ----
struct Sent {
    KilnOwner author;
    uint32_t op;
    KilnCameraLease lease;
    uint64_t revision;
    bool has_request;
    uint32_t parts, player, keys, duration;
    int32_t priority;
    KilnCameraDone done;
    void* user;
};
std::vector<Sent> g_sent;
std::vector<KilnOwner> g_retired;
KilnResult g_submit_rc = KILN_OK;
bool g_answer_inline = false;  // a broken bridge that calls done inside submit
uint64_t g_offered = KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE;

void answer(const Sent& s, bool ok, uint32_t reason, uint32_t flags, uint64_t frame, const char* text = "") {
    KilnCameraResult r{};
    r.size = sizeof r;
    r.ok = ok;
    r.reason_code = reason;
    r.commit_scope = ok ? KILN_CAMERA_COMMIT_CLIENT_FRAME : KILN_CAMERA_COMMIT_NONE;
    r.flags = flags;
    r.endpoint = KILN_CAMERA_REMOTE_VISUAL;
    r.player_entity = s.player;
    r.lease = s.lease;
    r.revision = s.revision;
    r.frame = frame;
    r.reason = KilnStr{text, std::strlen(text)};
    s.done(&r, s.user);
}

KilnResult KILN_CALL submit(KilnOwner author, uint32_t op, KilnCameraLease lease, uint64_t revision, const KilnCameraRequest* q, KilnCameraDone done, void* user) {
    Sent s{author, op, lease, revision, q != nullptr, 0, 0, 0, 0, 0, done, user};
    if (q) {
        CHECK(q->size == sizeof(KilnCameraRequest));
        s.parts = q->parts;
        s.player = q->player_entity;
        s.keys = q->key_count;
        s.duration = q->duration_ms;
        s.priority = q->priority;
        CHECK(q->endpoint == KILN_CAMERA_REMOTE_VISUAL);
        CHECK((q->key_count == 0) == (q->keys == nullptr));
        for (uint32_t i = 0; i < q->key_count; ++i) CHECK(q->keys[i].pose.rotation[3] != 0.0f || q->keys[i].pose.rotation[0] != 0.0f);
    }
    CHECK(done != nullptr);
    g_sent.push_back(s);
    if (g_submit_rc != KILN_OK) return g_submit_rc;
    if (g_answer_inline) answer(s, true, 0, 0, 1);
    return KILN_OK;
}
KilnResult KILN_CALL caps(KilnEntity player, uint64_t* mask) {
    if (player == kGuest) return *mask = g_offered, KILN_OK;
    if (player == kVanilla) return *mask = 0, KILN_OK;
    return KILN_ERR_NOT_FOUND;
}
void KILN_CALL retire(KilnOwner author) { g_retired.push_back(author); }
KilnCameraTransport table() { return {{sizeof(KilnCameraTransport), 1}, &submit, &caps, &retire}; }

// ---- what the authors saw ----
std::vector<std::string> g_log;
struct TestEnv : Env {
    std::vector<std::string> lines;
    bool call_done(KilnCameraDone fn, const KilnCameraResult& r, void* user) override { return fn(&r, user), true; }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& c, void* user) override { return fn(&c, user), true; }
    void log(int32_t, const std::string& text) override { lines.push_back(text); }
};
void KILN_CALL on_done(const KilnCameraResult* r, void* user) {
    char line[200];
    std::snprintf(line, sizeof line, "done %s lease=%llu ok=%u reason=%u rev=%llu flags=%u scope=%u frame=%llu", static_cast<const char*>(user),
                  static_cast<unsigned long long>(r->lease), r->ok, r->reason_code, static_cast<unsigned long long>(r->revision), r->flags, r->commit_scope,
                  static_cast<unsigned long long>(r->frame));
    g_log.push_back(line);
}
void KILN_CALL on_changed(const KilnCameraChange* c, void*) {
    static const char* const kKind[] = {"?", "applied", "updated", "ended"};
    char line[200];
    std::snprintf(line, sizeof line, "event %s lease=%llu rev=%llu end=%u reason=%u frame=%llu", kKind[c->kind < 4 ? c->kind : 0], static_cast<unsigned long long>(c->lease),
                  static_cast<unsigned long long>(c->revision), c->end_reason, c->reason_code, static_cast<unsigned long long>(c->frame));
    g_log.push_back(line);
}
char kA[] = "A", kB[] = "B", kE[] = "E", kU[] = "U";

std::string dump() {
    std::string s;
    for (const auto& l : g_log) s += "  " + l + "\n";
    return s;
}
bool saw(const std::string& line) {
    for (const auto& l : g_log)
        if (l == line) return true;
    std::fprintf(stderr, "missing \"%s\" in:\n%s", line.c_str(), dump().c_str());
    return false;
}

struct Rig {
    TestEnv env;
    Core core{&env};
    RemoteBackend remote{&core};
    KilnHandle sub = 0;
    Rig(bool bind = true) {
        g_sent.clear();
        g_retired.clear();
        g_log.clear();
        g_submit_rc = KILN_OK;
        g_answer_inline = false;
        core.set_backend(KILN_CAMERA_REMOTE_VISUAL, &remote);
        core.set_writes(true);
        core.world_started(kServer, 4);
        CHECK(core.subscribe(kWatch, &on_changed, nullptr, &sub) == KILN_OK);
        if (bind) {
            auto t = table();
            CHECK(remote.bind(kBridge, &t) == KILN_OK);
        }
    }
    // one server Pre: the backend's step, then the core's deliveries
    void pre() {
        remote.step();
        core.pre(kServer);
    }
};

KilnCameraRequest lens(KilnEntity player, int32_t priority = 0) {
    KilnCameraRequest q{};
    q.size = sizeof q;
    q.parts = KILN_CAMERA_LENS;
    q.endpoint = KILN_CAMERA_REMOTE_VISUAL;
    q.player_entity = player;
    q.input_policy = KILN_CAMERA_VISUAL_ONLY;
    q.duration_ms = 5000;
    q.priority = priority;
    q.lens_source = KILN_CAMERA_LENS_CONSTANT;
    q.fov_y_degrees = 70.0f;
    return q;
}

KilnCameraLease apply(Rig& rig, KilnOwner owner, const KilnCameraRequest& q, char* tag) {
    KilnCameraLease lease = 0;
    CHECK(rig.core.apply(owner, &q, &lease, &on_done, tag, kOnServer) == KILN_OK);
    CHECK(lease != 0);
    return lease;
}

// ---- tests ----

void no_bridge_is_unavailable() {
    Rig rig(false);
    uint64_t mask = 1;
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 0, &mask) == KILN_OK && mask == 0);
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, kGuest, &mask) == KILN_OK && mask == 0);
    auto q = lens(kGuest);
    KilnCameraLease lease = 0;
    CHECK(rig.core.apply(kAuthorA, &q, &lease, &on_done, kA, kOnServer) == KILN_ERR_UNAVAILABLE);
    rig.pre();
    CHECK(g_sent.empty() && g_log.empty());
    // the table's framework-private entries refuse what is not a bridge
    auto t = table();
    CHECK(rig.remote.bind(0, &t) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(rig.remote.bind(kBridge, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(rig.remote.unbind(kBridge) == KILN_ERR_NOT_FOUND);
    CHECK(rig.remote.transport_ended(kBridge, 1, 1, KILN_CAMERA_END_EXPIRED, 0, KilnStr{"", 0}) == KILN_ERR_NOT_FOUND);
    // bound: the row's bit, and for a player what its client offered
    CHECK(rig.remote.bind(kBridge, &t) == KILN_OK);
    CHECK(rig.remote.bind(kBridge + 1, &t) == KILN_ERR_INVALID_ARGUMENT);  // one bridge at a time
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 0, &mask) == KILN_OK && mask == KILN_CAMERA_CAP_REMOTE);
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, kGuest, &mask) == KILN_OK && mask == (KILN_CAMERA_CAP_REMOTE | g_offered));
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, kVanilla, &mask) == KILN_OK && mask == KILN_CAMERA_CAP_REMOTE);
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 99, &mask) == KILN_OK && mask == KILN_CAMERA_CAP_REMOTE);
    g_offered = ~0ull;  // the bridge cannot add a bit that is not a local part
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, kGuest, &mask) == KILN_OK &&
          mask == (KILN_CAMERA_CAP_REMOTE | KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE));
    g_offered = KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_PROFILE;
    // the owner-retiring observer is still required for any write
    rig.core.set_writes(false);
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, kGuest, &mask) == KILN_OK && mask == 0);
    rig.core.set_writes(true);
    CHECK(rig.remote.unbind(kBridge + 1) == KILN_ERR_REVOKED);
    CHECK(rig.remote.unbind(kBridge) == KILN_OK);
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 0, &mask) == KILN_OK && mask == 0);
    std::printf("  no bridge: unavailable, bound bridge: row bit and the recipient's offered parts\n");
}

void apply_commits_on_the_clients_frame() {
    Rig rig;
    auto q = lens(kGuest);
    const auto lease = apply(rig, kAuthorA, q, kA);
    CHECK(g_sent.empty());  // under the core's lock nothing goes to the bridge
    rig.core.pre(kServer);
    CHECK(g_sent.empty() && g_log.empty());
    rig.pre();
    CHECK(g_sent.size() == 1);
    const Sent s = g_sent[0];
    CHECK(s.author == kAuthorA && s.op == 1 && s.lease == lease && s.revision == 1 && s.has_request);
    CHECK(s.parts == KILN_CAMERA_LENS && s.player == kGuest && s.duration == 5000 && s.keys == 0);
    CHECK(g_log.empty() && rig.remote.outstanding() == 1);
    answer(s, true, 0, 0, 42);  // the client drew frame 42 with it (the bridge calls this on a server Pre)
    CHECK(g_log.empty());       // recorded only; reported at the next step
    rig.pre();
    CHECK(g_log.size() == 2);
    CHECK(g_log[0] == "done A lease=" + std::to_string(lease) + " ok=1 reason=0 rev=1 flags=0 scope=2 frame=42");
    CHECK(g_log[1] == "event applied lease=" + std::to_string(lease) + " rev=1 end=0 reason=0 frame=42");
    CHECK(rig.core.active_revision(lease) == 1 && rig.remote.active() == 1 && rig.remote.outstanding() == 0);
    // a second answer for the same submit changes nothing
    answer(s, false, KILN_CAMERA_REASON_BUSY, 0, 0, "late");
    rig.pre();
    CHECK(g_log.size() == 2);
    std::printf("  apply: sent at the next server Pre, Ok only on the client's frame (ClientFrame), done before APPLIED\n");
}

void refusals_keep_their_reasons() {
    Rig rig;
    struct Case { uint32_t reason, flags; const char* text; } cases[] = {
        {KILN_CAMERA_REASON_REQUIRES_KILN_CLIENT, 0, "the player's client does not run Kiln"},
        {KILN_CAMERA_REASON_CLIENT_CAPABILITY, 0, "the player's client does not offer camera.local_fov"},
        {KILN_CAMERA_REASON_NEGOTIATION, 0, "no Hello within 2 s"},
        {KILN_CAMERA_REASON_TRANSPORT_TIMEOUT, KILN_CAMERA_MAY_HAVE_APPLIED, "no ACK within 3 s"},
        {KILN_CAMERA_REASON_BODY_TOO_LARGE, 0, "too large"},
    };
    for (const auto& c : cases) {
        g_log.clear();
        g_sent.clear();
        const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
        rig.pre();
        CHECK(g_sent.size() == 1);
        answer(g_sent[0], false, c.reason, c.flags, 0, c.text);
        rig.pre();
        CHECK(g_log.size() == 1);  // no event for a lease that never applied
        CHECK(g_log[0] == "done A lease=" + std::to_string(lease) + " ok=0 reason=" + std::to_string(c.reason) + " rev=0 flags=" + std::to_string(c.flags) +
                              " scope=0 frame=0");
        CHECK(rig.core.live_leases() == 0);
        // the refused lease is a tombstone: its owner's end is ALREADY_ENDED, nothing is sent for it
        KilnCameraLease ignored = lease;
        CHECK(rig.core.end(kAuthorA, ignored, &on_done, kE, kOnServer) == KILN_OK);
        rig.pre();
        CHECK(g_sent.size() == 1);
        CHECK(saw("done E lease=" + std::to_string(lease) + " ok=1 reason=0 rev=0 flags=2 scope=0 frame=0"));
    }
    CHECK(rig.remote.outstanding() == 0 && rig.remote.active() == 0);
    std::printf("  refusals: RequiresKilnClient, ClientCapability, Negotiation, TransportTimeout (MayHaveApplied), BodyTooLarge\n");
}

void update_and_end_go_through_the_bridge() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 10);
    rig.pre();
    g_log.clear();
    auto q = lens(kGuest);
    q.fov_y_degrees = 90.0f;
    CHECK(rig.core.update(kAuthorA, lease, KILN_CAMERA_RESTART_TIMELINE, &q, &on_done, kU, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(g_sent.size() == 2 && g_sent[1].op == 3 && g_sent[1].revision == 2 && g_sent[1].lease == lease && g_sent[1].has_request);
    CHECK(rig.core.update(kAuthorA, lease, KILN_CAMERA_PRESERVE_TIMELINE, &q, &on_done, kU, kOnServer) == KILN_OK);  // one in flight: Busy
    rig.pre();
    CHECK(saw("done U lease=" + std::to_string(lease) + " ok=0 reason=1 rev=1 flags=0 scope=0 frame=0"));
    answer(g_sent[1], true, 0, 0, 11);
    rig.pre();
    CHECK(saw("done U lease=" + std::to_string(lease) + " ok=1 reason=0 rev=2 flags=0 scope=2 frame=11"));
    CHECK(saw("event updated lease=" + std::to_string(lease) + " rev=2 end=0 reason=0 frame=11"));
    CHECK(rig.core.active_revision(lease) == 2);
    // a refused update keeps the active revision
    CHECK(rig.core.update(kAuthorA, lease, KILN_CAMERA_PRESERVE_TIMELINE, &q, &on_done, kU, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(g_sent.size() == 3 && g_sent[2].op == 2 && g_sent[2].revision == 3);
    answer(g_sent[2], false, KILN_CAMERA_REASON_CLIENT_CAPABILITY, 0, 0, "no");
    rig.pre();
    CHECK(saw("done U lease=" + std::to_string(lease) + " ok=0 reason=25 rev=2 flags=0 scope=0 frame=0"));
    CHECK(rig.core.active_revision(lease) == 2);
    // end: the release goes to the bridge, ENDED(Released) when it answered
    g_log.clear();
    CHECK(rig.core.end(kAuthorA, lease, &on_done, kE, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(g_sent.size() == 4 && g_sent[3].op == 4 && !g_sent[3].has_request && g_sent[3].lease == lease && g_sent[3].revision == 2);
    CHECK(g_log.empty());
    answer(g_sent[3], true, 0, 0, 12);
    rig.pre();
    CHECK(g_log.size() == 2);
    CHECK(g_log[0] == "done E lease=" + std::to_string(lease) + " ok=1 reason=0 rev=2 flags=0 scope=0 frame=12");
    CHECK(g_log[1] == "event ended lease=" + std::to_string(lease) + " rev=2 end=1 reason=0 frame=12");
    CHECK(rig.remote.active() == 0 && rig.core.live_leases() == 0);
    // an End the client could not confirm (the peer left meanwhile) still releases: the client clears it by itself
    const auto l2 = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 20);
    rig.pre();
    CHECK(rig.core.end(kAuthorA, l2, &on_done, kE, kOnServer) == KILN_OK);
    rig.pre();
    answer(g_sent.back(), false, KILN_CAMERA_REASON_PEER_UNAVAILABLE, 0, 0, "gone");
    rig.pre();
    CHECK(saw("event ended lease=" + std::to_string(l2) + " rev=1 end=1 reason=0 frame=0"));
    std::printf("  update (Preserve/Restart, one in flight, refused keeps the revision) and end through the bridge\n");
}

void end_preempts_a_queued_update_and_apply() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    g_log.clear();
    auto q = lens(kGuest);
    CHECK(rig.core.update(kAuthorA, lease, KILN_CAMERA_PRESERVE_TIMELINE, &q, &on_done, kU, kOnServer) == KILN_OK);
    CHECK(rig.core.end(kAuthorA, lease, &on_done, kE, kOnServer) == KILN_OK);  // before any step: the update never leaves
    rig.pre();
    CHECK(g_sent.size() == 2 && g_sent[1].op == 4);
    CHECK(saw("done U lease=" + std::to_string(lease) + " ok=0 reason=24 rev=1 flags=0 scope=0 frame=0"));
    answer(g_sent[1], true, 0, 0, 2);
    rig.pre();
    CHECK(saw("event ended lease=" + std::to_string(lease) + " rev=1 end=1 reason=0 frame=2"));
    // an apply ended before it was sent is never sent; one ended after it was sent gets an End after it
    g_log.clear();
    g_sent.clear();
    const auto l2 = apply(rig, kAuthorA, lens(kGuest), kA);
    CHECK(rig.core.end(kAuthorA, l2, &on_done, kE, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(g_sent.empty());
    CHECK(saw("done A lease=" + std::to_string(l2) + " ok=0 reason=24 rev=0 flags=0 scope=0 frame=0"));
    CHECK(saw("event ended lease=" + std::to_string(l2) + " rev=0 end=13 reason=0 frame=0"));
    const auto l3 = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    CHECK(g_sent.size() == 1 && g_sent[0].op == 1);
    CHECK(rig.core.end(kAuthorA, l3, &on_done, kE, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(g_sent.size() == 2 && g_sent[1].op == 4 && g_sent[1].lease == l3);
    answer(g_sent[0], true, 0, 0, 5);  // the apply's late Ok: the lease is gone, nothing applies
    answer(g_sent[1], true, 0, 0, 5);
    rig.pre();
    for (const auto& l : g_log) CHECK(l.find("event applied lease=" + std::to_string(l3)) == std::string::npos);
    CHECK(rig.remote.outstanding() == 0 && rig.remote.active() == 0);
    std::printf("  end preempts: a queued update is cancelled unsent, an unsent apply never leaves, a sent one is ended\n");
}

void the_client_ends_a_lease_by_itself() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    g_log.clear();
    const char text[] = "the client's lease expired";
    CHECK(rig.remote.transport_ended(kBridge + 1, lease, 1, KILN_CAMERA_END_EXPIRED, 0, KilnStr{text, sizeof text - 1}) == KILN_ERR_REVOKED);
    CHECK(rig.remote.transport_ended(0, lease, 1, KILN_CAMERA_END_EXPIRED, 0, KilnStr{text, sizeof text - 1}) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(rig.remote.transport_ended(kBridge, lease, 1, KILN_CAMERA_END_NONE, 0, KilnStr{text, sizeof text - 1}) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(rig.remote.transport_ended(kBridge, lease, 1, 99, 0, KilnStr{text, sizeof text - 1}) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(rig.remote.transport_ended(kBridge, lease, 2, KILN_CAMERA_END_EXPIRED, 0, KilnStr{text, sizeof text - 1}) == KILN_OK);  // stale revision
    rig.pre();
    CHECK(g_log.empty() && rig.core.active_revision(lease) == 1);
    CHECK(rig.remote.transport_ended(kBridge, lease, 1, KILN_CAMERA_END_TARGET_LOST, KILN_CAMERA_REASON_TARGET_UNAVAILABLE, KilnStr{text, sizeof text - 1}) == KILN_OK);
    rig.pre();
    CHECK(g_log.size() == 1 && g_log[0] == "event ended lease=" + std::to_string(lease) + " rev=1 end=3 reason=10 frame=0");
    CHECK(rig.remote.active() == 0 && rig.core.live_leases() == 0);
    CHECK(g_sent.size() == 1);  // nothing is sent back for a lease the client ended
    // an End of the author afterwards is ALREADY_ENDED
    CHECK(rig.core.end(kAuthorA, lease, &on_done, kE, kOnServer) == KILN_OK);
    rig.pre();
    CHECK(saw("done E lease=" + std::to_string(lease) + " ok=1 reason=0 rev=0 flags=2 scope=0 frame=0"));
    std::printf("  transport_ended: bridge owner, end reason and revision checked; one ENDED, no duplicate\n");
}

// review S8 M1: an update the client never answered is refused TransportTimeout (MayHaveApplied) and the bridge ends the
// active lease (transport_ended, TRANSPORT_TIMEOUT) in the same step, in either order of arrival: the author gets the
// refusal first, then one ENDED; the slot is free again.
void a_timed_out_update_ends_its_lease() {
    for (int order = 0; order < 2; ++order) {
        Rig rig;
        const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
        rig.pre();
        answer(g_sent.back(), true, 0, 0, 1);
        rig.pre();
        g_log.clear();
        auto q = lens(kGuest);
        CHECK(rig.core.update(kAuthorA, lease, KILN_CAMERA_RESTART_TIMELINE, &q, &on_done, kU, kOnServer) == KILN_OK);
        rig.pre();
        CHECK(g_sent.size() == 2 && g_sent[1].op == 3);
        const KilnStr why{"no answer within 3 s", 20};
        if (order == 1) CHECK(rig.remote.transport_ended(kBridge, lease, 1, KILN_CAMERA_END_TRANSPORT_TIMEOUT, KILN_CAMERA_REASON_TRANSPORT_TIMEOUT, why) == KILN_OK);
        answer(g_sent[1], false, KILN_CAMERA_REASON_TRANSPORT_TIMEOUT, KILN_CAMERA_MAY_HAVE_APPLIED, 0, "no answer within 3 s");
        if (order == 0) CHECK(rig.remote.transport_ended(kBridge, lease, 1, KILN_CAMERA_END_TRANSPORT_TIMEOUT, KILN_CAMERA_REASON_TRANSPORT_TIMEOUT, why) == KILN_OK);
        rig.pre();
        CHECK(g_log.size() == 2);
        CHECK(g_log[0] == "done U lease=" + std::to_string(lease) + " ok=0 reason=15 rev=1 flags=1 scope=0 frame=0");
        CHECK(g_log[1] == "event ended lease=" + std::to_string(lease) + " rev=1 end=12 reason=15 frame=0");
        CHECK(rig.core.live_leases() == 0 && rig.remote.active() == 0 && rig.remote.outstanding() == 0);
        // the slot is free: the same author applies again and is not refused ALREADY_OWNED
        const auto again = apply(rig, kAuthorA, lens(kGuest), kA);
        rig.pre();
        CHECK(g_sent.back().lease == again && g_sent.back().op == 1);
    }
    std::printf("  an update that timed out: refused TransportTimeout (MayHaveApplied), then ENDED(TransportTimeout), slot free\n");
}

void a_higher_priority_lease_supersedes() {
    Rig rig;
    const auto a = apply(rig, kAuthorA, lens(kGuest, 0), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    g_log.clear();
    const auto b = apply(rig, kAuthorB, lens(kGuest, 100), kB);
    rig.pre();
    CHECK(g_sent.size() == 2 && g_sent[1].author == kAuthorB && g_sent[1].op == 1 && g_sent[1].priority == 100);
    answer(g_sent[1], true, 0, 0, 2);
    rig.pre();
    CHECK(g_log.size() == 3);
    CHECK(g_log[0] == "event ended lease=" + std::to_string(a) + " rev=1 end=6 reason=0 frame=2");
    CHECK(g_log[1] == "done B lease=" + std::to_string(b) + " ok=1 reason=0 rev=1 flags=0 scope=2 frame=2");
    CHECK(g_log[2] == "event applied lease=" + std::to_string(b) + " rev=1 end=0 reason=0 frame=2");
    CHECK(rig.remote.active() == 1);
    // the superseded lease's own late end from the client changes nothing
    CHECK(rig.remote.transport_ended(kBridge, a, 1, KILN_CAMERA_END_EXPIRED, 0, KilnStr{"", 0}) == KILN_OK);
    rig.pre();
    CHECK(g_log.size() == 3);
    std::printf("  a higher priority supersedes: old ENDED(Superseded) before the new done and APPLIED\n");
}

void retired_authors_are_ended_and_told() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    const auto pending = apply(rig, kAuthorA, lens(kGuest + 100), kA);  // another player, still waiting for its answer
    rig.pre();
    CHECK(g_sent.size() == 2 && g_sent[1].lease == pending);
    g_log.clear();
    rig.core.owner_retiring(kAuthorA);
    rig.pre();  // the core drops the owner's leases in this Pre; the bridge hears of it at the next step
    rig.pre();
    int ends = 0;
    for (size_t i = 2; i < g_sent.size(); ++i)
        if (g_sent[i].op == 4) {
            ++ends;
            CHECK(g_sent[i].lease == lease || g_sent[i].lease == pending);
        }
    CHECK(ends == 2);
    CHECK(g_retired.size() == 1 && g_retired[0] == kAuthorA);
    for (const auto& l : g_log) CHECK(l.rfind("done", 0) != 0);  // nothing for the retired owner
    CHECK(saw("event ended lease=" + std::to_string(lease) + " rev=1 end=8 reason=0 frame=0"));
    // the bridge answers what it had of the retired owner: nothing reaches it
    for (const auto& s : g_sent) answer(s, true, 0, 0, 9);
    const size_t before = g_log.size();
    rig.pre();
    CHECK(g_log.size() == before);
    CHECK(rig.remote.active() == 0 && rig.remote.outstanding() == 0);
    std::printf("  a retired author: its leases end (OwnerRetired), Ends go out, retire(author) once, no callback into it\n");
}

void unbind_ends_what_the_bridge_carried() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    const auto waiting = apply(rig, kAuthorB, lens(kGuest + 1), kB);
    rig.pre();
    g_log.clear();
    CHECK(rig.remote.unbind(kBridge) == KILN_OK);
    uint64_t mask = 0;
    CHECK(rig.core.capabilities(KILN_CAMERA_REMOTE_VISUAL, 0, &mask) == KILN_OK && mask == 0);
    rig.pre();
    CHECK(saw("done B lease=" + std::to_string(waiting) + " ok=0 reason=8 rev=0 flags=1 scope=0 frame=0"));
    CHECK(saw("event ended lease=" + std::to_string(lease) + " rev=1 end=7 reason=0 frame=0"));
    CHECK(rig.remote.active() == 0 && rig.remote.outstanding() == 0 && rig.core.live_leases() == 0);
    KilnCameraLease l = 0;
    auto q = lens(kGuest);
    CHECK(rig.core.apply(kAuthorA, &q, &l, &on_done, kA, kOnServer) == KILN_ERR_UNAVAILABLE);
    // a late answer of the old bridge changes nothing; a new bridge starts clean
    const size_t before = g_log.size();
    answer(g_sent.back(), true, 0, 0, 3);
    rig.pre();
    CHECK(g_log.size() == before);
    auto t = table();
    CHECK(rig.remote.bind(kBridge + 1, &t) == KILN_OK);
    const auto fresh = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    CHECK(g_sent.back().lease == fresh && g_sent.back().op == 1);
    std::printf("  unbind: waiting requests refused PeerUnavailable (MayHaveApplied), applied leases end CapabilityLost\n");
}

void world_stop_forgets_the_bridges_work() {
    Rig rig;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    answer(g_sent.back(), true, 0, 0, 1);
    rig.pre();
    const auto waiting = apply(rig, kAuthorB, lens(kGuest + 1), kB);
    rig.pre();
    g_log.clear();
    std::thread stopper([&] { rig.core.world_stopping(kServer, 4); });  // the destroying thread
    stopper.join();
    CHECK(saw("done B lease=" + std::to_string(waiting) + " ok=0 reason=12 rev=0 flags=0 scope=0 frame=0"));
    CHECK(saw("event ended lease=" + std::to_string(lease) + " rev=1 end=5 reason=0 frame=0"));
    const size_t before = g_log.size(), sent = g_sent.size();
    for (const auto& s : g_sent) answer(s, true, 0, 0, 7);  // the bridge's late answers
    rig.core.world_started(kServer, 5);
    rig.pre();
    CHECK(g_log.size() == before && g_sent.size() == sent);
    CHECK(rig.remote.outstanding() == 0 && rig.remote.active() == 0 && rig.remote.queued() == 0);
    const auto next = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    CHECK(g_sent.back().lease == next);
    std::printf("  world stop: WorldStopped on the destroying thread, the bridge's late answers change nothing\n");
}

void a_refusing_or_inline_bridge_is_survived() {
    Rig rig;
    g_submit_rc = KILN_ERR_UNAVAILABLE;
    const auto lease = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    rig.pre();
    CHECK(saw("done A lease=" + std::to_string(lease) + " ok=0 reason=8 rev=0 flags=0 scope=0 frame=0"));
    CHECK(rig.remote.outstanding() == 0);
    g_submit_rc = KILN_OK;
    g_answer_inline = true;  // never inside submit, says the header; if a bridge does, the answer still waits for a step
    g_log.clear();
    const auto l2 = apply(rig, kAuthorA, lens(kGuest), kA);
    rig.pre();
    CHECK(g_log.empty());
    rig.pre();
    CHECK(saw("done A lease=" + std::to_string(l2) + " ok=1 reason=0 rev=1 flags=0 scope=2 frame=1"));
    // a malformed answer (size too small, unknown job) is ignored
    g_answer_inline = false;
    const auto l3 = apply(rig, kAuthorA, lens(kGuest + 2), kA);
    rig.pre();
    KilnCameraResult tiny{};
    tiny.size = 8;
    tiny.ok = 1;
    g_sent.back().done(&tiny, g_sent.back().user);
    g_sent.back().done(nullptr, g_sent.back().user);
    KilnCameraResult ok{};
    ok.size = sizeof ok;
    ok.ok = 1;
    g_sent.back().done(&ok, reinterpret_cast<void*>(uintptr_t{0xdeadbeef}));
    rig.pre();
    CHECK(rig.core.active_revision(l3) == 0 && rig.remote.outstanding() == 1);
    std::printf("  a bridge that refuses submit, answers inside it, or sends malformed answers is survived\n");
}

void answers_from_another_thread() {
    Rig rig;
    std::vector<KilnCameraLease> leases;
    for (uint32_t p = 1; p <= 12; ++p) leases.push_back(apply(rig, kAuthorA, lens(100 + p), kA));
    rig.pre();
    CHECK(g_sent.size() == 12);
    std::atomic<bool> go{false};
    std::thread t([&] {
        while (!go.load()) std::this_thread::yield();
        for (const auto& s : g_sent) answer(s, true, 0, 0, 3);
    });
    go = true;
    for (int i = 0; i < 50; ++i) rig.pre();
    t.join();
    rig.pre();
    for (const auto l : leases) CHECK(rig.core.active_revision(l) == 1);
    std::printf("  answers recorded from another thread, reported on the server Pre\n");
}

}  // namespace

int main() {
    no_bridge_is_unavailable();
    apply_commits_on_the_clients_frame();
    refusals_keep_their_reasons();
    update_and_end_go_through_the_bridge();
    end_preempts_a_queued_update_and_apply();
    the_client_ends_a_lease_by_itself();
    a_timed_out_update_ends_its_lease();
    a_higher_priority_lease_supersedes();
    retired_authors_are_ended_and_told();
    unbind_ends_what_the_bridge_carried();
    world_stop_forgets_the_bridges_work();
    a_refusing_or_inline_bridge_is_survived();
    answers_from_another_thread();
    std::printf("camera remote backend: PASS (%d checks)\n", g_checks);
}
