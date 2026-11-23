// SPDX-License-Identifier: MIT
// kiln.camera's follow profile geometry (chapter 03, 4.2): where a follow camera is, from the target's sampled pose.
// Distances and offsets are absolute, unscaled metres (never added to the game's own distance, never multiplied by
// the target's scale); +Z is forward, +Y up; quaternions compose Hamilton-style. Pure functions on values.
//
//   pivot    = target position + Rotate(target rotation, offset_local) + (0, offset_world_y, 0)
//   FREE_ORBIT:      Q = the game's own selected orientation this frame (the player keeps orbiting; rotation is identity)
//   FOLLOW_HEADING:  Q = Normalize(Qh * rotation), Qh the target's upright heading: its forward projected on the world's
//                    XZ plane, yaw only (its pitch and roll never tilt the view)
//   FIXED_WORLD:     Q = Normalize(rotation)
//   camera   = pivot - distance * Rotate(Q, +Z), looking along Q's +Z toward the pivot
#ifndef KILN_CAMERA_PROFILE_H
#define KILN_CAMERA_PROFILE_H

#include <cmath>

#include "cam_math.h"
#include "services/kiln_camera.h"

namespace kcam {

constexpr double kMinHeadingLength = 0.0001;  // a forward this close to vertical has no heading of its own

// The target's upright heading: (0, sin(yaw/2), 0, cos(yaw/2)) with yaw = atan2(forward.x, forward.z). False: its
// forward is (nearly) vertical (keep the last valid heading).
inline bool heading_of(const Quat& target, Quat* out) {
    const Vec3 f = rotate(target, {0, 0, 1});
    const double h = std::sqrt(f.x * f.x + f.z * f.z);
    if (!std::isfinite(h) || h < kMinHeadingLength) return false;
    const double yaw = std::atan2(f.x / h, f.z / h);
    *out = Quat{0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    return true;
}

// The heading a FOLLOW_HEADING profile keeps between frames: reset on a new world, another target, another rotation
// mode and a Restart; kept across a Preserve update of the same lease.
struct HeadingMemory {
    bool valid = false;
    Quat heading;
};

enum class ProfileResult { Ok, NoHeading };

// The camera of `profile` for a target at (pt, qt) with the game's selected orientation `stock`. NoHeading: a
// FOLLOW_HEADING profile whose target points straight up or down and that has no earlier heading (TargetUnavailable).
inline ProfileResult profile_view(const KilnCameraProfile& profile, const Vec3& pt, const Quat& qt, const Quat& stock, HeadingMemory* memory, Vec3* position,
                                  Quat* rotation) {
    const Quat offset{profile.rotation[0], profile.rotation[1], profile.rotation[2], profile.rotation[3]};
    Quat q;
    switch (profile.rotation_mode) {
    case KILN_CAMERA_FOLLOW_HEADING: {
        Quat h;
        if (heading_of(qt, &h)) {
            memory->valid = true;
            memory->heading = h;
        } else if (!memory->valid) {
            return ProfileResult::NoHeading;
        }
        q = normalize(mul(memory->heading, offset));
        break;
    }
    case KILN_CAMERA_FIXED_WORLD: q = normalize(offset); break;
    default: q = stock; break;  // FREE_ORBIT
    }
    const Vec3 local{profile.offset_local[0], profile.offset_local[1], profile.offset_local[2]};
    const Vec3 pivot = pt + rotate(qt, local) + Vec3{0, profile.offset_world_y, 0};
    *position = pivot - rotate(q, {0, 0, 1}) * static_cast<double>(profile.distance);
    *rotation = q;
    return ProfileResult::Ok;
}

}  // namespace kcam
#endif
