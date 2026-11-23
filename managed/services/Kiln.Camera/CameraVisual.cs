// SPDX-License-Identifier: MIT
// The two pure camera helpers of chapter 03, section 5.2: CameraPose.LookAt (an orientation from a position, a target
// and an up direction) and CameraVisualSnapshot.Project (a world point on the screen of a frame's camera). They need no
// game, no capability and no world thread: they work on the values they are given. Doubles inside; +Z forward, +Y up.
using System.Numerics;

namespace Kiln;

public sealed partial record CameraPose
{
    /// <summary>A camera at <paramref name="position"/> looking at <paramref name="target"/>, with <paramref name="up"/>
    /// as the direction that is up on the screen (corrected to be square to the view). Since mod API 0.2.17.</summary>
    /// <param name="position">Where the camera is, world metres.</param>
    /// <param name="target">What it looks at; at least 0.0001 metres from <paramref name="position"/>.</param>
    /// <param name="up">Up for the camera, usually <see cref="Vector3.UnitY"/>; not along the view direction.</param>
    /// <exception cref="ArgumentException">A value is not finite, the target is the position, <paramref name="up"/> is
    /// zero, or <paramref name="up"/> points along the view (no roll is guessed for that).</exception>
    [AnyThread]
    public static CameraPose LookAt(WorldPosition position, WorldPosition target, Vector3 up)
    {
        if (!Finite(position.X, position.Y, position.Z)) throw new ArgumentException("CameraPose.LookAt: the position is not finite", nameof(position));
        if (!Finite(target.X, target.Y, target.Z)) throw new ArgumentException("CameraPose.LookAt: the target is not finite", nameof(target));
        if (!Finite(up.X, up.Y, up.Z)) throw new ArgumentException("CameraPose.LookAt: up is not finite", nameof(up));
        double fx = target.X - position.X, fy = target.Y - position.Y, fz = target.Z - position.Z;
        var distance = Math.Sqrt(fx * fx + fy * fy + fz * fz);
        if (!double.IsFinite(distance) || distance < 0.0001)
            throw new ArgumentException("CameraPose.LookAt: the target must be at least 0.0001 metres from the position", nameof(target));
        fx /= distance;
        fy /= distance;
        fz /= distance;
        double ux = up.X, uy = up.Y, uz = up.Z;
        var upLength = Math.Sqrt(ux * ux + uy * uy + uz * uz);
        if (!(upLength > 0) || !double.IsFinite(upLength)) throw new ArgumentException("CameraPose.LookAt: up is zero", nameof(up));
        ux /= upLength;
        uy /= upLength;
        uz /= upLength;
        if (Math.Abs(ux * fx + uy * fy + uz * fz) > 0.9999)
            throw new ArgumentException("CameraPose.LookAt: up points along the view direction", nameof(up));
        // right = up x forward, corrected up = forward x right: the camera's +X, +Y, +Z in the world
        double rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx;
        var rightLength = Math.Sqrt(rx * rx + ry * ry + rz * rz);
        rx /= rightLength;
        ry /= rightLength;
        rz /= rightLength;
        double cx = fy * rz - fz * ry, cy = fz * rx - fx * rz, cz = fx * ry - fy * rx;
        return new CameraPose(position, FromBasis(rx, ry, rz, cx, cy, cz, fx, fy, fz));
    }

    // The unit quaternion of the rotation whose columns are the camera's axes in the world (right, up, forward).
    private static Quaternion FromBasis(double m00, double m10, double m20, double m01, double m11, double m21, double m02, double m12, double m22)
    {
        double x, y, z, w;
        var trace = m00 + m11 + m22;
        if (trace > 0)
        {
            var s = Math.Sqrt(trace + 1) * 2;
            w = s / 4;
            x = (m21 - m12) / s;
            y = (m02 - m20) / s;
            z = (m10 - m01) / s;
        }
        else if (m00 > m11 && m00 > m22)
        {
            var s = Math.Sqrt(1 + m00 - m11 - m22) * 2;
            w = (m21 - m12) / s;
            x = s / 4;
            y = (m01 + m10) / s;
            z = (m02 + m20) / s;
        }
        else if (m11 > m22)
        {
            var s = Math.Sqrt(1 + m11 - m00 - m22) * 2;
            w = (m02 - m20) / s;
            x = (m01 + m10) / s;
            y = s / 4;
            z = (m12 + m21) / s;
        }
        else
        {
            var s = Math.Sqrt(1 + m22 - m00 - m11) * 2;
            w = (m10 - m01) / s;
            x = (m02 + m20) / s;
            y = (m12 + m21) / s;
            z = s / 4;
        }
        var n = Math.Sqrt(x * x + y * y + z * z + w * w);
        return new Quaternion((float)(x / n), (float)(y / n), (float)(z / n), (float)(w / n));
    }

    internal static bool Finite(double x, double y, double z) => double.IsFinite(x) && double.IsFinite(y) && double.IsFinite(z);
}

public sealed partial record CameraVisualSnapshot
{
    private const double MaxViewport = 32768;

    /// <summary>Where <paramref name="point"/> is on the screen of this frame's camera: the logical view the frame was
    /// drawn with (no anti-aliasing jitter, no lens distortion). Give the rectangle the game's view really fills on
    /// your surface (letterbox included); the snapshot's own aspect ratio and field of view are used for the lens.
    /// Since mod API 0.2.17.</summary>
    /// <param name="point">The world point.</param>
    /// <param name="viewportOrigin">The view rectangle's top-left corner in pixels.</param>
    /// <param name="viewportSize">Its width and height in pixels, each above 0 and at most 32768.</param>
    /// <returns>Not within depth (behind the near plane or past the far plane): no pixel. Within depth: the pixel,
    /// also when it lies outside the rectangle (<see cref="CameraProjection.OnScreen"/> false), unless it does not fit
    /// a float.</returns>
    /// <exception cref="ArgumentException">A value is not finite, the size is out of range, or the snapshot's lens or
    /// rotation is not one a frame can have.</exception>
    [AnyThread]
    public CameraProjection Project(WorldPosition point, Vector2 viewportOrigin, Vector2 viewportSize)
    {
        if (!CameraPose.Finite(point.X, point.Y, point.Z)) throw new ArgumentException("CameraVisualSnapshot.Project: the point is not finite", nameof(point));
        if (!float.IsFinite(viewportOrigin.X) || !float.IsFinite(viewportOrigin.Y))
            throw new ArgumentException("CameraVisualSnapshot.Project: the viewport origin is not finite", nameof(viewportOrigin));
        if (!(viewportSize.X > 0 && viewportSize.X <= MaxViewport && viewportSize.Y > 0 && viewportSize.Y <= MaxViewport))
            throw new ArgumentException("CameraVisualSnapshot.Project: each viewport size must be above 0 and at most 32768 pixels", nameof(viewportSize));
        var fovRadians = FieldOfViewDegrees * Math.PI / 180;
        if (!double.IsFinite(fovRadians) || fovRadians < 0.01 || fovRadians > Math.PI - 0.01)
            throw new ArgumentException($"CameraVisualSnapshot.Project: the field of view {FieldOfViewDegrees} is not a frame's");
        if (!float.IsFinite(AspectRatio) || AspectRatio < 0.05f || AspectRatio > 20f)
            throw new ArgumentException($"CameraVisualSnapshot.Project: the aspect ratio {AspectRatio} is not a frame's");
        if (!float.IsFinite(NearPlane) || !float.IsFinite(FarPlane) || !(NearPlane > 0) || !(FarPlane > NearPlane))
            throw new ArgumentException($"CameraVisualSnapshot.Project: the planes {NearPlane}..{FarPlane} are not a frame's");
        var q = Pose.Rotation;
        double qx = q.X, qy = q.Y, qz = q.Z, qw = q.W;
        var n2 = qx * qx + qy * qy + qz * qz + qw * qw;
        if (!double.IsFinite(n2) || Math.Abs(n2 - 1) > 0.01)
            throw new ArgumentException("CameraVisualSnapshot.Project: the snapshot's rotation is not a unit quaternion");
        var n = Math.Sqrt(n2);
        qx /= n;
        qy /= n;
        qz /= n;
        qw /= n;
        var c = Pose.Position;
        if (!CameraPose.Finite(c.X, c.Y, c.Z)) throw new ArgumentException("CameraVisualSnapshot.Project: the snapshot's position is not finite");
        // into the camera's axes: rotate d by the conjugate (x right, y up, z forward)
        var (x, y, z) = Rotate(-qx, -qy, -qz, qw, point.X - c.X, point.Y - c.Y, point.Z - c.Z);
        if (!(z > NearPlane) || z > FarPlane) return new CameraProjection(false, false, null, Depth(z), Frame);
        var t = Math.Tan(fovRadians / 2);
        var px = viewportOrigin.X + viewportSize.X * (0.5 + x / (2 * z * t * AspectRatio));
        var py = viewportOrigin.Y + viewportSize.Y * (0.5 - y / (2 * z * t));
        if (!double.IsFinite(px) || !double.IsFinite(py) || Math.Abs(px) > float.MaxValue || Math.Abs(py) > float.MaxValue)
            return new CameraProjection(true, false, null, Depth(z), Frame);
        var onScreen = px >= viewportOrigin.X && px <= viewportOrigin.X + (double)viewportSize.X && py >= viewportOrigin.Y && py <= viewportOrigin.Y + (double)viewportSize.Y;
        return new CameraProjection(true, onScreen, new Vector2((float)px, (float)py), Depth(z), Frame);
    }

    // Metres as a float that stays finite for any finite point (a depth past float's range reads as its largest value).
    private static float Depth(double z) => (float)Math.Clamp(z, -float.MaxValue, float.MaxValue);

    // v rotated by the unit quaternion (x, y, z, w): q v q*.
    private static (double X, double Y, double Z) Rotate(double x, double y, double z, double w, double vx, double vy, double vz)
    {
        // t = 2 * cross(q.xyz, v); v' = v + w * t + cross(q.xyz, t)
        var tx = 2 * (y * vz - z * vy);
        var ty = 2 * (z * vx - x * vz);
        var tz = 2 * (x * vy - y * vx);
        return (vx + w * tx + (y * tz - z * ty), vy + w * ty + (z * tx - x * tz), vz + w * tz + (x * ty - y * tx));
    }
}
