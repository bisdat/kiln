// SPDX-License-Identifier: MIT
// kiln.repl's send watch (kiln_repl.h watch / remove; the dossier's chapter 11 §4.1 "Server watch", §2.3, §2.6).
//
// A watch is the snapshot, no decoder and no mirror: what a peer is sent of an entity is the state the delta writer
// codes, so the DeltaWriter hook copies the watched entity's components out of that snapshot (offsets from the game's
// pure StateIndex, into a zeroed buffer on the job's stack) into the ring of its (machine, channel). A watch started
// mid-session therefore shows the next message's full values, never a delta. The EntityWriter hook (stats.cpp) says
// which entity the queue reader writes and, after the game's writer, whether a message went out: only then is the
// record committed, with the message's kind, tick, seq, baseline and payload size and the entity's template GUID. The
// SendCallback hook (stats.cpp) adds a record for a destroy list that names a watched entity in its list A.
// In the next Pre the tick thread reads the rings and calls each watch's fn: one event per message, then
// KILN_REPL_KIND_DESTROYED, then KILN_REPL_KIND_ENDED as the last event (destroyed, id reused for another template,
// its one peer left, the world stopped). Only a watched entity is copied; nothing allocates in a hook.
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstring>

#include "kiln_hook.hpp"
#include "repl.h"
#include "rings.h"
#include "wire.h"

namespace repl {
namespace {

constexpr uint32_t kMaxWatches = 64;
constexpr uintptr_t kRecordTemplate = 0x28;  // entity record: TemplateInfo* (its +0 is the template GUID, docs/re/spawn.md)
constexpr uintptr_t kCtxMsgs = 0x10, kCtxMsgCount = 0x18;  // a context's message list {MsgEntry* ptr; u64 count}

// ---- the watches (tick thread; remove() and an owner's revoke only set `removed`) ----
struct Watch {
    std::atomic<uint64_t> handle{0};  // 0: a free slot
    std::atomic<bool> removed{false};
    KilnOwner owner = 0;
    uint32_t entity = 0;
    uint32_t peer = KILN_REPL_ALL_MACHINES;  // or the one peer it is for
    uint32_t machines = 0;                    // bit per machine it copies for
    KilnReplWatchFn fn = nullptr;
    void* user = nullptr;
    bool have_guid = false;
    uint8_t guid[16] = {};
    bool ended = false;
    bool destroyed = false;  // a destroy record came this Pre: ENDED(destroyed) after the others
};
Watch g_w[kMaxWatches];
uint64_t g_serial = 0;
constexpr uint64_t kHandleTag = 0x5245ull << 48;  // "RE"

// What the hooks read: published on the tick thread (Pre, watch(), between frames).
struct Hot {
    std::atomic<uint32_t> n{0};
    std::atomic<uint32_t> entity[kMaxWatches];
    std::atomic<uint32_t> machines[kMaxWatches];
};
Hot g_hot;

// Per (machine, channel): the ring and the lab's cost counters of the delta hook.
struct alignas(64) WSlot {
    Ring ring;
    std::atomic<uint64_t> delta_calls{0}, delta_cycles{0}, records{0};
};
WSlot g_ws[KILN_REPL_MACHINES][2];
bool g_rings = false;  // tick thread

// ---- the DeltaWriter hook ----
Guard g_delta_guard{"watch-delta"};
kiln::RawHook h_delta;
bool g_delta_hooked = false;
using DeltaFn = uint64_t (*)(void* bits, uintptr_t registry, const uint8_t* snap, const uint8_t* base, uint64_t tmpl_flag,
                             void* base_ref, uint64_t pred);

// The queue reader's entity of this call, set by the EntityWriter hook only when it is watched for this machine.
struct Tls {
    int32_t machine = -1;
    uint8_t pred = 0;
    uint32_t entity = 0;
    uintptr_t ctx = 0;
    uint64_t msgs_before = 0;
    uint8_t* rec = nullptr;  // an open reservation in the (machine, channel)'s ring
    uint32_t rec_len = 0;
};
thread_local Tls t_w;

// Ring records.
enum : uint16_t { kRecWatch = 1, kRecDestroy = 2 };
struct WatchRec {
    uint32_t entity, tick, seq, baseline_tick, payload_bytes;
    uint8_t kind, pad[3];
    uint8_t guid[16];
    uint16_t count, pad2;
    uint32_t data_bytes;
};
struct RecComp {
    uint16_t index, size;
    uint32_t hash, offset;
    uint8_t custom, pad[3];
};
struct DestroyRec {
    uint32_t entity, tick, seq;
};
static_assert(sizeof(WatchRec) == 48 && sizeof(RecComp) == 16);

bool watched(uint32_t entity, uint32_t machine) {
    const uint32_t n = g_hot.n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
        if (g_hot.entity[i].load(std::memory_order_relaxed) == entity &&
            ((g_hot.machines[i].load(std::memory_order_relaxed) >> machine) & 1))
            return true;
    return false;
}

struct DeltaCall {
    const uint8_t* snap;
    uintptr_t registry;
};
void delta_before(void* p) {
    if (t_w.machine < 0) return;
    const uint64_t t0 = g_measure.load(std::memory_order_relaxed) ? __rdtsc() : 0;
    const auto* c = static_cast<DeltaCall*>(p);
    WSlot& ws = g_ws[t_w.machine][t_w.pred & 1];
    if (t_w.rec) {  // a second delta in one writer call: keep the newest
        ws.ring.abort();
        t_w.rec = nullptr;
    }
    const Registry* reg = registry_hot();
    if (!c->snap || !reg || !c->registry) return;
    Part parts[kMaxComponents / 4];
    uint32_t n = 0;
    if (!state_parts(*reg, c->snap, [&](uint8_t* index, const uint8_t* st) { return state_index(index, st, c->registry); }, parts,
                     static_cast<uint32_t>(std::size(parts)), &n))
        return;
    uint32_t data = 0;
    for (uint32_t k = 0; k < n; ++k) data += parts[k].size;
    const uint32_t len = static_cast<uint32_t>(sizeof(WatchRec) + n * sizeof(RecComp)) + data;
    uint8_t* rec = ws.ring.reserve(kRecWatch, len);
    if (!rec) return;  // full: dropped and counted; the next record carries gap
    WatchRec h{};
    h.entity = t_w.entity;
    h.count = static_cast<uint16_t>(n);
    h.data_bytes = data;
    auto* comps = reinterpret_cast<RecComp*>(rec + sizeof(WatchRec));
    uint8_t* out = rec + sizeof(WatchRec) + n * sizeof(RecComp);
    uint32_t off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const ComponentFacts& f = reg->at[parts[k].index];
        RecComp rc{parts[k].index, parts[k].size, f.hash, off, static_cast<uint8_t>((f.flags & KILN_REPL_COMPONENT_CUSTOM_CODEC) ? 1 : 0), {}};
        std::memcpy(&comps[k], &rc, sizeof rc);
        std::memcpy(out + off, c->snap + parts[k].off, parts[k].size);
        off += parts[k].size;
    }
    std::memcpy(rec, &h, sizeof h);
    t_w.rec = rec;
    t_w.rec_len = len;
    if (t0) {
        ws.delta_calls.fetch_add(1, std::memory_order_relaxed);
        ws.delta_cycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    }
}
uint64_t d_delta(void* bits, uintptr_t registry, const uint8_t* snap, const uint8_t* base, uint64_t tmpl_flag, void* base_ref,
                 uint64_t pred) {
    DeltaCall c{snap, registry};
    return detour(g_delta_guard, &c, &delta_before, nullptr, h_delta.original<DeltaFn>(), bits, registry, snap, base, tmpl_flag,
                  base_ref, pred);
}

// ---- delivery (tick thread) ----
// fn(e, user) under a fault guard: a callback that faults ends its watch instead of the server.
bool call_guarded(KilnReplWatchFn fn, const KilnReplWatchEvent* e, void* user) {
    __try {
        fn(e, user);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void deliver(Watch& w, const KilnReplWatchEvent& e) {
    if (w.ended || w.removed.load(std::memory_order_relaxed)) return;
    if (!call_guarded(w.fn, &e, w.user)) {
        logf(KILN_LOG_WARN, "repl: a watch callback of entity %u faulted; the watch ends", w.entity);
        w.ended = true;
    }
}

void end(Watch& w, uint32_t reason) {
    if (w.ended || w.removed.load(std::memory_order_relaxed)) return;
    KilnReplWatchEvent e{};
    e.size = sizeof e;
    e.direction = KILN_REPL_SEND;
    e.kind = KILN_REPL_KIND_ENDED;
    e.entity = w.entity;
    e.reason = reason;
    deliver(w, e);
    w.ended = true;
}

KilnReplComponent g_comps[kMaxComponents];  // the event's component list (tick thread)

void deliver_record(uint32_t m, uint32_t ch, const RecordHeader& rh, const uint8_t* p) {
    uint8_t flags = 0;
    const uint32_t peer = peer_of(m, &flags);
    if (rh.type == kRecDestroy && rh.len >= sizeof(DestroyRec)) {
        DestroyRec d;
        std::memcpy(&d, p, sizeof d);
        for (Watch& w : g_w) {
            if (!w.handle.load(std::memory_order_relaxed) || w.entity != d.entity || !((w.machines >> m) & 1)) continue;
            KilnReplWatchEvent e{};
            e.size = sizeof e;
            e.peer = peer;
            e.direction = KILN_REPL_SEND;
            e.channel = static_cast<uint8_t>(ch ? 5 : 4);
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
    if (rh.type != kRecWatch || rh.len < sizeof(WatchRec)) return;
    WatchRec h;
    std::memcpy(&h, p, sizeof h);
    if (sizeof(WatchRec) + uint64_t{h.count} * sizeof(RecComp) + h.data_bytes > rh.len || h.count > kMaxComponents) return;
    const auto* comps = p + sizeof(WatchRec);
    const uint8_t* data = comps + h.count * sizeof(RecComp);
    for (uint32_t k = 0; k < h.count; ++k) {
        RecComp rc;
        std::memcpy(&rc, comps + k * sizeof(RecComp), sizeof rc);
        if (rc.offset + uint64_t{rc.size} > h.data_bytes) return;
        g_comps[k] = KilnReplComponent{rc.index, rc.size, rc.hash, rc.custom, {}, data + rc.offset};
    }
    static constexpr uint8_t kNoGuid[16] = {};
    const bool guid_known = std::memcmp(h.guid, kNoGuid, 16) != 0;  // 0: the template could not be read for this record
    for (Watch& w : g_w) {
        if (!w.handle.load(std::memory_order_relaxed) || w.ended || w.entity != h.entity || !((w.machines >> m) & 1)) continue;
        if (!guid_known) {
        } else if (!w.have_guid) {
            std::memcpy(w.guid, h.guid, 16);
            w.have_guid = true;
        } else if (std::memcmp(w.guid, h.guid, 16) != 0) {  // the id belongs to an entity of another template now
            end(w, KILN_REPL_END_ID_REUSED);
            continue;
        }
        KilnReplWatchEvent e{};
        e.size = sizeof e;
        e.peer = peer;
        e.direction = KILN_REPL_SEND;
        e.channel = static_cast<uint8_t>(ch ? 5 : 4);
        e.kind = h.kind;
        e.gap = rh.gap;
        e.entity = h.entity;
        e.tick = h.tick;
        e.seq = h.seq;
        e.baseline_tick = h.baseline_tick;
        e.payload_bytes = h.payload_bytes;
        e.count = h.count;
        e.components = g_comps;
        deliver(w, e);
    }
}

void free_rings() {
    for (auto& m : g_ws)
        for (WSlot& s : m) s.ring.release();
    g_rings = false;
}

bool alloc_rings() {
    if (g_rings) return true;
    for (auto& m : g_ws)
        for (WSlot& s : m)
            if (!s.ring.init()) {
                free_rings();
                return false;
            }
    g_rings = true;
    return true;
}

}  // namespace

// ---- the EntityWriter and SendCallback hooks' watch part (stats.cpp calls these inside its guard) ----
bool watch_active() { return g_hot.n.load(std::memory_order_relaxed) != 0; }

void watch_writer_before(const uint8_t* args, uintptr_t ctx, int32_t machine, uint8_t pred) {
    t_w.machine = -1;
    t_w.rec = nullptr;
    if (machine < 0) return;
    const uintptr_t entry = *reinterpret_cast<const uintptr_t*>(args + kWriterEntry);
    const uint32_t entity = *reinterpret_cast<const uint32_t*>(entry);
    if (!watched(entity, static_cast<uint32_t>(machine))) return;
    t_w.entity = entity;
    t_w.pred = pred;
    t_w.ctx = ctx;
    t_w.msgs_before = *reinterpret_cast<const uint64_t*>(ctx + kCtxMsgCount);
    t_w.machine = machine;
}

void watch_writer_after(const WriterOut* out, uint64_t result) {
    if (t_w.machine < 0) return;
    WSlot& ws = g_ws[t_w.machine][t_w.pred & 1];
    uint8_t* rec = t_w.rec;
    t_w.machine = -1;
    t_w.rec = nullptr;
    if (!rec) return;
    const uint64_t count = *reinterpret_cast<const uint64_t*>(t_w.ctx + kCtxMsgCount);
    const auto* msgs = *reinterpret_cast<const MsgEntry* const*>(t_w.ctx + kCtxMsgs);
    Header hd{};
    if (!result || !out || out->skipped || count != t_w.msgs_before + 1 || !msgs || msgs[count - 1].type != kMsgEntity ||
        !parse_header(msgs[count - 1].data, msgs[count - 1].size, &hd) || hd.entity != t_w.entity) {
        ws.ring.abort();  // no message went out (unchanged, too big, gone): nothing to show
        return;
    }
    WatchRec h;
    std::memcpy(&h, rec, sizeof h);
    h.kind = static_cast<uint8_t>(hd.kind);
    h.tick = hd.tick;
    h.seq = hd.seq;
    h.baseline_tick = hd.kind == kKindUpdate ? hd.base : 0;
    h.payload_bytes = hd.payload_len;
    const uintptr_t ti = out->record ? *reinterpret_cast<const uintptr_t*>(out->record + kRecordTemplate) : 0;
    if (ti) std::memcpy(h.guid, reinterpret_cast<const void*>(ti), 16);
    else if (hd.has_guid) std::memcpy(h.guid, hd.guid, 16);
    std::memcpy(rec, &h, sizeof h);
    ws.ring.commit(t_w.rec_len);
    ws.records.fetch_add(1, std::memory_order_relaxed);
}

void watch_callback(const Batch* b) {
    if (b->machine >= KILN_REPL_MACHINES) return;
    WSlot& ws = g_ws[b->machine][b->pred & 1];
    const uint32_t ok = b->sent < b->count ? b->sent : b->count;
    for (uint32_t i = 0; i < ok; ++i) {
        const MsgEntry& m = b->msgs[i];
        DestroyList l;
        if (m.type != kMsgList || !m.data || !parse_destroy(m.data, m.size, &l)) continue;
        for (uint32_t k = 0; k < l.na; ++k) {  // list A: destroyed (list B: left this peer's relevance)
            const uint32_t id = u32_at(l.ids + 4 * k);
            if (!watched(id, b->machine)) continue;
            DestroyRec d{id, l.tick, l.seq};
            if (ws.ring.push(kRecDestroy, &d, sizeof d)) ws.records.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---- the service's side (tick thread) ----
void watch_init() { add_guard(&g_delta_guard); }

// Every Pre: the last frame's records to their watches (before the peer snapshot moves on).
void watch_deliver() {
    if (!g_rings) return;
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m)
        for (uint32_t ch = 0; ch < 2; ++ch)
            g_ws[m][ch].ring.drain([&](const RecordHeader& rh, const uint8_t* p) { deliver_record(m, ch, rh, p); });
    for (Watch& w : g_w)
        if (w.destroyed) {
            w.destroyed = false;
            end(w, KILN_REPL_END_DESTROYED);
        }
}

// The hot list, the rings and the delta hook follow the live watches; removed ones free their slots.
void watch_apply() {
    uint32_t n = 0;
    for (Watch& w : g_w) {
        if (!w.handle.load(std::memory_order_relaxed)) continue;
        if (w.removed.load(std::memory_order_acquire)) {
            w.handle.store(0, std::memory_order_release);
            w.removed.store(false, std::memory_order_relaxed);
            continue;
        }
        if (w.ended) continue;
        g_hot.entity[n].store(w.entity, std::memory_order_relaxed);
        g_hot.machines[n].store(w.machines, std::memory_order_relaxed);
        ++n;
    }
    g_hot.n.store(n, std::memory_order_release);
    if (n && !g_delta_hooked) {
        if (!alloc_rings()) {
            logf(KILN_LOG_WARN, "repl: watches have no rings (out of memory): no events");
            g_hot.n.store(0, std::memory_order_release);
            return;
        }
        if (!h_delta.install(g_api, g_owner, F.delta_writer, reinterpret_cast<void*>(&d_delta), "repl/watch DeltaWriter")) {
            logf(KILN_LOG_WARN, "repl: the watch hook could not be installed (%s): no events", h_delta.error() ? h_delta.error() : "?");
            g_hot.n.store(0, std::memory_order_release);
            return;
        }
        g_delta_hooked = true;
        logf(KILN_LOG_INFO, "repl: watch hooks installed");
    } else if (!n && g_delta_hooked) {
        h_delta.revoke();
        g_delta_hooked = false;
        bool any_slot = false;
        for (Watch& w : g_w) any_slot = any_slot || w.handle.load(std::memory_order_relaxed) != 0;
        if (!any_slot) free_rings();  // an ended watch not removed yet keeps nothing to read
        logf(KILN_LOG_INFO, "repl: watch hooks removed (no live watch)");
    }
    stats_apply_hooks();  // the writer and callback hooks serve the watches too
}

void watch_peer_changed(uint32_t machine, uint32_t old_peer) {
    if (machine >= KILN_REPL_MACHINES) return;
    if (g_rings)
        for (WSlot& s : g_ws[machine]) {
            s.ring.discard();
            s.ring.clear_counters();
        }
    if (!old_peer) return;
    for (Watch& w : g_w)
        if (w.handle.load(std::memory_order_relaxed) && w.peer == old_peer) end(w, KILN_REPL_END_PEER_LEFT);
}

void watch_world_stopping() {
    for (Watch& w : g_w)
        if (w.handle.load(std::memory_order_relaxed)) end(w, KILN_REPL_END_WORLD_STOPPED);
    if (g_rings)
        for (auto& m : g_ws)
            for (WSlot& s : m) s.ring.discard();
    watch_apply();
}

// An owner revoked (any thread): its watches go silently.
void watch_owner_retiring(KilnOwner owner) {
    for (Watch& w : g_w)
        if (w.handle.load(std::memory_order_acquire) && w.owner == owner) w.removed.store(true, std::memory_order_release);
}

uint64_t watch_dropped(uint32_t machine, uint32_t ch) { return g_rings ? g_ws[machine][ch].ring.dropped() : 0; }

void watch_cost(WatchCost* out) {
    for (auto& m : g_ws)
        for (WSlot& s : m) {
            out->delta_calls += s.delta_calls.load(std::memory_order_relaxed);
            out->delta_cycles += s.delta_cycles.load(std::memory_order_relaxed);
            out->records += s.records.load(std::memory_order_relaxed);
            out->dropped += s.ring.dropped();
        }
}

void watch_cost_reset() {
    for (auto& m : g_ws)
        for (WSlot& s : m) {
            s.delta_calls.store(0, std::memory_order_relaxed);
            s.delta_cycles.store(0, std::memory_order_relaxed);
            s.records.store(0, std::memory_order_relaxed);
        }
}

KilnResult KILN_CALL t_watch(KilnOwner owner, KilnEntity e, uint32_t peer_or_all, uint32_t direction, KilnReplWatchFn fn,
                             void* user, KilnHandle* out) {
    if (out) *out = 0;
    if (direction == KILN_REPL_RECEIVE) return KILN_ERR_UNAVAILABLE;  // the client side (S9)
    if (!observe_on()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!e || !fn || !out || direction != KILN_REPL_SEND) return KILN_ERR_INVALID_ARGUMENT;
    uint32_t machines = 0;
    if (peer_or_all == KILN_REPL_ALL_MACHINES) {
        machines = (1u << KILN_REPL_MACHINES) - 1;
    } else {
        for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
            uint8_t flags = 0;
            if (peer_or_all && peer_of(m, &flags) == peer_or_all) machines |= 1u << m;
        }
        if (!machines) return KILN_ERR_INVALID_ARGUMENT;  // not connected
    }
    Watch* slot = nullptr;
    for (Watch& w : g_w)
        if (!w.handle.load(std::memory_order_relaxed)) {
            slot = &w;
            break;
        }
    if (!slot) return KILN_ERR_INVALID_ARGUMENT;  // 64 watches
    slot->owner = owner;
    slot->entity = static_cast<uint32_t>(e);
    slot->peer = peer_or_all;
    slot->machines = machines;
    slot->fn = fn;
    slot->user = user;
    slot->have_guid = false;
    slot->ended = false;
    slot->destroyed = false;
    slot->removed.store(false, std::memory_order_relaxed);
    const KilnHandle h = kHandleTag | ++g_serial;
    slot->handle.store(h, std::memory_order_release);
    watch_apply();
    if (!g_delta_hooked) {  // no hook: nothing will come
        slot->handle.store(0, std::memory_order_release);
        watch_apply();
        return KILN_ERR_UNAVAILABLE;
    }
    *out = h;
    return KILN_OK;
}

KilnResult KILN_CALL t_remove(KilnHandle h) {
    if (!h) return KILN_ERR_INVALID_ARGUMENT;
    for (Watch& w : g_w)
        if (w.handle.load(std::memory_order_acquire) == h) {
            w.removed.store(true, std::memory_order_release);
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

}  // namespace repl
