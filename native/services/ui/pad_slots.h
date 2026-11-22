// kiln.ui: the pad Kiln's screens follow while they have the input (input.cpp hk_xinput_get_state, feed_pad;
// tests/pad_slots_test.cpp). The game polls XInputGetState for each user (0..3). Every poll's state is kept per user,
// a stick back at its centre included (kiln#123: only states with a button held or a stick past the dead zone were
// kept, so ImGui went on reading the last tilt and scrolled the focused window), with the sticks inside their dead zone
// and the triggers under their threshold set to 0. Kiln follows the pad that last changed to something other than
// idle, so a second or virtual pad that sits idle no longer overwrites the first one's state.
#pragma once
#include <cstdint>
#include <cstring>

namespace kiln::ui {

/** XINPUT_GAMEPAD's layout (WORD buttons, BYTE lt, rt, SHORT lx, ly, rx, ry). */
struct PadState {
    uint16_t buttons;
    uint8_t lt, rt;
    int16_t lx, ly, rx, ry;
};
static_assert(sizeof(PadState) == 12, "XINPUT_GAMEPAD is 12 bytes");

/** XInput's own dead zones (XINPUT_GAMEPAD_LEFT/RIGHT_THUMB_DEADZONE, XINPUT_GAMEPAD_TRIGGER_THRESHOLD). */
inline constexpr int kLeftDeadZone = 7849;
inline constexpr int kRightDeadZone = 8689;
inline constexpr int kTriggerThreshold = 30;

/** `s` with the sticks inside their dead zone and the triggers under their threshold at 0 (each axis on its own). */
inline PadState settle(PadState s) {
    auto zone = [](int16_t v, int dz) -> int16_t { return (v > -dz && v < dz) ? int16_t{0} : v; };
    s.lx = zone(s.lx, kLeftDeadZone);
    s.ly = zone(s.ly, kLeftDeadZone);
    s.rx = zone(s.rx, kRightDeadZone);
    s.ry = zone(s.ry, kRightDeadZone);
    if (s.lt < kTriggerThreshold) s.lt = 0;
    if (s.rt < kTriggerThreshold) s.rt = 0;
    return s;
}

inline bool idle(const PadState& s) {
    return s.buttons == 0 && s.lt == 0 && s.rt == 0 && s.lx == 0 && s.ly == 0 && s.rx == 0 && s.ry == 0;
}

/** Each user's latest settled state and the user Kiln follows. Not thread-safe: input.cpp holds its mutex. */
class PadSlots {
public:
    /** One poll of `user` (0..3; others are ignored). */
    void update(unsigned user, const PadState& raw) {
        if (user >= 4) return;
        const PadState s = settle(raw);
        const bool changed = !seen_[user] || std::memcmp(&pads_[user], &s, sizeof s) != 0;
        pads_[user] = s;
        seen_[user] = true;
        if (active_ < 0 || (changed && !idle(s))) active_ = static_cast<int>(user);
    }
    /** The followed pad's state (idle before any poll). */
    PadState followed() const { return active_ < 0 ? PadState{} : pads_[active_]; }
    bool seen() const { return active_ >= 0; }
    int active() const { return active_; }
    void reset() { *this = PadSlots{}; }

private:
    PadState pads_[4]{};
    bool seen_[4]{};
    int active_ = -1;
};

}  // namespace kiln::ui
