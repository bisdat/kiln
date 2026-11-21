// SPDX-License-Identifier: MIT
// kiln.routines: the assignments of the running world and the queue of changes to them (the tick thread's side).
// Changes are queued by the table's calls, committed in order at the next Pre, and each completed once on that step's
// Post. No game and no Windows in here: routines.cpp feeds it the world, tests/ run it on the host.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine.h"
#include "services/kiln_routines.h"

namespace kroutines {

struct Completion {
    KilnOwner owner = 0;  // whose call it was: a revoked owner is never called back
    KilnRoutineDone done = nullptr;
    void* user = nullptr;
    uint64_t ticket = 0;
    bool ok = false;
    uint32_t code = KILN_ROUTINE_OK;
    std::string reason;
    uint32_t npc_id = 0;
    uint64_t revision = 0;
};

// Where a villager's DynamicActiveNpcState is in the server world now: its address, 0 when the villager is not there
// (or more than one entity has that NpcId: then nobody can say which one the plan means).
using Resolver = std::function<uintptr_t(uint32_t npc_id)>;

class Book {
public:
    explicit Book(Engine& engine) : engine_(engine) {}

    // ---- the table's calls (tick thread). KILN_OK gives a ticket; anything else changed nothing. ----
    KilnResult assign(KilnOwner owner, uint32_t npc_id, std::string_view plan_json, KilnRoutineDone done, void* user,
                      uint64_t* ticket, std::string* problem = nullptr);
    KilnResult remove(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket);
    KilnResult resume(KilnOwner owner, uint32_t npc_id, KilnRoutineDone done, void* user, uint64_t* ticket);
    // since dev.31: a saved assignment bound again at its saved revision (Kiln.Blocks at world-ready), and the floor of
    // the world's revisions
    KilnResult restore(KilnOwner owner, uint32_t npc_id, std::string_view plan_json, uint64_t revision, bool suspended,
                       std::string_view reason, KilnRoutineDone done, void* user, uint64_t* ticket, std::string* problem = nullptr);
    void set_revision_floor(uint64_t floor) { if (floor > revision_) revision_ = floor; }
    // since dev.31: Kiln.Blocks names the owner a mod's calls come with. A mod's routines follow its claimed owner (they
    // move to it from an earlier owner of the mod); a claimed owner's plans must be its mod's, and another owner cannot
    // take the mod's plans. Tick thread.
    void claim(KilnOwner owner, std::string_view mod);
    bool inspect(uint32_t npc_id, std::string& json) const;
    void revoke_owner(KilnOwner owner);  // any thread

    // ---- the step (tick thread) ----
    // Pre: revoked owners go, the queue commits in order, and the hooks get this step's list of villagers.
    void pre(uint64_t step, const Resolver& where);
    // Post: what the workers reported becomes task events, and a manual command a worker saw suspends the routine.
    // Then take_completions() and take_events().
    void post();
    // The world stops: what is queued completes with WorldStopped, every assignment says goodbye with teardown events,
    // and nothing of the world is kept.
    void stop();

    std::vector<Completion> take_completions();
    std::vector<std::string> take_events();

    size_t queued() const { return queue_.size(); }
    size_t assigned() const { return assignments_.size(); }
    uint64_t revision() const { return revision_; }
    uint64_t dropped_task_events() const { return dropped_; }

private:
    enum class Op : uint8_t { Assign, Remove, Resume, Restore };
    struct Change {
        uint64_t ticket;
        Op op;
        KilnOwner owner;
        uint32_t npc_id;
        std::shared_ptr<const Plan> plan;
        KilnRoutineDone done;
        void* user;
        // Restore only
        uint64_t revision = 0;
        bool suspended = false;
        std::string reason;
    };
    struct Assignment {
        KilnOwner owner = 0;
        std::shared_ptr<const Plan> plan;
        uint64_t revision = 0;
        bool suspended = false;
        bool present = false;
        bool resumed = false;  // the owner's Resume was made at this Pre (cleared once this step's list is out)
        std::string suspend_reason;
        std::unique_ptr<Runtime> runtime = std::make_unique<Runtime>();
    };

    KilnResult enqueue(Op op, KilnOwner owner, uint32_t npc_id, std::shared_ptr<const Plan> plan, KilnRoutineDone done, void* user,
                       uint64_t* ticket);    void complete(const Change& c, bool ok, uint32_t code, std::string reason, uint64_t revision);
    std::string snapshot(uint32_t npc_id, const Assignment& a, const char* state_override = nullptr) const;
    void assignment_event(const std::string& before, const std::string& after, std::string_view reason, bool teardown);
    void task_event(uint32_t npc_id, const Assignment& a, const TaskEvent& e, bool teardown);
    // closes a running task from the tick thread (the plan is replaced, removed, the world stops)
    void close_task(uint32_t npc_id, Assignment& a, Reason why, bool teardown);
    uint64_t drop(uint32_t npc_id, std::string_view reason);
    void drain(uint32_t npc_id, Assignment& a);
    void release_revoked();

    Engine& engine_;
    std::vector<Change> queue_;
    std::map<uint32_t, Assignment> assignments_;
    std::map<KilnOwner, std::string> owner_mod_;
    std::map<std::string, KilnOwner> mod_owner_;
    std::vector<Completion> completions_;
    std::vector<std::string> events_;
    uint64_t tickets_ = 0, revision_ = 0, dropped_ = 0;

    std::mutex revoke_mu_;
    std::vector<KilnOwner> revoked_;
    std::set<KilnOwner> gone_;     // owners revoked and processed: never called back
    std::set<KilnOwner> claimed_;  // owners Kiln.Blocks named for their mod
};

}  // namespace kroutines
