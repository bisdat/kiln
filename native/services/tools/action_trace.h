// SPDX-License-Identifier: MIT
#pragma once
// The tool action path's trace (kiln#133): one relaxed counter per exit of the TerraformImpact binding, the outcome of
// a stroke the game's own terraform ran, and the last exit. The impact hook and the carve hooks only record (atomics and
// a thread-local tally: no allocation, lock or log); the tools service's Pre publisher turns a change of state into one
// kiln.log line and the tools.actions capability text, at most once a second (a state that flips every stroke becomes
// one line naming how many changes it covers). Host-tested in tests/action_trace.cpp.
#include "include/kiln_tools_policy.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace kiln::tools {
// In the order the binding checks them, then the outcomes of a stroke the game ran.
enum class ActionExit : uint8_t {
    Unreadable,        // the impact's context could not be read, so its program is unknown
    Closed,            // tool admission is closed (service shutdown)
    Retired,           // the family was removed while the stroke started
    Faulted,           // an earlier stroke faulted inside the game's terraform; tool actions are off until restart
    NotReady,          // a hook the action needs is not installed
    NoAccess,          // the impact carries no program access, program context or build zones
    NotServerWorld,    // the impact runs in another world than the authoritative server world (or there is none)
    Layout,            // the program's arguments are not a tool program's
    Marker,            // the mode argument is not the tool program's inert marker
    NoActor,           // no acting player
    NoUsedItem,        // no UsedItem: the spawning action carried no item
    RadiusRange,       // the program's radius is not 0.25..8 m
    Position,          // the hit position is out of range
    Aliased,           // the rewritten values overlap other variables
    ReadFault,         // reading the impact or the brush faulted
    NotFamilyItem,     // UsedItem is neither the tool nor one of its brushes
    RadiusMismatch,    // no brush of the family has this radius
    OutsideBase,       // the whole brush is not inside one base area
    NoMemory,          // Kiln could not allocate or lock what the stroke needs (its policy scope or impact copy)
    ContinuationFault, // the game's terraform faulted; tool actions are off until restart
    GameStopped,       // ran: the game's terraform stopped before carving (its own cursor, permission, range or zone checks)
    NoUndoRecord,      // ran: no undo record could be created for a stroke inside a base, so the game cancelled it
    NotRemovable,      // ran: the brush reached the terrain, but no allowed cell can be mined by it
    NothingMatched,    // ran: the brush reached the terrain, and none of its allowed cells were under it
    Carved,            // ran: the brush reached the terrain; how much it removed could not be read
    Removed,           // ran: allowed terrain was removed
    Added,             // ran: the game accepted an Add stroke
    Count
};

inline const char* action_exit_name(ActionExit exit) noexcept {
    switch(exit) {
    case ActionExit::Unreadable: return "unreadable";
    case ActionExit::Closed: return "closed";
    case ActionExit::Retired: return "family removed";
    case ActionExit::Faulted: return "faulted";
    case ActionExit::NotReady: return "not ready";
    case ActionExit::NoAccess: return "no access";
    case ActionExit::NotServerWorld: return "not the server world";
    case ActionExit::Layout: return "layout";
    case ActionExit::Marker: return "mode marker";
    case ActionExit::NoActor: return "no actor";
    case ActionExit::NoUsedItem: return "no UsedItem";
    case ActionExit::RadiusRange: return "radius out of range";
    case ActionExit::Position: return "position";
    case ActionExit::Aliased: return "aliased";
    case ActionExit::ReadFault: return "read fault";
    case ActionExit::NotFamilyItem: return "not a family item";
    case ActionExit::RadiusMismatch: return "radius mismatch";
    case ActionExit::OutsideBase: return "outside base";
    case ActionExit::NoMemory: return "no memory";
    case ActionExit::ContinuationFault: return "continuation fault";
    case ActionExit::GameStopped: return "game stopped";
    case ActionExit::NoUndoRecord: return "no undo record";
    case ActionExit::NotRemovable: return "not removable";
    case ActionExit::NothingMatched: return "nothing matched";
    case ActionExit::Carved: return "carved";
    case ActionExit::Removed: return "removed";
    case ActionExit::Added: return "added";
    default: return "unknown";
    }
}
inline const char* action_exit_meaning(ActionExit exit) noexcept {
    switch(exit) {
    case ActionExit::Unreadable: return "a terrain impact's context could not be read while a tool family is bound; it was refused";
    case ActionExit::Closed: return "tool admission is closed (the service is shutting down)";
    case ActionExit::Retired: return "the tool's family was removed as the stroke started";
    case ActionExit::Faulted: return "an earlier stroke faulted inside the game's terraform; tool actions stay off until the game restarts";
    case ActionExit::NotReady: return "a hook this action needs is not installed (tools.actions says which)";
    case ActionExit::NoAccess: return "the impact carries no program access, program context or build zones";
    case ActionExit::NotServerWorld: return "the impact ran in another world than the authoritative server world, or no server world runs";
    case ActionExit::Layout: return "the program's arguments are not a tool program's";
    case ActionExit::Marker: return "the program's mode argument is not the tool program's inert marker";
    case ActionExit::NoActor: return "the impact has no acting player";
    case ActionExit::NoUsedItem: return "the impact has no UsedItem: the action that spawned it carried no item";
    case ActionExit::RadiusRange: return "the program's brush radius is not 0.25..8 m";
    case ActionExit::Position: return "the stroke's position is out of range";
    case ActionExit::Aliased: return "the program's rewritten values overlap other variables";
    case ActionExit::ReadFault: return "reading the impact or the brush faulted";
    case ActionExit::NotFamilyItem: return "UsedItem is neither the tool nor one of its brushes";
    case ActionExit::RadiusMismatch: return "UsedItem belongs to the family, but no brush of it has this radius";
    case ActionExit::OutsideBase: return "the whole brush is not inside one base area";
    case ActionExit::NoMemory: return "Kiln could not allocate or lock what the stroke needs (its policy scope or impact copy)";
    case ActionExit::ContinuationFault: return "the game's terraform faulted; tool actions stay off until the game restarts";
    case ActionExit::GameStopped: return "the game's terraform ran but stopped before carving (its own cursor, permission, range or zone checks)";
    case ActionExit::NoUndoRecord: return "no undo record could be created for a stroke inside a base, so the game cancelled it";
    case ActionExit::NotRemovable: return "the brush reached the terrain, but no allowed cell can be mined by it";
    case ActionExit::NothingMatched: return "the brush reached the terrain, and none of its allowed cells were under it";
    case ActionExit::Carved: return "the brush reached the terrain; how much it removed could not be read";
    case ActionExit::Removed: return "allowed terrain was removed";
    case ActionExit::Added: return "the game accepted the Add stroke";
    default: return "unknown";
    }
}
inline bool action_exit_dispatched(ActionExit exit) noexcept {return exit>=ActionExit::GameStopped && exit<ActionExit::Count;}

// What the terrain kernel saw during one admitted stroke, on the stroke's own thread (the game calls the kernel
// synchronously inside the impact: docs/re/terrain-tool-carve.md). The impact hook scopes it around the continuation.
struct StrokeTally {
    uint32_t kernels = 0;          // terrain kernel calls under this stroke's Remove policy
    bool removable = false;        // an allowed cell had a positive removal coefficient
    bool measured = true;          // every kernel call's removed volume could be read
    bool removed = false;          // the kernel's report grew
    bool brush_fault = false;      // a brush descriptor could not be read (that kernel call was refused)
    bool no_undo_record = false;   // no undo record could be created under the policy
};
inline thread_local StrokeTally* stroke_tally = nullptr;
// Restores the enclosing tally on return, so a nested vanilla impact never writes into this stroke's tally.
class StrokeScope {
    StrokeTally* previous_;
public:
    explicit StrokeScope(StrokeTally* tally) noexcept : previous_(stroke_tally) { stroke_tally = tally; }
    ~StrokeScope() { stroke_tally = previous_; }
    StrokeScope(const StrokeScope&) = delete;
    StrokeScope& operator=(const StrokeScope&) = delete;
};
// removed: 1 the report grew, 0 it did not, -1 it could not be read.
inline void note_kernel(bool removable, int removed) noexcept {
    auto* t = stroke_tally; if(!t) return;
    ++t->kernels; t->removable = t->removable || removable;
    if(removed<0) t->measured = false; else if(removed) t->removed = true;
}
inline void note_brush_fault() noexcept { if(auto* t = stroke_tally) { ++t->kernels; t->brush_fault = true; } }
inline void note_no_undo_record() noexcept { if(auto* t = stroke_tally) t->no_undo_record = true; }

// The outcome of a stroke whose continuation returned. game: the impact's result code (0 success, 6 cancelled,
// 0x11 internal, 0x18 missing argument; 0xff unread).
inline ActionExit stroke_outcome(uint32_t mode, const StrokeTally& t, uint8_t game) noexcept {
    if(mode==KILN_TOOL_POLICY_ADD) return game==0 ? ActionExit::Added : ActionExit::GameStopped;
    if(t.brush_fault) return ActionExit::ReadFault;
    if(!t.kernels) return t.no_undo_record ? ActionExit::NoUndoRecord : ActionExit::GameStopped;
    if(t.removed) return ActionExit::Removed;
    if(!t.removable) return ActionExit::NotRemovable;
    return t.measured ? ActionExit::NothingMatched : ActionExit::Carved;
}

// Facts about the last recorded stroke, for the log line. Zero when unknown.
struct ActionDetail {
    uint32_t program = 0, item = 0;
    float radius = 0;
    uint8_t game = 0xff;
};

class ActionTrace {
    static constexpr size_t n = size_t(ActionExit::Count);
    std::array<std::atomic<uint64_t>, n> counts_{};
    std::atomic<uint64_t> others_{}, changes_{}, published_changes_{};
    std::atomic<uint8_t> last_{uint8_t(n)};
    std::atomic<uint32_t> program_{}, item_{}, radius_bits_{};
    std::atomic<uint8_t> game_{0xff};
public:
    // Hot path: the impact hook. Never allocates, locks or logs.
    void record(ActionExit exit, const ActionDetail& detail = {}) noexcept {
        const auto i = size_t(exit); if(i>=n) return;
        counts_[i].fetch_add(1, std::memory_order_relaxed);
        program_.store(detail.program, std::memory_order_relaxed);
        item_.store(detail.item, std::memory_order_relaxed);
        uint32_t bits = 0; std::memcpy(&bits, &detail.radius, sizeof bits);
        radius_bits_.store(bits, std::memory_order_relaxed);
        game_.store(detail.game, std::memory_order_relaxed);
        if(last_.exchange(uint8_t(i), std::memory_order_relaxed)!=uint8_t(i)) changes_.fetch_add(1, std::memory_order_relaxed);
    }
    // A terrain impact of a program no family binds here (vanilla terraforming): counted, never a state of the trace.
    void other() noexcept { others_.fetch_add(1, std::memory_order_relaxed); }
    uint64_t count(ActionExit exit) const noexcept { const auto i = size_t(exit); return i<n ? counts_[i].load(std::memory_order_relaxed) : 0; }
    uint64_t others() const noexcept { return others_.load(std::memory_order_relaxed); }
    uint64_t changes() const noexcept { return changes_.load(std::memory_order_relaxed); }
    bool any() const noexcept { return last_.load(std::memory_order_relaxed)<n; }
    ActionExit last() const noexcept { const auto v = last_.load(std::memory_order_relaxed); return v<n ? ActionExit(v) : ActionExit::Count; }
    ActionDetail detail() const noexcept {
        ActionDetail d; d.program = program_.load(std::memory_order_relaxed); d.item = item_.load(std::memory_order_relaxed);
        const uint32_t bits = radius_bits_.load(std::memory_order_relaxed); std::memcpy(&d.radius, &bits, sizeof bits);
        d.game = game_.load(std::memory_order_relaxed); return d;
    }
    // The publisher's cheap check: a change of state that no take has reported yet.
    bool pending() const noexcept { return changes_.load(std::memory_order_relaxed)!=published_changes_.load(std::memory_order_relaxed); }
    // The publisher: true once per change of state since the previous take. `covered` is how many changes it stands for.
    bool take(ActionExit* state, uint64_t* covered) noexcept {
        const auto now = changes_.load(std::memory_order_relaxed), before = published_changes_.load(std::memory_order_relaxed);
        if(now==before) return false;
        published_changes_.store(now, std::memory_order_relaxed);
        if(state) *state = last();
        if(covered) *covered = now-before;
        return true;
    }
    // "removed 3, not the server world 2; other programs 7", in exit order, nonzero counters only. Allocates: publisher only.
    std::string summary() const {
        std::string text;
        for(size_t i = 0; i<n; ++i) {
            const auto c = counts_[i].load(std::memory_order_relaxed); if(!c) continue;
            if(!text.empty()) text += ", ";
            text += action_exit_name(ActionExit(i)); text += ' '; text += std::to_string(c);
        }
        if(text.empty()) text = "no tool strokes yet";
        if(const auto o = others()) text += "; other programs " + std::to_string(o);
        return text;
    }
    // " (program 0x..., UsedItem 0x..., radius 0.50 m, game result 6)" for the last stroke, the facts it has.
    std::string detail_text() const {
        const auto d = detail();
        char buffer[160]; int used = 0;
        auto add = [&](const char* format, auto... values) {
            if(used<0 || size_t(used)>=sizeof buffer) return;
            const int w = std::snprintf(buffer+used, sizeof buffer-size_t(used), format, values...);
            if(w>0) used += w;
        };
        if(d.program) add("%sprogram 0x%08x", used ? ", " : "", d.program);
        if(d.item) add("%sUsedItem 0x%08x", used ? ", " : "", d.item);
        if(d.item && d.radius>0) add("%sradius %.2f m", used ? ", " : "", double(d.radius));
        if(d.game!=0xff) add("%sgame result %u", used ? ", " : "", unsigned(d.game));
        if(used<=0) return {};
        if(size_t(used)>=sizeof buffer) used = int(sizeof buffer)-1;
        return std::string(" (")+std::string(buffer, size_t(used))+")";
    }
    void reset() noexcept {
        for(auto& c : counts_) c.store(0, std::memory_order_relaxed);
        others_.store(0); changes_.store(0); published_changes_.store(0);
        last_.store(uint8_t(n)); program_.store(0); item_.store(0); radius_bits_.store(0); game_.store(0xff);
    }
};
} // namespace kiln::tools
