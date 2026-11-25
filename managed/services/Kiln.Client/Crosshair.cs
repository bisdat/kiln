// SPDX-License-Identifier: MIT
// Kiln.Crosshair: a read-only copy of the game client's own crosshair hit (kiln.client.crosshair's get,
// native/core/abi/services/kiln_client_crosshair.h). Since 0.2.0-dev.31.13, mod API 0.2.22. Asked for by XHL (Easy HUD,
// Vein Mining, Blueprint).
using System.Numerics;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Why <see cref="Crosshair.Read"/> has no fresh sample (kiln_client_crosshair.h's
/// <c>KILN_CROSSHAIR_REASON_*</c>). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
public enum CrosshairReason : uint
{
    /// <summary>A fresh sample of the live client session.</summary>
    None = 0,

    /// <summary>Nothing in this client session yet: the main menu, a loading screen, or the first read of the game
    /// (it installs Kiln's observation; samples start with the game's next cursor step).</summary>
    NoSample = 1,

    /// <summary>A sample was being published at that moment, every time Kiln asked (<see cref="Crosshair.BusyRetries"/>
    /// more times). Ask again.</summary>
    Busy = 2,

    /// <summary>The newest sample is older than <see cref="Crosshair.FreshFor"/>: the game stopped running its cursor
    /// (a loading screen, the main menu, a state where the game skips the local player's cursor).</summary>
    Stale = 3,

    /// <summary>Copying the game's result faulted: no samples until the next client session (kiln.log says more).</summary>
    Faulted = 4,
}

/// <summary>
/// One copy of the game's own crosshair hit: what the stock cursor and HUD see under the screen's centre, from the
/// local player's camera along its forward axis. Values only; no game memory is held. Since 0.2.0-dev.31.13, mod API
/// 0.2.22.
/// </summary>
/// <remarks>
/// <para>The trace is the game's: its reach is the held item's (<c>ItemInfo.equipment.maxDistance</c>), clamped by the
/// game to 7 to 31 metres, measured from the trace's start, which the game puts on the camera's forward axis (it is not
/// the eye). A field the game did not give as a sane value is null (or <see cref="EntityId.None"/>).</para>
/// <para>Freshness: <see cref="Sequence"/> grows by one per game trace, <see cref="ClientSession"/> and
/// <see cref="ClientStep"/> name the client world and its step (<see cref="Client.ClientWorldSession"/>,
/// <see cref="Client.ClientWorldSteps"/>), and <see cref="Age"/> is the sample's age when it was read. Two reads with
/// the same <see cref="Sequence"/> are the same trace.</para>
/// </remarks>
[ClientOnly]
public readonly record struct CrosshairHit
{
    /// <summary>The game's trace hit something within its reach.</summary>
    public bool IsHit { get; init; }

    /// <summary>The hit point in world metres: the game's own point, moved 1 cm either against the surface normal (into
    /// the surface, when the hit collider and the held item ask for it) or back along the ray toward the trace's start.
    /// Null without a hit.</summary>
    public WorldPosition? Position { get; init; }

    /// <summary>The surface normal at the hit, unit length. Null without a hit or when the game gave none.</summary>
    public Vector3? Normal { get; init; }

    /// <summary>Metres from the trace's start to the hit. Null without a hit.</summary>
    public float? Distance { get; init; }

    /// <summary>The game's entity of the hit (a creature, a prop, a placed object), <see cref="EntityId.None"/> for
    /// terrain and without a hit. A client-world id; on a host it is also the server world's. It is the game's own
    /// pick: where the game moves the point into the surface (see <see cref="Position"/>) and its trace found two or
    /// more hits, the entity is the second-nearest hit's while position, normal and distance are the nearest's.</summary>
    public EntityId Entity { get; init; }

    /// <summary>The voxel material id the game reports for the hit surface (terrain and voxel building cells; some
    /// colliders carry one), or null.</summary>
    public byte? VoxelMaterial { get; init; }

    /// <summary>The game's own result byte: 0 hit, 31 nothing within reach, 21 no usable camera direction.</summary>
    public uint GameStatus { get; init; }

    /// <summary>Game traces copied since the game started; never 0 for a sample.</summary>
    public ulong Sequence { get; init; }

    /// <summary>The client world session of the sample (<see cref="Client.ClientWorldSession"/>).</summary>
    public uint ClientSession { get; init; }

    /// <summary>The client world's step count at the sample (<see cref="Client.ClientWorldSteps"/>).</summary>
    public ulong ClientStep { get; init; }

    /// <summary>How old the sample was when it was read, rounded up to a whole tick (100 ns).</summary>
    public TimeSpan Age { get; init; }

    /// <summary>True when the hit is an entity (<see cref="Entity"/> is not <see cref="EntityId.None"/>).</summary>
    public bool HasEntity => !Entity.IsNone;

    /// <summary>True when the sample was at most <see cref="Crosshair.FreshFor"/> old when it was read.</summary>
    public bool IsFresh => Sequence != 0 && Age <= Crosshair.FreshFor;
}

/// <summary>
/// The game client's own crosshair hit, read-only: hit or miss, world position, surface normal, distance, the hit entity
/// and the voxel material, with the client session, step and age of the sample. It is the hit the stock cursor and HUD
/// use; Kiln does no raycast of its own. Since 0.2.0-dev.31.13, mod API 0.2.22.
/// </summary>
/// <remarks>
/// <para>Any thread: the client tick (<see cref="Client.Tick"/>), the UI thread, a worker. A read copies the newest
/// sample and never waits for the game. The first read of the game adds Kiln's observation of the game's trace, so it
/// answers <see cref="CrosshairReason.NoSample"/>; a game without a mod that reads the crosshair never has it.</para>
/// <para>For a target that must be current, use <see cref="TryGet"/> (a fresh sample of the live session only); to show
/// the last known target, use <see cref="Read"/> and look at <see cref="CrosshairHit.Age"/>.</para>
/// </remarks>
[ClientOnly]
[AnyThread]
public static unsafe class Crosshair
{
    /// <summary>The capability row: available on a game client whose build has the game's crosshair trace verified
    /// (hooked at the first read), unless the hook failed.</summary>
    public const string Capability = "client.crosshair";

    /// <summary>How often <see cref="Read"/> asks again when a sample was being published.</summary>
    public const int BusyRetries = 3;

    /// <summary>A sample older than this is stale (kiln_client_crosshair.h's <c>KILN_CROSSHAIR_FRESH_NS</c>: about 15
    /// client steps).</summary>
    public static TimeSpan FreshFor { get; } = TimeSpan.FromTicks((long)(CrosshairNative.FreshNs / 100));

    internal static readonly ServiceCapRows Rows = new(CrosshairNative.Service, ReportRows);

    static Crosshair() => Rows.Ensure();

    /// <summary>True when the crosshair can be read here: the capability row is on and kiln.client.crosshair is there.
    /// Any thread; never throws.</summary>
    public static bool Available => CrosshairNative.Members != null && Capabilities.Is(Capability);

    /// <summary>The newest sample, fresh or not; null when there is none to give (<paramref name="reason"/> says why).
    /// Any thread; never waits.</summary>
    /// <param name="reason"><see cref="CrosshairReason.None"/> for a fresh sample, <see cref="CrosshairReason.Stale"/>
    /// for an older one, else why there is none.</param>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off here (a dedicated server, safe idle,
    /// an unverified game build, or the hook failed).</exception>
    [RequiresCapability(Capability)]
    public static CrosshairHit? Read(out CrosshairReason reason)
    {
        Rows.Ensure();
        var t = CrosshairNative.Members;
        if (t == null) throw ServiceCaps.Unavailable(Capability, "kiln.client.crosshair is not here (a Kiln before 0.2.0-dev.31.13)");
        if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability);
        var rc = Get(t, out var hit, out reason);
        if (rc == CrosshairNative.Unavailable) throw ServiceCaps.Unavailable(Capability, "kiln.client.crosshair answered unavailable");
        if (rc != CrosshairNative.Ok) throw new InvalidOperationException($"kiln.client.crosshair's get answered {rc}");
        return hit;
    }

    /// <summary>A fresh sample of the live client session, hit or miss. False when there is none (no sample yet, a stale
    /// one, a fault, the row is off). Any thread; never waits, never throws.</summary>
    /// <param name="hit">The sample; <c>default</c> when false.</param>
    public static bool TryGet(out CrosshairHit hit)
    {
        hit = default;
        var t = CrosshairNative.Members;
        if (t == null || !Capabilities.Is(Capability)) return false;
        if (Get(t, out var got, out var reason) != CrosshairNative.Ok || reason != CrosshairReason.None || got is not { } h) return false;
        hit = h;
        return true;
    }

    private static int Get(CrosshairNative.Table* t, out CrosshairHit? hit, out CrosshairReason reason)
    {
        hit = null;
        reason = CrosshairReason.NoSample;
        for (var attempt = 0; ; attempt++)
        {
            var n = new CrosshairNative.Hit { Size = (uint)sizeof(CrosshairNative.Hit) };
            uint why = 0;
            var rc = t->Get(&n, &why);
            if (rc != CrosshairNative.Ok) return rc;
            reason = why <= (uint)CrosshairReason.Faulted ? (CrosshairReason)why : CrosshairReason.NoSample;
            if (reason == CrosshairReason.Busy && attempt < BusyRetries) continue;
            if (reason is CrosshairReason.None or CrosshairReason.Stale && n.Sequence != 0) hit = Convert(in n);
            return rc;
        }
    }

    private static CrosshairHit Convert(in CrosshairNative.Hit n)
    {
        var f = n.Flags;
        var hit = (f & CrosshairNative.FlagHit) != 0;
        fixed (CrosshairNative.Hit* p = &n)
        {
            return new CrosshairHit
            {
                IsHit = hit,
                Position = hit && (f & CrosshairNative.FlagPosition) != 0 ? new WorldPosition(p->Position[0], p->Position[1], p->Position[2]) : null,
                Normal = hit && (f & CrosshairNative.FlagNormal) != 0 ? new Vector3(p->Normal[0], p->Normal[1], p->Normal[2]) : null,
                Distance = hit && (f & CrosshairNative.FlagDistance) != 0 ? p->Distance : null,
                Entity = hit && (f & CrosshairNative.FlagEntity) != 0 ? new EntityId(p->Entity) : EntityId.None,
                VoxelMaterial = hit && (f & CrosshairNative.FlagMaterial) != 0 && p->Material <= byte.MaxValue ? (byte)p->Material : null,
                GameStatus = p->Status,
                Sequence = p->Sequence,
                ClientSession = p->ClientSession,
                ClientStep = p->ClientStep,
                // rounded up to a whole 100 ns tick, so IsFresh (Age <= FreshFor) agrees with Kiln's STALE (age_ns > 250 ms)
                Age = TimeSpan.FromTicks((long)Math.Min(p->AgeNs / 100 + (p->AgeNs % 100 != 0 ? 1ul : 0ul), (ulong)TimeSpan.MaxValue.Ticks)),
            };
        }
    }

    private static void ReportRows()
    {
        if (CrosshairNative.Members != null || Capabilities.TryGetRow(Capability, out _)) return;  // the core's row stands
        var (reason, text) = ServiceCaps.Cause(CapabilityReason.ServiceMissing,
            "kiln.client.crosshair is not here (a Kiln before 0.2.0-dev.31.13)", CrosshairNative.Service);
        ServiceCaps.Report(Capability, CapabilityState.Unavailable, reason, text);
    }
}

/// <summary>kiln.client.crosshair v1 (native/core/abi/services/kiln_client_crosshair.h). Byte-exact with the header:
/// Kiln.Api.Tests' CrosshairTests check every size and offset.</summary>
internal static unsafe class CrosshairNative
{
    public const string Service = "kiln.client.crosshair";
    public const int Ok = 0, InvalidArgument = 1, Unavailable = 3;  // KilnResult (kiln_api.h)
    public const ulong FreshNs = 250_000_000;                          // KILN_CROSSHAIR_FRESH_NS
    // KILN_CROSSHAIR_*
    public const uint FlagHit = 1, FlagPosition = 2, FlagNormal = 4, FlagDistance = 8, FlagEntity = 16, FlagMaterial = 32;

    /// <summary>KilnCrosshairHit (88 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Hit
    {
        public uint Size, Flags, Status, Entity, Material, ClientSession;
        public ulong Sequence, ClientStep, AgeNs;
        public fixed double Position[3];
        public fixed float Normal[3];
        public float Distance;
    }

    /// <summary>KilnClientCrosshairApi (16 bytes).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<Hit*, uint*, int> Get;
    }

    // The table found under a backend (a test swaps backends; a game has one): asked once per backend while it is there.
    private sealed record Found(IKilnBackend Backend, nint Address);
    private static volatile Found? _found;

    /// <summary>The table (header.size of at least 16), or null.</summary>
    public static Table* Members
    {
        get
        {
            var backend = Backend.Current;
            if (_found is { } found && ReferenceEquals(found.Backend, backend)) return (Table*)found.Address;
            var t = Services.Get<Table>(Service, 1);
            if (t != null) _found = new Found(backend, (nint)t);
            return t;
        }
    }
}
