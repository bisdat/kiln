// SPDX-License-Identifier: MIT
// Stage-only qualification of the real path.cpp glue, not of an engine build or EMF parity. All memory belongs to
// this fixture and all game-call leaves are fakes. Run each scenario in a fresh process: faults latch for the process.
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "../path.cpp"

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace {
using namespace kq;
struct RayGrid { float origin[3] = {-1000, -1000, -1000}; uint32_t shift = 6, horizontal = 64, vertical = 64; } ray;
uintptr_t realms[] = {reinterpret_cast<uintptr_t>(&ray)};
struct VoxelGrid { uintptr_t realms; uint64_t count; } grid{reinterpret_cast<uintptr_t>(realms), 1};
std::array<unsigned char, 0xbc> job{};
uintptr_t context = reinterpret_cast<uintptr_t>(job.data());
uint64_t iter[] = {11, 12, reinterpret_cast<uintptr_t>(&grid)};
int command_buffer = 0;
float transform[12]{};
uint64_t stamp = 77;
Clock clock_value{1, 10, true};
KilnApi api{};
KilnWorldApi world{};
KilnCapsApi caps{};
KilnProgramEachFn program_each = nullptr;
KilnTickFn pre = nullptr;
bool tick = true, inside = false, exists = true, stamped = true, revoked = false, feature = true, layout = true;
bool readable = false, destroy_queued = true, retire_in_create = false;
uint32_t program_state = KILN_PROGRAM_ACTIVE, create_id = 100, result = 1;
uint64_t result_count = 2;
std::vector<RawPoint> raw{{0, 0, 0, 0, {}}, {3, 0, 0, 0, {}}};
std::string fault;
int defaults_calls = 0, create_calls = 0, result_calls = 0, destroy_calls = 0, reports = 0, grid_reads = 0;
constexpr KilnOwner owner = 5;

bool readable_range(uintptr_t p, size_t n, const void* base, size_t size) {
    const uintptr_t b = reinterpret_cast<uintptr_t>(base);
    return p >= b && p - b <= size && n <= size - (p - b);
}
template <typename T> T get(const void* p, size_t offset) { T value; std::memcpy(&value, static_cast<const unsigned char*>(p) + offset, sizeof value); return value; }
int calls() { return defaults_calls + create_calls + result_calls + destroy_calls; }
KilnQueryPathRequest request() {
    KilnQueryPathRequest r{};
    r.size = sizeof r; r.anchor = 2000; r.to = {3, 0, 0};
    r.horizontal_space = 0.5; r.vertical_space = 2; r.max_iterations = 2000;
    return r;
}
KilnResult submit(KilnQueryPathAdmission* a, const KilnQueryPathRequest& r = request()) {
    *a = {}; a->size = sizeof *a;
    return t_path_submit(owner, &r, a);
}
uint64_t admit() {
    KilnQueryPathAdmission a{};
    CHECK(submit(&a) == KILN_OK && a.ok && a.request && a.status == KQ_PENDING);
    return a.request;
}
void step() {
    CHECK(pre && program_each);
    KilnTickInfo info{}; info.step = ++clock_value.step;
    pre(&info, nullptr);
    program_each(reinterpret_cast<KilnProgramCtx*>(&context), iter, 0, nullptr);
}
KilnQueryPathUpdate poll(uint32_t status, bool terminal) {
    KilnQueryPathUpdate u{}; u.size = sizeof u;
    KilnQueryPathPoint points[4]{}; uint32_t needed = 999;
    CHECK(t_path_poll(owner, &u, points, 4, &needed) == KILN_OK);
    CHECK(u.status == status && u.terminal == static_cast<uint32_t>(terminal));
    CHECK(terminal ? (u.ok == (status == KQ_FOUND || status == KQ_PARTIAL)) : !u.ok);
    return u;
}
void empty() {
    KilnQueryPathUpdate u{}; u.size = sizeof u; uint32_t needed = 999;
    CHECK(t_path_poll(owner, &u, nullptr, 0, &needed) == KILN_ERR_NOT_FOUND && needed == 0);
}
void setup() {
    api.size = sizeof api; api.abi_version = KILN_ABI_VERSION;
    api.on_tick_thread = [] { return tick; };
    api.post = [](KilnOwner, KilnTaskFn, void*) -> KilnResult { return revoked ? KILN_ERR_REVOKED : KILN_OK; };
    api.server_world = []() -> uintptr_t { return clock_value.world ? reinterpret_cast<uintptr_t>(&grid) : 0; };
    api.singleton = [](uintptr_t, KilnStr) -> uintptr_t { ++grid_reads; return reinterpret_cast<uintptr_t>(&grid); };
    api.feature = [](KilnStr) { return feature; };
    api.symbol = [](KilnStr name, uintptr_t* out) -> KilnResult {
        // Inert identities, not callable addresses. Only our fake leaves below receive them.
        const std::string s(name.ptr, name.len);
        *out = s == "queries/PathQueryDefaults" ? 1 : s == "queries/CreatePathQuery" ? 2 : s == "queries/GetPathQueryResult" ? 3 : 4;
        return KILN_OK;
    };
    api.constant = [](KilnStr name, int64_t* out) -> KilnResult {
        const char* names[] = {"queries/query.start", "queries/query.target", "queries/query.max_iterations", "queries/query.moves",
            "queries/job.realm", "queries/query.target_type", "queries/query.start_search_v", "queries/query.target_search_v",
            "queries/query.space_h", "queries/query.space_v"};
        const int64_t values[] = {0x10, 0x1c, 0x48, 0x54, 0xbb, 0x36, 0xf4, 0xfc, 0x100, 0x104};
        for (size_t i = 0; i < 10; ++i) if (std::string(name.ptr, name.len) == names[i]) {
            *out = values[i] + (!layout && i == 6 ? 4 : 0); return KILN_OK;
        }
        return KILN_ERR_NOT_FOUND;
    };
    api.subscribe_tick = [](KilnOwner, int32_t phase, KilnTickFn fn, void*, KilnHandle* h) -> KilnResult {
        CHECK(phase == KILN_TICK_PRE); pre = fn; *h = 1; return KILN_OK;
    };
    world.header = {sizeof world, KILN_WORLD_VERSION};
    world.in_program = []() -> uint32_t { return inside; };
    world.exists = [](KilnEntity e) { return exists && e == 2000; };
    world.component = [](KilnEntity, uint32_t hash, uint32_t* n) -> const void* {
        if (!exists) return nullptr;
        if (hash == 11) return *n = sizeof transform, transform;
        if (hash == 12 && stamped) return *n = sizeof stamp, &stamp;
        return nullptr;
    };
    world.read_other = [](KilnProgramCtx*, uint64_t slot, KilnEntity e, uint32_t* n) -> const void* {
        return e == 2000 ? world.component(e, static_cast<uint32_t>(slot), n) : nullptr;
    };
    world.command_buffer = [](KilnProgramCtx*) -> void* { return &command_buffer; };
    world.component_hash = [](KilnStr s) -> uint32_t { return std::string(s.ptr, s.len) == "CurrentTransform" ? 11 : 12; };
    world.register_program = [](KilnOwner, const KilnProgramDesc* d, KilnHandle* h) -> KilnResult {
        CHECK(d->flags == (KILN_PROGRAM_SYSTEM | KILN_PROGRAM_HOST_ONLY | KILN_PROGRAM_BARRIER));
        CHECK(d->singletons_read.count == 3 && d->other_read.count == 2);
        CHECK(std::string(d->name.ptr, d->name.len) == "kiln_queries_path");
        program_each = d->each; *h = 1; return KILN_OK;
    };
    world.program_slot = [](KilnHandle, int32_t kind, KilnStr name) -> int32_t {
        if (kind == KILN_SLOT_SINGLETON) return 2;
        return std::string(name.ptr, name.len) == "CurrentTransform" ? 0 : 1;
    };
    world.program_info = [](KilnHandle, KilnProgramInfo* info) -> KilnResult { info->state = program_state; return KILN_OK; };
    caps.report = [](KilnOwner, KilnStr, uint32_t state, uint32_t, KilnStr) -> KilnResult {
        CHECK(state == KILN_CAP_UNAVAILABLE); ++reports; return KILN_OK;
    };
    g_api = &api; g_world = &world; g_caps = &caps; g_layout = {0, 8, 8, 0, 12, 16, 20}; g_grid_ok = true;
    path_resolve(); path_ready();
}
}  // namespace

// The only engine boundary used by path.cpp is replaced here. No fn identity is cast or called.
namespace kq {
const KilnApi* g_api = nullptr;
KilnOwner g_owner = 1;
const KilnWorldApi* g_world = nullptr;
const KilnCapsApi* g_caps = nullptr;
GridLayout g_layout{};
std::atomic<bool> g_grid_ok{false};
void logf(int32_t, const char*, ...) {}
Clock now() { return clock_value; }
bool world_ready(const Clock& c) { return c.world && c.session != 0; }
bool seh_copy(uintptr_t p, void* out, size_t n) {
    if (fault == "copy" && p == reinterpret_cast<uintptr_t>(raw.data())) return false;
    if (!(readable_range(p, n, &context, sizeof context) || readable_range(p, n, job.data(), job.size()) ||
          readable_range(p, n, &grid, sizeof grid) || readable_range(p, n, realms, sizeof realms) ||
          readable_range(p, n, &ray, sizeof ray) || readable_range(p, n, raw.data(), raw.size() * sizeof(RawPoint)))) return false;
    std::memcpy(out, reinterpret_cast<const void*>(p), n); return true;
}
bool seh_path_defaults(uintptr_t fn, void* q) {
    CHECK(fn == 1); ++defaults_calls;
    CHECK(get<uint32_t>(q, 0) == 0); static_cast<unsigned char*>(q)[0x80] = 0x5a;
    return fault != "defaults";
}
bool seh_path_create(uintptr_t fn, void* cb, uint32_t* id, const void* q) {
    CHECK(fn == 2 && cb == &command_buffer); ++create_calls;
    CHECK(get<uint32_t>(q, 0) == 2000 && get<float>(q, 0x1c) == 3);
    CHECK(get<uint32_t>(q, 0x48) == 2000 && get<uint16_t>(q, 0x54) == 1);
    CHECK(get<uint32_t>(q, 0x100) == 1 && get<uint32_t>(q, 0x104) == 4 && get<uint8_t>(q, 0x80) == 0x5a);
    if (retire_in_create) t_path_retire(owner);
    *id = create_id; return fault != "create";
}
bool seh_path_result(uintptr_t fn, void*, void* out, uint32_t id, bool* ready) {
    CHECK(fn == 3 && id == create_id); ++result_calls;
    *ready = readable;
    if (readable) {
        const uintptr_t points = reinterpret_cast<uintptr_t>(raw.data());
        std::memcpy(out, &points, 8); std::memcpy(static_cast<unsigned char*>(out) + 8, &result_count, 8);
        static_cast<unsigned char*>(out)[0x24] = static_cast<uint8_t>(result);
    }
    return fault != "result";
}
bool seh_path_destroy(uintptr_t fn, void* cb, uint32_t anchor, uint32_t id, bool* queued) {
    CHECK(fn == 4 && cb == &command_buffer && anchor == 2000 && id == create_id); ++destroy_calls;
    *queued = destroy_queued; return fault != "destroy";
}
}  // namespace kq

int main(int argc, char** argv) {
    using namespace kq;
    CHECK(argc == 2); const std::string scenario = argv[1];
    if (scenario == "layout") layout = false;
    if (scenario == "feature") feature = false;
    setup();
    if (scenario == "layout" || scenario == "feature") {
        CHECK(!program_each && !g_path_on); KilnQueryPathAdmission a{};
        CHECK(submit(&a) == KILN_ERR_UNAVAILABLE && !a.ok && !a.request && !calls());
    } else if (scenario == "gates") {
        KilnQueryPathAdmission a{};
        tick = false; CHECK(submit(&a) == KILN_ERR_WRONG_THREAD && !a.ok && !a.request);
        tick = true; inside = true; CHECK(submit(&a) == KILN_ERR_WRONG_THREAD && !a.ok && !a.request);
        CHECK(std::string(a.reason.ptr, a.reason.len) == "InsideProgram"); inside = false;
        clock_value.world = false; CHECK(submit(&a) == KILN_ERR_UNAVAILABLE && !a.ok); clock_value.world = true;
        revoked = true; CHECK(submit(&a) == KILN_ERR_REVOKED && !a.ok); revoked = false;
        CHECK(grid_reads == 0 && !calls() && !g_queue.open());
        exists = false; CHECK(submit(&a) == KILN_OK && !a.ok && a.status == KQ_OWNER_GONE); exists = true;
        stamped = false; CHECK(submit(&a) == KILN_OK && !a.ok && a.status == KQ_IDENTITY_UNAVAILABLE); stamped = true;
        auto r = request(); r.to.x = std::numeric_limits<double>::quiet_NaN();
        CHECK(submit(&a, r) == KILN_ERR_INVALID_ARGUMENT && !a.ok);
        r = request(); r.from.x = r.to.x = 4000;
        CHECK(submit(&a, r) == KILN_OK && !a.ok && a.status == KQ_OUT_OF_GRID && !calls() && !g_queue.open());
    } else {
        const uint64_t token = admit(); CHECK(!calls());
        poll(KQ_PENDING, false);
        if (scenario == "cancel-before") {
            CHECK(t_path_cancel(owner + 1, token) == KILN_ERR_NOT_FOUND);
            CHECK(t_path_cancel(owner, token) == KILN_OK); step();
            poll(KQ_CANCELLED, true); CHECK(!calls() && !g_queue.open()); empty();
        } else if (scenario == "realm" || scenario == "grid" || scenario == "program") {
            if (scenario == "realm") job[0xbb] = 1;
            if (scenario == "grid") ray.origin[0] = 100;
            if (scenario == "program") { program_state = KILN_PROGRAM_FAULTED; clock_value.step = 63; }
            step(); poll(scenario == "grid" ? KQ_OUT_OF_GRID : KQ_REFUSED, true);
            CHECK(!calls());
        } else {
            if (scenario == "retire") retire_in_create = true;
            if (scenario == "fault-defaults") fault = "defaults";
            if (scenario == "fault-create") fault = "create";
            step();
            if (scenario == "fault-defaults" || scenario == "fault-create") {
                poll(KQ_REFUSED, true); CHECK(!g_path_on && reports == 2);
                const int before = calls(); step(); CHECK(calls() == before);
            } else if (scenario == "retire") {
                CHECK(t_path_cancel(owner, token) == KILN_ERR_REVOKED);
                step(); CHECK(destroy_calls == 1 && !result_calls && !g_queue.open());
            } else {
                poll(KQ_PENDING, false);
                CHECK(defaults_calls == 1 && create_calls == 1);
                if (scenario == "cancel" || scenario == "identity" || scenario == "world") {
                    if (scenario == "cancel") CHECK(t_path_cancel(owner, token) == KILN_OK);
                    if (scenario == "identity") ++stamp;
                    if (scenario == "world") { path_world_closed(1); clock_value.session = 2; }
                    step();
                    poll(scenario == "cancel" ? KQ_CANCELLED : scenario == "identity" ? KQ_OWNER_GONE : KQ_WORLD_CLOSED, true);
                    step(); CHECK(!result_calls && destroy_calls == (scenario == "world" ? 0 : 1) && !g_queue.open());
                } else {
                    step(); CHECK(result_calls == 1 && !destroy_calls); empty();  // false Get is pending, never success
                    readable = true;
                    uint32_t expected = KQ_FOUND;
                    if (scenario == "partial") { raw.back().x = 0; expected = KQ_PARTIAL; }
                    else if (scenario == "unsupported") { raw.back().move = 5; expected = KQ_UNSUPPORTED; }
                    else if (scenario == "empty") { result_count = 0; expected = KQ_MALFORMED; }
                    else if (scenario == "truncated") { result_count = KILN_QUERY_PATH_MAX_POINTS + 1; expected = KQ_TRUNCATED; }
                    else if (scenario == "copy") { fault = "copy"; expected = KQ_MALFORMED; }
                    else if (scenario == "nonfinite") { raw.back().x = std::numeric_limits<float>::infinity(); expected = KQ_MALFORMED; }
                    else if (scenario == "no-path") { result = 2; expected = KQ_NO_PATH; }
                    else if (scenario == "fault-result" || scenario == "fault-destroy") { fault = scenario.substr(6); expected = scenario == "fault-result" ? KQ_REFUSED : KQ_FOUND; }
                    else CHECK(scenario == "found" || scenario == "world-result");
                    destroy_queued = false; step();
                    if (scenario == "world-result") { path_world_closed(1); clock_value.session = 2; expected = KQ_WORLD_CLOSED; }
                    if (expected == KQ_FOUND && scenario == "found") {
                        KilnQueryPathUpdate u{}; u.size = sizeof u; uint32_t needed = 0;
                        CHECK(t_path_poll(owner, &u, nullptr, 0, &needed) == KILN_ERR_BUFFER_TOO_SMALL && needed == 2);
                        raw.back().x = 999;  // engine memory changed after the answer: caller must receive the copy
                        KilnQueryPathPoint points[2]{};
                        CHECK(t_path_poll(owner, &u, points, 2, &needed) == KILN_OK);
                        CHECK(u.status == KQ_FOUND && u.ok && u.reaches_target && u.length == 3 && points[1].position.x == 3);
                    } else {
                        const auto u = poll(expected, true);
                        CHECK(u.reaches_target == (expected == KQ_FOUND));
                    }
                    if (scenario == "fault-result" || scenario == "fault-destroy") {
                        CHECK(!g_path_on && reports == 2);
                        const int before = calls(); step(); CHECK(calls() == before);
                    } else {
                        CHECK(g_queue.open() == (scenario == "world-result" ? 0 : 1));
                        destroy_queued = true; step(); CHECK(!g_queue.open());
                    }
                    empty();
                }
            }
        }
    }
    std::printf("queries path lifecycle %s: PASS (%d checks)\n", scenario.c_str(), checks);
}
