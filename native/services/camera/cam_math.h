// SPDX-License-Identifier: MIT
// kiln.camera's numbers (chapter 03, 4.2-4.4): the engine's 32.32 fixed-point metres and unit quaternions. Pure
// functions, no game and no Windows: the host tests check them on the reference model's vectors.
#ifndef KILN_CAMERA_MATH_H
#define KILN_CAMERA_MATH_H

#include <cmath>
#include <cstdint>

namespace kcam {

constexpr double kFixedOne = 4294967296.0;  // 2^32: one metre in the engine's 32.32 positions

inline double fixed_to_metres(int64_t v) { return static_cast<double>(v) / kFixedOne; }

// A unit quaternion x, y, z, w (Hamilton; +Z forward, +Y up).
struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

inline double norm2(const Quat& q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }

// An engine (or derived) quaternion: finite, |norm^2 - 1| <= 0.01, then normalized. False: refuse it.
inline bool engine_unit(Quat* q) {
    const double n2 = norm2(*q);
    if (!std::isfinite(n2) || !(n2 > 0) || std::fabs(n2 - 1.0) > 0.01) return false;
    const double n = std::sqrt(n2);
    q->x /= n, q->y /= n, q->z /= n, q->w /= n;
    return true;
}

}  // namespace kcam
#endif
