// SPDX-License-Identifier: MIT
// kiln.repl's receive side on a Kiln game client (KILN_REPL_RECEIVE watches, capability bit KILN_REPL_CAP_CLIENT; the
// dossier's chapter 11 §4.1 "Client watch", §1 "Client").
//
// A client watch is the game's own decode: the message store (ClientStore, main thread, both channels) names the entity
// of each message before the readers decode it; the Decode hook copies the watched entity's components out of the state
// the game decoded (borrowed, never kept or freed; offsets from the game's pure StateIndex) into a ring, and a destroy
// list that names it in its list A gives KILN_REPL_KIND_DESTROYED. The client world's Pre (the same main thread) hands
// the records to each watch's fn: one event per message of the client channel, then KILN_REPL_KIND_ENDED (destroyed,
// or the client world stopped). The client's own receive counters (stats_enable / peer_stats called on the client
// world's thread) count every message of both channels in the same ClientStore hook. ClientStore is hooked only while a
// receive watch or a counter holder exists, Decode only while a receive watch exists; each hook has guard.h's shape.
#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <iterator>

#include "kiln_hook.hpp"
#include "repl.h"
#include "rings.h"
#include "wire.h"

namespace repl {
namespace {

constexpr uint32_t kMaxRx = 16;
constexpr uint64_t kRxTag = 0x5258ull << 48;  // "RX"

bool g_resolved = false;
uintptr_t f_store = 0, f_decode = 0;

struct RxWatch {
    std::atomic<uint64_t> handle{0};
    std::atomic<bool> removed{false};
    KilnOwner owner = 0;
    uint32_t entity = 0;
    KilnReplWatchFn fn = nullptr;
    void* user = nullptr;
    bool ended = false;
    bool destroyed = false;
};
RxWatch g_rx[kMaxRx];
uint64_t g_rx_serial = 0;
std::atomic<uint32_t> g_hot_n{0};
std::atomic<uint32_t> g_hot[kMaxRx];
std::atomic<bool> g_dirty{false};

Ring g_ring;  // producer: the store and decode hooks (main thread); consumer: the client world's Pre (main thread)
Registry* g_reg = nullptr;                     // the client world's registry facts (main thread, outside the hooks)
std::atomic<const Registry*> g_reg_hot{nullptr};
std::atomic<uint64_t> g_records{0};

Guard g_store_guard{"client-store"}, g_decode_guard{"client-decode"};
kiln::RawHook h_store, h_decode;
bool g_hooked = false;         // ClientStore (receive watches or the receive counters)
bool g_decode_hooked = false;  // Decode (receive watches only)

// ---- the client's own receive counters (stats_enable / peer_stats on the client world's thread) ----
// Counted in the ClientStore hook, on the game's main thread only (one writer); read on the same thread.
struct RxCounters {
    uint64_t frames = 0, messages = 0, bytes = 0, creates = 0, updates = 0, destroy_lists = 0, destroyed_ids = 0;
    uint32_t last_tick = 0;
};
RxCounters g_rx_count[2];  // [0] client channel, [1] prediction channel
constexpr uint32_t kMaxRxOwners = 64;
std::mutex g_rx_owners_mu;  // the main thread and an owner-retiring callback (any thread)
KilnOwner g_rx_owners[kMaxRxOwners];
uint32_t g_rx_owner_n = 0;
std::atomic<bool> g_rx_counting{false};
using StoreFn = void (*)(uintptr_t world, const Slice* msg, uint64_t pred);
using DecodeFn = StateRef* (*)(StateRef* out, void* alloc, const Slice* payload, const uint8_t* base, uint64_t pred,
                               uintptr_t registry, uint64_t tick, void* zero);

enum : uint16_t { kRecState = 1, kRecDestroy = 2 };
struct StateRec {
    uint32_t entity, tick, seq, kind;
    uint16_t count, pad;
    uint32_t data_bytes;
};
struct Comp {
    uint16_t index, size;
    uint32_t hash, offset;
    uint8_t custom, pad[3];
};
struct DestroyRec {
    uint32_t entity, tick, seq;
};
static_assert(sizeof(StateRec) == 24 && sizeof(Comp) == 16);

// The message the store is handling now (main thread only).
struct Current {
    bool active = false;
    uint32_t entity = 0, kind = 0, tick = 0, seq = 0;
};
thread_local Current t_cur;

bool watched(uint32_t entity) {
    const uint32_t n = g_hot_n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
        if (g_hot[i].load(std::memory_order_relaxed) == entity) return true;
    return false;
}

// ---- ClientStore: which entity the readers decode next; destroy lists ----
struct StoreCall {
    const Slice* msg;
    uint64_t pred;
};
// The receive counters: every message of both channels, as the client's session handed it to the game.
void count_received(const uint8_t* d, uint64_t size, uint32_t ch) {
    RxCounters& r = g_rx_count[ch];
    const uint32_t w0 = u32_at(d), kind = w0 & 7, tick = w0 >> 3;
    if (tick != r.last_tick) {  // a new server frame on this channel
        ++r.frames;
        r.last_tick = tick;
    }
    ++r.messages;
    r.bytes += size;
    if (kind == kKindCreate) ++r.creates;
    else if (kind == kKindUpdate) ++r.updates;
    else if (kind == kKindDestroyList) {
        DestroyList l;
        if (parse_destroy(d, size, &l)) {
            ++r.destroy_lists;
            r.destroyed_ids += l.na + l.nb;
        }
    }
}

void store_before(void* p) {
    const auto* c = static_cast<StoreCall*>(p);
    t_cur.active = false;
    if (!c->msg || !c->msg->ptr || c->msg->size < 4) return;
    if (g_rx_counting.load(std::memory_order_relaxed)) count_received(c->msg->ptr, c->msg->size, (c->pred & 0xff) ? 1 : 0);
    if ((c->pred & 0xff) != 0 || !g_hot_n.load(std::memory_order_relaxed)) return;  // the watches: the client channel only
    const uint8_t* d = c->msg->ptr;
    const uint32_t kind = d[0] & 7;
    if (kind == kKindCreate || kind == kKindUpdate) {
        Header h{};
        if (!parse_header(d, c->msg->size, &h) || !watched(h.entity)) return;
        t_cur = Current{true, h.entity, h.kind, h.tick, h.seq};
    } else if (kind == kKindDestroyList) {
        DestroyList l;
        if (!parse_destroy(d, c->msg->size, &l)) return;
        for (uint32_t k = 0; k < l.na; ++k) {  // list A: destroyed (list B: left this client's view)
            const uint32_t id = u32_at(l.ids + 4 * k);
            if (!watched(id)) continue;
            DestroyRec r{id, l.tick, l.seq};
            if (g_ring.push(kRecDestroy, &r, sizeof r)) g_records.fetch_add(1, std::memory_order_relaxed);
        }
    }
}
void d_store(uintptr_t world, const Slice* msg, uint64_t pred) {
    StoreCall c{msg, pred};
    detour(g_store_guard, &c, &store_before, nullptr, h_store.original<StoreFn>(), world, msg, pred);
    t_cur.active = false;  // outside the guard: the next message starts clean
}

// ---- Decode: the state the game decoded for the current message ----
struct DecodeCall {
    StateRef* out;
    uintptr_t registry;
    uint64_t pred;
    StateRef* result;
};
void decode_after(void* p) {
    const auto* c = static_cast<DecodeCall*>(p);
    if (!t_cur.active || (c->pred & 0xff) != 0 || !c->result || !c->result->state) return;
    t_cur.active = false;  // one decode per message
    const Registry* reg = g_reg_hot.load(std::memory_order_acquire);
    if (!reg) return;  // the client world's registry is read at the next Pre; this record is missed
    const uint8_t* st = c->result->state;
    Part parts[kMaxComponents / 4];
    uint32_t n = 0;
    if (!state_parts(*reg, st, [&](uint8_t* index, const uint8_t* s) { return state_index(index, s, c->registry); }, parts,
                     static_cast<uint32_t>(std::size(parts)), &n))
        return;
    uint32_t data = 0;
    for (uint32_t k = 0; k < n; ++k) data += parts[k].size;
    const uint32_t len = static_cast<uint32_t>(sizeof(StateRec) + n * sizeof(Comp)) + data;
    uint8_t* rec = g_ring.reserve(kRecState, len);
    if (!rec) return;
    const StateRec h{t_cur.entity, t_cur.tick, t_cur.seq, t_cur.kind, static_cast<uint16_t>(n), 0, data};
    std::memcpy(rec, &h, sizeof h);
    uint8_t* comps = rec + sizeof h;
    uint8_t* out = comps + n * sizeof(Comp);
    uint32_t off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const ComponentFacts& f = reg->at[parts[k].index];
        const Comp cc{parts[k].index, parts[k].size, f.hash, off, static_cast<uint8_t>((f.flags & KILN_REPL_COMPONENT_CUSTOM_CODEC) ? 1 : 0), {}};
        std::memcpy(comps + k * sizeof(Comp), &cc, sizeof cc);
        std::memcpy(out + off, st + parts[k].off, parts[k].size);
        off += parts[k].size;
    }
    g_ring.commit(len);
    g_records.fetch_add(1, std::memory_order_relaxed);
}
StateRef* d_decode(StateRef* out, void* alloc, const Slice* payload, const uint8_t* base, uint64_t pred, uintptr_t registry,
                   uint64_t tick, void* zero) {
    DecodeCall c{out, registry, pred, nullptr};
    return detour(g_decode_guard, &c, nullptr, &decode_after, h_decode.original<DecodeFn>(), out, alloc, payload, base, pred,
                  registry, tick, zero);
}

// ---- delivery (the client world's Pre, main thread) ----
bool call_guarded(KilnReplWatchFn fn, const KilnReplWatchEvent* e, void* user) {
    __try {
        fn(e, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void deliver(RxWatch& w, const KilnReplWatchEvent& e) {
    if (w.ended || w.removed.load(std::memory_order_relaxed)) return;
    if (!call_guarded(w.fn, &e, w.user)) {
        logf(KILN_LOG_WARN, "repl: a receive watch callback of entity %u faulted; the watch ends", w.entity);
        w.ended = true;
    }
}
void end(RxWatch& w, uint32_t reason) {
    if (w.ended || w.removed.load(std::memory_order_relaxed)) return;
    KilnReplWatchEvent e{};
    e.size = sizeof e;
    e.direction = KILN_REPL_RECEIVE;
    e.kind = KILN_REPL_KIND_ENDED;
    e.entity = w.entity;
    e.reason = reason;
    deliver(w, e);
    w.ended = true;
    // ENDED is its last event: the slot goes back at the next apply (a remove() after it is not needed)
    w.removed.store(true, std::memory_order_release);
    g_dirty.store(true, std::memory_order_relaxed);
}

KilnReplComponent g_comps[kMaxComponents];

void deliver_record(const RecordHeader& rh, const uint8_t* p) {
    if (rh.type == kRecDestroy && rh.len >= sizeof(DestroyRec)) {
        DestroyRec d;
        std::memcpy(&d, p, sizeof d);
        for (RxWatch& w : g_rx) {
            if (!w.handle.load(std::memory_order_relaxed) || w.entity != d.entity) continue;
            KilnReplWatchEvent e{};
            e.size = sizeof e;
            e.direction = KILN_REPL_RECEIVE;
            e.channel = 4;
            e.kind = KILN_REPL_KIND_DESTROYED;
            e.gap = rh.gap;
            e.entity = d.entity;
            e.tick = d.tick;
            e.seq = d.seq;
            deliver(w, e);
            w.destroyed = true;
        }
        return;
    }
    if (rh.type != kRecState || rh.len < sizeof(StateRec)) return;
    StateRec h;
    std::memcpy(&h, p, sizeof h);
    if (sizeof(StateRec) + uint64_t{h.count} * sizeof(Comp) + h.data_bytes > rh.len || h.count > kMaxComponents) return;
    const uint8_t* comps = p + sizeof h;
    const uint8_t* data = comps + h.count * sizeof(Comp);
    for (uint32_t k = 0; k < h.count; ++k) {
        Comp cc;
        std::memcpy(&cc, comps + k * sizeof(Comp), sizeof cc);
        if (cc.offset + uint64_t{cc.size} > h.data_bytes) return;
        g_comps[k] = KilnReplComponent{cc.index, cc.size, cc.hash, cc.custom, {}, data + cc.offset};
    }
    for (RxWatch& w : g_rx) {
        if (!w.handle.load(std::memory_order_relaxed) || w.ended || w.entity != h.entity) continue;
        KilnReplWatchEvent e{};
        e.size = sizeof e;
        e.direction = KILN_REPL_RECEIVE;
        e.channel = 4;
        e.kind = static_cast<uint8_t>(h.kind);
        e.gap = rh.gap;
        e.entity = h.entity;
        e.tick = h.tick;
        e.seq = h.seq;
        e.count = h.count;
        e.components = g_comps;
        deliver(w, e);
    }
}

void apply() {
    uint32_t n = 0;
    for (RxWatch& w : g_rx) {
        if (!w.handle.load(std::memory_order_relaxed)) continue;
        if (w.removed.load(std::memory_order_acquire)) {
            w.handle.store(0, std::memory_order_release);
            w.removed.store(false, std::memory_order_relaxed);
            continue;
        }
        if (!w.ended) g_hot[n++].store(w.entity, std::memory_order_relaxed);
    }
    bool counting;
    {
        std::lock_guard lock(g_rx_owners_mu);
        counting = g_rx_owner_n > 0;
    }
    if (n && !g_ring.ready() && !g_ring.init()) {
        logf(KILN_LOG_WARN, "repl: receive watches have no ring (out of memory): no events");
        n = 0;
    }
    // the message store for the watches and the counters; the decode for the watches
    const bool want_store = n || counting, want_decode = n != 0;
    if (want_store && !g_hooked) {
        g_hooked = h_store.install(g_api, g_owner, f_store, reinterpret_cast<void*>(&d_store), "repl/client ClientStore");
        if (g_hooked) logf(KILN_LOG_INFO, "repl: receive hook installed (message store) for %s", n ? "a receive watch" : "the receive counters");
        else logf(KILN_LOG_WARN, "repl: receive hook not installed (%s)", h_store.error() ? h_store.error() : "?");
    } else if (!want_store && g_hooked) {
        h_store.revoke();
        g_hooked = false;
        logf(KILN_LOG_INFO, "repl: receive hook removed (no receive watch, no counters)");
    }
    if (want_decode && g_hooked && !g_decode_hooked) {
        g_decode_hooked = h_decode.install(g_api, g_owner, f_decode, reinterpret_cast<void*>(&d_decode), "repl/client Decode");
        if (g_decode_hooked) logf(KILN_LOG_INFO, "repl: receive hook installed (decode) for the receive watches");
        else logf(KILN_LOG_WARN, "repl: receive hook not installed (%s)", h_decode.error() ? h_decode.error() : "?");
    } else if ((!want_decode || !g_hooked) && g_decode_hooked) {
        h_decode.revoke();
        g_decode_hooked = false;
        logf(KILN_LOG_INFO, "repl: receive hook removed (decode: no receive watch)");
    }
    g_hot_n.store(g_hooked && g_decode_hooked ? n : 0, std::memory_order_release);
    g_rx_counting.store(counting && g_hooked, std::memory_order_relaxed);
}

// The client world's registry facts, read outside the hooks (they only read the published copy).
void read_client_registry() {
    if (g_reg_hot.load(std::memory_order_relaxed)) return;
    const uintptr_t world = g_api->world(KILN_WORLD_CLIENT);
    if (!world) return;
    if (Registry* reg = build_registry(world)) {
        g_reg = reg;
        g_reg_hot.store(reg, std::memory_order_release);
    }
}

void KILN_CALL on_client_pre(const KilnTickInfo*, void*) {
    if (g_dirty.exchange(false, std::memory_order_relaxed)) apply();
    if (!g_hooked && !g_ring.ready()) return;
    read_client_registry();
    if (g_ring.ready()) g_ring.drain([](const RecordHeader& rh, const uint8_t* p) { deliver_record(rh, p); });
    for (RxWatch& w : g_rx)
        if (w.destroyed) {
            w.destroyed = false;
            end(w, KILN_REPL_END_DESTROYED);
        }
}

void KILN_CALL on_client_world(const KilnWorldEvent* ev, void*) {
    if (!ev || ev->kind != KILN_WORLD_CLIENT || ev->event != KILN_WORLD_STOPPING) return;
    for (RxWatch& w : g_rx)
        if (w.handle.load(std::memory_order_relaxed)) end(w, KILN_REPL_END_WORLD_STOPPED);
    g_ring.discard();
    for (RxCounters& r : g_rx_count) r = RxCounters{};  // the receive counters start over with the next world
    // the next client world reads its registry again (the hooks are out of its frames until then)
    g_reg_hot.store(nullptr, std::memory_order_release);
    delete g_reg;
    g_reg = nullptr;
    g_dirty.store(false, std::memory_order_relaxed);
    apply();
}

}  // namespace

bool client_init() {
    auto sym = [](const char* name, uintptr_t* out) { return g_api->symbol(KilnStr{name, std::strlen(name)}, out) == KILN_OK; };
    g_resolved = side() == KILN_SIDE_CLIENT && g_api->feature(KilnStr{"repl.client", 11}) && sym("repl/ClientStore", &f_store) &&
                 sym("repl/Decode", &f_decode) && sym("repl/StateIndex", &F.state_index) && L.world_storage > 0;
    if (!g_resolved) return false;
    add_guard(&g_store_guard);
    add_guard(&g_decode_guard);
    KilnHandle h = 0;
    if (g_api->subscribe_world_tick(g_owner, KILN_WORLD_CLIENT, KILN_TICK_PRE, &on_client_pre, nullptr, &h) != KILN_OK) {
        g_resolved = false;
        return false;
    }
    g_api->subscribe_world(g_owner, &on_client_world, nullptr, &h);
    return true;
}

bool client_on() { return g_resolved; }

KilnResult client_watch(KilnOwner owner, KilnEntity e, KilnReplWatchFn fn, void* user, KilnHandle* out) {
    if (out) *out = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_world_thread(KILN_WORLD_CLIENT)) return KILN_ERR_WRONG_THREAD;
    if (!e || !fn || !out) return KILN_ERR_INVALID_ARGUMENT;
    if (g_dirty.exchange(false, std::memory_order_relaxed)) apply();  // ended and removed watches give their slots back
    RxWatch* slot = nullptr;
    for (RxWatch& w : g_rx)
        if (!w.handle.load(std::memory_order_relaxed)) {
            slot = &w;
            break;
        }
    if (!slot) return KILN_ERR_INVALID_ARGUMENT;  // 16 receive watches
    slot->owner = owner;
    slot->entity = static_cast<uint32_t>(e);
    slot->fn = fn;
    slot->user = user;
    slot->ended = false;
    slot->destroyed = false;
    slot->removed.store(false, std::memory_order_relaxed);
    const KilnHandle h = kRxTag | ++g_rx_serial;
    slot->handle.store(h, std::memory_order_release);
    apply();
    if (!g_hooked || !g_decode_hooked) {
        slot->handle.store(0, std::memory_order_release);
        apply();
        return KILN_ERR_UNAVAILABLE;
    }
    *out = h;
    return KILN_OK;
}

bool client_stats_thread() { return g_resolved && g_api->on_world_thread(KILN_WORLD_CLIENT) && !g_api->on_tick_thread(); }

KilnResult client_stats_enable(KilnOwner owner, bool on) {
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_world_thread(KILN_WORLD_CLIENT)) return KILN_ERR_WRONG_THREAD;
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    {
        std::lock_guard lock(g_rx_owners_mu);
        uint32_t i = 0;
        while (i < g_rx_owner_n && g_rx_owners[i] != owner) ++i;
        if (on && i == g_rx_owner_n) {
            if (g_rx_owner_n >= kMaxRxOwners) return KILN_ERR_INVALID_ARGUMENT;
            if (g_rx_owner_n == 0)
                for (RxCounters& r : g_rx_count) r = RxCounters{};  // the first holder: from 0 (main thread: no store runs now)
            g_rx_owners[g_rx_owner_n++] = owner;
        } else if (!on && i < g_rx_owner_n) {
            g_rx_owners[i] = g_rx_owners[--g_rx_owner_n];
        }
    }
    apply();
    if (on && !g_hooked) {  // kiln.hooks refused the message store: this owner holds nothing
        {
            std::lock_guard lock(g_rx_owners_mu);
            for (uint32_t i = 0; i < g_rx_owner_n; ++i)
                if (g_rx_owners[i] == owner) {
                    g_rx_owners[i] = g_rx_owners[--g_rx_owner_n];
                    break;
                }
        }
        apply();
        return KILN_ERR_UNAVAILABLE;
    }
    return KILN_OK;
}

KilnResult client_peer_stats(KilnReplPeerStats* out, uint32_t cap, uint32_t* count) {
    if (count) *count = 0;
    if (!g_resolved) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_world_thread(KILN_WORLD_CLIENT)) return KILN_ERR_WRONG_THREAD;
    if (!count || (cap && !out)) return KILN_ERR_INVALID_ARGUMENT;
    uint32_t rows = 0;
    for (uint32_t ch = 0; ch < 2; ++ch) {
        const RxCounters& r = g_rx_count[ch];
        const uint64_t dropped = ch == 0 ? g_ring.dropped() : 0;
        if (!r.messages && !dropped) continue;
        if (rows < cap) {
            KilnReplPeerStats& o = out[rows];
            if (o.size < sizeof(KilnReplPeerStats)) return KILN_ERR_INVALID_ARGUMENT;
            const uint32_t size = o.size;
            std::memset(&o, 0, sizeof o);
            o.size = size;
            o.peer = 0;  // the server this client is joined to (or hosts)
            o.channel = static_cast<uint8_t>(ch ? 5 : 4);
            o.flags = KILN_REPL_PEER_RECEIVED;
            o.frames = r.frames;
            o.messages = r.messages;
            o.bytes = r.bytes;
            o.creates = r.creates;
            o.updates = r.updates;
            o.destroy_lists = r.destroy_lists;
            o.destroyed_ids = r.destroyed_ids;
            o.dropped = dropped;
        }
        ++rows;
    }
    *count = rows;
    return rows > cap ? KILN_ERR_BUFFER_TOO_SMALL : KILN_OK;
}

KilnResult client_remove(KilnHandle h) {
    for (RxWatch& w : g_rx)
        if (w.handle.load(std::memory_order_acquire) == h) {
            if (!w.removed.exchange(true, std::memory_order_acq_rel)) g_dirty.store(true, std::memory_order_relaxed);
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

void client_owner_retiring(KilnOwner owner) {
    for (RxWatch& w : g_rx)
        if (w.handle.load(std::memory_order_acquire) && w.owner == owner) {
            w.removed.store(true, std::memory_order_release);
            g_dirty.store(true, std::memory_order_relaxed);
        }
    std::lock_guard lock(g_rx_owners_mu);
    for (uint32_t i = 0; i < g_rx_owner_n; ++i)
        if (g_rx_owners[i] == owner) {
            g_rx_owners[i] = g_rx_owners[--g_rx_owner_n];
            g_dirty.store(true, std::memory_order_relaxed);  // the client Pre lets the hooks go
            break;
        }
}

}  // namespace repl
