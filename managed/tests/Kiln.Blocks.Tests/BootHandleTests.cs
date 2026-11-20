// SPDX-License-Identifier: MIT
// The boot generation's handle map (chapter 08 §4.2, fix round 08-onboot.handles): a committed boot definition is found by Blocks.Find and
// by a block's reference from the commit on, before the mod's runtime activation and after its owner reset; a runtime Define under its name
// is a reference when it is the same definition and refused (RestartRequiredBoot) when it is another one.
using System.Text.Json.Nodes;
using AffixBlockFace;
using Xunit;

namespace Kiln.Blocks.Tests;

[CollectionDefinition("boot generation", DisableParallelization = true)]
public sealed class BootGenerationCollection;

[Collection("boot generation")]  // BootGeneration is process-wide
public sealed class BootHandleTests : IDisposable
{
    private const string Mod = "boot-handles";

    public void Dispose()
    {
        BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        TheBlocks.ModFor(Mod, () => 1).Reset();
    }

    private static List<BootStaged> Staged()
    {
        var f = new BootContext(Mod, 1, BootRoles.DedicatedServer, new UnavailableGameData());
        var d = BootDefinitions.For(f);
        Assert.True(d.Define("cold", new StatusBlock { Name = "Boot Cold", Seconds = 4 }).Ok);
        Assert.True(d.Define("cold-hit", new EffectBlock { Applies = new StatusApply { Status = "cold" } }).Ok);
        Assert.True(d.Define("wood-token", new ItemBlock { Name = "Boot Token", Stack = 20 }).Ok);
        Assert.True(d.Define("cold-talent", new TalentBlock { Name = "Boot Cold Talent", Keeps = "cold", Game = new GameTreePlace { From = ["0x8efcc472"], Offset = [170, 0] } }).Ok);
        return f.Staged;
    }

    private static void Commit(List<BootStaged> staged, BootModState state = BootModState.Committed) =>
        BootGeneration.Record(3, [new BootModReport(Mod, 3, state, staged.Select(s => $"{s.Kind}/{s.Key}").ToList(), [])],
            state == BootModState.Committed ? new Dictionary<string, IReadOnlyList<BootStaged>> { [Mod] = staged } : new Dictionary<string, IReadOnlyList<BootStaged>>());

    [Fact]
    public void A_committed_boot_block_is_found_before_its_runtime_activation_and_after_an_owner_reset()
    {
        Commit(Staged());
        var cold = TheBlocks.Find("cold", Mod);
        Assert.NotNull(cold);
        var (lowered, _, _) = Compiler.Lower(new ModBlocks(Mod, 1) { ManifestClients = "kiln" }, "cold", new StatusBlock { Name = "Boot Cold", Seconds = 4 });
        Assert.Equal(lowered.Guid, cold.Guid);  // the id the generation published
        Assert.Equal($"{Mod}.cold", cold.GameName);
        Assert.NotNull(TheBlocks.Find("cold-hit", Mod));
        Assert.NotNull(TheBlocks.Find("cold-talent", Mod));  // a talent asks "clients": "mod" of its mod: the map is built at that level
        Assert.Equal(Compiler.Lower(new ModBlocks(Mod, 1) { ManifestClients = "kiln" }, "wood-token", new ItemBlock { Name = "Boot Token", Stack = 20 }).Handle.Guid,
            TheBlocks.Find("wood-token", Mod)?.Guid);

        // the runtime instance unloads (its ledger is reset): the boot content stays in the game, and so does its handle
        TheBlocks.ModFor(Mod, () => 1).Reset();
        Assert.Equal(cold.Guid, TheBlocks.Find("cold", Mod)?.Guid);
        Assert.Null(TheBlocks.Find("frost", Mod));
    }

    [Fact]
    public void Nothing_is_found_for_a_mod_whose_boot_content_is_not_committed()
    {
        Commit(Staged(), BootModState.ExcludedCallback);
        Assert.Null(TheBlocks.Find("cold", Mod));
        BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
        Assert.Null(TheBlocks.Find("cold", Mod));
    }

    [Fact]
    public void OnLoad_code_names_a_committed_boot_block_by_name()
    {
        Commit(Staged());
        var mod = TestMods.Create(Mod);
        var previous = Lowered._sinkOverride;
        Lowered._sinkOverride = new Counting();
        try
        {
            var hit = TestMods.As(mod, () => TheBlocks.Define("chill-hit", new EffectBlock { Applies = new StatusApply { Status = "cold" } }));
            Assert.Equal("chill-hit", hit.Name);
            Assert.Contains("\"chill-hit\"", TestMods.As(mod, () => TheBlocks.Lowered("chill-hit")));
        }
        finally { Lowered._sinkOverride = previous; }
    }

    [Fact]
    public void A_runtime_Define_of_the_committed_definition_is_a_reference_and_another_one_is_refused()
    {
        Commit(Staged());
        var mod = TestMods.Create(Mod);
        var sink = new Counting();
        var previous = Lowered._sinkOverride;
        Lowered._sinkOverride = sink;
        try
        {
            var same = TestMods.As(mod, () => TheBlocks.Define("cold", new StatusBlock { Name = "Boot Cold", Seconds = 4 }));
            Assert.Equal(TheBlocks.Find("cold", Mod)!.Guid, same.Guid);
            Assert.Equal(0, sink.Count);  // the generation registered it: nothing is registered again

            var other = Assert.Throws<BlockError>(() => TestMods.As(mod, () => TheBlocks.Define("cold", new StatusBlock { Name = "Boot Cold", Seconds = 5 })));
            Assert.StartsWith("RestartRequiredBoot:", other.Problem);
            Assert.Contains("restart", other.Problem);
            var kind = Assert.Throws<BlockError>(() => TestMods.As(mod, () => TheBlocks.Define("cold", new ItemBlock { Name = "Cold", Stack = 2 })));
            Assert.StartsWith("RestartRequiredBoot:", kind.Problem);
            Assert.Equal(0, sink.Count);

            // a name the generation does not hold compiles as before
            TestMods.As(mod, () => TheBlocks.Define("warm", new StatusBlock { Name = "Warm", Seconds = 3 }));
            Assert.True(sink.Count > 0);
        }
        finally { Lowered._sinkOverride = previous; }
    }

    private sealed class Counting : Lowered.ILoweredSink
    {
        public int Count;
        public void Program(ulong owner, Guid guid, byte[] blob) => Count++;
        public void Clone(ulong owner, string section, JsonObject def) => Count++;
        public void Item(ulong owner, ItemDefinition d) => Count++;
        public void Recipe(ulong owner, RecipeDefinition d) => Count++;
    }
}
