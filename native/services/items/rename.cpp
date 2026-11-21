// SPDX-License-Identifier: MIT
// kiln.items v9 (since 0.2.0-dev.30.3, GitHub issue #112, thanks Fuzen777): renaming a container. A request is checked
// at the call (the text like the game's text field; on the tick thread the entity too), queued, and run in the next Pre
// through the game's own rename (names.cpp: set_string, the tracked write or AddComponent, as the message dispatcher's
// rename case does for a player's Rename). See kiln_items.h "renaming a container".
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "engine.h"
#include "equip_logic.h"
#include "names.h"
#include "rename.h"
#include "rename_logic.h"
#include "services/kiln_world.h"
#include "xfer.h"

namespace kitems {
uint64_t items_next_ticket();
uint64_t current_step();

namespace {

constexpr size_t kMaxPending = 1024;
constexpr uint32_t kPerStep = 64;
constexpr uint64_t kTimeoutSteps = 250;  // ~10 s at 24.5 Hz

struct Req {
    uint64_t ticket = 0;
    KilnOwner owner = 0;
    KilnEntity entity = 0;
    uint32_t len = 0;
    char text[KILN_ITEMS_NAME_CAPACITY] = {};
    uint64_t submitted = 0;
};
struct Callback {
    KilnItemsRenameDoneFn fn = nullptr;
    void* user = nullptr;
    KilnOwner owner = 0;
};

std::mutex g_mu;
std::deque<Req> g_pending;
std::vector<KilnItemsRenameResult> g_done;
std::unordered_map<uint64_t, Callback> g_callbacks;
std::atomic<uint32_t> g_pending_n{0}, g_ok{0}, g_refused{0};
std::atomic<bool> g_off{false};
std::mutex g_detail_mu;
char g_detail[160] = "";

void set_detail(const char* s) {
    std::lock_guard lock(g_detail_mu);
    std::snprintf(g_detail, sizeof g_detail, "%s", s);
}

void fill(KilnItemsRenameResult* r, int32_t status, uint64_t ticket, KilnEntity e, const char* msg) {
    std::memset(r, 0, sizeof *r);
    r->size = sizeof *r;
    r->status = status;
    r->ticket = ticket;
    r->entity = e;
    std::snprintf(r->message, sizeof r->message, "%s", msg);
}

bool server_here() {
    const KilnApi* api = items_api();
    if (!api) return false;
    KilnSessionInfo si{};
    si.size = sizeof si;
    return equip_here(api->session(&si), si.state) == EquipHere::Yes;
}

const KilnWorldApi* world_api() {
    static std::atomic<const KilnWorldApi*> cached{nullptr};
    if (auto* w = cached.load(std::memory_order_acquire)) return w;
    const KilnApi* api = items_api();
    if (!api) return nullptr;
    auto* w = static_cast<const KilnWorldApi*>(api->service(KilnStr{KILN_WORLD_SERVICE, std::strlen(KILN_WORLD_SERVICE)}, 1));
    if (w && w->header.size >= offsetof(KilnWorldApi, exists) + sizeof(void*) && w->exists) cached.store(w, std::memory_order_release);
    return cached.load(std::memory_order_acquire);
}

// ---- the game, for rename_logic.h ----
struct GameCtx {
    uintptr_t world;
};
int op_exists(void*, KilnEntity e) {
    const KilnWorldApi* w = world_api();
    if (!w) return -1;
    __try {
        return w->exists(e) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int op_container(void* c, KilnEntity e) {
    bool m = false;
    unsigned long code = 0;
    if (!inventory_matches(static_cast<GameCtx*>(c)->world, e, KILN_ITEMS_FIND_CONTAINERS, &m, &code)) return -1;
    return m ? 1 : 0;
}
bool op_name_id(void* c, KilnEntity e, bool* has, uint64_t* id) { return name_component(static_cast<GameCtx*>(c)->world, e, has, id); }
bool op_table(void* c, uint64_t id, bool* holds, uint32_t* used, uint32_t* limit) {
    return names_table(static_cast<GameCtx*>(c)->world, id, holds, used, limit);
}
bool op_set_string(void* c, uint64_t old_id, const char* text, uint32_t len, uint8_t* error, uint64_t* id) {
    return names_set_string(static_cast<GameCtx*>(c)->world, old_id, text, len, error, id);
}
bool op_track(void* c, KilnEntity e, void** slot) { return names_track(static_cast<GameCtx*>(c)->world, e, slot); }
bool op_store(void*, void* slot, uint64_t id) { return names_store(slot, id); }
bool op_add_id(void* c, KilnEntity e, uint64_t id) { return names_add_id(static_cast<GameCtx*>(c)->world, e, id); }

rename::RenameOps game_ops(GameCtx* ctx) {
    rename::RenameOps g{};
    g.ctx = ctx;
    g.exists = &op_exists;
    g.container = &op_container;
    g.name_id = &op_name_id;
    g.table = &op_table;
    g.set_string = &op_set_string;
    g.track = &op_track;
    g.store = &op_store;
    g.add_id = &op_add_id;
    return g;
}

bool guarded_done(KilnItemsRenameDoneFn fn, const KilnItemsRenameResult* r, void* user) {
    __try {
        fn(r, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

void rename_resolved() {
    if (E.rename_ok) return;
    set_detail(!E.names_ok ? "items.rename needs items.names, which is off on this build"
               : !E.add_component || !E.write_component_tick ? "items.rename is off on this build (the rename handler's calls were not found)"
                                                               : "items.rename is off on this build (the string table's layout differs)");
}

KilnResult rename_submit(KilnOwner owner, const KilnItemsRenameRequest* r, KilnItemsRenameDoneFn done, void* user,
                         uint64_t* out_ticket, KilnItemsRenameResult* out_refusal) {
    KilnItemsRenameResult refusal{};
    auto refuse = [&](KilnResult rc, int32_t status, const char* msg) {
        fill(&refusal, status, 0, r ? r->entity : 0, msg);
        if (out_refusal) *out_refusal = refusal;
        return rc;
    };
    if (out_ticket) *out_ticket = 0;
    if (!r || r->size < sizeof(KilnItemsRenameRequest) || r->reserved != 0)
        return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_ITEMS_RENAME_BAD_TEXT, "no request, a short one, or reserved fields not 0");
    if (r->entity == 0) return refuse(KILN_ERR_INVALID_ARGUMENT, KILN_ITEMS_RENAME_GONE, "no entity (0)");
    char why[160];
    const int32_t text = rename::check_text(r->text, r->len, why, sizeof why);
    if (text != KILN_ITEMS_RENAME_DONE) return refuse(KILN_ERR_INVALID_ARGUMENT, text, why);
    if (!E.rename_ok || g_off.load()) {
        std::lock_guard lock(g_detail_mu);
        return refuse(KILN_ERR_UNAVAILABLE, KILN_ITEMS_RENAME_UNAVAILABLE, g_detail[0] ? g_detail : "items.rename is off");
    }
    if (!server_here())
        return refuse(KILN_ERR_UNAVAILABLE, KILN_ITEMS_RENAME_NOT_SERVER,
                      "no server world in this process (a game client joined to another machine's world, or in its menu): "
                      "names are set where the world runs");
    const KilnApi* api = items_api();
    if (api->on_tick_thread()) {
        const uintptr_t world = api->server_world();
        if (world) {  // the entity as it is now; the Pre checks it again
            GameCtx ctx{world};
            const int alive = op_exists(&ctx, r->entity);
            if (alive == 0) return refuse(KILN_ERR_NOT_FOUND, KILN_ITEMS_RENAME_GONE, "no such entity");
            if (op_container(&ctx, r->entity) == 0)
                return refuse(KILN_ERR_NOT_FOUND, KILN_ITEMS_RENAME_NOT_CONTAINER,
                              alive == 1 ? "not a container (no inventory of its own, or a player's or an NPC's)" : "not a container (or no such entity)");
        }
    }
    Req q{};
    q.ticket = items_next_ticket();
    q.owner = owner;
    q.entity = r->entity;
    q.len = r->len;
    std::memcpy(q.text, r->text, r->len);
    q.submitted = current_step();
    {
        std::lock_guard lock(g_mu);
        if (g_pending.size() >= kMaxPending) return refuse(KILN_ERR_UNAVAILABLE, KILN_ITEMS_RENAME_UNAVAILABLE, "1024 renames are waiting");
        g_pending.push_back(q);
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()));
        if (done) g_callbacks[q.ticket] = Callback{done, user, owner};
    }
    if (out_ticket) *out_ticket = q.ticket;
    return KILN_OK;
}

void rename_pre(uint64_t step) {
    const KilnApi* api = items_api();
    const uintptr_t world = api ? api->server_world() : 0;
    std::vector<Req> batch;
    std::vector<KilnItemsRenameResult> done;
    {
        std::lock_guard lock(g_mu);
        while (!g_pending.empty() && (g_off.load() || !E.rename_ok)) {
            KilnItemsRenameResult r;
            fill(&r, KILN_ITEMS_RENAME_UNAVAILABLE, g_pending.front().ticket, g_pending.front().entity, "renames were switched off (see kiln.log)");
            r.steps = static_cast<uint32_t>(step - g_pending.front().submitted);
            g_done.push_back(r);
            g_pending.pop_front();
        }
        if (!world) {
            while (!g_pending.empty() && step > g_pending.front().submitted + kTimeoutSteps) {
                KilnItemsRenameResult r;
                fill(&r, KILN_ITEMS_RENAME_DROPPED, g_pending.front().ticket, g_pending.front().entity, "no server world ran for 10 s");
                r.steps = static_cast<uint32_t>(step - g_pending.front().submitted);
                g_done.push_back(r);
                g_pending.pop_front();
            }
        } else {
            while (!g_pending.empty() && batch.size() < kPerStep) {
                batch.push_back(g_pending.front());
                g_pending.pop_front();
            }
        }
        g_pending_n.store(static_cast<uint32_t>(g_pending.size()));
    }
    if (!batch.empty()) {
        GameCtx ctx{world};
        const rename::RenameOps g = game_ops(&ctx);
        for (const Req& q : batch) {
            KilnItemsRenameResult r;
            if (g_off.load()) {
                fill(&r, KILN_ITEMS_RENAME_UNAVAILABLE, q.ticket, q.entity, "renames were switched off (see kiln.log)");
            } else {
                const rename::Outcome o = rename::run(g, q.entity, q.text, q.len);
                fill(&r, o.status, q.ticket, q.entity, o.message);
                r.changed = o.changed;
                r.cleared = o.cleared;
                r.game_error = o.game_error;
                // a fault, a layout surprise or an id the game did not compute like Kiln: no more renames this session
                if (o.status == KILN_ITEMS_RENAME_UNAVAILABLE && !g_off.exchange(true)) {
                    char d[160];
                    std::snprintf(d, sizeof d, "renames switched off: %s", o.message);
                    set_detail(d);
                    items_log(KILN_LOG_ERROR, "container renames switched off until a restart: %s", o.message);
                }
            }
            r.steps = static_cast<uint32_t>(step - q.submitted);
            done.push_back(r);
        }
    }
    std::vector<Callback> cbs;
    {
        std::lock_guard lock(g_mu);
        for (auto& r : g_done) done.push_back(r);
        g_done.clear();
        for (const auto& r : done) {
            auto it = g_callbacks.find(r.ticket);
            if (it != g_callbacks.end()) {
                cbs.push_back(it->second);
                g_callbacks.erase(it);
            } else {
                cbs.push_back(Callback{});
            }
        }
    }
    for (size_t i = 0; i < done.size(); ++i) {
        const KilnItemsRenameResult& r = done[i];
        (r.status == KILN_ITEMS_RENAME_DONE ? g_ok : g_refused).fetch_add(1, std::memory_order_relaxed);
        items_log(r.status == KILN_ITEMS_RENAME_DONE ? KILN_LOG_INFO : KILN_LOG_WARN, "rename ticket %llu (owner %llu): entity %u -> status %d%s: %s",
                  static_cast<unsigned long long>(r.ticket), static_cast<unsigned long long>(cbs[i].owner), r.entity, r.status,
                  r.changed ? " (changed)" : "", r.message);
        if (cbs[i].fn && !guarded_done(cbs[i].fn, &r, cbs[i].user))
            items_log(KILN_LOG_ERROR, "a rename result callback (owner %llu) faulted; ignored", static_cast<unsigned long long>(cbs[i].owner));
    }
}

void rename_world_closed() {
    std::lock_guard lock(g_mu);
    const uint64_t step = current_step();
    while (!g_pending.empty()) {
        KilnItemsRenameResult r;
        fill(&r, KILN_ITEMS_RENAME_DROPPED, g_pending.front().ticket, g_pending.front().entity, "the server world closed");
        r.steps = static_cast<uint32_t>(step - g_pending.front().submitted);
        g_done.push_back(r);
        g_pending.pop_front();
    }
    g_pending_n.store(0);
}

bool rename_cancel(uint64_t ticket) {
    std::lock_guard lock(g_mu);
    return g_callbacks.erase(ticket) != 0;
}

void rename_cancel_owner(KilnOwner owner) {
    std::lock_guard lock(g_mu);
    for (auto it = g_callbacks.begin(); it != g_callbacks.end();) {
        if (it->second.owner == owner) it = g_callbacks.erase(it);
        else ++it;
    }
}

KilnResult rename_status(KilnItemsRenameStatus* out) {
    if (!out || out->size < sizeof(KilnItemsRenameStatus)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(out, 0, sizeof *out);
    out->size = sizeof *out;
    out->feature = E.rename_ok && !g_off.load();
    out->server = server_here();
    out->available = out->feature && out->server;
    out->pending = g_pending_n.load();
    out->done = g_ok.load();
    out->refused = g_refused.load();
    std::lock_guard lock(g_detail_mu);
    std::snprintf(out->detail, sizeof out->detail, "%s",
                  out->available ? "" : !out->feature ? (g_detail[0] ? g_detail : "items.rename is off")
                                      : "no server world in this process (a game client joined to another machine's world, or in its menu)");
    return KILN_OK;
}

}  // namespace kitems
