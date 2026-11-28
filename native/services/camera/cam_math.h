// SPDX-License-Identifier: MIT
// kiln.camera's numbers (chapter 03, 4.2-4.4): the engine's 32.32 fixed-point metres, vectors and unit quaternions
// (Hamilton, x y z w; +Z forward, +Y up), shortest-arc slerp and the blends' smoothstep. Pure functions, no game and
// no Windows: the host tests check them on the reference model's vectors (out/camera/evidence/test_camera_contract.py).
#ifndef KILN_CAMERA_MATH_H
#define KILN_CAMERA_MATH_H

#include <cmath>
#include <cstdint>

namespace kcam {

constexpr double kFixedOne = 4294967296.0;  // 2^32: one metre in the engine's 32.32 positions
constexpr double kPi = 3.14159265358979323846;
// The world bound, the one distance limit kept (owner, 0.2.0-dev.31.25: no arbitrary limits). It protects the game, it
// does not shape views: within +-1,000,000 m a camera position fits the engine's 32.32 metres with room to spare (they
// end near 2.1e9 m), a float copy of it keeps a 6 cm step and a 0.5 m grid index of it fits 32 bits easily; far past
// it the renderer's float maths would lose whole metres. The playable map is about 10 km across: no real view gets near.
constexpr double kMaxWorldMetres = 1000000.0;

inline double fixed_to_metres(int64_t v) { return static_cast<double>(v) / kFixedOne; }

// Metres to 32.32: finite and within the world policy, then times 2^32 checked against int64 before the conversion,
// ties rounded away from zero (llround). False: not representable; nothing is clamped.
inline bool metres_to_fixed(double m, int64_t* out) {
    if (!std::isfinite(m) || std::fabs(m) > kMaxWorldMetres) return false;
    const double z = m * kFixedOne;
    if (!(z >= -9223372036854775808.0 && z < 9223372036854775808.0)) return false;
    *out = static_cast<int64_t>(std::llround(z));
    return true;
}

struct Vec3 {
    double x = 0, y = 0, z = 0;
};
inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
inline bool finite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
inline Vec3 lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

// A unit quaternion x, y, z, w.
struct Quat {
    double x = 0, y = 0, z = 0, w = 1;
};

inline double norm2(const Quat& q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }
inline double dot(const Quat& a, const Quat& b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline bool finite(const Quat& q) { return std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z) && std::isfinite(q.w); }

// An engine (or derived) quaternion: finite, |norm^2 - 1| <= 0.01, then normalized. False: refuse it.
inline bool engine_unit(Quat* q) {
    const double n2 = norm2(*q);
    if (!std::isfinite(n2) || !(n2 > 0) || std::fabs(n2 - 1.0) > 0.01) return false;
    const double n = std::sqrt(n2);
    q->x /= n, q->y /= n, q->z /= n, q->w /= n;
    return true;
}

inline Quat normalize(const Quat& q) {
    const double n = std::sqrt(norm2(q));
    return {q.x / n, q.y / n, q.z / n, q.w / n};
}

// Hamilton product: rotate(a * b, v) == rotate(a, rotate(b, v)).
inline Quat mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// v rotated by the unit quaternion q: q v q*.
inline Vec3 rotate(const Quat& q, const Vec3& v) {
    const double tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty), v.y + q.w * ty + (q.z * tx - q.x * tz), v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

// Shortest-arc slerp of unit quaternions: the second is negated for a negative dot, normalized lerp above 0.9995, the
// result normalized.
inline Quat slerp(const Quat& a, Quat b, double t) {
    double d = dot(a, b);
    if (d < 0) {
        b = {-b.x, -b.y, -b.z, -b.w};
        d = -d;
    }
    if (d > 0.9995) return normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    const double angle = std::acos(d < 1 ? d : 1), s = std::sin(angle);
    const double wa = std::sin((1 - t) * angle) / s, wb = std::sin(t * angle) / s;
    return normalize({wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z, wa * a.w + wb * b.w});
}

inline double clamp01(double u) { return u < 0 ? 0 : (u > 1 ? 1 : u); }
// The blends' curve: s(u) = u^2 (3 - 2u), u clamped to [0, 1].
inline double smoothstep(double u) {
    u = clamp01(u);
    return u * u * (3 - 2 * u);
}

inline double radians(double degrees) { return degrees * (kPi / 180.0); }

}  // namespace kcam
#endif
