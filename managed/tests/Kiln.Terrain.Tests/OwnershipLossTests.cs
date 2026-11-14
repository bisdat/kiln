// SPDX-License-Identifier: MIT
// Ownership-loss detection (TerrainLoss, docs/kiln/services/terrain.md "When the ledger is gone"): Kiln keeps a witness
// of every kept plan a mod has a ledger record of, per world save, outside the world's Kiln data. A kept plan with no
// record whose witness entry is older than the world file is a lost ledger: refused, nothing written. With no entry it
// is a first install, also over ground that already holds the plan's result. The loss is made in the in-memory world as
// it happens in a game: the mod's book (the ledger key) is gone, and for a lost sidecar Kiln.Saves reports no data for
// the world (DataMatch.None); the cells and the witness stay. MemoryWorld.SaveWorld is the game writing the world file.
using Kiln;
using Xunit;

namespace TerrainTests;

public sealed class OwnershipLossTests
{
    private const byte Ore = 12;   // MemoryWorld's ground is stone (1) below y 100, air from it up
    private const string Mod = "mod-a";

    private sealed class Checked(MemoryWorld world)
    {
        public readonly MemoryWorld World = world;
        public readonly TerrainSession Session = new();
        public readonly List<TerrainChangedEvent> Events = [];

        public TerrainResult Apply(TerrainPatch patch, string mod = Mod)
        {
            var o = new TerrainApplyOptions();
            return Done(new TerrainEngine(World.For(mod), Session, Events.Add).Apply(TerrainPlan.Lower(patch), o, TerrainPlan.BatchEdge(o.CellsPerTick)));
        }

        public TerrainPlanReport Inspect(TerrainPatch patch)
        {
            var o = new TerrainApplyOptions();
            return Done(new TerrainEngine(World.For(Mod), Session, null).Inspect(TerrainPlan.Lower(patch), o, TerrainPlan.BatchEdge(o.CellsPerTick)));
        }

        public TerrainEngine Engine => new(World.For(Mod), Session, null);

        private static T Done<T>(Task<T> t)
        {
            Assert.True(t.IsCompleted, "the in-memory world never waits");
            return t.GetAwaiter().GetResult();
        }
    }

    private static readonly CellBox Two = new(64, 96, 64, 16, 8, 8);   // two blocks, half in the ground

    private static TerrainPatch Box(string id = "box", int version = 1, CellBox? region = null, TerrainPersistence persistence = TerrainPersistence.Kept)
    {
        var r = region ?? Two;
        var cells = new byte[r.Count];
        Array.Fill(cells, (byte)1);
        return new TerrainPatch(id, version, r, new TerrainVoxelVolume(cells), [new TerrainPaletteEntry(Ore)], persistence);
    }

    private static void Lose(MemoryWorld world, bool wholeSidecar)
    {
        Assert.True(world.Books.Remove(world.WorldId + "|" + Mod));
        if (wholeSidecar) world.Match = DataMatch.None;
    }

    private static TerrainBook BookOf(MemoryWorld world, string worldId) => world.Books[worldId + "|" + Mod];

    private static List<KeyValuePair<(int, int, int), Voxel>> Edits(MemoryWorld world) => world.Edits.OrderBy(e => e.Key).ToList();

    /// <summary>The ground of <see cref="Two"/> set to the box's result by hand (a player's build, a natural vein).</summary>
    private static void GroundHoldsTheResult(MemoryWorld world)
    {
        for (var z = 0; z < Two.SizeZ; z++)
        for (var y = 0; y < Two.SizeY; y++)
        for (var x = 0; x < Two.SizeX; x++)
            world.Set(Two.X + x, Two.Y + y, Two.Z + z, Voxel.Of(Ore));
    }

    [Fact]
    public void A_failed_witness_write_refuses_the_plan_before_changing_ground()
    {
        var world = new MemoryWorld { WitnessWriteError = new IOException("disk full") };
        var run = new Checked(world);
        var result = run.Apply(Box());
        Assert.Equal(TerrainState.Refused, result.State);
        Assert.Empty(world.Writes);
        Assert.Empty(world.Edits);
        world.WitnessWriteError = null;
        Assert.Equal(TerrainState.Completed, run.Apply(Box()).State);
        Assert.NotNull(world.Witness(Mod));
    }

    // ---- the ledger was saved, then lost: refused ----

    [Theory]
    [InlineData(true, false)]
    [InlineData(false, false)]
    [InlineData(true, true)]
    [InlineData(false, true)]
    public void A_lost_ledger_is_refused_and_nothing_is_written_whatever_the_ground_holds_now(bool wholeSidecar, bool everyBlockChanged)
    {
        var world = new MemoryWorld();
        Assert.Equal(TerrainState.Completed, new Checked(world).Apply(Box()).State);
        world.SaveWorld();
        if (everyBlockChanged)
        {
            world.Set(64, 97, 64, Voxel.Air);               // a player dug in both blocks since: no block shows the plan
            world.Set(72, 97, 64, Voxel.Air);
        }
        Lose(world, wholeSidecar);
        var before = Edits(world);
        var writes = world.Writes.Count;

        var run = new Checked(world);                       // the restart after the loss
        var r = run.Apply(Box());
        Assert.Equal((false, TerrainState.Refused, 0L, 0), (r.Ok, r.State, r.ChangedCells, r.Batches));
        Assert.StartsWith("ownership lost: this world had plan 'box' (recorded at world revision 7; the world file is at revision 8 now)", r.Reason);
        Assert.Contains(wholeSidecar ? "this world has no Kiln data that could be read" : "this mod's terrain ledger has no record of it", r.Reason);
        Assert.Equal(before, Edits(world));
        Assert.Equal(writes, world.Writes.Count);           // no write at all, not even a dry run
        Assert.Empty(run.Events);
        Assert.Empty(run.Engine.Applied());                 // nothing recorded: asking again is refused again
        Assert.Equal(TerrainState.Refused, run.Apply(Box()).State);
        Assert.NotNull(world.Witness(Mod)!.Plans["box"]);   // and the evidence stays

        var seen = run.Inspect(Box());
        Assert.Equal((true, TerrainState.Refused), (seen.Ok, seen.Would));
        Assert.StartsWith("ownership lost:", seen.Reason);
        Assert.Equal(writes, world.Writes.Count);
    }

    // ---- never had it: a first install, whatever the ground holds ----

    [Fact]
    public void A_first_install_over_ground_that_already_holds_the_result_is_accepted_and_recorded()
    {
        var world = new MemoryWorld { Match = DataMatch.None };   // no Kiln data either: a world new to Kiln
        GroundHoldsTheResult(world);
        var run = new Checked(world);
        Assert.Equal((true, TerrainState.Completed), (run.Inspect(Box()).Ok, run.Inspect(Box()).Would));
        var r = run.Apply(Box());
        Assert.Equal((true, TerrainState.Completed, 0L), (r.Ok, r.State, r.ChangedCells));
        Assert.Equal(("box", true, 2), (run.Engine.Applied()[0].Plan, run.Engine.Applied()[0].Complete, run.Engine.Applied()[0].Blocks));
        Assert.Equal(7u, world.Witness(Mod)!.Plans["box"].Since);
    }

    [Fact]
    public void A_record_that_never_reached_a_save_is_not_a_loss()
    {
        // the server stopped before its next save: the world file is the one the apply ran on, and has no record
        var world = new MemoryWorld();
        new Checked(world).Apply(Box());
        Lose(world, wholeSidecar: true);
        world.Set(64, 97, 64, Voxel.Air);
        var r = new Checked(world).Apply(Box());
        Assert.Equal((TerrainState.Completed, 1L), (r.State, r.ChangedCells));
        Assert.Equal(7u, world.Witness(Mod)!.Plans["box"].Since);
    }

    [Fact]
    public void An_older_world_file_put_back_and_played_on_is_a_first_install_and_renews_the_entry()
    {
        var world = new MemoryWorld();
        new Checked(world).Apply(Box());
        world.SaveWorld();                                  // revision 8 carries the record
        Lose(world, wholeSidecar: false);
        world.Revision = 5;                                 // an older world file (and its Kiln data, which never had the plan)
        var r = new Checked(world).Apply(Box());
        Assert.Equal(TerrainState.Completed, r.State);
        Assert.Equal(5u, world.Witness(Mod)!.Plans["box"].Since);
        world.SaveWorld();
        Lose(world, wholeSidecar: false);                   // lost again after a save of this line: refused again
        Assert.Equal(TerrainState.Refused, new Checked(world).Apply(Box()).State);
    }

    [Fact]
    public void Forget_takes_the_entry_away_so_the_next_apply_is_a_first_install_even_without_the_tombstone()
    {
        var world = new MemoryWorld();
        var run = new Checked(world);
        run.Apply(Box());
        world.SaveWorld();
        Assert.True(run.Engine.Forget("box"));
        Assert.False(world.Witness(Mod)!.Plans.ContainsKey("box"));
        world.SaveWorld();
        Lose(world, wholeSidecar: true);                    // the tombstone goes with the ledger
        world.Set(64, 97, 64, Voxel.Air);
        var r = new Checked(world).Apply(Box());
        Assert.Equal((TerrainState.Completed, 1L), (r.State, r.ChangedCells));
    }

    [Fact]
    public void After_Forget_an_apply_is_a_first_install_and_uses_the_tombstone_up()
    {
        var world = new MemoryWorld();
        var run = new Checked(world);
        Assert.Equal(TerrainState.Completed, run.Apply(Box()).State);
        world.SaveWorld();
        Assert.True(run.Engine.Forget("box"));
        Assert.True(BookOf(world, "world-a").WasForgotten("box"));
        var again = new Checked(world);
        Assert.Equal((true, TerrainState.Completed), (again.Inspect(Box()).Ok, again.Inspect(Box()).Would));
        Assert.True(BookOf(world, "world-a").WasForgotten("box"));   // Inspect looks and leaves it
        var r = again.Apply(Box());
        Assert.Equal((true, TerrainState.Completed, 0L), (r.Ok, r.State, r.ChangedCells));
        Assert.Null(BookOf(world, "world-a").Forgotten);
        world.SaveWorld();
        Assert.True(BookOf(world, "world-a").Plans.Remove("box"));   // its record lost now, after a save: a lost ledger
        Assert.Equal(TerrainState.Refused, new Checked(world).Apply(Box()).State);
    }

    // ---- where the witness comes from ----

    [Fact]
    public void A_new_world_gets_its_witness_with_its_first_save()
    {
        var world = new MemoryWorld { SaveId = null, Revision = 0, Match = DataMatch.None };
        new Checked(world).Apply(Box());
        Assert.Empty(world.Witnesses);                      // no id yet
        world.SaveWorld();                                  // the first save: the world's id, revision 1
        Assert.Equal(0u, world.Witness(Mod)!.Plans["box"].Since);
        Lose(world, wholeSidecar: true);
        Assert.Equal(TerrainState.Refused, new Checked(world).Apply(Box()).State);
    }

    [Fact]
    public void A_ledger_without_a_witness_gets_one_at_the_next_apply()
    {
        // a world whose ledger is older than the witness (or whose witness file was lost)
        var world = new MemoryWorld();
        new Checked(world).Apply(Box());
        world.Witnesses.Clear();
        world.SaveWorld();
        Assert.Equal(TerrainState.AlreadyApplied, new Checked(world).Apply(Box()).State);
        Assert.Equal(8u, world.Witness(Mod)!.Plans["box"].Since);
        world.SaveWorld();
        Lose(world, wholeSidecar: false);
        Assert.Equal(TerrainState.Refused, new Checked(world).Apply(Box()).State);
    }

    [Fact]
    public void A_witness_that_cannot_be_read_refuses_a_first_install_and_is_never_replaced()
    {
        var world = new MemoryWorld();
        var run = new Checked(world);
        run.Apply(Box());
        var key = world.SaveId + "|" + Mod;
        world.Witnesses[key] = "{not json";
        world.SaveWorld();
        var other = Box("other", region: Two.Offset(new CellPos(64, 0, 0)));
        var r = run.Apply(other);
        Assert.Equal((TerrainState.Refused, 0L), (r.State, r.ChangedCells));
        Assert.StartsWith("ownership unknown:", r.Reason);
        Assert.Equal(TerrainState.AlreadyApplied, run.Apply(Box()).State);   // a plan with its record is not held up
        Assert.Equal("{not json", world.Witnesses[key]);
    }

    [Fact]
    public void Transient_patches_and_other_mods_are_not_witnessed_or_checked()
    {
        var world = new MemoryWorld();
        var run = new Checked(world);
        run.Apply(Box());
        run.Apply(Box("flash", persistence: TerrainPersistence.Transient, region: Two.Offset(new CellPos(64, 0, 0))));
        world.SaveWorld();
        Assert.Equal(["box"], world.Witness(Mod)!.Plans.Keys.ToList());
        Lose(world, wholeSidecar: false);
        // mod-b's own plan "box" elsewhere: mod-a's witness is not its
        var b = run.Apply(Box(region: Two.Offset(new CellPos(128, 0, 0))), "mod-b");
        Assert.Equal(TerrainState.Completed, b.State);
        Assert.Equal(TerrainState.Completed, run.Apply(Box("flash", persistence: TerrainPersistence.Transient, region: Two.Offset(new CellPos(64, 0, 0)))).State);
    }

    [Fact]
    public void The_refusal_says_what_was_lost_and_what_an_operator_can_do()
    {
        var text = TerrainLoss.Reason("vein", 7, 9, DataMatch.Exact, "kiln/data/kiln.terrain/x/mod-a.json");
        Assert.Contains("plan 'vein' (recorded at world revision 7; the world file is at revision 9 now)", text);
        Assert.Contains("Nothing was written", text);
        Assert.Contains("Saves.RestoreOnNextStart", text);
        Assert.Contains("remove 'vein' from kiln/data/kiln.terrain/x/mod-a.json", text);
        Assert.Contains("no Kiln data that could be read", TerrainLoss.Reason("vein", 7, 9, DataMatch.None, "w"));
    }

    [Fact]
    public void Tombstones_are_bounded_validated_and_optional()
    {
        var book = new TerrainBook();
        for (var i = 0; i <= TerrainBook.MaxForgotten; i++)
        {
            book.Plans[$"p{i}"] = new TerrainRecord();
            Assert.True(book.Forget($"p{i}"));
        }
        Assert.Equal(TerrainBook.MaxForgotten, book.Forgotten!.Count);
        Assert.False(book.WasForgotten("p0"));              // the oldest went
        Assert.True(book.WasForgotten($"p{TerrainBook.MaxForgotten}"));
        Assert.False(book.Forget("no-such-plan"));

        var world = new MemoryWorld();
        var patch = Box();
        new Checked(world).Apply(patch);
        var json = System.Text.Json.JsonSerializer.Serialize(BookOf(world, "world-a"));
        Assert.DoesNotContain("\"Forgotten\":{", json);    // none yet
        var old = System.Text.Json.Nodes.JsonNode.Parse(json)!.AsObject();
        old.Remove("Forgotten");                            // a ledger written before tombstones
        var read = System.Text.Json.JsonSerializer.Deserialize<TerrainBook>(old.ToJsonString())!;
        Assert.Null(read.Forgotten);
        read.Validate((20480, 20480));

        Action<TerrainBook>[] corrupt =
        [
            b => b.Forgotten = new() { ["Not An Id"] = DateTimeOffset.UtcNow },
            b => b.Forgotten = new() { ["box"] = DateTimeOffset.UtcNow },          // a forgotten plan with a record
            b => b.Forgotten = Enumerable.Range(0, TerrainBook.MaxForgotten + 1).ToDictionary(i => $"t{i}", _ => DateTimeOffset.UtcNow),
        ];
        foreach (var damage in corrupt)
        {
            var w = new MemoryWorld();
            var run = new Checked(w);
            run.Apply(patch);
            damage(BookOf(w, "world-a"));
            Assert.Equal(TerrainState.Refused, run.Apply(patch with { Version = 2 }).State);
            Assert.False(run.Engine.Forget("box"));
        }
    }

}
