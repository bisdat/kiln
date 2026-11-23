// SPDX-License-Identifier: MIT
// kiln.camera's timeline (chapter 03, 4.2 blend paragraph, 4.3): where a keyed path is at a time, what a lens key
// gives, and how far a blend has gone. Times are milliseconds as doubles, from integer QueryPerformanceCounter
// nanoseconds subtracted first (the caller). Pure functions; the host tests check them on the reference vectors.
#ifndef KILN_CAMERA_TIMELINE_H
#define KILN_CAMERA_TIMELINE_H

#include <cstdint>

#include "cam_math.h"
#include "services/kiln_camera.h"

namespace kcam {

// A camera as the transaction computes it: world metres, a unit quaternion, a vertical field of view in radians.
struct View {
    Vec3 position;
    Quat rotation;
    double fov = 0;
};

inline Vec3 vec(const double p[3]) { return {p[0], p[1], p[2]}; }
inline Quat quat(const float q[4]) { return {q[0], q[1], q[2], q[3]}; }

// The path at t milliseconds from T0. Before the first key: the first key; after the last: the last (held). Inside a
// segment: positions in a straight line (doubles), orientations by shortest-arc slerp, the lens key linear in degrees
// (when keyed). keys: 1..256 with rising times, the first at 0 (validated by the core).
struct PathPoint {
    Vec3 position;
    Quat rotation;
    double fov_degrees = 0;  // keyed lens only
};
inline PathPoint path_at(const KilnCameraKey* keys, uint32_t n, double t_ms) {
    const auto at = [&](uint32_t i) { return PathPoint{vec(keys[i].pose.position), quat(keys[i].pose.rotation), keys[i].fov_y_degrees}; };
    if (n == 0) return PathPoint{};
    if (t_ms <= keys[0].at_ms) return at(0);
    if (t_ms >= keys[n - 1].at_ms) return at(n - 1);
    // the enclosing segment: the last key at or before t (binary search over rising times)
    uint32_t lo = 0, hi = n - 1;
    while (hi - lo > 1) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (keys[mid].at_ms <= t_ms) lo = mid;
        else hi = mid;
    }
    const KilnCameraKey& a = keys[lo];
    const KilnCameraKey& b = keys[hi];
    const double u = (t_ms - a.at_ms) / static_cast<double>(b.at_ms - a.at_ms);
    PathPoint p;
    p.position = lerp(vec(a.pose.position), vec(b.pose.position), u);
    p.rotation = slerp(quat(a.pose.rotation), quat(b.pose.rotation), u);
    p.fov_degrees = a.fov_y_degrees + (b.fov_y_degrees - a.fov_y_degrees) * u;
    return p;
}

// Blend in: 0 at the commit, 1 after blend_in_ms (smoothstep); no blend is 1 at once.
inline double blend_in_weight(double since_commit_ms, uint32_t blend_in_ms) {
    return blend_in_ms ? smoothstep(since_commit_ms / blend_in_ms) : 1.0;
}
// Automatic blend out: the weight of the fresh natural camera over the last blend_out_ms of the duration, 0 before.
inline double blend_out_weight(double elapsed_ms, uint32_t duration_ms, uint32_t blend_out_ms) {
    if (!blend_out_ms) return 0.0;
    return smoothstep((elapsed_ms - (static_cast<double>(duration_ms) - blend_out_ms)) / blend_out_ms);
}

// From a toward b by w: positions and the lens linear, orientations shortest-arc.
inline View mix(const View& a, const View& b, double w) {
    if (w <= 0) return a;
    if (w >= 1) return b;
    return View{lerp(a.position, b.position, w), slerp(a.rotation, b.rotation, w), a.fov + (b.fov - a.fov) * w};
}

}  // namespace kcam
#endif
