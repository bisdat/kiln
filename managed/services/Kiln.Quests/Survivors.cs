// SPDX-License-Identifier: MIT
// Survivors: the game's roster survivors (the NPCs a player rescues and summons with a tool) for server mods. Read the
// roster with live state, recruit a survivor for the world, give its summon tool, summon or move it into a base, assign
// and unassign its bed. Kiln owns the game side (kiln.survivors in kiln-quests.dll, native/core/abi/services/
// kiln_survivors.h); mods never name a game function, field or offset. Guide: docs/kiln/services/survivors.md. Bed
// contract for other packages: the same page, "Beds".
namespace Kiln;

/// <summary>The game's roster survivors: read, recruit, summon, beds. Server side, tick thread. Since 0.2.0-dev.31.</summary>
/// <remarks>Every write is a request that ends in one <see cref="SurvivorResult"/> within about 30 s. A request that wrote
/// something and then could not finish ends <see cref="SurvivorCompletion.Partial"/> with the exact
/// <see cref="SurvivorResult.Effects"/>: Kiln never takes back an unlock, a tool or a move to fake a rollback. Abandoning
/// the task (a timeout of your own) does not cancel the request.</remarks>
[ServerOnly]
public static unsafe class Survivors
{
    /// <summary>The capability of the reads.</summary>
    public const string ReadCapability = "survivors.read";
    /// <summary>The capability of <see cref="RecruitWorld"/>.</summary>
    public const string RecruitCapability = "survivors.recruit";
    /// <summary>The capability of <see cref="Summon"/>.</summary>
    public const string SummonCapability = "survivors.summon";
    /// <summary>The capability of <see cref="AssignBed"/> and <see cref="UnassignBed"/>.</summary>
    public const string AssignCapability = "survivors.assign";

    /// <summary>The survivor with this id, live; null when no roster entry has it.</summary>
    /// <exception cref="KilnUnavailableException"><c>survivors.read</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(ReadCapability)]
    public static SurvivorInfo? Find(SurvivorId id)
    {
        var t = Use("Survivors.Find", ReadCapability);
        SurvivorsNative.State s = default;
        s.Size = (uint)sizeof(SurvivorsNative.State);
        var rc = t->Get(id.Value, &s);
        if (rc == SurvivorsNative.NotFound || rc == SurvivorsNative.InvalidArgument) return null;
        Check(rc, "Survivors.Find", ReadCapability);
        return SurvivorsNative.Info(s);
    }

    /// <summary>Every roster entry with its live state, in roster order. Empty before the world's first step.</summary>
    /// <exception cref="KilnUnavailableException"><c>survivors.read</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(ReadCapability)]
    public static IReadOnlyList<SurvivorInfo> List()
    {
        var t = Use("Survivors.List", ReadCapability);
        var n = t->Count();
        var list = new List<SurvivorInfo>((int)n);
        for (uint i = 0; i < n; i++)
        {
            SurvivorsNative.State s = default;
            s.Size = (uint)sizeof(SurvivorsNative.State);
            if (t->At(i, &s) == SurvivorsNative.Ok) list.Add(SurvivorsNative.Info(s));
        }
        return list;
    }

    /// <summary>The survivor a live NPC entity is; null when it is none (not a roster NPC, or no such entity).</summary>
    /// <exception cref="KilnUnavailableException"><c>survivors.read</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(ReadCapability)]
    public static SurvivorInfo? Of(EntityId npc)
    {
        if (npc.IsNone) return null;
        var t = Use("Survivors.Of", ReadCapability);
        uint id = 0;
        var rc = t->OfEntity(npc.Value, &id);
        if (rc == SurvivorsNative.NotFound) return null;
        Check(rc, "Survivors.Of", ReadCapability);
        return Find(new SurvivorId(id));
    }

    /// <summary>The survivor a bed is assigned to (the bed's side of the relation); null when it is free.</summary>
    /// <exception cref="ArgumentException"><paramref name="bed"/> is none, or not a bed.</exception>
    /// <exception cref="KilnUnavailableException"><c>survivors.assign</c> is off (the bed layout is not known on this build).</exception>
    [TickThreadOnly, RequiresCapability(AssignCapability)]
    public static SurvivorId? BedOccupant(EntityId bed)
    {
        if (bed.IsNone) throw new ArgumentException("EntityId.None is not a bed", nameof(bed));
        var t = Use("Survivors.BedOccupant", AssignCapability);
        uint id = 0;
        var rc = t->BedOccupant(bed.Value, &id);
        if (rc == SurvivorsNative.NotFound) throw new ArgumentException($"entity {bed} is not a bed", nameof(bed));
        Check(rc, "Survivors.BedOccupant", AssignCapability);
        return id == 0 ? null : new SurvivorId(id);
    }

    /// <summary>The base whose build zone holds <paramref name="position"/> (the id <see cref="Summon"/> takes); null when no
    /// loaded base zone holds it. An abandoned base is returned too: Summon refuses it.</summary>
    /// <exception cref="KilnUnavailableException"><c>survivors.read</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(ReadCapability)]
    public static Guid? BaseAt(WorldPosition position)
    {
        var t = Use("Survivors.BaseAt", ReadCapability);
        var id = stackalloc byte[16];
        uint altar = 0, abandoned = 0;
        var rc = t->BaseAt(new SurvivorsNative.Vec3 { X = position.X, Y = position.Y, Z = position.Z }, id, &altar, &abandoned);
        if (rc == SurvivorsNative.NotFound) return null;
        Check(rc, "Survivors.BaseAt", ReadCapability);
        return SurvivorsNative.ReadGuid(id);
    }

    /// <summary>Recruits a survivor for the world, as its rescue does: its unlock is written, the game's NPC registry
    /// must confirm it, then (with <see cref="SurvivorRecruitOptions.GiveToolTo"/>) one summon tool goes to that player.
    /// Already recruited: no write (NoChange without a tool; the tool is still given when asked for). A survivor whose
    /// unlock is a compound condition is refused (UnsupportedUnlockRequirement). This is a trusted server operation:
    /// check your own permissions before you call it for a player's command.</summary>
    /// <exception cref="ArgumentException"><paramref name="id"/> is 0, or GiveToolTo has no entity.</exception>
    /// <exception cref="KilnUnavailableException"><c>survivors.recruit</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(RecruitCapability)]
    public static Task<SurvivorResult> RecruitWorld(SurvivorId id, SurvivorRecruitOptions? options = null)
    {
        var r = NewRequest(id, SurvivorsNative.OpRecruit);
        if (options?.GiveToolTo is { } p)
        {
            if (p.Entity.IsNone) throw new ArgumentException("GiveToolTo has no entity", nameof(options));
            r.Flags = SurvivorsNative.GiveTool;
            r.Recipient = p.Entity.Value;
        }
        return Submit("Survivors.RecruitWorld", RecruitCapability, r);
    }

    /// <summary>Brings a survivor to a point in a base: a survivor already in the world is moved there and that becomes
    /// its home (the game's own move, as with its summon tool); otherwise it is created there. A locked survivor is
    /// recruited first (no tool). The point must lie inside the build zone of the loaded, kept base
    /// <paramref name="baseId"/>; pick a point on the ground (Kiln does not search). Done when one live NPC stands within
    /// 1 m and its home within 0.05 m. Walking there, path reachability and a bed are not part of it.</summary>
    /// <param name="id">The survivor.</param>
    /// <param name="baseId">The base (<see cref="SurvivorInfo.BaseId"/> of a survivor there, or the altar's base id).</param>
    /// <param name="position">Where it stands.</param>
    /// <param name="yawDegrees">Facing, about the up axis.</param>
    /// <exception cref="ArgumentException">id 0, an empty base id, or a position or yaw that is not finite.</exception>
    /// <exception cref="KilnUnavailableException"><c>survivors.summon</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(SummonCapability)]
    public static Task<SurvivorResult> Summon(SurvivorId id, Guid baseId, WorldPosition position, float yawDegrees = 0)
    {
        if (baseId == Guid.Empty) throw new ArgumentException("an empty base id names no base", nameof(baseId));
        if (!double.IsFinite(position.X) || !double.IsFinite(position.Y) || !double.IsFinite(position.Z))
            throw new ArgumentException("the position is not finite", nameof(position));
        if (!float.IsFinite(yawDegrees)) throw new ArgumentException("the yaw is not finite", nameof(yawDegrees));
        var r = NewRequest(id, SurvivorsNative.OpSummon);
        SurvivorsNative.WriteGuid(baseId, r.BaseId);
        r.Position = new SurvivorsNative.Vec3 { X = position.X, Y = position.Y, Z = position.Z };
        r.YawDegrees = yawDegrees;
        return Submit("Survivors.Summon", SummonCapability, r);
    }

    /// <summary>Assigns a bed to a survivor: both ends of the game's relation are written in one step (saved and sent to
    /// players like the game's own), its old bed is freed and the bed's previous survivor loses it
    /// (<see cref="SurvivorResult.DisplacedNpc"/>). Confirmed by a readback one update later. The survivor must be
    /// recruited and in the world; the bed must be within the NPC's bed radius of its home (else BedOutOfHomeRange: move it
    /// with <see cref="Summon"/> first). <paramref name="options"/> decides who answers for it (D5 in the docs).</summary>
    /// <exception cref="ArgumentNullException"><paramref name="options"/> is null.</exception>
    /// <exception cref="ArgumentException">id 0, bed none, PlayerPermissions without an actor, TrustedServer with one.</exception>
    /// <exception cref="KilnUnavailableException"><c>survivors.assign</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(AssignCapability)]
    public static Task<SurvivorResult> AssignBed(SurvivorId id, EntityId bed, SurvivorAssignmentOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        if (bed.IsNone) throw new ArgumentException("EntityId.None is not a bed", nameof(bed));
        var r = NewRequest(id, SurvivorsNative.OpAssignBed);
        Authorize(ref r, options);
        r.Bed = bed.Value;
        return Submit("Survivors.AssignBed", AssignCapability, r);
    }

    /// <summary>Takes a survivor's bed away: both ends are cleared (and any stale bed that still names it). NoChange when
    /// it has none (after the authorization check).</summary>
    /// <exception cref="ArgumentNullException"><paramref name="options"/> is null.</exception>
    /// <exception cref="ArgumentException">id 0, PlayerPermissions without an actor, TrustedServer with one.</exception>
    /// <exception cref="KilnUnavailableException"><c>survivors.assign</c> is off.</exception>
    [TickThreadOnly, RequiresCapability(AssignCapability)]
    public static Task<SurvivorResult> UnassignBed(SurvivorId id, SurvivorAssignmentOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        var r = NewRequest(id, SurvivorsNative.OpUnassignBed);
        Authorize(ref r, options);
        return Submit("Survivors.UnassignBed", AssignCapability, r);
    }

    // ---- internals

    internal static SurvivorsNative.Request NewRequest(SurvivorId id, uint op)
    {
        if (id.Value == 0) throw new ArgumentException("SurvivorId 0 names no survivor", nameof(id));
        return new SurvivorsNative.Request { Size = (uint)sizeof(SurvivorsNative.Request), Op = op, NpcId = id.Value };
    }

    internal static void Authorize(ref SurvivorsNative.Request r, SurvivorAssignmentOptions o)
    {
        switch (o.Authorization)
        {
            case SurvivorAuthorization.PlayerPermissions:
                if (o.Actor.IsNone) throw new ArgumentException("PlayerPermissions needs the acting player (Actor)", nameof(o));
                break;
            case SurvivorAuthorization.TrustedServer:
                if (!o.Actor.IsNone) throw new ArgumentException("TrustedServer takes no actor", nameof(o));
                break;
            default:
                throw new ArgumentException($"unknown authorization {o.Authorization}", nameof(o));
        }
        r.Authorization = (uint)o.Authorization;
        r.Actor = o.Actor.Value;
    }

    private static SurvivorsNative.Table* Use(string api, string capability)
    {
        Services.RequireTickThread(api);
        var t = SurvivorsNative.T;
        if (t == null)
        {
            QuestsCaps.Rows.Ensure();
            throw new KilnUnavailableException(capability, "kiln.survivors is not loaded (update Kiln)");
        }
        if (!Capabilities.Is(capability)) throw new KilnUnavailableException(capability, Capabilities.Why(capability));
        SurvivorsPump.Start();
        return t;
    }

    private static void Check(int rc, string api, string capability)
    {
        if (rc == SurvivorsNative.Ok) return;
        if (rc == SurvivorsNative.Unavailable) throw new KilnUnavailableException(capability, Capabilities.Why(capability));
        if (rc == SurvivorsNative.WrongThread) throw new InvalidOperationException($"{api}: not on the tick thread");
        throw new InvalidOperationException($"{api}: kiln.survivors returned {rc}");
    }

    private static Task<SurvivorResult> Submit(string api, string capability, SurvivorsNative.Request r)
    {
        var t = Use(api, capability);
        ulong id = 0;
        var rc = t->Submit(Services.CallerOwner, &r, &id);
        if (rc == SurvivorsNative.InvalidArgument) throw new ArgumentException($"{api}: kiln.survivors refused the request's shape");
        Check(rc, api, capability);
        return SurvivorsPump.Wait(id);
    }
}

/// <summary>Results and events of kiln.survivors, on the service tick.</summary>
internal static unsafe class SurvivorsPump
{
    private static readonly object Lock = new();
    private static readonly Dictionary<ulong, (TaskCompletionSource<SurvivorResult> Tcs, ulong Since)> Waiting = [];
    private static IDisposable? _tick;
    private static ulong _cursor;

    internal static void Start()
    {
        lock (Lock)
        {
            if (_tick is not null) return;
            var t = SurvivorsNative.T;
            if (t == null) return;
            SurvivorsNative.Status s = default;
            s.Size = (uint)sizeof(SurvivorsNative.Status);
            _cursor = t->GetStatus(&s) == SurvivorsNative.Ok ? s.Events : 0;
            _tick = ServiceTick.Add("kiln.survivors", Pump);
        }
    }

    internal static Task<SurvivorResult> Wait(ulong id)
    {
        var tcs = new TaskCompletionSource<SurvivorResult>(TaskCreationOptions.None);  // continuations: tick thread
        lock (Lock) Waiting[id] = (tcs, ServiceTick.Step);
        return tcs.Task;
    }

    private static void Pump(in TickInfo tick)
    {
        var t = SurvivorsNative.T;
        if (t == null) return;
        List<(ulong Id, TaskCompletionSource<SurvivorResult> Tcs, ulong Since)> due;
        lock (Lock) due = [.. Waiting.Select(kv => (kv.Key, kv.Value.Tcs, kv.Value.Since))];
        foreach (var (id, tcs, since) in due)
        {
            SurvivorsNative.Result r = default;
            r.Size = (uint)sizeof(SurvivorsNative.Result);
            if (t->GetResult(id, &r) != SurvivorsNative.Ok)
            {
                // the native side ends every request (30 s, then draining); a result older than its cache is gone
                if (tick.Step > since + 50 * 60 * 15)
                {
                    lock (Lock) Waiting.Remove(id);
                    tcs.TrySetException(new InvalidOperationException($"kiln.survivors request {id}: its result was not seen within 15 minutes"));
                }
                continue;
            }
            lock (Lock) Waiting.Remove(id);
            tcs.TrySetResult(SurvivorsNative.ToResult(r));
        }
        PumpEvents(t);
    }

    private static void PumpEvents(SurvivorsNative.Table* t)
    {
        var any = Events.HasSubscribers<SurvivorRecruitedEvent>() || Events.HasSubscribers<SurvivorToolGivenEvent>() ||
                  Events.HasSubscribers<SurvivorSummonedEvent>() || Events.HasSubscribers<SurvivorAssignmentChangedEvent>();
        const int Cap = 32;
        var buf = stackalloc SurvivorsNative.Event[Cap];
        for (var round = 0; round < 8; round++)
        {
            uint n = 0;
            if (t->Events(_cursor, buf, Cap, &n) != SurvivorsNative.Ok || n == 0) return;
            for (var i = 0; i < n; i++)
            {
                _cursor = buf[i].Sequence;
                if (any) Post(in buf[i]);
            }
            if (n < Cap) return;
        }
    }

    private static void Post(in SurvivorsNative.Event e)
    {
        var before = SurvivorsNative.Info(e.Before);
        var after = SurvivorsNative.Info(e.After);
        KilnEvent ev = e.Kind switch
        {
            SurvivorsNative.EvRecruited => new SurvivorRecruitedEvent(after.Id, before, after, e.Request, e.Step),
            SurvivorsNative.EvToolGiven => new SurvivorToolGivenEvent(after.Id, before, after, e.Request, e.Step),
            SurvivorsNative.EvSummoned => new SurvivorSummonedEvent(after.Id, before, after, e.Request, e.Step),
            _ => new SurvivorAssignmentChangedEvent(after.Id, before, after, e.Request, e.Step),
        };
        Events.Post(ev);
    }
}
