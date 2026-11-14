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
#include "engine.h"

#include <cstring>

namespace kroutines {

namespace {

// The worker call this thread is in. Set by on_worker for the time of the game's worker, read by the nested hooks.
struct Auth {
    void* ctx = nullptr;
    const Bind* slot = nullptr;
    bool choosing = false;
};
thread_local Auth t_auth;

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
    if (!a.slot || a.ctx != ctx || !a.slot->active || phase == native::PhaseExit) {
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
    game.set_state_next(ctx, state);
    const Auth a = t_auth;
    if (!a.slot || a.ctx != ctx) return;
    Runtime& rt = *a.slot->runtime;
    rt.native_state = state_of(ctx);
    if (a.choosing || !a.slot->active || !running(rt)) return;
    if (state == native::AvoidBuildingCursor || state == native::AvoidWater || state == native::InsideWater)
        rt.push(TaskState::Suspended, Reason::Safety);
}

bool Engine::on_finish_state(void* ctx, uint8_t condition) {
    const Auth a = t_auth;
    const bool watch = a.slot && a.ctx == ctx && a.slot->active && !a.choosing;
    const uint8_t before = watch ? state_of(ctx) : 0;
    const bool moved = game.finish_state_next(ctx, condition);
    if (!watch) return moved;
    Runtime& rt = *a.slot->runtime;
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

}  // namespace kroutines
