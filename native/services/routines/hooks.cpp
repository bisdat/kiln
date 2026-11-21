// SPDX-License-Identifier: MIT
// kiln.routines: the hooks on the villager worker, its task chooser and the two setters it reports through.
// Evidence and the order of the game's own chooser: docs/re (villager routines), chapter 16g of the engine dossier.
//
//   Worker(ctx)             authorizes: the context's villager has an assignment and its DynamicActiveNpcState is at
//                           the address this step's Pre saw. The authorization is the thread's, for this one call.
//   DecideTask(ctx, phase)  owned and active: phase 2 runs the game's (a no-op there), phase 3 records the pause,
//                           phases 0, 1 and 4 take the plan's task. A call from inside the chooser's own setters
//                           (SetState runs the old state's exit and the new one's enter at once) chooses nothing.
//   SetState, FinishState   observe: where the task stands (started, completed, refused, taken by a safety state).
//   ProcessCommands(ctx)    a Reset or GoTo command the worker takes (seen as the SetState inside it) for an owned,
//                           active villager suspends its routine until the owner resumes it (chapter 16g §4.3); the
//                           game decides for it from that moment. Commands the game ignores change nothing.
//   SleepCheck(ctx)         the game's own check (it puts the villager to bed when it says yes) runs exactly once for
//                           a villager the service does not own, a suspended one, and one whose task is the game's own
//                           choice or Sleep. For a task of the plan that is not Sleep (Idle, Roam, Sit, Inspect) the
//                           keep-awake prefix runs instead and the answer is no: the marks and the one flag write of
//                           the game's own "no" path, no setter, no state, decision or failure count touched (chapter
//                           16g §4.1). Inside one decision the check runs once: a second call (a Sleep the game did not
//                           allow, then a vanilla fallback whose chooser asks again) gets the first call's answer.
#include "engine.h"

#include <cstring>

namespace kroutines {

namespace {

// The worker call this thread is in. Set by on_worker for the time of the game's worker, read by the nested hooks.
struct Auth {
    void* ctx = nullptr;
    const Bind* slot = nullptr;
    bool choosing = false;
    int8_t sleep_answer = -1;  // the game's SleepCheck answer in this decision: -1 not asked yet, 0 no, 1 yes
};
thread_local Auth t_auth;
// Inside the game's ProcessCommands on this thread: a SetState now is a command the worker took (Reset or GoTo).
thread_local bool t_in_command = false;

// The worker call owns the context and the plan decides for it: assigned, the address Pre saw, not suspended, and no
// manual command reached it in this step.
inline bool deciding(const Auth& a, void* ctx) noexcept {
    return a.slot && a.ctx == ctx && a.slot->active && !a.slot->runtime->manual;
}

template <class T>
inline T read(const void* base, uint32_t offset) noexcept {
    T v;
    std::memcpy(&v, static_cast<const char*>(base) + offset, sizeof v);
    return v;
}

inline uint8_t terminal_of(Kind k) noexcept {
    switch (k) {
        case Kind::Idle: return native::Idle;
        case Kind::Roam: return native::Roam;
        case Kind::Sit: return native::Sitting;
        case Kind::Inspect: return native::Interacting;
        case Kind::Sleep: return native::Sleep;
        default: return 0;
    }
}

// The states a task passes through before its terminal one.
inline bool preparatory(Kind k, uint8_t state) noexcept {
    switch (k) {
        case Kind::Sit:
        case Kind::Inspect: return state == native::FindTarget || state == native::GoToTarget;
        case Kind::Sleep: return state == native::FindBed || state == native::GoToTarget;
        default: return false;
    }
}

inline bool running(const Runtime& rt) noexcept { return rt.state == TaskState::Chosen || rt.state == TaskState::Started; }

// Hours of the day as the game turns them into its clock's nanoseconds: double(hours) * 3.6e12, cut toward zero
// (cvttsd2si; a value it cannot hold becomes INT64_MIN there, and here, without undefined behaviour).
inline int64_t hours_ns(float hours) noexcept {
    const double ns = static_cast<double>(hours) * 3.6e12;
    if (!(ns > -9.2e18 && ns < 9.2e18)) return INT64_MIN;
    return static_cast<int64_t>(ns);
}

}  // namespace

const char* task_state_name(TaskState s) {
    static const char* const names[] = {"None", "Chosen", "Started", "Completed", "Refused", "Suspended"};
    return names[static_cast<size_t>(s)];
}

const char* reason_text(Reason r) {
    static const char* const names[] = {"",        "NativeSleepNotCurrentlyAllowed", "TaskFailed",      "NoTransition", "SafetyInterrupt",
                                        "Interrupted", "Replaced",                   "ManualSuspended", "Removed",      "WorldStopped"};
    return names[static_cast<size_t>(r)];
}

void Runtime::push(TaskState to, Reason reason) noexcept {
    const TaskEvent e{invocation, window, task, state, to, reason};
    if (n_events < kEvents) events[n_events++] = e;
    else {
        events[kEvents - 1] = e;
        ++dropped;
    }
    state = to;
    if (reason != Reason::None) last_reason = reason;
}

const Bind* Engine::Snapshot::find(uint32_t npc_id) const noexcept {
    if (slots.empty() || !npc_id) return nullptr;
    const size_t mask = slots.size() - 1;
    for (size_t i = (npc_id * 0x9e3779b1u) & mask, n = 0; n < slots.size(); i = (i + 1) & mask, ++n) {
        if (slots[i].npc_id == npc_id) return &slots[i];
        if (!slots[i].npc_id) return nullptr;
    }
    return nullptr;
}

void Engine::publish(std::vector<Bind> binds, uint64_t step) {
    auto* next = new Snapshot;
    next->step = step;
    size_t size = 8;
    while (size < binds.size() * 2) size <<= 1;
    next->slots.resize(size);
    const size_t mask = size - 1;
    for (auto& b : binds) {
        if (!b.npc_id || !b.dynamic || !b.plan || !b.runtime) continue;  // not in the world this step: the game's code runs
        size_t i = (b.npc_id * 0x9e3779b1u) & mask;
        while (next->slots[i].npc_id && next->slots[i].npc_id != b.npc_id) i = (i + 1) & mask;
        next->slots[i] = std::move(b);
    }
    const Snapshot* old = live_.exchange(next, std::memory_order_acq_rel);
    delete retired_[1];
    retired_[1] = retired_[0];
    retired_[0] = old;
}

void Engine::clear() {
    const Snapshot* old = live_.exchange(nullptr, std::memory_order_acq_rel);
    delete retired_[1];
    retired_[1] = retired_[0];
    retired_[0] = old;
}

Engine::~Engine() {
    delete live_.load();
    delete retired_[0];
    delete retired_[1];
}

uint8_t Engine::state_of(void* ctx) const noexcept {
    const void* dyn = read<const void*>(ctx, layout.ctx_dynamic);
    return dyn ? read<uint8_t>(dyn, layout.dyn_state) : 0;
}

void Engine::on_worker(void* ctx) {
    stats.workers.fetch_add(1, std::memory_order_relaxed);
    const Snapshot* snap = live_.load(std::memory_order_acquire);
    // a worker inside a worker on one thread is not something the game does: stay out of it
    if (!snap || !ctx || t_auth.ctx) return game.worker_next(ctx);
    const void* tmpl = read<const void*>(ctx, layout.ctx_template);
    const void* dyn = read<const void*>(ctx, layout.ctx_dynamic);
    const Bind* slot = tmpl && dyn ? snap->find(read<uint32_t>(tmpl, 0)) : nullptr;
    if (!slot) return game.worker_next(ctx);
    // Another world's copy of the villager, or a record that moved since Pre: the game's code for this step.
    if (slot->dynamic != reinterpret_cast<uintptr_t>(dyn)) {
        stats.stale.fetch_add(1, std::memory_order_relaxed);
        return game.worker_next(ctx);
    }
    stats.owned.fetch_add(1, std::memory_order_relaxed);
    t_auth = Auth{ctx, slot, false};
    game.worker_next(ctx);
    t_auth = Auth{};
}

void Engine::on_decide(void* ctx, uint8_t phase) {
    const Auth a = t_auth;
    if (!deciding(a, ctx) || phase == native::PhaseExit) {
        stats.passed.fetch_add(1, std::memory_order_relaxed);
        return game.decide_next(ctx, phase);
    }
    Runtime& rt = *a.slot->runtime;
    if (a.choosing) {
        // the chooser's own SetState entered DecideTask again: the next update decides, not this call
        stats.nested.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (phase == native::PhasePause) {
        rt.paused = true;
        stats.paused.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (phase == native::PhaseResume) stats.resumes.fetch_add(1, std::memory_order_relaxed);
    rt.paused = false;
    choose(ctx, phase, *a.slot);
}

void Engine::choose(void* ctx, uint8_t phase, const Bind& slot) {
    Runtime& rt = *slot.runtime;
    stats.decisions.fetch_add(1, std::memory_order_relaxed);
    ++rt.decisions;
    // What the last decision started and nothing closed: the game's own choice has no end the hooks can name, so a
    // new decision is its end; a task of the plan that was still running was taken over by something else.
    if (running(rt)) rt.push(rt.task == Kind::Vanilla ? TaskState::Completed : TaskState::Suspended,
                             rt.task == Kind::Vanilla ? Reason::None : Reason::Interrupted);

    const void* time = read<const void*>(ctx, layout.ctx_time);
    const int32_t minute = time ? minute_of(read<int64_t>(time, layout.time_now)) : 0;
    int32_t window = slot.plan->select(minute);
    if (window >= 0 && window == rt.skip_window) window = -1;  // refused last time: the fallback once, then again
    rt.skip_window = Runtime::kNoSkip;
    t_auth.sleep_answer = -1;  // a new decision asks the game's sleep check afresh
    enter(ctx, phase, slot, window, slot.plan->task_at(window));
}

void Engine::enter(void* ctx, uint8_t phase, const Bind& slot, int32_t window, Kind kind) {
    Runtime& rt = *slot.runtime;
    rt.invocation = invocations_.fetch_add(1, std::memory_order_relaxed) + 1;
    rt.window = window;
    rt.task = kind;
    rt.state = TaskState::None;
    rt.push(TaskState::Chosen, Reason::None);

    t_auth.choosing = true;
    bool started = true;
    Reason refused = Reason::None;
    switch (kind) {
        case Kind::Idle:
            game.set_decision(ctx, native::WantsIdle);
            game.set_state(ctx, native::Idle);
            break;
        case Kind::Roam:
            game.set_decision(ctx, native::WantsRoam);
            game.set_state(ctx, native::Roam);
            break;
        case Kind::Sit:
        case Kind::Inspect:
            game.set_decision(ctx, kind == Kind::Sit ? native::WantsSit : native::WantsInteract);
            // FinishState takes the game's own way from DecideTask into the target search
            if (!game.finish_state(ctx, 0)) started = false, refused = Reason::NoTransition;
            break;
        case Kind::Sleep:
            // The game's own sleep check, once: when it says yes it has already taken FindBed; when it says no,
            // sleep is not allowed now and nothing forces it.
            if (!game.sleep_check(ctx)) started = false, refused = Reason::SleepNotAllowed;
            break;
        default:  // Vanilla: the game's chooser, once
            game.decide_next(ctx, phase);
            break;
    }
    t_auth.choosing = false;
    rt.native_state = state_of(ctx);

    if (started) {
        rt.push(TaskState::Started, Reason::None);
        return;
    }
    rt.push(TaskState::Refused, refused);
    if (kind == Kind::Sleep) {
        // the fallback is a task of its own, below every window; it is never Sleep (the plan's check)
        enter(ctx, phase, slot, -1, slot.plan->fallback);
        return;
    }
    rt.skip_window = window;
}

void Engine::on_set_state(void* ctx, uint8_t state) {
    const Auth a = t_auth;
    if (t_in_command && deciding(a, ctx) && a.slot->resumed) stats.dropped_intents.fetch_add(1, std::memory_order_relaxed);
    if (t_in_command && deciding(a, ctx) && !a.slot->resumed) {
        // The worker takes a Reset or GoTo for this villager (the game sets the state of a command it takes, also the
        // state it is in already): the routine is suspended until its owner resumes it, before the game's SetState runs
        // the new state's enter. From here on in this step the game decides; Post makes the suspension the assignment's.
        Runtime& rt = *a.slot->runtime;
        rt.manual = true;
        stats.manual.fetch_add(1, std::memory_order_relaxed);
        if (running(rt)) rt.push(TaskState::Suspended, Reason::ManualSuspended);
    }
    game.set_state_next(ctx, state);
    if (!a.slot || a.ctx != ctx) return;
    Runtime& rt = *a.slot->runtime;
    rt.native_state = state_of(ctx);
    if (a.choosing || !deciding(a, ctx) || !running(rt)) return;
    if (state == native::AvoidBuildingCursor || state == native::AvoidWater || state == native::InsideWater)
        rt.push(TaskState::Suspended, Reason::Safety);
}

bool Engine::on_finish_state(void* ctx, uint8_t condition) {
    const Auth a = t_auth;
    const bool watch = deciding(a, ctx) && !a.choosing;
    const uint8_t before = watch ? state_of(ctx) : 0;
    const uint64_t invocation = watch ? a.slot->runtime->invocation : 0;
    const bool moved = game.finish_state_next(ctx, condition);
    if (!watch) return moved;
    Runtime& rt = *a.slot->runtime;
    // The game's FinishState can enter DecideTask at once (its SetState runs the new state's enter): the plan then chose
    // the next task inside this call, and this finish belongs to the task before it, which that decision closed.
    if (rt.invocation != invocation) {
        rt.native_state = state_of(ctx);
        return moved;
    }
    rt.native_state = state_of(ctx);
    if (rt.state != TaskState::Started || rt.task == Kind::Vanilla || rt.task == Kind::None) return moved;
    const bool terminal = before == terminal_of(rt.task);
    if (!terminal && !preparatory(rt.task, before)) return moved;  // a state that is not this task's
    if (condition == 1) {
        rt.push(TaskState::Refused, Reason::TaskFailed);
        rt.skip_window = rt.window;
    } else if (condition == 0 && terminal) {
        rt.push(TaskState::Completed, Reason::None);
    }
    return moved;
}


bool Engine::on_sleep_check(void* ctx) {
    stats.sleep_checks.fetch_add(1, std::memory_order_relaxed);
    const Auth a = t_auth;
    if (!deciding(a, ctx)) return game.sleep_check_next(ctx);
    if (a.choosing) {
        // Inside the plan's decision: the Sleep task asks first; a vanilla fallback's chooser asks again. The game's
        // check has exit and enter handlers behind its yes, so it runs once and the second call hears its answer.
        if (a.sleep_answer >= 0) {
            stats.sleep_repeats.fetch_add(1, std::memory_order_relaxed);
            return a.sleep_answer == 1;
        }
        const bool yes = game.sleep_check_next(ctx);
        t_auth.sleep_answer = yes ? 1 : 0;
        return yes;
    }
    // The interrupt pass before the state's update (and any other caller outside a decision). Not chosen yet, the
    // game's own choice, or Sleep: the game decides. A task of the plan: it stays awake until its next decision.
    switch (a.slot->runtime->task) {
        case Kind::None:
        case Kind::Vanilla:
        case Kind::Sleep: return game.sleep_check_next(ctx);
        default: break;
    }
    stats.kept_awake.fetch_add(1, std::memory_order_relaxed);
    keep_awake(ctx);
    return false;
}

void Engine::on_commands(void* ctx) {
    stats.commands.fetch_add(1, std::memory_order_relaxed);
    if (t_in_command) return game.commands_next(ctx);  // nested: the outer call's flag holds
    t_in_command = true;
    game.commands_next(ctx);
    t_in_command = false;
}

void Engine::mark(void* ctx, uint32_t record, uint32_t tracker) {
    void* const t = read<void*>(ctx, tracker);
    if (!t || !game.mark_written) return;  // this job tracks no writes to the record
    const char* const b = static_cast<const char*>(ctx) + record;
    game.mark_written(t, read<uint64_t>(b, 8), read<uint64_t>(b, 16), read<uint64_t>(b, 24), read<uint16_t>(b, 32), read<uint8_t>(b, 34));
    stats.marks.fetch_add(1, std::memory_order_relaxed);
}

// The game's SleepCheck up to its "no", without its yes (chapter 16g §4.1, server 0x1f0490..0x1f0694): the same marks in
// the same order, the forced sleep window's flag cleared when it has run out, nothing else written. A record's pointer
// is read again after every mark: the tracker may have moved the record.
void Engine::keep_awake(void* ctx) {
    const Layout& l = layout;
    mark(ctx, l.ctx_dynamic, l.ctx_dynamic_tracker);
    const uint8_t* dyn = read<const uint8_t*>(ctx, l.ctx_dynamic);
    if (!dyn) return;
    const uint8_t state = dyn[l.dyn_state];
    if (state != native::Idle && state != native::DecideTask && state != native::Roam) {
        if (state != native::GoToTarget) return;
        mark(ctx, l.ctx_dynamic, l.ctx_dynamic_tracker);
        dyn = read<const uint8_t*>(ctx, l.ctx_dynamic);
        if (!dyn || dyn[l.dyn_decision] == native::WantsSleep) return;
    }
    const void* const time = read<const void*>(ctx, l.ctx_time);
    if (!time) return;
    const int64_t now = read<int64_t>(time, l.time_now);
    // The game reads the sleep and wake hours here to ask whether now is in the villager's sleep window; on its "no"
    // path nothing uses the answer, so only the marks around the reads are kept.
    mark(ctx, l.ctx_common, l.ctx_common_tracker);
    mark(ctx, l.ctx_common, l.ctx_common_tracker);
    uint8_t* common = read<uint8_t*>(ctx, l.ctx_common);
    if (!common || !(common[l.common_flags] & l.sleep_override_bit)) return;
    // The forced sleep window: read sleep, read wake, each after a mark; once now is past the wake hour the flag goes.
    mark(ctx, l.ctx_common, l.ctx_common_tracker);
    mark(ctx, l.ctx_common, l.ctx_common_tracker);
    common = read<uint8_t*>(ctx, l.ctx_common);
    if (!common) return;
    if (now > hours_ns(read<float>(common, l.common_wake))) {
        mark(ctx, l.ctx_common, l.ctx_common_tracker);
        common = read<uint8_t*>(ctx, l.ctx_common);
        if (common) common[l.common_flags] = static_cast<uint8_t>(common[l.common_flags] & ~l.sleep_override_bit);
    }
}

}  // namespace kroutines
