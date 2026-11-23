// SPDX-License-Identifier: MIT
// CameraRequests.Build: a CameraOverride as the native request, and every way one can be wrong. The limits are the
// service's (native/services/camera/state.cpp, validate; chapter 03 section 4.4).
using System.Numerics;
using Kiln;
using Xunit;

namespace CameraTests;

public sealed class RequestTests
{
    private static readonly EntityId Player = new(5);

    private static CameraBuilt Local(CameraOverride request) => CameraRequests.Build(request, CameraEndpoint.Local, EntityId.None, "test");

    private static void Bad(CameraOverride request) => Assert.Throws<ArgumentException>(() => Local(request));

    private static CameraPose At(double x, Quaternion? q = null) => new(new WorldPosition(x, 10, 0), q ?? Quaternion.Identity);

    private static CameraKey Key(double seconds, double x, float? fov = null) => new(TimeSpan.FromSeconds(seconds), At(x), fov);

    private static CameraOverride Path(params CameraKey[] keys) => new(Path: new CameraPath(keys), Duration: TimeSpan.FromSeconds(2));

    private static CameraOverride Follow(CameraProfile profile) => new(Profile: profile);

    [Fact]
    public void A_lens_alone_is_a_whole_request()
    {
        var built = Local(new CameraOverride(Lens: new CameraLens(40)));
        var r = built.Request;
        Assert.Equal((CameraNative.PartLens, 2u, 0u, CameraNative.LensConstant, 40f), (r.Parts, r.Endpoint, r.Player, r.LensSource, r.FovDegrees));
        Assert.Equal((10000u, 0u, 0u, 0u, 0), (r.DurationMs, r.BlendInMs, r.BlendOutMs, r.InputPolicy, r.Priority));   // zero duration means 10 s
        Assert.Equal((0f, 0f, 0f, 1f), (r.Profile.Qx, r.Profile.Qy, r.Profile.Qz, r.Profile.Qw));   // what no part selects is zero, with an identity rotation
        Assert.Empty(built.Keys);
        Assert.False(built.IsMode);
    }

    [Fact]
    public void A_mode_lock_stands_alone_with_the_stock_policy()
    {
        var mode = new CameraOverride(Mode: new CameraModeRequest(CameraMode.PlayerCameraBuildingFirstPerson, CameraTransitionOverride.CameraTransitionLerpSpeed),
            Duration: TimeSpan.FromSeconds(120), Priority: -1000, InputPolicy: CameraInputPolicy.StockMode);
        var r = CameraRequests.Build(mode, CameraEndpoint.ServerMode, Player, "test").Request;
        Assert.Equal((CameraNative.PartMode, 1u, 5u, 1u, 120000u, -1000, (byte)34, (byte)2), (r.Parts, r.Endpoint, r.Player, r.InputPolicy, r.DurationMs, r.Priority, r.CameraId, r.Transition));
        Bad(mode with { InputPolicy = CameraInputPolicy.VisualOnly });                       // the game's own mode changes: say so
        Bad(mode with { Lens = new CameraLens(40) });                                         // no visual part with it
        Bad(mode with { Blend = new CameraBlend(TimeSpan.FromSeconds(1), TimeSpan.Zero) });   // the game blends by the transition
        Bad(mode with { Mode = new CameraModeRequest((CameraMode)46) });
        Bad(mode with { Mode = new CameraModeRequest(CameraMode.PlayerCameraSitting, (CameraTransitionOverride)3) });
        Bad(new CameraOverride(Lens: new CameraLens(40), InputPolicy: CameraInputPolicy.StockMode));   // and the stock policy is for a mode lock only
    }

    [Fact]
    public void Times_are_whole_milliseconds_inside_the_limits()
    {
        var lens = new CameraOverride(Lens: new CameraLens(40));
        Assert.Equal(1u, Local(lens with { Duration = TimeSpan.FromMilliseconds(1) }).Request.DurationMs);
        Assert.Equal(120000u, Local(lens with { Duration = TimeSpan.FromSeconds(120) }).Request.DurationMs);
        Bad(lens with { Duration = TimeSpan.FromMilliseconds(120001) });
        Bad(lens with { Duration = TimeSpan.FromTicks(15000) });        // 1.5 ms
        Bad(lens with { Duration = TimeSpan.FromSeconds(-1) });
        var blended = lens with { Duration = TimeSpan.FromSeconds(1), Blend = new CameraBlend(TimeSpan.FromMilliseconds(400), TimeSpan.FromMilliseconds(600)) };
        Assert.Equal((400u, 600u), (Local(blended).Request.BlendInMs, Local(blended).Request.BlendOutMs));
        Bad(blended with { Blend = new CameraBlend(TimeSpan.FromMilliseconds(401), TimeSpan.FromMilliseconds(600)) });   // longer than the duration
        Bad(blended with { Blend = new CameraBlend(TimeSpan.FromTicks(1), TimeSpan.Zero) });
        Bad(lens with { Priority = 1001 });
        Bad(lens with { Priority = -1001 });
        Bad(lens with { InputPolicy = (CameraInputPolicy)3 });
        Bad(new CameraOverride());   // no part at all
        Assert.Throws<ArgumentNullException>(() => Local(null!));
    }

    [Fact]
    public void A_lens_is_5_to_160_degrees()
    {
        Assert.Equal(5f, Local(new CameraOverride(Lens: new CameraLens(5))).Request.FovDegrees);
        Assert.Equal(160f, Local(new CameraOverride(Lens: new CameraLens(160))).Request.FovDegrees);
        foreach (var bad in new[] { 4.99f, 160.01f, float.NaN, float.PositiveInfinity, 0f, -40f }) Bad(new CameraOverride(Lens: new CameraLens(bad)));
    }

    [Fact]
    public void A_path_has_rising_keys_from_zero_inside_the_duration()
    {
        var built = Local(Path(Key(0, 1), Key(0.5, 2), Key(2, 3)));
        Assert.Equal((CameraNative.PartPath, CameraNative.LensNone), (built.Request.Parts, built.Request.LensSource));
        Assert.Equal([0u, 500u, 2000u], built.Keys.Select(k => k.AtMs));
        Assert.Equal((2.0, 10.0, 0.0, 1f, 0f), (built.Keys[1].Pose.X, built.Keys[1].Pose.Y, built.Keys[1].Pose.Z, built.Keys[1].Pose.Qw, built.Keys[1].FovDegrees));
        Assert.Single(Local(Path(Key(0, 1))).Keys);
        Bad(Path());
        Bad(Path(Key(0.1, 1)));                    // the first key is at zero
        Bad(Path(Key(0, 1), Key(0, 2)));           // strictly rising
        Bad(Path(Key(0, 1), Key(2.001, 2)));       // after the duration
        Bad(Path(Key(0, 1), new CameraKey(TimeSpan.FromTicks(5), At(2))));
        Bad(Path(Key(0, 1), null!));
        Bad(Path(Key(0, 1), new CameraKey(TimeSpan.FromSeconds(1), null!)));
        Bad(Path(Key(0, double.NaN)));
        Bad(Path(Key(0, 1_000_001)));
        Bad(Path(new CameraKey(TimeSpan.Zero, At(1, new Quaternion(0, 0, 0, 1.01f)))));   // not a unit quaternion
        Bad(Path(new CameraKey(TimeSpan.Zero, At(1, new Quaternion(0, 0, 0, 0)))));                        // the zero quaternion
        Bad(new CameraOverride(Path: new CameraPath(Enumerable.Range(0, 257).Select(i => new CameraKey(TimeSpan.FromMilliseconds(i), At(i))).ToArray())));
        Assert.Equal(256, Local(new CameraOverride(Path: new CameraPath(Enumerable.Range(0, 256).Select(i => new CameraKey(TimeSpan.FromMilliseconds(i), At(i))).ToArray()))).Keys.Length);
        // a slightly long quaternion is taken and normalized
        var long_ = Local(Path(new CameraKey(TimeSpan.Zero, At(1, new Quaternion(0, 0, 0, 1.0004f)))));
        Assert.Equal(1f, long_.Keys[0].Pose.Qw, 6);
        // keyed lenses: all or none, and no static lens besides
        var keyed = Local(Path(Key(0, 1, 50), Key(1, 2, 70)));
        Assert.Equal((CameraNative.PartPath | CameraNative.PartLens, CameraNative.LensKeys, 0f), (keyed.Request.Parts, keyed.Request.LensSource, keyed.Request.FovDegrees));
        Assert.Equal([50f, 70f], keyed.Keys.Select(k => k.FovDegrees));
        Bad(Path(Key(0, 1, 50), Key(1, 2)));
        Bad(Path(Key(0, 1), Key(1, 2, 50)));
        Bad(Path(Key(0, 1, 50), Key(1, 2, 170)));
        Bad(Path(Key(0, 1, 50)) with { Lens = new CameraLens(40) });
        Assert.Equal(CameraNative.LensConstant, Local(Path(Key(0, 1)) with { Lens = new CameraLens(40) }).Request.LensSource);   // a static lens over a plain path
        Bad(Path(Key(0, 1)) with { Profile = new CameraProfile(EntityId.None, 8, Vector3.Zero) });   // a path or a profile
    }

    [Fact]
    public void A_profile_is_absolute_metres_with_a_rotation_that_fits_its_mode()
    {
        var p = new CameraProfile(new EntityId(77), 8, new Vector3(0, 1, -2), 3, CameraRotationMode.FollowHeading, Quaternion.CreateFromAxisAngle(Vector3.UnitX, 0.2f));
        var r = Local(Follow(p)).Request;
        Assert.Equal((CameraNative.PartProfile, 77u, 1u, 0u, 8f, 3f), (r.Parts, r.Profile.Follow, r.Profile.RotationMode, r.Profile.Collision, r.Profile.Distance, r.Profile.OffsetWorldY));
        Assert.Equal((0f, 1f, -2f), (r.Profile.OffsetX, r.Profile.OffsetY, r.Profile.OffsetZ));
        Assert.True(p.LockRotation);
        var free = new CameraProfile(EntityId.None, 0.25f, Vector3.Zero);
        Assert.False(free.LockRotation);
        Assert.Equal(1f, Local(Follow(free)).Request.Profile.Qw);   // a null rotation is identity
        Assert.Equal(200f, Local(Follow(free with { Distance = 200 })).Request.Profile.Distance);
        Bad(Follow(free with { Distance = 0.24f }));
        Bad(Follow(free with { Distance = 200.5f }));
        Bad(Follow(free with { Distance = float.NaN }));
        Bad(Follow(free with { OffsetLocal = new Vector3(80, 80, 0) }));   // the vector's length, not each axis
        Assert.Equal(100f, Local(Follow(free with { OffsetLocal = new Vector3(0, 100, 0) })).Request.Profile.OffsetY);
        Bad(Follow(free with { OffsetWorldY = 100.5f }));
        Bad(Follow(free with { Collision = (CameraCollisionPolicy)1 }));
        Bad(Follow(free with { RotationMode = (CameraRotationMode)3 }));
        Bad(Follow(free with { Rotation = Quaternion.CreateFromAxisAngle(Vector3.UnitY, 0.5f) }));   // a free orbit keeps the game's orientation
        Bad(Follow(free with { RotationMode = CameraRotationMode.FixedWorld, Rotation = new Quaternion(1, 1, 1, 1) }));
        // a profile with a lens and a shake is one request
        var all = Local(Follow(free) with { Lens = new CameraLens(90), Shake = new CameraShake(0.1f, 2, TimeSpan.FromSeconds(1), 42) }).Request;
        Assert.Equal(CameraNative.PartProfile | CameraNative.PartLens | CameraNative.PartShake, all.Parts);
        Assert.Equal((0.1f, 2f, 1000u, 42u), (all.Shake.Amplitude, all.Shake.Frequency, all.Shake.FalloffMs, all.Shake.Seed));
    }

    [Fact]
    public void A_shake_is_small_and_fades_inside_the_duration()
    {
        CameraOverride Shake(float a, float f, double falloff = 0) => new(Shake: new CameraShake(a, f, TimeSpan.FromSeconds(falloff)), Duration: TimeSpan.FromSeconds(2));
        Assert.Equal(CameraNative.PartShake, Local(Shake(0.5f, 30, 2)).Request.Parts);
        Assert.Equal(0f, Local(Shake(0, 0.1f)).Request.Shake.Amplitude);
        Bad(Shake(0.51f, 1));
        Bad(Shake(-0.1f, 1));
        Bad(Shake(0.1f, 0.09f));
        Bad(Shake(0.1f, 30.5f));
        Bad(Shake(0.1f, 1, 2.001));   // longer than the duration
        Bad(Shake(float.NaN, 1));
    }
}
