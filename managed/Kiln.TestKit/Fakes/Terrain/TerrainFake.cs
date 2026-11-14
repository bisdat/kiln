// SPDX-License-Identifier: MIT
// The kit's fake behind Kiln.Terrain. Kiln.Terrain has no native table: it reaches the world through one internal seam
// (Terrain.BackendFactory), and this fake puts an in-memory voxel world of the running harness behind it. So the
// library's own code runs offline: lowering, batches, the ledger's rules, conflicts, events.
namespace Kiln.TestKit;

/// <summary>One harness's terrain: sparse cells over flat ground, bases, every mod's ledger book, and what was asked.</summary>
internal sealed class TerrainFakeState
{
    public readonly object Lock = new();
    public int GroundY = 100;
    public byte GroundMaterial = 1;
    public string WorldId = "fake-world";
    public bool Loaded = true;
    public readonly Dictionary<(int, int, int), Voxel> Cells = new();
    public readonly List<CellBox> Bases = [];
    public readonly Dictionary<string, TerrainBook> Books = new(StringComparer.Ordinal);
    /// <summary>The world file's revision: a save adds one (HarnessTerrain.SaveWorld).</summary>
    public uint Revision = 1;
    /// <summary>Kiln's witness files (kiln/data/kiln.terrain/&lt;world&gt;/&lt;mod&gt;.json), by "world|mod", as JSON.</summary>
    public readonly Dictionary<string, string> Witnesses = new(StringComparer.Ordinal);
    /// <summary>Worlds whose Kiln data is lost (Kiln.Saves reports DataMatch.None there).</summary>
    public readonly HashSet<string> DataLost = new(StringComparer.Ordinal);
    public readonly List<Action<string, uint>> AfterSave = [];
    /// <summary>kiln.build's transient journal: per cell a transient write changed, its value before the first such write
    /// and after the newest. A kept write takes its cells out; HarnessTerrain.Restart puts the rest back.</summary>
    public readonly Dictionary<(int, int, int), (Voxel Before, Voxel After)> Transient = new();

    /// <summary>The next start: every journaled cell that still holds what the transient write left gets its earlier value
    /// back; a cell changed since keeps the change. The journal empties.</summary>
    public int PutBackTransient()
    {
        var n = 0;
        foreach (var ((x, y, z), (before, after)) in Transient)
        {
            if (Get(x, y, z) != after) continue;
            Cells[(x, y, z)] = before;
            n++;
        }
        Transient.Clear();
        return n;
    }
    public readonly List<FakeTerrainWrite> Writes = [];
    /// <summary>Runs inside each write where kiln.build posts BuildEditingEvent: after the overlay read and decided, before
    /// the guard compares and the write commits (<see cref="HarnessTerrain.Editing"/>).</summary>
    public Action<CellBox>? Editing;

    public Voxel Get(int x, int y, int z) =>
        Cells.TryGetValue((x, y, z), out var v) ? v : y < GroundY ? Voxel.Of(GroundMaterial) : Voxel.Air;
}

internal sealed class TerrainFake : IKitFake
{
    /// <summary>Not a native service: the name of the library's own row, which is what a test switches off.</summary>
    public string Service => "kiln.terrain";

    public void Register() { }

    public void Start(KilnHarness harness)
    {
        var state = KitFakes.State<TerrainFakeState>(harness);
        Terrain.ResetSession();
        Terrain.CapabilityOverride = row =>
        {
            if (FakeTables.Off(row)) throw new KilnUnavailableException(row, "switched off by the test");
        };
        Terrain.BackendFactory = () =>
        {
            var mod = HostAccess.Current;
            if (mod is not null) Terrain.Watch(mod);
            return new Backend(harness, state, mod);
        };
    }

    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness)
    {
        Terrain.BackendFactory = null;
        Terrain.CapabilityOverride = null;
        Terrain.ResetSession();
    }

    public void AddCoverage(List<CoverageRow> rows) =>
        rows.Add(new CoverageRow("Terrain", CoverageKind.RecordingFake, ["kiln.terrain", "terrain.overlays", "terrain.baked", "terrain.biomes"],
            "Kiln.Terrain's own code on an in-memory world (KilnHarness.Terrain): flat ground, the cells a test sets, base boxes, one ledger per mod and " +
            "world id, and Kiln's witness of the kept plans with the world file's revision (SaveWorld, LoseLedger: a lost ledger is refused, a first install " +
            "over matching ground is not). Inspect, Apply, Forget and Applied run as in the game: batches, the ledger's rules, conflicts, Terrain.Changed. " +
            "Not modelled: streaming (tiles are loaded unless the test says not), the game's save itself (the ledger lives in the harness, not in the fake " +
            "kiln.saves), kiln.build's own events (its transient journal is modelled: Restart puts transient cells back), " +
            "and time: from code that is not a mod's, every batch runs in the same step. terrain.baked and terrain.biomes are off, as in the game."));

    private sealed class Backend(KilnHarness harness, TerrainFakeState s, ModContext? mod) : ITerrainBackend
    {
        public string Mod => mod?.Info.Id ?? "kiln";
        private readonly long _session = Lifecycle.ServerSession;
        public bool WorldRunning => (mod?.Alive ?? true) && _session == Lifecycle.ServerSession && Lifecycle.Phase is not (ServerPhase.Stopping or ServerPhase.Idle);
        public string? WorldId { get { lock (s.Lock) return s.WorldId; } }
        public (int X, int Z)? GridCells => (20480, 20480);
        public string KilnVersion => "testkit";

        public async Task NextTick()
        {
            // as a mod: the mod's next tick, so one batch a step; as Kiln (KilnHarness.Run) there is no scheduler to wait on
            if (mod is not null) await mod.Scheduler.NextTick();
        }

        public Task<bool> Load(CellBox box, TimeSpan timeout) { lock (s.Lock) return Task.FromResult(s.Loaded); }

        public bool TryRead(CellBox box, out Voxel[] cells)
        {
            lock (s.Lock)
            {
                cells = new Voxel[box.Count];
                for (var z = 0; z < box.SizeZ; z++)
                for (var y = 0; y < box.SizeY; y++)
                for (var x = 0; x < box.SizeX; x++)
                    cells[box.Index(x, y, z)] = s.Get(box.X + x, box.Y + y, box.Z + z);
                return s.Loaded;
            }
        }

        public Task<BuildResult> Write(CellBox box, Voxel[] cells, bool[] mask, bool inBases, bool transient, string label, TimeSpan timeout, bool dryRun = false,
            Voxel[]? expected = null)
        {
            lock (s.Lock)
            {
                s.Editing?.Invoke(box);
                // the guard, as Build.WriteIfUnchanged: every masked cell must still be what the overlay read
                if (expected is not null)
                    for (var z = 0; z < box.SizeZ; z++)
                    for (var y = 0; y < box.SizeY; y++)
                    for (var x = 0; x < box.SizeX; x++)
                    {
                        var i = box.Index(x, y, z);
                        if (mask[i] && s.Get(box.X + x, box.Y + y, box.Z + z) != expected[i])
                            return Task.FromResult(new BuildResult(0, 0, false, false, box, 0, BuildRefusal.Changed));
                    }
                if (!inBases && s.Bases.Any(b => !TerrainPlan.Intersect(b, box).IsEmpty))
                    return Task.FromResult(new BuildResult(0, 0, false, false, box, 0, BuildRefusal.Base));
                if (dryRun) return Task.FromResult(new BuildResult(0, 0, false, false, box, 0, BuildRefusal.DryRun));
                long changed = 0;
                for (var z = 0; z < box.SizeZ; z++)
                for (var y = 0; y < box.SizeY; y++)
                for (var x = 0; x < box.SizeX; x++)
                {
                    var i = box.Index(x, y, z);
                    if (!mask[i]) continue;
                    var at = (box.X + x, box.Y + y, box.Z + z);
                    var old = s.Get(at.Item1, at.Item2, at.Item3);
                    if (old != cells[i]) changed++;
                    s.Cells[at] = cells[i];
                    // kiln.build's transient journal, as in the game (native/services/build/transient.cpp)
                    if (!transient) s.Transient.Remove(at);
                    else if (s.Transient.TryGetValue(at, out var j)) s.Transient[at] = (j.Before, cells[i]);
                    else if (old != cells[i]) s.Transient[at] = (old, cells[i]);
                    if (transient && s.Transient.TryGetValue(at, out var k) && k.Before == k.After) s.Transient.Remove(at);
                }
                s.Writes.Add(new FakeTerrainWrite(Mod, label, box, changed, transient, harness.CurrentStep));
                return Task.FromResult(new BuildResult(changed, 0, true, !transient, box, 0));
            }
        }

        public TerrainBook Book()
        {
            lock (s.Lock)
            {
                var key = s.WorldId + "|" + Mod;
                if (!s.Books.TryGetValue(key, out var b)) s.Books[key] = b = new TerrainBook();
                return b;
            }
        }

        public void Save(TerrainBook book) { lock (s.Lock) s.Books[s.WorldId + "|" + Mod] = book; }

        // Kiln's witness of the kept plans (TerrainLoss), as in the game: per world and mod, outside the ledger
        public DataMatch LoadedData { get { lock (s.Lock) return s.DataLost.Contains(s.WorldId) ? DataMatch.None : DataMatch.Exact; } }
        public (string? Id, uint Revision) SaveIdentity { get { lock (s.Lock) return (s.WorldId, s.Revision); } }
        public string WitnessLocation(string saveId) => $"kiln/data/kiln.terrain/{saveId}/{Mod}.json";

        public TerrainWitness? ReadWitness(string saveId)
        {
            string? json;
            lock (s.Lock) s.Witnesses.TryGetValue(saveId + "|" + Mod, out json);
            if (json is null) return null;
            try
            {
                var w = System.Text.Json.JsonSerializer.Deserialize<TerrainWitness>(json) ?? throw new InvalidDataException("empty");
                w.Validate();
                return w;
            }
            catch (System.Text.Json.JsonException e) { throw new InvalidDataException(e.Message, e); }
        }

        public void WriteWitness(string saveId, TerrainWitness witness)
        {
            var json = System.Text.Json.JsonSerializer.Serialize(witness);
            lock (s.Lock) s.Witnesses[saveId + "|" + Mod] = json;
        }

        public void AfterNextSave(Action<string, uint> action) { lock (s.Lock) s.AfterSave.Add(action); }
    }
}
