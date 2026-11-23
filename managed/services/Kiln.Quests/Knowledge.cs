// SPDX-License-Identifier: MIT
// Managed face of kiln.knowledge (native/services/quests/kiln_knowledge.h): the game's knowledge per world and per
// player: read, grant, revoke, set, names, queries, the NPC roster. Usage: docs/kiln/services/knowledge.md.
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

/// <summary>The game's knowledge types.</summary>
public enum KnowledgeType : byte
{
    /// <summary>0 or 1.</summary>
    Bool = 0,
    /// <summary>A number that only grows through the game's queue.</summary>
    Counter = 1,
    /// <summary>A bit mask; each flag is set on its own (pickup / use / inspect of an item, ...).</summary>
    Flags = 2,
    /// <summary>No such object.</summary>
    None = 0xff,
}

/// <summary>What GameKnowledgeResource says about an id.</summary>
[Flags]
public enum KnowledgeObjectFlags : uint
{
    /// <summary>The game does not know the id.</summary>
    None = 0,
    /// <summary>A world object: shared progress (NPC rescues, world quests, lines heard by anyone).</summary>
    World = 0x01,
    /// <summary>A player object: per character (quest rewards, skill-point knowledge, ...).</summary>
    Player = 0x02,
    /// <summary>A world object with a player twin of the same id (personal progression).</summary>
    Twin = 0x04,
    /// <summary>The game reports it to its analytics when it is set.</summary>
    Analytics = 0x08,
    /// <summary>The game pays XP when the player object is set.</summary>
    Experience = 0x10,
    /// <summary>The player object gives skill points.</summary>
    SkillPoints = 0x20,
}

/// <summary>The kind of a requirement (the game's KnowledgeQueryDataType).</summary>
public enum KnowledgeQueryType : byte
{
    /// <summary>A named query of the game's query db (AND/OR of knowledge, inventory, NPC state, flame level, ...).</summary>
    Extern = 0,
    /// <summary>A Bool knowledge object compared to a value.</summary>
    SimpleBool = 1,
    /// <summary>A Counter knowledge object compared to a value.</summary>
    SimpleCount = 2,
    /// <summary>A Flags knowledge object compared to a value.</summary>
    SimpleFlag = 3,
}

/// <summary>How a requirement compares a knowledge object's value with its compare value.</summary>
public enum KnowledgeCompare : byte
{
    /// <summary>The object's value is greater than the compare value.</summary>
    GreaterThan = 0,
    /// <summary>The object's value is less than the compare value.</summary>
    LessThan = 1,
    /// <summary>The object's value equals the compare value.</summary>
    Equals = 2,
}

/// <summary>How a player's requirement reads knowledge that has a world object and a player twin.</summary>
public enum KnowledgeMode : uint
{
    /// <summary>The world object (shared progression).</summary>
    Shared = 0,
    /// <summary>What the player's own setting says (personal progression on or off).</summary>
    Player = 1,
    /// <summary>The player object (the game's quest rewards read it so).</summary>
    Personal = 2,
}

/// <summary>A requirement exactly as the game stores it (KnowledgeQueryData): a knowledge object compared to a value,
/// or a named query (<see cref="KnowledgeQueryType.Extern"/>). Evaluate it with <see cref="Knowledge.Query"/>.</summary>
/// <param name="Id">The knowledge id to compare, or the named query's id when <paramref name="Type"/> is
/// <see cref="KnowledgeQueryType.Extern"/>.</param>
/// <param name="Type">The kind of requirement.</param>
/// <param name="Compare">How the object's value is compared with <paramref name="CompareValue"/>.</param>
/// <param name="CompareValue">The value to compare with.</param>
/// <param name="PlayerOnly">Read the player's own object even in shared progression (the game's
/// <c>isExplicitPlayerKnowledgeQuery</c>).</param>
public readonly record struct KnowledgeQuery(uint Id, KnowledgeQueryType Type, KnowledgeCompare Compare, uint CompareValue,
    bool PlayerOnly = false)
{
    /// <summary>"this Bool knowledge is set".</summary>
    public static KnowledgeQuery IsSet(uint id) => new(id, KnowledgeQueryType.SimpleBool, KnowledgeCompare.Equals, 1);
    /// <summary>True when <c>Id</c> is 0: there is no requirement.</summary>
    public bool IsEmpty => Id == 0;
    /// <summary>The requirement as text: the id, the kind, the comparison and the value.</summary>
    public override string ToString() => Type == KnowledgeQueryType.Extern
        ? $"query 0x{Id:x8}"
        : $"0x{Id:x8} {Type} {Compare} {CompareValue}{(PlayerOnly ? " (player)" : "")}";
}

/// <summary>What GameKnowledgeResource says about one id (<see cref="Knowledge.Info"/>).</summary>
/// <param name="Id">The knowledge id.</param>
/// <param name="Flags">Which kinds of object the id is (world, player, twin, experience, skill points...). None: the game
/// does not know it.</param>
/// <param name="WorldType">The type of the id's world object; <see cref="KnowledgeType.None"/> when it has none.</param>
/// <param name="PlayerType">The type of the id's player object; <see cref="KnowledgeType.None"/> when it has none.</param>
/// <param name="WorldFlags">For a Flags world object: the bits that exist.</param>
/// <param name="PlayerFlags">For a Flags player object: the bits that exist.</param>
/// <param name="ExperienceId">For a player object that pays XP: the game's experience value id; 0 for none.</param>
/// <param name="SkillPoints">For a player object that gives skill points: how many.</param>
public readonly record struct KnowledgeInfo(uint Id, KnowledgeObjectFlags Flags, KnowledgeType WorldType, KnowledgeType PlayerType,
    uint WorldFlags, uint PlayerFlags, uint ExperienceId, int SkillPoints)
{
    /// <summary>True when the game knows the id (it has at least one flag).</summary>
    public bool Known => Flags != KnowledgeObjectFlags.None;
    /// <summary>True when the id is a world object.</summary>
    public bool IsWorld => (Flags & KnowledgeObjectFlags.World) != 0;
    /// <summary>True when the id is a player object.</summary>
    public bool IsPlayer => (Flags & KnowledgeObjectFlags.Player) != 0;
    /// <summary>The id, its flags and the world and player types.</summary>
    public override string ToString() => $"0x{Id:x8} {Flags} world {WorldType} player {PlayerType}";
}

/// <summary>One id in one map, read at <see cref="Step"/>.</summary>
/// <param name="Target">0 = the world map, else the player.</param>
/// <param name="Value">Bool 0/1, Counter n, Flags mask; 0 = not set.</param>
/// <param name="Present">The map has an entry (it can hold 0 after a revoke).</param>
/// <param name="Id">The knowledge id.</param>
/// <param name="Type">The object's type in this target.</param>
/// <param name="NewMask">The game's "new" markers (bits 32 to 39 of the state); 0xff for an id without an entry.</param>
/// <param name="Step">The server-world step of the read.</param>
public readonly record struct KnowledgeState(EntityId Target, uint Id, uint Value, KnowledgeType Type, bool Present, byte NewMask, ulong Step)
{
    /// <summary>True when the value is not 0.</summary>
    public bool IsSet => Value != 0;
    /// <summary>For a Flags object: true when flag number <paramref name="flag"/> (0 to 31) is set.</summary>
    public bool HasFlag(int flag) => flag is >= 0 and < 32 && ((Value >> flag) & 1) != 0;
    /// <summary>The id, the value, the type and the target.</summary>
    public override string ToString() => $"0x{Id:x8} = {Value} ({Type}{(Target.IsNone ? ", world" : $", player {Target}")})";
}

/// <summary>What a name means (<see cref="Knowledge.Resolve"/>).</summary>
/// <param name="IsQuery">The name is a named query (evaluate with <see cref="Knowledge.Query"/>; it cannot be granted).</param>
/// <param name="Requirement">The full requirement behind the name: an NPC's unlock requirement, a quest step's query; a
/// plain knowledge name: "is set".</param>
/// <param name="Text">The name as it was asked for.</param>
/// <param name="Id">The knowledge id (or the query id when <paramref name="IsQuery"/>).</param>
public sealed record KnowledgeName(string Text, bool IsQuery, uint Id, KnowledgeQuery Requirement)
{
    /// <summary>The name, and whether it is a query or knowledge, with its id.</summary>
    public override string ToString() => $"{Text} = {(IsQuery ? "query" : "knowledge")} 0x{Id:x8}";
}

/// <summary>A change to the game's knowledge.</summary>
public enum KnowledgeOp
{
    /// <summary>Grant through the game's own apply: set a Bool, add to a Counter, or set a Flags flag (XP, triggers and
    /// replication are the game's).</summary>
    Grant = 1,
    /// <summary>Clear a knowledge id: a Flags flag, or all of the object. A direct write; the game has no path that
    /// lowers a value.</summary>
    Revoke = 2,
    /// <summary>Write an exact value (a direct write: no XP, no "new" marker).</summary>
    Set = 3,
}

/// <summary>What a change did. <see cref="Before"/>/<see cref="After"/> are the target's value around the apply (inside the
/// game's knowledge update of the step).</summary>
/// <param name="Request">The id of the request this answers; 0 when it was refused at once.</param>
/// <param name="Op">The change that was asked for.</param>
/// <param name="Target">The target's entity; <see cref="EntityId.None"/> for the world.</param>
/// <param name="Id">The knowledge id.</param>
/// <param name="Ok">True when the game applied the request. False means a refusal: see <paramref name="Reason"/>.</param>
/// <param name="Status">The result code as text.</param>
/// <param name="Before">The target's value before the apply.</param>
/// <param name="After">The target's value after the apply.</param>
/// <param name="Changed">True when the value changed.</param>
/// <param name="Steps">The number of server steps from the request to the apply.</param>
/// <param name="Reason">Why a change was refused or did nothing, in words; empty on a plain success.</param>
public sealed record KnowledgeResult(ulong Request, KnowledgeOp Op, EntityId Target, uint Id, bool Ok, string Status, uint Before,
    uint After, bool Changed, int Steps, string Reason) : IKilnResult
{
    // IKilnResult's reason is null when the change was applied; this record's own Reason may still carry a note then.
    string? IKilnResult.Reason => Ok ? null : Reason;

    /// <summary>The change and the values around it, or the failure and its reason.</summary>
    public override string ToString() => Ok
        ? $"{Op} 0x{Id:x8}: {Before} -> {After}{(Reason.Length > 0 ? $" ({Reason})" : "")}"
        : $"{Op} 0x{Id:x8} failed: {Reason}";
}

/// <summary>What kiln.knowledge can do on this build, and its counters.</summary>
/// <param name="Knowledge">Reads and grants work (feature <c>quests.knowledge</c>).</param>
/// <param name="Changes">The in-step apply works: revoke, set, ordered grants and change events (feature
/// <c>quests.changes</c>).</param>
/// <param name="Journal">The journal and the query evaluator work (feature <c>quests.journal</c>).</param>
/// <param name="Npc">The NPC registry's unlock states can be read (feature <c>quests.npc</c>).</param>
/// <param name="Dialogue">Dialogue events and holds work (feature <c>quests.dialogue</c>).</param>
/// <param name="ClientInput">The hold on knowledge that clients send works (feature <c>quests.client_input</c>).</param>
/// <param name="Program">Kiln's knowledge program runs.</param>
/// <param name="World">A server world with a knowledge registry exists.</param>
/// <param name="WorldObjects">The number of static world objects.</param>
/// <param name="PlayerObjects">The number of static player objects.</param>
/// <param name="WorldEntries">The number of entries in the world map now.</param>
/// <param name="Players">The number of player maps loaded now.</param>
/// <param name="Npcs">The number of NPCs in the catalog.</param>
/// <param name="Quests">The number of quests in the catalog.</param>
/// <param name="Queries">The number of named queries in the catalog.</param>
/// <param name="Pending">The number of requests waiting for the next apply.</param>
/// <param name="Submitted">The number of requests submitted.</param>
/// <param name="Applied">The number of requests the game applied.</param>
/// <param name="Failed">The number of requests that failed.</param>
/// <param name="ChangesSeen">The number of change events produced.</param>
/// <param name="ClientOps">The number of knowledge operations clients sent.</param>
/// <param name="ClientVetoed">The number of those operations that were dropped by the client veto.</param>
/// <param name="DialogueEvents">The number of dialogue events seen.</param>
/// <param name="DialogueHeld">The number of dialogue lines whose knowledge was held for a check.</param>
/// <param name="DialogueDropped">The number of held dialogue knowledge operations that were dropped.</param>
/// <param name="QueueMax">The most entries seen in the game's knowledge trigger queue at an apply.</param>
/// <param name="Reason">Why a part is off; empty when all is on.</param>
/// <param name="Applying">The game's knowledge update has run in this world (the world runs and the save's knowledge is
/// in the maps). Before that, reads come back empty and changes wait.</param>
public sealed record KnowledgeStatus(bool Knowledge, bool Changes, bool Journal, bool Npc, bool Dialogue, bool ClientInput,
    bool Program, bool World, int WorldObjects, int PlayerObjects, int WorldEntries, int Players, int Npcs, int Quests, int Queries,
    int Pending, ulong Submitted, ulong Applied, ulong Failed, ulong ChangesSeen, ulong ClientOps, ulong ClientVetoed,
    ulong DialogueEvents, ulong DialogueHeld, ulong DialogueDropped, int QueueMax, string Reason, bool Applying = false)
{
    /// <summary>Which parts are on or off, and the catalog sizes, as text.</summary>
    public override string ToString() =>
        $"knowledge {(Knowledge ? "on" : "OFF")}, in-step changes {(Changes ? "on" : "OFF")}, journal {(Journal ? "on" : "OFF")}, " +
        $"npc {(Npc ? "on" : "OFF")}, dialogue {(Dialogue ? "on" : "OFF")}, client input {(ClientInput ? "on" : "OFF")}, " +
        $"program {(Program ? "on" : "OFF")}; {WorldObjects} world + {PlayerObjects} player objects, world map {WorldEntries} " +
        $"entries{(Applying ? "" : " (world still loading)")}, {Players} player maps; catalog {Npcs} NPCs, {Quests} quests, {Queries} queries";
}

/// <summary>An NPC of the game's roster (NpcCollection) with the NPC registry's unlock state.</summary>
/// <param name="Index">The NPC's index in the roster.</param>
/// <param name="DebugName">The NPC's debug name, such as <c>NPC_Workshop_Bard01</c>.</param>
/// <param name="NpcId">The game's NPC id (FNV-1a of the NPC template's GUID).</param>
/// <param name="NameTag">The loca tag of the NPC's name.</param>
/// <param name="Workshop">The NPC's workshop id; 0 for none.</param>
/// <param name="SummonItem">The item id of the NPC's summon tool.</param>
/// <param name="Unlock">The requirement that unlocks the NPC. An empty one means the NPC is always unlocked.</param>
/// <param name="Unlocked">Whether the game's NPC registry says the NPC is unlocked; null when it is not known.</param>
/// <param name="CraftingAssistant">Whether the NPC is a crafting assistant.</param>
public sealed record KnowledgeNpc(int Index, string DebugName, uint NpcId, uint NameTag, uint Workshop, uint SummonItem,
    KnowledgeQuery Unlock, bool? Unlocked, bool CraftingAssistant)
{
    /// <summary>"npc:&lt;debugName&gt;".</summary>
    public string Name => "npc:" + DebugName;
    /// <summary>The NPC's debug name and id, its unlock requirement and whether it is unlocked.</summary>
    public override string ToString() => $"{DebugName} (0x{NpcId:x8}) unlock {Unlock}: {(Unlocked is null ? "?" : Unlocked.Value ? "unlocked" : "locked")}";
}

/// <summary>The knowledge of one target: the world (<see cref="Knowledge.World"/>) or one player
/// (<see cref="Knowledge.For(EntityId)"/>). Ids are given as numbers or names ("npc:NPC_Workshop_Bard01.unlock",
/// "quest:0x8755c335", "0x88b160c1"; see <see cref="Knowledge.Resolve"/>).</summary>
public sealed class KnowledgeScope
{
    internal KnowledgeScope(EntityId target) => Target = target;

    /// <summary>0 = the world.</summary>
    public EntityId Target { get; }
    /// <summary>True for the world's knowledge.</summary>
    public bool IsWorld => Target.IsNone;

    /// <summary>The id's state (tick thread); null when the id is not an object of this kind of target.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public KnowledgeState? Get(uint id) => Knowledge.Get(Target, id);
    /// <summary>The name's state, as <see cref="Get(uint)"/> (tick thread). The name is resolved as in
    /// <see cref="Knowledge.Resolve"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public KnowledgeState? Get(string name) => Knowledge.Get(Target, Knowledge.KnowledgeId(name, nameof(Get)));

    /// <summary>True when the id is set; for a name with a requirement (an NPC's unlock, a quest step, a named query) the
    /// requirement is evaluated with the game's evaluator. Tick thread.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public bool Has(uint id) => Get(id)?.IsSet ?? false;
    /// <summary>True when the name holds, as <see cref="Has(uint)"/>; a query is evaluated (tick thread).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public bool Has(string name) => Knowledge.Has(Target, name);

    /// <summary>Every set id of this target (tick thread).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public IReadOnlyList<KnowledgeState> List() => Knowledge.List(Target);

    /// <summary>Grants through the game's own knowledge apply (XP, triggers, replication: the game's). Bool: set; Counter:
    /// + <paramref name="value"/>; Flags: flag number <paramref name="value"/>. A player grant of a world object with a
    /// player twin also sets the world object (the game's rule).</summary>
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Grant(uint id, uint value = 1) => Knowledge.Submit(KnowledgeOp.Grant, Target, id, value);
    /// <summary>Grants a name, as <see cref="Grant(uint, uint)"/>. A query name cannot be granted (it throws
    /// <see cref="InvalidOperationException"/>).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Grant(string name, uint value = 1) => Grant(Knowledge.KnowledgeId(name, nameof(Grant)), value);

    /// <summary>Clears the id (Flags: one flag, or all with -1). A direct write of the map, the change list and the version;
    /// clients erase it, the next save leaves it out. Re-granting a player object pays its XP again.</summary>
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Revoke(uint id, int flag = -1) =>
        Knowledge.Submit(KnowledgeOp.Revoke, Target, id, flag < 0 ? uint.MaxValue : (uint)flag);
    /// <summary>Revokes a name, as <see cref="Revoke(uint, int)"/> (tick thread).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Revoke(string name, int flag = -1) => Revoke(Knowledge.KnowledgeId(name, nameof(Revoke)), flag);

    /// <summary>Writes an exact value (a direct write: no XP, no query-trigger event).</summary>
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Set(uint id, uint value) => Knowledge.Submit(KnowledgeOp.Set, Target, id, value);
    /// <summary>Writes an exact value for a name, as <see cref="Set(uint, uint)"/> (tick thread).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public Task<KnowledgeResult> Set(string name, uint value) => Set(Knowledge.KnowledgeId(name, nameof(Set)), value);

    /// <summary>A short text: "world knowledge", or "knowledge of player" and the entity.</summary>
    public override string ToString() => IsWorld ? "world knowledge" : $"knowledge of player {Target}";
}

/// <summary>The game's knowledge: every unlock, quest step, line heard and NPC rescued (kiln.knowledge).</summary>
/// <remarks>Reads run on the tick thread. Changes are applied in the next server-world step inside the game's own
/// knowledge update; the task completes on the tick thread right after that step. Game reasons (player left, already
/// set) come back as <see cref="KnowledgeResult.Ok"/> = false / <see cref="KnowledgeResult.Changed"/> = false, never as
/// an exception; unknown ids and bad arguments throw.</remarks>
public static unsafe class Knowledge
{
    /// <summary>True when kiln.knowledge is loaded (see <see cref="Status"/> for its parts).</summary>
    public static bool Available => KnowledgeNative.T != null;

    /// <summary>What kiln.knowledge can do on this build, and its counters; null when the service is not loaded.</summary>
    public static KnowledgeStatus? Status
    {
        get
        {
            var t = KnowledgeNative.T;
            if (t == null) return null;
            KnowledgeNative.Status s = default;
            s.Size = (uint)sizeof(KnowledgeNative.Status);
            if (t->Status(&s) != KnowledgeNative.Ok) return null;
            return new KnowledgeStatus(s.Knowledge != 0, s.Changes != 0, s.Journal != 0, s.Npc != 0, s.Dialogue != 0, s.ClientInput != 0,
                s.Program != 0, s.World != 0, (int)s.WorldObjects, (int)s.PlayerObjects, (int)s.WorldEntries, (int)s.Players, (int)s.Npcs,
                (int)s.Quests, (int)s.Queries, (int)s.Pending, s.Submitted, s.Applied, s.Failed, s.ChangesSeen, s.ClientOps, s.ClientVetoed,
                s.DialogueEvents, s.DialogueHeld, s.DialogueDropped, (int)s.QueueMax, KnowledgeNative.Text(s.Reason, 160), s.Applying != 0);
        }
    }

    /// <summary>Diagnostics, tick thread, lab only: plays a joining client's record on a free player block (phase 0 the login's
    /// activation, 1 the record's load, 2 takes the block back), for tests of the change events' baseline. False when it
    /// did not work (the block has a player, an unknown build).</summary>
    [System.Diagnostics.CodeAnalysis.Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [System.ComponentModel.EditorBrowsable(System.ComponentModel.EditorBrowsableState.Never)]
    public static bool LabRecord(EntityId block, int phase, params (uint Id, uint Value)[] record)
    {
        var t = KnowledgeNative.T;
        if (t == null) return false;
        var ids = record.Select(r => r.Id).ToArray();
        var values = record.Select(r => r.Value).ToArray();
        fixed (uint* i = ids, v = values) return t->LabRecord(block.Value, (uint)phase, i, v, (uint)record.Length) == KnowledgeNative.Ok;
    }

    /// <summary>The world's (shared) knowledge.</summary>
    public static KnowledgeScope World { get; } = new(EntityId.None);
    /// <summary>One player's knowledge (while they are on the server).</summary>
    public static KnowledgeScope For(EntityId player) => player.IsNone ? World : new KnowledgeScope(player);
    /// <summary>One player's knowledge, as <see cref="For(EntityId)"/>.</summary>
    public static KnowledgeScope For(Player player) => For(player.Entity);

    // ---- shortcuts (player first) ----
    /// <summary>Grants <paramref name="name"/> to a player, as <see cref="KnowledgeScope.Grant(string, uint)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> Grant(EntityId player, string name, uint value = 1) => For(player).Grant(name, value);
    /// <summary>Grants <paramref name="name"/> to a player, as <see cref="KnowledgeScope.Grant(string, uint)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> Grant(Player player, string name, uint value = 1) => For(player.Entity).Grant(name, value);
    /// <summary>Revokes <paramref name="name"/> for a player, as <see cref="KnowledgeScope.Revoke(string, int)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> Revoke(EntityId player, string name, int flag = -1) => For(player).Revoke(name, flag);
    /// <summary>Revokes <paramref name="name"/> for a player, as <see cref="KnowledgeScope.Revoke(string, int)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> Revoke(Player player, string name, int flag = -1) => For(player.Entity).Revoke(name, flag);
    /// <summary>True when the name holds for the player (tick thread), as <see cref="Has(EntityId, string)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static bool Has(Player player, string name) => Has(player.Entity, name);
    /// <summary>The name's state in the player's map (tick thread), as <see cref="KnowledgeScope.Get(string)"/>.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static KnowledgeState? Get(Player player, string name) => For(player.Entity).Get(name);

    /// <summary>What GameKnowledgeResource says about an id; null when the game does not know it.</summary>
    [RequiresCapability("kiln.knowledge")]
    public static KnowledgeInfo? Info(uint id)
    {
        var t = KnowledgeNative.Require(nameof(Info));
        KnowledgeNative.Info i = default;
        i.Size = (uint)sizeof(KnowledgeNative.Info);
        var rc = t->Info(id, &i);
        if (rc == KnowledgeNative.NotFound) return null;
        KnowledgeNative.Check(rc, nameof(Info));
        return new KnowledgeInfo(i.Id, (KnowledgeObjectFlags)i.Flags, (KnowledgeType)i.WorldType, (KnowledgeType)i.PlayerType,
            i.WorldFlags, i.PlayerFlags, i.ExperienceId, i.SkillPoints);
    }

    /// <summary>An id in a target's map (tick thread). Null when the id is not an object of that kind of target (a
    /// player-only id asked of the world, an unknown id). Throws <see cref="NotSupportedException"/> when the player is not
    /// in game.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static KnowledgeState? Get(EntityId target, uint id)
    {
        var t = KnowledgeNative.Require(nameof(Get));
        Services.RequireTickThread("Knowledge.Get");
        KnowledgeNative.State s = default;
        s.Size = (uint)sizeof(KnowledgeNative.State);
        var rc = t->Get(target.Value, id, &s);
        if (rc == KnowledgeNative.NotFound) return null;
        KnowledgeNative.Check(rc, nameof(Get));
        return new KnowledgeState(new EntityId(s.Target), s.Id, s.Value, (KnowledgeType)s.Type, s.Present != 0, s.NewMask, s.Step);
    }

    /// <summary>True when the name's requirement holds for the target (see <see cref="KnowledgeScope.Has(string)"/>).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static bool Has(EntityId target, string name)
    {
        var n = Resolve(name);
        var q = n.Requirement;
        if (!n.IsQuery && q.Type == KnowledgeQueryType.SimpleBool && q.Compare == KnowledgeCompare.Equals && q.CompareValue == 1)
            return Get(target, n.Id)?.IsSet ?? false;
        return Query(target, q, target.IsNone ? KnowledgeMode.Shared : KnowledgeMode.Player);
    }

    /// <summary>Every set id of a target (tick thread).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static IReadOnlyList<KnowledgeState> List(EntityId target)
    {
        var t = KnowledgeNative.Require(nameof(List));
        Services.RequireTickThread("Knowledge.List");
        var buf = new KnowledgeNative.Entry[2048];
        while (true)
        {
            uint n = 0;
            int rc;
            fixed (KnowledgeNative.Entry* p = buf) rc = t->List(target.Value, p, (uint)buf.Length, &n);
            if (rc == KnowledgeNative.BufferTooSmall && n > buf.Length)
            {
                buf = new KnowledgeNative.Entry[n + 256];
                continue;
            }
            KnowledgeNative.Check(rc, nameof(List));
            var list = new List<KnowledgeState>((int)n);
            for (var i = 0; i < n; i++)
                list.Add(new KnowledgeState(target, buf[i].Id, buf[i].Value, KnowledgeType.None, true, 0, ServiceTick.Step));
            return list;
        }
    }

    /// <summary>Evaluates a requirement with the game's evaluator (named queries, NPC / inventory / flame runtime queries
    /// included) for the world (<paramref name="target"/> 0; player queries throw <see cref="ArgumentException"/>) or a
    /// player. Tick thread.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static bool Query(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player)
    {
        var t = KnowledgeNative.Require(nameof(Query));
        Services.RequireTickThread("Knowledge.Query");
        var q = KnowledgeNative.ToNative(query);
        byte r = 0;
        var rc = t->Query(target.Value, &q, (uint)mode, &r);
        if (rc == KnowledgeNative.NotFound) throw new KeyNotFoundException($"Knowledge.Query: the game does not know {query}");
        if (rc == KnowledgeNative.InvalidArgument)
            throw new ArgumentException($"Knowledge.Query: {query} cannot be evaluated for {(target.IsNone ? "the world (a player query)" : $"player {target}")}");
        KnowledgeNative.Check(rc, nameof(Query));
        return r != 0;
    }

    /// <summary>Evaluates a requirement; null when it cannot be (no player for a player query, unknown query, part off).</summary>
    [TickThreadOnly]
    public static bool? TryQuery(EntityId target, KnowledgeQuery query, KnowledgeMode mode = KnowledgeMode.Player)
    {
        try
        {
            return Query(target, query, mode);
        }
        catch (Exception e) when (e is KeyNotFoundException or ArgumentException or NotSupportedException)
        {
            return null;
        }
    }

    // ---- names ----
    private static readonly ConcurrentDictionary<string, KnowledgeName> NameCache = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>Resolves a name: "0x1234abcd" / decimal, "npc:&lt;debugName&gt;[.unlock]", "quest:0x&lt;entryId&gt;"
    /// (the quest's "rewarded" knowledge) and "quest:0x&lt;entryId&gt;.step&lt;n&gt;.visible|.done",
    /// "query:&lt;query name&gt;", "line:&lt;template&gt;/option&lt;k&gt;|init&lt;k&gt;|transition&lt;k&gt;|farewell&lt;k&gt;".
    /// Throws <see cref="KeyNotFoundException"/> for an unknown name. Tick thread for names (raw ids: any thread); the
    /// catalog exists from the world's first steps on.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static KnowledgeName Resolve(string name) =>
        TryResolve(name, out var n) ? n : throw new KeyNotFoundException($"Knowledge: unknown knowledge name '{name}'");

    /// <summary>Resolves a name like <see cref="Resolve"/>, without throwing: false for an unknown name or when the
    /// catalog is not built yet.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static bool TryResolve(string name, out KnowledgeName result)
    {
        ArgumentNullException.ThrowIfNull(name);
        result = null!;
        var s = name.Trim();
        if (NameCache.TryGetValue(s, out var cached))
        {
            result = cached;
            return true;
        }
        if (TryParseId(s, out var raw))
        {
            result = new KnowledgeName(s, false, raw, KnowledgeQuery.IsSet(raw));
            return true;
        }
        var t = KnowledgeNative.Require(nameof(Resolve));
        Services.RequireTickThread("Knowledge.Resolve");
        KnowledgeNative.Name n = default;
        n.Size = (uint)sizeof(KnowledgeNative.Name);
        var bytes = Encoding.UTF8.GetBytes(s);
        int rc;
        fixed (byte* p = bytes) rc = t->Resolve(new NativeStr { Ptr = p, Len = (nuint)bytes.Length }, &n);
        if (rc == KnowledgeNative.NotFound) return false;
        if (rc == KnowledgeNative.Unavailable) return false;  // catalog not built yet
        KnowledgeNative.Check(rc, nameof(Resolve));
        result = new KnowledgeName(s, n.Kind == 2, n.Id, KnowledgeNative.FromNative(n.Query));
        NameCache[s] = result;
        return true;
    }

    /// <summary>The catalog name of a knowledge id ("npc:…​.unlock", "quest:0x…") or of a query id ("query:…"); null
    /// when it has none. Tick thread.</summary>
    public static string? NameOf(uint id, bool query = false)
    {
        var t = KnowledgeNative.T;
        if (t == null || !Game.IsTickThread) return null;
        var buf = stackalloc byte[512];
        nuint len = 0;
        var rc = t->NameOf(id, query ? 2u : 1u, buf, 512, &len);
        return rc == KnowledgeNative.Ok || rc == KnowledgeNative.BufferTooSmall ? KnowledgeNative.Text(buf, 512) : null;
    }

    /// <summary>The catalog's names (NPC unlocks, journal quests, named queries). Tick thread.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static IReadOnlyList<KnowledgeName> Catalog(KnowledgeCatalog catalog)
    {
        var t = KnowledgeNative.Require(nameof(Catalog));
        Services.RequireTickThread("Knowledge.Catalog");
        var count = t->CatalogCount((uint)catalog);
        var list = new List<KnowledgeName>((int)count);
        var buf = stackalloc byte[512];
        for (uint i = 0; i < count; i++)
        {
            KnowledgeNative.Name n = default;
            n.Size = (uint)sizeof(KnowledgeNative.Name);
            nuint len = 0;
            if (t->CatalogAt((uint)catalog, i, &n, buf, 512, &len) is not (KnowledgeNative.Ok or KnowledgeNative.BufferTooSmall)) continue;
            list.Add(new KnowledgeName(KnowledgeNative.Text(buf, 512), n.Kind == 2, n.Id, KnowledgeNative.FromNative(n.Query)));
        }
        return list;
    }

    /// <summary>The game's NPC roster with the registry's unlock state (tick thread; empty before the catalog exists).</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static IReadOnlyList<KnowledgeNpc> Npcs
    {
        get
        {
            var t = KnowledgeNative.Require(nameof(Npcs));
            Services.RequireTickThread("Knowledge.Npcs");
            var count = t->CatalogCount(1);
            var list = new List<KnowledgeNpc>((int)count);
            var buf = stackalloc byte[256];
            for (uint i = 0; i < count; i++)
            {
                KnowledgeNative.Npc n = default;
                n.Size = (uint)sizeof(KnowledgeNative.Npc);
                nuint len = 0;
                if (t->Npc(i, &n, buf, 256, &len) is not (KnowledgeNative.Ok or KnowledgeNative.BufferTooSmall)) continue;
                list.Add(new KnowledgeNpc((int)i, KnowledgeNative.Text(buf, 256), n.NpcId, n.Name, n.Workshop, n.Item,
                    KnowledgeNative.FromNative(n.Unlock), n.Unlocked == 0xff ? null : n.Unlocked != 0, n.CraftingAssistant != 0));
            }
            return list;
        }
    }

    /// <summary>The NPC with this debug name ("NPC_Workshop_Bard01") or "npc:" name; null when there is none.</summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.knowledge")]
    public static KnowledgeNpc? FindNpc(string name)
    {
        var s = name.StartsWith("npc:", StringComparison.OrdinalIgnoreCase) ? name[4..] : name;
        if (s.EndsWith(".unlock", StringComparison.OrdinalIgnoreCase)) s = s[..^7];
        foreach (var n in Npcs)
            if (n.DebugName.Equals(s, StringComparison.OrdinalIgnoreCase)) return n;
        return null;
    }

    /// <summary>Parses a raw id: "0x1234abcd" or decimal.</summary>
    public static bool TryParseId(string s, out uint id)
    {
        id = 0;
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            return uint.TryParse(s.AsSpan(2), System.Globalization.NumberStyles.HexNumber, null, out id);
        return s.Length > 0 && char.IsAsciiDigit(s[0]) && uint.TryParse(s, out id);
    }

    internal static uint KnowledgeId(string name, string api)
    {
        var n = Resolve(name);
        if (n.IsQuery)
            throw new InvalidOperationException($"Knowledge.{api}: '{name}' is a query (a condition over other knowledge), not knowledge; " +
                                                "evaluate it with Knowledge.Query");
        return n.Id;
    }

    // ---- changes ----
    private static readonly object Lock = new();
    private static readonly Dictionary<ulong, (TaskCompletionSource<KnowledgeResult> Tcs, KnowledgeOp Op, EntityId Target, uint Id, ulong Since)> Waiting = new();
    private static IDisposable? _pump;

    /// <summary>A grant the game's apply gets even when <paramref name="id"/> is in no static map, where the game drops it
    /// (<see cref="KnowledgeResult.After"/> stays 0). For tests of the game's own rule; docs/kiln/services/npcs.md.</summary>
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> GrantUnchecked(EntityId target, uint id) => Submit(KnowledgeOp.Grant, target, id, 1, 1);

    /// <summary>Queues a change (any thread). Unknown ids and bad flags throw <see cref="ArgumentException"/>; a player who
    /// is not in game or a part that is off completes the task with Ok = false. The task completes on the tick thread after
    /// the step in which the game applied the change.</summary>
    /// <param name="op">The change: grant, revoke or set.</param>
    /// <param name="target">The player entity whose knowledge changes; <see cref="EntityId.None"/> for the world.</param>
    /// <param name="id">The knowledge id.</param>
    /// <param name="value">Its meaning follows <paramref name="op"/>: for a grant, the counter amount to add or the flag number to set;
    /// for a revoke, the flag number to clear or <c>uint.MaxValue</c> for all; for a set, the exact new value.</param>
    [RequiresCapability("kiln.knowledge")]
    public static Task<KnowledgeResult> Submit(KnowledgeOp op, EntityId target, uint id, uint value) => Submit(op, target, id, value, 0);

    private static Task<KnowledgeResult> Submit(KnowledgeOp op, EntityId target, uint id, uint value, uint flags)
    {
        var t = KnowledgeNative.Require(op.ToString());
        var req = new KnowledgeNative.Request
        {
            Size = (uint)sizeof(KnowledgeNative.Request), Op = (uint)op, Target = target.Value, Id = id, Value = value, Flags = flags,
        };
        ulong rid = 0;
        var rc = t->Submit(Services.CallerOwner, &req, &rid);
        if (rc == KnowledgeNative.NotFound)
            throw new ArgumentException($"Knowledge.{op}: 0x{id:x8} is not {(target.IsNone ? "world" : "player")} knowledge in this game build");
        if (rc == KnowledgeNative.InvalidArgument)
            throw new ArgumentException($"Knowledge.{op}: invalid value {value} for 0x{id:x8} (a flag that does not exist, or 0 for a counter)");
        if (rc != KnowledgeNative.Ok)
        {
            var why = rc switch
            {
                KnowledgeNative.Unavailable => target.IsNone || op != KnowledgeOp.Grant
                    ? "this does not work on this server build or there is no world (see Knowledge.Status)"
                    : $"player {target} is not in game (their knowledge is not loaded)",
                KnowledgeNative.BufferTooSmall => "too many changes are waiting; try again in a moment",
                _ => $"kiln.knowledge returned {rc}",
            };
            return Task.FromResult(new KnowledgeResult(0, op, target, id, false, KnowledgeNative.Code(rc), 0, 0, false, 0, why));
        }
        var tcs = new TaskCompletionSource<KnowledgeResult>(TaskCreationOptions.None);  // continuations: tick thread
        lock (Lock)
        {
            Waiting[rid] = (tcs, op, target, id, ServiceTick.Step);
            _pump ??= ServiceTick.Add("kiln.knowledge results", Pump);
        }
        return tcs.Task;
    }

    private static void Pump(in TickInfo tick)
    {
        var t = KnowledgeNative.T;
        if (t == null) return;
        List<(ulong Id, TaskCompletionSource<KnowledgeResult> Tcs, KnowledgeOp Op, EntityId Target, uint Kid, ulong Since)> due;
        lock (Lock)
        {
            if (Waiting.Count == 0) return;
            due = [.. Waiting.Select(kv => (kv.Key, kv.Value.Tcs, kv.Value.Op, kv.Value.Target, kv.Value.Id, kv.Value.Since))];
        }
        foreach (var (id, tcs, op, target, kid, since) in due)
        {
            KnowledgeNative.Result r = default;
            r.Size = (uint)sizeof(KnowledgeNative.Result);
            if (t->Result(id, &r) != KnowledgeNative.Ok)
            {
                if (tick.Step > since + 400)  // the native side fails a request after 50 steps; this is the backstop
                {
                    lock (Lock) Waiting.Remove(id);
                    tcs.TrySetResult(new KnowledgeResult(id, op, target, kid, false, "lost", 0, 0, false, 0, "no result from kiln.knowledge"));
                }
                continue;
            }
            lock (Lock) Waiting.Remove(id);
            tcs.TrySetResult(new KnowledgeResult(id, op, new EntityId(r.Target), r.Id, r.Status == KnowledgeNative.Ok,
                KnowledgeNative.Code(r.Status), r.Before, r.After, r.Changed != 0, (int)r.Steps, KnowledgeNative.Text(r.Reason, 128)));
        }
    }

    /// <summary>The opt-in server checks (kiln/config/knowledge.json; both off by default). See
    /// docs/kiln/services/knowledge.md "Security" for what they protect and what they can break.</summary>
    public static class Security
    {
        /// <summary>The settings in use.</summary>
        [RequiresCapability("kiln.knowledge")]
        public static KnowledgeSecurity Current
        {
            get
            {
                var t = KnowledgeNative.Require("Security");
                KnowledgeNative.Settings s = default;
                s.Size = (uint)sizeof(KnowledgeNative.Settings);
                KnowledgeNative.Check(t->GetSettings(&s), "Security");
                return new KnowledgeSecurity(s.VetoClient != 0, s.CheckDialogue != 0, s.DialogueRange, s.LogClient != 0, (int)s.AllowCount);
            }
        }

        /// <summary>Applies settings now (until kiln/config/knowledge.json changes). <paramref name="save"/> also writes them
        /// into that file (tick thread).</summary>
        [RequiresCapability("kiln.knowledge")]
        public static void Apply(KnowledgeSecurity settings, bool save = false)
        {
            var t = KnowledgeNative.Require("Security");
            var s = new KnowledgeNative.Settings
            {
                Size = (uint)sizeof(KnowledgeNative.Settings), VetoClient = settings.VetoClientKnowledge ? (byte)1 : (byte)0,
                CheckDialogue = settings.CheckDialogue ? (byte)1 : (byte)0, LogClient = settings.LogClientKnowledge ? (byte)1 : (byte)0,
                DialogueRange = (float)settings.DialogueRange,
            };
            KnowledgeNative.Check(t->Configure(&s), "Security.Apply");
            if (save) KnowledgeSettingsFile.Save(settings);
        }

        /// <summary>Ids that pass the client veto at run time (in addition to the file's allowClientKnowledge).</summary>
        [RequiresCapability("kiln.knowledge")]
        public static void Allow(IEnumerable<uint> ids)
        {
            var t = KnowledgeNative.Require("Security.Allow");
            var a = ids.ToArray();
            fixed (uint* p = a) KnowledgeNative.Check(t->Allow(p, (uint)a.Length), "Security.Allow");
        }
    }
}

/// <summary>Catalogs of <see cref="Knowledge.Catalog"/>.</summary>
public enum KnowledgeCatalog : uint
{
    /// <summary>The NPC roster: <c>npc:&lt;debugName&gt;</c> names, each with its unlock requirement.</summary>
    Npcs = 1,
    /// <summary>The journal's quests: <c>quest:0x&lt;entryId&gt;</c> names.</summary>
    Quests = 2,
    /// <summary>The game's named queries: <c>query:&lt;name&gt;</c> names.</summary>
    Queries = 3,
}

/// <summary>The opt-in checks (<see cref="Knowledge.Security"/>).</summary>
/// <param name="VetoClientKnowledge">Hold every knowledge operation a client sends (TriggerKnowledgeAction) and drop it
/// unless its id is allowed or a <see cref="KnowledgeRequestEvent"/> handler allows it. Off by default: the vanilla
/// client sends some knowledge itself (tutorial steps), which the veto would drop.</param>
/// <param name="CheckDialogue">Hold a dialogue line's knowledge and run it only if the line's requirement holds for the
/// player and the player is within <paramref name="DialogueRange"/> metres of the NPC.</param>
/// <param name="DialogueRange">The most metres between player and NPC for <paramref name="CheckDialogue"/> (default 12; 0
/// means no distance check).</param>
/// <param name="LogClientKnowledge">Log each distinct knowledge id a client sends, once per player and session (default
/// true).</param>
/// <param name="AllowedIds">The number of ids in the client veto's allow list (read-only information:
/// <see cref="Knowledge.Security.Apply"/> ignores it; use <see cref="Knowledge.Security.Allow"/>).</param>
public sealed record KnowledgeSecurity(bool VetoClientKnowledge = false, bool CheckDialogue = false, double DialogueRange = 12.0,
    bool LogClientKnowledge = true, int AllowedIds = 0);
