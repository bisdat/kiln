// SPDX-License-Identifier: MIT
// A CameraOverride as the native request: checked here first, so a mistake is an ArgumentException that names the
// member, not a bare "invalid argument" from the service. The rules are the service's (native/services/camera/state.cpp,
// validate): the two must agree, and Kiln.Camera.Tests holds this side to the numbers.
using System.Numerics;

namespace Kiln;

/// <summary>A checked request: the native struct and the keys it points to while it is pinned.</summary>
internal readonly struct CameraBuilt(CameraNative.Request request, CameraNative.Key[] keys)
{
    public readonly CameraNative.Request Request = request;
    public readonly CameraNative.Key[] Keys = keys;
    public uint Parts => Request.Parts;
    public bool IsMode => (Request.Parts & CameraNative.PartMode) != 0;
}

internal static class CameraRequests
{
    public const int DefaultDurationMs = 10_000;
    // Only guards that protect the game (0.2.0-dev.31.25: no arbitrary limits): finite numbers, the world bound on positions
    // (the engine's 32.32 metres and float maths; native cam_math.h says why), unit rotations, a lens the projection takes.
    private const double MaxMetres = 1_000_000;
    private const float MinLensDegrees = 1, MaxLensDegrees = 179;  // inside the 0.01 rad margin from 0 and 180 degrees

    /// <exception cref="ArgumentException">The request is not one the service takes.</exception>
    public static CameraBuilt Build(CameraOverride request, CameraEndpoint endpoint, EntityId player, string api)
    {
        ArgumentNullException.ThrowIfNull(request);
        ArgumentException Bad(string what) => new($"{api}: {what}", nameof(request));

        var r = new CameraNative.Request { Endpoint = (uint)endpoint, Player = player.Value };
        r.Profile.Qw = 1;
        if (request.Mode is not null) r.Parts |= CameraNative.PartMode;
        if (request.Path is not null) r.Parts |= CameraNative.PartPath;
        if (request.Profile is not null) r.Parts |= CameraNative.PartProfile;
        if (request.Shake is not null) r.Parts |= CameraNative.PartShake;
        if (request.Lens is not null) r.Parts |= CameraNative.PartLens;
        if (r.Parts == 0) throw Bad("the request has no part (Mode, Path, Profile, Lens or Shake)");
        if (!Enum.IsDefined(request.InputPolicy)) throw Bad($"InputPolicy {request.InputPolicy} is not a policy");
        r.InputPolicy = (uint)request.InputPolicy;
        if (request.Priority is < -1000 or > 1000) throw Bad($"Priority {request.Priority} is not -1000 to 1000");
        r.Priority = request.Priority;
        r.DurationMs = request.Duration == TimeSpan.Zero ? DefaultDurationMs : Millis(request.Duration, "Duration", api);
        if (r.DurationMs is < 1 or > CameraNative.MaxDurationMs) throw Bad($"Duration {request.Duration} is not 1 ms to 120 s");
        if (request.Blend is { } blend)
        {
            r.BlendInMs = Millis(blend.In, "Blend.In", api);
            r.BlendOutMs = Millis(blend.Out, "Blend.Out", api);
            if ((ulong)r.BlendInMs + r.BlendOutMs > r.DurationMs) throw Bad("Blend.In and Blend.Out together are longer than Duration");
        }

        if (request.Mode is { } mode)
        {
            if (r.Parts != CameraNative.PartMode) throw Bad("a Mode lock stands alone: no Path, Profile, Lens or Shake with it");
            if (request.InputPolicy == CameraInputPolicy.VisualOnly) throw Bad("a Mode lock needs InputPolicy.StockMode: the game's own camera mode changes");
            if (!Enum.IsDefined(mode.Mode)) throw Bad($"Mode {(byte)mode.Mode} is not a camera mode of the game");
            if (!Enum.IsDefined(mode.Transition)) throw Bad($"Transition {mode.Transition} is not a transition");
            if (r.BlendInMs != 0 || r.BlendOutMs != 0) throw Bad("a Mode lock takes no Blend: the game blends by the transition");
            r.CameraId = (byte)mode.Mode;
            r.Transition = (byte)mode.Transition;
            return new CameraBuilt(r, []);
        }

        if (request.InputPolicy == CameraInputPolicy.StockMode) throw Bad("InputPolicy.StockMode is for a Mode lock; a visual part is VisualOnly");
        if (request.Path is not null && request.Profile is not null) throw Bad("Path and Profile exclude each other");

        CameraNative.Key[] keys = [];
        var keyedLens = false;
        if (request.Path is { } path)
        {
            if (path.Keys is null || path.Keys.Count is < 1 or > CameraNative.MaxKeys) throw Bad($"a Path has 1 to {CameraNative.MaxKeys} keys");
            keys = new CameraNative.Key[path.Keys.Count];
            keyedLens = path.Keys[0]?.FieldOfViewDegrees is not null;
            uint previous = 0;
            for (var i = 0; i < keys.Length; i++)
            {
                var key = path.Keys[i] ?? throw Bad($"Path key {i} is null");
                var at = Millis(key.At, $"Path key {i}'s time", api);
                if (i == 0 ? at != 0 : at <= previous) throw Bad(i == 0 ? "the first Path key is at zero" : $"Path key {i} is not later than the key before it");
                if (at > r.DurationMs) throw Bad($"Path key {i} is after the Duration");
                previous = at;
                if (key.Pose is null) throw Bad($"Path key {i} has no Pose");
                if (key.FieldOfViewDegrees is not null != keyedLens) throw Bad("either every Path key has a field of view, or none has");
                keys[i] = new CameraNative.Key { AtMs = at, Pose = Pose(key.Pose, $"Path key {i}", api) };
                if (key.FieldOfViewDegrees is { } fov) keys[i].FovDegrees = Lens(fov, $"Path key {i}'s field of view", api);
            }
        }
        if (keyedLens)
        {
            if (request.Lens is not null) throw Bad("a Path whose keys carry a field of view takes no Lens besides");
            r.Parts |= CameraNative.PartLens;
            r.LensSource = CameraNative.LensKeys;
        }
        else if (request.Lens is { } lens)
        {
            r.LensSource = CameraNative.LensConstant;
            r.FovDegrees = Lens(lens.FieldOfViewDegrees, "Lens.FieldOfViewDegrees", api);
        }

        if (request.Profile is { } profile)
        {
            if (!Enum.IsDefined(profile.RotationMode)) throw Bad($"Profile.RotationMode {profile.RotationMode} is not a rotation mode");
            if (profile.Collision != CameraCollisionPolicy.None) throw Bad("Profile.Collision: this version has no camera collision (None)");
            if (!float.IsFinite(profile.Distance) || profile.Distance < 0f) throw Bad($"Profile.Distance {profile.Distance} is not a finite distance of 0 metres or more");
            var o = profile.OffsetLocal;
            if (!float.IsFinite(o.X) || !float.IsFinite(o.Y) || !float.IsFinite(o.Z)) throw Bad("Profile.OffsetLocal is not a finite vector");
            if (!float.IsFinite(profile.OffsetWorldY)) throw Bad($"Profile.OffsetWorldY {profile.OffsetWorldY} is not a finite number of metres");
            var q = Unit(profile.Rotation ?? Quaternion.Identity, "Profile.Rotation", api);
            if (profile.RotationMode == CameraRotationMode.FreeOrbit && (MathF.Abs(q.X) > 1e-5f || MathF.Abs(q.Y) > 1e-5f || MathF.Abs(q.Z) > 1e-5f))
                throw Bad("Profile.Rotation stays identity with RotationMode.FreeOrbit: the view keeps the game's own orbit");
            r.Profile = new CameraNative.Profile
            {
                Follow = profile.Follow.Value, RotationMode = (uint)profile.RotationMode, Collision = 0, Distance = profile.Distance,
                OffsetX = o.X, OffsetY = o.Y, OffsetZ = o.Z, OffsetWorldY = profile.OffsetWorldY, Qx = q.X, Qy = q.Y, Qz = q.Z, Qw = q.W,
            };
        }
        if (request.Shake is { } shake)
        {
            if (!float.IsFinite(shake.AmplitudeMetres) || shake.AmplitudeMetres < 0f) throw Bad($"Shake.AmplitudeMetres {shake.AmplitudeMetres} is not a finite 0 or more");
            if (!float.IsFinite(shake.FrequencyHz) || shake.FrequencyHz < 0f) throw Bad($"Shake.FrequencyHz {shake.FrequencyHz} is not a finite 0 or more");
            var falloff = Millis(shake.Falloff, "Shake.Falloff", api);
            if (falloff > r.DurationMs) throw Bad("Shake.Falloff is longer than Duration");
            r.Shake = new CameraNative.Shake { Amplitude = shake.AmplitudeMetres, Frequency = shake.FrequencyHz, FalloffMs = falloff, Seed = shake.Seed };
        }
        return new CameraBuilt(r, keys);
    }

    // A time in whole milliseconds, 0 to uint.MaxValue.
    private static uint Millis(TimeSpan t, string what, string api)
    {
        if (t < TimeSpan.Zero || t.Ticks % TimeSpan.TicksPerMillisecond != 0 || t.Ticks / TimeSpan.TicksPerMillisecond > uint.MaxValue)
            throw new ArgumentException($"{api}: {what} {t} is not a time of whole milliseconds, zero or more", "request");
        return (uint)(t.Ticks / TimeSpan.TicksPerMillisecond);
    }

    private static float Lens(float degrees, string what, string api)
    {
        if (!float.IsFinite(degrees) || degrees < MinLensDegrees || degrees > MaxLensDegrees)
            throw new ArgumentException($"{api}: {what} {degrees} is not {MinLensDegrees} to {MaxLensDegrees} degrees", "request");
        return degrees;
    }

    private static Quaternion Unit(Quaternion q, string what, string api)
    {
        var n2 = (double)q.X * q.X + (double)q.Y * q.Y + (double)q.Z * q.Z + (double)q.W * q.W;
        if (!double.IsFinite(n2) || !(n2 > 0) || Math.Abs(n2 - 1.0) > 0.001) throw new ArgumentException($"{api}: {what} {q} is not a unit quaternion", "request");
        var n = Math.Sqrt(n2);
        return new Quaternion((float)(q.X / n), (float)(q.Y / n), (float)(q.Z / n), (float)(q.W / n));
    }

    private static CameraNative.Pose Pose(CameraPose pose, string what, string api)
    {
        var p = pose.Position;
        if (!double.IsFinite(p.X) || !double.IsFinite(p.Y) || !double.IsFinite(p.Z) || Math.Abs(p.X) > MaxMetres || Math.Abs(p.Y) > MaxMetres || Math.Abs(p.Z) > MaxMetres)
            throw new ArgumentException($"{api}: {what}'s position {p} is not a finite point within 1,000,000 metres", "request");
        var q = Unit(pose.Rotation, $"{what}'s rotation", api);
        return new CameraNative.Pose { X = p.X, Y = p.Y, Z = p.Z, Qx = q.X, Qy = q.Y, Qz = q.Z, Qw = q.W };
    }
}
