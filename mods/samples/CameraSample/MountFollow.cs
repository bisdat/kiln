// SPDX-License-Identifier: MIT
// Since mod API 0.2.17. A riding view for a big or flying mount (chapter 03, "S9 addendum"): the game's own third-person
// camera ends up inside a large flyer's body, so a Kiln client gets a render-only follow profile built from the mount's
// bounds instead. This is the sample a riding package starts from; the formula is the sample's, not Kiln API.
//
// Who knows what. The mount's author supplies its bounds (the scaled, animated enclosing sphere: centre and radius, wings
// and child pieces included), the rider's seat height and an honest top speed; the camera never guesses a radius. From
// them BuildFlyerProfile chooses a height H above the mount's origin and an absolute distance D behind it such that the
// camera stays at least 1 m outside the sphere while the sample it follows is at most 250 ms old and the mount moves no
// faster than the stated speed (chapter 03: H = max(seat + 0.75, c + R + M), D = max(8, R + H + c + M), M = 1 + v x
// 0.25 s). Any size of mount is framed: Kiln has no cap on a profile's distance or height, nor on how far the camera is
// from the rider (since 0.2.0-dev.31.25; the sample used to refuse H over 100 m, D over 200 m and 250 m from the rider).
// It only refuses bounds that are not numbers it can use; it never shrinks the view into the body.
//
// The view (MountView): FollowHeading on the mount's upright heading, 12 degrees down, no camera collision (it can clip
// terrain: an outdoor view), a 90 degree lens, a 120 s lease renewed with Restart every 30 s while riding and on a size
// change, ended before a controlled teleport and on dismount. On the server it is a remote visual for the rider's Kiln
// client (camera.remote; MountView.Start asks for nothing for a rider whose client does not offer the parts, a vanilla
// one included, and that rider keeps the stock camera); a client mod uses
// Camera.SetLocal for its own player the same way. The rider's aim and input camera never move.
using System.Numerics;
using Kiln;

namespace CameraSample;

/// <summary>What the mount's author knows of its body: never measured by the camera.</summary>
/// <param name="CenterUnscaled">The enclosing sphere's centre in the mount's local axes, before <paramref name="Scale"/>.</param>
/// <param name="RadiusUnscaled">The sphere's radius before <paramref name="Scale"/>, with wings and child render pieces.</param>
/// <param name="Scale">The mount's uniform scale (the 0.3 Fog Dragon of chapter 03's session 5: 0.3).</param>
/// <param name="SeatHeight">The rider's carry height above the mount's origin, metres.</param>
/// <param name="MaxSpeed">The mount's top speed, metres a second (a promise: faster breaks the clearance guarantee).</param>
public readonly record struct MountBounds(Vector3 CenterUnscaled, float RadiusUnscaled, float Scale, float SeatHeight = 2.6f, float MaxSpeed = 25f);

/// <summary>The S9 numbers for one mount size.</summary>
/// <param name="Radius">R: the scaled radius.</param>
/// <param name="CenterDistance">c: how far the scaled centre is from the mount's origin.</param>
/// <param name="Margin">M = 1 + MaxSpeed x 0.25 s.</param>
/// <param name="Height">H: metres above the mount's origin (the profile's world-Y offset).</param>
/// <param name="Distance">D: metres behind, absolute (the profile's distance).</param>
/// <param name="Rotation">12 degrees down about +X, on the mount's upright heading.</param>
/// <param name="Separation">How far the camera is from the rider's seat (no limit: a huge mount is framed from far away).</param>
public sealed record FlyerProfile(double Radius, double CenterDistance, double Margin, double Height, double Distance, Quaternion Rotation, double Separation);

/// <summary>The formula and the request of the S9 riding view.</summary>
public static class MountFollow
{
    /// <summary>The camera's stale-sample limit: a follow sample older than this ends the lease.</summary>
    public const double SampleAgeSeconds = 0.250;
    public const double DownPitchDegrees = 12;
    public const float LensDegrees = 90;
    public const int Priority = 100;
    public static readonly TimeSpan LeaseTime = TimeSpan.FromSeconds(120), RenewEvery = TimeSpan.FromSeconds(30), BlendOut = TimeSpan.FromSeconds(0.3);

    /// <summary>H and D for <paramref name="bounds"/> (chapter 03, S9 addendum).</summary>
    /// <exception cref="ArgumentException">A value is not finite, the radius is negative, the scale is not positive, the speed
    /// is negative, or H or D does not fit a float (no camera can be built from it): refused, never clamped.</exception>
    public static FlyerProfile BuildFlyerProfile(in MountBounds bounds)
    {
        var b = bounds;
        double cx = b.CenterUnscaled.X, cy = b.CenterUnscaled.Y, cz = b.CenterUnscaled.Z, r0 = b.RadiusUnscaled, s = b.Scale, seat = b.SeatHeight, v = b.MaxSpeed;
        if (!double.IsFinite(cx) || !double.IsFinite(cy) || !double.IsFinite(cz) || !double.IsFinite(r0) || !double.IsFinite(s) || !double.IsFinite(seat) ||
            !double.IsFinite(v) || r0 < 0 || s <= 0 || v < 0)
            throw new ArgumentException("mount bounds: every value finite, a radius of 0 or more, a positive scale, a speed of 0 or more", nameof(bounds));
        var c = Math.Sqrt(cx * s * (cx * s) + cy * s * (cy * s) + cz * s * (cz * s));
        var r = r0 * s;
        var m = 1 + v * SampleAgeSeconds;
        var h = Math.Max(seat + 0.75, c + r + m);
        var d = Math.Max(8, r + h + c + m);
        if (!float.IsFinite((float)h) || !float.IsFinite((float)d))
            throw new ArgumentException($"mount bounds: H {h} m or D {d} m does not fit a float, so no camera profile can hold it", nameof(bounds));
        var half = DownPitchDegrees * Math.PI / 360;
        var rotation = new Quaternion((float)Math.Sin(half), 0, 0, (float)Math.Cos(half));
        // the camera sits at the pivot (0, H, 0) moved D back along the down-pitched heading: (0, H + D sin 12, -D cos 12)
        // in the heading's axes, whatever the yaw; the rider sits at (0, seat, 0)
        var pitch = 2 * half;
        var separation = Math.Sqrt(Math.Pow(h + d * Math.Sin(pitch) - seat, 2) + Math.Pow(d * Math.Cos(pitch), 2));
        return new FlyerProfile(r, c, m, h, d, rotation, separation);
    }

    /// <summary>The riding view's request: follow <paramref name="mount"/>'s rendered root at <paramref name="profile"/>,
    /// with a lens unless <paramref name="lens"/> is null; no blend in (mounting shows it at once), 0.3 s out.</summary>
    public static CameraOverride Request(EntityId mount, FlyerProfile profile, float? lens = LensDegrees) =>
        new(Profile: new CameraProfile(mount, (float)profile.Distance, Vector3.Zero, (float)profile.Height, CameraRotationMode.FollowHeading, profile.Rotation,
                CameraCollisionPolicy.None),
            Lens: lens is { } fov ? new CameraLens(fov) : null, Duration: LeaseTime, Priority: Priority, Blend: new CameraBlend(TimeSpan.Zero, BlendOut),
            InputPolicy: CameraInputPolicy.VisualOnly);

    /// <summary>The capabilities a rider's client must offer for this view (19a: before a <c>Required</c> mount).</summary>
    public static CameraCapability Needs(bool lens = true) => CameraCapability.LocalProfile | (lens ? CameraCapability.LocalFov : 0);
}

/// <summary>
/// One rider's view on the server (call every member on the tick thread). Mount: <see cref="Start"/>. Every step:
/// <see cref="Tick"/> renews the lease every 30 s with Restart. A size change: <see cref="Resize"/>. Before a controlled
/// teleport and on dismount: <see cref="Stop"/> (End). Feed <see cref="Ended"/> from <c>Camera.Changed</c>.
/// </summary>
/// <remarks>
/// Trouble gets one retry; a renewal that holds earns the next trouble its own. A refused renewal keeps the lease (that is
/// the API's rule), so the same lease is renewed again, never set over (a Set would be refused AlreadyOwned). An uncertain
/// one (MayHaveApplied, TransportTimeout: the rider's client may show anything) ends the lease and sets a new one. A lease
/// that ended by itself while seated (stale samples, the client left) is set anew. A second failure in a row stops the
/// view, ends its lease, and <see cref="Failed"/> says why, for the riding code to react (end a Required ride, show a
/// message).
/// </remarks>
public sealed class MountView
{
    private enum Asked { None, Set, Update, End }

    private readonly EntityId _rider, _mount;
    private FlyerProfile _profile;
    private CameraLease _lease;
    private Task<CameraResult>? _pending;
    private Asked _asked;
    private TimeSpan _renewed;
    private bool _retried, _riding;

    private MountView(EntityId rider, EntityId mount, FlyerProfile profile)
    {
        _rider = rider;
        _mount = mount;
        _profile = profile;
    }

    /// <summary>The lease of the view (none before its first answer).</summary>
    public CameraLease Lease => _lease;

    /// <summary>Why the view stopped by itself (null while it runs or after <see cref="Stop"/>).</summary>
    public string? Failed { get; private set; }

    /// <summary>The numbers in use.</summary>
    public FlyerProfile Profile => _profile;

    /// <summary>Checks the rider's client and the bounds, then asks for the view. Null with <paramref name="why"/> when the
    /// rider cannot have it (no Kiln client with the parts, or bounds that are not usable numbers): the stock camera stays.</summary>
    public static MountView? Start(EntityId rider, EntityId mount, in MountBounds bounds, TimeSpan now, out string? why)
    {
        why = null;
        if (!Capabilities.Is("camera.service") || !Capabilities.Is("camera.server"))
        {
            why = "kiln.camera cannot serve a server's camera here: " + Capabilities.Why("camera.server");
            return null;
        }
        var have = Camera.GetCapabilities(CameraEndpoint.Remote, rider);
        if ((have & MountFollow.Needs()) != MountFollow.Needs())
        {
            why = $"the rider's client offers {have}, the riding view needs {MountFollow.Needs()}";
            return null;
        }
        FlyerProfile profile;
        try
        {
            profile = MountFollow.BuildFlyerProfile(bounds);
        }
        catch (ArgumentException e)
        {
            why = e.Message;
            return null;
        }
        var view = new MountView(rider, mount, profile) { _riding = true, _renewed = now };
        if (!view.Ask(Asked.Set))
        {
            why = view.Failed;
            return null;
        }
        return view;
    }

    /// <summary>Once a step while riding.</summary>
    public void Tick(TimeSpan now)
    {
        if (!_riding) return;
        if (_pending is { IsCompleted: true } done)
        {
            var asked = _asked;
            _pending = null;
            _asked = Asked.None;
            var r = done.Result;
            var why = $"{r.ReasonCode}: {r.Reason}";
            switch (asked)
            {
                case Asked.Set when r.Ok:
                    _lease = r.Lease;
                    break;
                case Asked.Set:
                    // a Set that may have shown something comes back with its lease: end that first
                    if (r.MayHaveApplied && r.Lease.Value != 0) _lease = r.Lease;
                    if (!Retry(now, why, _lease.Value != 0 ? Asked.End : Asked.Set)) return;
                    break;
                case Asked.Update when r.Ok:
                    _retried = false;  // the renewal held: the next trouble gets its own retry
                    break;
                case Asked.Update when r.MayHaveApplied || r.ReasonCode == CameraReason.TransportTimeout:
                    if (!Retry(now, why, Asked.End)) return;  // uncertain: end it, then set a new one
                    break;
                case Asked.Update:
                    if (!Retry(now, why, Asked.Update)) return;  // refused: the lease holds, renew it again
                    break;
                case Asked.End:
                    // the uncertain lease is gone (or was already): a new one
                    _lease = default;
                    if (!Ask(Asked.Set)) return;
                    break;
            }
        }
        if (_pending is null && _lease.Value != 0 && now - _renewed >= MountFollow.RenewEvery)
        {
            _renewed = now;
            Ask(Asked.Update);
        }
    }

    /// <summary>The view's lease ended by itself (Camera.Changed, Ended, while still riding): a new one, once.</summary>
    public void Ended(CameraChange change, TimeSpan now)
    {
        if (!_riding || change.Kind != CameraChangeKind.Ended || change.Lease.Value != _lease.Value || _lease.Value == 0) return;
        _lease = default;
        if (_asked == Asked.End) return;  // ending it anyway: its answer brings the new Set
        _pending = null;
        _asked = Asked.None;
        Retry(now, $"ended {change.EndReason}", Asked.Set);
    }

    // The one retry of this trouble; a second failure in a row stops the view (and ends its lease).
    private bool Retry(TimeSpan now, string why, Asked next)
    {
        if (_retried)
        {
            Stopped("the riding view failed twice in a row (" + why + ")");
            return false;
        }
        _retried = true;
        _renewed = now;
        return Ask(next);
    }

    private bool Ask(Asked what)
    {
        try
        {
            _pending = what switch
            {
                Asked.Set => Camera.Set(_rider, MountFollow.Request(_mount, _profile)),
                Asked.Update => Camera.Update(_lease, MountFollow.Request(_mount, _profile), CameraUpdateTiming.Restart),
                _ => Camera.End(_lease),
            };
            _asked = what;
            return true;
        }
        catch (KilnUnavailableException e)
        {
            Stopped(e.Message);
            return false;
        }
    }

    private void Stopped(string why)
    {
        _riding = false;
        _pending = null;
        _asked = Asked.None;
        Failed = why;
        if (_lease.Value == 0) return;
        try
        {
            _ = Camera.End(_lease);  // never leave the view on screen unrenewed; its answer is not needed
        }
        catch (KilnUnavailableException)
        {
            // no kiln.camera: nothing holds a camera
        }
        _lease = default;
    }

    /// <summary>The mount changed size: new numbers, the lease restarted with them.</summary>
    /// <exception cref="ArgumentException">The new bounds are not usable numbers: the caller stops the view.</exception>
    public void Resize(in MountBounds bounds, TimeSpan now)
    {
        _profile = MountFollow.BuildFlyerProfile(bounds);
        if (_lease.Value == 0 || _pending is not null) return;  // the request in flight is replaced at the next renewal
        _renewed = now;
        Ask(Asked.Update);
    }

    /// <summary>Dismount, or before a controlled teleport: the view ends (after its 0.3 s blend out).</summary>
    public Task<CameraResult>? Stop()
    {
        _riding = false;
        Failed = null;
        _pending = null;
        _asked = Asked.None;
        if (_lease.Value == 0) return null;
        try
        {
            return Camera.End(_lease);  // works while a capability is off; only a missing kiln.camera throws
        }
        catch (KilnUnavailableException)
        {
            return null;  // no kiln.camera: nothing holds a camera
        }
    }
}
