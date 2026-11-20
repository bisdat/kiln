// SPDX-License-Identifier: MIT
// kiln.routines: what runs inside the game's villager worker. The hooks' logic, apart from how the links are installed
// (routines.cpp): it sees the game through Layout (where things are in the worker's context) and Game (the functions
// to call), so tests/ run it on the host against stubs.
//
// Rules of this file (the hooks run on the game's worker threads, one job per villager):
//  - no allocation, no lock, no log line, no managed callback, nothing held across a call into the game;
//  - the worker's context is a stack object of one job: never kept, and pointers read from it are read again after
//    every call into the game (a write tracker may move a record);
//  - a context the service does not own (no assignment, another world's replica, an address that is not the one seen
//    in this step's Pre) and every phase-2 call run the game's code and nothing else;
//  - the task is entered only through the game's own setters (SetDecision, SetState, FinishState): no state write.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "plan.h"

namespace kroutines {

enum class TaskState : uint8_t { None, Chosen, Started, Completed, Refused, Suspended };
enum class Reason : uint8_t {
    None,
    SleepNotAllowed,  // NativeSleepNotCurrentlyAllowed
    TaskFailed,       // the game finished the task's state with "failed"
    NoTransition,     // the game's FinishState had no state to go to
    Safety,           // the game's interrupt pass took it (water, a build cursor)
    Interrupted,      // another state took over before the task ended (a command, the forced way home)
    Replaced,
    ManualSuspended,
    Removed,
    WorldStopped,
};
const char* task_state_name(TaskState s);
const char* reason_text(Reason r);

// keen::active_npc::State and Decision, as far as the chooser names them.
namespace native {
enum : uint8_t { FindBed = 2, Idle = 3, DecideTask = 4, FindTarget = 5, GoToTarget = 6, Roam = 7, Sleep = 8, Sitting = 9,
                 Interacting = 10, AvoidBuildingCursor = 14, AvoidWater = 15, InsideWater = 16, FinishTask = 17, StateCount = 18 };
enum : uint8_t { WantsRoam = 1, WantsSit = 2, WantsInteract = 3, WantsSleep = 5, WantsIdle = 7 };
enum : uint8_t { PhaseEnter = 0, PhaseUpdate = 1, PhaseExit = 2, PhasePause = 3, PhaseResume = 4 };
}  // namespace native

struct TaskEvent {
    uint64_t invocation;
    int32_t window;
    Kind task;
    TaskState from, to;
    Reason reason;
};

// One per assignment. Written by the one worker job that runs the villager in a step; read and reset on the tick
// thread between steps (Post waits for the step's jobs), so the fields need no atomics.
struct Runtime {
    static constexpr uint32_t kEvents = 16;
    static constexpr int32_t kNoSkip = INT32_MIN;

    uint64_t invocation = 0;
    int32_t window = -1;
    Kind task = Kind::None;
    TaskState state = TaskState::None;
    Reason last_reason = Reason::None;
    uint8_t native_state = 0;
    bool paused = false;
    // the window whose task the game refused last: the next decision in it takes the fallback once instead
    int32_t skip_window = kNoSkip;
    uint64_t decisions = 0;
    // a manual command (Reset, GoTo) reached the villager in this step: Post suspends the assignment
    bool manual = false;

    // this step's task changes, in order. When more than kEvents happen in one step the last slot is overwritten, so
    // the newest change is always there; `dropped` counts what was lost in between.
    TaskEvent events[kEvents];
    uint32_t n_events = 0;
    uint32_t dropped = 0;

    void push(TaskState to, Reason reason) noexcept;
};

// One assigned villager as this step's Pre saw it.
struct Bind {
    uint32_t npc_id = 0;
    uintptr_t dynamic = 0;  // the address of its DynamicActiveNpcState in the server world: compared, never read
    std::shared_ptr<const Plan> plan;
    bool active = false;    // false: suspended by a manual command (the game decides; the hooks only watch)
    Runtime* runtime = nullptr;
};

// Offsets into the worker's context and its records (sigdb constants routines/*).
struct Layout {
    uint32_t ctx_dynamic, ctx_common, ctx_template, ctx_time, time_now;
    uint32_t dyn_state, dyn_previous, dyn_decision;
    // The keep-awake prefix (S4): each write record's tracker object (the record's pointer + 0x28; the bookkeeping
    // words MarkWritten takes sit between them), and NpcCommon's flags byte, sleep and wake hours and the bit of the
    // game's forced sleep window.
    uint32_t ctx_dynamic_tracker = 0, ctx_common_tracker = 0;
    uint32_t common_flags = 0, common_sleep = 0, common_wake = 0;
    uint8_t sleep_override_bit = 0;
};

// The game. *_next: the rest of the hook chain and then the game's function (what a hook calls to go on).
// The others are the functions' own entries: a call goes through every hook on them, this service's included.
struct Game {
    void (*worker_next)(void* ctx);
    void (*decide_next)(void* ctx, uint8_t phase);
    void (*set_state_next)(void* ctx, uint8_t state);
    bool (*finish_state_next)(void* ctx, uint8_t condition);
    void (*set_decision)(void* ctx, uint8_t decision);
    void (*set_state)(void* ctx, uint8_t state);
    bool (*finish_state)(void* ctx, uint8_t condition);
    bool (*sleep_check)(void* ctx);
    // S4: the rest of the chain on SleepCheck, and the game's MarkWritten (a write tracker object and the record's
    // bookkeeping words, copied as they are).
    bool (*sleep_check_next)(void* ctx) = nullptr;
    void (*mark_written)(void* tracker, uint64_t a, uint64_t b, uint64_t c, uint16_t d, uint8_t e) = nullptr;
    void (*commands_next)(void* ctx) = nullptr;  // S5: the rest of the chain on ProcessCommands
};

struct Stats {
    std::atomic<uint64_t> workers{0}, owned{0}, stale{0}, decisions{0}, passed{0}, nested{0}, paused{0};
    // SleepCheck: calls seen, calls answered by the keep-awake prefix, second calls inside one decision answered from
    // the first, MarkWritten calls the prefix made
    std::atomic<uint64_t> sleep_checks{0}, kept_awake{0}, sleep_repeats{0}, marks{0};
    std::atomic<uint64_t> commands{0}, manual{0};  // ProcessCommands calls; manual commands that suspended a routine
};

class Engine {
public:
    Layout layout{};
    Game game{};
    Stats stats;

    // Tick thread, in Pre (no worker runs): the villagers the hooks own in this step. Replaces the last list.
    void publish(std::vector<Bind> binds, uint64_t step);
    // Tick thread, no worker running: nothing is owned any more.
    void clear();
    bool empty() const noexcept { return live_.load(std::memory_order_acquire) == nullptr; }

    // The hooks. Any thread.
    void on_worker(void* ctx);
    void on_decide(void* ctx, uint8_t phase);
    void on_set_state(void* ctx, uint8_t state);
    bool on_finish_state(void* ctx, uint8_t condition);
    bool on_sleep_check(void* ctx);
    void on_commands(void* ctx);

    // The invocation serial given last (tick thread reads it for tests).
    uint64_t invocations() const noexcept { return invocations_.load(std::memory_order_relaxed); }

    ~Engine();

private:
    struct Snapshot {
        uint64_t step = 0;
        std::vector<Bind> slots;  // open addressing on npc_id, a power of two; npc_id 0 = free
        const Bind* find(uint32_t npc_id) const noexcept;
    };
    void choose(void* ctx, uint8_t phase, const Bind& slot);
    void enter(void* ctx, uint8_t phase, const Bind& slot, int32_t window, Kind kind);
    uint8_t state_of(void* ctx) const noexcept;
    void keep_awake(void* ctx);
    void mark(void* ctx, uint32_t record, uint32_t tracker);

    std::atomic<const Snapshot*> live_{nullptr};
    // Lists a worker may still have loaded when the next one is published are never freed at once: the last two wait.
    const Snapshot* retired_[2] = {nullptr, nullptr};
    std::atomic<uint64_t> invocations_{0};
};

}  // namespace kroutines
