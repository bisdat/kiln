// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.BlockTests;

/// <summary>NpcBodies.Status and NpcBodyBuildChangedEvent (chapter 16f §5.4) on a scripted boot-build manifest (its "bodies" rows).</summary>
public sealed class NpcBodyStatusTests
{
    private const string Mod = "npc-body-test";
    private static readonly Guid Template = new("11111111-2222-3333-4444-555555555555");
    private static readonly Guid Model = new("aaaaaaaa-0000-0000-0000-000000000001");
    private static readonly Guid Graph = new("bbbbbbbb-0000-0000-0000-000000000002");
    private static readonly string Closure = new('c', 64);

    private static JsonObject Row(string npc, string state, string reason = "", ulong generation = 1, string? mod = null, string? key = null) => new()
    {
        ["mod"] = mod ?? Mod, ["npc"] = npc, ["key"] = key ?? npc, ["state"] = state, ["reason"] = reason,
        ["template"] = state == "ready" ? Template.ToString() : "", ["model"] = state == "ready" ? Model.ToString() : "",
        ["graph"] = state == "ready" ? Graph.ToString() : "", ["closure"] = state == "ready" ? Closure : "", ["generation"] = generation,
    };

    /// <summary>The statics NpcBodies and NpcBodyRows read, scripted for one test and put back after it.</summary>
    private sealed class Scope : IDisposable
    {
        private readonly Func<BlockCapabilities.Row>? _row = BlockCapabilities.NpcBodyForTests;
        private readonly Func<bool> _client = NpcBodies.IsClient;
        private readonly Action<Action> _post = NpcBodies.PostToClient;
        public readonly List<NpcBodyBuildChangedEvent> Events = [];
        public readonly List<Action> Posted = [];
        public string? Manifest;
        public bool Client;
        public bool Available = true;

        public Scope(params JsonObject[] rows)
        {
            NpcBodies.ResetForTests();
            Set(rows);
            NpcBodyRows.TestSource = () => Manifest;
            NpcBodies.Raise = Events.Add;
            NpcBodies.IsClient = () => Client;
            NpcBodies.PostToClient = Posted.Add;
            BlockCapabilities.NpcBodyForTests = () => Available
                ? new(CapabilityState.Available, CapabilityReason.None, null)
                : new(CapabilityState.Unavailable, CapabilityReason.ServiceMissing, "quests.boot_pack: the native quests backend is not loaded");
        }

        public void Set(params JsonObject[] rows) =>
            Manifest = new JsonObject { ["mods"] = new JsonArray(), ["bodies"] = new JsonArray(rows.Select(r => (JsonNode)r.DeepClone()).ToArray()) }.ToJsonString();

        public void Dispose()
        {
            NpcBodyRows.TestSource = null;
            NpcBodies.Raise = null;
            NpcBodies.IsClient = _client;
            NpcBodies.PostToClient = _post;
            BlockCapabilities.NpcBodyForTests = _row;
            NpcBodies.ResetForTests();
        }
    }

    [Fact]
    public void A_ready_row_is_Ready_with_the_builds_identities()
    {
        using var s = new Scope(Row("tobin", "ready", generation: 7));
        var r = NpcBodies.Status($"{Mod}:tobin");
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal(NpcBodyBuildState.Ready, r.State);
        Assert.Equal(Mod, r.ModId);
        Assert.Equal($"{Mod}:tobin", r.BlockKey);
        Assert.Equal((Template, Model, Graph, 7UL), (r.Template, r.Model, r.Graph, r.Generation));
    }

    [Fact]
    public void A_failed_row_is_Failed_with_the_native_refusal_and_is_queryable_without_a_handle()
    {
        const string why = "npc-body-test: tobin: an authored body needs the network clearance (blocks.npc_body), which this build does not have";
        using var s = new Scope(Row("tobin", "failed", why));
        var r = NpcBodies.Status($"{Mod}:tobin");
        Assert.False(r.Ok);
        Assert.Equal(NpcBodyBuildState.Failed, r.State);
        Assert.Equal(why, r.Reason);
        Assert.Equal(Guid.Empty, r.Model);
    }

    [Fact]
    public void A_removed_row_is_Removed()
    {
        using var s = new Scope(Row("tobin", "removed"));
        var r = NpcBodies.Status($"{Mod}:tobin");
        Assert.False(r.Ok);
        Assert.Equal(NpcBodyBuildState.Removed, r.State);
        Assert.False(string.IsNullOrEmpty(r.Reason));
    }

    [Fact]
    public void A_well_formed_key_without_a_record_is_Failed_NoDefinition()
    {
        using var s = new Scope(Row("tobin", "ready"));
        var r = NpcBodies.Status($"{Mod}:nobody");
        Assert.Equal((false, "NoDefinition", NpcBodyBuildState.Failed, $"{Mod}:nobody"), (r.Ok, r.Reason, r.State, r.BlockKey));
        s.Manifest = """{"mods":[]}""";  // an older native build: no "bodies" key at all
        Assert.Equal("NoDefinition", NpcBodies.Status($"{Mod}:tobin").Reason);
        s.Manifest = null;  // no manifest (no kiln.knowledge)
        Assert.Equal("NoDefinition", NpcBodies.Status($"{Mod}:tobin").Reason);
    }

    [Fact]
    public void A_block_renamed_with_Was_is_found_by_its_key_too()
    {
        using var s = new Scope(Row("tobin2", "ready", key: "tobin"));
        Assert.Equal(NpcBodyBuildState.Ready, NpcBodies.Status($"{Mod}:tobin").State);
        Assert.Equal($"{Mod}:tobin2", NpcBodies.Status($"{Mod}:tobin").BlockKey);
    }

    [Fact]
    public void A_handle_of_a_renamed_block_finds_the_row_the_build_keyed_by_Was()
    {
        // a definition from code is written under its key (Was, or the name) and the native row then names that key
        using var s = new Scope(Row("tobin", "ready"));
        var npc = new NpcBlock { Name = "Tobin", Was = "tobin", Body = Bodies.Fisher };
        var handle = new BlockHandle<NpcBlock>(Mod, "tobin-renamed", BlockIds.Derive(Mod, "npc", "tobin"), $"Kiln_{Mod}_tobin", npc);
        Assert.Equal(NpcBodyBuildState.Ready, NpcBodies.Status(handle).State);
        // the same body defined from code: v1 builds authored bodies from data files only, so it is refused whatever the start built
        // (review3 M2), and nothing waits for a restart
        NpcBodies.DefinedFromCode(Mod, "tobin-renamed");
        Assert.Equal(NpcBodyBuildState.Failed, NpcBodies.Status($"{Mod}:tobin-renamed").State);
        Assert.Contains("blocks/*.json", NpcBodies.Status($"{Mod}:tobin-renamed").Reason);
        Assert.Empty(NpcBodyRows.Late);
    }

    [Theory]
    [InlineData("")]
    [InlineData("tobin")]
    [InlineData(":tobin")]
    [InlineData("npc-body-test:")]
    [InlineData("a:b:c")]
    [InlineData("npc-body-test: tobin")]
    public void Invalid_key_syntax_throws_ArgumentException(string key)
    {
        using var s = new Scope();
        Assert.Throws<ArgumentException>(() => NpcBodies.Status(key));
    }

    [Fact]
    public void A_null_key_or_handle_throws()
    {
        using var s = new Scope();
        Assert.Throws<ArgumentNullException>(() => NpcBodies.Status((string)null!));
        Assert.Throws<ArgumentNullException>(() => NpcBodies.Status((BlockHandle<NpcBlock>)null!));
    }

    [Fact]
    public void A_handle_of_an_NPC_block_asks_by_its_name_and_a_foreign_one_throws()
    {
        using var s = new Scope(Row("tobin", "ready"));
        var npc = new NpcBlock { Name = "Tobin", Body = Bodies.Fisher };
        var own = new BlockHandle<NpcBlock>(Mod, "tobin", BlockIds.Derive(Mod, "npc", "tobin"), $"Kiln_{Mod}_tobin", npc);
        Assert.Equal(NpcBodyBuildState.Ready, NpcBodies.Status(own).State);
        // the GUID another kind of block derives: not a handle Blocks.Define gave for an NPC
        var foreign = new BlockHandle<NpcBlock>(Mod, "tobin", BlockIds.Derive(Mod, "item", "tobin"), $"{Mod}.tobin", npc);
        Assert.Throws<ArgumentException>(() => NpcBodies.Status(foreign));
    }

    [Fact]
    public void With_the_capability_off_Status_throws_naming_blocks_npc_body()
    {
        using var s = new Scope(Row("tobin", "ready")) { Available = false };
        var e = Assert.Throws<KilnUnavailableException>(() => NpcBodies.Status($"{Mod}:tobin"));
        Assert.Equal("blocks.npc_body", e.Capability);
        Assert.Contains("blocks.npc_body", e.Message, StringComparison.Ordinal);
    }

    // ---- events ----------------------------------------------------------------------------------------------------------------

    [Fact]
    public void The_boot_builds_rows_give_exactly_one_event_each_Failed_included()
    {
        using var s = new Scope(Row("tobin", "ready", generation: 3), Row("bram", "failed", "refused: no clearance"));
        NpcBodies.Kick();
        NpcBodies.Kick();
        NpcBodies.Flush(clientSide: false);
        Assert.Equal(2, s.Events.Count);
        var bram = s.Events.Single(e => e.After.BlockKey == $"{Mod}:bram");
        Assert.Equal((NpcBodyBuildState.Failed, "NoDefinition"), (bram.Before.State, bram.Before.Reason));
        Assert.Equal((NpcBodyBuildState.Failed, "refused: no clearance"), (bram.After.State, bram.After.Reason));
        var tobin = s.Events.Single(e => e.After.BlockKey == $"{Mod}:tobin");
        Assert.Equal((NpcBodyBuildState.Ready, 3UL), (tobin.After.State, tobin.After.Generation));
        Assert.All(s.Events, e => Assert.False(e.ClientSide));
    }

    [Fact]
    public void An_unload_of_a_Ready_body_is_RemovalPendingRestart_once_and_a_reload_is_Ready_again()
    {
        using var s = new Scope(Row("tobin", "ready"), Row("bram", "failed", "refused"));
        NpcBodies.ModLoaded(Mod);
        NpcBodies.Kick();
        Assert.Equal(2, s.Events.Count);
        s.Events.Clear();
        NpcBodies.ModUnloaded(Mod);
        NpcBodies.ModUnloaded(Mod);
        NpcBodies.Kick();
        var e = Assert.Single(s.Events);  // bram stays Failed: no event for it
        Assert.Equal((NpcBodyBuildState.Ready, NpcBodyBuildState.RemovalPendingRestart), (e.Before.State, e.After.State));
        var r = NpcBodies.Status($"{Mod}:tobin");
        Assert.Equal((false, NpcBodyBuildState.RemovalPendingRestart, Model), (r.Ok, r.State, r.Model));  // the live body is left alone
        NpcBodies.ModLoaded(Mod);
        Assert.Equal(2, s.Events.Count);
        Assert.Equal((NpcBodyBuildState.RemovalPendingRestart, NpcBodyBuildState.Ready), (s.Events[1].Before.State, s.Events[1].After.State));
    }

    [Fact]
    public void A_refusal_without_a_generation_is_still_one_Failed_event()
    {
        using var s = new Scope(Row("bram", "failed", "refused: no clearance", generation: 0));
        NpcBodies.Kick();
        NpcBodies.Flush(clientSide: false);
        var e = Assert.Single(s.Events);
        Assert.Equal(("NoDefinition", "refused: no clearance", 0UL), (e.Before.Reason, e.After.Reason, e.After.Generation));
    }

    [Fact]
    public void A_mod_that_never_loaded_here_is_not_unloaded()
    {
        using var s = new Scope(Row("tobin", "ready"));
        NpcBodies.ModUnloaded(Mod);
        Assert.Equal(NpcBodyBuildState.Ready, NpcBodies.Status($"{Mod}:tobin").State);
    }

    [Fact]
    public void A_new_generation_is_one_more_event()
    {
        using var s = new Scope(Row("tobin", "ready", generation: 1));
        NpcBodies.Kick();
        s.Set(Row("tobin", "ready", generation: 2));
        NpcBodies.Kick();
        NpcBodies.Kick();
        Assert.Equal(2, s.Events.Count);
        Assert.Equal((1UL, 2UL), (s.Events[1].Before.Generation, s.Events[1].After.Generation));
    }

    [Fact]
    public void A_body_defined_from_code_is_Failed_with_the_way_to_define_it_and_a_handle_never_implies_Ready()
    {
        // review3 M2: a body from C# has no cooked source identity for the join check and no material check: v1 refuses it and says to
        // define it in blocks/*.json; nothing waits for a restart, so the handshake sends no pending key
        using var s = new Scope();
        NpcBodies.DefinedFromCode(Mod, "tobin");
        var r = NpcBodies.Status($"{Mod}:tobin");
        Assert.Equal((false, NpcBodyBuildState.Failed), (r.Ok, r.State));
        Assert.Contains("defined in C#", r.Reason);
        Assert.Contains("blocks/*.json", r.Reason);
        Assert.Equal((Guid.Empty, Guid.Empty, Guid.Empty, 0UL), (r.Template, r.Model, r.Graph, r.Generation));
        Assert.Empty(NpcBodyRows.Late);
        NpcBodies.DefinedFromCode(Mod, "tobin");  // the same definition again (a reload): no second event
        var e = Assert.Single(s.Events);
        Assert.Equal((NpcBodyBuildState.Failed, "NoDefinition", NpcBodyBuildState.Failed, r.Reason), (e.Before.State, e.Before.Reason, e.After.State, e.After.Reason));
        NpcBodies.FromData(Mod, "tobin");  // now from a data file: the boot build's record is the status
        Assert.Equal("NoDefinition", NpcBodies.Status($"{Mod}:tobin").Reason);
    }

    [Fact]
    public void A_body_defined_from_code_is_Failed_even_where_the_start_built_one_under_that_name()
    {
        using var s = new Scope(Row("tobin", "ready"));
        NpcBodies.DefinedFromCode(Mod, "tobin");
        Assert.Equal(NpcBodyBuildState.Failed, NpcBodies.Status($"{Mod}:tobin").State);
        Assert.Empty(NpcBodyRows.Late);
    }

    [Fact]
    public void A_game_client_publishes_on_its_main_thread_once_per_change()
    {
        using var s = new Scope(Row("tobin", "ready")) { Client = true };
        NpcBodies.Kick();
        NpcBodies.Kick();
        Assert.Empty(s.Events);  // nothing before the client's main thread runs it
        var post = Assert.Single(s.Posted);
        post();
        var e = Assert.Single(s.Events);
        Assert.True(e.ClientSide);
        NpcBodies.Kick();
        Assert.Single(s.Posted);  // review3 N7: nothing changed since, so nothing is posted to the main thread
        s.Posted[^1]();
        Assert.Single(s.Events);
    }
}
