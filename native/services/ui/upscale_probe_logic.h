// SPDX-License-Identifier: MIT
// kiln.ui upscaler swap: the output check's plain logic (0.2.0-dev.31.13; upscale_probe.h runs it on the GPU). Once after a
// backend's set-up, a small compute pass of Kiln's (shaders/output_probe.comp) reads a 32x32 grid of points of the game's
// colour (the upscaler's input) and of the upscaler's output, in the frame's own command buffer right after the
// upscaler's work, and writes their luminance and how many are not finite into a small host-visible buffer. Some frames
// later, when the GPU is surely done with that frame (the game waits for its command buffers in turn, upscale_logic.h
// kRetireFrames), Kiln reads it. A black or NaN output while the game's colour is not black: FSR 4 gives way to FSR 3.1
// for the rest of the game, any other upscaler to the game's own FSR 3 (upscale.cpp). Nothing of it runs per frame after
// the check. No Vulkan calls here: host test tests/upscale_test.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace kui::upscale::probe {

inline constexpr uint32_t kGrid = 32;      // points per side, over each image
inline constexpr uint32_t kThreads = 256;  // one workgroup, 4 points each
inline constexpr uint32_t kPoints = kGrid * kGrid;
static_assert(kPoints == kThreads * 4, "output_probe.comp reads 4 points an invocation");

// What one invocation of output_probe.comp writes (std430, 32 bytes).
struct Slot {
    uint32_t tag;         // the probe's tag: this slot was written by that probe
    uint32_t colour_bad;  // points of the game's colour that are not finite (NaN, Inf)
    uint32_t output_bad;  // ... of the upscaler's output
    uint32_t points;      // points it read (4)
    float colour_max, colour_sum;  // luminance (Rec. 709, below 0 as 0) of the finite points
    float output_max, output_sum;
};
static_assert(sizeof(Slot) == 32, "std430 layout of output_probe.comp's Slot");
inline constexpr uint32_t kBufferBytes = kThreads * static_cast<uint32_t>(sizeof(Slot));

struct Stats {
    uint32_t points = 0, nonfinite = 0;
    double max_lum = 0, mean_lum = 0;  // over the finite points
};
struct Result {
    Stats colour, output;
};

// The result in the buffer once every slot carries `tag` (false: the GPU has not written all of it yet).
inline bool reduce(const Slot* s, uint32_t n, uint32_t tag, Result* out) {
    if (!s || !n || !tag) return false;
    Result r;
    double csum = 0, osum = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (s[i].tag != tag) return false;
        const uint32_t p = std::min<uint32_t>(s[i].points, 4);
        r.colour.points += p;
        r.output.points += p;
        r.colour.nonfinite += std::min(s[i].colour_bad, p);
        r.output.nonfinite += std::min(s[i].output_bad, p);
        if (std::isfinite(s[i].colour_max)) r.colour.max_lum = std::max(r.colour.max_lum, static_cast<double>(s[i].colour_max));
        if (std::isfinite(s[i].output_max)) r.output.max_lum = std::max(r.output.max_lum, static_cast<double>(s[i].output_max));
        if (std::isfinite(s[i].colour_sum)) csum += s[i].colour_sum;
        if (std::isfinite(s[i].output_sum)) osum += s[i].output_sum;
    }
    const uint32_t cf = r.colour.points - r.colour.nonfinite, of = r.output.points - r.output.nonfinite;
    r.colour.mean_lum = cf ? csum / cf : 0;
    r.output.mean_lum = of ? osum / of : 0;
    *out = r;
    return true;
}

enum class Verdict {
    ok,           // the output is a picture
    black,        // the output is black (at most 2% of the colour's mean and brightest luminance)
    nan,          // a quarter of the output's points or more are not finite
    scene_black,  // the game's own colour is black (a load, a fade): no verdict, look again later
    input_bad,    // the game's own colour is mostly not finite: not the upscaler's doing, look again later
};
inline constexpr double kSceneBlack = 1e-5;  // the colour's brightest point below this: the game's picture is black
inline constexpr double kBlackRatio = 0.02;
inline Verdict judge(const Result& r) {
    const Stats &c = r.colour, &o = r.output;
    if (!c.points || !o.points || c.nonfinite * 2 > c.points) return Verdict::input_bad;
    if (!(c.max_lum > kSceneBlack)) return Verdict::scene_black;
    if (o.nonfinite * 4 >= o.points) return Verdict::nan;
    if (o.mean_lum <= c.mean_lum * kBlackRatio && o.max_lum <= c.max_lum * kBlackRatio) return Verdict::black;
    return Verdict::ok;
}
inline bool bad(Verdict v) { return v == Verdict::black || v == Verdict::nan; }
inline const char* verdict_text(Verdict v) {
    switch (v) {
        case Verdict::ok: return "the picture is fine";
        case Verdict::black: return "the picture is black";
        case Verdict::nan: return "the picture is not a number (NaN or Inf)";
        case Verdict::scene_black: return "the game's own picture is black (a load or a fade)";
        case Verdict::input_bad: return "the game's own picture is not finite";
    }
    return "?";
}
// The readout's numbers, for kiln.log.
inline std::string readout_text(const Result& r) {
    char b[256];
    std::snprintf(b, sizeof b,
                  "output: mean luminance %.3g, brightest %.3g, %u of %u points not finite; the game's colour: mean %.3g, brightest %.3g, %u not finite",
                  r.output.mean_lum, r.output.max_lum, r.output.nonfinite, r.output.points, r.colour.mean_lum, r.colour.max_lum,
                  r.colour.nonfinite);
    return b;
}

// When the check runs and is read (in the core's frame count: one per FSR frame of the game).
inline constexpr uint64_t kWarmFrames = 30;   // after a set-up's first frame: FSR's history has settled
inline constexpr uint64_t kReadFrames = 8;    // after the probe's frame: the GPU is done with it (kRetireFrames is 4)
inline constexpr uint64_t kReadGiveUp = 240;  // no complete result by then: the check is off for this context
inline constexpr uint64_t kRetryFrames = 60;  // the game's picture was black: look again a second later
inline constexpr uint32_t kMaxTries = 120;    // about two minutes of a black game picture: the check gives up
struct Schedule {
    enum class State { off, armed, pending, done };
    State state = State::off;
    uint64_t next = 0, recorded_at = 0;
    uint32_t tag = 0, tries = 0;
    // A new context (or a new upscaler behind it): the check runs kWarmFrames after `frame`.
    void arm(uint64_t frame) {
        state = State::armed;
        next = frame + kWarmFrames;
        tries = 0;
    }
    void stop() { state = State::done; }
    bool due_record(uint64_t frame) const { return state == State::armed && frame >= next; }
    void recorded(uint64_t frame, uint32_t t) {
        state = State::pending;
        recorded_at = frame;
        tag = t;
        ++tries;
    }
    bool due_read(uint64_t frame) const { return state == State::pending && frame >= recorded_at + kReadFrames; }
    bool overdue(uint64_t frame) const { return state == State::pending && frame >= recorded_at + kReadGiveUp; }
    // No verdict this time (the game's picture was black): again kRetryFrames on. False: it gave up (kMaxTries).
    bool retry(uint64_t frame) {
        if (tries >= kMaxTries) {
            state = State::done;
            return false;
        }
        state = State::armed;
        next = frame + kRetryFrames;
        return true;
    }
    bool active() const { return state == State::armed || state == State::pending; }
};

}  // namespace kui::upscale::probe
