using Kiln;
using Xunit;
using Kiln.TestKit;
namespace TerrainTests;
public sealed class AdoptionTests
{
    private static readonly CellBox Box = new(64, 96, 64, 8, 8, 8);
    private static TerrainPatch Patch => new("cloud", 1, Box, new TerrainVoxelVolume(Enumerable.Repeat((byte)1, 512).ToArray()), [new(12)]);
    [Fact]
    public async Task An_existing_save_without_history_never_refills_excavations()
    {
        var w = new MemoryWorld { Match = DataMatch.None, ProvenNewSession=false };
        for (var z=0;z<8;z++) for(var y=0;y<8;y++) for(var x=0;x<8;x++)
            w.Set(64+x,96+y,64+z,Voxel.Of(12));
        w.Set(64,96,64,Voxel.Air);
        w.ProvenNewSession=false;
        // Model a mostly intact overlay moved without any Kiln data.
        var b = new UnknownHistory(w.For("cloud-mod"));
        var e = new TerrainEngine(b, new TerrainSession(), null);
        var r = await e.Apply(TerrainPlan.Lower(Patch), new(), 8);
        Assert.False(r.Ok);
        Assert.Equal("NeedsAdoption", r.State.ToString());
        Assert.Empty(w.Writes);
        Assert.True(w.Get(64,96,64).IsAir);
    }
    [Theory]
    [InlineData(TerrainInstallChoice.Adopt)]
    [InlineData(TerrainInstallChoice.InstallAnyway)]
    public async Task Explicit_choices_are_distinct_and_adoption_cannot_restore(TerrainInstallChoice choice)
    {
        var w = new MemoryWorld { Match = DataMatch.None, ProvenNewSession=false };
        var e = new TerrainEngine(new UnknownHistory(w.For("cloud-mod")), new TerrainSession(), null);
        var r = await e.Apply(TerrainPlan.Lower(Patch), new() { InstallChoice = choice }, 8);
        Assert.True(r.Ok);
        if (choice == TerrainInstallChoice.Adopt)
        {
            Assert.Equal(TerrainState.Adopted, r.State);
            Assert.Empty(w.Writes);
            Assert.Contains("cannot be restored", r.Reason);
            Assert.True(e.Applied().Single().Adopted);
            var uninstall = e.Forget("cloud", true);
            Assert.False(uninstall.Ok);
            Assert.Contains("cannot be restored", uninstall.Reason);
            Assert.Single(e.Applied());
        }
        else { Assert.Equal(TerrainState.Completed, r.State); Assert.NotEmpty(w.Writes); }
        Assert.Equal(TerrainState.AlreadyApplied, (await e.Apply(TerrainPlan.Lower(Patch), new(), 8)).State);
    }

    [Fact]
    public async Task Proven_new_world_installs_and_retains_proof_after_its_first_save()
    {
        var w = new MemoryWorld { SaveId = null, Match = DataMatch.None };
        var b = w.For("cloud-mod");
        var e = new TerrainEngine(b, new TerrainSession(), null);
        Assert.Equal(TerrainState.Completed, (await e.Apply(TerrainPlan.Lower(Patch), new(), 8)).State);
        w.SaveWorld();
        Assert.True(b.Book().EstablishedInNewWorld);
        Assert.Equal(TerrainState.AlreadyApplied, (await e.Apply(TerrainPlan.Lower(Patch), new(), 8)).State);
    }

    [Fact]
    public void A_harness_restart_retires_new_world_proof()
    {
        using var h=new KilnHarness();
        h.Step(2);
        Assert.True(Terrain.BackendFactory!().NewWorldSession);
        h.Terrain.Restart();
        Assert.False(Terrain.BackendFactory!().NewWorldSession);
    }

    [Fact]
    public async Task Transient_adoption_is_refused_instead_of_silently_writing_cells()
    {
        var w=new MemoryWorld();
        var e=new TerrainEngine(w.For("m"),new TerrainSession(),null);
        var transient=TerrainPlan.Lower(Patch with {Persistence=TerrainPersistence.Transient});
        await Assert.ThrowsAsync<ArgumentException>(()=>e.Apply(transient,new(){InstallChoice=TerrainInstallChoice.Adopt},8));
        Assert.Empty(w.Writes);
    }

    [Fact]
    public async Task An_adopted_plan_can_upgrade_owned_cells_without_adopting_the_upgrade()
    {
        var w = new MemoryWorld();
        var e = new TerrainEngine(new UnknownHistory(w.For("m")), new TerrainSession(), null);
        await e.Apply(TerrainPlan.Lower(Patch), new() { InstallChoice=TerrainInstallChoice.Adopt }, 8);
        var upgraded = Patch with { Version=2, Palette=[new TerrainPaletteEntry(7)] };
        var r = await e.Apply(TerrainPlan.Lower(upgraded), new(), 8);
        Assert.Equal(TerrainState.Completed, r.State);
        Assert.NotEmpty(w.Writes);
        Assert.Equal(Voxel.Of(7),w.Get(64,96,64));
    }

    [Fact]
    public void Missing_world_stop_observation_makes_the_overlay_row_unavailable()
    {
        using var h = new KilnHarness();
        var prior = Terrain.BackendFactory;
        Terrain.BackendFactory = () => new UnknownHistory(new MemoryWorld().For("m"), "persistent terrain writes require world.lifecycle and a world-stop subscription");
        try
        {
            var ex = Assert.Throws<KilnUnavailableException>(() => h.Run(() => TerrainCaps.Require(TerrainCaps.Overlays)));
            Assert.Contains("world.lifecycle", ex.Message);
            Assert.False(h.Run(() => Capabilities.Is("terrain.overlays")));
        }
        finally { Terrain.BackendFactory = prior; }
    }

    private sealed class UnknownHistory(ITerrainBackend inner, string? failure = null) : ITerrainBackend
    {
        public string Mod => inner.Mod;
        public string? PersistenceFailure => failure;
        public bool WorldRunning => inner.WorldRunning;
        public string? WorldId => inner.WorldId;
        public (int X,int Z)? GridCells => inner.GridCells;
        public string KilnVersion => inner.KilnVersion;
        public DataMatch LoadedData => DataMatch.None;
        public (string? Id,uint Revision) SaveIdentity => inner.SaveIdentity;
        public Task NextTick() => inner.NextTick();
        public Task<bool> Load(CellBox b,TimeSpan t) => inner.Load(b,t);
        public bool TryRead(CellBox b,out Voxel[] cells) => inner.TryRead(b,out cells);
        public Task<BuildResult> Write(CellBox b,Voxel[] c,bool[] m,bool bases,bool transient,string label,TimeSpan t,bool dryRun=false,Voxel[]? expected=null) => inner.Write(b,c,m,bases,transient,label,t,dryRun,expected);
        public TerrainBook Book() => inner.Book();
        public void Save(TerrainBook b) => inner.Save(b);
    }
}
