// SPDX-License-Identifier: MIT
// Foreign views (since 0.2.0-dev.29, chapter 06c 9.1): code that runs inside one of the game's ECS programs and holds
// that program's iterator (kiln.abilities' natives inside impact_simulation: NativeCtx+0x30 is its iterator) reads and
// writes other entities through the program's own other_read / other_write accessors, with the job's storage, tracker
// and write flag, as tracked writes do (writes.cpp). The slots come from the wraps' view algorithm (layout.cpp's
// game_layout over the live descriptor), checked against the job's group counts before they are used (foreign_check),
// and each live accessor against the world's component map. Reads and writes take explicit arguments;
// attribute formulas follow the last write on that thread inside the program's update.
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "layout.h"
#include "world_internal.h"
#include "wraps.h"
#include "write_queue.h"

namespace kw {
namespace {

constexpr const char* kAttributeRegistry = "impact::AttributeRegistry";
using AttrChangedFn = void (*)(void* component, void* registry);

// A computed view: immutable after foreign_layout made it, except each accessor's failure latch. Kept for the
// process (one per program and world session that asked: a handful).
struct ForeignView {
    uint32_t kind = 0;
    uint64_t session = 0;
    int32_t index = -1;
    ProgramLayout layout;
    uintptr_t registry = 0;  // the world's registry when it was made (the component map for the accessor checks)
    struct Access {
        int32_t read = -1, write = -1;  // iterator qwords: other_read (or other_write) / other_write
        std::string name;
        int attribute = -1;             // -1 unknown, 0 ordinary, 1 an AttributeComponent
        uint16_t index = 0;             // the world's type index of the component (0: not in the component map)
    };
    std::unordered_map<uint32_t, Access> by_hash;  // write_hash(short name)
    int32_t attr_registry = -1;
    // per iterator qword: 0 not checked, 1 the accessor is the component map's type, 2 it is not
    std::unique_ptr<std::atomic<uint8_t>[]> checked;
    uint32_t qwords = 0;
};

std::mutex g_foreign_mu;
std::deque<ForeignView> g_views;  // stable addresses: a handle is a view's address
std::atomic<AttrChangedFn> g_attr_fn{nullptr};
std::atomic<bool> g_attr_resolved{false};

struct LastWrite {
    const ForeignView* view = nullptr;
    const uint64_t* iter = nullptr;
    void* component = nullptr;
    uint32_t size = 0;
    int attribute = -1;
};
thread_local LastWrite t_last_write;

int attribute_kind(std::string_view name) {
    const auto* types = program_types();
    if (!types || !types->type_by_name || !types->type_info) return -1;
    const std::string full = "keen::ecs::" + std::string(name);
    const KilnType* type = types->type_by_name(kstr(full));
    if (!type) return -1;
    for (unsigned depth = 0; depth < 16 && type; ++depth) {
        KilnTypeInfo info{}; info.size = sizeof info;
        if (types->type_info(type, &info) != KILN_OK) return -1;
        if (sv(info.name) == "AttributeComponent") return depth > 0 ? 1 : 0;
        type = info.inner;
    }
    return type ? -1 : 0;
}

const ForeignView* view_of(const KilnForeignLayout* l) {
    if (!l || l->size < offsetof(KilnForeignLayout, why) || !l->handle) return nullptr;
    const auto* v = reinterpret_cast<const ForeignView*>(l->handle);
    if (v->session != l->session || game_world_session(v->kind) != v->session) return nullptr;  // that world ended
    return v;
}

ProgramCtx* ctx_of(const uint64_t* iter) { return iter ? reinterpret_cast<ProgramCtx*>(iter[0]) : nullptr; }

// The live accessor at `slot` is the component map's type (its first u16 is the type index).
bool accessor_ok(const ForeignView& v, int32_t slot, uint16_t index, const uint64_t* iter) {
    if (slot < 1 || static_cast<uint32_t>(slot) >= v.qwords || !iter[slot] || index == 0) return false;
    std::atomic<uint8_t>& c = v.checked[static_cast<size_t>(slot)];
    const uint8_t s = c.load(std::memory_order_acquire);
    if (s == 2) return false;  // a fault stays closed, but a success belongs to that iterator only
    uint16_t held = 0;
    const bool ok = seh_copy(&held, reinterpret_cast<const void*>(iter[slot]), 2) && held == index;
    c.store(ok ? 1 : 2, std::memory_order_release);
    if (!ok)
        KW_E("foreign view of %s: the accessor at qword %d is not what the world's component map says (type %u, held %u); "
             "that component stays closed for this view", v.layout.name.c_str(), slot, index, held);
    return ok;
}

void* storage_of_job(const ProgramCtx* ctx) {
    void* storage = nullptr;
    return ctx && ctx->job && L.job_storage >= 0 && seh_copy(&storage, ctx->job + L.job_storage, sizeof storage) ? storage : nullptr;
}

const ForeignView::Access* access_of(const ForeignView& v, KilnStr component) {
    const std::string_view name = sv(component);
    if (name.empty()) return nullptr;
    const auto at = v.by_hash.find(write_hash(name));
    return at == v.by_hash.end() || at->second.name != name ? nullptr : &at->second;
}

void set_rc(KilnResult* rc, KilnResult value) {
    if (rc) *rc = value;
}

bool counts_known() {
    return L.job_tpl_count >= 0 && L.job_read_count >= 0 && L.job_write_count >= 0 && L.job_singletons >= 0 &&
           L.singletons_count >= 0 && L.job_other_tpl_count >= 0 && L.job_other_count >= 0 && L.job_events_in_count >= 0 &&
           L.job_events_out_count >= 0;
}

// job_counts (layout.h) through guarded copies: the job comes from an iterator a caller handed in.
bool read_job_counts(const uint8_t* j, JobCounts* c) {
    uint8_t t = 0, ot = 0, o = 0, s = 0;
    uint16_t r = 0, w = 0;
    uint64_t ei = 0;
    uint32_t eo = 0;
    const uint8_t* singletons = nullptr;
    if (!seh_copy(&t, j + L.job_tpl_count, 1) || !seh_copy(&r, j + L.job_read_count, 2) ||
        !seh_copy(&w, j + L.job_write_count, 2) || !seh_copy(&singletons, j + L.job_singletons, sizeof singletons) ||
        !seh_copy(&ot, j + L.job_other_tpl_count, 1) || !seh_copy(&o, j + L.job_other_count, 1) ||
        !seh_copy(&ei, j + L.job_events_in_count, 8) || !seh_copy(&eo, j + L.job_events_out_count, 4))
        return false;
    if (singletons && !seh_copy(&s, singletons + L.singletons_count, 1)) return false;
    *c = JobCounts{t, r, w, s, ot, o, static_cast<uint32_t>(ei), eo};
    return true;
}

}  // namespace

KilnResult KILN_CALL foreign_layout(uint32_t world_kind, KilnStr name, KilnForeignLayout* out) {
    if (!out || out->size < sizeof(KilnForeignLayout)) return KILN_ERR_INVALID_ARGUMENT;
    std::memset(reinterpret_cast<uint8_t*>(out) + 4, 0, sizeof(KilnForeignLayout) - 4);
    out->world_kind = world_kind;
    out->program = -1;
    ProgramRef ref;
    if (!game_program_lookup(world_kind, sv(name), &ref) || !ref.desc) return KILN_ERR_NOT_FOUND;
    const uintptr_t world = g_api->size >= offsetof(KilnApi, session) + sizeof(void*) ? g_api->world(world_kind) : g_world.load();
    if (!world) return KILN_ERR_NOT_FOUND;
    ForeignView v;
    v.kind = world_kind;
    v.session = game_world_session(world_kind);
    v.index = ref.index;
    v.registry = registry_of(world);
    std::string why;
    if (!game_layout(reinterpret_cast<const EngineDesc*>(ref.desc), world, v.layout, why)) {
        std::snprintf(out->why, sizeof out->why, "%s", why.c_str());
        return KILN_ERR_UNAVAILABLE;
    }
    for (int id : {L_OREAD, L_OWRITE}) {
        const List& l = v.layout.lists[id];
        for (size_t i = 0; i < l.names.size(); ++i) {
            auto& a = v.by_hash[write_hash(l.names[i])];
            a.name = l.names[i];
            a.attribute = attribute_kind(l.names[i]);
            if (id == L_OWRITE) a.write = l.slot[i];
            if (a.read < 0 || id == L_OREAD) a.read = l.slot[i];
            if (l.index.size() > i) a.index = l.index[i];
        }
    }
    for (int id : {L_SREAD, L_SWRITE}) {
        const List& l = v.layout.lists[id];
        for (size_t i = 0; i < l.names.size(); ++i)
            if (l.names[i] == kAttributeRegistry) v.attr_registry = l.slot[i];
    }
    v.qwords = v.layout.iter_size / 8;
    v.checked = std::make_unique<std::atomic<uint8_t>[]>(v.qwords);
    for (uint32_t i = 0; i < v.qwords; ++i) v.checked[i].store(0);
    const ForeignView* made = nullptr;
    {
        std::lock_guard lock(g_foreign_mu);
        g_views.push_back(std::move(v));
        made = &g_views.back();
    }
    out->session = made->session;
    out->program = made->index;
    out->iter_size = made->layout.iter_size;
    const ProgramLayout& p = made->layout;
    const uint32_t counts[8] = {p.T, p.R, p.W, p.S, p.OT, p.O, p.EI, p.EO};
    std::memcpy(out->counts, counts, sizeof counts);
    out->handle = reinterpret_cast<uint64_t>(made);
    if (!g_attr_resolved.exchange(true)) {
        uintptr_t address = 0;
        if (g_api->symbol(kstr("items/AttrChanged"), &address) == KILN_OK)
            g_attr_fn.store(reinterpret_cast<AttrChangedFn>(address), std::memory_order_release);
    }
    return KILN_OK;
}

int32_t KILN_CALL foreign_slot(const KilnForeignLayout* layout, int32_t list, KilnStr name) {
    const ForeignView* v = view_of(layout);
    if (!v || list < 0 || list >= L_COUNT) return -1;
    const List& l = v->layout.lists[list];
    const std::string_view n = sv(name);
    for (size_t i = 0; i < l.names.size(); ++i)
        if (l.names[i] == n) return l.slot[i];
    return -1;
}

KilnResult KILN_CALL foreign_check(const KilnForeignLayout* layout, const uint64_t* iter, uint32_t* out_counts) {
    const ForeignView* v = view_of(layout);
    ProgramCtx* ctx = ctx_of(iter);
    if (!v || !ctx || !ctx->job || !counts_known()) return KILN_ERR_UNAVAILABLE;
    // Counts and type indices can coincide across worlds. The job's storage must belong to this view's registry.
    uintptr_t registry = 0;
    const void* storage = storage_of_job(ctx);
    if (!storage || !seh_copy(&registry, storage, sizeof registry) || !registry || registry != v->registry)
        return KILN_ERR_UNAVAILABLE;
    JobCounts c;
    if (!read_job_counts(ctx->job, &c)) return KILN_ERR_UNAVAILABLE;
    if (out_counts) {
        const uint32_t n[8] = {c.T, c.R, c.W, c.S, c.OT, c.O, c.EI, c.EO};
        std::memcpy(out_counts, n, sizeof n);
    }
    return same_counts(v->layout, c) ? KILN_OK : KILN_ERR_UNAVAILABLE;
}

const void* KILN_CALL foreign_read(const KilnForeignLayout* layout, const uint64_t* iter, KilnStr component, uint32_t entity,
                                   uint32_t* size, KilnResult* rc) {
    if (size) *size = 0;
    const ForeignView* v = view_of(layout);
    ProgramCtx* ctx = ctx_of(iter);
    if (!v || !ctx || foreign_check(layout, iter, nullptr) != KILN_OK || !F.read_other || !fn.read_other) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    const ForeignView::Access* a = access_of(*v, component);
    if (!a || a->read < 0) return set_rc(rc, KILN_ERR_NOT_FOUND), nullptr;
    if (!accessor_ok(*v, a->read, a->index, iter)) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    void* storage = storage_of_job(ctx);
    if (!storage) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    Slice s{};
    unsigned long code = 0;
    if (!seh_read_other(&s, storage, reinterpret_cast<void*>(iter[a->read]), entity, &code)) return set_rc(rc, KILN_ERR_INTERNAL), nullptr;
    set_rc(rc, KILN_OK);
    if (!s.ptr || s.size == 0 || s.size > UINT32_MAX) return nullptr;
    if (size) *size = static_cast<uint32_t>(s.size);
    return s.ptr;
}

void* KILN_CALL foreign_write(const KilnForeignLayout* layout, const uint64_t* iter, KilnStr component, uint32_t entity,
                              uint32_t* size, KilnResult* rc) {
    t_last_write = {};
    if (size) *size = 0;
    const ForeignView* v = view_of(layout);
    ProgramCtx* ctx = ctx_of(iter);
    if (!v || !ctx || foreign_check(layout, iter, nullptr) != KILN_OK || !F.write_other) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    const ForeignView::Access* a = access_of(*v, component);
    if (!a || a->write < 0) return set_rc(rc, KILN_ERR_NOT_FOUND), nullptr;
    if (!accessor_ok(*v, a->write, a->index, iter)) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    void* storage = storage_of_job(ctx);
    if (!storage) return set_rc(rc, KILN_ERR_UNAVAILABLE), nullptr;
    Slice s{};
    unsigned long code = 0;
    if (!seh_write_other(&s, storage, ctx, iter[a->write], entity, &code)) return set_rc(rc, KILN_ERR_INTERNAL), nullptr;
    set_rc(rc, KILN_OK);
    if (!s.ptr || s.size == 0 || s.size > UINT32_MAX) return nullptr;
    if (size) *size = static_cast<uint32_t>(s.size);
    t_last_write = LastWrite{v, iter, const_cast<void*>(s.ptr), static_cast<uint32_t>(s.size), a->attribute};
    return const_cast<void*>(s.ptr);
}

KilnResult KILN_CALL foreign_attr_changed(const KilnForeignLayout* layout, const uint64_t* iter, void* component) {
    const ForeignView* v = view_of(layout);
    if (!v || !iter) return KILN_ERR_UNAVAILABLE;
    const AttrChangedFn f = g_attr_fn.load(std::memory_order_acquire);
    if (!f) return KILN_ERR_UNAVAILABLE;
    if (t_last_write.view != v || t_last_write.iter != iter || t_last_write.component != component ||
        t_last_write.size < 8 || t_last_write.attribute == 0) return KILN_ERR_INVALID_ARGUMENT;
    if (t_last_write.attribute < 0 || foreign_check(layout, iter, nullptr) != KILN_OK) return KILN_ERR_UNAVAILABLE;
    uint8_t flags = 0;
    if (!component || v->attr_registry < 1 || !iter[v->attr_registry] ||
        !seh_copy(&flags, static_cast<const uint8_t*>(component) + 6, 1) || !(flags & 1))
        return KILN_ERR_INVALID_ARGUMENT;
    unsigned long code = 0;
    return seh_attr_changed(f, component, reinterpret_cast<void*>(iter[v->attr_registry]), &code) ? KILN_OK : KILN_ERR_INTERNAL;
}

}  // namespace kw
