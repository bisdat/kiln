// SPDX-License-Identifier: MIT
// kiln_mid_frames.hpp: pairs two mid hooks inside one call of a function (one right after its prologue, one right before
// its epilogue) by the function's own rsp, which is the same at both sites, so what the first site takes reaches the
// second site of the same call. A service uses it to wrap a function from inside instead of hooking its entry, which
// other mods find by its first bytes (docs/kiln/engines/README.md, "Mods that find game code by its bytes").
//
//   struct Call { explicit Call(void* groups); ... };
//   thread_local kiln::MidFrames<Call> calls;                // one per thread: the game may run the function anywhere
//   void hk_body(KilnRegisters& r) { calls.enter(r.rsp, reinterpret_cast<void*>(r.rcx)); }
//   void hk_done(KilnRegisters& r) { if (Call* c = calls.leave(r.rsp)) { ...; calls.pop(); } }
//
// Rules:
//  - Per thread, at most N calls are open at once (nested calls of the function). One more is not tracked: its first site
//    returns nullptr and the call stays the game's.
//  - A second site with no first (a hook that went in while a call was already past the first site) finds nothing.
//    Install the second site's hook before the first's, so no call is ever left open by the install.
//  - A first site whose second never came (a call that left the function another way) is dropped by the next call on
//    that thread that cannot be inside it: an enclosing call has a higher rsp.
//  - T is built in place (it may hold scopes that neither move nor copy) and destroyed by pop() or by a drop.
// Header-only, no allocation, no locks. Not a mod API: Kiln's services share it.
#ifndef KILN_MID_FRAMES_HPP
#define KILN_MID_FRAMES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace kiln {

template <class T, size_t N = 4>
class MidFrames {
public:
    MidFrames() = default;
    MidFrames(const MidFrames&) = delete;
    MidFrames& operator=(const MidFrames&) = delete;

    // The first site of the call whose rsp is `frame`: drops this thread's open calls that cannot enclose it, then builds
    // its T from `args`. nullptr: N calls are open, `frame` is 0, or T's constructor threw (nothing is kept).
    template <class... Args>
    T* enter(uintptr_t frame, Args&&... args) noexcept {
        while (depth_ && frames_[depth_ - 1] <= frame) pop();
        if (!frame || depth_ == N) return nullptr;
        try {
            slots_[depth_].emplace(std::forward<Args>(args)...);
        } catch (...) {
            slots_[depth_].reset();
            return nullptr;
        }
        frames_[depth_] = frame;
        return &*slots_[depth_++];
    }

    // The second site of the call whose rsp is `frame`: drops the calls inside it that never reached theirs, then returns
    // its own T (pop() it when done), or nullptr when its first site did not run.
    T* leave(uintptr_t frame) noexcept {
        while (depth_ && frames_[depth_ - 1] < frame) pop();
        return depth_ && frames_[depth_ - 1] == frame ? &*slots_[depth_ - 1] : nullptr;
    }

    // Closes the innermost open call.
    void pop() noexcept {
        if (!depth_) return;
        --depth_;
        slots_[depth_].reset();
        frames_[depth_] = 0;
    }

    size_t depth() const noexcept { return depth_; }

private:
    std::array<std::optional<T>, N> slots_{};
    std::array<uintptr_t, N> frames_{};
    size_t depth_ = 0;
};

}  // namespace kiln

#endif /* KILN_MID_FRAMES_HPP */
