// SPDX-License-Identifier: MIT
// kiln.routines: assignments, the change queue and the events. See state.h and kiln_routines.h (the contract).
#include "state.h"

#include <algorithm>

namespace kroutines {

namespace {

void put_string(std::string& s, std::string_view v) {
    s += '"';
    for (char c : v) {
        if (c == '"' || c == '\\') s += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) s += c;
    }
    s += '"';
}

void put_optional(std::string& s, std::string_view v) {
    if (v.empty()) s += "null";
    else put_string(s, v);
}

}  // namespace

KilnResult Book::enqueue(Op op, KilnOwner owner, uint32_t npc_id, std::shared_ptr<const Plan> plan, KilnRoutineDone done, void* user,
                         uint64_t* ticket) {
    if (ticket) *ticket = 0;
    if (!owner || !npc_id || !done || !ticket) return KILN_ERR_INVALID_ARGUMENT;
    // full: refused before anything changes, so no ticket, no completion and no event
    if (queue_.size() >= KILN_ROUTINES_MAX_QUEUED) return KILN_ERR_BUFFER_TOO_SMALL;
    *ticket = ++tickets_;
    queue_.push_back(Change{*ticket, op, owner, npc_id, std::move(plan), done, user, 0, false, {}});
    return KILN_OK;
}

KilnResult Book::assign(KilnOwner owner, uint32_t npc_id, std::string_view plan_json, KilnRoutineDone done, void* user, uint64_t* ticket,
                        std::string* problem) {
    if (ticket) *ticket = 0;
    std::string why;
    auto plan = std::make_shared<Plan>();
    if (!owner || !npc_id || !done || !ticket) why = "an owner, a villager, a callback and a place for the ticket are needed";
    else if (!parse_plan(plan_json, *plan, why)) {
    } else {
        release_revoked();
        // The owner's mod is the key's: the first plan binds it, and neither side of the binding moves while it lasts.
        auto bound = owner_mod_.find(owner);
        auto holder = mod_owner_.find(plan->mod);
        if (bound != owner_mod_.end() && bound->second != plan->mod)
            why = "the plan " + plan->key + " is not a plan of " + bound->second;
        else if (holder != mod_owner_.end() && holder->second != owner)
            why = "another owner assigns the plans of " + plan->mod;
    }
    if (!why.empty()) {
        if (problem) *problem = why;
        return KILN_ERR_INVALID_ARGUMENT;
    }
    const std::string mod = plan->mod;
    const KilnResult rc = enqueue(Op::Assign, owner, npc_id, std::move(plan), done, user, ticket);
    if (rc == KILN_OK) {
        owner_mod_[owner] = mod;
        mod_owner_[mod] = owner;
    }
    return rc;
}

KilnResult Book::restore(KilnOwner owner, uint32_t npc_id, std::string_view plan_json, uint64_t revision, bool suspended, std::string_view reason,
                         KilnRoutineDone done, void* user, uint64_t* ticket, std::string* problem) {
    if (ticket) *ticket = 0;
    if (!revision) {
        if (problem) *problem = "a restored assignment has its saved revision (not 0)";
        return KILN_ERR_INVALID_ARGUMENT;
    }
    // the same checks and the same queue as assign; then the change says what the saved row said
    const KilnResult rc = assign(owner, npc_id, plan_json, done, user, ticket, problem);
    if (rc != KILN_OK) return rc;
    Change& c = queue_.back();
    c.op = Op::Restore;
    c.revision = revision;
    c.suspended = suspended;
    c.reason = std::string(reason);
    return KILN_OK;
}

void Book::claim(KilnOwner owner, std::string_view mod) {
    if (!owner || mod.empty()) return;
    release_revoked();
    const std::string m(mod);
    claimed_.insert(owner);
    // a plan of another mod bound this owner before Kiln named it: that binding was only the plan's word
    if (auto b = owner_mod_.find(owner); b != owner_mod_.end() && b->second != m) {
        if (auto h = mod_owner_.find(b->second); h != mod_owner_.end() && h->second == owner) mod_owner_.erase(h);
    }
    // the mod's routines follow its claimed owner (a reload's new owner, or the code owner of a mod whose routines a
    // restart bound under its blocks owner)
    if (auto held = mod_owner_.find(m); held != mod_owner_.end() && held->second != owner) {
        const KilnOwner old = held->second;
        owner_mod_.erase(old);
        for (auto& [npc, a] : assignments_)
            if (a.owner == old) a.owner = owner;
    }
    owner_mod_[owner] = m;
    mod_owner_[m] = owner;
}

KilnResult Book::remove(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket) {
    return enqueue(Op::Remove, owner, npc_id, nullptr, done, user, ticket);
}

KilnResult Book::resume(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket) {
    return enqueue(Op::Resume, owner, npc_id, nullptr, done, user, ticket);
}

void Book::revoke_owner(KilnOwner owner) {
    if (!owner) return;
    std::lock_guard lock(revoke_mu_);
    revoked_.push_back(owner);
}

void Book::complete(const Change& c, bool ok, uint32_t code, std::string reason, uint64_t revision) {
    completions_.push_back(Completion{c.owner, c.done, c.user, c.ticket, ok, code, std::move(reason), c.npc_id, revision});
}

std::string Book::snapshot(uint32_t npc_id, const Assignment& a, const char* state_override) const {
    const Runtime& rt = *a.runtime;
    std::string s = "{\"npcId\":" + std::to_string(npc_id) + ",\"owner\":";
    put_string(s, a.plan->mod);
    s += ",\"key\":";
    put_string(s, a.plan->key);
    s += ",\"planVersion\":" + std::to_string(a.plan->version) + ",\"revision\":" + std::to_string(a.revision) + ",\"assignment\":\"";
    s += state_override ? state_override : a.suspended ? "ManualSuspended" : a.present ? "Assigned" : "PendingSpawn";
    s += "\",\"windowIndex\":" + std::to_string(rt.task == Kind::None ? -1 : rt.window) + ",\"invocation\":" + std::to_string(rt.invocation) +
         ",\"task\":";
    put_optional(s, rt.task == Kind::None ? "" : kind_name(rt.task));
    s += ",\"taskState\":\"";
    s += task_state_name(rt.state);
    s += "\",\"nativeState\":" + std::to_string(a.present ? rt.native_state : 0) + ",\"lastReason\":";
    put_optional(s, a.suspended && rt.last_reason == Reason::ManualSuspended ? std::string_view(a.suspend_reason)
                                                                             : std::string_view(reason_text(rt.last_reason)));
    s += ",\"paused\":";
    s += rt.paused ? "true" : "false";
    s += '}';
    return s;
}

void Book::assignment_event(const std::string& before, const std::string& after, std::string_view reason, bool teardown) {
    std::string s = "{\"type\":\"assignment\",\"before\":";
    s += before.empty() ? "null" : before;
    s += ",\"after\":" + after + ",\"reason\":";
    put_string(s, reason);
    s += ",\"teardown\":";
    s += teardown ? "true" : "false";
    s += '}';
    events_.push_back(std::move(s));
}

void Book::task_event(uint32_t npc_id, const Assignment& a, const TaskEvent& e, bool teardown) {
    std::string s = "{\"type\":\"task\",\"npcId\":" + std::to_string(npc_id) + ",\"key\":";
    put_string(s, a.plan->key);
    s += ",\"planVersion\":" + std::to_string(a.plan->version) + ",\"revision\":" + std::to_string(a.revision) +
         ",\"invocation\":" + std::to_string(e.invocation) + ",\"windowIndex\":" + std::to_string(e.window) + ",\"task\":";
    put_optional(s, e.task == Kind::None ? "" : kind_name(e.task));
    s += ",\"from\":\"";
    s += task_state_name(e.from);
    s += "\",\"to\":\"";
    s += task_state_name(e.to);
    s += "\",\"reason\":";
    put_optional(s, reason_text(e.reason));
    s += ",\"teardown\":";
    s += teardown ? "true" : "false";
    s += '}';
    events_.push_back(std::move(s));
}

void Book::drain(uint32_t npc_id, Assignment& a) {
    Runtime& rt = *a.runtime;
    for (uint32_t i = 0; i < rt.n_events; ++i) task_event(npc_id, a, rt.events[i], false);
    dropped_ += rt.dropped;
    rt.n_events = 0;
    rt.dropped = 0;
}

void Book::close_task(uint32_t npc_id, Assignment& a, Reason why, bool teardown) {
    Runtime& rt = *a.runtime;
    drain(npc_id, a);
    if (rt.state != TaskState::Chosen && rt.state != TaskState::Started) return;
    rt.push(TaskState::Suspended, why);
    task_event(npc_id, a, rt.events[0], teardown);
    rt.n_events = 0;
}

uint64_t Book::drop(uint32_t npc_id, std::string_view reason) {
    auto it = assignments_.find(npc_id);
    Assignment& a = it->second;
    const std::string before = snapshot(npc_id, a);
    close_task(npc_id, a, Reason::Removed, false);
    a.revision = ++revision_;
    const uint64_t revision = a.revision;
    assignment_event(before, snapshot(npc_id, a, "Removed"), reason, false);
    assignments_.erase(it);
    return revision;
}

void Book::pre(uint64_t step, const Resolver& where) {
    // 1. owners that went since the last step
    release_revoked();
    std::vector<KilnOwner> gone;
    {
        std::lock_guard lock(revoke_mu_);
        gone.swap(revoked_);
    }
    for (KilnOwner owner : gone) {
        gone_.insert(owner);
        claimed_.erase(owner);
        // its queued changes go without a call: the owner is gone, and nothing of it is called any more
        std::erase_if(queue_, [&](const Change& c) { return c.owner == owner; });
        std::vector<uint32_t> theirs;
        for (auto& [npc, a] : assignments_)
            if (a.owner == owner) theirs.push_back(npc);
        for (uint32_t npc : theirs) drop(npc, "OwnerUnloaded");
    }

    // 2. the queue, in order
    for (Change& c : queue_) {
        auto it = assignments_.find(c.npc_id);
        if (it != assignments_.end() && it->second.owner != c.owner) {
            // another mod's routine is on this villager: nothing is written, no event
            complete(c, false, KILN_ROUTINE_OWNED_BY, "OwnedBy:" + it->second.plan->mod, it->second.revision);
            continue;
        }
        switch (c.op) {
            case Op::Assign: {
                std::string before;
                if (it == assignments_.end()) {
                    it = assignments_.emplace(c.npc_id, Assignment{}).first;
                    it->second.owner = c.owner;
                } else {
                    before = snapshot(c.npc_id, it->second);
                    // the same owner replaces its plan; a manual suspension stays until an explicit resume
                    close_task(c.npc_id, it->second, Reason::Replaced, false);
                    Runtime& rt = *it->second.runtime;
                    rt.window = -1;
                    rt.task = Kind::None;
                    rt.state = TaskState::None;
                    rt.skip_window = Runtime::kNoSkip;
                }
                Assignment& a = it->second;
                a.plan = std::move(c.plan);
                a.revision = ++revision_;
                a.present = where && where(c.npc_id) != 0;
                assignment_event(before, snapshot(c.npc_id, a), before.empty() ? "Assigned" : "Replaced", false);
                complete(c, true, KILN_ROUTINE_OK, "", a.revision);
                break;
            }
            case Op::Restore: {
                // A saved assignment, bound again at its saved revision with its saved suspension. The service forgot
                // the world's assignments when it stopped, so there is normally none; one made since (a mod assigned
                // before world-ready) is the owner's newer word and stays.
                if (it != assignments_.end()) {
                    complete(c, true, KILN_ROUTINE_OK, "", it->second.revision);
                    break;
                }
                it = assignments_.emplace(c.npc_id, Assignment{}).first;
                Assignment& a = it->second;
                a.owner = c.owner;
                a.plan = std::move(c.plan);
                a.revision = c.revision;
                if (c.revision > revision_) revision_ = c.revision;
                a.suspended = c.suspended;
                if (c.suspended) {
                    a.suspend_reason = "ManualCommand";
                    a.runtime->last_reason = Reason::ManualSuspended;
                }
                a.present = where && where(c.npc_id) != 0;
                if (!c.reason.empty()) assignment_event("", snapshot(c.npc_id, a), c.reason, false);
                complete(c, true, KILN_ROUTINE_OK, "", a.revision);
                break;
            }
            case Op::Remove:
                // nothing to remove is already as asked: ok, no revision, no event
                if (it == assignments_.end()) complete(c, true, KILN_ROUTINE_OK, "", 0);
                else complete(c, true, KILN_ROUTINE_OK, "", drop(c.npc_id, "Removed"));
                break;
            case Op::Resume:
                if (it == assignments_.end()) complete(c, false, KILN_ROUTINE_NOT_ASSIGNED, "NotAssigned", 0);
                else if (!it->second.suspended) complete(c, true, KILN_ROUTINE_OK, "", it->second.revision);
                else {
                    Assignment& a = it->second;
                    const std::string before = snapshot(c.npc_id, a);
                    a.suspended = false;
                    a.suspend_reason.clear();
                    a.resumed = true;
                    // the plan decides afresh at the next decision: no task of the plan is running until then
                    Runtime& rt = *a.runtime;
                    rt.task = Kind::None;
                    rt.state = TaskState::None;
                    rt.window = -1;
                    rt.skip_window = Runtime::kNoSkip;
                    rt.manual = false;
                    a.revision = ++revision_;
                    assignment_event(before, snapshot(c.npc_id, a), "Resumed", false);
                    complete(c, true, KILN_ROUTINE_OK, "", a.revision);
                }
                break;
        }
    }
    queue_.clear();

    // 3. this step's villagers for the hooks
    if (assignments_.empty()) {
        if (!engine_.empty()) engine_.clear();
        return;
    }
    std::vector<Bind> binds;
    binds.reserve(assignments_.size());
    for (auto& [npc, a] : assignments_) {
        const uintptr_t dynamic = where ? where(npc) : 0;
        a.present = dynamic != 0;
        binds.push_back(Bind{npc, dynamic, a.plan, !a.suspended, a.runtime.get(), a.resumed});
        a.resumed = false;
    }
    engine_.publish(std::move(binds), step);
}

void Book::post() {
    for (auto& [npc, a] : assignments_) {
        Runtime& rt = *a.runtime;
        const bool manual = rt.manual;
        rt.manual = false;
        if (manual && !a.suspended) {
            // A Reset or GoTo the worker took this step: the assignment first, then the task it ended (the hook already
            // closed it with ManualSuspended).
            std::string before = snapshot(npc, a);
            a.suspended = true;
            a.suspend_reason = "ManualCommand";
            rt.last_reason = Reason::ManualSuspended;
            a.revision = ++revision_;
            assignment_event(before, snapshot(npc, a), "ManualSuspended", false);
        }
        drain(npc, a);
    }
}

void Book::stop() {
    post();
    for (const Change& c : queue_) {
        auto have = assignments_.find(c.npc_id);
        complete(c, false, KILN_ROUTINE_WORLD_STOPPED, "WorldStopped", have == assignments_.end() ? 0 : have->second.revision);
    }
    queue_.clear();
    engine_.clear();
    for (auto& [npc, a] : assignments_) {
        const std::string before = snapshot(npc, a);
        close_task(npc, a, Reason::WorldStopped, true);
        a.present = false;
        assignment_event(before, snapshot(npc, a, "PendingSpawn"), "WorldStopped", true);
    }
    assignments_.clear();
    revision_ = 0;
    release_revoked();
    std::lock_guard lock(revoke_mu_);
    revoked_.clear();
}

// An owner that was revoked no longer holds its mod, also before the next Pre takes its assignments away: the mod's
// next owner (a reload) can queue its plans at once.
void Book::release_revoked() {
    std::lock_guard lock(revoke_mu_);
    for (KilnOwner owner : revoked_)
        if (auto it = owner_mod_.find(owner); it != owner_mod_.end()) {
            mod_owner_.erase(it->second);
            owner_mod_.erase(it);
        }
}

bool Book::inspect(uint32_t npc_id, std::string& json) const {
    auto it = assignments_.find(npc_id);
    if (it == assignments_.end()) return false;
    json = snapshot(npc_id, it->second);
    return true;
}

std::vector<Completion> Book::take_completions() {
    std::vector<Completion> out;
    out.swap(completions_);
    // an owner revoked since its change completed (between Pre and Post) is not called either
    std::lock_guard lock(revoke_mu_);
    std::erase_if(out, [&](const Completion& c) {
        return gone_.count(c.owner) || std::find(revoked_.begin(), revoked_.end(), c.owner) != revoked_.end();
    });
    return out;
}

std::vector<std::string> Book::take_events() {
    std::vector<std::string> out;
    out.swap(events_);
    return out;
}

}  // namespace kroutines
