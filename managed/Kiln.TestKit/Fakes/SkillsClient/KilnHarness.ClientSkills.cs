// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessClientSkills? _clientSkills;

    /// <summary>The fake kiln.skills.client of this harness: the local player's skill tree as the game client holds it, as
    /// <see cref="ClientSkills"/> reads it. Since 0.2.0-dev.31.13.</summary>
    public HarnessClientSkills ClientSkills => _clientSkills ??= new HarnessClientSkills(this);
}

/// <summary>
/// The client's skill tree of a harness (<see cref="KilnHarness.ClientSkills"/>): what <see cref="Kiln.ClientSkills"/> reads
/// on a joined client. Since 0.2.0-dev.31.13.
/// </summary>
/// <remarks>
/// A test builds the tree (<see cref="AddNode"/>, <see cref="Link"/>), sets the local player's levels (<see cref="Unlock"/>,
/// <see cref="Lock"/>) and their total points (<see cref="Total"/>). Spent is the levels' costs (shadow nodes included, as
/// Kiln's PointsSpent hook makes the game count them), available is total minus spent and never below 0, as the game's
/// screen. <see cref="Loaded"/> false is the time before the first copy. The row <c>skills.client_view</c> can be switched off.
/// </remarks>
public sealed class HarnessClientSkills
{
    private readonly KilnHarness _kiln;

    internal HarnessClientSkills(KilnHarness kiln) => _kiln = kiln;

    private SkillsClientState S => KitFakes.State<SkillsClientState>(_kiln);

    /// <summary>The local player (entity 1 unless a test says).</summary>
    public EntityId Player
    {
        get
        {
            lock (S.Gate) return S.Player;
        }
        set
        {
            lock (S.Gate) S.Player = value;
        }
    }

    /// <summary>The player's skill points in all (level and skill-point knowledge, as the server counts them).</summary>
    public int Total
    {
        get
        {
            lock (S.Gate) return S.Total;
        }
        set
        {
            lock (S.Gate) S.Total = value;
        }
    }

    /// <summary>A copy exists (the client has a local player and its tree). False: <c>ClientSkills.Points</c> is null and
    /// the lists are empty, as before Kiln's first copy.</summary>
    public bool Loaded
    {
        get
        {
            lock (S.Gate) return S.Loaded;
        }
        set
        {
            lock (S.Gate) S.Loaded = value;
        }
    }

    /// <summary>How many reads mods made of the client's skill-tree view.</summary>
    public int Reads
    {
        get
        {
            lock (S.Gate) return S.Reads;
        }
    }

    /// <summary>Adds a node to the client's tree (the next tree index), locked.</summary>
    public FakeClientSkillNode AddNode(uint id, SkillNodeType type = SkillNodeType.PassiveSkill, int cost = 1, int maxLevel = 0,
        float uiX = 0, float uiY = 0, int classAlignment = 3, bool shadow = false)
    {
        if (cost is < 0 or > ushort.MaxValue) throw new ArgumentOutOfRangeException(nameof(cost));
        if (maxLevel is < 0 or > 7) throw new ArgumentOutOfRangeException(nameof(maxLevel), maxLevel, "0 (one unlock) to 7 levels");
        lock (S.Gate)
        {
            if (S.Nodes.Any(n => n.Id == id)) throw new ArgumentException($"node 0x{id:x8} is in the tree already", nameof(id));
            var node = new FakeClientSkillNode(id) { Type = type, Cost = cost, MaxLevel = maxLevel, UiX = uiX, UiY = uiY, ClassAlignment = classAlignment, Shadow = shadow };
            S.Nodes.Add(node);
            return node;
        }
    }

    /// <summary>Links two nodes of the tree; the link's id is returned.</summary>
    public uint Link(uint from, uint to, bool bidirectional = false)
    {
        lock (S.Gate)
        {
            var id = (uint)S.Links.Count + 1;
            S.Links.Add((id, from, to, bidirectional));
            return id;
        }
    }

    /// <summary>The player has the node at <paramref name="level"/> (1 for a plain node).</summary>
    public void Unlock(uint id, int level = 1)
    {
        lock (S.Gate)
        {
            var n = S.Nodes.FirstOrDefault(x => x.Id == id) ?? throw new ArgumentException($"node 0x{id:x8} is not in the tree", nameof(id));
            if (level < 1 || level > Math.Max(1, n.MaxLevel)) throw new ArgumentOutOfRangeException(nameof(level));
            n.Level = level;
        }
    }

    /// <summary>The player does not have the node.</summary>
    public void Lock(uint id)
    {
        lock (S.Gate)
            if (S.Nodes.FirstOrDefault(x => x.Id == id) is { } n) n.Level = 0;
    }

    /// <summary>Empties the tree (a world change).</summary>
    public void Clear()
    {
        lock (S.Gate)
        {
            S.Nodes.Clear();
            S.Links.Clear();
        }
    }
}

/// <summary>One node of the client's tree in a harness (<see cref="HarnessClientSkills.AddNode"/>). Since 0.2.0-dev.31.13.</summary>
public sealed class FakeClientSkillNode
{
    internal FakeClientSkillNode(uint id) => Id = id;

    /// <summary>The node's id.</summary>
    public uint Id { get; }

    /// <summary>Its type.</summary>
    public SkillNodeType Type { get; set; }

    /// <summary>Its class (0 Warrior, 1 Mage, 2 Ranger, 3 Neutral).</summary>
    public int ClassAlignment { get; set; }

    /// <summary>Its cost per level.</summary>
    public int Cost { get; set; }

    /// <summary>0: one unlock.</summary>
    public int MaxLevel { get; set; }

    /// <summary>The player's level: 0 locked.</summary>
    public int Level { get; set; }

    /// <summary>A shadow node.</summary>
    public bool Shadow { get; set; }

    /// <summary>Its x position on the skill screen.</summary>
    public float UiX { get; set; }

    /// <summary>Its y position on the skill screen.</summary>
    public float UiY { get; set; }
}
