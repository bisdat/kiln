// SPDX-License-Identifier: MIT
// kiln.skills.client (client_view.cpp, 0.2.0-dev.31.13) against a fake client world, no game: what the state says while
// the skill tree does not resolve as a singleton, and a new client world without a stop of the old one (review 31.13).
// Built by native/build.sh skills-client-view-native-test; runs under Wine: native/services/skills/tests/run.sh
#include "../client_view.cpp"

#include <string>

namespace kiln::skills::shadow {
bool is_shadow(uint32_t) { return false; }
}  // namespace kiln::skills::shadow

namespace {
int checks = 0, failures = 0;
void check(bool b, const char* s) {
    ++checks;
    std::printf("%s %s\n", b ? "PASS" : "FAIL", s);
    failures += !b;
}
std::string S(KilnStr s) { return std::string(s.ptr, s.len); }

constexpr int64_t kRecords = 0x100, kTotal = 0x10, kSpent = 0x14, kAvail = 0x18;
uintptr_t client_world = 0x5000;
bool tree_here = true;
alignas(16) uint8_t play[0x200] = {};
alignas(16) uint8_t tree[0x200] = {};
alignas(16) uint8_t data[16 + 0x8c] = {};
uint32_t ids[1] = {77};
KilnTickFn post_fn = nullptr;
KilnWorldEventFn world_fn = nullptr;

bool KILN_CALL feature(KilnStr) { return true; }
uint32_t KILN_CALL side() { return KILN_SIDE_CLIENT; }
uintptr_t KILN_CALL world(uint32_t kind) { return kind == KILN_WORLD_CLIENT ? client_world : 0; }
void KILN_CALL log(KilnOwner, int32_t, KilnStr) {}
KilnResult KILN_CALL client_state(KilnClientState* s) {
    s->local_player = 9;
    return KILN_OK;
}
const KilnClientApi client_api = [] {
    KilnClientApi c{};
    c.header.size = sizeof c;
    c.header.version = KILN_CLIENT_VERSION;
    c.state = &client_state;
    return c;
}();
const void* KILN_CALL service(KilnStr n, uint32_t) { return S(n) == "kiln.client" ? &client_api : nullptr; }
KilnResult KILN_CALL constant(KilnStr n, int64_t* out) {
    const std::string s = S(n);
    const std::pair<const char*, int64_t> rows[] = {
        {"skills/ui.records", kRecords}, {"skills/ui.record_size", 0x44}, {"skills/ui.points_total", kTotal},
        {"skills/ui.points_spent", kSpent}, {"skills/ui.points_available", kAvail}, {"skills/ui.rec_level", 0x41},
        {"skills/ui.rec_unlocked", 0x43}, {"skills/tree.data", 0x20}, {"skills/tree.ids", 0x150},
        {"skills/ui.unlock_levels", 0x30}, {"skills/ui.map_cap", 0x10}, {"skills/ui.map_values", 0x40},
        {"skills/node.level_index", 0x89}, {"skills/ui.records_cap_at", kRecords + 0x10},
        {"skills/ui.records_bits_at", kRecords + 0x08}, {"skills/ui.records_keys_at", kRecords + 0x28},
        {"skills/ui.records_values_at", kRecords + 0x40}};
    for (const auto& r : rows)
        if (s == r.first) return *out = r.second, KILN_OK;
    return KILN_ERR_UNAVAILABLE;
}
uintptr_t KILN_CALL singleton(uintptr_t w, KilnStr n) {
    if (!w || w != client_world) return 0;
    const std::string s = S(n);
    if (s == "G38UiPlayData") return reinterpret_cast<uintptr_t>(play);
    if (s == "g38_skilltree::SkillTree") return tree_here ? reinterpret_cast<uintptr_t>(tree) : 0;
    return 0;
}
KilnResult KILN_CALL sub_tick(KilnOwner, uint32_t, int32_t, KilnTickFn fn, void*, KilnHandle* h) {
    post_fn = fn;
    *h = 1;
    return KILN_OK;
}
KilnResult KILN_CALL sub_world(KilnOwner, KilnWorldEventFn fn, void*, KilnHandle* h) {
    world_fn = fn;
    *h = 2;
    return KILN_OK;
}
KilnResult KILN_CALL reg(KilnOwner, KilnStr, const void*) { return KILN_OK; }

KilnApi make_api() {
    KilnApi a{};
    a.size = sizeof a;
    a.log = &log;
    a.feature = &feature;
    a.side = &side;
    a.world = &world;
    a.service = &service;
    a.constant = &constant;
    a.singleton = &singleton;
    a.subscribe_world_tick = &sub_tick;
    a.subscribe_world = &sub_world;
    a.register_service = &reg;
    return a;
}
KilnApi g_fake = make_api();

template <class T>
void put(uint8_t* p, size_t off, T v) {
    std::memcpy(p + off, &v, sizeof v);
}

void fill_world() {
    put<int32_t>(play, kTotal, 5);
    put<int32_t>(play, kSpent, 2);
    put<int32_t>(play, kAvail, 3);
    // the resource: header {nodes rel32 16, 1 node, links rel32, 0 links}, then one node of id 77
    put<int32_t>(data, 0, 16);
    put<uint32_t>(data, 4, 1);
    put<int32_t>(data, 8, 8);
    put<uint32_t>(data, 12, 0);
    put<uint32_t>(data, 16, 77);
    put<uint64_t>(tree, 0x20, reinterpret_cast<uint64_t>(data));
    put<uint64_t>(tree, 0x150, reinterpret_cast<uint64_t>(ids));
    put<uint64_t>(tree, 0x158, 1);
}

uint64_t g_step = 0;
void post() {
    KilnTickInfo t{};
    t.size = sizeof t;
    t.step = ++g_step;
    post_fn(&t, nullptr);
}
KilnResult state(KilnSkillsClientState* s) {
    *s = KilnSkillsClientState{};
    s->size = sizeof *s;
    return kiln::skills::client_view::kTable.state(s);
}
uint32_t node_count() {
    KilnSkillsClientNode n[4] = {};
    uint32_t c = 0;
    kiln::skills::client_view::kTable.nodes(n, 4, &c);
    return c;
}
}  // namespace

int main() {
    using namespace kiln::skills::client_view;
    fill_world();
    init(&g_fake, 3);
    ready();
    check(g_on.load() && post_fn && world_fn, "the view resolves and subscribes on the fake client");

    // 1: the skill tree does not resolve as a singleton: the state says why no copy is taken (the row stays up)
    tree_here = false;
    KilnSkillsClientState s;
    state(&s);  // the demand
    for (int i = 0; i < 5; ++i) post();
    {
        const KilnResult r = state(&s);
        check(r == KILN_ERR_NOT_FOUND && s.on == 1 && s.copying == 1, "no tree: on, copying, no copy");
        check(s.detail[0] != 0, "no tree: state.detail says why nothing is copied");
        std::printf("   detail: \"%s\"\n", s.detail);
    }
    tree_here = true;
    post();
    {
        const KilnResult r = state(&s);
        check(r == KILN_OK && s.nodes == 1 && s.total == 5 && s.detail[0] == 0, "the tree resolves: a copy, detail empty again");
    }

    // 2: a new client world starts without a stop of the old one (world.lifecycle off): A's player is gone
    tree_here = false;  // B has no tree yet
    client_world = 0x6000;
    KilnWorldEvent ev{};
    ev.size = sizeof ev;
    ev.event = KILN_WORLD_STARTED;
    ev.kind = KILN_WORLD_CLIENT;
    ev.session = 2;
    ev.world = client_world;
    world_fn(&ev, nullptr);
    post();
    {
        const KilnResult r = state(&s);
        check(r == KILN_ERR_NOT_FOUND && s.nodes == 0 && s.player == 0, "world B started without A stopping: no copy of A");
        check(node_count() == 0, "world B: no nodes of A");
    }

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
