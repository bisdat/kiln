// SPDX-License-Identifier: MIT
// The kit's fake of kiln.world v4's game programs (Fakes/Systems) under the real Kiln.Systems: a mod's program wraps run
// offline on the programs a test declares (KilnHarness.Programs) over the fake world's entities, typed views included.
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.TestKit.Tests;

public sealed class SystemsFakeTests
{
    private sealed class WrapMod : Mod;

    private static Ecs.Fuel Water(int units, int max)
    {
        var f = new Ecs.Fuel { storageOffset = 8, storageCount = 2 };
        MemoryMarshal.AsBytes(new Span<Ecs.Fuel>(ref f))[6] = 1;   // flags: has values
        f.Values[0] = units;
        f.Values[1] = max;
        return f;
    }

    private static readonly FakeProgramLists FuelLists = new() { Read = ["CurrentTransform"], Write = ["Fuel"], SingletonsRead = ["impact::AttributeRegistry"] };

    // Two containers with water and a transform (one archetype), one entity with a transform only (not in the program).
    private static (EntityId A, EntityId B, EntityId Other) Containers(KilnHarness kiln)
    {
        var a = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        var b = kiln.Entities.Create(new WorldPosition(4, 5, 6));
        var other = kiln.Entities.Create(new WorldPosition(7, 8, 9));
        kiln.Entities.Set(a, Water(10, 100)).Set(b, Water(20, 200));
        return (a, b, other);
    }

    [Fact]
    public unsafe void Reservation_service_has_the_extended_shape_and_explicit_offline_results()
    {
        using var kiln = new KilnHarness();
        Assert.Equal(24, sizeof(WorldBudgetTable.Table));
        Assert.Equal(24, sizeof(Systems.ReservationTable));
        Assert.Equal(16, (int)Marshal.OffsetOf<WorldBudgetTable.Table>(nameof(WorldBudgetTable.Table.ProgramReservation)));
        Assert.Equal(16, (int)Marshal.OffsetOf<Systems.ReservationTable>(nameof(Systems.ReservationTable.ProgramReservation)));
        var t = (WorldBudgetTable.Table*)KitBackend.Instance.Service("kiln.world.budget", 1);
        Assert.True(t != null && *(uint*)t == 24);
        uint size = 123;
        Assert.Equal(FakeTables.Unavailable, t->ProgramReservation(default, default, null, 0, &size));
        Assert.Equal(0u, size);
        Assert.False(kiln.Capabilities.Is("systems.define"));
    }

    [Fact]
    public void Original_time_is_excluded_and_a_fast_stage_resets_the_overrun_streak()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("original-budget")); kiln.Step(1);
        long clock = 0;
        KitFakes.State<SystemsState>(kiln).Timestamp = () => clock;
        var slow = System.Diagnostics.Stopwatch.Frequency / 500;
        kiln.Programs.Add("fuel_setup", FuelLists, _ => clock += slow); Containers(kiln);
        var calls = 0;
        var wrap = kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!);
        kiln.As(mod, () => wrap.Replace((ref Slice s) => { calls++; s.RunOriginal(); if (calls != 10) clock += slow; }));
        kiln.Step(19);
        Assert.Equal(19, calls);
        Assert.Equal(18, kiln.Run(() => wrap.Stats).Overruns);
        kiln.Step(3);
        Assert.Equal(20, calls); // tenth consecutive overrun since the fast tenth call
    }

    [Fact]
    public void Ten_consecutive_budget_overruns_disable_only_that_stage()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("budget-kit")); kiln.Step(1);
        kiln.Programs.Add("fuel_setup", FuelLists); Containers(kiln);
        var calls = 0;
        long clock = 0;
        KitFakes.State<SystemsState>(kiln).Timestamp = () => clock;
        var disabled = new List<WrapDisabledEvent>();
        var wrap = kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => { calls++; clock += System.Diagnostics.Stopwatch.Frequency / 500; }));
        wrap.Disabled += disabled.Add;
        kiln.Step(12);
        Assert.Equal(10, calls);
        Assert.Equal(10, kiln.Run(() => wrap.Stats).Overruns);
        Assert.Single(disabled); Assert.Contains("budget", disabled[0].Reason);
        Assert.Equal(ModState.Loaded, mod.State);
    }

    [Fact]
    public void Faulting_stages_disable_and_charge_the_real_mod_policy()
    {
        using var kiln = new KilnHarness(new KilnHarnessOptions { ThrowOnModFault = false });
        var mod = kiln.Load<WrapMod>(new ModManifest("fault-kit")); kiln.Step(1);
        kiln.Programs.Add("fuel_setup", FuelLists); Containers(kiln);
        for (var n = 0; n < 5; n++)
            kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => throw new InvalidOperationException("stage fault")));
        kiln.Step(3);
        Assert.Equal(ModState.Disabled, mod.State);
        Assert.Contains("5 faults", mod.Error);
    }

    [Fact]
    public void The_table_is_kiln_world_v4_and_the_wrap_rows_are_faked()
    {
        using var kiln = new KilnHarness();
        Assert.True(kiln.Capabilities.Is("systems.wraps"), kiln.Capabilities.Why("systems.wraps"));
        Assert.True(kiln.Capabilities.Is("systems.wraps.typed"));
        Assert.False(kiln.Capabilities.Is("systems.define"));
        Assert.False(kiln.Capabilities.Is("systems.wraps.client"));
        Assert.False(kiln.Capabilities.Is("systems.wraps.prediction"));
        Assert.Contains(Coverage.All, r => r.Area == "Systems" && r.Kind == CoverageKind.RecordingFake && r.Capabilities.Contains("systems.wraps"));
    }

    [Fact]
    public void An_untyped_wrap_runs_around_the_declared_update_once_per_slice_and_goes_with_Dispose()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("wrap-kit"));
        kiln.Step(1);  // the server starts and loads the mod
        var order = new List<string>();
        kiln.Programs.Add("fuel_setup", FuelLists, s => order.Add($"update {s.Entities.Count}"));
        var (a, b, _) = Containers(kiln);
        Assert.Equal(["fuel_setup"], kiln.Programs.Names);
        var info = kiln.Run(() => Systems.Find("fuel_setup"));
        Assert.True(info is { Runs: true, World: WorldKind.Server } && info.Writes.Contains("Fuel"), info?.ToString());
        Assert.False(kiln.Run(() => Systems.Wrap("no_such_program")).Ok);
        var wrap = kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!
            .Before((ref Slice s) => order.Add($"before {s.Count} {s.EntityAt(0)}"), 1)
            .Before((ref Slice s) => order.Add("before low"), -1)
            .After((ref Slice s) => order.Add($"after {s.Program}")));
        Assert.Equal(WrapState.Pending, kiln.Run(() => wrap.State));
        order.Clear();
        kiln.Step(1);
        Assert.Equal([$"before 2 {a}", "before low", "update 2", "after fuel_setup"], order);
        Assert.Equal(WrapState.Active, kiln.Run(() => wrap.State));
        Assert.Equal(1, kiln.Programs.Updates("fuel_setup"));   // one step, one slice
        Assert.Equal((1L, 2L), (kiln.Run(() => wrap.Stats).Slices / 3, kiln.Run(() => wrap.Stats).Entities / 3));  // three stages, one slice of two
        kiln.As(mod, () => wrap.Dispose());
        kiln.Step(2);
        order.Clear();
        kiln.Step(1);
        Assert.Equal(["update 2"], order);
        Assert.Equal(WrapState.Removed, kiln.Run(() => wrap.State));
        _ = b;
    }

    [Fact]
    public void Per_entity_views_write_the_fake_world_mark_once_and_raise_one_event_per_entity()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("wrap-kit"));
        kiln.Step(1);  // the server starts and loads the mod
        kiln.Programs.Add("fuel_setup", FuelLists);
        var (a, b, other) = Containers(kiln);
        var events = new List<ComponentWrittenEvent>();
        using var sub = kiln.Run(() => Events.On<ComponentWrittenEvent>(e => events.Add(e)));
        var seen = new List<EntityId>();
        string? guard = null;
        var wrap = kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!.AfterEach((ref EntityView e) =>
        {
            seen.Add(e.Entity);
            var fuel = e.Attribute("Fuel");
            fuel[0] = fuel[1];       // full
            _ = e.Attribute("Fuel"); // a second use: marked once
            fuel.Changed();          // the kit has no formulas: it checks the block and changes nothing
            try { Systems.Wrap("fuel_setup"); } catch (InvalidOperationException x) { guard ??= x.Message; }
        }));
        kiln.Step(1);
        kiln.As(mod, () => wrap.Dispose());
        kiln.Step(3);
        Assert.Equal([a, b], seen);
        Assert.DoesNotContain(other, seen);
        var fa = kiln.Entities.Get<Ecs.Fuel>(a);
        var fb = kiln.Entities.Get<Ecs.Fuel>(b);
        Assert.Equal((100, 200), (fa.Values[0], fb.Values[0]));
        Assert.Equal([(a, "Fuel"), (b, "Fuel")], kiln.Programs.Marks.Select(m => (m.Entity, m.Component)));
        Assert.Contains("program wrap", guard);   // tick-thread APIs refuse inside a handler, also on the kit's one thread
        Assert.Equal(2, events.Count);
        Assert.All(events, e => Assert.Equal(("wrap-kit/wrap:fuel_setup", "Fuel"), (e.Writer, e.Component)));
        Assert.Equal(0, kiln.Run(() => wrap.Stats).Faults);
    }

    [Fact]
    public void One_Replace_per_program_runs_the_original_only_when_asked()
    {
        using var kiln = new KilnHarness();
        var one = kiln.Load<WrapMod>(new ModManifest("replace-one"));
        var two = kiln.Load<WrapMod>(new ModManifest("replace-two"));
        kiln.Step(1);
        var updates = 0;
        kiln.Programs.Add("fuel_setup", FuelLists, _ => updates++);
        Containers(kiln);
        var calls = 0;
        var w = kiln.As(one, () => Systems.Wrap("fuel_setup").Wrap!);
        var r = kiln.As(one, () => w.Replace((ref Slice s) => { if (++calls % 2 == 0) s.RunOriginal(); }));
        Assert.True(r.Ok, r.Reason);
        var second = kiln.As(two, () => Systems.Wrap("fuel_setup").Wrap!.Replace((ref Slice s) => { }));
        Assert.False(second.Ok);
        Assert.Contains("replace-one", second.Reason);
        kiln.Step(4);
        Assert.Equal(4, calls);
        Assert.Equal(2, updates);
        Assert.Equal(2, kiln.Programs.Updates("fuel_setup"));
    }

    [Fact]
    public void WhenArchetype_decides_by_the_entities_component_set()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("wrap-kit"));
        kiln.Step(1);  // the server starts and loads the mod
        kiln.Programs.Add("fuel_setup", FuelLists);
        var (a, _, _) = Containers(kiln);
        kiln.Entities.SetBytes(a, "SpawnTime", new byte[8]);   // another archetype: a slice of its own
        long with = 0, without = 0, all = 0;
        kiln.As(mod, () =>
        {
            Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) => all++);
            Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["SpawnTime"]).Before((ref Slice s) => with++);
            Systems.Wrap("fuel_setup").Wrap!.WhenArchetype(["Fuel"], ["SpawnTime"]).Before((ref Slice s) => without++);
            return 0;
        });
        kiln.Step(2);
        Assert.Equal((4L, 2L, 2L), (all, with, without));
    }

    [Fact]
    public void A_typed_wrap_of_a_Kiln_Game_view_is_made_and_binds_on_the_view_s_program()
    {
        using var kiln = new KilnHarness();
        var mod = kiln.Load<WrapMod>(new ModManifest("typed-kit"));
        kiln.Step(1);
        kiln.Programs.Add<Ecs.Programs.FuelSetup>();
        var e = kiln.Entities.Create(new WorldPosition(1, 2, 3));
        kiln.Entities.Set(e, Water(5, 50));
        // fuel_setup writes DynamicFuelSetup too: an entity of the program has it when the fake world knows the type
        if (TypesFake.IsComponent(WorldAbi.Hash("DynamicFuelSetup"))) kiln.Entities.SetBytes(e, "DynamicFuelSetup", new byte[16]);
        var values = new List<int>();
        var made = kiln.As(mod, () => Systems.Wrap<Ecs.Programs.FuelSetup>());
        Assert.True(made.Ok, made.Reason);
        kiln.As(mod, () => made.Wrap!.BeforeEach((ref Ecs.Programs.FuelSetup v) => values.Add(v.FuelValues[0])));
        kiln.Step(2);
        Assert.Equal([5, 5], values);
        Assert.Equal(0, kiln.Run(() => made.Wrap!.Stats).Faults);
    }

    // kiln.spawn refuses to create or destroy inside the world's step (a wrap stage, a Kiln program): the kit's table does
    // as the game's service does (WRONG_THREAD, for a native caller), Kiln.Spawn's message says how to defer the call,
    // and posted, the call runs after the step.
    [Fact]
    public unsafe void Spawn_inside_a_wrap_stage_is_refused_and_the_message_names_slice_Post()
    {
        using var kiln = new KilnHarness();
        kiln.Spawned.AddTemplate("Enemy_Vukah_Melee", TemplateKind.Creature, "Vukah");
        var mod = kiln.Load<WrapMod>(new ModManifest("spawn-mid-step")); kiln.Step(1);
        kiln.Programs.Add("fuel_setup", FuelLists); Containers(kiln);
        string? created = null, destroyed = null;
        var posted = 0;
        var tableInside = new List<int>();
        var victim = kiln.Entities.Create(new WorldPosition(9, 9, 9));
        var table = (SpawnAbi.Table*)KitBackend.Instance.Service("kiln.spawn", 1);
        kiln.As(mod, () => Systems.Wrap("fuel_setup").Wrap!.Before((ref Slice s) =>
        {
            uint id = 0;
            var request = new SpawnAbi.Request { Size = (uint)sizeof(SpawnAbi.Request) };
            tableInside.Add(table->Destroy(victim.Value));
            tableInside.Add(table->Spawn(&request, &id));
            try { Spawn.TryEntity("Enemy_Vukah_Melee", new WorldPosition(1, 2, 3)); }
            catch (InvalidOperationException e) { created ??= e.Message; }
            try { Spawn.Destroy(victim); }
            catch (InvalidOperationException e) { destroyed ??= e.Message; }
            if (posted == 0) s.Post(() => { if (Spawn.TryEntity("Enemy_Vukah_Melee", new WorldPosition(1, 2, 3)).Ok) posted++; });
        }));
        kiln.Step(3);
        Assert.NotNull(created);
        Assert.Contains("inside the world's step", created);
        Assert.Contains("slice.Post", created);
        Assert.NotNull(destroyed);
        Assert.Contains("slice.Post", destroyed);
        Assert.True(kiln.Entities.Exists(victim));
        Assert.Equal(1, posted);
        Assert.Single(kiln.Spawned.All);
        Assert.True(tableInside.Count >= 2 && tableInside.All(rc => rc == FakeTables.WrongThread),
            $"a native caller inside a stage gets WRONG_THREAD from the kit's kiln.spawn ({string.Join(",", tableInside)})");
        Assert.Equal(FakeTables.Ok, kiln.Run(() => table->Destroy(victim.Value)));  // after the step: allowed again
    }
}
