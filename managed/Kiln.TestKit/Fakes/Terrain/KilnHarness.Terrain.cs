// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessTerrain? _terrain;

    /// <summary>The fake world behind Kiln.Terrain in this harness: its cells, bases and what overlays wrote.</summary>
    public HarnessTerrain Terrain => _terrain ??= new HarnessTerrain(this);
}

/// <summary>One write an overlay made (a batch, or a single block when a base was in the way).</summary>
/// <param name="Mod">The mod that applied the patch ("kiln" from code that is not a mod's).</param>
/// <param name="Label">"terrain &lt;plan&gt; v&lt;version&gt;".</param>
/// <param name="Step">The harness step it was made in.</param>
public readonly record struct FakeTerrainWrite(string Mod, string Label, CellBox Box, long CellsChanged, bool Transient, ulong Step);

/// <summary>
/// The fake world of a harness behind Kiln.Terrain (<see cref="KilnHarness.Terrain"/>): <c>Terrain.Inspect</c>,
/// <c>Apply</c>, <c>Forget</c> and <c>Applied</c> run their own code on it.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> Flat ground (solid below <see cref="GroundY"/>), the cells a test or an overlay sets, base
/// boxes an overlay must leave alone, one ledger per mod and <see cref="WorldId"/>, and the capability row
/// <c>terrain.overlays</c>: after <c>Capabilities.Override("terrain.overlays", false)</c> the members throw
/// <c>KilnUnavailableException</c> naming it (the row itself, which Kiln.Terrain reports, may still read available).</para>
/// <para><b>The ledger and its loss.</b> Kiln keeps a witness of every mod's kept plans per world (as in the game,
/// outside the ledger). <see cref="SaveWorld"/> is the game writing the world file; <see cref="LoseLedger"/> loses a
/// mod's ledger while the cells and the witness stay. A plan whose record was saved and then lost is refused
/// ("ownership lost"). A pristine harness session permits first installation, including over matching ground.
/// Restart retires that new-world proof; missing history then needs adoption unless the ledger preserves its provenance.</para>
/// <para><b>Transient overlays.</b> kiln.build's transient journal is modelled: <see cref="Restart"/> puts every cell
/// a transient write changed back to its earlier value unless it was changed since (as the game's next start does).</para>
/// <para><b>Not modelled.</b> Streaming, the game's save itself (a new harness starts with an empty world and no
/// ledger), kiln.build's own events and limits. From code that is not a mod's (<c>KilnHarness.Run</c>) every batch runs in the
/// same step; as a mod (<c>KilnHarness.As</c>, a mod's own code) one batch a step.</para>
/// </remarks>
public sealed class HarnessTerrain
{
    private readonly KilnHarness _harness;

    internal HarnessTerrain(KilnHarness harness) => _harness = harness;

    private TerrainFakeState State => KitFakes.State<TerrainFakeState>(_harness);

    /// <summary>Cells below this height are solid ground, cells from it up are air, where nothing was set. Default 100.</summary>
    public int GroundY
    {
        get { lock (State.Lock) return State.GroundY; }
        set { lock (State.Lock) State.GroundY = value; }
    }

    /// <summary>The world's id: a ledger belongs to one world. Default "fake-world". Changing it does not change the cells.</summary>
    /// <exception cref="ArgumentException">An empty id.</exception>
    public string WorldId
    {
        get { lock (State.Lock) return State.WorldId; }
        set
        {
            ArgumentException.ThrowIfNullOrEmpty(value);
            lock (State.Lock) State.WorldId = value;
        }
    }

    /// <summary>Whether the voxel tiles are there. False: an overlay stops with "did not load". Default true.</summary>
    public bool Loaded
    {
        get { lock (State.Lock) return State.Loaded; }
        set { lock (State.Lock) State.Loaded = value; }
    }

    /// <summary>The cell at <paramref name="at"/>.</summary>
    public Voxel Get(CellPos at) { lock (State.Lock) return State.Get(at.X, at.Y, at.Z); }

    /// <summary>Sets one cell, as a player's edit would (no ledger knows of it).</summary>
    public void Set(CellPos at, Voxel cell) { lock (State.Lock) State.Cells[(at.X, at.Y, at.Z)] = cell; }

    /// <summary>A player base: overlays leave blocks that touch <paramref name="box"/> alone unless bases are allowed.</summary>
    /// <exception cref="ArgumentException">An empty box.</exception>
    public void AddBase(CellBox box)
    {
        if (box.IsEmpty) throw new ArgumentException("an empty box is no base", nameof(box));
        lock (State.Lock) State.Bases.Add(box);
    }

    /// <summary>Runs inside every write of an overlay where kiln.build posts its <c>BuildEditingEvent</c>: after the
    /// overlay read the cells and decided, before kiln.build's guard compares them and the write commits. A test makes
    /// another mod's edit here (<see cref="Set"/>); the overlay then leaves that block alone. Null: nothing runs.</summary>
    /// <remarks>It gets the box of the write: a whole batch, then each of its blocks when the batch is written block by
    /// block (a base, or an edit made here). Since 0.2.0-dev.28.</remarks>
    public Action<CellBox>? Editing
    {
        get { lock (State.Lock) return State.Editing; }
        set { lock (State.Lock) State.Editing = value; }
    }

    /// <summary>The world file's revision; <see cref="SaveWorld"/> adds one. Starts at 1. Since 0.2.0-dev.28.</summary>
    public uint Revision { get { lock (State.Lock) return State.Revision; } }

    /// <summary>The game writes the world file: the ledgers as they are now are in that save (a record made before it
    /// and lost after it is a lost ledger). Since 0.2.0-dev.28.</summary>
    public void SaveWorld()
    {
        List<Action<string, uint>> waiting;
        string world;
        uint revision;
        lock (State.Lock)
        {
            revision = ++State.Revision;
            world = State.WorldId;
            waiting = [.. State.AfterSave];
            State.AfterSave.Clear();
        }
        foreach (var a in waiting) a(world, revision);
    }

    /// <summary>The server saves the world and starts again (<see cref="SaveWorld"/>, then the next start). As in the
    /// game, every cell a transient write changed (<c>TerrainPersistence.Transient</c>, <c>BuildFlags.Transient</c>) and
    /// that still holds what that write left gets its earlier value back; a cell changed since (<see cref="Set"/>, a kept
    /// write) keeps the change. Kept cells and the ledgers stay. Since 0.2.0-dev.28.</summary>
    public void Restart()
    {
        SaveWorld();
        lock (State.Lock)
        {
            State.RestartedWorlds.Add(State.WorldId);
            State.PutBackTransient();
        }
    }

    /// <summary>Loses <paramref name="mod"/>'s terrain ledger in this world, as when the world's Kiln data or the
    /// ledger key is lost; the cells and Kiln's witness stay. With <paramref name="wholeWorldData"/> the world has no Kiln
    /// data at all (<c>WorldData.Loaded</c> would say <c>DataMatch.None</c>; the refusal says so). False when the mod
    /// had no ledger here. Since 0.2.0-dev.28.</summary>
    /// <exception cref="ArgumentException">An empty mod id.</exception>
    public bool LoseLedger(string mod, bool wholeWorldData = false)
    {
        ArgumentException.ThrowIfNullOrEmpty(mod);
        lock (State.Lock)
        {
            if (wholeWorldData) State.DataLost.Add(State.WorldId);
            return State.Books.Remove(State.WorldId + "|" + mod);
        }
    }

    /// <summary>The plans Kiln's witness has for <paramref name="mod"/> in this world, sorted. Since 0.2.0-dev.28.</summary>
    public IReadOnlyList<string> Witnessed(string mod)
    {
        lock (State.Lock)
        {
            if (!State.Witnesses.TryGetValue(State.WorldId + "|" + mod, out var json)) return [];
            var w = System.Text.Json.JsonSerializer.Deserialize<TerrainWitness>(json);
            return w?.Plans.Keys.OrderBy(k => k, StringComparer.Ordinal).ToList() ?? [];
        }
    }

    /// <summary>Every write overlays made, in order.</summary>
    public IReadOnlyList<FakeTerrainWrite> Writes { get { lock (State.Lock) return [.. State.Writes]; } }
}
