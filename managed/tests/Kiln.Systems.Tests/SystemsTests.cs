// SPDX-License-Identifier: MIT
// Kiln.Systems over a fake kiln.world v4 table: what Wrap answers, how a stage is dispatched, the slice guard,
// per-entity stages, filters, exceptions, a second Replace, Dispose and the mod's unload.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.SystemsTests;

[CollectionDefinition("kiln.world fake", DisableParallelization = true)]
public sealed class FakeWorldCollection;

[Collection("kiln.world fake")]
public sealed unsafe class SystemsTests
{
    private static ModContext NewMod(string id) =>
        HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Path.GetTempPath()) { Side = ModSide.Server }, 40 + (ulong)id.Length, Path.GetTempPath());

    private static T InMod<T>(ModContext mod, Func<T> f)
    {
        var previous = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;
        try { return f(); }
        finally { ModContext.CurrentMod = previous; }
    }

    [Fact]
    public void Wrap_refuses_what_is_not_a_program_of_that_world_and_throws_for_bad_arguments()
    {
        using var w = new FakeWorld();
        Assert.Throws<ArgumentException>(() => Systems.Wrap(""));
        Assert.Throws<ArgumentException>(() => Systems.Wrap("fuel_setup", new WrapOptions { World = WorldKind.Prediction }));
        var none = Systems.Wrap("no_such_program");
        Assert.False(none.Ok);
        Assert.Contains("no such program", none.Reason);
        Assert.Null(none.Wrap);
        var client = Systems.Wrap("client_program");
        Assert.False(client.Ok);
        Assert.Contains("no update", client.Reason);
        Assert.True(Systems.Wrap("fuel_setup").Ok);
    }

    [Fact]
    public void Wrap_throws_when_the_capability_is_off_and_refuses_while_native_SET_is_loaded()
    {
        using var w = new FakeWorld(_ => false);
        var e = Assert.Throws<KilnUnavailableException>(() => Systems.Wrap("fuel_setup"));
        Assert.Equal("systems.wraps", e.Capability);
        Capabilities.Report("systems.wraps", CapabilityState.Unavailable, CapabilityReason.Policy,
            "wraps: native SET is loaded (it names programs by their update pointer)");
        var set = Systems.Wrap("fuel_setup");
        Assert.False(set.Ok);
        Assert.Contains("native SET", set.Reason);
    }

    [Fact]
    public void A_client_world_wrap_is_refused_while_the_gate_holds_it()
    {
        using var w = new FakeWorld(n => n is "systems.wraps");
        var e = Assert.Throws<KilnUnavailableException>(() => Systems.Wrap("fuel_setup", new WrapOptions { World = WorldKind.Client }));
        Assert.Equal("systems.wraps.client", e.Capability);
        Capabilities.Report("systems.wraps.client", CapabilityState.Unavailable, CapabilityReason.Policy, "game test pending: 06a-wraps.client");
        Assert.True(Capabilities.IsHeld("systems.wraps.client"));
        Assert.Throws<KilnUnavailableException>(() => Systems.Wrap("fuel_setup", new WrapOptions { World = WorldKind.Client }));
    }

    [Fact]
    public void Stages_reach_kiln_world_with_the_mod_s_owner_stage_and_priority()
    {
        using var w = new FakeWorld();
        var mod = NewMod("alpha");
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup", new WrapOptions { Priority = 3 }).Wrap!);
        wrap.Before((ref Slice s) => { }).After((ref Slice s) => { }, priority: -2);
        Assert.Equal(2, w.Wraps.Count);
        Assert.All(w.Wraps, x => Assert.Equal(mod.Owner, x.Owner));
        Assert.Equal((SystemsNative.Before, 3), (w.Wraps[0].Stage, w.Wraps[0].Priority));
        Assert.Equal((SystemsNative.After, -2), (w.Wraps[1].Stage, w.Wraps[1].Priority));
        ProgramWrap.Poll();
        Assert.Equal(WrapState.Active, wrap.State);
        var info = Systems.Find("fuel_setup")!;
        Assert.Equal(["Fuel"], info.Writes);
        Assert.Equal(2, info.Wraps.Count);
        Assert.All(info.Wraps, i => Assert.Equal("alpha", i.Mod));
    }

    [Fact]
    public void A_slice_stage_sees_its_slice_and_cannot_call_tick_thread_APIs()
    {
        using var w = new FakeWorld();
        int count = 0;
        ulong step = 0;
        bool world = false;
        EntityId second = default;
        string? program = null;
        Exception? guarded = null;
        var outOfRange = false;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.Before((ref Slice s) =>
        {
            count = s.Count;
            step = s.Step;
            world = s.IsWorldThread;
            second = s.EntityAt(1);
            program = s.Program;
            try { Services.RequireTickThread("a test"); } catch (InvalidOperationException e) { guarded = e; }
            try { s.EntityAt(3); } catch (ArgumentOutOfRangeException) { outOfRange = true; }
        });
        w.Call(w.Wraps[0]);
        Assert.Equal((3, 77ul, true, new EntityId(102), "fuel_setup"), (count, step, world, second, program));
        Assert.NotNull(guarded);
        Assert.Contains("program wrap", guarded!.Message);
        Assert.True(outOfRange);
        Services.RequireTickThread("outside a stage");  // the guard ends with the stage
        Assert.Throws<InvalidOperationException>(() => InSlice(() => Game.AssertTickThread()));
    }

    private static void InSlice(Action a)
    {
        SliceGuard.Depth++;
        try { a(); } finally { SliceGuard.Depth--; }
    }

    [Fact]
    public void A_second_Replace_is_refused_naming_the_mod_that_holds_it()
    {
        using var w = new FakeWorld();
        var a = InMod(NewMod("first-mod"), () => Systems.Wrap("fuel_setup").Wrap!);
        var b = InMod(NewMod("second"), () => Systems.Wrap("fuel_setup").Wrap!);
        Assert.True(a.Replace((ref Slice s) => { }).Ok);
        var r = b.Replace((ref Slice s) => { });
        Assert.False(r.Ok);
        Assert.Contains("first-mod", r.Reason);
        // one native kiln.world refuses (a native service holds it): the reason says so
        w.Wraps.Clear();
        Systems.Live.Clear();
        w.Wraps.Add(new FakeWorld.FakeWrap { Owner = 7, Handle = 50, Stage = SystemsNative.Replace, Index = 0 });
        var c = InMod(NewMod("third"), () => Systems.Wrap("fuel_setup").Wrap!).Replace((ref Slice s) => { });
        Assert.False(c.Ok);
        Assert.Contains("owner 7", c.Reason);
    }

    [Fact]
    public void A_Replace_runs_the_original_once_and_a_throwing_or_filtered_Replace_leaves_the_slice_to_the_game()
    {
        using var w = new FakeWorld();
        var calls = 0;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        Exception? second = null;
        wrap.Replace((ref Slice s) =>
        {
            calls++;
            if (calls == 1)
            {
                s.RunOriginal();
                try { s.RunOriginal(); } catch (InvalidOperationException e) { second = e; }
            }
            else throw new InvalidOperationException("the second slice throws");
        });
        w.Call(w.Wraps[0]);
        Assert.Equal(1, w.OriginalRuns);
        Assert.NotNull(second);
        w.InRunOriginal = false;
        w.Call(w.Wraps[0]);  // throws before RunOriginal: the slice is the game's
        Assert.Equal(2, w.OriginalRuns);
        Assert.True(w.Wraps[0].Gone);  // the stage is off from now on
        var disabled = new List<WrapDisabledEvent>();
        wrap.Disabled += disabled.Add;
        ProgramWrap.Poll();
        Assert.Equal(1, wrap.Stats.Faults);
        w.InRunOriginal = false;
        // a filter that says no: the slice is the game's too
        var other = Systems.Wrap("fuel_setup").Wrap!.When(_ => false);
        w.Wraps.Clear();
        Systems.Live.Clear();
        other.Replace((ref Slice s) => calls += 100);
        w.Call(w.Wraps[0]);
        Assert.Equal(3, w.OriginalRuns);
        Assert.True(calls < 100);
    }

    [Fact]
    public void A_throw_disables_only_its_stage_raises_Disabled_and_counts_for_the_mod()
    {
        using var w = new FakeWorld();
        var mod = NewMod("thrower");
        var ok = 0;
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        WrapDisabledEvent? seen = null;
        wrap.Disabled += e => seen = e;
        wrap.Before((ref Slice s) => throw new FormatException("bad")).After((ref Slice s) => ok++);
        w.Call(w.Wraps[0]);
        w.Call(w.Wraps[1]);
        w.Call(w.Wraps[0]);  // gone: not called again
        w.Call(w.Wraps[1]);
        Assert.Equal(2, ok);
        ProgramWrap.Poll();
        Assert.NotNull(seen);
        Assert.Equal(WrapStage.Before, seen!.Stage);
        Assert.Contains("FormatException", seen.Reason);
        Assert.Equal("thrower", seen.Mod);
        // the mod's scheduler holds the rethrow the host counts toward its fault policy
        Assert.Throws<FormatException>(() => { lock (mod.PendingLock) mod.Pending.Dequeue()(); });
    }

    [Fact]
    public void Per_entity_stages_run_for_every_entity_and_a_throw_stops_them()
    {
        using var w = new FakeWorld();
        var seen = new List<uint>();
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.BeforeEach(e => seen.Add(e.Value));
        w.Call(w.Wraps[0]);
        Assert.Equal([101u, 102u, 103u], seen);
        var n = 0;
        wrap.AfterEach(e => { if (++n == 2) throw new InvalidOperationException("entity 2"); });
        w.Call(w.Wraps[1]);
        Assert.Equal(2, n);
        Assert.True(w.Wraps[1].Gone);
    }

    [Fact]
    public void WhenArchetype_decides_once_per_archetype()
    {
        using var w = new FakeWorld();
        var ran = 0;
        var wrap = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel"], ["Health"]).Before((ref Slice s) => ran++);
        w.Call(w.Wraps[0]);
        Assert.Equal(0, ran);  // layout 0x5000 lacks Fuel
        w.Layout = 0x6000;
        w.LayoutHas.Add(Hash("Fuel"));
        w.Call(w.Wraps[0]);
        Assert.Equal(1, ran);  // a new layout, decided: it has Fuel and lacks Health
        w.LayoutHas.Clear();
        w.Call(w.Wraps[0]);
        Assert.Equal(2, ran);  // cached for this layout: not asked again
        w.Layout = 0x5000;
        w.LayoutHas.Add(Hash("Fuel"));
        w.Call(w.Wraps[0]);
        Assert.Equal(2, ran);  // 0x5000 was decided (no) before
        w.Layout = 0x7000;
        w.LayoutHas.Add(Hash("Health"));
        w.Call(w.Wraps[0]);
        Assert.Equal(2, ran);  // has Fuel but also Health
        SliceInfo? info = null;
        Systems.Wrap("fuel_setup").Wrap!.When(i => { info = i; return true; }).After((ref Slice s) => { });
        w.Call(w.Wraps[^1], worldThread: 0);
        Assert.Equal(new SliceInfo("fuel_setup", WorldKind.Server, 3, 77, false, 0x7000), info);
    }

    private static uint Hash(string name)
    {
        var h = 0x811C9DC5u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes("keen::ecs::" + name)) h = (h ^ b) * 0x01000193u;
        return h;
    }

    [Fact]
    public void Dispose_stops_every_stage_and_the_mod_s_unload_waits_until_none_runs()
    {
        using var w = new FakeWorld();
        var mod = NewMod("unloader");
        var calls = 0;
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        wrap.Before((ref Slice s) => calls++);
        wrap.Dispose();
        Assert.True(w.Wraps[0].Gone);
        Assert.Equal(WrapState.Removed, wrap.State);
        var second = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        second.After((ref Slice s) => calls++);
        w.ReleasePending = 3;  // three polls with a stage still running somewhere
        mod.Retire();
        Assert.True(w.Releases >= 4);
        Assert.True(w.Wraps[1].Gone);
        Assert.Equal(0, calls);
        Assert.Equal(WrapState.Removed, second.State);
    }

    // ---- the mirrors, against the native record ----

    private static JsonNode Abi([CallerFilePath] string self = "")
    {
        var repo = Path.GetFullPath(Path.Combine(Path.GetDirectoryName(self)!, "..", "..", ".."));
        return JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "native", "services", "world", "tests", "abi.json")))!;
    }

    private static void Same(JsonNode abi, string native, Type mirror, (string Mirror, string Native)[]? names = null)
    {
        var fields = abi[native]!["fields"]!.AsArray().Select(f => ((string)f![0]!, (int)f[1]!)).ToList();
        Assert.Equal((int)abi[native]!["size"]!, Marshal.SizeOf(mirror));
        var mine = mirror.GetFields(System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.Public)
            .OrderBy(f => f.MetadataToken).Select(f => (int)Marshal.OffsetOf(mirror, f.Name)).ToList();
        Assert.Equal(fields.Select(f => f.Item2).ToList(), mine);
    }

    [Fact]
    public void The_mirrors_match_the_header_field_for_field()
    {
        var abi = Abi();
        Same(abi, "KilnGameSlice", typeof(SystemsNative.Slice));
        Same(abi, "KilnGameWrapInfo", typeof(SystemsNative.WrapInfo));
        Same(abi, "KilnGameProgramInfo", typeof(SystemsNative.ProgramInfo));
        Same(abi, "KilnGameProgramsStatus", typeof(SystemsNative.ProgramsStatus));
        var v4 = abi["KilnWorldApiV4"]!["fields"]!.AsArray().Select(f => ((string)f![0]!, (int)f[1]!)).ToDictionary(f => f.Item1, f => f.Item2);
        var t = typeof(SystemsNative.Table);
        Assert.Equal(v4["game_program_find"], (int)Marshal.OffsetOf(t, "GameProgramFind"));
        Assert.Equal(v4["game_program_wrap"], (int)Marshal.OffsetOf(t, "GameProgramWrap"));
        Assert.Equal(v4["game_slice_run_original"], (int)Marshal.OffsetOf(t, "GameSliceRunOriginal"));
        Assert.Equal(v4["game_program_list"], (int)Marshal.OffsetOf(t, "GameProgramList"));
        Assert.Equal(v4["game_archetype_has"], (int)Marshal.OffsetOf(t, "GameArchetypeHas"));
        Assert.Equal((int)abi["KilnWorldApiV4"]!["size"]!, Marshal.SizeOf(t));
    }
}
