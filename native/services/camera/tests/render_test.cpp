// SPDX-License-Identifier: MIT
// kiln.camera's render transaction (S5) without a game: the math on the reference model's vectors
// (out/camera/evidence/test_camera_contract.py), the timeline, and the LOCAL_VISUAL backend on a fake renderer and a
// fake clock with the real lease model: only the 52 pose bytes and xmm6 are written, Applied comes after the frame
// showed it, the limits and refusals, blends, expiry, End, a later writer, a fault, the world and the owner going.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../cam_math.h"
#include "../profile.h"
#include "../render_tx.h"
#include "../shake.h"
#include "../timeline.h"

static int checks;
#define CHECK(x)                                                         \
    do {                                                                 \
        ++checks;                                                        \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "FAIL render:%d: %s\n", __LINE__, #x);  \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

using namespace kcam;

namespace {

bool near(double a, double b, double eps = 1e-9) { return std::fabs(a - b) <= eps; }
bool near(const Vec3& a, const Vec3& b, double eps = 1e-9) { return length(a - b) <= eps; }

// ---- the math -----------------------------------------------------------------------------------------------------------

void test_math() {
    const double h = std::sqrt(0.5);
    const Quat yaw90{0, h, 0, h};  // +90 degrees about world Y
    CHECK(near(rotate(yaw90, {0, 0, 1}), {1, 0, 0}, 1e-12));
    CHECK(near(rotate(yaw90, {1, 0, 0}), {0, 0, -1}, 1e-12));
    // Hamilton: rotate(a*b, v) == rotate(a, rotate(b, v))
    const Quat pitch{std::sin(0.3), 0, 0, std::cos(0.3)}, yaw{0, std::sin(0.4), 0, std::cos(0.4)};
    const Vec3 v{0.3, -1.2, 2.5};
    CHECK(near(rotate(mul(yaw, pitch), v), rotate(yaw, rotate(pitch, v)), 1e-12));
    CHECK(!near(rotate(mul(pitch, yaw), v), rotate(yaw, rotate(pitch, v)), 1e-3));

    // shortest arc: the negated second quaternion is the same rotation, so halfway is the first (reference vector)
    const Quat id{0, 0, 0, 1}, neg{0, 0, 0, -1};
    CHECK(near(std::fabs(slerp(id, neg, 0.5).w), 1, 1e-12));
    CHECK(near(slerp(id, yaw90, 0).w, 1) && near(slerp(id, yaw90, 1).y, h, 1e-12));
    const Quat half = slerp(id, yaw90, 0.5);  // 45 degrees
    CHECK(near(half.y, std::sin(kPi / 8), 1e-12) && near(half.w, std::cos(kPi / 8), 1e-12));
    const Quat close{0, std::sin(0.001), 0, std::cos(0.001)};  // dot > 0.9995: normalized lerp, still unit
    CHECK(near(norm2(slerp(id, close, 0.5)), 1, 1e-12) && near(slerp(id, close, 0.5).y, std::sin(0.0005), 1e-7));

    CHECK(smoothstep(0) == 0 && smoothstep(1) == 1 && smoothstep(0.5) == 0.5 && smoothstep(-3) == 0 && smoothstep(7) == 1);
    CHECK(near(smoothstep(0.25), 0.15625));

    // 32.32 with ties away from zero, checked before the conversion (reference vectors)
    int64_t f = 0;
    CHECK(metres_to_fixed(0.5 / 4294967296.0, &f) && f == 1);
    CHECK(metres_to_fixed(-0.5 / 4294967296.0, &f) && f == -1);
    CHECK(metres_to_fixed(1.5 / 4294967296.0, &f) && f == 2);
    CHECK(metres_to_fixed(-2.5 / 4294967296.0, &f) && f == -3);
    CHECK(metres_to_fixed(10.5, &f) && f == static_cast<int64_t>(10.5 * 4294967296.0));
    CHECK(metres_to_fixed(1000000.0, &f) && !metres_to_fixed(1000000.001, &f) && !metres_to_fixed(-1000000.001, &f));
    CHECK(!metres_to_fixed(INFINITY, &f) && !metres_to_fixed(NAN, &f));
    CHECK(fixed_to_metres(static_cast<int64_t>(-3.25 * 4294967296.0)) == -3.25);

    Quat q{0, 0, 0, 1.004};
    CHECK(engine_unit(&q) && q.w == 1);
    Quat bad{0, 0, 0, 1.1};
    CHECK(!engine_unit(&bad));
    Quat zero{0, 0, 0, 0};
    CHECK(!engine_unit(&zero));
}

// ---- the timeline ---------------------------------------------------------------------------------------------------------

KilnCameraKey key(uint32_t at, double x, double y, double z, Quat q = {}, float fov = 0) {
    KilnCameraKey k{};
    k.at_ms = at;
    k.pose.position[0] = x, k.pose.position[1] = y, k.pose.position[2] = z;
    k.pose.rotation[0] = static_cast<float>(q.x), k.pose.rotation[1] = static_cast<float>(q.y), k.pose.rotation[2] = static_cast<float>(q.z),
    k.pose.rotation[3] = static_cast<float>(q.w);
    k.fov_y_degrees = fov;
    return k;
}

void test_timeline() {
    // reference: keys (0, origin, q, 40) and (1000, (10,0,0), -q, 80); at 500: (5,0,0), |w| = 1, 60 degrees
    const KilnCameraKey two[] = {key(0, 0, 0, 0, {0, 0, 0, 1}, 40), key(1000, 10, 0, 0, {0, 0, 0, -1}, 80)};
    PathPoint p = path_at(two, 2, 500);
    CHECK(near(p.position, {5, 0, 0}) && near(std::fabs(p.rotation.w), 1, 1e-6) && near(p.fov_degrees, 60));
    CHECK(near(path_at(two, 2, -1).position, {0, 0, 0}) && path_at(two, 2, -1).fov_degrees == 40);  // before: the first key
    CHECK(near(path_at(two, 2, 2000).position, {10, 0, 0}) && path_at(two, 2, 2000).fov_degrees == 80);  // after: the last, held
    CHECK(near(path_at(two, 2, 1000).position, {10, 0, 0}));
    // the enclosing segment of many keys, endpoints exact
    std::vector<KilnCameraKey> many;
    for (uint32_t i = 0; i < 256; ++i) many.push_back(key(i * 10, i, 0, -static_cast<double>(i)));
    CHECK(near(path_at(many.data(), 256, 1234).position, {123.4, 0, -123.4}, 1e-9));
    CHECK(near(path_at(many.data(), 256, 2550).position, {255, 0, -255}) && near(path_at(many.data(), 256, 20).position, {2, 0, -2}));
    const double h = std::sqrt(0.5);
    const KilnCameraKey turn[] = {key(0, 0, 0, 0), key(100, 0, 0, 0, {0, h, 0, h})};
    CHECK(near(path_at(turn, 2, 50).rotation.y, std::sin(kPi / 8), 1e-6));
    const KilnCameraKey one[] = {key(0, 1, 2, 3)};
    CHECK(near(path_at(one, 1, 77).position, {1, 2, 3}));

    CHECK(blend_in_weight(0, 100) == 0 && blend_in_weight(50, 100) == 0.5 && blend_in_weight(100, 100) == 1 && blend_in_weight(0, 0) == 1);
    CHECK(blend_out_weight(899, 1000, 100) == 0 && blend_out_weight(950, 1000, 100) == 0.5 && blend_out_weight(1000, 1000, 100) == 1 &&
          blend_out_weight(999, 1000, 0) == 0);
    const View a{{0, 0, 0}, {0, 0, 0, 1}, 1.0}, b{{10, 0, 0}, {0, h, 0, h}, 2.0};
    const View m = mix(a, b, 0.5);
    CHECK(near(m.position, {5, 0, 0}) && near(m.fov, 1.5) && near(m.rotation.y, std::sin(kPi / 8), 1e-12));
}

// ---- the transaction --------------------------------------------------------------------------------------------------------

struct FakeClock : Clock {
    int64_t now = 5'000'000'000;
    int64_t now_ns() const override { return now; }
    void ms(double d) { now += static_cast<int64_t>(d * 1e6); }
};

const std::map<std::string, int64_t> kConstants = {
    {"camera/render.client_world", 464},     {"camera/render.pose_start", 4616872},   {"camera/render.pose_block1", 4616888},
    {"camera/render.pose_block2", 4616904},  {"camera/render.pose_tail", 4616920},    {"camera/render.near", 4616928},
    {"camera/render.far", 4616932},          {"camera/render.selected_fov", 4616936}, {"camera/render.proj_aspect", 4625336},
    {"camera/render.proj_near", 4625340},    {"camera/render.proj_far", 4625344},     {"camera/render.proj_fov", 4625348},
};
constexpr uint64_t kWorld = 0x7ff612340000ull;
constexpr float kNaturalFov = 1.13446f;

struct FakeRenderer : RendererMem {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x469400);
    bool fault_writes = false;
    std::vector<std::pair<uint64_t, size_t>> writes;
    uintptr_t base() const { return reinterpret_cast<uintptr_t>(bytes.data()); }
    bool copy_in(uintptr_t a, void* dst, size_t n) override {
        std::memcpy(dst, reinterpret_cast<const void*>(a), n);
        return true;
    }
    bool copy_out(uintptr_t a, const void* src, size_t n) override {
        writes.push_back({a - base(), n});
        if (fault_writes) return false;
        std::memcpy(reinterpret_cast<void*>(a), src, n);
        return true;
    }
};

struct Env : kcam::Env {
    std::vector<std::string> lines;
    bool call_done(KilnCameraDone fn, const KilnCameraResult& r, void* user) override { fn(&r, user); return true; }
    bool call_changed(KilnCameraChanged fn, const KilnCameraChange& c, void* user) override { fn(&c, user); return true; }
    void log_line(const std::string& s) { lines.push_back(s); }
    void log(int32_t, const std::string&) override {}
};
Env* g_env = nullptr;

struct Result {
    bool seen = false;
    KilnCameraResult r{};
    std::string text;
};
void KILN_CALL on_done(const KilnCameraResult* r, void* user) {
    auto* out = static_cast<Result*>(user);
    out->seen = true;
    out->r = *r;
    out->text.assign(r->reason.ptr ? r->reason.ptr : "", r->reason.len);
}
void KILN_CALL on_changed(const KilnCameraChange* c, void*) {
    static const char* const kKind[] = {"?", "applied", "updated", "ended"};
    char line[160];
    std::snprintf(line, sizeof line, "%s lease=%llu rev=%llu end=%u code=%u frame=%llu", kKind[c->kind < 4 ? c->kind : 0], static_cast<unsigned long long>(c->lease),
                  static_cast<unsigned long long>(c->revision), c->end_reason, c->reason_code, static_cast<unsigned long long>(c->frame));
    g_env->log_line(line);
}

struct LocalPlayer : LocalWorld {
    uint32_t id = 1;
    uint32_t local_player() override { return id; }
};

struct FakeSource : SampleSource {
    std::map<uint32_t, EnginePose> at;
    bool fault = false;
    Read read(uint32_t entity, EnginePose* out) override {
        if (fault) return Read::Fault;
        auto it = at.find(entity);
        if (it == at.end()) return Read::Absent;
        *out = it->second;
        return Read::Ok;
    }
    void put(uint32_t entity, double x, double y, double z, Quat q = {}) {
        EnginePose p{};
        p.position[0] = static_cast<int64_t>(std::llround(x * kFixedOne)), p.position[1] = static_cast<int64_t>(std::llround(y * kFixedOne)),
        p.position[2] = static_cast<int64_t>(std::llround(z * kFixedOne));
        p.rotation[0] = static_cast<float>(q.x), p.rotation[1] = static_cast<float>(q.y), p.rotation[2] = static_cast<float>(q.z), p.rotation[3] = static_cast<float>(q.w);
        p.scale[0] = p.scale[1] = p.scale[2] = 2;  // a scaled target: never changes a distance
        at[entity] = p;
    }
};

// One client with a renderer, the lease model and the backend.
struct Rig {
    FakeClock clock;
    Env env;
    Core core{&env};
    Frames frames{&clock};
    SampleBoard board;
    FakeSource source;
    Sampler sampler{&source, &board, &clock};
    LocalPlayer world;
    LocalBackend backend{&core, &frames, &board, &clock, &world};
    FakeRenderer mem;
    RenderLayout layout;
    uint64_t step = 100;
    std::vector<uint8_t> sentinel;
    EnginePose stock{};
    float fov_reg = kNaturalFov;
    bool other_writer = false;  // something after kiln.camera moves the pose before the frame is prepared

    explicit Rig(uint64_t serving = KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_POSE) {
        g_env = &env;
        std::string why;
        CHECK(RenderLayout::make([](const char* n, int64_t* out) { auto it = kConstants.find(n); return it != kConstants.end() && (*out = it->second, true); }, &layout, &why));
        frames.set_layout(layout);
        frames.set_selection_hooked(true);
        backend.set_serving(serving);
        core.set_backend(KILN_CAMERA_LOCAL_VISUAL, &backend);
        core.set_writes(true);
        KilnHandle h = 0;
        CHECK(core.subscribe(7, &on_changed, nullptr, &h) == KILN_OK);
        for (size_t i = 0; i < mem.bytes.size(); ++i) mem.bytes[i] = static_cast<uint8_t>(i * 131 + 7);  // a sentinel pattern
        std::memcpy(&mem.bytes[layout.client_world], &kWorld, 8);
        set_stock(0, 1.7, 0);
        put_lens();
        source.put(1, 0, 0, 0);
        core.world_started(kClient, 1);
        frames.world_started(1, kWorld);
        sentinel = mem.bytes;
    }
    void set_stock(double x, double y, double z, Quat q = {}) {
        stock = EnginePose{};
        metres_to_fixed(x, &stock.position[0]), metres_to_fixed(y, &stock.position[1]), metres_to_fixed(z, &stock.position[2]);
        stock.rotation[0] = static_cast<float>(q.x), stock.rotation[1] = static_cast<float>(q.y), stock.rotation[2] = static_cast<float>(q.z),
        stock.rotation[3] = static_cast<float>(q.w);
        stock.scale[0] = stock.scale[1] = stock.scale[2] = 1;
    }
    void put_lens() {
        const float lens[4] = {16.f / 9.f, 0.1f, 1000.f, kNaturalFov};
        std::memcpy(&mem.bytes[layout.proj_aspect], lens, sizeof lens);
    }
    // The game's frame: extraction copies the stock pose, the selection hook runs (fov in xmm6), the store, frame
    // preparation copies the lens, the capture hook runs.
    void frame(double advance_ms = 16) {
        clock.ms(advance_ms);
        std::memcpy(&mem.bytes[layout.pose], &stock, kPoseValueBytes);
        fov_reg = kNaturalFov;
        Renderer r(mem, layout, mem.base());
        if (const uint64_t sel = frames.select(r)) backend.on_select(r, sel, &fov_reg);
        std::memcpy(&mem.bytes[layout.sel_fov()], &fov_reg, 4);  // the original movss
        std::memcpy(&mem.bytes[layout.proj_fov()], &fov_reg, 4);  // frame preparation's copy
        if (other_writer) mem.bytes[layout.pose] ^= 1;
        frames.capture(r);
    }
    // Time passes with the client stepping: a Pre, then a frame at once.
    void later(double ms) {
        pre(ms);
        frame(0);
    }
    // The client Pre: the sampler, the backend's step, the core's deliveries.
    bool pre(double advance_ms = 0) {
        clock.ms(advance_ms);
        uint32_t t[kSampleSlots];
        const uint32_t n = backend.targets(t, kSampleSlots - 1);
        sampler.sample(frames.epoch(), ++step, world.local_player(), t, n);
        frames.set_sample_step(step);
        const bool changed = backend.step();
        core.pre(kClient);
        return changed;
    }
    KilnCameraRequest lens(float degrees, uint32_t duration = 10000) {
        KilnCameraRequest q{};
        q.size = sizeof q;
        q.endpoint = KILN_CAMERA_LOCAL_VISUAL;
        q.parts = KILN_CAMERA_LENS;
        q.lens_source = KILN_CAMERA_LENS_CONSTANT;
        q.fov_y_degrees = degrees;
        q.duration_ms = duration;
        q.profile.rotation[3] = 1;
        return q;
    }
    KilnCameraRequest path(const std::vector<KilnCameraKey>& keys, uint32_t duration = 10000) {
        KilnCameraRequest q = lens(0, duration);
        q.parts = KILN_CAMERA_PATH;
        q.lens_source = KILN_CAMERA_LENS_NONE;
        q.fov_y_degrees = 0;
        q.keys = keys.data();
        q.key_count = static_cast<uint32_t>(keys.size());
        return q;
    }
    KilnCameraLease apply(KilnOwner owner, const KilnCameraRequest& q, Result* r) {
        KilnCameraLease lease = 0;
        CHECK(core.apply(owner, &q, &lease, &on_done, r, kOnClient) == KILN_OK);
        return lease;
    }
    EnginePose pose() const {
        EnginePose p{};
        std::memcpy(&p, &mem.bytes[layout.pose], kPoseValueBytes);
        return p;
    }
    Vec3 position() const {
        const EnginePose p = pose();
        return {fixed_to_metres(p.position[0]), fixed_to_metres(p.position[1]), fixed_to_metres(p.position[2])};
    }
    bool pose_is_stock() const { return std::memcmp(&mem.bytes[layout.pose], &stock, kPoseValueBytes) == 0; }
    // every byte but the selected pose's 52 and the two lens copies is as the rig left it
    bool untouched_elsewhere() const {
        for (size_t i = 0; i < mem.bytes.size(); ++i) {
            if (i >= layout.pose && i < layout.pose + kPoseValueBytes) continue;
            if (i >= layout.sel_fov() && i < layout.sel_fov() + 4) continue;
            if (i >= layout.proj_fov() && i < layout.proj_fov() + 4) continue;
            if (mem.bytes[i] != sentinel[i]) return false;
        }
        return true;
    }
};

void test_lens_only() {
    // camera.local_pose off: a lens alone still works, writes only xmm6, needs no sample
    Rig g(KILN_CAMERA_CAP_LOCAL_FOV);
    g.source.at.clear();  // no local player sample at all
    Result r;
    const KilnCameraLease lease = g.apply(30, g.lens(40), &r);
    CHECK(lease);
    g.pre();
    CHECK(!r.seen);
    g.frame();
    CHECK(near(g.fov_reg, radians(40), 1e-6) && g.pose_is_stock());
    CHECK(!r.seen);  // Applied only after the frame showed it, at the next Pre
    g.pre();
    CHECK(r.seen && r.r.ok && r.r.commit_scope == KILN_CAMERA_COMMIT_CLIENT_FRAME && r.r.revision == 1 && r.r.frame == 1 && r.r.player_entity == 1);
    CHECK(g.env.lines.size() == 1 && g.env.lines[0].rfind("applied lease=1 rev=1", 0) == 0);
    for (int i = 0; i < 10; ++i) g.frame();
    CHECK(near(g.fov_reg, radians(40), 1e-6));
    CHECK(g.backend.counters().wrote_pose == 0 && g.mem.writes.empty());
    CHECK(g.untouched_elsewhere());
    // a path with the pose row off is unavailable, not refused
    std::vector<KilnCameraKey> keys = {key(0, 1, 2, 3)};
    KilnCameraLease l2 = 0;
    Result r2;
    KilnCameraRequest p = g.path(keys);
    CHECK(g.core.apply(31, &p, &l2, &on_done, &r2, kOnClient) == KILN_ERR_UNAVAILABLE);
}

void test_path_writes_only_the_pose() {
    Rig g;
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0), key(1000, 10, 10, 0)};
    Result r;
    g.apply(30, g.path(keys, 2000), &r);
    g.pre();
    g.frame();  // T0
    CHECK(near(g.position(), {0, 10, 0}));
    g.pre();
    CHECK(r.seen && r.r.ok);
    g.later(500);
    CHECK(near(g.position(), {5, 10, 0}, 1e-9));
    const EnginePose p = g.pose();
    CHECK(p.scale[0] == 1 && p.scale[1] == 1 && p.scale[2] == 1);  // the scale is the game's
    CHECK(g.fov_reg == kNaturalFov);                               // no lens part: the natural field of view
    g.later(600);
    CHECK(near(g.position(), {10, 10, 0}));  // after the last key: held
    for (const auto& w : g.mem.writes) CHECK(w.first == g.layout.pose && w.second == kPoseValueBytes);
    CHECK(g.untouched_elsewhere());
    // expiry at T0 + 2000 ms: nothing written from then on, Ended(Expired) from the Pre
    g.clock.ms(899);
    g.frame(1);
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.env.lines.back().rfind("ended lease=1 rev=1 end=2", 0) == 0);
}

void test_frame_must_confirm() {
    // something after kiln.camera changes the pose before frame preparation: FrameFault, MayHaveApplied, no Applied
    Rig g;
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
    Result r;
    g.apply(30, g.path(keys), &r);
    g.pre();
    g.other_writer = true;
    g.frame();
    g.pre();
    CHECK(r.seen && !r.r.ok && r.r.reason_code == KILN_CAMERA_REASON_FRAME_FAULT && (r.r.flags & KILN_CAMERA_MAY_HAVE_APPLIED));
    CHECK(g.env.lines.empty());
    g.other_writer = false;
    g.frame();
    CHECK(g.pose_is_stock());  // the refused attempt writes nothing more

    // an active lease whose frame is changed later: ended SafetyRejected with FRAME_FAULT
    Result r2;
    g.apply(30, g.lens(50), &r2);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r2.r.ok);
    // move the projection's field of view after us
    Renderer rr(g.mem, g.layout, g.mem.base());
    g.clock.ms(16);
    std::memcpy(&g.mem.bytes[g.layout.pose], &g.stock, kPoseValueBytes);
    g.fov_reg = kNaturalFov;
    const uint64_t sel = g.frames.select(rr);
    g.backend.on_select(rr, sel, &g.fov_reg);
    const float other = 0.5f;
    std::memcpy(&g.mem.bytes[g.layout.proj_fov()], &other, 4);
    g.frames.capture(rr);
    g.pre();
    CHECK(g.env.lines.back().find("end=11 code=18") != std::string::npos);
}

void test_no_frame_deadline() {
    Rig g;
    Result r;
    g.apply(30, g.lens(40), &r);
    g.pre(999);
    CHECK(!r.seen);
    g.pre(2);  // over 1 s without a frame
    CHECK(r.seen && !r.r.ok && r.r.reason_code == KILN_CAMERA_REASON_NO_CAMERA_FRAME && !(r.r.flags & KILN_CAMERA_MAY_HAVE_APPLIED));
    // written but never confirmed (the capture hook did not run): MayHaveApplied
    Result r2;
    g.apply(30, g.lens(40), &r2);
    g.pre();
    {
        std::memcpy(&g.mem.bytes[g.layout.pose], &g.stock, kPoseValueBytes);  // the extraction's copy, then the hook
        Renderer rr(g.mem, g.layout, g.mem.base());
        const uint64_t sel = g.frames.select(rr);
        g.backend.on_select(rr, sel, &g.fov_reg);
        CHECK(near(g.fov_reg, radians(40), 1e-6));
    }
    g.pre(1001);
    CHECK(r2.seen && r2.r.reason_code == KILN_CAMERA_REASON_NO_CAMERA_FRAME && (r2.r.flags & KILN_CAMERA_MAY_HAVE_APPLIED));
    // a pose request without the local player's sample: nothing written, then NoCameraFrame
    Result r3;
    g.source.at.clear();
    std::vector<KilnCameraKey> keys = {key(0, 0, 5, 0)};
    g.apply(30, g.path(keys), &r3);
    g.pre();
    g.frame();
    CHECK(g.pose_is_stock());
    g.pre(1001);
    CHECK(r3.seen && r3.r.reason_code == KILN_CAMERA_REASON_NO_CAMERA_FRAME);
}

void test_updates_and_timeline() {
    Rig g;
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0), key(1000, 10, 10, 0)};
    Result r;
    const KilnCameraLease lease = g.apply(30, g.path(keys, 3000), &r);
    g.pre();
    g.frame();
    g.pre();
    g.later(400);
    CHECK(near(g.position(), {4, 10, 0}, 1e-9));
    // Preserve: the new curve at the time already elapsed
    std::vector<KilnCameraKey> keys2 = {key(0, 0, 20, 0), key(1000, 10, 20, 0)};
    KilnCameraRequest q2 = g.path(keys2, 3000);
    Result u;
    CHECK(g.core.update(30, lease, KILN_CAMERA_PRESERVE_TIMELINE, &q2, &on_done, &u, kOnClient) == KILN_OK);
    g.later(100);
    CHECK(near(g.position(), {5, 20, 0}, 1e-9));
    g.pre();
    CHECK(u.seen && u.r.ok && u.r.revision == 2 && g.env.lines.back().rfind("updated lease=1 rev=2", 0) == 0);
    // Restart: T0 again
    Result u2;
    CHECK(g.core.update(30, lease, KILN_CAMERA_RESTART_TIMELINE, &q2, &on_done, &u2, kOnClient) == KILN_OK);
    g.later(100);
    CHECK(near(g.position(), {0, 20, 0}, 1e-9));
    g.pre();
    CHECK(u2.r.ok && u2.r.revision == 3);
    g.later(250);
    CHECK(near(g.position(), {2.5, 20, 0}, 1e-9));
    // Preserve with no time left: ExpiredUpdate, the old revision stays
    std::vector<KilnCameraKey> keys3 = {key(0, 0, 30, 0), key(100, 1, 30, 0)};
    KilnCameraRequest shortq = g.path(keys3, 200);
    Result u3;
    CHECK(g.core.update(30, lease, KILN_CAMERA_PRESERVE_TIMELINE, &shortq, &on_done, &u3, kOnClient) == KILN_OK);
    g.pre();
    CHECK(u3.seen && !u3.r.ok && u3.r.reason_code == KILN_CAMERA_REASON_EXPIRED_UPDATE && g.core.active_revision(lease) == 3);
    // Preserve with less time left than the new blends: InsufficientLifetime, the old revision stays
    KilnCameraRequest blends = g.path(keys3, 600);
    blends.blend_in_ms = 200, blends.blend_out_ms = 200;
    Result u4;
    CHECK(g.core.update(30, lease, KILN_CAMERA_PRESERVE_TIMELINE, &blends, &on_done, &u4, kOnClient) == KILN_OK);
    g.pre();
    CHECK(u4.seen && !u4.r.ok && u4.r.reason_code == KILN_CAMERA_REASON_INSUFFICIENT_LIFETIME && g.core.active_revision(lease) == 3);
    g.frame();
    CHECK(near(g.position(), {2.66, 20, 0}, 1e-9));  // revision 3 goes on
}

void test_blends_and_end() {
    Rig g(KILN_CAMERA_CAP_LOCAL_FOV);
    KilnCameraRequest q = g.lens(40, 1000);
    q.blend_in_ms = 100, q.blend_out_ms = 200;
    Result r;
    const KilnCameraLease lease = g.apply(30, q, &r);
    g.pre();
    g.frame();  // commit frame: from the natural lens
    CHECK(near(g.fov_reg, kNaturalFov, 1e-6));
    g.pre();
    CHECK(r.r.ok);
    g.frame(50);  // halfway through the blend in (smoothstep 0.5)
    CHECK(near(g.fov_reg, (kNaturalFov + radians(40)) / 2, 1e-6));
    g.frame(50);
    CHECK(near(g.fov_reg, radians(40), 1e-6));
    g.frame(800);  // 900 ms: in the blend out, 0.5 of the way back (smoothstep(0.5))
    CHECK(near(g.fov_reg, (kNaturalFov + radians(40)) / 2, 1e-6));
    // End with a blend out: from the last shown lens to the natural one over 200 ms; done only after it
    Result e;
    CHECK(g.core.end(30, lease, &on_done, &e, kOnClient) == KILN_OK);
    g.pre();
    CHECK(!e.seen);
    const float last = g.fov_reg;
    g.frame(1);  // the release starts here
    CHECK(near(g.fov_reg, last, 1e-6));
    g.frame(100);
    CHECK(near(g.fov_reg, (last + kNaturalFov) / 2, 1e-6));
    g.frame(100);
    CHECK(g.fov_reg == kNaturalFov);
    g.pre();
    CHECK(e.seen && e.r.ok && g.env.lines.back().rfind("ended lease=1 rev=1 end=1", 0) == 0);

    // End without a blend out: at once
    Result r2, e2;
    const KilnCameraLease l2 = g.apply(30, g.lens(60), &r2);
    g.pre();
    g.frame();
    g.pre();
    CHECK(g.core.end(30, l2, &on_done, &e2, kOnClient) == KILN_OK);
    g.frame();
    CHECK(g.fov_reg == kNaturalFov);
    g.pre();
    CHECK(e2.seen && e2.r.ok);
}

void test_supersession() {
    Rig g;
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
    Result a, b;
    const KilnCameraLease la = g.apply(30, g.path(keys), &a);
    g.pre();
    g.frame();
    g.pre();
    CHECK(a.r.ok);
    // another owner, a higher priority, a lens with a blend in: shown from its first frame, from the path's view
    KilnCameraRequest q = g.lens(40);
    q.priority = 5;
    q.blend_in_ms = 100;
    const KilnCameraLease lb = g.apply(40, q, &b);
    g.pre();
    g.frame();
    CHECK(g.pose_is_stock() && near(g.fov_reg, kNaturalFov, 1e-6));  // the path's part is gone at once; the lens blends from the shown one
    g.pre();
    CHECK(b.r.ok && b.r.lease == lb);
    const size_t n = g.env.lines.size();
    CHECK(n >= 2 && g.env.lines[n - 2].rfind("ended lease=" + std::to_string(la) + " rev=1 end=6", 0) == 0 &&
          g.env.lines[n - 1].rfind("applied lease=" + std::to_string(lb), 0) == 0);
    // an equal priority is refused Busy and leaves it alone
    Result c;
    KilnCameraRequest q3 = g.lens(30);
    q3.priority = 5;
    g.apply(50, q3, &c);
    g.pre();
    CHECK(c.seen && c.r.reason_code == KILN_CAMERA_REASON_BUSY);
}

void test_limits_and_safety() {
    Rig g;
    // a key more than 250 m from the player is refused before anything is written
    g.pre();
    std::vector<KilnCameraKey> far_keys = {key(0, 0, 0, 0), key(100, 251, 0, 0)};
    Result r;
    g.apply(30, g.path(far_keys), &r);
    g.pre();
    CHECK(r.seen && r.r.reason_code == KILN_CAMERA_REASON_BOUNDS);
    // a lease whose player walks away: the derived view leaves the 250 m bound and it ends SafetyRejected
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
    Result r2;
    g.apply(30, g.path(keys), &r2);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r2.r.ok);
    g.source.put(1, 300, 0, 0);
    g.pre();
    g.frame();
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.env.lines.back().find("end=11 code=21") != std::string::npos);
    // no client Pre for over 250 ms: a pose lease ends SafetyRejected; a lens lease goes on
    g.source.put(1, 0, 0, 0);
    g.pre();  // the player is back near the keys (the stage checks them against the newest sample)
    Result r3, r4;
    g.apply(30, g.path(keys), &r3);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r3.r.ok);
    g.frame(251);
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.env.lines.back().find("end=11 code=0") != std::string::npos);
    g.apply(30, g.lens(40), &r4);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r4.r.ok);
    g.frame(300);
    g.frame(300);
    CHECK(near(g.fov_reg, radians(40), 1e-6));
    // the game's own view is one v1 does not take (a scaled camera): nothing is written
    g.stock.scale[1] = 1.5f;
    g.frame();
    CHECK(g.fov_reg == kNaturalFov && g.pose_is_stock());
    g.stock.scale[1] = 1;
}

void test_world_owner_and_fault() {
    Rig g;
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
    Result r;
    g.apply(30, g.path(keys), &r);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r.r.ok);
    // the owner retires: nothing is written from that moment on, before any Pre
    g.core.owner_retiring(30);
    g.frame();
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.backend.plans_held() <= 1);

    // the world stops: nothing of it is written, its lease ends WorldStopped
    Result r2;
    g.apply(31, g.lens(40), &r2);
    g.pre();
    g.frame();
    g.pre();
    CHECK(r2.r.ok);
    g.core.world_stopping(kClient, 1);
    g.frame();
    CHECK(g.fov_reg == kNaturalFov);
    CHECK(g.env.lines.back().find("end=5") != std::string::npos);
    g.core.world_started(kClient, 2);
    g.frames.world_started(2, kWorld);
    g.pre();
    g.frame();
    CHECK(g.fov_reg == kNaturalFov);

    // a write that faults: no visual write for the rest of the world, the rows go off, the lease ends
    Result r3;
    g.apply(32, g.path(keys), &r3);
    g.pre();
    g.mem.fault_writes = true;
    g.frame();
    CHECK(g.pre());  // caps changed
    CHECK(r3.seen && !r3.r.ok && r3.r.reason_code == KILN_CAMERA_REASON_FRAME_FAULT && (r3.r.flags & KILN_CAMERA_MAY_HAVE_APPLIED));
    CHECK(g.backend.caps() == 0);
    uint64_t mask = 0;
    g.core.capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &mask);
    CHECK(mask == 0);
    g.mem.fault_writes = false;
    g.core.world_stopping(kClient, 2);
    g.core.world_started(kClient, 3);
    g.frames.world_started(3, kWorld);
    CHECK(g.pre());  // a new world: the rows come back
    CHECK(g.backend.caps() != 0);
}


// ---- S6: the follow profile, the shake, the sampler -----------------------------------------------------------------------

Quat yaw_q(double a) { return {0, std::sin(a / 2), 0, std::cos(a / 2)}; }
Quat pitch_q(double a) { return {std::sin(a / 2), 0, 0, std::cos(a / 2)}; }

KilnCameraProfile profile_of(uint32_t follow, float distance, uint32_t mode, Quat rotation = {}) {
    KilnCameraProfile p{};
    p.size = sizeof p;
    p.follow_entity = follow;
    p.distance = distance;
    p.rotation_mode = mode;
    p.rotation[0] = static_cast<float>(rotation.x), p.rotation[1] = static_cast<float>(rotation.y), p.rotation[2] = static_cast<float>(rotation.z),
    p.rotation[3] = static_cast<float>(rotation.w);
    return p;
}

void test_profile_geometry() {
    const Quat id{}, yaw90 = yaw_q(kPi / 2);
    // reference: a target at (10,0,0) turned +90 degrees about Y, distance 5, offset (0,0,2) in its axes, 3 m up, the
    // stock orientation identity (free orbit): camera at (12, 3, -5); absolute metres
    KilnCameraProfile pr = profile_of(9, 5, KILN_CAMERA_FREE_ORBIT);
    pr.offset_local[2] = 2;
    pr.offset_world_y = 3;
    HeadingMemory m;
    Vec3 pos;
    Quat rot;
    CHECK(profile_view(pr, {10, 0, 0}, yaw90, id, &m, &pos, &rot) == ProfileResult::Ok);
    CHECK(near(pos, {12, 3, -5}, 1e-9) && rot.w == 1);
    // free orbit follows the game's orientation: behind the pivot along the stock forward
    CHECK(profile_view(pr, {10, 0, 0}, yaw90, yaw90, &m, &pos, &rot) == ProfileResult::Ok && near(pos, {12 - 5, 3, 0}, 1e-9) && near(rot.y, yaw90.y, 1e-12));

    // the heading drops pitch and roll and keeps the yaw
    Quat h;
    const Quat banked = mul(mul(yaw_q(0.8), pitch_q(0.6)), Quat{0, 0, std::sin(0.2), std::cos(0.2)});
    CHECK(heading_of(banked, &h));
    CHECK(near(rotate(h, {0, 0, 1}).y, 0, 1e-12) && near(rotate(h, {0, 1, 0}).y, 1, 1e-12));
    CHECK(near(h.y, std::sin(0.4), 1e-9) && near(h.w, std::cos(0.4), 1e-9));
    // follow heading with the flyer's 12 degrees down: behind, above the pivot, looking down; pitch of the target ignored
    KilnCameraProfile fh = profile_of(9, 10, KILN_CAMERA_FOLLOW_HEADING, pitch_q(12 * kPi / 180));
    fh.offset_world_y = 4;
    HeadingMemory mh;
    CHECK(profile_view(fh, {0, 0, 0}, mul(yaw90, pitch_q(-1.0)), id, &mh, &pos, &rot) == ProfileResult::Ok);
    const Vec3 fwd = rotate(rot, {0, 0, 1});
    CHECK(fwd.x > 0.97 && near(fwd.y, -std::sin(12 * kPi / 180), 1e-6) && pos.x < 0 && pos.y > 4);  // the rotation crossed the ABI as floats
    CHECK(near(length(pos - Vec3{0, 4, 0}), 10, 1e-9));  // exactly the distance from the pivot
    // straight up: no heading of its own; the last one stays, and a first frame has none
    Vec3 pos2;
    Quat rot2;
    CHECK(profile_view(fh, {0, 0, 0}, pitch_q(-kPi / 2), id, &mh, &pos2, &rot2) == ProfileResult::Ok && near(rot2.y, rot.y, 1e-12));
    HeadingMemory fresh;
    CHECK(profile_view(fh, {0, 0, 0}, pitch_q(-kPi / 2), id, &fresh, &pos2, &rot2) == ProfileResult::NoHeading);
    // fixed world: the rotation as given, the target's turning ignored
    KilnCameraProfile fw = profile_of(9, 2, KILN_CAMERA_FIXED_WORLD, yaw90);
    CHECK(profile_view(fw, {1, 1, 1}, yaw_q(2.5), id, &fresh, &pos2, &rot2) == ProfileResult::Ok && near(pos2, {-1, 1, 1}, 1e-6));
}

void test_shake() {
    KilnCameraShake sh{0.5f, 7.0f, 2000, 123};
    // the reference model's own numbers (shake(.5, 7, 123, t, 10, 2), seconds there)
    const Vec3 a = shake_offset(sh, 1000, 10000);
    CHECK(near(a, {0.013374902915747337, -0.11353465475813093, 0.019346954361648922}, 1e-7));
    const Vec3 b = shake_offset(sh, 9500, 10000);  // in the falloff: a quarter strength
    CHECK(near(b, {-0.003343725728937576, 0.028383663689534877, -0.004836738590411489}, 1e-7));
    KilnCameraShake zero{0.25f, 3.0f, 0, 0};  // seed 0 is 0x9e3779b9
    CHECK(near(shake_offset(zero, 400, 10000), {-0.015021416250746041, 0.0660985061110754, -0.1318466855854053}, 1e-7));
    const ShakePhases p0 = shake_phases(0), pg = shake_phases(0x9e3779b9u);
    CHECK(p0.phi[0] == pg.phi[0] && p0.phi[1] == pg.phi[1] && p0.phi[2] == pg.phi[2]);
    // never above the amplitude, equal for equal seeds, zero at the end
    for (int i = 0; i <= 200; ++i) {
        const Vec3 v = shake_offset(sh, i * 50.0, 10000);
        CHECK(length(v) <= 0.5 + 1e-12);
        const Vec3 w = shake_offset(sh, i * 50.0, 10000);
        CHECK(v.x == w.x && v.y == w.y && v.z == w.z);
    }
    KilnCameraShake other = sh;
    other.seed = 124;
    CHECK(!near(shake_offset(other, 1000, 10000), a, 1e-6));
    CHECK(shake_offset(sh, 10000, 10000).x == 0 && length(shake_offset(sh, 10000, 10000)) == 0);
    CHECK(shake_gain(9000, 10000, 2000) == 0.5 && shake_gain(5000, 10000, 2000) == 1 && shake_gain(9999, 10000, 0) == 1 && shake_gain(10000, 10000, 0) == 0);
}

void test_sampler() {
    FakeClock c;
    SampleBoard board;
    FakeSource src;
    Sampler s(&src, &board, &c);
    src.put(1, 1, 2, 3, yaw_q(0.5));
    src.put(5, -4, 0, 9);
    const uint32_t targets[] = {5, 0, 1, 5, 7, 8};
    s.sample(3, 9, 1, targets, 6);
    SampleSet set;
    CHECK(board.try_read(&set));
    CHECK(set.s[0].entity == 1 && set.s[0].state == kSamplePresent && set.s[0].step == 9 && set.s[0].epoch == 3 && set.s[0].ns == c.now);
    CHECK(near(Vec3{set.s[0].position[0], set.s[0].position[1], set.s[0].position[2]}, {1, 2, 3}) && near(set.s[0].rotation[1], std::sin(0.25), 1e-6));
    // targets once each, never the local player again, 0 skipped; a missing one is "absent"
    CHECK(set.s[1].entity == 5 && set.s[1].state == kSamplePresent && set.s[2].entity == 7 && set.s[2].state == kSampleAbsent && set.s[3].entity == 8);
    CHECK(set.find(0) == &set.s[0] && set.find(5)->position[0] == -4 && set.find(7)->state == kSampleAbsent && !set.find(99));
    // a fault: the sampler stops (nothing usable) until the next world
    src.fault = true;
    s.sample(3, 10, 1, targets, 1);
    CHECK(s.faulted() && board.try_read(&set) && set.s[0].state == kSampleNone);
    src.fault = false;
    s.sample(3, 11, 1, targets, 1);
    CHECK(set.s[0].state == kSampleNone);
    s.world_started();
    s.sample(4, 12, 1, targets, 1);
    CHECK(!s.faulted() && board.try_read(&set) && set.s[0].state == kSamplePresent && set.s[0].epoch == 4);
}

constexpr uint64_t kAllLocal = KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE;

KilnCameraRequest follow_request(Rig& g, const KilnCameraProfile& pr, uint32_t duration = 10000) {
    KilnCameraRequest q = g.lens(0, duration);
    q.parts = KILN_CAMERA_PROFILE;
    q.lens_source = KILN_CAMERA_LENS_NONE;
    q.fov_y_degrees = 0;
    q.profile = pr;
    return q;
}

void test_follow_camera() {
    Rig g(kAllLocal);
    g.source.put(5, 10, 0, 0, yaw_q(kPi / 2));  // its scale is 2: no distance changes with it
    KilnCameraProfile pr = profile_of(5, 4, KILN_CAMERA_FOLLOW_HEADING);
    pr.offset_world_y = 2;
    Result r;
    const KilnCameraLease lease = g.apply(30, follow_request(g, pr), &r);
    uint32_t t[4];
    CHECK(g.backend.targets(t, 3) == 1 && t[0] == 5);  // the sampler copies the target from the next Pre
    g.pre();
    g.frame(0);
    CHECK(near(g.position(), {6, 2, 0}, 1e-6));  // 4 m behind the pivot (10, 2, 0) along the heading +X
    CHECK(near(g.pose().rotation[1], std::sin(kPi / 4), 1e-6));
    g.pre();
    CHECK(r.seen && r.r.ok);
    // one completed step of latency: the frame uses the newest Pre's copy, not where the target is now
    g.source.put(5, 12, 0, 0, yaw_q(kPi / 2));
    g.frame(0);
    CHECK(near(g.position(), {6, 2, 0}, 1e-6));
    g.later(16);
    CHECK(near(g.position(), {8, 2, 0}, 1e-6));
    // the snapshot says which step's samples the frame was built from
    KilnCameraVisual v{};
    v.size = sizeof v;
    CHECK(g.frames.get_visual(&v) == 0 && v.target_sample_step == g.step);
    // the target points straight up: its heading is kept
    g.source.put(5, 12, 0, 0, pitch_q(-kPi / 2));
    g.later(16);
    CHECK(near(g.position(), {8, 2, 0}, 1e-6) && near(g.pose().rotation[1], std::sin(kPi / 4), 1e-6));
    // a Preserve update with the same target and mode keeps the heading; a Restart forgets it
    KilnCameraRequest q2 = follow_request(g, pr);
    Result u;
    CHECK(g.core.update(30, lease, KILN_CAMERA_PRESERVE_TIMELINE, &q2, &on_done, &u, kOnClient) == KILN_OK);
    g.later(16);
    g.pre();
    CHECK(u.seen && u.r.ok && near(g.pose().rotation[1], std::sin(kPi / 4), 1e-6));
    Result u2;
    CHECK(g.core.update(30, lease, KILN_CAMERA_RESTART_TIMELINE, &q2, &on_done, &u2, kOnClient) == KILN_OK);
    g.later(16);
    g.pre(1001);
    CHECK(u2.seen && !u2.r.ok && u2.r.reason_code == KILN_CAMERA_REASON_TARGET_UNAVAILABLE && g.core.active_revision(lease) == 2);
    // the target is gone: TargetLost
    g.source.put(5, 12, 0, 0, yaw_q(kPi / 2));
    g.later(16);
    g.source.at.erase(5);
    g.later(16);
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.env.lines.back().find("end=3 code=10") != std::string::npos);
}

void test_follow_refusals() {
    Rig g(kAllLocal);
    // a target the client world does not have: TargetUnavailable from the Pre that asked for it
    Result r;
    g.apply(30, follow_request(g, profile_of(77, 4, KILN_CAMERA_FIXED_WORLD)), &r);
    g.pre();
    g.pre();
    CHECK(r.seen && !r.r.ok && r.r.reason_code == KILN_CAMERA_REASON_TARGET_UNAVAILABLE);
    // a follow-heading target that points straight up on the first frame: TargetUnavailable
    g.source.put(6, 0, 0, 5, pitch_q(kPi / 2));
    Result r2;
    g.apply(30, follow_request(g, profile_of(6, 4, KILN_CAMERA_FOLLOW_HEADING)), &r2);
    g.pre();
    g.frame(0);
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(r2.seen && r2.r.reason_code == KILN_CAMERA_REASON_TARGET_UNAVAILABLE);
    // follow 0 is the local player; a stale target sample after it was shown ends the lease SafetyRejected
    g.source.put(1, 0, 0, 0, yaw_q(0));
    Result r3;
    g.apply(30, follow_request(g, profile_of(0, 3, KILN_CAMERA_FOLLOW_HEADING)), &r3);
    g.pre();
    g.frame(0);
    CHECK(near(g.position(), {0, 0, -3}, 1e-9));
    g.pre();
    CHECK(r3.r.ok);
    g.frame(251);
    CHECK(g.pose_is_stock());
    g.pre();
    CHECK(g.env.lines.back().find("end=11") != std::string::npos);
}

void test_shake_through_the_transaction() {
    // a shake alone moves the game's own pose in the camera's axes, never turns it, and two clients agree
    std::vector<Vec3> seen[2];
    for (int run = 0; run < 2; ++run) {
        Rig g(kAllLocal);
        g.set_stock(3, 2, 1, yaw_q(kPi / 2));
        KilnCameraRequest q = g.lens(0, 1000);
        q.parts = KILN_CAMERA_SHAKE;
        q.lens_source = KILN_CAMERA_LENS_NONE;
        q.fov_y_degrees = 0;
        q.shake = KilnCameraShake{0.5f, 2.0f, 0, 9};
        Result r;
        g.apply(30, q, &r);
        g.pre();
        for (int i = 0; i < 20; ++i) {
            g.later(i ? 37 : 0);
            const Vec3 d = g.position() - Vec3{3, 2, 1};
            CHECK(length(d) <= 0.5 + 1e-6);
            CHECK(near(g.pose().rotation[1], g.stock.rotation[1], 1e-6) && near(g.pose().rotation[3], g.stock.rotation[3], 1e-6) && g.pose().rotation[0] == 0);
            const Vec3 want = rotate(yaw_q(kPi / 2), shake_offset(q.shake, 37.0 * i, 1000));
            CHECK(near(d, want, 1e-6));
            seen[run].push_back(d);
        }
        CHECK(r.r.ok);
    }
    for (size_t i = 0; i < seen[0].size(); ++i) CHECK(seen[0][i].x == seen[1][i].x && seen[0][i].y == seen[1][i].y && seen[0][i].z == seen[1][i].z);
}

// ---- review fixes ---------------------------------------------------------------------------------------------------------

// M1: a drawn staged request that the render path ends before the next Pre (stale sample, more than 250 m, target
// gone) is committed (a frame did show it) and ends at once with the right reason; it never holds the camera on.
void test_drawn_then_ended_before_pre() {
    for (int variant = 0; variant < 3; ++variant) {
        Rig g(kAllLocal);
        g.source.put(5, 10, 0, 0, yaw_q(kPi / 2));
        std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
        KilnCameraRequest q = variant == 2 ? follow_request(g, profile_of(5, 4, KILN_CAMERA_FIXED_WORLD)) : g.path(keys, 10000);
        q.blend_out_ms = 500;
        Result r;
        const KilnCameraLease lease = g.apply(30, q, &r);
        g.pre();
        g.frame(0);  // drawn; the capture confirms it
        CHECK(!g.pose_is_stock());
        if (variant == 0) {
            g.frame(300);  // no Pre for 300 ms (a simulation hitch while the renderer draws on): the sample is stale
        } else if (variant == 1) {
            g.source.put(1, 1000, 0, 0);  // the player teleports: the copy reaches the board before the backend's step
            g.sampler.sample(g.frames.epoch(), ++g.step, 1, nullptr, 0);
            g.frame(0);
        } else {
            g.source.at.erase(5);  // the target goes
            const uint32_t t[] = {5};
            g.sampler.sample(g.frames.epoch(), ++g.step, 1, t, 1);
            g.frame(0);
        }
        CHECK(g.pose_is_stock());
        g.pre();
        CHECK(r.seen && r.r.ok);  // a frame showed it
        CHECK(g.core.live_leases() == 0);
        const size_t n = g.env.lines.size();
        CHECK(n == 2 && g.env.lines[0].rfind("applied lease=" + std::to_string(lease), 0) == 0);
        const char* want = variant == 0 ? "end=11 code=0" : variant == 1 ? "end=11 code=21" : "end=3 code=10";
        CHECK(g.env.lines[1].rfind("ended lease=" + std::to_string(lease), 0) == 0 && g.env.lines[1].find(want) != std::string::npos);
        // End after it: already ended, at once (no blend out to wait for)
        Result e;
        CHECK(g.core.end(30, lease, &on_done, &e, kOnClient) == KILN_OK);
        g.pre();
        CHECK(e.seen && e.r.ok && (e.r.flags & KILN_CAMERA_ALREADY_ENDED));
        // the camera is free for the next request
        Result r2;
        g.apply(31, g.lens(40), &r2);
        g.pre();
        g.frame(0);
        g.pre();
        CHECK(r2.seen && r2.r.ok);
    }
    // End with a blend out that cannot finish (the sample goes stale during it): it ends, it does not hang
    Rig g(kAllLocal);
    std::vector<KilnCameraKey> keys = {key(0, 0, 10, 0)};
    KilnCameraRequest q = g.path(keys, 10000);
    q.blend_out_ms = 500;
    Result r, e;
    const KilnCameraLease lease = g.apply(30, q, &r);
    g.pre();
    g.frame(0);
    g.pre();
    CHECK(r.r.ok);
    CHECK(g.core.end(30, lease, &on_done, &e, kOnClient) == KILN_OK);
    g.frame(0);
    g.frame(300);
    g.pre();
    CHECK(e.seen && e.r.ok && g.core.live_leases() == 0);
}






}  // namespace

int main(int argc, char** argv) {
    // with a name: only that test (the red-first runs)
    const struct {
        const char* name;
        void (*fn)();
    } named[] = {{"drawn_then_ended", test_drawn_then_ended_before_pre}};
    if (argc > 1) {
        for (const auto& t : named)
            if (std::strcmp(argv[1], t.name) == 0) {
                t.fn();
                std::printf("camera render %s: PASS (%d checks)\n", t.name, checks);
                return 0;
            }
        std::fprintf(stderr, "no test %s\n", argv[1]);
        return 2;
    }
    test_math();
    test_timeline();
    test_lens_only();
    test_path_writes_only_the_pose();
    test_frame_must_confirm();
    test_no_frame_deadline();
    test_updates_and_timeline();
    test_blends_and_end();
    test_supersession();
    test_limits_and_safety();
    test_world_owner_and_fault();
    test_profile_geometry();
    test_shake();
    test_sampler();
    test_follow_camera();
    test_follow_refusals();
    test_shake_through_the_transaction();
    for (const auto& t : named) t.fn();
    std::printf("camera render: PASS (%d checks)\n", checks);
}
