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
    public void Filters_run_under_the_slice_guard_and_mod_context()
    {
        using var w = new FakeWorld();
        var mod = NewMod("filter-owner");
        var guarded = false;
        ModContext? caller = null;
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        wrap.When(_ =>
        {
            caller = ModContext.CurrentMod;
            try { Services.RequireTickThread("filter"); }
            catch (InvalidOperationException) { guarded = true; }
            return true;
        }).Before((ref Slice s) => { });
        w.Call(w.Wraps[0]);
        Assert.True(guarded);
        Assert.Same(mod, caller);
        Services.RequireTickThread("after filter");
    }

    [Fact]
    public void A_throwing_Replace_filter_disables_the_stage_and_runs_the_original()
    {
        using var w = new FakeWorld();
        var wrap = Systems.Wrap("fuel_setup").Wrap!.When(_ => throw new FormatException("filter"));
        WrapDisabledEvent? disabled = null;
        wrap.Disabled += e => disabled = e;
        wrap.Replace((ref Slice s) => { });
        w.Call(w.Wraps[0]);
        Assert.Equal(1, w.OriginalRuns);
        Assert.True(w.Wraps[0].Gone);
        Assert.NotNull(disabled);
        Assert.Equal(1, wrap.Stats.Faults);
    }

    [Theory]
    [InlineData(SystemsNative.Unavailable)]
    [InlineData(SystemsNative.Internal)]
    public void A_failed_mark_refuses_the_write(int result)
    {
        using var w = new FakeWorld();
        w.MarkResult = result;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.BeforeEach((ref EntityView e) => e.Write<FuelBlock>("Fuel").Current = 7);
        w.Call(w.Wraps[0]);
        Assert.Equal(100, *(int*)(w.Fuel + 8));
        Assert.True(w.Wraps[0].Gone);
        Assert.Equal(1, wrap.Stats.Faults);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void An_unknown_component_size_refuses_access(bool write)
    {
        using var w = new FakeWorld();
        w.FuelSize = 0;
        var returned = false;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.BeforeEach((ref EntityView e) =>
        {
            if (write) e.Write<Big>("Fuel");
            else e.Read<Big>("Fuel");
            returned = true;
        });
        w.Call(w.Wraps[0]);
        Assert.False(returned);
        Assert.True(w.Wraps[0].Gone);
    }

    [Fact]
    public void A_faulted_stage_retries_unwrap_on_tick_when_native_refuses_it_inside_the_slice()
    {
        using var w = new FakeWorld();
        w.RefuseUnwrapInSlice = true;  // production game_program_unwrap checks t_in_slice
        var calls = 0;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.Before((ref Slice s) => { calls++; throw new FormatException("stage"); });
        w.Call(w.Wraps[0]);
        w.Call(w.Wraps[0]);
        Assert.Equal(1, calls);  // managed admission closes immediately
        Assert.False(w.Wraps[0].Gone);
        ProgramWrap.Poll();
        Assert.True(w.Wraps[0].Gone);  // native chain can now release its handle and Replace reservation
        Assert.Equal(WrapState.Removed, wrap.State);
    }

    [Fact]
    public void A_timed_out_retirement_keeps_polling_until_native_removal()
    {
        using var w = new FakeWorld();
        var wrap = Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => { });
        var stage = Assert.Single(Systems.Live.Keys);
        stage.Forget(false);  // unload's two-second drain timed out
        Assert.Contains(stage, Systems.Live.Keys);
        w.Wraps[0].State = SystemsNative.Removed;  // the next native Pre removes the retired stage
        ProgramWrap.Poll();
        Assert.DoesNotContain(stage, Systems.Live.Keys);
        var self = (GCHandle)typeof(ManagedStage).GetField("_self", System.Reflection.BindingFlags.NonPublic |
            System.Reflection.BindingFlags.Instance)!.GetValue(stage)!;
        Assert.False(self.IsAllocated);
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
    public void A_per_entity_stage_on_a_build_that_cannot_iterate_is_turned_off_with_the_reason_and_not_counted_for_the_mod()
    {
        using var w = new FakeWorld();
        var mod = NewMod("noiter");
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        var seen = 0;
        WrapDisabledEvent? off = null;
        wrap.Disabled += e => off = e;
        InMod(mod, () => wrap.BeforeEach(_ => seen++));
        w.IterateResult = SystemsNative.Unavailable;  // kiln.world cannot iterate this program's slices on this build
        w.Call(w.Wraps[0]);
        Assert.Equal(0, seen);
        Assert.True(w.Wraps[0].Gone);
        Assert.NotNull(off);
        Assert.Contains("per-entity", off!.Reason);
        Assert.Contains("this build", off.Reason);
        lock (mod.PendingLock) Assert.Empty(mod.Pending);  // not a fault of the mod: nothing for its fault policy
        w.Call(w.Wraps[0]);  // off: never called again
        Assert.Equal(0, seen);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct FuelBlock
    {
        public uint Root;
        public ushort Offset;
        public byte Flags, Count;
        public int Current, Max;
    }

    [Fact]
    public void Entity_views_read_and_write_the_program_s_slots_mark_once_and_raise_one_event_per_entity_and_step()
    {
        using var w = new FakeWorld();
        var mod = NewMod("writer");
        var events = new List<ComponentWrittenEvent>();
        using var sub = Events.On<ComponentWrittenEvent>(events.Add);
        var wrap = InMod(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        var has = new List<bool>();
        long transform = 0;
        wrap.BeforeEach((ref EntityView e) =>
        {
            has.Add(e.Has("CurrentTransform"));
            if (e.Has("CurrentTransform")) transform += e.Read<long>("CurrentTransform");
            Assert.Equal(77ul, e.Step);
            e.Write<FuelBlock>("Fuel").Current += 1;
            e.Write<FuelBlock>("keen::ecs::Fuel").Current += 1;  // the same slot: marked once
        });
        wrap.AfterEach((ref EntityView e) =>
        {
            var a = e.Attribute("Fuel");
            Assert.Equal(2, a.Count);
            Assert.True(a.HasValues);
            Assert.Equal(0x05b0bf75u, a.RootId);
            a[0] += 5;
        });
        w.Call(w.Wraps[0]);
        w.Call(w.Wraps[1]);
        Assert.Equal([true, true, false], has);
        Assert.Equal(7000 + 7001, transform);
        // one mark per entity and component per stage
        Assert.Equal(new (ulong, int, int)[] { (0, 4, 0), (1, 4, 0), (2, 4, 0), (0, 4, 0), (1, 4, 0), (2, 4, 0) }, w.Marks);
        Assert.Empty(events);  // raised on the tick thread's next poll
        ProgramWrap.Poll();
        Assert.Equal(3, events.Count);
        for (var r = 0; r < 3; r++)
        {
            var e = events[r];
            Assert.Equal(new EntityId(101 + (uint)r), e.Entity);
            Assert.Equal("Fuel", e.Component);
            Assert.Equal("writer/wrap:fuel_setup", e.Writer);
            Assert.Equal(["writer/wrap:fuel_setup"], e.Writers);
            Assert.Equal("writer", e.ModId);
            Assert.True(e.Result.Changed);
            Assert.Equal(100 * (r + 1), BitConverter.ToInt32(e.Result.Before.Span[8..]));
            Assert.Equal(100 * (r + 1) + 7, BitConverter.ToInt32(e.Result.After.Span[8..]));
        }
        ProgramWrap.Poll();
        Assert.Equal(3, events.Count);
    }

    [Fact]
    public void Two_mods_writing_one_component_in_a_step_raise_one_event_naming_both_and_a_new_step_raises_again()
    {
        using var w = new FakeWorld();
        var events = new List<ComponentWrittenEvent>();
        using var sub = Events.On<ComponentWrittenEvent>(events.Add);
        var a = InMod(NewMod("first"), () => Systems.Wrap("fuel_setup").Wrap!);
        var b = InMod(NewMod("second-mod"), () => Systems.Wrap("fuel_setup").Wrap!);
        a.BeforeEach((ref EntityView e) => e.Write<int>("Fuel"));             // touched, not changed: no event of its own
        b.BeforeEach((ref EntityView e) => e.Attribute("Fuel")[0] -= 10);
        a.AfterEach((ref EntityView e) => e.Attribute("Fuel")[0] += 1);
        foreach (var x in w.Wraps) w.Call(x);
        w.StepNow = 78;
        foreach (var x in w.Wraps) w.Call(x);
        ProgramWrap.Poll();
        Assert.Equal(6, events.Count);  // 3 entities x 2 steps
        var e0 = events[0];
        Assert.Equal("second-mod/wrap:fuel_setup", e0.Writer);
        Assert.Equal(["second-mod/wrap:fuel_setup", "first/wrap:fuel_setup"], e0.Writers);
        Assert.Equal(100, BitConverter.ToInt32(e0.Result.Before.Span[8..]));
        Assert.Equal(91, BitConverter.ToInt32(e0.Result.After.Span[8..]));
        Assert.Equal(91, BitConverter.ToInt32(events[3].Result.Before.Span[8..]));
        Assert.Equal(82, BitConverter.ToInt32(events[3].Result.After.Span[8..]));
    }

    [Fact]
    public void Entity_views_refuse_what_the_program_does_not_list_and_copy_nothing_without_listeners()
    {
        using var w = new FakeWorld();
        var errors = new List<string>();
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.BeforeEach((ref EntityView e) =>
        {
            errors.Add(Try(ref e, static (ref EntityView v) => v.Read<int>("Health")));
            errors.Add(Try(ref e, static (ref EntityView v) => v.Write<long>("CurrentTransform")));
            errors.Add(Try(ref e, static (ref EntityView v) => v.Read<Big>("CurrentTransform")));
            errors.Add(Try(ref e, static (ref EntityView v) => v.Write<Big>("Fuel")));
            Assert.False(e.Has("Health"));
            Assert.False(e.Has(""));
            if (e.Row == 2) errors.Add(Try(ref e, static (ref EntityView v) => v.Read<long>("CurrentTransform")));
            e.Write<FuelBlock>("Fuel").Current = 5;
        });
        w.Call(w.Wraps[0]);
        Assert.Equal(0, WrapWrites.Waiting);  // nobody listens: nothing copied, compared or queued
        Assert.Contains("fuel_setup does not list Health", errors[0]);
        Assert.Contains("fuel_setup does not write CurrentTransform (its write lists: Fuel)", errors[1]);
        Assert.Contains("CurrentTransform is 8 bytes on this build", errors[2]);
        Assert.Contains("Fuel is 16 bytes on this build", errors[3]);
        Assert.Contains("has no CurrentTransform (an optional component of fuel_setup)", errors[^1]);
    }

    [Fact]
    public void Attribute32_Changed_runs_the_formulas_and_throws_without_the_registry()
    {
        using var w = new FakeWorld();
        var results = new List<int>();
        string? error = null;
        var wrap = Systems.Wrap("fuel_setup").Wrap!;
        wrap.AfterEach((ref EntityView e) =>
        {
            var a = e.Attribute("Fuel");
            a[0] = a[1] + 500;
            try
            {
                a.Changed();
                results.Add(a.Values[0]);
            }
            catch (InvalidOperationException x)
            {
                error = x.Message;
            }
            var outOfRange = false;
            try { _ = a[2]; }
            catch (ArgumentOutOfRangeException) { outOfRange = true; }
            Assert.True(outOfRange);
        });
        w.Call(w.Wraps[0]);
        Assert.Equal([1000, 1000, 1000], results);
        Assert.Equal(3, w.AttrCalls);
        Assert.Null(error);
        w.NoRegistry = true;
        w.Call(w.Wraps[0]);
        Assert.Contains("impact::AttributeRegistry", error);
    }

    private delegate void ViewUse(ref EntityView v);

    private static string Try(ref EntityView e, ViewUse use)
    {
        try { use(ref e); return "no throw"; }
        catch (InvalidOperationException x) { return x.Message; }
    }

    [StructLayout(LayoutKind.Sequential, Size = 64)]
    private struct Big
    {
        public long A;
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

    [Theory]
    [InlineData("unavailable")]
    [InlineData("unknown")]
    public void WhenArchetype_that_cannot_decide_turns_its_stage_off_instead_of_skipping_every_slice(string why)
    {
        using var w = new FakeWorld();
        var ran = 0;
        var disabled = new List<WrapDisabledEvent>();
        var wrap = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel", "Fule"]).Before((ref Slice s) => ran++);
        wrap.Disabled += disabled.Add;
        w.LayoutHas.Add(Hash("Fuel"));
        if (why == "unavailable") w.ArchetypeResult = SystemsNative.Unavailable;  // a build whose entity map did not resolve
        else w.UnknownComponents.Add(Hash("Fule"));                             // a name this world has no component of
        w.Call(w.Wraps[0]);
        Assert.Equal(0, ran);
        var e = Assert.Single(disabled);
        Assert.Contains(why == "unavailable" ? "cannot read a slice's archetype" : "Fule is not a component of this world", e.Reason);
        Assert.True(w.Wraps[0].Gone);
        Assert.Equal(0, wrap.Stats.Faults);  // not the mod's fault: no count for its policy
    }

    [Fact]
    public void An_empty_slice_or_an_entity_the_map_lacks_only_skips_that_slice()
    {
        using var w = new FakeWorld();
        var ran = 0;
        var wrap = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel"]).Before((ref Slice s) => ran++);
        w.LayoutHas.Add(Hash("Fuel"));
        w.ArchetypeResult = SystemsNative.NotFound;
        w.Call(w.Wraps[0]);
        Assert.Equal(0, ran);
        Assert.False(w.Wraps[0].Gone);
        w.ArchetypeResult = SystemsNative.Ok;
        w.Call(w.Wraps[0]);
        Assert.Equal(1, ran);
    }

    [Fact]
    public void WhenArchetype_publishes_both_lists_and_its_cache_together()
    {
        using var w = new FakeWorld();
        var ran = 0;
        var wrap = Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => ran++);
        w.LayoutHas.Add(Hash("Fuel"));
        IEnumerable<string> Lacks()
        {
            // A slice can enter while the tick thread enumerates the new filter's second list.
            w.Call(w.Wraps[0]);
            yield return "Health";
        }
        wrap.WhenArchetype(["Fuel"], Lacks());
        Assert.Equal(1, ran);  // sees the old (unfiltered) configuration until publication
        Assert.False(w.Wraps[0].Gone);
        w.Call(w.Wraps[0]);
        Assert.Equal(2, ran);
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

    [Fact]
    public void Attribute_refuses_a_non_attribute_before_exposing_its_bytes()
    {
        using var w = new FakeWorld();
        w.FuelIsAttribute = false; // still a writable 16-byte block with byte 6 set
        var refused = false;
        Systems.Wrap("fuel_setup").Wrap!.BeforeEach((ref EntityView e) =>
        {
            try { e.Attribute("Fuel"); }
            catch (ArgumentException) { refused = true; }
        });
        w.Call(w.Wraps[0]);
        Assert.True(refused);
        Assert.Equal(0, w.AttrCalls);
        Assert.Empty(w.Marks);
    }

    [Fact]
    public void A_new_session_uses_new_archetype_results_and_descriptor_slots_without_a_pending_poll()
    {
        using var w = new FakeWorld();
        w.LayoutHas.Add(Hash("Fuel"));
        var calls = 0;
        var filter = Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel"]).Before((ref Slice s) => calls++);
        var value = 123;
        Systems.Wrap("fuel_setup").Wrap!.BeforeEach((ref EntityView e) => e.Write<int>("Fuel") = value);
        ProgramWrap.Poll();
        foreach (var x in w.Wraps) w.Call(x);
        Assert.Equal(1, calls);
        Assert.Equal(123, *(int*)w.Fuel);
        // STOP/START and reapply happen between polls. The address and observed Active state are reused.
        w.Session++;
        w.NewDescriptor = true;
        w.LayoutHas.Clear();
        value = 456;
        foreach (var x in w.Wraps) w.Call(x);
        Assert.Equal(1, calls);
        Assert.Equal(456, *(int*)w.Fuel);
        Assert.Equal(7000, *(long*)w.Transform);
        Assert.Equal(WrapState.Active, filter.State);
    }

    [Theory]
    [InlineData(424u)]
    [InlineData(432u)]
    [InlineData(440u)]
    public void Older_native_tails_are_refused_before_calling_new_members(uint size)
    {
        using var w = new FakeWorld();
        w.SetTableSize(size);
        Assert.Null(Systems.Find("fuel_setup"));
        Assert.Throws<KilnUnavailableException>(() => Systems.Wrap("fuel_setup"));
        Assert.Empty(w.Wraps);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(12)]
    [InlineData(65532)]
    public void Attribute_refuses_storage_outside_the_component(int offset)
    {
        using var w = new FakeWorld();
        *(ushort*)(w.Fuel + 4) = (ushort)offset; // two values in a 16-byte component
        w.EntityIds = [101];
        var refused = false;
        Systems.Wrap("fuel_setup").Wrap!.BeforeEach((ref EntityView e) =>
        {
            try { e.Attribute("Fuel"); }
            catch (ArgumentException) { refused = true; }
        });
        w.Call(w.Wraps[0]);
        Assert.True(refused);
        Assert.Equal(0, w.AttrCalls);
        Assert.Empty(w.Marks);
    }

    [Fact]
    public void Attribute_accessors_use_the_validated_storage_offset()
    {
        using var w = new FakeWorld();
        *(ushort*)(w.Fuel + 4) = 12;
        w.Fuel[7] = 1;
        w.EntityIds = [101];
        Systems.Wrap("fuel_setup").Wrap!.BeforeEach((ref EntityView e) =>
        {
            var a = e.Attribute("Fuel");
            a.Values[0] = 500;
            a[0] += 1;
        });
        w.Call(w.Wraps[0]);
        Assert.Equal(100, *(int*)(w.Fuel + 8));
        Assert.Equal(501, *(int*)(w.Fuel + 12));
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
        Assert.Equal(v4["wrap_original"], (int)Marshal.OffsetOf(t, "WrapOriginal"));
        Assert.Equal(v4["in_program"], (int)Marshal.OffsetOf(t, "InProgram"));
        Assert.Equal(v4["game_program_list"], (int)Marshal.OffsetOf(t, "GameProgramList"));
        Assert.Equal(v4["game_slice_iterate"], (int)Marshal.OffsetOf(t, "GameSliceIterate"));
        Assert.Equal(v4["game_slice_archetype"], (int)Marshal.OffsetOf(t, "GameSliceArchetype"));
        Assert.Equal(v4["game_archetype_has"], (int)Marshal.OffsetOf(t, "GameArchetypeHas"));
        Assert.Equal(v4["game_slice_slot"], (int)Marshal.OffsetOf(t, "GameSliceSlot"));
        Assert.Equal(v4["game_slice_mark"], (int)Marshal.OffsetOf(t, "GameSliceMark"));
        Assert.Equal(v4["game_world_session"], (int)Marshal.OffsetOf(t, "GameWorldSession"));
        Assert.Equal(v4["game_slice_attr_validate"], (int)Marshal.OffsetOf(t, "GameSliceAttrValidate"));
        Assert.Equal(v4["game_slice_attr_changed"], (int)Marshal.OffsetOf(t, "GameSliceAttrChanged"));
        Assert.Equal(v4["game_program_contract"], (int)Marshal.OffsetOf(t, "GameProgramContract"));
        Assert.Equal((int)abi["KilnWorldApiV4"]!["size"]!, Marshal.SizeOf(t));
    }
}
