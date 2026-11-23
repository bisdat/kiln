// SPDX-License-Identifier: MIT
// CameraPose.LookAt and CameraVisualSnapshot.Project (chapter 03, section 5.2): pure math on given values, no game.
using System.Numerics;
using Kiln;
using Xunit;

namespace CameraTests;

public sealed class CameraVisualTests
{
    private static readonly WorldPosition Origin = new(0, 0, 0);

    private static Vector3 Forward(Quaternion q) => Vector3.Transform(Vector3.UnitZ, q);
    private static Vector3 Up(Quaternion q) => Vector3.Transform(Vector3.UnitY, q);

    private static void Near(Vector3 want, Vector3 got, float eps = 1e-5f) =>
        Assert.True(Vector3.Distance(want, got) <= eps, $"want {want}, got {got}");

    // A snapshot of a camera at `at` turned by `q`: 90 degrees, square, near 0.1, far 1000.
    private static CameraVisualSnapshot Shot(Quaternion? q = null, WorldPosition? at = null, float fov = 90, float aspect = 1, float near = 0.1f, float far = 1000) =>
        new(new CameraPose(at ?? Origin, q ?? Quaternion.Identity), fov, aspect, near, far, 77, 5, 1, TimeSpan.Zero);

    private static readonly Vector2 Zero = Vector2.Zero, Hundred = new(100, 100);

    [Fact]
    public void LookAt_turns_plus_Z_onto_the_target()
    {
        // +90 degrees about world Y maps +Z to +X (the chapter's helper check)
        var east = CameraPose.LookAt(Origin, new WorldPosition(10, 0, 0), Vector3.UnitY);
        Near(Vector3.UnitX, Forward(east.Rotation));
        Near(Vector3.UnitY, Up(east.Rotation));
        Assert.Equal(Origin, east.Position);
        var half = MathF.Sqrt(0.5f);
        Assert.True(Math.Abs(Quaternion.Dot(new Quaternion(0, half, 0, half), east.Rotation)) > 0.99999f);

        var ahead = CameraPose.LookAt(Origin, new WorldPosition(0, 0, 5), Vector3.UnitY);
        Assert.True(Math.Abs(Quaternion.Dot(Quaternion.Identity, ahead.Rotation)) > 0.99999f);

        // looking down and to the side: forward is the normalized direction, up stays square to it and on the up side
        var from = new WorldPosition(100, 50, -20);
        var down = CameraPose.LookAt(from, new WorldPosition(103, 46, -20), Vector3.UnitY);
        Near(Vector3.Normalize(new Vector3(3, -4, 0)), Forward(down.Rotation));
        Assert.True(Math.Abs(Vector3.Dot(Up(down.Rotation), Forward(down.Rotation))) < 1e-5f);
        Assert.True(Up(down.Rotation).Y > 0);
        Assert.Equal(1f, down.Rotation.Length(), 5);

        // up that is not square to the view is corrected, not refused; its length does not matter
        var tilted = CameraPose.LookAt(Origin, new WorldPosition(0, 0, 1), new Vector3(0, 3, 3));
        Near(Vector3.UnitY, Up(tilted.Rotation));

        // every quadrant of the basis conversion (the trace is negative behind the camera)
        foreach (var target in new[] { new WorldPosition(0, 0, -1), new WorldPosition(-1, 0, 0), new WorldPosition(0.3, 0.2, -1), new WorldPosition(-1, -0.5, -0.2) })
        {
            var p = CameraPose.LookAt(Origin, target, Vector3.UnitY);
            var t = Vector3.Normalize(new Vector3((float)target.X, (float)target.Y, (float)target.Z));
            Near(t, Forward(p.Rotation));
            Assert.True(Up(p.Rotation).Y > 0);
        }
    }

    [Fact]
    public void LookAt_refuses_what_has_no_single_answer()
    {
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(0, 10, 0), Vector3.UnitY));          // up along the view
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(0, -10, 0), Vector3.UnitY));         // and against it
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(0, 0, 0.00005), Vector3.UnitY));     // under 0.0001 m
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(1, 0, 0), Vector3.Zero));
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(1, 0, 0), new Vector3(float.NaN, 1, 0)));
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(new WorldPosition(double.PositiveInfinity, 0, 0), new WorldPosition(1, 0, 0), Vector3.UnitY));
        Assert.Throws<ArgumentException>(() => CameraPose.LookAt(Origin, new WorldPosition(1, double.NaN, 0), Vector3.UnitY));
        // just inside the limit is fine: |dot| = 0.9998
        var f = new WorldPosition(Math.Sqrt(1 - 0.9998 * 0.9998), 0.9998, 0);
        CameraPose.LookAt(Origin, f, Vector3.UnitY);
    }

    [Fact]
    public void Project_puts_the_view_centre_in_the_middle_and_keeps_depth()
    {
        var p = Shot().Project(new WorldPosition(0, 0, 10), Zero, Hundred);
        Assert.Equal(new CameraProjection(true, true, new Vector2(50, 50), 10, 77), p);
        // 90 degrees, aspect 1: x = z is the right edge, y = z/2 a quarter from the top
        Assert.Equal(new Vector2(100, 50), Shot().Project(new WorldPosition(10, 0, 10), Zero, Hundred).Pixel);
        Assert.Equal(new Vector2(50, 25), Shot().Project(new WorldPosition(0, 5, 10), Zero, Hundred).Pixel);
        // the aspect ratio widens x only; the origin shifts the rectangle
        var wide = Shot(aspect: 2).Project(new WorldPosition(10, 0, 10), new Vector2(10, 20), new Vector2(200, 100));
        Assert.Equal(new Vector2(10 + 200 * 0.75f, 20 + 50), wide.Pixel);
        Assert.True(wide.OnScreen);
        // the camera's own pose: turned to +X and moved
        var east = Shot(q: Quaternion.CreateFromAxisAngle(Vector3.UnitY, MathF.PI / 2), at: new WorldPosition(1000, 50, -3));
        var centre = east.Project(new WorldPosition(1010, 50, -3), Zero, Hundred);
        Assert.True(centre.OnScreen);
        Assert.Equal(10f, centre.DepthMetres, 4);
        Near(new Vector3(50, 50, 0), new Vector3(centre.Pixel!.Value, 0), 1e-3f);
        // a narrower lens: tan(30 deg) at 60 degrees
        var narrow = Shot(fov: 60).Project(new WorldPosition(0, 10 * Math.Tan(Math.PI / 6), 10), Zero, Hundred);
        Assert.Equal(0f, narrow.Pixel!.Value.Y, 3);
    }

    [Fact]
    public void Project_outside_the_depth_has_no_pixel_and_outside_the_rectangle_is_not_on_screen()
    {
        Assert.Equal(new CameraProjection(false, false, null, -5, 77), Shot().Project(new WorldPosition(0, 0, -5), Zero, Hundred));   // behind
        Assert.Equal(new CameraProjection(false, false, null, 0.1f, 77), Shot().Project(new WorldPosition(0, 0, 0.1), Zero, Hundred)); // on the near plane
        Assert.False(Shot().Project(new WorldPosition(0, 0, 1000.5), Zero, Hundred).WithinDepth);                                      // past the far plane
        Assert.True(Shot().Project(new WorldPosition(0, 0, 1000), Zero, Hundred).WithinDepth);                                         // on it
        var right = Shot().Project(new WorldPosition(30, 0, 10), Zero, Hundred);
        Assert.Equal(new CameraProjection(true, false, new Vector2(200, 50), 10, 77), right);   // off-screen pixels stay available
        // a pixel that does not fit a float: none, and not on screen
        var overflow = Shot(near: 1e-30f).Project(new WorldPosition(1e30, 0, 1e-29), Zero, Hundred);
        Assert.True(overflow.WithinDepth);
        Assert.False(overflow.OnScreen);
        Assert.Null(overflow.Pixel);
    }

    [Fact]
    public void Project_refuses_bad_arguments_and_snapshots_no_frame_can_have()
    {
        var s = Shot();
        var point = new WorldPosition(0, 0, 10);
        Assert.Throws<ArgumentException>(() => s.Project(new WorldPosition(double.NaN, 0, 10), Zero, Hundred));
        Assert.Throws<ArgumentException>(() => s.Project(point, new Vector2(float.PositiveInfinity, 0), Hundred));
        Assert.Throws<ArgumentException>(() => s.Project(point, Zero, new Vector2(0, 100)));
        Assert.Throws<ArgumentException>(() => s.Project(point, Zero, new Vector2(100, 32769)));
        Assert.Throws<ArgumentException>(() => s.Project(point, Zero, new Vector2(float.NaN, 100)));
        s.Project(point, Zero, new Vector2(32768, 32768));
        Assert.Throws<ArgumentException>(() => Shot(fov: 0.5f).Project(point, Zero, Hundred));      // under 0.01 rad
        Assert.Throws<ArgumentException>(() => Shot(fov: 179.9f).Project(point, Zero, Hundred));
        Assert.Throws<ArgumentException>(() => Shot(aspect: 30).Project(point, Zero, Hundred));
        Assert.Throws<ArgumentException>(() => Shot(near: 0).Project(point, Zero, Hundred));
        Assert.Throws<ArgumentException>(() => Shot(near: 5, far: 5).Project(point, Zero, Hundred));
        Assert.Throws<ArgumentException>(() => Shot(q: new Quaternion(0, 0, 0, 2)).Project(point, Zero, Hundred));
        Assert.Throws<ArgumentException>(() => Shot(at: new WorldPosition(double.NaN, 0, 0)).Project(point, Zero, Hundred));
        // a snapshot rotation within the engine's tolerance is normalized, not refused
        Assert.True(Shot(q: new Quaternion(0, 0, 0, 1.004f)).Project(point, Zero, Hundred).OnScreen);
    }
}
