// SPDX-License-Identifier: MIT
// kiln.ui.input pad_state (0.2.0-dev.31.13): a controller as the game's last XInputGetState poll read it, normalized for
// mods (Free Climb reads the triggers and sticks). Plain logic: input.cpp keeps one View per controller under its pad
// mutex; tests/input_own_test.cpp checks it on the host.
//  - Sticks: XInput's radial dead zone taken out (left 7849, right 8689 of 32767): 0 inside, then 0..1 along the
//    stick's direction from the zone's edge to full tilt. Up and right are positive (XInput's sign).
//  - Triggers: 0 under XInput's threshold (a pull of 30 of 255, which pad_keys.h counts as pressed), then above 0 up to 1.
//  - buttons: bit (code - KILN_UI_VK_PAD_UP) of each held button, LT and RT as bits 16 and 17 from the threshold, the
//    same bits as pad_keys.h held().
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "pad_keys.h"
#include "pad_slots.h"
#include "services/kiln_ui_input.h"

namespace kui::pad {

inline void stick(int16_t x, int16_t y, int dead, float* ox, float* oy) {
    const double fx = x, fy = y, m = std::sqrt(fx * fx + fy * fy);
    if (m <= dead) {
        *ox = *oy = 0.0f;
        return;
    }
    const double s = std::fmin(1.0, (m - dead) / (32767.0 - dead)) / m;
    *ox = static_cast<float>(std::fmax(-1.0, std::fmin(1.0, fx * s)));
    *oy = static_cast<float>(std::fmax(-1.0, std::fmin(1.0, fy * s)));
}

// pressed (pad_keys.h held(): from a pull of 30) exactly when this is above 0
inline float trigger(uint8_t t) {
    constexpr int k = kiln::ui::kTriggerThreshold;
    return t < k ? 0.0f : static_cast<float>(t - k + 1) / static_cast<float>(256 - k);
}

// One controller's view: the last poll (or none), and how many.
struct View {
    bool connected = false;
    kiln::ui::PadState raw{};
    uint32_t packet = 0, hidden = 0;
    uint64_t poll_ms = 0, polls = 0;

    void poll(const kiln::ui::PadState& s, uint32_t pkt, uint32_t hidden_bits, uint64_t now_ms) {
        connected = true;
        raw = s;
        packet = pkt;
        hidden = hidden_bits;
        poll_ms = now_ms ? now_ms : 1;
        ++polls;
    }
    void lost(uint64_t now_ms) {
        connected = false;
        raw = {};
        hidden = 0;
        poll_ms = now_ms ? now_ms : 1;
        ++polls;
    }
    // Fills `out` (its size already checked by the caller); `owned`: the owners' buttons now (input_own.h pad_owned).
    void fill(uint32_t controller, uint32_t owned, KilnUiPadState* out) const {
        std::memset(out, 0, sizeof *out);
        out->size = sizeof *out;
        out->controller = controller;
        out->connected = connected ? 1 : 0;
        out->buttons = connected ? held(raw.buttons, raw.lt, raw.rt) : 0u;
        out->owned = owned;
        out->hidden = hidden | owned;
        stick(raw.lx, raw.ly, kiln::ui::kLeftDeadZone, &out->left_x, &out->left_y);
        stick(raw.rx, raw.ry, kiln::ui::kRightDeadZone, &out->right_x, &out->right_y);
        out->left_trigger = trigger(raw.lt);
        out->right_trigger = trigger(raw.rt);
        out->raw_left_x = raw.lx;
        out->raw_left_y = raw.ly;
        out->raw_right_x = raw.rx;
        out->raw_right_y = raw.ry;
        out->raw_left_trigger = raw.lt;
        out->raw_right_trigger = raw.rt;
        out->packet = packet;
        out->poll_ms = poll_ms;
        out->polls = polls;
    }
};

}  // namespace kui::pad
