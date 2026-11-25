// SPDX-License-Identifier: MIT
// ClientSkills (since 0.2.0-dev.31.13, mod API 0.2.22): the local player's skill tree as the game client holds it,
// read-only, for a player who joined someone else's world. Native: kiln.skills.client (native/core/abi/services/
// kiln_skills_client.h, in kiln-skills.dll); guide: docs/kiln/services/skills.md ("On a joined client: the local player's
// tree"); evidence: docs/re/progression.md section 8.
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>The blittable mirror of kiln.skills.client v1: a node 32 bytes, a link 16, the state 216, the table 32 (x64).</summary>
internal static unsafe class ClientSkillsNative
{
    public const string Service = "kiln.skills.client";
    public const uint Version = 1;
    public const byte FlagUnlocked = 0x1, FlagShadow = 0x2, FlagRecord = 0x4;
    public const int Ok = 0, InvalidArgument = 1, NotFound = 2, Unavailable = 3, BufferTooSmall = 5;

    [StructLayout(LayoutKind.Sequential)]
    public struct Node
    {
        public uint Id;
        public ushort Index, Cost;
        public byte Type, ClassAlignment, MaxLevel, Level, Flags, LevelIndex;
        public ushort Reserved;
        public float X, Y;
        public uint Incoming, Outgoing;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Link
    {
        public uint Id, From, To;
        public byte Bidirectional, R0, R1, R2;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct State
    {
        public uint Size;
        public byte On, World, Copying, Reserved0;
        public uint Player;
        public int Total, Spent, Available;
        public uint Nodes, Unlocked, Links, Reserved1;
        public ulong Step;
        public long AgeMs;
        public fixed byte Detail[160];
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Table
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<State*, int> GetState;
        public delegate* unmanaged[Cdecl]<Node*, uint, uint*, int> Nodes;
        public delegate* unmanaged[Cdecl]<Link*, uint, uint*, int> Links;
    }

    internal static bool LayoutOk => sizeof(Node) == 32 && sizeof(Link) == 16 && sizeof(State) == 216 && sizeof(Table) == 32;

    private static Table* _table;
    private static readonly object Lock = new();

    /// <summary>The table, or null when kiln-skills.dll does not serve it. Looked up until found.</summary>
    public static Table* T
    {
        get
        {
            if (_table != null) return _table;
            if (!LayoutOk) return null;
            lock (Lock)
            {
                if (_table != null) return _table;
                var t = (Table*)SkillsInternals.Service(Service, Version);
                return _table = t != null && t->Header.Size >= (uint)sizeof(Table) && t->Nodes != null ? t : null;
            }
        }
    }

    public static string Text(byte* p, int cap)
    {
        var n = 0;
        while (n < cap && p[n] != 0) n++;
        return System.Text.Encoding.UTF8.GetString(p, n);
    }
}

/// <summary>
/// The local player's skill tree as the game client itself holds it, read-only: the tree's layout (every node, its
/// position on the skill screen, its links), the player's unlocked nodes and levels, and their skill points. For a player
/// who joined someone else's world, where <see cref="Progression"/> has no server world; it works the same on a hosting
/// player's own client. Since 0.2.0-dev.31.13, mod API 0.2.22. Thanks XHL.
/// </summary>
/// <remarks>
/// <para><b>Where it comes from.</b> The client's own world: the skill tree it loaded (with the nodes mods add) and what the
/// game's skill screen program fills every client step for the local player from the replicated character data (its
/// unlock masks and levels), with the screen's skill points (spent: the game's own sum; available: total minus spent).
/// Shadow nodes (past the game's caps, <see cref="GameTree"/>) read as the table the server sent their client.</para>
/// <para><b>Stale.</b> What the server changes (an unlock, a respec, a level-up's points) shows on the client's next update
/// of its player, as on the game's own screen; nothing here asks the server.</para>
/// <para><b>Threads.</b> Any thread: every read answers from the copy of the client world's last step. The first read
/// after a pause of 5 seconds starts the copying and answers from the previous copy (or none yet: null, empty). Nothing here
/// writes or sends anything: unlocking is still the game's own screen or your server half's
/// (<see cref="Progression"/>).</para>
/// <para><b>Off.</b> <see cref="Capability"/> (<c>skills.client_view</c>) is off on a dedicated server, on a game build
/// whose screen data sigdb does not qualify, and after a fault; then every member but <see cref="Available"/> throws
/// <see cref="KilnUnavailableException"/> naming it.</para>
/// </remarks>
public static unsafe class ClientSkills
{
    /// <summary>The capability row: the client's skill tree can be read here (sigdb feature, since 0.2.0-dev.31.13, mod
    /// API 0.2.22).</summary>
    public const string Capability = "skills.client_view";

    internal static readonly ServiceCapRows Rows = new(ClientSkillsNative.Service, LowerWithoutService);

    static ClientSkills() => Rows.Ensure();

    private static void LowerWithoutService()
    {
        if (ClientSkillsNative.T == null) ServiceCaps.LowerFeatures(ClientSkillsNative.Service, Capability);
    }

    /// <summary>The view works here now (a game client whose build qualifies the screen data, nothing faulted). Since
    /// 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    [AnyThread]
    public static bool Available
    {
        get
        {
            var t = ClientSkillsNative.T;
            if (t == null) return false;
            ClientSkillsNative.State s = default;
            s.Size = (uint)sizeof(ClientSkillsNative.State);
            var rc = t->GetState(&s);
            return (rc == ClientSkillsNative.Ok || rc == ClientSkillsNative.NotFound) && s.On != 0;
        }
    }

    /// <summary>The local player's skill points and counts; null before the first copy (no local player, the tree not
    /// loaded, or the copying just started). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static ClientSkillPoints? Points
    {
        get
        {
            var t = Require("ClientSkills.Points");
            ClientSkillsNative.State s = default;
            s.Size = (uint)sizeof(ClientSkillsNative.State);
            var rc = t->GetState(&s);
            if (rc == ClientSkillsNative.NotFound) return null;
            Check(rc, "ClientSkills.Points", ClientSkillsNative.Text(s.Detail, 160));
            return new ClientSkillPoints(new EntityId(s.Player), s.Total, s.Spent, s.Available, (int)s.Unlocked, (int)s.Nodes, s.Step,
                TimeSpan.FromMilliseconds(Math.Max(0, s.AgeMs)));
        }
    }

    /// <summary>Every node of the client's tree by tree index, with the local player's level of it: the tree's layout and
    /// the player's state in one list. Empty before the first copy. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static IReadOnlyList<ClientSkillNode> Nodes
    {
        get
        {
            var t = Require("ClientSkills.Nodes");
            var rows = new ClientSkillsNative.Node[512];
            uint n0 = 0;
            while (true)
            {
                int rc;
                fixed (ClientSkillsNative.Node* p = rows) rc = t->Nodes(p, (uint)rows.Length, &n0);
                if (rc == ClientSkillsNative.BufferTooSmall && n0 > rows.Length && n0 <= 1 << 16)
                {
                    rows = new ClientSkillsNative.Node[n0];
                    continue;
                }
                Check(rc == ClientSkillsNative.BufferTooSmall ? ClientSkillsNative.Ok : rc, "ClientSkills.Nodes", null);
                break;
            }
            return rows.Take((int)Math.Min(n0, (uint)rows.Length)).Select(n => new ClientSkillNode(n.Id, n.Index, (SkillNodeType)n.Type, n.ClassAlignment, n.Cost, n.MaxLevel, n.Level,
                (n.Flags & ClientSkillsNative.FlagUnlocked) != 0, (n.Flags & ClientSkillsNative.FlagShadow) != 0, n.X, n.Y,
                (int)n.Incoming, (int)n.Outgoing)).ToList();
        }
    }

    /// <summary>The nodes the local player has (level above 0), by tree index. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static IReadOnlyList<ClientSkillNode> Unlocked => Nodes.Where(n => n.Unlocked).ToList();

    /// <summary>Every link of the client's tree (node ids; bidirectional links work both ways). Since 0.2.0-dev.31.13, mod
    /// API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static IReadOnlyList<ClientSkillLink> Links
    {
        get
        {
            var t = Require("ClientSkills.Links");
            var rows = new ClientSkillsNative.Link[1024];
            uint n0 = 0;
            while (true)
            {
                int rc;
                fixed (ClientSkillsNative.Link* p = rows) rc = t->Links(p, (uint)rows.Length, &n0);
                if (rc == ClientSkillsNative.BufferTooSmall && n0 > rows.Length && n0 <= 1 << 16)
                {
                    rows = new ClientSkillsNative.Link[n0];
                    continue;
                }
                Check(rc == ClientSkillsNative.BufferTooSmall ? ClientSkillsNative.Ok : rc, "ClientSkills.Links", null);
                break;
            }
            return rows.Take((int)Math.Min(n0, (uint)rows.Length)).Select(l => new ClientSkillLink(l.Id, l.From, l.To, l.Bidirectional != 0)).ToList();
        }
    }

    /// <summary>The node with this id in the client's tree, or null. Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
    /// <exception cref="KilnUnavailableException"><see cref="Capability"/> is off.</exception>
    [AnyThread, ClientOnly, RequiresCapability(Capability)]
    public static ClientSkillNode? Find(uint id)
    {
        foreach (var n in Nodes)
            if (n.Id == id) return n;
        return null;
    }

    private static ClientSkillsNative.Table* Require(string api)
    {
        var t = ClientSkillsNative.T;
        if (t == null) throw ServiceCaps.Unavailable(Capability, $"{api}: kiln.skills.client is not loaded");
        if (!Capabilities.Is(Capability)) throw ServiceCaps.Unavailable(Capability, $"{api}: the client's skill-tree view is off");
        return t;
    }

    private static void Check(int rc, string api, string? detail)
    {
        switch (rc)
        {
            case ClientSkillsNative.Ok: return;
            case ClientSkillsNative.Unavailable:
                throw ServiceCaps.Unavailable(Capability, $"{api}: {(detail is { Length: > 0 } ? detail : "the client's skill-tree view is off here")}");
            default: throw new InvalidOperationException($"{api}: kiln.skills.client answered {rc}");
        }
    }
}

/// <summary>The local player's skill points as the client's skill screen has them (<see cref="ClientSkills.Points"/>).
/// Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="Player">The local player.</param>
/// <param name="Total">Skill points in all (from the level and the skill-point knowledge, as the server counts them).</param>
/// <param name="Spent">Spent on unlocked nodes, shadow nodes included.</param>
/// <param name="Available">Left to spend: <paramref name="Total"/> minus <paramref name="Spent"/>, at least 0 (the
/// screen's "available").</param>
/// <param name="UnlockedNodes">Nodes with a level above 0.</param>
/// <param name="TreeNodes">Nodes in the client's tree.</param>
/// <param name="Step">The client world's step of the copy.</param>
/// <param name="Age">Since the copy was taken.</param>
public readonly record struct ClientSkillPoints(EntityId Player, int Total, int Spent, int Available, int UnlockedNodes, int TreeNodes,
    ulong Step, TimeSpan Age)
{
    /// <summary>The points on one line: "4 of 30 skill points left (26 spent, 11 nodes)".</summary>
    public override string ToString() => $"{Available} of {Total} skill points left ({Spent} spent, {UnlockedNodes} nodes)";
}

/// <summary>One node of the client's skill tree and the local player's level of it (<see cref="ClientSkills.Nodes"/>).
/// Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="Id">The node's id (the game's SkillNodeId, as <see cref="SkillNode.Id"/>).</param>
/// <param name="Index">Its tree index.</param>
/// <param name="Type">What kind of node it is.</param>
/// <param name="ClassAlignment">0 Warrior, 1 Mage, 2 Ranger, 3 Neutral.</param>
/// <param name="Cost">Skill points (per level for a node with levels).</param>
/// <param name="MaxLevel">0: one unlock; else levels 1..MaxLevel.</param>
/// <param name="Level">The local player's level: 0 locked, 1 for a plain node, up to <paramref name="MaxLevel"/>.</param>
/// <param name="Unlocked">The player has it (<paramref name="Level"/> above 0).</param>
/// <param name="Shadow">A shadow node past the game's caps (its state is Kiln's table, as the server sent it).</param>
/// <param name="UiX">Its x position on the game's skill screen.</param>
/// <param name="UiY">Its y position on the game's skill screen.</param>
/// <param name="Incoming">Links into it.</param>
/// <param name="Outgoing">Links out of it.</param>
public readonly record struct ClientSkillNode(uint Id, int Index, SkillNodeType Type, int ClassAlignment, int Cost, int MaxLevel, int Level,
    bool Unlocked, bool Shadow, float UiX, float UiY, int Incoming, int Outgoing)
{
    /// <summary>What its levels cost now: <see cref="Cost"/> times <see cref="Level"/>.</summary>
    public int Spent => Cost * Level;

    /// <summary>The node's id, level and cost on one line.</summary>
    public override string ToString() => $"0x{Id:x8} {Type} {(MaxLevel > 0 ? $"{Level}/{MaxLevel}" : Unlocked ? "unlocked" : "locked")} cost {Cost}";
}

/// <summary>One link of the client's skill tree (<see cref="ClientSkills.Links"/>). Since 0.2.0-dev.31.13, mod API 0.2.22.</summary>
/// <param name="Id">The link's id.</param>
/// <param name="From">The node it leads from.</param>
/// <param name="To">The node it leads to.</param>
/// <param name="Bidirectional">It works both ways.</param>
public readonly record struct ClientSkillLink(uint Id, uint From, uint To, bool Bidirectional);
