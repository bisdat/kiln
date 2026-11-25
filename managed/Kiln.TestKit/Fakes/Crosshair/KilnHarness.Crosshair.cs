// SPDX-License-Identifier: MIT
using System.Numerics;

namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    /// <summary>The fake kiln.client.crosshair of this harness: what the game's crosshair hits, as the test aims it.
    /// Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    public HarnessCrosshair Crosshair => new(this);
}

/// <summary>Values the kit's crosshair fake gives every sample. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
public static class FakeCrosshair
{
    /// <summary>The client session of every sample (<c>CrosshairHit.ClientSession</c>).</summary>
    public const uint Session = 1;
}

/// <summary>
/// The fake kiln.client.crosshair of a harness (<see cref="KilnHarness.Crosshair"/>): Kiln.Client's
/// <c>Crosshair.Read</c> and <c>Crosshair.TryGet</c> run on it unchanged, from any thread. Since 0.2.0-dev.31.13, mod
/// API 0.2.22.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> No sample until the test aims. Each <see cref="Aim"/> or <see cref="AimAtNothing"/> is one game
/// trace: the next sequence, the harness's current step, session <see cref="FakeCrosshair.Session"/>, age 0 (fresh) until
/// <see cref="Age"/>. Past <c>Crosshair.FreshFor</c> a sample reads stale. A scripted <see cref="Fault"/> reads faulted.
/// The row <c>client.crosshair</c>, which a test can switch off (<c>Capabilities.Override</c>): then
/// <c>Crosshair.Read</c> throws <see cref="KilnUnavailableException"/> and <c>TryGet</c> is false.</para>
/// <para><b>Not modelled.</b> The game's trace: its reach (the held item's, 7 to 31 m), what it can hit, the 1 cm the
/// game moves the point. The first read of the process, which in a game answers <c>NoSample</c> while Kiln's hook
/// goes in (the fake answers the aimed sample at once). A field the test does not give is not set (null), as when the
/// game gives none.</para>
/// </remarks>
public sealed class HarnessCrosshair(KilnHarness harness)
{
    private CrosshairFakeState State => KitFakes.State<CrosshairFakeState>(harness);

    /// <summary>How many times mods read the crosshair (every get the fake answered).</summary>
    public int Reads
    {
        get
        {
            var s = State;
            lock (s.Lock) return s.Reads;
        }
    }

    /// <summary>The game's trace hits <paramref name="position"/>: a fresh sample with the next sequence.</summary>
    /// <param name="position">The hit point, world metres.</param>
    /// <param name="normal">The surface normal (unit length); null: the game gave none.</param>
    /// <param name="distance">Metres from the trace's start; null: none.</param>
    /// <param name="entity">The hit entity; none for terrain.</param>
    /// <param name="voxelMaterial">The voxel material id the game reports for the surface; null: none.</param>
    public unsafe void Aim(WorldPosition position, Vector3? normal = null, float? distance = null, EntityId entity = default, byte? voxelMaterial = null)
    {
        var h = new CrosshairNative.Hit { Flags = CrosshairNative.FlagHit | CrosshairNative.FlagPosition, Status = 0 };
        h.Position[0] = position.X;
        h.Position[1] = position.Y;
        h.Position[2] = position.Z;
        if (normal is { } n)
        {
            h.Flags |= CrosshairNative.FlagNormal;
            h.Normal[0] = n.X;
            h.Normal[1] = n.Y;
            h.Normal[2] = n.Z;
        }
        if (distance is { } d)
        {
            if (!float.IsFinite(d) || d < 0) throw new ArgumentOutOfRangeException(nameof(distance), d, "a distance is finite and not negative");
            h.Flags |= CrosshairNative.FlagDistance;
            h.Distance = d;
        }
        if (!entity.IsNone)
        {
            h.Flags |= CrosshairNative.FlagEntity;
            h.Entity = entity.Value;
        }
        if (voxelMaterial is { } m)
        {
            h.Flags |= CrosshairNative.FlagMaterial;
            h.Material = m;
        }
        Publish(h);
    }

    /// <summary>The game's trace hits nothing within its reach (its status 31): a fresh miss with the next sequence.</summary>
    public void AimAtNothing() => Publish(new CrosshairNative.Hit { Status = 31 });

    /// <summary>The newest sample is <paramref name="age"/> old at the next reads (past <c>Crosshair.FreshFor</c>: stale).</summary>
    public void Age(TimeSpan age)
    {
        if (age < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(age), age, "an age is not negative");
        var s = State;
        lock (s.Lock) s.AgeNs = (ulong)age.Ticks * 100;
    }

    /// <summary>Copying the game's result faulted: reads answer faulted, no sample, until <see cref="Clear"/>.</summary>
    public void Fault()
    {
        var s = State;
        lock (s.Lock) s.Faulted = true;
    }

    /// <summary>No sample (as at the start of a client session), and no fault.</summary>
    public void Clear()
    {
        var s = State;
        lock (s.Lock)
        {
            s.Sample = default;
            s.Faulted = false;
            s.AgeNs = 0;
        }
    }

    private void Publish(CrosshairNative.Hit h)
    {
        var s = State;
        lock (s.Lock)
        {
            h.Sequence = ++s.Sequence;
            h.ClientSession = FakeCrosshair.Session;
            h.ClientStep = harness.CurrentStep;
            s.Sample = h;
            s.AgeNs = 0;
        }
    }
}
