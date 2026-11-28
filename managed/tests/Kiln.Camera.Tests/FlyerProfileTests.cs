// SPDX-License-Identifier: MIT
// S9: the mount-follow sample's formula against chapter 03's reference model (dev28/out/camera/evidence/
// test_camera_contract.py, build_flyer_profile and test_flyer_size_and_motion_margin; the vectors below are its output),
// its refusals, its request, and the sample's riding view on the test kit's kiln.camera.
using System.Numerics;
using CameraSample;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace CameraTests;

public sealed class FlyerProfileProbe : Mod
{
    public override void OnLoad() { }
}

public sealed class FlyerProfileTests
{
    private const double Tolerance = 1e-5;

    // ---- the reference model's geometry (profile() with mode 1: FollowHeading), in doubles ----
    private static (double X, double Y, double Z, double W) Mul((double X, double Y, double Z, double W) a, (double X, double Y, double Z, double W) b) =>
        (a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y, a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X, a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
            a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    private static (double X, double Y, double Z) Rotate((double X, double Y, double Z, double W) q, (double X, double Y, double Z) v)
    {
        var p = Mul(q, (v.X, v.Y, v.Z, 0));
        var r = Mul(p, (-q.X, -q.Y, -q.Z, q.W));
        return (r.X, r.Y, r.Z);
    }

    // the camera for a mount at the origin turned `yaw` about +Y, with the profile's D, H and rotation
    private static (double X, double Y, double Z) Camera(FlyerProfile f, double yaw)
    {
        var qt = (0.0, Math.Sin(yaw / 2), 0.0, Math.Cos(yaw / 2));
        var forward = Rotate(qt, (0, 0, 1));
        var heading = Math.Atan2(forward.X, forward.Z);
        var q = Mul((0, Math.Sin(heading / 2), 0, Math.Cos(heading / 2)), (f.Rotation.X, f.Rotation.Y, f.Rotation.Z, f.Rotation.W));
        var dir = Rotate(q, (0, 0, 1));
        return (-dir.X * f.Distance, f.Height - dir.Y * f.Distance, -dir.Z * f.Distance);
    }

    private static double Dist((double X, double Y, double Z) a, (double X, double Y, double Z) b) =>
        Math.Sqrt((a.X - b.X) * (a.X - b.X) + (a.Y - b.Y) * (a.Y - b.Y) + (a.Z - b.Z) * (a.Z - b.Z));

    [Fact]
    public void The_formula_equals_the_reference_model()
    {
        // build_flyer_profile((0,0,0), 4/.3, .3): R 4.0, c 0.0, M 7.25, H 11.25, D 22.5, rotation (sin 6 deg, 0, 0, cos 6 deg)
        var f = MountFollow.BuildFlyerProfile(new MountBounds(Vector3.Zero, 4 / 0.3f, 0.3f));
        Assert.Equal(4.0, f.Radius, Tolerance);
        Assert.Equal(0.0, f.CenterDistance, Tolerance);
        Assert.Equal(7.25, f.Margin);
        Assert.Equal(11.25, f.Height, Tolerance);
        Assert.Equal(22.5, f.Distance, Tolerance);
        Assert.Equal(0.10452846326765347, f.Rotation.X, 1e-7);
        Assert.Equal(0.9945218953682733, f.Rotation.W, 1e-7);
        Assert.Equal((0f, 0f), (f.Rotation.Y, f.Rotation.Z));
        // build_flyer_profile((1,2,0.5), 6, 0.5, 3.0, 10): R 3, c 1.14564392373896, M 3.5, H 7.64564392373896, D 15.29128784747792
        var g = MountFollow.BuildFlyerProfile(new MountBounds(new Vector3(1, 2, 0.5f), 6, 0.5f, 3.0f, 10));
        Assert.Equal(3.0, g.Radius, Tolerance);
        Assert.Equal(1.14564392373896, g.CenterDistance, Tolerance);
        Assert.Equal(3.5, g.Margin);
        Assert.Equal(7.64564392373896, g.Height, Tolerance);
        Assert.Equal(15.29128784747792, g.Distance, Tolerance);
    }

    [Fact]
    public void The_camera_stays_outside_the_body_at_every_heading()
    {
        var f = MountFollow.BuildFlyerProfile(new MountBounds(Vector3.Zero, 4 / 0.3f, 0.3f));
        // the reference model's camera positions for yaw 0, 0.5, 1, 2, 3 rad (profile(..., mode=1, rotation=cfg['rotation']))
        (double Yaw, double X, double Y, double Z)[] reference =
        [
            (0, 0.0, 15.928013043399584, -22.008321016510624),
            (0.5, -10.551351157114805, 15.928013043399584, -19.314118740575125),
            (1, -18.519363559731524, 15.928013043399584, -11.891146593506933),
            (2, -20.01210966906668, 15.928013043399584, 9.158693168734878),
            (3, -3.105814439234124, 15.928013043399584, 21.788072669119405),
        ];
        foreach (var (yaw, x, y, z) in reference)
        {
            var p = Camera(f, yaw);
            Assert.Equal(x, p.X, 1e-4);
            Assert.Equal(y, p.Y, 1e-4);
            Assert.Equal(z, p.Z, 1e-4);
            // the body may have moved up to M - 1 = 6.25 m in any direction since the sample: at least R + 1 away, and above it
            foreach (var delta in new (double X, double Y, double Z)[] { (0, 6.25, 0), (6.25, 0, 0), (0, 0, -6.25) })
            {
                Assert.True(Dist(p, delta) >= f.Radius + 1, $"yaw {yaw}: {Dist(p, delta)} from the moved centre");
                Assert.True(p.Y >= delta.Y + f.Radius + 1, $"yaw {yaw}: camera y {p.Y} not above the moved body");
            }
            Assert.Equal(f.Separation, Dist(p, (0, 2.6, 0)), 1e-4);
        }
    }

    // 0.2.0-dev.31.25: Kiln has no cap on a profile's distance or height, nor on the distance from the rider, so a huge
    // mount is framed from far away instead of being refused (the sample used to refuse H over 100 m, D over 200 m and
    // the camera more than 250 m from the rider). The reference model's "far too big" mount, radius 1000 m: H 1007.25 m.
    [Theory]
    [InlineData(0, 0, 0, 1000, 1, 2.6, 25)]               // the reference model's refusal of old: far too big for v1
    [InlineData(0, 0, 0, 0, 1, 100, 25)]                  // the seat alone puts H over 100 m
    [InlineData(0, 0, 0, 1, 1, -250, 0)]                  // the camera more than 250 m from the rider
    public void A_huge_mount_is_framed_from_far_away_never_refused(float cx, float cy, float cz, float radius, float scale, float seat, float speed)
    {
        var f = MountFollow.BuildFlyerProfile(new MountBounds(new Vector3(cx, cy, cz), radius, scale, seat, speed));
        Assert.True(f.Height >= f.Radius + f.CenterDistance + f.Margin && f.Distance >= f.Radius + f.Height + f.CenterDistance + f.Margin);
        Assert.True(f.Height > 100 || f.Separation > 250);
        // the request it makes passes Kiln.Camera's checks (no ArgumentException)
        _ = CameraRequests.Build(MountFollow.Request(new EntityId(77), f), CameraEndpoint.Local, EntityId.None, "test");
    }

    [Theory]
    [InlineData(0, 0, 0, 1e38f, 10, 2.6, 25, "float")]     // H and D do not fit a float: no camera can hold them
    [InlineData(0, 0, 0, -1, 1, 2.6, 25, "bounds")]       // a negative radius
    [InlineData(0, 0, 0, 1, 0, 2.6, 25, "bounds")]        // no scale
    [InlineData(0, 0, 0, 1, 1, 2.6, -1, "bounds")]        // a negative speed
    [InlineData(float.NaN, 0, 0, 1, 1, 2.6, 25, "bounds")]
    [InlineData(0, 0, 0, float.PositiveInfinity, 1, 2.6, 25, "bounds")]
    public void A_mount_v1_cannot_frame_is_refused_never_clamped(float cx, float cy, float cz, float radius, float scale, float seat, float speed, string why)
    {
        var e = Assert.Throws<ArgumentException>(() => MountFollow.BuildFlyerProfile(new MountBounds(new Vector3(cx, cy, cz), radius, scale, seat, speed)));
        Assert.Contains(why, e.Message);
    }

    [Fact]
    public void The_request_is_the_S9_view_and_passes_Kiln_Cameras_checks()
    {
        var f = MountFollow.BuildFlyerProfile(new MountBounds(Vector3.Zero, 4 / 0.3f, 0.3f));
        var mount = new EntityId(77);
        var r = MountFollow.Request(mount, f);
        Assert.NotNull(r.Profile);
        Assert.Equal((mount, (float)f.Distance, Vector3.Zero, (float)f.Height), (r.Profile!.Follow, r.Profile.Distance, r.Profile.OffsetLocal, r.Profile.OffsetWorldY));
        Assert.Equal((CameraRotationMode.FollowHeading, CameraCollisionPolicy.None), (r.Profile.RotationMode, r.Profile.Collision));
        Assert.Equal(f.Rotation, r.Profile.Rotation);
        Assert.Equal(90f, r.Lens!.FieldOfViewDegrees);
        Assert.Equal((TimeSpan.FromSeconds(120), 100, CameraInputPolicy.VisualOnly), (r.Duration, r.Priority, r.InputPolicy));
        Assert.Equal((TimeSpan.Zero, TimeSpan.FromSeconds(0.3)), (r.Blend!.In, r.Blend.Out));
        Assert.Null(r.Path);
        Assert.Null(r.Mode);
        Assert.Null(MountFollow.Request(mount, f, lens: null).Lens);
        // Kiln.Camera's own validation of the remote request (what Camera.Set runs before the service)
        var built = CameraRequests.Build(r, CameraEndpoint.Remote, new EntityId(5), "test");
        Assert.Equal(CameraNative.PartProfile | CameraNative.PartLens, built.Parts);
        Assert.Equal(CameraCapability.LocalProfile | CameraCapability.LocalFov, MountFollow.Needs());
        Assert.Equal(CameraCapability.LocalProfile, MountFollow.Needs(lens: false));
    }

    [Fact]
    public void The_riding_view_runs_against_the_kits_camera()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<FlyerProfileProbe>(new ModManifest("flyer-probe"));
        kiln.Step(2);
        var rider = new EntityId(5);
        var mount = new EntityId(77);
        var bounds = new MountBounds(Vector3.Zero, 4 / 0.3f, 0.3f);
        var now = TimeSpan.Zero;
        // a vanilla rider: no view, the stock camera stays
        Assert.Null(kiln.As(mod, () => MountView.Start(rider, mount, bounds, now, out var why) is { } v ? v : Fail(why)));
        // a Kiln rider without a lens: no view either (the view needs camera.local_profile and camera.local_fov)
        kiln.Camera.KilnClient(rider, (ulong)CameraCapability.LocalProfile);
        Assert.Null(kiln.As(mod, () => MountView.Start(rider, mount, bounds, now, out var why) is { } v ? v : Fail(why)));
        kiln.Camera.KilnClient(rider);
        var view = kiln.As(mod, () => MountView.Start(rider, mount, bounds, now, out _))!;
        Assert.NotNull(view);
        kiln.Step();
        kiln.As(mod, () => view.Tick(now));
        Assert.NotEqual(0ul, view.Lease.Value);
        var apply = Assert.Single(kiln.Camera.Calls);
        Assert.Equal((FakeCameraCallKind.Apply, 3u, rider, CameraNative.PartProfile | CameraNative.PartLens, 120000u, 100, 0u),
            (apply.Kind, apply.Endpoint, apply.Player, apply.Parts, apply.DurationMs, apply.Priority, apply.Refusal));
        // 29 s: nothing; 30 s: renewed with Restart
        now = TimeSpan.FromSeconds(29);
        kiln.As(mod, () => view.Tick(now));
        Assert.Single(kiln.Camera.Calls);
        now = TimeSpan.FromSeconds(30);
        kiln.As(mod, () => view.Tick(now));
        kiln.Step();
        Assert.Equal(FakeCameraCallKind.Update, kiln.Camera.Calls[^1].Kind);
        kiln.As(mod, () => view.Tick(now));
        // the view ends by itself while seated: one retry, a second end stops it
        Assert.True(kiln.Camera.Expire(view.Lease.Value));
        kiln.Step();
        var first = view.Lease;
        kiln.As(mod, () => view.Ended(new CameraChange(first, 1, CameraChangeKind.Ended, CameraEndReason.TargetLost, CameraReason.None, null, 0), now));
        kiln.Step();
        kiln.As(mod, () => view.Tick(now));
        Assert.NotEqual(first.Value, view.Lease.Value);
        Assert.Null(view.Failed);
        Assert.True(kiln.Camera.Expire(view.Lease.Value));
        kiln.Step();
        var second = view.Lease;
        kiln.As(mod, () => view.Ended(new CameraChange(second, 1, CameraChangeKind.Ended, CameraEndReason.TargetLost, CameraReason.None, null, 0), now));
        Assert.NotNull(view.Failed);
        // a fresh view, then dismount (or a controlled teleport): ended
        var again = kiln.As(mod, () => MountView.Start(rider, mount, bounds, now, out _))!;
        kiln.Step();
        kiln.As(mod, () => again.Tick(now));
        var stop = kiln.As(mod, () => again.Stop()!);
        kiln.Step();
        Assert.True(stop.Result.Ok);
        Assert.Equal(FakeCameraCallKind.End, kiln.Camera.Calls[^1].Kind);
        Assert.Empty(kiln.Camera.Leases);
    }

    // review S8 S2 (its F3, inverted): a refused renewal keeps the lease (that is the API's rule), so the view renews the same
    // lease again instead of a Set over it (AlreadyOwned); an uncertain one (TransportTimeout) ends it and sets anew; a second
    // refusal in a row stops the view and ends its lease, never leaving it on screen unrenewed.
    [Fact]
    public void A_refused_renewal_renews_the_same_lease_and_never_sets_over_it()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<FlyerProfileProbe>(new ModManifest("flyer-probe-renewal"));
        kiln.Step(2);
        var rider = new EntityId(5);
        kiln.Camera.KilnClient(rider);
        var now = TimeSpan.Zero;
        var view = kiln.As(mod, () => MountView.Start(rider, new EntityId(77), new MountBounds(Vector3.Zero, 4 / 0.3f, 0.3f), now, out _))!;
        kiln.Step();
        kiln.As(mod, () => view.Tick(now));
        var lease = view.Lease;
        Assert.NotEqual(0ul, lease.Value);
        void Renew(int seconds, uint? refuse)
        {
            if (refuse is { } r) kiln.Camera.RefuseNext(r);
            now = TimeSpan.FromSeconds(seconds);
            kiln.As(mod, () => view.Tick(now));   // the renewal
            kiln.Step();
            kiln.As(mod, () => view.Tick(now));   // its answer (a refusal: the same lease renewed again)
            kiln.Step();
            kiln.As(mod, () => view.Tick(now));
        }
        // a transient refusal (RateLimit): the same lease is renewed again and holds
        Renew(30, 22);
        Assert.Null(view.Failed);
        Assert.Equal(lease.Value, view.Lease.Value);
        Assert.Contains(lease.Value, kiln.Camera.Leases);
        Assert.DoesNotContain(kiln.Camera.Calls, c => c.Kind == FakeCameraCallKind.Apply && c.Refusal != 0);
        Assert.Equal([22u, 0u], kiln.Camera.Calls.Where(c => c.Kind == FakeCameraCallKind.Update).Select(c => c.Refusal).ToArray());
        // a renewal that held earns the next trouble its own retry
        Renew(60, 22);
        Assert.Null(view.Failed);
        Assert.Equal(lease.Value, view.Lease.Value);
        // an uncertain refusal (TransportTimeout: the client may show anything): the lease is ended, then set anew
        Renew(90, 15);
        kiln.Step();
        kiln.As(mod, () => view.Tick(now));
        Assert.Null(view.Failed);
        Assert.NotEqual(lease.Value, view.Lease.Value);
        Assert.DoesNotContain(lease.Value, kiln.Camera.Leases);
        Assert.Contains(view.Lease.Value, kiln.Camera.Leases);
        Assert.DoesNotContain(kiln.Camera.Calls, c => c.Kind == FakeCameraCallKind.Apply && c.Refusal != 0);
        // two refusals in a row: the view stops and ends its lease
        Renew(120, null);  // a renewal that holds after the recovery
        var last = view.Lease;
        kiln.Camera.RefuseNext(22);
        kiln.Camera.RefuseNext(22);
        Renew(150, null);
        kiln.Step();
        Assert.NotNull(view.Failed);
        Assert.DoesNotContain(last.Value, kiln.Camera.Leases);
        Assert.Empty(kiln.Camera.Leases);
    }

    private static MountView? Fail(string? why)
    {
        Assert.False(string.IsNullOrEmpty(why));
        return null;
    }
}
