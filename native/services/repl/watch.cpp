// SPDX-License-Identifier: MIT
// kiln.repl's send watch and capture (kiln_repl.h watch / remove / capture; the dossier's chapter 11 §4.1 "Server watch",
// §2.3, §2.6, §5.2 "Surfaces").
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
//
// A capture is the same for every entity one peer is sent, for a few seconds: the snapshot of each entity message, and
// (SendCallback) the bytes of every message the session accepted for that peer. The tick thread writes them as JSON
// lines (tools/replinspect reads them) into kiln/logs.
#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

#include "kiln_hook.hpp"
#include "repl.h"
#include "rings.h"
#include "wire.h"

namespace repl {
namespace {

constexpr uint32_t kMaxWatches = 64;
constexpr uintptr_t kRecordTemplate = 0x28;  // entity record: TemplateInfo* (its +0 is the template GUID, docs/re/spawn.md)

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
std::atomic<bool> g_dirty{false};                 // a watch or the capture was removed off the tick thread: Pre applies it

// What the hooks read: published on the tick thread (Pre, watch(), capture(), between frames).
struct Hot {
    std::atomic<uint32_t> n{0};
    std::atomic<uint32_t> entity[kMaxWatches];
    std::atomic<uint32_t> machines[kMaxWatches];
    std::atomic<uint32_t> capture{0};  // bit per machine: every entity's snapshot and every message's bytes
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
    uint8_t entry_kind = 0;  // the queue reader's ask: 0 create, 1 and 2 update
    uint32_t entity = 0, tick = 0, seq = 0;
    WSlot* open = nullptr;   // the slot whose ring this writer call may hold a reservation in
    uint8_t* rec = nullptr;  // that reservation
    uint32_t rec_len = 0;
};
thread_local Tls t_w;

// Ring records.
enum : uint16_t { kRecWatch = 1, kRecDestroy = 2, kRecRaw = 3, kRecMeta = 4 };
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
    uint32_t entity, tick, seq, raw;  // raw: the machine slot's peer handle when the callback ran
};
struct RawRec {  // then `size` bytes: a whole message as the session took it (capture)
    uint32_t seq, size, raw;
    uint8_t type, pad[3];
};
// The send callback's word on an entity message of a watched entity: the session took it, with this header. A snapshot
// record (kRecWatch, written in the writer call, where the message's bytes are not final yet) is delivered only with
// its meta: a message the session refused was not sent.
struct MetaRec {
    uint32_t seq, entity, kind, tick, base, payload_bytes;
    uint32_t raw;  // the machine slot's peer handle when the callback ran (a slot reused within one frame is a new peer)
};
static_assert(sizeof(WatchRec) == 48 && sizeof(RecComp) == 16 && sizeof(RawRec) == 16 && sizeof(MetaRec) == 28 &&
              sizeof(DestroyRec) == 16);

bool watched_list(uint32_t entity, uint32_t machine) {
    const uint32_t n = g_hot.n.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n; ++i)
        if (g_hot.entity[i].load(std::memory_order_relaxed) == entity &&
            ((g_hot.machines[i].load(std::memory_order_relaxed) >> machine) & 1))
            return true;
    return false;
}
bool captured(uint32_t machine) { return (g_hot.capture.load(std::memory_order_acquire) >> machine) & 1; }
bool watched(uint32_t entity, uint32_t machine) { return captured(machine) || watched_list(entity, machine); }

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
    if (!rec) return;  // full or held by another producer: dropped and counted; the next record carries gap
    t_w.open = &ws;    // ours from here: a fault below still lets the writer's end give it back (never another's)
    WatchRec h{};
    h.entity = t_w.entity;
    h.count = static_cast<uint16_t>(n);
    h.data_bytes = data;
    auto* comps = rec + sizeof(WatchRec);
    uint8_t* out = rec + sizeof(WatchRec) + n * sizeof(RecComp);
    uint32_t off = 0;
    for (uint32_t k = 0; k < n; ++k) {
        const ComponentFacts& f = reg->at[parts[k].index];
        RecComp rc{parts[k].index, parts[k].size, f.hash, off, static_cast<uint8_t>((f.flags & KILN_REPL_COMPONENT_CUSTOM_CODEC) ? 1 : 0), {}};
        std::memcpy(comps + k * sizeof(RecComp), &rc, sizeof rc);
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
    // ENDED is its last event: the slot goes back at the next apply (a remove() after it is not needed)
    w.removed.store(true, std::memory_order_release);
    g_dirty.store(true, std::memory_order_relaxed);
}

KilnReplComponent g_comps[kMaxComponents];  // the event's component list (tick thread)

// ---- the capture (tick thread) ----
struct CaptureState {
    std::FILE* f = nullptr;
    KilnOwner owner = 0;
    uint32_t peer = 0, machines = 0;
    uint64_t end_ms = 0;  // GetTickCount64() deadline
    uint64_t messages = 0, dropped_before = 0, bytes = 0;
    uint32_t pending = 0;  // machines while t_capture installs the hooks (before the file exists)
    uint32_t pres = 0;
    std::atomic<bool> removed{false};  // its owner went (any thread)
    std::string path;
};
CaptureState g_cap;
constexpr uint64_t kCaptureMaxBytes = 256ull << 20;  // a capture file ends at 256 MiB
// The records of the (machine, channel) being drained: delivered once its whole frame is read, so a snapshot record meets
// the callback's meta (and a capture line its snapshot) by seq. They stay valid until the next send step.
struct Item {
    uint16_t type;
    uint8_t gap;
    const uint8_t* p;
    uint32_t len;
};
constexpr uint32_t kItems = 4096;  // a ring holds at most 2048 records
Item g_items[kItems];
uint32_t g_item_n = 0;

// The drained frame's metas and snapshot records by seq: open addressing, emptied by a generation number (no clearing).
constexpr uint32_t kIndex = 8192;  // a power of two, twice kItems
struct IndexSlot {
    uint32_t gen, seq;
    uint32_t item;
};
IndexSlot g_meta_ix[kIndex], g_snap_ix[kIndex];
uint32_t g_ix_gen = 0;
void index_put(IndexSlot* ix, uint32_t seq, uint32_t item) {
    for (uint32_t i = (seq * 2654435761u) & (kIndex - 1), probes = 0; probes < kIndex; ++probes, i = (i + 1) & (kIndex - 1)) {
        if (ix[i].gen != g_ix_gen) {
            ix[i] = IndexSlot{g_ix_gen, seq, item};
            return;
        }
        if (ix[i].seq == seq) return;  // the first record of a seq wins
    }
}
const Item* index_get(const IndexSlot* ix, uint32_t seq) {
    for (uint32_t i = (seq * 2654435761u) & (kIndex - 1), probes = 0; probes < kIndex; ++probes, i = (i + 1) & (kIndex - 1)) {
        if (ix[i].gen != g_ix_gen) return nullptr;
        if (ix[i].seq == seq) return &g_items[ix[i].item];
    }
    return nullptr;
}
void index_items() {
    if (++g_ix_gen == 0) ++g_ix_gen;
    for (uint32_t i = 0; i < g_item_n; ++i) {
        uint32_t seq = 0;
        if (g_items[i].type == kRecMeta && g_items[i].len >= sizeof(MetaRec)) {
            std::memcpy(&seq, g_items[i].p + offsetof(MetaRec, seq), 4);
            index_put(g_meta_ix, seq, i);
        } else if (g_items[i].type == kRecWatch && g_items[i].len >= sizeof(WatchRec)) {
            std::memcpy(&seq, g_items[i].p + offsetof(WatchRec, seq), 4);
            index_put(g_snap_ix, seq, i);
        }
    }
}
bool find_meta(uint32_t seq, MetaRec* out) {
    const Item* it = index_get(g_meta_ix, seq);
    if (!it) return false;
    std::memcpy(out, it->p, sizeof *out);
    return true;
}
const Item* find_snapshot(uint32_t seq) { return index_get(g_snap_ix, seq); }

uint64_t utc_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return t / 10000 - 11644473600000ull;
}
std::string utc_iso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char b[40];
    std::snprintf(b, sizeof b, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                  st.wMilliseconds);
    return b;
}
void hex(std::string& out, const uint8_t* p, uint64_t n) {
    static constexpr char k[] = "0123456789abcdef";
    const size_t at = out.size();
    out.resize(at + n * 2);
    for (uint64_t i = 0; i < n; ++i) {
        out[at + 2 * i] = k[p[i] >> 4];
        out[at + 2 * i + 1] = k[p[i] & 15];
    }
}
void json_str(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out += c;
    }
    out += '"';
}
uint64_t capture_ring_drops() {
    uint64_t d = 0;
    if (g_rings)
        for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m)
            if ((g_cap.machines >> m) & 1) d += g_ws[m][0].ring.dropped() + g_ws[m][1].ring.dropped();
    return d;
}

void capture_end(const char* why) {
    if (!g_cap.f) return;
    const uint64_t drops_now = capture_ring_drops();
    const uint64_t dropped = drops_now >= g_cap.dropped_before ? drops_now - g_cap.dropped_before : drops_now;
    std::string line = "{\"type\":\"end\",\"messages\":" + std::to_string(g_cap.messages) + ",\"dropped\":" + std::to_string(dropped) +
                       ",\"utc\":\"" + utc_iso() + "\",\"reason\":";
    json_str(line, why);
    line += "}\n";
    std::fputs(line.c_str(), g_cap.f);
    std::fclose(g_cap.f);
    g_cap.f = nullptr;
    logf(KILN_LOG_INFO, "repl: capture of peer 0x%x ended (%s): %llu messages, %llu records dropped; %s", g_cap.peer, why,
         static_cast<unsigned long long>(g_cap.messages), static_cast<unsigned long long>(dropped), g_cap.path.c_str());
    g_cap.machines = 0;
    g_cap.owner = 0;
    g_cap.bytes = 0;
    g_cap.removed.store(false, std::memory_order_relaxed);
    g_dirty.store(true, std::memory_order_relaxed);
}

// One line per message the captured peer's session took: its bytes and, for an entity message, the snapshot it coded.
void capture_line(uint32_t m, uint32_t ch, const RecordHeader& rh, const uint8_t* p) {
    if (!g_cap.f || !((g_cap.machines >> m) & 1) || rh.len < sizeof(RawRec)) return;
    RawRec r;
    std::memcpy(&r, p, sizeof r);
    if (sizeof(RawRec) + uint64_t{r.size} > rh.len) return;
    const uint8_t* d = p + sizeof(RawRec);
    uint8_t flags = 0;
    const uint32_t peer = peer_at(m, r.raw, &flags);
    std::string line;
    line.reserve(256 + r.size * 2);
    line += "{\"type\":\"msg\",\"t\":" + std::to_string(utc_ms()) + ",\"peer\":" + std::to_string(peer) + ",\"ch\":" +
            std::to_string(ch ? 5 : 4) + ",\"msg\":" + std::to_string(r.type) + ",\"kind\":";
    uint32_t entity = 0, tick = 0, base = 0;
    const uint8_t* snap = nullptr;
    uint32_t snap_len = 0;
    std::string extra;
    if (r.type == kMsgEntity) {
        Header h{};
        if (parse_header(d, r.size, &h)) {
            line += h.kind == kKindCreate ? "\"create\"" : "\"update\"";
            entity = h.entity;
            tick = h.tick;
            base = h.kind == kKindUpdate ? h.base : 0;
            if (const Item* it = find_snapshot(r.seq)) {
                snap = it->p;
                snap_len = it->len;
            }
        } else {
            line += "\"entity\"";
        }
    } else if (r.type == kMsgList) {
        line += "\"destroy\"";
        DestroyList l;
        if (parse_destroy(d, r.size, &l)) {
            tick = l.tick;
            extra += ",\"a\":[";
            for (uint32_t k = 0; k < l.na; ++k) extra += (k ? "," : "") + std::to_string(u32_at(l.ids + 4 * k));
            extra += "],\"b\":[";
            for (uint32_t k = 0; k < l.nb; ++k) extra += (k ? "," : "") + std::to_string(u32_at(l.ids + 4 * (l.na + k)));
            extra += "]";
        }
    } else {
        line += r.type == kMsgHandshake ? "\"handshake\"" : "\"frame\"";
        if (r.type == kMsgFrame && r.size >= 4) tick = u32_at(d) >> 3;
    }
    line += ",\"entity\":" + std::to_string(entity) + ",\"tick\":" + std::to_string(tick) + ",\"seq\":" + std::to_string(r.seq) +
            ",\"base\":" + std::to_string(base) + ",\"bytes\":" + std::to_string(r.size) + ",\"gap\":" + (rh.gap ? "true" : "false");
    line += extra;
    const Registry* reg = registry();
    if (snap && reg && snap_len >= sizeof(WatchRec)) {
        WatchRec h;
        std::memcpy(&h, snap, sizeof h);
        if (sizeof(WatchRec) + uint64_t{h.count} * sizeof(RecComp) + h.data_bytes <= snap_len) {
            const uint8_t* comps = snap + sizeof(WatchRec);
            const uint8_t* data = comps + h.count * sizeof(RecComp);
            line += ",\"components\":{";
            for (uint32_t k = 0; k < h.count; ++k) {
                RecComp rc;
                std::memcpy(&rc, comps + k * sizeof(RecComp), sizeof rc);
                if (rc.offset + uint64_t{rc.size} > h.data_bytes) break;
                if (k) line += ',';
                json_str(line, rc.index < reg->count ? reg->names[rc.index] : "#" + std::to_string(rc.index));
                line += ":\"";
                hex(line, data + rc.offset, rc.size);
                line += '"';
            }
            line += '}';
        }
    }
    line += ",\"raw\":\"";
    hex(line, d, r.size);
    line += "\"}\n";
    std::fwrite(line.data(), 1, line.size(), g_cap.f);
    ++g_cap.messages;
    g_cap.bytes += line.size();
    if (g_cap.bytes >= kCaptureMaxBytes) capture_end("size cap (256 MiB)");
}

void deliver_record(uint32_t m, uint32_t ch, const RecordHeader& rh, const uint8_t* p) {
    if (rh.type == kRecRaw) {
        capture_line(m, ch, rh, p);
        return;
    }
    if (rh.type == kRecMeta) return;  // read with its snapshot record
    if (rh.type == kRecDestroy && rh.len >= sizeof(DestroyRec)) {
        DestroyRec d;
        std::memcpy(&d, p, sizeof d);
        uint8_t flags = 0;
        const uint32_t peer = peer_at(m, d.raw, &flags);
        for (Watch& w : g_w) {
            if (!w.handle.load(std::memory_order_relaxed) || w.entity != d.entity || !((w.machines >> m) & 1)) continue;
            if (w.peer != KILN_REPL_ALL_MACHINES && w.peer != peer) continue;  // the slot's new peer, not its old one
            KilnReplWatchEvent e{};
            e.size = sizeof e;
            e.peer = peer;
            e.peer_flags = flags;
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
    bool any = false;  // a capture's own snapshot records interest no watch: the cheap test first
    for (Watch& w : g_w)
        any = any || (w.handle.load(std::memory_order_relaxed) && !w.ended && w.entity == h.entity && ((w.machines >> m) & 1));
    if (!any) return;
    MetaRec meta{};
    if (!find_meta(h.seq, &meta) || meta.entity != h.entity) return;  // the session refused it, or it is not this message
    h.kind = static_cast<uint8_t>(meta.kind);
    h.tick = meta.tick;
    h.baseline_tick = meta.base;
    h.payload_bytes = meta.payload_bytes;
    uint8_t flags = 0;
    const uint32_t peer = peer_at(m, meta.raw, &flags);
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
        if (w.peer != KILN_REPL_ALL_MACHINES && w.peer != peer) continue;  // the slot's new peer, not its old one
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
        e.peer_flags = flags;
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
bool watch_active() { return g_hot.n.load(std::memory_order_relaxed) != 0 || g_hot.capture.load(std::memory_order_relaxed) != 0; }

void watch_writer_before(const uint8_t* args, uintptr_t ctx, int32_t machine, uint8_t pred) {
    t_w.machine = -1;
    t_w.rec = nullptr;
    t_w.open = nullptr;
    if (machine < 0 || !watch_active()) return;
    const uintptr_t entry = *reinterpret_cast<const uintptr_t*>(args + kWriterEntry);
    const uint32_t entity = *reinterpret_cast<const uint32_t*>(entry);
    if (!watched(entity, static_cast<uint32_t>(machine))) return;
    (void)ctx;
    t_w.entity = entity;
    t_w.entry_kind = *reinterpret_cast<const uint8_t*>(entry + 4);
    t_w.tick = *reinterpret_cast<const uint32_t*>(args + kWriterTick);
    t_w.seq = *reinterpret_cast<const uint32_t*>(args + kWriterSeq);
    t_w.pred = pred;
    t_w.machine = machine;
}

void watch_writer_after(const WriterOut* out, uint64_t result) {
    if (t_w.machine < 0) return;
    WSlot& ws = g_ws[t_w.machine][t_w.pred & 1];
    uint8_t* rec = t_w.rec;
    t_w.machine = -1;
    t_w.rec = nullptr;
    t_w.open = nullptr;
    if (!rec) return;
    // §2.6: true and not skipped = a message was appended for this entity (its bytes are final only at the callback)
    if (!(result & 0xff) || !out || out->skipped) {
        ws.ring.abort();  // no message went out (unchanged, too big, gone): nothing to show
        return;
    }
    WatchRec h;
    std::memcpy(&h, rec, sizeof h);
    h.kind = static_cast<uint8_t>(t_w.entry_kind == 0 ? kKindCreate : kKindUpdate);  // the callback's meta has the header's
    h.tick = t_w.tick;
    h.seq = t_w.seq;
    const uintptr_t ti = out->record ? *reinterpret_cast<const uintptr_t*>(out->record + kRecordTemplate) : 0;
    if (ti) std::memcpy(h.guid, reinterpret_cast<const void*>(ti), 16);
    std::memcpy(rec, &h, sizeof h);
    ws.ring.commit(t_w.rec_len);
    ws.records.fetch_add(1, std::memory_order_relaxed);
}

// After every writer call, outside any guard (plain stores to kiln.repl's own memory): a reservation an observer left
// open (it faulted, or its guard is off) is given back, so a ring never stays held.
void watch_writer_done() {
    if (t_w.open && t_w.open->ring.reserved()) t_w.open->ring.abort();
    t_w.open = nullptr;
    t_w.rec = nullptr;
    t_w.machine = -1;
}

void watch_callback(const Batch* b, uintptr_t gs) {
    if (b->machine >= KILN_REPL_MACHINES || !b->msgs) return;
    WSlot& ws = g_ws[b->machine][b->pred & 1];
    // the peer handle in the machine's slot now (inside the hook's guard): a slot reused within one frame is a new peer
    const uint32_t raw = gs && L.gs_machines >= 0
                             ? *reinterpret_cast<const uint32_t*>(gs + static_cast<uintptr_t>(L.gs_machines) + b->machine * kMachineSlotStride)
                             : 0;
    const bool cap = captured(b->machine);
    const uint32_t ok = b->sent < b->count ? b->sent : b->count;
    for (uint32_t i = 0; i < ok; ++i) {
        const MsgEntry& m = b->msgs[i];
        if (!m.data) continue;
        if (cap && m.size <= 0xffffu) {
            uint8_t* r = ws.ring.reserve(kRecRaw, static_cast<uint32_t>(sizeof(RawRec) + m.size));
            if (r) {
                const RawRec h{m.seq, static_cast<uint32_t>(m.size), raw, m.type, {}};
                std::memcpy(r, &h, sizeof h);
                std::memcpy(r + sizeof h, m.data, m.size);
                ws.ring.commit(static_cast<uint32_t>(sizeof(RawRec) + m.size));
                ws.records.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (m.type == kMsgEntity) {  // the header of a watched entity's message, for its snapshot record
            Header hd{};
            if (!parse_header(m.data, m.size, &hd) || !watched(hd.entity, b->machine)) continue;
            const MetaRec meta{hd.seq, hd.entity, hd.kind, hd.tick, hd.kind == kKindUpdate ? hd.base : 0, hd.payload_len, raw};
            ws.ring.push(kRecMeta, &meta, sizeof meta);
            continue;
        }
        DestroyList l;
        if (m.type != kMsgList || !parse_destroy(m.data, m.size, &l)) continue;
        for (uint32_t k = 0; k < l.na; ++k) {  // list A: destroyed (list B: left this peer's relevance)
            const uint32_t id = u32_at(l.ids + 4 * k);
            if (!watched_list(id, b->machine)) continue;
            DestroyRec d{id, l.tick, l.seq, raw};
            if (ws.ring.push(kRecDestroy, &d, sizeof d)) ws.records.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---- the service's side (tick thread) ----
void watch_init() { add_guard(&g_delta_guard); }

// Every Pre: the last frame's records to their watches and the capture file (before the peer snapshot moves on).
void watch_deliver() {
    if (g_rings)
        for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m)
            for (uint32_t ch = 0; ch < 2; ++ch) {
                g_item_n = 0;
                g_ws[m][ch].ring.drain([&](const RecordHeader& rh, const uint8_t* p) {
                    if (g_item_n < kItems) g_items[g_item_n++] = Item{rh.type, rh.gap, p, rh.len};
                });
                if (!g_item_n) continue;
                index_items();
                for (uint32_t i = 0; i < g_item_n; ++i) {
                    const RecordHeader rh{g_items[i].len, g_items[i].type, g_items[i].gap, 0};
                    deliver_record(m, ch, rh, g_items[i].p);
                }
            }
    g_item_n = 0;
    for (Watch& w : g_w)
        if (w.destroyed) {
            w.destroyed = false;
            end(w, KILN_REPL_END_DESTROYED);
        }
    if (g_cap.f) {
        if (g_cap.removed.load(std::memory_order_acquire)) capture_end("its owner went");
        else if (GetTickCount64() >= g_cap.end_ms) capture_end("time");
        else if (++g_cap.pres % 30 == 0) std::fflush(g_cap.f);  // the file grows in about one-second steps
    }
    if (g_dirty.exchange(false, std::memory_order_relaxed)) watch_apply();
}

// The hot list, the rings and the delta hook follow the live watches and the capture; removed watches free their slots.
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
    const uint32_t cap = g_cap.f ? g_cap.machines : g_cap.pending;
    const bool need = n || cap;
    if (need && !g_delta_hooked) {
        if (!alloc_rings()) {
            logf(KILN_LOG_WARN, "repl: watches have no rings (out of memory): no events");
            n = 0;
        } else if (!h_delta.install(g_api, g_owner, F.delta_writer, reinterpret_cast<void*>(&d_delta), "repl/watch DeltaWriter")) {
            logf(KILN_LOG_WARN, "repl: the watch hook could not be installed (%s): no events", h_delta.error() ? h_delta.error() : "?");
            n = 0;
        } else {
            g_delta_hooked = true;
            logf(KILN_LOG_INFO, "repl: watch hooks installed");
        }
    }
    if (!g_delta_hooked) {
        g_hot.n.store(0, std::memory_order_release);
        g_hot.capture.store(0, std::memory_order_release);
    } else {
        g_hot.n.store(n, std::memory_order_release);
        g_hot.capture.store(cap, std::memory_order_release);
        if (!need) {
            h_delta.revoke();
            g_delta_hooked = false;
            bool any_slot = false;
            for (Watch& w : g_w) any_slot = any_slot || w.handle.load(std::memory_order_relaxed) != 0;
            if (!any_slot) free_rings();  // an ended watch not removed yet keeps nothing to read
            logf(KILN_LOG_INFO, "repl: watch hooks removed (no live watch)");
        }
    }
    stats_apply_hooks();  // the writer and callback hooks serve the watches too
}

void watch_peer_changed(uint32_t machine, uint32_t old_peer) {
    if (machine >= KILN_REPL_MACHINES) return;
    if (old_peer && g_cap.f && g_cap.peer == old_peer) capture_end("the peer left");  // before its ring's counters go
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
    capture_end("the world stopped");
    if (g_rings)
        for (auto& m : g_ws)
            for (WSlot& s : m) s.ring.discard();
    watch_apply();
}

// An owner revoked (any thread): its watches go silently, its capture ends at the next Pre.
void watch_owner_retiring(KilnOwner owner) {
    for (Watch& w : g_w)
        if (w.handle.load(std::memory_order_acquire) && w.owner == owner) {
            w.removed.store(true, std::memory_order_release);
            g_dirty.store(true, std::memory_order_relaxed);
        }
    if (owner && g_cap.owner == owner) g_cap.removed.store(true, std::memory_order_release);
}

void watch_clear_dropped() {
    if (g_rings)
        for (auto& m : g_ws)
            for (WSlot& s : m) s.ring.clear_counters();
}

uint64_t watch_dropped(uint32_t machine, uint32_t ch) {
    return g_rings && machine < KILN_REPL_MACHINES && ch < 2 ? g_ws[machine][ch].ring.dropped() : 0;
}

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
    if (direction == KILN_REPL_RECEIVE) return client_watch(owner, e, fn, user, out);
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
    if (g_dirty.exchange(false, std::memory_order_relaxed)) watch_apply();  // ended and removed watches give their slots back
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
    if (!g_delta_hooked || !stats_send_hooked()) {  // a hook kiln.hooks refused: nothing would come
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
            if (!w.removed.exchange(true, std::memory_order_acq_rel)) g_dirty.store(true, std::memory_order_relaxed);
            return KILN_OK;
        }
    return client_remove(h);
}

// A capture's file name: a plain name in kiln/logs (letters, digits, '.', '_', '-'), nothing that leaves the folder.
bool plain_file_name(const std::string& n) {
    if (n.empty() || n.size() > 200 || n[0] == '.') return false;
    for (char c : n)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return false;
    return n.find("..") == std::string::npos;
}

KilnResult KILN_CALL t_capture(KilnOwner owner, uint32_t peer, uint32_t seconds, KilnStr path) {
    if (!observe_on()) return KILN_ERR_UNAVAILABLE;
    if (!g_api->on_tick_thread()) return KILN_ERR_WRONG_THREAD;
    if (!owner || !peer || peer == KILN_REPL_ALL_MACHINES || seconds < 1 || seconds > 600 || (path.len && !path.ptr) ||
        path.len > 200)
        return KILN_ERR_INVALID_ARGUMENT;
    // the file: a plain name in kiln/logs (empty: one is made up), checked before anything else
    std::string name(path.len ? std::string(path.ptr, path.len) : std::string());
    if (name.empty()) {
        SYSTEMTIME st;
        GetSystemTime(&st);
        char b[64];
        std::snprintf(b, sizeof b, "repl-%08x-%04u%02u%02uT%02u%02u%02uZ.jsonl", peer, st.wYear, st.wMonth, st.wDay, st.wHour,
                      st.wMinute, st.wSecond);
        name = b;
    }
    if (!plain_file_name(name)) return KILN_ERR_INVALID_ARGUMENT;
    if (g_cap.f) return KILN_ERR_INVALID_ARGUMENT;  // one capture at a time
    uint32_t machines = 0;
    for (uint32_t m = 0; m < KILN_REPL_MACHINES; ++m) {
        uint8_t flags = 0;
        if (peer_of(m, &flags) == peer) machines |= 1u << m;
    }
    if (!machines) return KILN_ERR_INVALID_ARGUMENT;  // not connected
    const Registry* reg = registry();
    if (!reg) return KILN_ERR_UNAVAILABLE;
    char dir[1024];
    size_t n = 0;
    if (g_api->path(KILN_PATH_LOGS, dir, sizeof dir, &n) != KILN_OK || n >= sizeof dir) return KILN_ERR_UNAVAILABLE;
    name = std::string(dir, n) + "\\" + name;
    // the hooks first: no file when they cannot go in
    g_cap.pending = machines;
    watch_apply();
    g_cap.pending = 0;
    if (!g_delta_hooked || !stats_send_hooked()) {
        watch_apply();
        return KILN_ERR_UNAVAILABLE;
    }
    const int wn = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wn > 0 ? wn : 0), L'\0');
    if (wn > 0) MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), wn);
    // never over a file that is there (kiln.log, another capture)
    std::FILE* f = wn > 0 && GetFileAttributesW(wide.c_str()) == INVALID_FILE_ATTRIBUTES ? _wfopen(wide.c_str(), L"wb") : nullptr;
    if (!f) {
        logf(KILN_LOG_WARN, "repl: capture: could not create %s (or it exists)", name.c_str());
        watch_apply();
        return KILN_ERR_INVALID_ARGUMENT;
    }
    std::setvbuf(f, nullptr, _IOFBF, 1u << 20);
    std::string head = "{\"type\":\"header\",\"version\":1,\"peer\":" + std::to_string(peer) + ",\"seconds\":" + std::to_string(seconds) +
                       ",\"utc\":\"" + utc_iso() + "\",\"registry\":[";
    for (uint32_t i = 0; i < reg->count; ++i) {
        if (i) head += ',';
        head += "{\"index\":" + std::to_string(i) + ",\"name\":";
        json_str(head, reg->names[i]);
        head += ",\"hash\":" + std::to_string(reg->at[i].hash) + ",\"size\":" + std::to_string(reg->at[i].size) +
                ",\"flags\":" + std::to_string(reg->at[i].flags) + "}";
    }
    head += "]}\n";
    std::fputs(head.c_str(), f);
    g_cap.f = f;
    g_cap.owner = owner;
    g_cap.peer = peer;
    g_cap.machines = machines;
    g_cap.messages = 0;
    g_cap.bytes = head.size();
    g_cap.pres = 0;
    g_cap.end_ms = GetTickCount64() + seconds * 1000ull;
    g_cap.removed.store(false, std::memory_order_relaxed);
    g_cap.path = name;
    watch_apply();  // the hot list keeps the machines (now from g_cap.machines)
    g_cap.dropped_before = capture_ring_drops();
    logf(KILN_LOG_INFO, "repl: capture of peer 0x%x for %u s into %s", peer, seconds, name.c_str());
    return KILN_OK;
}

}  // namespace repl
