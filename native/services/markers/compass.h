// SPDX-License-Identifier: MIT
// kiln.markers' drawn compass strip and the follow smoothing (chapter 04 §4.5 "Compass", §4.4 POSE interpolation). Pure
// math, header-only, shared by the frame callback and the host tests.
//
// The strip covers the full 360 degrees, centred horizontally at y = 54 * scale, width = min(640 * scale, 0.9 * frame
// width). A point's bearing is atan2(dx, dz) (north = +Z = 0, east = +X = pi/2); its place on the strip is
// width/2 + (wrap(bearing - heading) / 2 pi) * stripWidth with wrap into [-pi, pi). No native 40 m cut-off applies.
#ifndef KILN_MARKERS_COMPASS_H
#define KILN_MARKERS_COMPASS_H
#include <algorithm>
#include <cmath>

namespace kmk {

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kPiD = 3.14159265358979323846;

struct Strip {
    float cx = 0, y = 0, width = 0, scale = 1;
};

inline Strip strip_of(float frame_width, float scale) {
    Strip s;
    s.scale = scale > 0 && std::isfinite(scale) ? scale : 1.0f;
    s.cx = frame_width / 2;
    s.y = 54 * s.scale;
    s.width = std::min(640 * s.scale, 0.9f * frame_width);
    return s;
}

// a in [-pi, pi)
inline double wrap_pi(double a) {
    if (!std::isfinite(a)) return 0;
    a = std::fmod(a + kPiD, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a - kPiD;
}

inline double bearing_to(const double cam[3], double x, double z) { return std::atan2(x - cam[0], z - cam[2]); }

inline float strip_x(const Strip& s, double heading, double bearing) {
    return s.cx + static_cast<float>(wrap_pi(bearing - heading) / kTwoPi) * s.width;
}

// Linear from the last applied position to the newest over 100 ms, clamped (no extrapolation).
inline void interpolate(const double from[3], const double to[3], double elapsed_ms, double out[3]) {
    double t = elapsed_ms / 100.0;
    if (!(t > 0)) t = 0;
    if (t > 1) t = 1;
    for (int i = 0; i < 3; ++i) out[i] = from[i] + (to[i] - from[i]) * t;
}

}  // namespace kmk
#endif
