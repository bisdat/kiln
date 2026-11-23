// SPDX-License-Identifier: MIT
// kiln.camera's client side through the real entry point (camera.cpp + local.cpp) on a host: a fake core with
// kiln.hooks (mid2 records the hooks), kiln.caps (records the rows), kiln.client, kiln.types and a fake GetComponent.
// One scenario per process (the service keeps globals): run.sh runs each by name.
//   ok             both hooks, every local row, a lens through the hooks: only xmm6's low float changes, get_visual
//   no-constants   the renderer's layout constants are missing: no hook, the rows say why
//   no-capture     the capture place is not resolved: no write row, no snapshot
//   no-mid2        kiln.hooks predates ordered mid hooks
//   bad-reflection RenderTransform is not the checked type: lens and snapshot only
//   pose-row-off   the sigdb row camera.local_pose is off: that row only
//   sampler-fault  GetComponent answers another size in a world: pose, profile and shake off until the next world
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "services/kiln_camera.h"
#include "services/kiln_caps.h"
#include "services/kiln_client.h"
#include "services/kiln_hooks.h"
#include "services/kiln_types.h"
#include "../seh.h"

extern "C" KilnResult KILN_CALL kiln_plugin_main(const KilnApi*, KilnOwner, KilnPluginInfo*);
namespace kcam {
bool seh_done(KilnCameraDone fn, const KilnCameraResult* r, void* u) { fn(r, u); return true; }
bool seh_changed(KilnCameraChanged fn, const KilnCameraChange* r, void* u) { fn(r, u); return true; }
}  // namespace kcam

static int checks;
#define CHECK(x)                                                         \
    do {                                                                 \
        ++checks;                                                        \
        if (!(x)) {                                                      \
            std::fprintf(stderr, "FAIL client:%d: %s\n", __LINE__, #x);  \
            std::exit(1);                                                \
        }                                                                \
    } while (0)

namespace {

constexpr KilnOwner kService = 10, kAuthor = 20;
constexpr uintptr_t kSelectAt = 0x1000, kCaptureAt = 0x2000;
constexpr uint32_t kAnyReason = UINT32_MAX;
constexpr float kNaturalFov = 1.13446f;

std::string scenario;
std::string str(KilnStr s) { return std::string(s.ptr ? s.ptr : "", s.len); }

const std::map<std::string, int64_t> kConstants = {
    {"camera/render.client_world", 464},     {"camera/render.pose_start", 4616872},   {"camera/render.pose_block1", 4616888},
    {"camera/render.pose_block2", 4616904},  {"camera/render.pose_tail", 4616920},    {"camera/render.near", 4616928},
    {"camera/render.far", 4616932},          {"camera/render.selected_fov", 4616936}, {"camera/render.proj_aspect", 4625336},
    {"camera/render.proj_near", 4625340},    {"camera/render.proj_far", 4625344},     {"camera/render.proj_fov", 4625348},
};

// ---- kiln.hooks: mid2 records, the owner observer is there ----
struct Hook {
    uintptr_t at;
    KilnMidFn fn;
    void* user;
    int32_t priority;
    std::string label;
};
std::vector<Hook> hooks;
KilnHooksApi hooks_table{};
KilnResult KILN_CALL mid2(KilnOwner owner, uintptr_t at, const KilnHookOrder* order, KilnMidFn fn, void* user, KilnHandle* out) {
    CHECK(owner == kService && order && order->size == sizeof(KilnHookOrder));
    hooks.push_back({at, fn, user, order->priority, str(order->label)});
    *out = hooks.size();
    return KILN_OK;
}
KilnResult KILN_CALL subscribe_owner(KilnOwner, KilnOwnerRetiringFn, void*, KilnHandle* h) {
    *h = 99;
    return KILN_OK;
}
const Hook* hook_at(uintptr_t at) {
    for (const Hook& h : hooks)
        if (h.at == at) return &h;
    return nullptr;
}

// ---- kiln.caps: the newest report of each row ----
struct Row {
    uint32_t state = 99, reason = 0;
    std::string text;
};
std::map<std::string, Row> rows;
KilnCapsApi caps_table{};
KilnResult KILN_CALL report(KilnOwner owner, KilnStr name, uint32_t state, uint32_t reason, KilnStr text) {
    CHECK(owner == kService);
    CHECK((state == KILN_CAP_AVAILABLE) == (reason == KILN_CAP_REASON_NONE));
    rows[str(name)] = Row{state, reason, str(text)};
    return KILN_OK;
}
void available(const char* name) {
    const Row& r = rows[name];
    if (r.state != KILN_CAP_AVAILABLE) std::fprintf(stderr, "  row %s: state %u reason %u '%s'\n", name, r.state, r.reason, r.text.c_str());
    CHECK(r.state == KILN_CAP_AVAILABLE);
}
void unavailable(const char* name, uint32_t reason, const char* words = nullptr) {
    const Row& r = rows[name];
    if (r.state != KILN_CAP_UNAVAILABLE || (reason != kAnyReason && r.reason != reason))
        std::fprintf(stderr, "  row %s: state %u reason %u '%s'\n", name, r.state, r.reason, r.text.c_str());
    CHECK(r.state == KILN_CAP_UNAVAILABLE);
    CHECK(reason == kAnyReason || r.reason == reason);
    CHECK(!words || r.text.find(words) != std::string::npos);
    CHECK(r.text.find("does not have it yet") == std::string::npos);  // a reason, never "not built"
}

// ---- kiln.client: the local player ----
KilnClientApi client_table{};
KilnResult KILN_CALL client_state(KilnClientState* out) {
    out->side = KILN_SIDE_CLIENT;
    out->local_player = 1;
    out->flags = KILN_CLIENT_LOCAL_PLAYER | KILN_CLIENT_LIFECYCLE;
    return KILN_OK;
}

// ---- kiln.types: RenderTransform's reflection ----
struct FakeField {
    const char* name;
    const struct FakeType* type;
    uint64_t offset;
};
struct FakeType {
    const char* name;
    const char* qualified;
    uint32_t size;
    uint8_t kind;
    uint32_t hash;
    std::vector<FakeField> fields;
};
FakeType t_pos{"WorldPosition", "keen::WorldPosition", 24, KILN_TYPE_STRUCT, 1, {}};
FakeType t_quat{"Quaternion", "keen::Quaternion", 16, KILN_TYPE_STRUCT, 2, {}};
FakeType t_scale{"float3", "keen::float3", 12, KILN_TYPE_STRUCT, 3, {}};
FakeType t_wt{"WorldTransform", "keen::WorldTransform", 56, KILN_TYPE_STRUCT, 4, {{"position", &t_pos, 0}, {"orientation", &t_quat, 24}, {"scale", &t_scale, 40}}};
FakeType t_rt{"RenderTransform", "keen::ecs::RenderTransform", 176, KILN_TYPE_STRUCT, 0x3b8476dd, {{"transform", &t_wt, 0}}};
KilnTypesApi types_table{};
const KilnType* KILN_CALL type_by_hash(uint32_t hash) { return hash == t_rt.hash ? reinterpret_cast<const KilnType*>(&t_rt) : nullptr; }
KilnResult KILN_CALL type_info(const KilnType* type, KilnTypeInfo* out) {
    const auto* t = reinterpret_cast<const FakeType*>(type);
    out->name = KilnStr{t->name, std::strlen(t->name)};
    out->qualified_name = KilnStr{t->qualified, std::strlen(t->qualified)};
    out->type_size = t->size;
    out->kind = t->kind;
    out->hash = t->hash;
    out->field_count = static_cast<uint32_t>(t->fields.size());
    out->inner = nullptr;
    return KILN_OK;
}
KilnResult KILN_CALL type_field(const KilnType* type, uint32_t i, KilnTypeField* out) {
    const auto* t = reinterpret_cast<const FakeType*>(type);
    if (i >= t->fields.size()) return KILN_ERR_NOT_FOUND;
    out->name = KilnStr{t->fields[i].name, std::strlen(t->fields[i].name)};
    out->type = reinterpret_cast<const KilnType*>(t->fields[i].type);
    out->offset = t->fields[i].offset;
    return KILN_OK;
}

// ---- GetComponent of the client world: the local player stands at (0, 1, 0) ----
struct Slice {
    const void* data;
    uint64_t size;
};
uint8_t render_transform[176];
uint64_t component_size = 176;
const uintptr_t kWorld = 0x7ff612340000ull;
void fake_get_component(Slice* out, uintptr_t world, uint32_t hash, uint32_t entity) {
    CHECK(world == kWorld && hash == 0x3b8476dd);
    *out = entity == 1 ? Slice{render_transform, component_size} : Slice{nullptr, 0};
}

// ---- the core ----
const KilnCameraApi* camera = nullptr;
std::vector<std::pair<KilnWorldEventFn, void*>> world_subs;
KilnTickFn client_pre = nullptr, server_pre = nullptr;
void* client_pre_user = nullptr;
bool live = false;

const void* KILN_CALL service(KilnStr name, uint32_t) {
    const std::string n = str(name);
    if (n == KILN_HOOKS_SERVICE) return &hooks_table;
    if (n == KILN_CAPS_SERVICE) return &caps_table;
    if (n == KILN_CLIENT_SERVICE) return &client_table;
    if (n == KILN_TYPES_SERVICE) return &types_table;
    return nullptr;  // no kiln.world, no kiln.players: no server Mode here
}
KilnResult KILN_CALL publish(KilnOwner, KilnStr name, const void* t) {
    CHECK(str(name) == "kiln.camera");
    camera = static_cast<const KilnCameraApi*>(t);
    return KILN_OK;
}
bool KILN_CALL feature(KilnStr name) { return !(scenario == "pose-row-off" && str(name) == "camera.local_pose"); }
void KILN_CALL log(KilnOwner, int32_t, KilnStr) {}
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }
bool KILN_CALL on_thread(uint32_t kind) { return kind == KILN_WORLD_CLIENT || kind == KILN_WORLD_SERVER; }
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_CLIENT && live ? kWorld : 0; }
KilnResult KILN_CALL sub_world(KilnOwner, KilnWorldEventFn fn, void* user, KilnHandle* h) {
    world_subs.push_back({fn, user});
    *h = world_subs.size();
    return KILN_OK;
}
KilnResult KILN_CALL sub_tick(KilnOwner, uint32_t kind, int32_t phase, KilnTickFn fn, void* user, KilnHandle* h) {
    CHECK(phase == KILN_TICK_PRE);
    if (kind == KILN_WORLD_CLIENT) client_pre = fn, client_pre_user = user;
    else server_pre = fn;
    *h = 5;
    return KILN_OK;
}
KilnResult KILN_CALL symbol(KilnStr name, uintptr_t* out) {
    const std::string n = str(name);
    if (n == "camera/RenderFovSelected") return *out = kSelectAt, KILN_OK;
    if (n == "camera/VisualFrameReady" && scenario != "no-capture") return *out = kCaptureAt, KILN_OK;
    if (n == "GetComponent") return *out = reinterpret_cast<uintptr_t>(&fake_get_component), KILN_OK;
    return KILN_ERR_NOT_FOUND;
}
KilnResult KILN_CALL constant(KilnStr name, int64_t* out) {
    if (scenario == "no-constants") return KILN_ERR_NOT_FOUND;
    const auto it = kConstants.find(str(name));
    if (it == kConstants.end()) return KILN_ERR_NOT_FOUND;
    *out = it->second;
    return KILN_OK;
}

void world_event(uint32_t what, uint32_t epoch) {
    live = what == KILN_WORLD_STARTED;
    KilnWorldEvent e{};
    e.size = sizeof e;
    e.event = what;
    e.kind = KILN_WORLD_CLIENT;
    e.session = epoch;
    e.world = kWorld;
    for (auto& [fn, user] : world_subs) fn(&e, user);
}
uint64_t step = 0;
void pre() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.world_kind = KILN_WORLD_CLIENT;
    t.step = ++step;
    client_pre(&t, client_pre_user);
}

// ---- a renderer the hooks read ----
std::vector<uint8_t> renderer(0x469400);
template <class T> void put(uint64_t off, T v) { std::memcpy(&renderer[off], &v, sizeof v); }
void stock_frame() {
    put<uint64_t>(464, kWorld);
    int64_t pos[3] = {0, static_cast<int64_t>(1.7 * 4294967296.0), 0};
    std::memcpy(&renderer[4616872], pos, sizeof pos);
    const float rot[4] = {0, 0, 0, 1}, scale[3] = {1, 1, 1};
    std::memcpy(&renderer[4616872 + 24], rot, sizeof rot);
    std::memcpy(&renderer[4616872 + 40], scale, sizeof scale);
    const float lens[4] = {16.f / 9.f, 0.1f, 1000.f, kNaturalFov};
    std::memcpy(&renderer[4625336], lens, sizeof lens);
}
// One frame through the two hooks; returns the select hook's xmm6 low float.
float frame() {
    stock_frame();
    KilnRegisters regs;
    std::memset(&regs, 0xab, sizeof regs);
    regs.r13 = reinterpret_cast<uintptr_t>(renderer.data());
    std::memcpy(regs.xmm6.bytes, &kNaturalFov, 4);
    KilnRegisters before = regs;
    const Hook* sel = hook_at(kSelectAt);
    CHECK(sel);
    sel->fn(&regs, sel->user);
    // only xmm6's low float may differ
    CHECK(std::memcmp(regs.xmm6.bytes + 4, before.xmm6.bytes + 4, 12) == 0);
    std::memcpy(before.xmm6.bytes, regs.xmm6.bytes, 4);
    CHECK(std::memcmp(&regs, &before, sizeof regs) == 0);
    float fov = 0;
    std::memcpy(&fov, regs.xmm6.bytes, 4);
    put<float>(4616936, fov);  // the original store
    put<float>(4625348, fov);  // frame preparation's copy
    if (const Hook* cap = hook_at(kCaptureAt)) {
        KilnRegisters c;
        std::memset(&c, 0, sizeof c);
        c.rsi = reinterpret_cast<uintptr_t>(renderer.data());
        cap->fn(&c, cap->user);
    }
    return fov;
}

struct Done {
    bool seen = false;
    KilnCameraResult r{};
};
void KILN_CALL on_done(const KilnCameraResult* r, void* user) {
    auto* d = static_cast<Done*>(user);
    d->seen = true;
    d->r = *r;
}

KilnCameraRequest lens(float degrees) {
    KilnCameraRequest q{};
    q.size = sizeof q;
    q.endpoint = KILN_CAMERA_LOCAL_VISUAL;
    q.parts = KILN_CAMERA_LENS;
    q.lens_source = KILN_CAMERA_LENS_CONSTANT;
    q.fov_y_degrees = degrees;
    q.duration_ms = 10000;
    q.profile.rotation[3] = 1;
    return q;
}

uint64_t local_bits() {
    uint64_t m = 0;
    CHECK(camera->capabilities(KILN_CAMERA_LOCAL_VISUAL, 0, &m) == KILN_OK);
    return m;
}

constexpr uint64_t kWrites = KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_LOCAL_POSE | KILN_CAMERA_CAP_LOCAL_PROFILE | KILN_CAMERA_CAP_LOCAL_SHAKE;

}  // namespace

int main(int argc, char** argv) {
    scenario = argc > 1 ? argv[1] : "ok";
    hooks_table.header = {scenario == "no-mid2" ? static_cast<uint32_t>(offsetof(KilnHooksApi, mid2)) : static_cast<uint32_t>(sizeof hooks_table), KILN_HOOKS_VERSION};
    hooks_table.mid2 = &mid2;
    hooks_table.subscribe_owner_retiring = &subscribe_owner;
    caps_table.header = {sizeof caps_table, KILN_CAPS_VERSION};
    caps_table.report = &report;
    client_table.header = {sizeof client_table, KILN_CLIENT_VERSION};
    client_table.state = &client_state;
    types_table.header = {sizeof types_table, KILN_TYPES_VERSION};
    types_table.type_by_hash = &type_by_hash;
    types_table.type_info = &type_info;
    types_table.type_field = &type_field;
    if (scenario == "bad-reflection") t_rt.size = 160;
    int64_t pos[3] = {0, static_cast<int64_t>(4294967296.0), 0};
    std::memcpy(render_transform, pos, sizeof pos);
    const float rot[4] = {0, 0, 0, 1}, scale[3] = {1, 1, 1};
    std::memcpy(render_transform + 24, rot, sizeof rot);
    std::memcpy(render_transform + 40, scale, sizeof scale);

    KilnApi api{};
    api.size = sizeof api;
    api.abi_version = KILN_ABI_VERSION;
    api.service = &service;
    api.register_service = &publish;
    api.feature = &feature;
    api.log = &log;
    api.side = &side;
    api.on_world_thread = &on_thread;
    api.world = &world;
    api.subscribe_world = &sub_world;
    api.subscribe_world_tick = &sub_tick;
    api.symbol = &symbol;
    api.constant = &constant;
    KilnPluginInfo info{};
    CHECK(kiln_plugin_main(&api, kService, &info) == KILN_OK);
    CHECK(camera && client_pre && info.on_ready);
    info.on_ready(nullptr);
    available("camera.service");

    KilnCameraVisual v{};
    uint32_t why = 0;
    if (scenario == "no-constants" || scenario == "no-mid2") {
        CHECK(hooks.empty());
        CHECK(local_bits() == 0);
        const uint32_t reason = scenario == "no-mid2" ? KILN_CAP_REASON_SERVICE_OLDER : KILN_CAP_REASON_SYMBOL_MISSING;
        for (const char* r : {"camera.local_fov", "camera.local_pose", "camera.local_profile", "camera.local_shake", "camera.rendered_snapshot"}) unavailable(r, reason);
        v.size = sizeof v;
        CHECK(camera->get_visual(&v, &why) == KILN_ERR_UNAVAILABLE);
        std::printf("camera client %s: PASS (%d checks)\n", scenario.c_str(), checks);
        return 0;
    }
    if (scenario == "no-capture") {
        CHECK(hooks.size() == 1 && hooks[0].at == kSelectAt);
        CHECK(local_bits() == 0);
        for (const char* r : {"camera.local_fov", "camera.local_pose", "camera.local_profile", "camera.local_shake", "camera.rendered_snapshot"})
            unavailable(r, KILN_CAP_REASON_SYMBOL_MISSING, "camera/VisualFrameReady");
        std::printf("camera client %s: PASS (%d checks)\n", scenario.c_str(), checks);
        return 0;
    }
    // both hooks, after the other links on their place
    CHECK(hooks.size() == 2 && hooks[0].at == kSelectAt && hooks[1].at == kCaptureAt);
    CHECK(hooks[0].priority == -100 && hooks[1].priority == -100 && hooks[0].label == "camera/RenderFovSelected" && hooks[1].label == "camera/VisualFrameReady");
    available("camera.local_fov");
    available("camera.rendered_snapshot");
    if (scenario == "bad-reflection" || scenario == "pose-row-off") {
        if (scenario == "bad-reflection") {
            CHECK(local_bits() == (KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_RENDERED_SNAPSHOT));
            for (const char* r : {"camera.local_pose", "camera.local_profile", "camera.local_shake"}) unavailable(r, KILN_CAP_REASON_SYMBOL_MISSING, "RenderTransform");
        } else {
            CHECK(local_bits() == (kWrites & ~KILN_CAMERA_CAP_LOCAL_POSE) + KILN_CAMERA_CAP_RENDERED_SNAPSHOT);
            unavailable("camera.local_pose", KILN_CAP_REASON_SYMBOL_MISSING, "camera.local_pose");
            available("camera.local_profile");
        }
        std::printf("camera client %s: PASS (%d checks)\n", scenario.c_str(), checks);
        return 0;
    }
    for (const char* r : {"camera.local_pose", "camera.local_profile", "camera.local_shake"}) available(r);
    CHECK(local_bits() == (kWrites | KILN_CAMERA_CAP_RENDERED_SNAPSHOT));

    world_event(KILN_WORLD_STARTED, 1);
    if (scenario == "sampler-fault") {
        component_size = 160;  // the component is not the checked size any more
        pre();
        CHECK(local_bits() == (KILN_CAMERA_CAP_LOCAL_FOV | KILN_CAMERA_CAP_RENDERED_SNAPSHOT));
        available("camera.local_fov");
        for (const char* r : {"camera.local_pose", "camera.local_profile", "camera.local_shake"}) unavailable(r, KILN_CAP_REASON_FAULTED, "next world");
        // the next world starts healthy
        component_size = 176;
        world_event(KILN_WORLD_STOPPING, 1);
        world_event(KILN_WORLD_STARTED, 2);
        pre();
        CHECK(local_bits() == (kWrites | KILN_CAMERA_CAP_RENDERED_SNAPSHOT));
        available("camera.local_pose");
        std::printf("camera client %s: PASS (%d checks)\n", scenario.c_str(), checks);
        return 0;
    }

    // get_visual: arguments, then no frame yet
    pre();
    v.size = sizeof v - 1;
    CHECK(camera->get_visual(&v, &why) == KILN_ERR_INVALID_ARGUMENT);
    v.size = sizeof v;
    CHECK(camera->get_visual(&v, nullptr) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(camera->get_visual(nullptr, &why) == KILN_ERR_INVALID_ARGUMENT);
    CHECK(camera->get_visual(&v, &why) == KILN_OK && v.valid == 0 && why == KILN_CAMERA_REASON_NO_CAMERA_FRAME);

    // a lens through the real hooks: Applied after the frame that showed it
    Done d;
    KilnCameraRequest q = lens(40);
    KilnCameraLease lease = 0;
    CHECK(camera->apply(kAuthor, &q, &lease, &on_done, &d) == KILN_OK && lease);
    pre();
    CHECK(!d.seen);
    const float fov = frame();
    CHECK(std::fabs(fov - 40.0 * 3.14159265358979323846 / 180.0) < 1e-6);
    pre();
    CHECK(d.seen && d.r.ok && d.r.commit_scope == KILN_CAMERA_COMMIT_CLIENT_FRAME && d.r.frame == 1 && d.r.player_entity == 1);
    CHECK(camera->get_visual(&v, &why) == KILN_OK && v.valid == 1 && std::fabs(v.fov_y_radians - fov) < 1e-7 && v.local_generation == 1);
    // the world stops: nothing of it is drawn any more
    world_event(KILN_WORLD_STOPPING, 1);
    CHECK(frame() == kNaturalFov);
    CHECK(camera->get_visual(&v, &why) == KILN_OK && v.valid == 0);
    std::printf("camera client %s: PASS (%d checks)\n", scenario.c_str(), checks);
    return 0;
}
