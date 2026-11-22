// SPDX-License-Identifier: MIT
// kiln.world's named services: everything before the gate and after its release; see named_services.h. The hold itself
// is named_service_hold.cpp.
#include "named_services.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>

namespace kw::named {

namespace {
using kiln::engine::EcsGateScope;
using kiln::engine::EcsView;
using kiln::engine::GateAdmission;
using kiln::engine::Reason;
using kiln::engine::Role;

// KilnMutationReply's fields appended after its first release are written only when the caller's size covers them,
// as ecs_compare_exchange does (ecs_writes.cpp).
constexpr uint32_t kReplyV1 = offsetof(KilnMutationReply, restore_ns);
bool reply_has_restore(uint32_t size) { return size >= offsetof(KilnMutationReply, restore_ns) + sizeof(uint64_t); }
KilnResult result_of(uint32_t status) {
    switch (status) {
        case KILN_ECS_OK: return KILN_OK;
        case KILN_ECS_ABSENT: return KILN_ERR_NOT_FOUND;
        case KILN_ECS_STALE: return KILN_ECS_ERR_STALE;
        case KILN_ECS_UNREADABLE: return KILN_ECS_ERR_UNREADABLE;
        case KILN_ECS_BUSY: return KILN_ECS_ERR_BUSY;
        case KILN_ECS_CONFLICT: return KILN_ECS_ERR_CONFLICT;
        case KILN_ECS_NOT_WRITABLE: return KILN_ECS_ERR_NOT_WRITABLE;
        case KILN_ECS_UNCERTAIN: return KILN_ECS_ERR_UNCERTAIN;
        default: return KILN_ERR_UNAVAILABLE;
    }
}
// The gate's refusals, as the compare-exchange writes read them (ecs_writes.cpp): busy (a later attempt can win) or
// unavailable (it cannot from here, or not now at all).
uint32_t gate_status(Reason r, uint32_t* why) {
    switch (r) {
        case Reason::InFrame:
            *why = KILN_ECS_WRITE_WHY_IN_FRAME;
            return KILN_ECS_BUSY;
        case Reason::KilnHolderBusy:
        case Reason::KilnBusyLatched:
        case Reason::EcsSceneFrameBusy:
        case Reason::EcsThreadRegistryBusy:
        case Reason::EcsEngineOwnerChangedDuringRead:
        case Reason::EcsServerThreadChangedDuringRead:
        case Reason::EcsActiveSceneListChanged:
        case Reason::EcsReadySceneContextChanged:
        case Reason::EcsWriteOwnerChangedOrNotAuthoritative:
            *why = KILN_ECS_WRITE_WHY_GATE;
            return KILN_ECS_BUSY;
        default:
            *why = KILN_ECS_WRITE_WHY_GATE;
            return KILN_ECS_UNAVAILABLE;
    }
}
bool has_gate(const KilnEngineNativeApi* e) {
    return e && e->header.size >= offsetof(KilnEngineNativeApi, ecs_gate_release) + sizeof(void*) && e->layout &&
           e->ecs_view && e->copy && e->ecs_gate_acquire && e->ecs_gate_validate && e->ecs_gate_write &&
           e->ecs_gate_release;
}
// ShroudForge's selector rule (access_named_service.rs get, resolve_named_service.cpp contract): the original DI name,
// 1..127 bytes, no NUL, never the qualified "keen::" form.
bool valid_name(KilnStr name) {
    if (!name.ptr || !name.len || name.len > 127) return false;
    if (std::memchr(name.ptr, 0, name.len)) return false;
    return !(name.len >= 6 && std::memcmp(name.ptr, "keen::", 6) == 0);
}
bool same_world(const EcsView& view, const kiln::engine::Owners& owners, std::uintptr_t registry,
                const std::array<std::uint64_t, 2>& scene) {
    return view.owners == owners && view.registry_owner == registry && view.scene_uuid == scene;
}
}  // namespace

const kiln::engine::Layout* Services::layout() const { return has_gate(src_.engine) ? src_.engine->layout() : nullptr; }

const char* Services::unavailable_reason() const {
    if (!src_.engine) return "no exact engine profile for this build, or kiln.internal.engine is not here";
    if (!has_gate(src_.engine)) return "kiln.internal.engine has no write gate here";
    const kiln::engine::Layout* L = src_.engine->layout();
    if (!L) return "no exact engine profile for this build";
    if (!L->write.enabled) return "this build's engine profile has no write gate";
    if (!L->services.enabled || !L->services.contract_count) return "this build's engine profile names no named service";
    if (!L->authority) return "this build's engine profile does not locate the local authority";
    if (!src_.server) return "no world facts here";
    if (!src_.writable) return "no memory-range check here";
    return nullptr;
}

void Services::log(int level, const char* text) const {
    if (src_.log) src_.log(src_.user, level, text);
}

bool Services::retired(KilnOwner owner) const {
    std::lock_guard lock(mu_);
    return retired_.count(owner) != 0;
}

Services::Found Services::find(KilnOwner owner, KilnHandle handle, View* out) const {
    std::lock_guard lock(mu_);
    for (const View& v : views_) {
        if (v.handle != handle) continue;
        if (v.owner != owner) return Found::Other;
        *out = v;
        return Found::Mine;
    }
    return Found::None;
}

// A latch of an earlier session never matches again; a new session clears it in effect.
bool Services::latched(uint64_t session, uint32_t contract) const {
    return session && contract < latches_.size() && latches_[contract].load(std::memory_order_acquire) == session;
}

size_t Services::views() const {
    std::lock_guard lock(mu_);
    return views_.size();
}

size_t Services::views_of(KilnOwner owner) const {
    std::lock_guard lock(mu_);
    return static_cast<size_t>(std::count_if(views_.begin(), views_.end(), [&](const View& v) { return v.owner == owner; }));
}

void Services::owner_retired(KilnOwner owner) {
    {
        std::lock_guard lock(mu_);
        constexpr size_t kMaxRetired = 4096;  // owners are never reused; the oldest (lowest) is forgotten first
        if (retired_.size() >= kMaxRetired) retired_.erase(std::min_element(retired_.begin(), retired_.end()));
        retired_.insert(owner);
        views_.erase(std::remove_if(views_.begin(), views_.end(), [&](const View& v) { return v.owner == owner; }),
                     views_.end());
    }
    retirements_.fetch_add(1, std::memory_order_acq_rel);
}

void Services::attempt(KilnOwner owner, const View* bound, uint32_t contract, uint64_t want_session, HoldInput& in,
                       Attempt& out) {
    const kiln::engine::Layout& L = *layout();
    // The server world now, from Kiln's own facts; a bound view needs its own session, an open the one it names.
    if (bound) want_session = bound->session;
    uint64_t session = 0;
    if (!src_.server(src_.user, &session)) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = KILN_ECS_WRITE_WHY_NO_WORLD;
        return;
    }
    out.session = session;
    if (!session) {
        out.status = KILN_ECS_UNAVAILABLE;
        out.why = KILN_ECS_WRITE_WHY_NO_WORLD;
        return;
    }
    if (want_session && want_session != session) {
        out.status = KILN_ECS_STALE;
        out.why = KILN_ECS_WRITE_WHY_REF;
        return;
    }
    out.step = src_.server_step ? src_.server_step(src_.user) : 0;
    // The view the gate takes: the local authority's, the world the view was opened in.
    EcsView view{};
    uint32_t reason = 0;
    if (src_.engine->ecs_view(0, &view, &reason) != KILN_OK) {
        out.engine_reason = reason;
        out.status = gate_status(static_cast<Reason>(reason), &out.why);
        return;
    }
    if (view.role != Role::LocalAuthority) {
        out.status = KILN_ECS_UNAVAILABLE;
        out.why = KILN_ECS_WRITE_WHY_NOT_SERVER_WORLD;
        return;
    }
    if (bound && !same_world(view, bound->owners, bound->registry_owner, bound->scene_uuid)) {
        out.status = KILN_ECS_STALE;
        out.why = KILN_ECS_WRITE_WHY_REF;
        return;
    }
    out.view = view;
    in.layout = &L.services;
    in.contract = &L.services.contracts[contract];
    in.copy = src_.engine->copy;
    in.writable = src_.writable;
    in.user = src_.user;
    in.retirements = &retirements_;
    HoldOutput h{};
    {
        EcsGateScope scope(*src_.engine, view, owner, out.step ? out.step : 1, GateAdmission::KilnMod);
        if (!scope.held()) {
            const Reason r = scope.reason();
            scope.release();
            out.engine_reason = static_cast<uint32_t>(r);
            // a refusal after the priority raise restored it: that restore is this call's too
            out.restore_ns = scope.gate().restore_ns;
            out.status = gate_status(r, &out.why);
            return;
        }
        in.scope = &scope;
        hold(in, h);
        scope.release();
        // The gate's own measurements of that release: the Scene word's exclusive interval, ending at that word's
        // release, and the priority restore after it, apart (GitHub #58).
        out.hold_ns = scope.gate().hold_ns;
        out.restore_ns = scope.gate().restore_ns;
    }
    in.scope = nullptr;
    out.status = h.status;
    out.flags = h.flags;
    out.why = h.why;
    // The session again, after the release: a world that stopped and started across the hold is not this one.
    uint64_t after = 0;
    if (out.status != KILN_ECS_UNCERTAIN && (!src_.server(src_.user, &after) || after != session) &&
        !(out.flags & KILN_MUTATION_WRITTEN)) {
        out.status = KILN_ECS_UNREADABLE;
        out.why = KILN_ECS_WRITE_WHY_NO_WORLD;
        out.flags &= ~static_cast<uint32_t>(KILN_MUTATION_CONFIRMED | KILN_MUTATION_NO_OP);
    }
}

KilnResult Services::open(KilnOwner owner, KilnStr name, uint64_t expected_session, KilnHandle* view,
                          const KilnType** type) {
    if (!owner || !view || !valid_name(name)) return KILN_ERR_INVALID_ARGUMENT;
    *view = 0;
    if (type) *type = nullptr;
    if (retired(owner)) return KILN_ECS_ERR_STALE;
    if (!available()) return KILN_ERR_UNAVAILABLE;
    const kiln::engine::Layout& L = *layout();
    uint32_t contract = L.services.contract_count;
    for (uint32_t i = 0; i < L.services.contract_count && i < kiln::engine::kServiceContracts; ++i) {
        const auto& c = L.services.contracts[i];
        if (c.key_size == name.len && std::memcmp(c.key.data(), name.ptr, name.len) == 0) contract = i;
    }
    if (contract >= L.services.contract_count) return KILN_ERR_NOT_FOUND;
    if (views_of(owner) >= kViewsPerOwner || views() >= kViews) return KILN_ERR_BUFFER_TOO_SMALL;
    const kiln::engine::ServiceContract& c = L.services.contracts[contract];
    // One read under the gate proves the service is there and readable now (ShroudForge's get reads it once).
    std::vector<uint8_t> before(c.size);
    HoldInput in;
    in.before = before.data();
    in.retirements_seen = retirements_.load(std::memory_order_acquire);
    Attempt a;
    attempt(owner, nullptr, contract, expected_session, in, a);
    if (a.status != KILN_ECS_OK) return result_of(a.status);
    {
        std::lock_guard lock(mu_);
        if (retired_.count(owner)) return KILN_ECS_ERR_STALE;  // retired during the call: no view outlives it
        size_t mine = 0;
        for (const View& v : views_) mine += v.owner == owner;
        if (mine >= kViewsPerOwner || views_.size() >= kViews) return KILN_ERR_BUFFER_TOO_SMALL;
        View v;
        v.handle = ++next_handle_;
        v.owner = owner;
        v.contract = contract;
        v.session = a.session;
        v.owners = a.view.owners;
        v.registry_owner = a.view.registry_owner;
        v.scene_uuid = a.view.scene_uuid;
        views_.push_back(v);
        *view = v.handle;
    }
    if (type && src_.types && src_.types->header.size >= offsetof(KilnTypesApi, type_info) + sizeof(void*) &&
        src_.types->type_by_hash && src_.types->type_info) {
        const KilnType* t = src_.types->type_by_hash(c.type_hash);
        KilnTypeInfo info{};
        info.size = sizeof info;
        if (t && src_.types->type_info(t, &info) == KILN_OK && info.hash == c.type_hash && info.type_size == c.size)
            *type = t;
    }
    return KILN_OK;
}

KilnResult Services::snapshot(KilnOwner owner, KilnHandle handle, void* out, uint32_t capacity, uint32_t* required) {
    if (!owner || (capacity && !out)) return KILN_ERR_INVALID_ARGUMENT;
    if (required) *required = 0;
    if (retired(owner)) return KILN_ECS_ERR_STALE;
    View v;
    if (find(owner, handle, &v) != Found::Mine) return KILN_ERR_NOT_FOUND;
    if (!available()) return KILN_ERR_UNAVAILABLE;
    const kiln::engine::ServiceContract& c = layout()->services.contracts[v.contract];
    if (required) *required = c.size;
    if (capacity < c.size) return KILN_ERR_BUFFER_TOO_SMALL;
    std::vector<uint8_t> before(c.size);
    HoldInput in;
    in.before = before.data();
    in.retirements_seen = retirements_.load(std::memory_order_acquire);
    Attempt a;
    attempt(owner, &v, v.contract, 0, in, a);
    if (a.status != KILN_ECS_OK) return result_of(a.status);
    std::memcpy(out, before.data(), c.size);
    return KILN_OK;
}

KilnResult Services::close(KilnOwner owner, KilnHandle handle) {
    if (!owner) return KILN_ERR_INVALID_ARGUMENT;
    std::lock_guard lock(mu_);
    for (auto it = views_.begin(); it != views_.end(); ++it)
        if (it->handle == handle && it->owner == owner) {
            views_.erase(it);
            return KILN_OK;
        }
    return KILN_ERR_NOT_FOUND;
}

KilnResult Services::compare_exchange(KilnOwner owner, KilnHandle handle, const void* expected, const void* mask,
                                      const void* desired, uint32_t bytes, void* confirmed, KilnMutationReply* reply) {
    if (!owner || !expected || !desired || !bytes || !reply || reply->size < kReplyV1) return KILN_ERR_INVALID_ARGUMENT;
    const uint32_t reply_size = reply->size;
    std::memset(reply, 0, std::min<size_t>(reply_size, sizeof *reply));  // never past the caller's record
    // On return: the bytes this Kiln filled, the smaller of the caller's size and its own.
    reply->size = static_cast<uint32_t>(std::min<size_t>(reply_size, sizeof *reply));
    auto finish = [&](uint32_t status, uint32_t why, uint32_t flags = 0) -> KilnResult {
        reply->status = status;
        reply->why_code = why;
        reply->flags |= flags;
        return result_of(status);
    };
    const uint64_t retirements = retirements_.load(std::memory_order_acquire);
    if (retired(owner)) return finish(KILN_ECS_STALE, KILN_ECS_WRITE_WHY_OWNER_RETIRED);
    View v;
    if (const Found f = find(owner, handle, &v); f != Found::Mine)
        return finish(KILN_ECS_ABSENT, f == Found::Other ? KILN_ECS_WRITE_WHY_NOT_OWNERS_REF : KILN_ECS_WRITE_WHY_REF);
    reply->session = v.session;
    if (!available()) return finish(KILN_ECS_UNAVAILABLE, KILN_ECS_WRITE_WHY_NO_PROFILE);
    const kiln::engine::ServiceContract& c = layout()->services.contracts[v.contract];
    if (bytes != c.size) return finish(KILN_ECS_NOT_WRITABLE, KILN_ECS_WRITE_WHY_SIZE);
    // An earlier uncertain write to this service this session: no gate.
    if (latched(v.session, v.contract))
        return finish(KILN_ECS_UNCERTAIN, KILN_ECS_WRITE_WHY_TARGET_LATCHED, KILN_MUTATION_TARGET_LATCHED);
    // Staging: the caller's buffers are copied once and never read inside the hold.
    std::vector<uint8_t> stage(static_cast<size_t>(bytes) * 6);
    uint8_t* const exp = stage.data();
    uint8_t* const msk = exp + bytes;
    uint8_t* const des = msk + bytes;
    uint8_t* const before = des + bytes;
    uint8_t* const merged = before + bytes;
    uint8_t* const verified = merged + bytes;
    std::memcpy(exp, expected, bytes);
    std::memcpy(des, desired, bytes);
    if (mask) std::memcpy(msk, mask, bytes);
    HoldInput in;
    in.write = true;
    in.expected = exp;
    in.mask = mask ? msk : nullptr;
    in.desired = des;
    in.before = before;
    in.merged = merged;
    in.verified = verified;
    in.retirements_seen = retirements;
    in.latch = &latches_[v.contract];
    in.session = v.session;
    Attempt a;
    attempt(owner, &v, v.contract, 0, in, a);
    reply->engine_reason = a.engine_reason;
    reply->step = a.step;
    reply->hold_ns = a.hold_ns;
    if (reply_has_restore(reply_size)) reply->restore_ns = a.restore_ns;
    if (a.session) reply->session = a.session;
    // The hold latched the service before it released the gate; it answers TARGET_LATCHED itself for a write that began
    // before an uncertain one ended (nothing written, nothing new to log).
    if (a.status == KILN_ECS_UNCERTAIN) a.flags |= KILN_MUTATION_TARGET_LATCHED;
    if (a.status == KILN_ECS_UNCERTAIN && a.why != KILN_ECS_WRITE_WHY_TARGET_LATCHED) {
        char line[200];
        std::snprintf(line, sizeof line,
                      "world.named_write: a compare-exchange of %.*s could not be proven (why %u); the service is "
                      "latched uncertain for server session %llu",
                      static_cast<int>(c.key_size), c.key.data(), a.why, static_cast<unsigned long long>(v.session));
        log(KILN_LOG_WARN, line);
    }
    if (confirmed && (a.flags & KILN_MUTATION_CONFIRMED))
        std::memcpy(confirmed, a.status == KILN_ECS_OK && (a.flags & KILN_MUTATION_WRITTEN) ? merged : before, bytes);
    else
        a.flags &= ~static_cast<uint32_t>(KILN_MUTATION_CONFIRMED);
    return finish(a.status, a.why, a.flags);
}

}  // namespace kw::named
