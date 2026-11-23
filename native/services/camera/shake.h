// SPDX-License-Identifier: MIT
// kiln.camera's shake (chapter 03, 4.3): translation only, in the camera's own axes (+X right, +Y up, +Z forward),
// applied after the path, the profile or the game's own pose: P += Rotate(Q, offset), Q unchanged.
//
// Each axis has amplitude A / sqrt(3) and the waveform sin(2 pi f t + phi_i), so the offset's length never exceeds A.
// The three phases come from successive xorshift32 steps (x ^= x << 13; x ^= x >> 17; x ^= x << 5) of the seed, or of
// 0x9e3779b9 when the seed is 0: phi_i = (state >> 8) * 2 pi / 2^24. No random draw per frame, so equal seeds give equal
// shakes at equal times whatever the frame rate. The phase origin is T0 (a Preserve update keeps it, a Restart resets
// it). Falloff F = 0 holds the amplitude until the lease ends; F > 0 fades it linearly over the duration's last F ms.
#ifndef KILN_CAMERA_SHAKE_H
#define KILN_CAMERA_SHAKE_H

#include <cmath>
#include <cstdint>

#include "cam_math.h"
#include "services/kiln_camera.h"

namespace kcam {

struct ShakePhases {
    double phi[3];
};

inline ShakePhases shake_phases(uint32_t seed) {
    uint32_t x = seed ? seed : 0x9e3779b9u;
    ShakePhases p{};
    for (double& phi : p.phi) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        phi = static_cast<double>(x >> 8) * (2 * kPi / 16777216.0);
    }
    return p;
}

// The falloff's gain at t ms of a duration_ms lease: clamp((duration - t) / F, 0, 1), or 1 until the end without one.
inline double shake_gain(double t_ms, uint32_t duration_ms, uint32_t falloff_ms) {
    if (!falloff_ms) return t_ms < duration_ms ? 1.0 : 0.0;
    return clamp01((static_cast<double>(duration_ms) - t_ms) / falloff_ms);
}

// The offset in the camera's axes at t ms from T0.
inline Vec3 shake_offset(const KilnCameraShake& s, double t_ms, uint32_t duration_ms) {
    const ShakePhases p = shake_phases(s.seed);
    const double a = s.amplitude_metres / std::sqrt(3.0) * shake_gain(t_ms, duration_ms, s.falloff_ms);
    const double w = 2 * kPi * s.frequency_hz * (t_ms / 1000.0);
    return {a * std::sin(w + p.phi[0]), a * std::sin(w + p.phi[1]), a * std::sin(w + p.phi[2])};
}

}  // namespace kcam
#endif
