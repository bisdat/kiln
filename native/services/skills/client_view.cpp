// SPDX-License-Identifier: MIT
// kiln.skills.client (since 0.2.0-dev.31.13, mod API 0.2.22): a read-only copy of the local player's skill tree as the
// game client holds it, for a player who joined someone else's world. ABI: native/core/abi/services/kiln_skills_client.h;
// pure part: client_view_logic.h (host test tests/test_client_view.cpp); symbols: sigdb client.d/skills-client-view.json
// (tests/client_view_sigdb_test.py); evidence: docs/re/progression.md section 8.
//
// The copy is taken on the client world's thread after its step (Post), only while a caller read in the last
// KILN_SKILLS_CLIENT_DEMAND_MS; readers on any thread get that copy. Nothing here calls game code or writes game memory:
// it copies bytes the game's skilltree_ui and the tree loader wrote, under SEH.
#include "client_view.h"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "client_view_logic.h"
#include "services/kiln_caps.h"
#include "services/kiln_client.h"
#include "services/kiln_skills_client.h"
#include "shadow.h"
#include "tree.h"

namespace kiln::skills::client_view {
namespace {

using namespace kiln::skills::client;
static_assert(NodeOffsets::size == kNode && NodeOffsets::pos == F::pos && NodeOffsets::in == F::in &&
                  NodeOffsets::out == F::out && NodeOffsets::cost == F::cost && NodeOffsets::type == F::type &&
                  NodeOffsets::cls == F::cls && NodeOffsets::max_level == F::max_level &&
                  NodeOffsets::level_index == F::level_index && kLinkSize == kLink,
              "client_view_logic.h's node layout is tree.h's");

constexpr const char* kFeature = "skills.client_view";

const KilnApi* g_api = nullptr;
KilnOwner g_owner = 0;
const KilnClientApi* g_client = nullptr;
std::atomic<bool> g_on{false}, g_fault{false};
std::atomic<uint64_t> g_read_ms{0};
int64_t g_records = 0, g_record_size = 0x44, g_total = 0, g_spent = 0, g_available = 0, g_rec_level = 0x41,
        g_rec_unlocked = 0x43, g_tree_data = 0x20, g_tree_ids = 0x150;

std::mutex g_mu;  // the copy and the detail
struct Copy {
    KilnEntity player = 0;
    int32_t total = 0, spent = 0, available = 0;
    std::vector<KilnSkillsClientNode> nodes;
    std::vector<KilnSkillsClientLink> links;
    uint64_t step = 0, at_ms = 0;
    bool any = false;
} g_copy;
char g_detail[160] = "not resolved yet";
char g_skip[160] = "";  // while on and no copy yet: why the last demanded step took none

void skip(const char* why) {
    std::lock_guard lock(g_mu);
    std::snprintf(g_skip, sizeof g_skip, "%s", why);
}

KilnStr str(const char* s) { return KilnStr{s, std::strlen(s)}; }
uint64_t now_ms() { return GetTickCount64(); }

void logf(int level, const char* fmt, ...) {
    if (!g_api) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > static_cast<int>(sizeof buf) - 1) n = sizeof buf - 1;
    g_api->log(g_owner, level, KilnStr{buf, static_cast<size_t>(n)});
}

void set_detail(const char* why) {
    std::lock_guard lock(g_mu);
    std::snprintf(g_detail, sizeof g_detail, "%s", why);
}

void lower(uint32_t reason, const char* why) {
    set_detail(why);
    const auto* caps = static_cast<const KilnCapsApi*>(g_api ? g_api->service(str(KILN_CAPS_SERVICE), KILN_CAPS_VERSION) : nullptr);
    if (caps && caps->report) caps->report(g_owner, str(kFeature), KILN_CAP_UNAVAILABLE, reason, str(why));
}

bool cst(const char* n, int64_t* out) { return g_api->constant(str(n), out) == KILN_OK; }

// one guarded copy (no C++ objects with destructors inside __try)
bool raw(void* dst, uintptr_t src, size_t n) {
    if (!src) return false;
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(src), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T>
bool raw(T* out, uintptr_t src) {
    return raw(out, src, sizeof(T));
}

void fault(const char* what) {
    if (g_fault.exchange(true)) return;
    char why[160];
    std::snprintf(why, sizeof why, "the client's skill tree faulted while %s: off until the game restarts", what);
    logf(KILN_LOG_WARN, "skills: %s", why);
    lower(KILN_CAP_REASON_FAULTED, why);
}

// Reads the tree and the player's state from the client world. false with *why: no copy this step (nothing faulted).
bool take(uintptr_t w, KilnEntity player, uint64_t step, Copy* c, const char** why) {
    const uintptr_t play = g_api->singleton(w, str("G38UiPlayData"));
    const uintptr_t tree = g_api->singleton(w, str("g38_skilltree::SkillTree"));
    if (!play || !tree) return *why = "no skill tree in the client world yet", false;
    int32_t pts[3] = {};
    if (!raw(&pts[0], play + g_total) || !raw(&pts[1], play + g_spent) || !raw(&pts[2], play + g_available))
        return fault("the points were read"), *why = "faulted", false;
    if (!points_ok(pts[0], pts[1], pts[2])) return *why = "the points are not a total, spent and available yet", false;
    uintptr_t data = 0, ids = 0;
    uint64_t count = 0;
    uint8_t header[16] = {};
    if (!raw(&data, tree + g_tree_data) || !raw(&ids, tree + g_tree_ids) || !raw(&count, tree + g_tree_ids + 8))
        return fault("the tree was read"), *why = "faulted", false;
    if (!data || !ids || !count) return *why = "the client's tree is not loaded yet", false;
    Arrays arr;
    if (!raw(header, data, sizeof header) || !arrays_of(header, &arr) || count > arr.node_count)
        return *why = "the client's tree resource does not check out", false;
    std::vector<uint32_t> order(static_cast<size_t>(count));
    std::vector<uint8_t> nodes(static_cast<size_t>(arr.node_count) * kNode), links(static_cast<size_t>(arr.link_count) * kLink);
    if (!raw(order.data(), ids, order.size() * 4) || !raw(nodes.data(), data + static_cast<uintptr_t>(arr.nodes), nodes.size()) ||
        (!links.empty() && !raw(links.data(), data + static_cast<uintptr_t>(arr.links), links.size())))
        return fault("the tree's arrays were read"), *why = "faulted", false;
    std::unordered_map<uint32_t, size_t> at;  // node id -> its bytes in the resource
    for (size_t i = 0; i < arr.node_count; ++i) at.emplace(rd<uint32_t>(nodes.data(), i * kNode), i * kNode);
    // the screen's records map
    const uintptr_t map = play + static_cast<uintptr_t>(g_records);
    uintptr_t bits = 0, keys = 0, vals = 0;
    uint64_t cap = 0;
    if (!raw(&bits, map + 0x08) || !raw(&cap, map + 0x10) || !raw(&keys, map + 0x28) || !raw(&vals, map + 0x40))
        return fault("the screen's records were read"), *why = "faulted", false;
    std::vector<uint64_t> bitv;
    std::vector<uint32_t> keyv;
    std::vector<uint8_t> valv;
    const bool have_map = cap && !(cap & (cap - 1)) && cap <= 65536 && bits && keys && vals;
    if (have_map) {
        bitv.resize(static_cast<size_t>((cap + 63) / 64));
        keyv.resize(static_cast<size_t>(cap));
        valv.resize(static_cast<size_t>(cap * static_cast<uint64_t>(g_record_size)));
        if (!raw(bitv.data(), bits, bitv.size() * 8) || !raw(keyv.data(), keys, keyv.size() * 4) || !raw(valv.data(), vals, valv.size()))
            return fault("the screen's records were copied"), *why = "faulted", false;
    }
    c->nodes.clear();
    c->links.clear();
    for (size_t i = 0; i < order.size(); ++i) {
        const auto it = at.find(order[i]);
        if (it == at.end()) continue;  // an id of the game's list the resource does not have: nothing to describe
        Record rec;
        if (have_map) {
            const int64_t slot = find_slot(order[i], cap, [&](uint64_t s) { return ((bitv[s >> 6] >> (s & 63)) & 1) != 0; },
                                           [&](uint64_t s) { return keyv[s]; });
            if (slot >= 0) {
                const uint8_t* r = valv.data() + static_cast<size_t>(slot) * static_cast<size_t>(g_record_size);
                rec = Record{true, r[g_rec_level], r[g_rec_unlocked]};
            }
        }
        c->nodes.push_back(node_of(nodes.data() + it->second, static_cast<uint16_t>(i), rec, shadow::is_shadow(order[i])));
    }
    for (size_t i = 0; i < arr.link_count; ++i) c->links.push_back(link_of(links.data() + i * kLink));
    c->player = player;
    c->total = pts[0];
    c->spent = pts[1];
    c->available = pts[2];
    c->step = step;
    c->at_ms = now_ms();
    c->any = true;
    return true;
}

void KILN_CALL on_post(const KilnTickInfo* tick, void*) {
    if (!g_on.load() || g_fault.load() || !demanded(g_read_ms.load(std::memory_order_relaxed), now_ms())) return;
    KilnClientState st{sizeof st};
    if (!g_client || g_client->state(&st) != KILN_OK || !st.local_player) return skip("no local player in the client world yet");
    const uintptr_t w = g_api->world(KILN_WORLD_CLIENT);
    if (!w) return skip("no client world");
    Copy c;
    const char* why = nullptr;
    if (!take(w, st.local_player, tick ? tick->step : 0, &c, &why)) return skip(why ? why : "no copy");  // the last copy stays
    std::lock_guard lock(g_mu);
    g_copy = std::move(c);
    g_skip[0] = 0;
}

void KILN_CALL on_world(const KilnWorldEvent* ev, void*) {
    // STOPPING needs world.lifecycle; without it the next world's STARTED is the first word that the old one is gone
    if (!ev || ev->kind != KILN_WORLD_CLIENT || (ev->event != KILN_WORLD_STOPPING && ev->event != KILN_WORLD_STARTED)) return;
    std::lock_guard lock(g_mu);
    g_copy = Copy{};  // another session's player must not show
}

KilnResult gate() {
    if (!g_api || g_api->side() != KILN_SIDE_CLIENT || !g_on.load() || g_fault.load()) return KILN_ERR_UNAVAILABLE;
    g_read_ms.store(now_ms(), std::memory_order_relaxed);
    return KILN_OK;
}

KilnResult KILN_CALL t_state(KilnSkillsClientState* out) {
    if (!out || out->size != sizeof *out) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->on = g_on.load() && !g_fault.load();
    out->world = g_api && g_api->world(KILN_WORLD_CLIENT) != 0;
    const KilnResult g = gate();
    out->copying = g == KILN_OK;
    std::lock_guard lock(g_mu);
    // off: why; on without a copy: why the last demanded step took none; on with a copy: ""
    std::snprintf(out->detail, sizeof out->detail, "%s", !out->on ? g_detail : g_copy.any ? "" : g_skip);
    if (g != KILN_OK) return g;
    if (!g_copy.any) return KILN_ERR_NOT_FOUND;
    out->player = g_copy.player;
    out->total = g_copy.total;
    out->spent = g_copy.spent;
    out->available = g_copy.available;
    out->nodes = static_cast<uint32_t>(g_copy.nodes.size());
    for (const auto& n : g_copy.nodes) out->unlocked += (n.flags & KILN_SKILLS_CLIENT_UNLOCKED) ? 1u : 0u;
    out->links = static_cast<uint32_t>(g_copy.links.size());
    out->step = g_copy.step;
    out->age_ms = static_cast<int64_t>(now_ms() - g_copy.at_ms);
    return KILN_OK;
}

KilnResult KILN_CALL t_nodes(KilnSkillsClientNode* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (KilnResult g = gate(); g != KILN_OK) return g;
    std::lock_guard lock(g_mu);
    return copy_out(g_copy.nodes, out, capacity, out_count);
}

KilnResult KILN_CALL t_links(KilnSkillsClientLink* out, uint32_t capacity, uint32_t* out_count) {
    if (!out_count || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    *out_count = 0;
    if (KilnResult g = gate(); g != KILN_OK) return g;
    std::lock_guard lock(g_mu);
    return copy_out(g_copy.links, out, capacity, out_count);
}

const KilnSkillsClientApi kTable = {{sizeof(KilnSkillsClientApi), KILN_SKILLS_CLIENT_VERSION}, &t_state, &t_nodes, &t_links};

}  // namespace

KilnResult init(const KilnApi* api, KilnOwner owner) {
    g_api = api;
    g_owner = owner;
    if (api->side() != KILN_SIDE_CLIENT) {
        set_detail("a dedicated server has no client world: kiln.progression has the players' trees");
    } else if (!api->feature(str(kFeature))) {
        set_detail("skills.client_view did not resolve on this game build");
    } else {
        int64_t unlock_levels = 0, map_cap = 0, map_values = 0, level_index = 0, cap_at = 0, bits_at = 0, keys_at = 0, values_at = 0;
        const bool consts = cst("skills/ui.records", &g_records) && cst("skills/ui.record_size", &g_record_size) &&
                            cst("skills/ui.points_total", &g_total) && cst("skills/ui.points_spent", &g_spent) &&
                            cst("skills/ui.points_available", &g_available) && cst("skills/ui.rec_level", &g_rec_level) &&
                            cst("skills/ui.rec_unlocked", &g_rec_unlocked) && cst("skills/tree.data", &g_tree_data) &&
                            cst("skills/tree.ids", &g_tree_ids) && cst("skills/ui.unlock_levels", &unlock_levels) &&
                            cst("skills/ui.map_cap", &map_cap) && cst("skills/ui.map_values", &map_values) &&
                            cst("skills/node.level_index", &level_index) && cst("skills/ui.records_cap_at", &cap_at) &&
                            cst("skills/ui.records_bits_at", &bits_at) && cst("skills/ui.records_keys_at", &keys_at) &&
                            cst("skills/ui.records_values_at", &values_at);
        const char* why = !consts ? "a constant of skills.client_view is missing"
                          : (bits_at != g_records + 0x08 || cap_at != g_records + 0x10 || keys_at != g_records + 0x28 ||
                             values_at != g_records + 0x40 || map_cap != 0x10 || map_values != 0x40)
                              ? "the screen's records map is not where Kiln reads it"
                          : level_index != static_cast<int64_t>(F::level_index) ? "the node's levelIndex moved"
                          : (g_rec_level < 0 || g_rec_unlocked < 0 || g_rec_level >= g_record_size || g_rec_unlocked >= g_record_size)
                              ? "the screen's record layout does not check out"
                          : nullptr;
        if (why) {
            logf(KILN_LOG_WARN, "skills: the client's skill-tree view is off: %s", why);
            lower(KILN_CAP_REASON_SYMBOL_MISSING, why);
        } else {
            g_on = true;
            set_detail("");
        }
    }
    return api->register_service(owner, str(KILN_SKILLS_CLIENT_SERVICE), &kTable);
}

void ready() {
    if (!g_on.load()) return;
    g_client = static_cast<const KilnClientApi*>(g_api->service(str("kiln.client"), 1));
    KilnHandle h = 0;
    if (!g_client || g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_POST, &on_post, nullptr, &h) != KILN_OK ||
        g_api->subscribe_world(g_owner, &on_world, nullptr, &h) != KILN_OK) {
        g_on = false;
        lower(KILN_CAP_REASON_SERVICE_MISSING, !g_client ? "kiln.client is not here: no local player for the client's skill-tree view"
                                                          : "no tick of the client world for the client's skill-tree view");
        return;
    }
    logf(KILN_LOG_INFO, "skills: the client's skill-tree view is on (the local player's tree as the client holds it, copied while read)");
}

}  // namespace kiln::skills::client_view
