// SPDX-License-Identifier: MIT
// The roster's unlock states and budget (package 17c; chapter 17.3 decisions I2 and I7; native/services/quests/roster.cpp,
// kiln_knowledge.h "the roster"; docs/kiln/services/npcs.md "The roster's size"). The game keeps the roster's unlock
// states in one 64-bit value per world; Kiln keeps a second page of 64 where a mod's roster needs more than 64 entries.
// Every read here answers from a snapshot the world published after its last step: any thread, never a game call.
namespace Kiln;

/// <summary>Which world's roster a later read names: the answer of <see cref="NativeQuests.GetServerRosterWorld"/> and
/// <see cref="NativeQuests.GetClientRosterWorld"/>.</summary>
/// <param name="Ok">True when the world has a complete snapshot: <paramref name="World"/> names it.</param>
/// <param name="Reason">Null when <paramref name="Ok"/>; <c>NoWorld</c> (no such world in this process now) or
/// <c>NotReady</c> (the world has no complete snapshot yet, or its roster is one this side cannot read).</param>
/// <param name="World">The world's token while it lives; null when not <paramref name="Ok"/>. A receipt for this process
/// only: never save or send it.</param>
public sealed record RosterWorldResult(bool Ok, string? Reason, CapacityWorldToken? World) : IKilnResult;

/// <summary>One roster entry's unlock state in one world's snapshot.</summary>
/// <param name="Ok">True when the snapshot answered: <paramref name="Unlocked"/> and <paramref name="KnowledgeVersion"/>
/// are set.</param>
/// <param name="Reason">Null when <paramref name="Ok"/>; else <c>WorldEnded</c> (the token's world stopped, or another
/// world of that kind runs now), <c>NotReady</c> (no complete snapshot of the token's world yet) or <c>UnknownNpc</c>
/// (the world's roster has no entry with that id). Never "locked": a state that cannot be answered is not false.</param>
/// <param name="NpcId">The NpcId asked for (FNV-1a-32 of the NPC template's GUID).</param>
/// <param name="Unlocked">The game's unlock state of the entry; null when not <paramref name="Ok"/>.</param>
/// <param name="KnowledgeVersion">The engine's knowledge version the snapshot was computed at; null when not
/// <paramref name="Ok"/>.</param>
/// <param name="World">The world the request named, also when it failed.</param>
/// <param name="SnapshotRevision">kiln.knowledge's revision of that snapshot (it rises whenever a published roster
/// answer changes).</param>
public sealed record NpcUnlockResult(bool Ok, string? Reason, uint NpcId, bool? Unlocked, ushort? KnowledgeVersion,
    CapacityWorldToken World, ulong SnapshotRevision) : IKilnResult;

/// <summary>A roster entry's unlock state changed between two snapshots of one world (a knowledge version change alone,
/// and a world's first snapshot, are not one). Posted on the service tick after the world's step that published it.
/// Snapshots are published only while something reads the roster: subscribing to this event is such a read.</summary>
public sealed class RosterUnlockChangedEvent : KilnEvent
{
    static RosterUnlockChangedEvent() => KnowledgeEventPump.Start();

    internal RosterUnlockChangedEvent(CapacityWorldToken world, uint npcId, bool before, bool after, ushort version, ulong revision)
    {
        World = world;
        NpcId = npcId;
        Before = before;
        After = after;
        KnowledgeVersion = version;
        SnapshotRevision = revision;
    }

    /// <summary>The world whose roster changed.</summary>
    public CapacityWorldToken World { get; }
    /// <summary>The entry's NpcId.</summary>
    public uint NpcId { get; }
    /// <summary>Unlocked in the previous snapshot.</summary>
    public bool Before { get; }
    /// <summary>Unlocked now.</summary>
    public bool After { get; }
    /// <summary>The engine's knowledge version of the new snapshot.</summary>
    public ushort KnowledgeVersion { get; }
    /// <summary>The new snapshot's revision.</summary>
    public ulong SnapshotRevision { get; }

    /// <inheritdoc />
    public override string ToString() => $"{World.Kind} world {World.Generation}: NPC 0x{NpcId:x8} {(After ? "unlocked" : "locked")}";
}

public static partial class NativeQuests
{
    private const uint WorldServer = 0, WorldClient = 2;
    private const uint ReadinessValid = 1, ReadinessNoWorld = 2, ReadinessWorldEnded = 3, ReadinessUnknownNpc = 7;

    /// <summary>The roster's maximum the game's own value allows: 64 entries, the game's included.</summary>
    public const int RosterPage = 64;

    /// <summary>
    /// This process's server world (a dedicated server's, or a hosting client's) as a token for <see cref="GetUnlock"/>
    /// and <see cref="GetRosterBudget"/>: <c>NoWorld</c> before it runs, <c>NotReady</c> until its first snapshot (one step
    /// after the first read: snapshots are published only while something reads the roster). Any thread.
    /// </summary>
    /// <exception cref="KilnUnavailableException"><c>npcs.roster_read</c>: kiln.knowledge is missing or older, or this build
    /// has no NPC registry layout.</exception>
    [ServerOnly]
    [AnyThread]
    [RequiresCapability("npcs.roster_read")]
    public static RosterWorldResult GetServerRosterWorld() => RosterWorld(WorldServer, QuestsCaps.RosterRead);

    /// <summary>This process's client world as a token for <see cref="GetClientUnlock"/> and <see cref="GetRosterBudget"/>;
    /// as <see cref="GetServerRosterWorld"/>. There is no read of a remote server's roster from a joined client: the
    /// client world's roster is the one the client's own registry computed from the knowledge it received.</summary>
    /// <exception cref="KilnUnavailableException"><c>npcs.client_roster_read</c>: a dedicated server, or as above.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability("npcs.client_roster_read")]
    public static RosterWorldResult GetClientRosterWorld() => RosterWorld(WorldClient, QuestsCaps.ClientRosterRead);

    /// <summary>
    /// The unlock state of the roster entry <paramref name="npcId"/> in the server world <paramref name="serverWorld"/>
    /// names, from the snapshot of that world's last completed step (a step whose registry could not be read answers
    /// <c>NotReady</c>, never an older state). Entries 64 and up answer from Kiln's
    /// second page (where it is installed), never from the game's value (which would be another entry's bit). A stopped
    /// world answers <c>WorldEnded</c>, never the next world's state. Any thread.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="npcId"/> is 0, or <paramref name="serverWorld"/> is not a server
    /// world's token (another kind, generation 0).</exception>
    /// <exception cref="KilnUnavailableException"><c>npcs.roster_read</c>.</exception>
    [ServerOnly]
    [AnyThread]
    [RequiresCapability("npcs.roster_read")]
    public static NpcUnlockResult GetUnlock(uint npcId, CapacityWorldToken serverWorld) =>
        Unlock(npcId, serverWorld, CapacityWorldKind.Server, QuestsCaps.RosterRead, nameof(serverWorld));

    /// <summary>The unlock state of <paramref name="npcId"/> in the client world <paramref name="clientWorld"/> names; as
    /// <see cref="GetUnlock"/>. A hosting client has both worlds, each with its own registry and its own answer.</summary>
    /// <exception cref="ArgumentException"><paramref name="npcId"/> is 0, or <paramref name="clientWorld"/> is not a client
    /// world's token.</exception>
    /// <exception cref="KilnUnavailableException"><c>npcs.client_roster_read</c>: a dedicated server, or as above.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability("npcs.client_roster_read")]
    public static NpcUnlockResult GetClientUnlock(uint npcId, CapacityWorldToken clientWorld) =>
        Unlock(npcId, clientWorld, CapacityWorldKind.Client, QuestsCaps.ClientRosterRead, nameof(clientWorld));

    /// <summary>
    /// The roster entries of the world <paramref name="world"/> names: <see cref="CapacityInfo.Capacity"/> 64, or 128 where
    /// Kiln's second page is installed (<see cref="CapacityInfo.Expanded"/>: it is installed only when a mod's roster needs
    /// more than 64 entries, at the start); <see cref="CapacityInfo.GameUsed"/> the entries of the collection Kiln found at
    /// the start (the game's 35, or what another loader served before Kiln's build), <see cref="CapacityInfo.KilnUsed"/>
    /// the entries Kiln's build added for mods, <see cref="CapacityInfo.Reserved"/> entries neither put there. The roster
    /// is built once at the start: its room does not change while the process runs. Any thread.
    /// </summary>
    /// <exception cref="ArgumentException"><paramref name="world"/> is not a token a roster read returned.</exception>
    /// <exception cref="KilnUnavailableException"><c>npcs.roster_budget</c> (a client world's token on a dedicated server:
    /// <c>npcs.client_roster_read</c>).</exception>
    [AnyThread]
    [RequiresCapability("npcs.roster_budget")]
    public static unsafe CapacityQueryResult GetRosterBudget(CapacityWorldToken world)
    {
        CapacityMapping.CheckToken(world, nameof(world));
        var r = RosterNative(QuestsCaps.RosterBudget);
        var n = KilnCapacityInfo.New();
        var rc = r->RosterBudget((uint)world.Kind, world.Generation, &n);
        if (rc == KnowledgeNative.Unavailable)
            throw QuestsCaps.Off(world.Kind == CapacityWorldKind.Client ? QuestsCaps.ClientRosterRead : QuestsCaps.RosterBudget,
                $"the {world.Kind.ToString().ToLowerInvariant()} world's roster is not read on this side");
        if (rc == KnowledgeNative.InvalidArgument) throw new ArgumentException($"world {world} is not a roster world", nameof(world));
        if (rc != KnowledgeNative.Ok) throw new InvalidOperationException($"NativeQuests.GetRosterBudget: kiln.knowledge returned {rc}");
        return RosterBudgetWatch.Seen(CapacityMapping.ToResult(n));
    }

    private static unsafe KnowledgeNative.RosterTable* RosterNative(string capability)
    {
        var t = KnowledgeNative.T;
        var r = KnowledgeNative.Roster(t);
        if (r != null) return r;
        throw QuestsCaps.Off(capability, t == null ? "kiln.knowledge is not loaded" : "this kiln.knowledge has no roster reads (update Kiln)");
    }

    private static unsafe RosterWorldResult RosterWorld(uint kind, string capability)
    {
        var r = RosterNative(capability);
        uint gen = 0, readiness = 0;
        var rc = r->RosterWorld(kind, &gen, &readiness);
        if (rc == KnowledgeNative.Unavailable) throw QuestsCaps.Off(capability, "the roster is not read on this side");
        if (rc != KnowledgeNative.Ok) throw new InvalidOperationException($"NativeQuests roster world: kiln.knowledge returned {rc}");
        if (readiness == ReadinessValid && gen != 0)
            return new RosterWorldResult(true, null, new CapacityWorldToken((CapacityWorldKind)kind, gen));
        return new RosterWorldResult(false, readiness == ReadinessNoWorld ? "NoWorld" : "NotReady", null);
    }

    private static unsafe NpcUnlockResult Unlock(uint npcId, CapacityWorldToken token, CapacityWorldKind kind, string capability, string param)
    {
        if (npcId == 0) throw new ArgumentException("NpcId 0 is no NPC", nameof(npcId));
        CapacityMapping.CheckToken(token, param);
        if (token.Kind != kind) throw new ArgumentException($"{param} is a {token.Kind} world's token; this read takes the {kind} world's", param);
        var r = RosterNative(capability);
        var reply = new KnowledgeNative.NpcUnlockReply { Size = (uint)sizeof(KnowledgeNative.NpcUnlockReply) };
        var rc = r->NpcUnlockAt((uint)kind, token.Generation, npcId, &reply);
        if (rc == KnowledgeNative.Unavailable) throw QuestsCaps.Off(capability, "the roster is not read on this side");
        if (rc == KnowledgeNative.InvalidArgument) throw new ArgumentException($"NPC 0x{npcId:x8} in {token}: not a roster read", param);
        if (rc != KnowledgeNative.Ok) throw new InvalidOperationException($"NativeQuests unlock read: kiln.knowledge returned {rc}");
        return ToResult(reply, npcId, token);
    }

    internal static NpcUnlockResult ToResult(in KnowledgeNative.NpcUnlockReply reply, uint npcId, CapacityWorldToken token)
    {
        if (reply.Readiness == ReadinessValid)
            return new NpcUnlockResult(true, null, npcId, reply.Unlocked != 0, (ushort)reply.KnowledgeVersion, token, reply.Revision);
        var reason = reply.Readiness switch
        {
            ReadinessWorldEnded => "WorldEnded",
            ReadinessUnknownNpc => "UnknownNpc",
            ReadinessNoWorld => "NoWorld",
            _ => "NotReady",  // anything else is not an answer
        };
        return new NpcUnlockResult(false, reason, npcId, null, null, token, reply.Revision);
    }
}

/// <summary>The roster budget's <see cref="CapacityChangedEvent"/> (Quota <c>npcs.roster_budget</c>): a world's budget
/// as last seen, compared every 25 steps while something listens, and at every read.</summary>
internal static unsafe class RosterBudgetWatch
{
    private static readonly object Gate = new();
    private static readonly CapacityInfo?[] Last = new CapacityInfo?[3];
    private static bool _ticking;

    public static CapacityQueryResult Seen(CapacityQueryResult r)
    {
        if (r.Budget.World is { } w) Compare((uint)w.Kind, r.Budget);
        EnsureTick();
        return r;
    }

    public static void EnsureTick()
    {
        lock (Gate)
        {
            if (_ticking) return;
            _ticking = true;
        }
        ServiceTick.Add("kiln.knowledge roster budget", static (in TickInfo t) => Tick(t.Step));
    }

    private static void Tick(ulong step)
    {
        if (step % 25 != 9 || !Events.HasSubscribers<CapacityChangedEvent>()) return;
        var r = KnowledgeNative.Roster(KnowledgeNative.T);
        if (r == null) return;
        foreach (var kind in (uint[])[0, 2])
        {
            uint gen = 0, readiness = 0;
            if (r->RosterWorld(kind, &gen, &readiness) != KnowledgeNative.Ok || gen == 0) continue;
            var n = KilnCapacityInfo.New();
            if (r->RosterBudget(kind, gen, &n) == KnowledgeNative.Ok) Compare(kind, CapacityMapping.ToResult(n).Budget);
        }
    }

    private static void Compare(uint kind, CapacityInfo now)
    {
        if (kind > 2) return;
        CapacityInfo? prev;
        lock (Gate)
        {
            prev = Last[kind];
            Last[kind] = now;
        }
        if (prev != null && Events.HasSubscribers<CapacityChangedEvent>() && CapacityMapping.Changed(QuestsCaps.RosterBudget, prev, now) is { } ev)
            Events.Enqueue(ev);
    }
}
