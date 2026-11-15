// SPDX-License-Identifier: MIT
// Everything Kiln.Terrain asks of the world, behind one interface: the real one calls Kiln.Build, Kiln.Saves and
// Kiln.Spawn as the calling mod; tests and the test kit's fake put an in-memory world behind it.
namespace Kiln;

internal interface ITerrainBackend
{
    /// <summary>The calling mod's id.</summary>
    string Mod { get; }
    /// <summary>The world runs (its save is loaded, so the ledger is this world's).</summary>
    bool WorldRunning { get; }
    string? WorldId { get; }
    /// <summary>Grid size in cells (x, z), or null while unknown.</summary>
    (int X, int Z)? GridCells { get; }
    string KilnVersion { get; }
    DataMatch LoadedData => DataMatch.Exact;
    Task NextTick();
    Task<bool> Load(CellBox box, TimeSpan timeout);
    bool TryRead(CellBox box, out Voxel[] cells);
    /// <summary>One write of <paramref name="cells"/> where <paramref name="mask"/> is set. With
    /// <paramref name="expected"/> (the cells the overlay read) it is a guarded write: it commits only if the world still
    /// holds them where <paramref name="mask"/> is set, compared after kiln.build's <see cref="BuildEditingEvent"/>
    /// handlers and right before the commit; otherwise nothing is written and the refusal is
    /// <see cref="BuildRefusal.Changed"/>.</summary>
    Task<BuildResult> Write(CellBox box, Voxel[] cells, bool[] mask, bool inBases, bool transient, string label, TimeSpan timeout, bool dryRun = false,
        Voxel[]? expected = null);
    /// <summary>The mod's book in this world: a live object, saved with the world. <see cref="Save"/> stores it after a
    /// plan was added or removed.</summary>
    TerrainBook Book();
    void Save(TerrainBook book);

    /// <summary>The world save's lasting id (its GUID text) and the world file's newest revision (loaded or written).
    /// Id null: a new world before its first save.</summary>
    (string? Id, uint Revision) SaveIdentity => (null, 0);
    /// <summary>The mod's ownership witness for world save <paramref name="saveId"/> (TerrainLoss), kept outside the
    /// world's Kiln data; null when there is none.</summary>
    /// <exception cref="InvalidDataException">It exists and cannot be read.</exception>
    TerrainWitness? ReadWitness(string saveId) => null;
    void WriteWitness(string saveId, TerrainWitness witness) { }
    /// <summary>Where the witness is, for messages.</summary>
    string WitnessLocation(string saveId) => "the terrain witness";
    /// <summary>Runs <paramref name="action"/> once, with the world's id and the new revision, after the next world file
    /// was written.</summary>
    void AfterNextSave(Action<string, uint> action) { }
}

internal sealed class BuildTerrainBackend(ModContext mod) : ITerrainBackend
{
    public string Mod => mod.Info.Id;
    private readonly long _session = Lifecycle.ServerSession;
    public bool WorldRunning => mod.Alive && _session == Lifecycle.ServerSession && Lifecycle.Phase is not (ServerPhase.Stopping or ServerPhase.Idle) &&
        (Spawn.Available ? Spawn.WorldRunning : Build.Grid is not null);
    public DataMatch LoadedData => WorldData.Loaded;
    // A new world's GUID can be assigned by its first save. Its process session is stable across that save,
    // and WorldRunning invalidates this backend when the session ends. WorldData selects the persistent GUID.
    public string? WorldId => $"session:{_session}";
    public (int X, int Z)? GridCells => Build.Grid is { } g ? (g.CellsX, g.CellsZ) : null;
    public string KilnVersion => Game.KilnVersion;
    public async Task NextTick() => await mod.Scheduler.NextTick();
    public Task<bool> Load(CellBox box, TimeSpan timeout) => Build.Load(box, timeout);
    public bool TryRead(CellBox box, out Voxel[] cells) => Build.TryRead(box, out cells);

    public async Task<BuildResult> Write(CellBox box, Voxel[] cells, bool[] mask, bool inBases, bool transient, string label, TimeSpan timeout, bool dryRun = false,
        Voxel[]? expected = null)
    {
        var flags = BuildFlags.None;
        if (inBases) flags |= BuildFlags.InBases;
        if (transient) flags |= BuildFlags.Transient;
        if (dryRun) flags |= BuildFlags.DryRun;
        // no journal: an overlay is not a session edit to undo, and its cells would sit in the journal twice
        if (!await Build.Load(box, timeout) || !WorldRunning)
            return new BuildResult(0, 0, false, false, box, 0, BuildRefusal.NotLoaded);
        var options = new BuildOptions { Flags = flags, Journal = false, Label = label, LoadTimeout = timeout };
        return expected is null ? Build.Write(box, cells, options, mask) : Build.WriteIfUnchanged(box, cells, expected, options, mask);
    }

    public TerrainBook Book()
    {
        if (!WorldData.Contains(TerrainBook.Key)) return WorldData.Get<TerrainBook>(TerrainBook.Key);
        if (!WorldData.TryGet<TerrainBook>(TerrainBook.Key, out var book))
            throw new InvalidDataException("terrain ledger is null; stored data was kept");
        return book;
    }
    public void Save(TerrainBook book) => WorldData.Set(TerrainBook.Key, book);

    public (string? Id, uint Revision) SaveIdentity => (Saves.WorldId, Saves.Status?.Revision ?? 0);

    // kiln/data/kiln.terrain/<world GUID>/<mod>.json: beside kiln/data/worlds/<GUID>/ (where the ledger is), not in it
    private string WitnessPath(string saveId)
    {
        if (!Guid.TryParse(saveId, out var guid)) throw new InvalidDataException($"'{saveId}' is not a world id");
        var data = Path.GetDirectoryName(Path.GetFullPath(mod.DataDirectory)) ?? throw new IOException("no Kiln data folder");
        return Path.Combine(data, "kiln.terrain", guid.ToString("D"), mod.Info.Id + ".json");
    }

    public string WitnessLocation(string saveId)
    {
        try { return WitnessPath(saveId); }
        catch (Exception e) when (e is IOException or InvalidDataException) { return "the terrain witness"; }
    }

    public TerrainWitness? ReadWitness(string saveId)
    {
        var path = WitnessPath(saveId);
        if (!File.Exists(path)) return null;
        try
        {
            var w = System.Text.Json.JsonSerializer.Deserialize<TerrainWitness>(File.ReadAllBytes(path)) ?? throw new InvalidDataException("empty");
            w.Validate();
            return w;
        }
        catch (Exception e) when (e is System.Text.Json.JsonException or IOException or UnauthorizedAccessException or NotSupportedException)
        {
            throw new InvalidDataException(e.Message, e);
        }
    }

    public void WriteWitness(string saveId, TerrainWitness witness)
    {
        var path = WitnessPath(saveId);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var tmp = path + ".tmp";
        using (var f = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
        {
            System.Text.Json.JsonSerializer.Serialize(f, witness, new System.Text.Json.JsonSerializerOptions { WriteIndented = true });
            f.Flush(true);
        }
        File.Move(tmp, path, overwrite: true);
    }

    public void AfterNextSave(Action<string, uint> action)
    {
        EventSubscription? sub = null;
        sub = Events.On<WorldSavedEvent>(e =>
        {
            if (!e.Success || e.WorldId is null || _session != Lifecycle.ServerSession) return;
            sub?.Dispose();
            action(e.WorldId, e.Revision);
        });
    }
}
