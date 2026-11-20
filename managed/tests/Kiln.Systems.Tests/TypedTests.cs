// SPDX-License-Identifier: MIT
// Typed wraps over the fake kiln.world table: Systems.Wrap<T>'s contract check and its refusal text, a view's positions,
// Bind's own checks, typed per-entity stages and AroundEach (chapter 06a 5.3, 9.6, 9.7).
using Xunit;

namespace Kiln.SystemsTests;

// What Kiln.Game generates for fuel_setup (typegen.py programs), by hand for the fake's lists.
internal ref struct FuelView : IGameProgram<FuelView>
{
    private readonly EntityView _e;
    private FuelView(EntityView e) => _e = e;
    public static string Name => "fuel_setup";
    public static uint Contract => FakeWorld.FuelContract;
    public static string Lists => "read_opt=CurrentTransform\nwrite=Fuel\nsys_read=impact::AttributeRegistry\n" + FakeWorld.FuelTail;
    static FuelView IGameProgram<FuelView>.Bind(EntityView entity) => new(entity.Expect(Name, Contract));
    public readonly EntityId Entity => _e.Entity;
    public readonly bool HasCurrentTransform => _e.HasAt(SystemsNative.ListReadOpt, 0);
    public readonly ref readonly long CurrentTransform => ref _e.ReadAt<long>(SystemsNative.ListReadOpt, 0, "CurrentTransform");
    public readonly Attribute32 Fuel => _e.AttributeAt(SystemsNative.ListWrite, 0, "Fuel");
}

// A view of another program.
internal ref struct OtherView : IGameProgram<OtherView>
{
    private readonly EntityView _e;
    private OtherView(EntityView e) => _e = e;
    public static string Name => "client_program";
    public static uint Contract => 7;
    public static string Lists => FakeWorld.FuelTail;
    static OtherView IGameProgram<OtherView>.Bind(EntityView entity) => new(entity.Expect(Name, Contract));
}

// A view whose author left out the contract.
internal ref struct NoContractView : IGameProgram<NoContractView>
{
    public static string Name => "fuel_setup";
    public static uint Contract => 0;
    public static string Lists => "";
    static NoContractView IGameProgram<NoContractView>.Bind(EntityView entity) => default;
}

[Collection("kiln.world fake")]
public sealed unsafe class TypedTests
{
    [Fact]
    public void An_M2_table_keeps_untyped_wraps_and_refuses_typed_access_without_reading_the_tail()
    {
        using var w = new FakeWorld();
        w.SetTableSize(440);
        Assert.NotNull(Systems.Find("fuel_setup"));
        var wrap = Systems.Wrap("fuel_setup");
        Assert.True(wrap.Ok, wrap.Reason);
        var calls = 0;
        wrap.Wrap!.BeforeEach((ref EntityView e) => calls++);
        w.Call(w.Wraps[0]);
        Assert.Equal(3, calls);
        var typed = Systems.Wrap<FuelView>();
        Assert.False(typed.Ok);
        Assert.Contains("contract", typed.Reason);
        Assert.Equal(0, w.ContractCalls);
    }

    [Fact]
    public void A_typed_wrap_checks_the_contract_and_its_view_reads_the_programs_slots()
    {
        using var w = new FakeWorld();
        var r = Systems.Wrap<FuelView>();
        Assert.True(r.Ok, r.Reason);
        Assert.Equal("fuel_setup", r.Wrap!.Program);
        var seen = new List<(uint, int, long)>();
        r.Wrap.BeforeEach((ref FuelView v) =>
        {
            var fuel = v.Fuel;
            seen.Add((v.Entity.Value, fuel[0], v.HasCurrentTransform ? v.CurrentTransform : -1));
        });
        w.Call(w.Wraps[0]);
        Assert.Equal([(101u, 100, 7000L), (102u, 200, 7001L), (103u, 300, -1L)], seen);
        ProgramWrap.Poll();
        Assert.Equal(WrapState.Active, r.Wrap.State);
    }

    [Fact]
    public void A_live_descriptor_with_another_contract_is_refused_with_the_lists_that_changed()
    {
        using var w = new FakeWorld();
        w.ContractValue = 0x0BADF00D;
        w.ContractText = FakeWorld.Full("read_opt=CurrentTransform\nwrite=Fuel,DynamicFuelSetupV2\nsys_read=impact::AttributeRegistry\n")
            .Replace("flags=00000000", "flags=00000400");
        var r = Systems.Wrap<FuelView>();
        Assert.False(r.Ok);
        Assert.Null(r.Wrap);
        Assert.Contains("fuel_setup: the live descriptor is not the view's", r.Reason);
        Assert.Contains("write list changed: +DynamicFuelSetupV2", r.Reason);
        Assert.Contains("flags changed: 00000000 -> 00000400", r.Reason);
        Assert.DoesNotContain("read_opt", r.Reason);
        Assert.Empty(w.Wraps);
    }

    [Fact]
    public void Contract_differences_name_added_removed_reordered_lists_and_scalars()
    {
        var tail = FakeWorld.FuelTail;
        var said = Contracts.Differences("p", "write=A,B\nread=C\n" + tail, FakeWorld.Full("write=B,D\nread=C\nwith=E\n").Replace("interval=1", "interval=2"));
        Assert.Equal("p: the live descriptor is not the view's (made for another game build): write list changed: +D -A; " +
            "with list changed: +E; interval changed: 1 -> 2", said);
        Assert.Contains("read list changed: its order", Contracts.Differences("p", "read=A,B\n" + tail, FakeWorld.Full("read=B,A\n")));
    }

    [Fact]
    public void Wrap_T_refuses_what_the_untyped_Wrap_refuses_and_throws_for_a_view_without_a_contract()
    {
        using var w = new FakeWorld();
        Assert.Throws<ArgumentException>(() => Systems.Wrap<NoContractView>());
        var client = Systems.Wrap<OtherView>();
        Assert.False(client.Ok);
        Assert.Contains("no update", client.Reason);
        w.ContractResult = SystemsNative.Unavailable;
        var unreadable = Systems.Wrap<FuelView>();
        Assert.False(unreadable.Ok);
        Assert.Contains("cannot read its descriptor", unreadable.Reason);
        Assert.Empty(w.Wraps);
    }

    [Fact]
    public void Wrap_T_throws_when_typed_wraps_are_off()
    {
        using var w = new FakeWorld(n => n is "systems.wraps");
        var e = Assert.Throws<KilnUnavailableException>(() => Systems.Wrap<FuelView>());
        Assert.Equal("systems.wraps.typed", e.Capability);
        Assert.True(Systems.Wrap("fuel_setup").Ok);  // untyped wraps go on
    }

    [Fact]
    public void Bind_refuses_a_view_of_another_program_and_a_contract_the_live_descriptor_lacks()
    {
        using var w = new FakeWorld();
        string? other = null, stale = null;
        var ok = 0;
        Systems.Wrap("fuel_setup").Wrap!.BeforeEach((ref EntityView e) =>
        {
            try { IGameProgramBind<OtherView>(e); }
            catch (InvalidOperationException x) { other = x.Message; }
            try
            {
                IGameProgramBind<FuelView>(e);
                ok++;
            }
            catch (InvalidOperationException x) { stale = x.Message; }
        });
        w.EntityIds = [101];
        w.Call(w.Wraps[0]);
        Assert.Contains("a view of client_program cannot bind an entity of a fuel_setup slice", other);
        Assert.Equal(1, ok);
        Assert.Null(stale);
        // the next world: its descriptor's contract is asked again (once per session), and is not the view's
        w.Session++;
        w.ContractValue = 0x12345678;
        var calls = w.ContractCalls;
        w.Call(w.Wraps[0]);
        w.Call(w.Wraps[0]);
        Assert.Equal(calls + 1, w.ContractCalls);
        Assert.Contains("the live descriptor's contract is 0x12345678, the view's 0xC0FFEE01", stale);
        Assert.Equal(1, ok);
    }

    private static T IGameProgramBind<T>(EntityView e) where T : IGameProgram<T>, allows ref struct => T.Bind(e);

    [Fact]
    public void AroundEach_applies_with_the_snapshot_its_capture_kept_for_the_same_row()
    {
        using var w = new FakeWorld();
        var wrap = Systems.Wrap<FuelView>().Wrap!;
        wrap.AroundEach(capture: (in FuelView v) => v.Fuel[0],
            apply: (ref FuelView v, in int before) =>
            {
                var fuel = v.Fuel;
                var added = fuel[0] - before;
                if (added > 0) fuel[0] += 3 * added;  // four times what the game added
            });
        Assert.Equal(2, w.Wraps.Count);
        Assert.Equal([SystemsNative.Before, SystemsNative.After], w.Wraps.Select(x => x.Stage));
        // the game's update adds 10 to the first two rows
        w.Dispatch(update: () =>
        {
            *(int*)(w.Fuel + 8) += 10;
            *(int*)(w.Fuel + 16 + 8) += 10;
        });
        Assert.Equal(140, *(int*)(w.Fuel + 8));
        Assert.Equal(240, *(int*)(w.Fuel + 16 + 8));
        Assert.Equal(300, *(int*)(w.Fuel + 32 + 8));
        // a second slice: the snapshots were this slice's only
        w.Dispatch();
        Assert.Equal(140, *(int*)(w.Fuel + 8));
    }

    [Fact]
    public void An_After_whose_capture_did_not_run_on_this_thread_applies_nothing()
    {
        using var w = new FakeWorld();
        var applied = 0;
        Systems.Wrap<FuelView>().Wrap!.AroundEach((in FuelView v) => v.Fuel[0], (ref FuelView v, in int before) => applied++);
        w.Call(w.Wraps[1]);  // the After alone
        Assert.Equal(0, applied);
        w.Dispatch();
        Assert.Equal(3, applied);
    }

    [Fact]
    public void A_capture_whose_After_never_came_is_dropped_by_the_next_slice()
    {
        using var w = new FakeWorld();
        var applied = new List<int>();
        Systems.Wrap<FuelView>().Wrap!.AroundEach((in FuelView v) => v.Fuel[0], (ref FuelView v, in int before) => applied.Add(before));
        w.Call(w.Wraps[0]);                       // a Before alone (its After went between them): 100, 200, 300 kept
        *(int*)(w.Fuel + 8) = 150;
        w.Dispatch();                             // the next slice captures again: its After sees its own snapshots
        Assert.Equal([150, 200, 300], applied);
    }

    [Fact]
    public void A_capture_that_throws_turns_both_halves_off_and_the_slice_is_the_games()
    {
        using var w = new FakeWorld();
        var applied = 0;
        var disabled = new List<WrapDisabledEvent>();
        var wrap = Systems.Wrap<FuelView>().Wrap!;
        wrap.Disabled += disabled.Add;
        wrap.AroundEach((in FuelView v) => v.Entity.Value == 102 ? throw new InvalidOperationException("capture") : v.Fuel[0],
            (ref FuelView v, in int before) => applied++);
        w.Dispatch(update: () => *(int*)(w.Fuel + 8) += 10);
        Assert.Equal(0, applied);
        Assert.Equal(110, *(int*)(w.Fuel + 8));
        Assert.All(w.Wraps, x => Assert.True(x.Gone));
        Assert.Equal(2, disabled.Count);
        Assert.Contains(disabled, e => e.Stage == WrapStage.Before && e.Reason.Contains("InvalidOperationException"));
        Assert.Contains(disabled, e => e.Stage == WrapStage.After && e.Reason.Contains("the other half of its AroundEach"));
        Assert.Equal(1, wrap.Stats.Faults);
    }

    [Fact]
    public void Typed_AfterEach_and_Replace_pass_through_to_the_untyped_wrap()
    {
        using var w = new FakeWorld();
        var wrap = Systems.Wrap<FuelView>().Wrap!;
        var after = 0;
        wrap.AfterEach((ref FuelView v) => after++).When(i => i.Count > 0);
        var replace = wrap.Replace((ref Slice s) => s.RunOriginal());
        Assert.True(replace.Ok);
        Assert.Same(wrap, replace.Wrap);
        w.Call(w.Wraps[0]);
        Assert.Equal(3, after);
        wrap.Dispose();
        Assert.Equal(WrapState.Removed, wrap.State);
        Assert.Same(wrap.Untyped, wrap.Untyped);
    }
}
