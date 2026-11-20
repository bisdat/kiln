// SPDX-License-Identifier: MIT
// Systems.Define over the fake kiln.world table's boot reservations (programs v2): what it binds, what it refuses, how a
// reservation that does not match the [Query] is reported.
using Xunit;

namespace Kiln.SystemsTests;

[Query(Template = ["FuelSetup"], Write = ["Fuel"], ReadOptional = ["CurrentTransform"], Without = ["DynamicShelter"], HostOnly = true)]
public ref struct Barrels : IQuery<Barrels>
{
    public EcsProgramContext Context;
    public static Barrels Bind(EcsProgramContext context) => new() { Context = context };
}

[Query(Key = "my_wells", Read = ["Fuel"])]
public ref struct Wells : IQuery<Wells>
{
    public EntityId Entity;
    public static Wells Bind(EcsProgramContext context) => new() { Entity = context.Entity };
}

public ref struct Unmarked : IQuery<Unmarked>
{
    public static Unmarked Bind(EcsProgramContext context) => default;
}

[Collection("kiln.world fake")]
public sealed unsafe class DefineTests
{
    private const string BarrelsKey = "Kiln_SystemsTests_Barrels";

    private static ModContext NewMod(string id, ulong? owner = null) =>
        HostAccess.CreateContext(new ModInfo(id, id, "1.0.0", Path.GetTempPath()) { Side = ModSide.Server }, owner ?? (60 + (ulong)id.Length), Path.GetTempPath());

    private static T InMod<T>(ModContext mod, Func<T> f)
    {
        var previous = ModContext.CurrentMod;
        ModContext.CurrentMod = mod;
        try { return f(); }
        finally { ModContext.CurrentMod = previous; }
    }

    private static FakeWorld World(bool define = true) =>
        new(n => n is "systems.wraps" or "systems.wraps.typed" || (define && n == "systems.define"));

    private static void Reserve(FakeWorld w, string mod, string key)
    {
        w.Reserved.Add((mod, key));
        w.ReservedSlots[0] = ["FuelSetup"];
        w.ReservedSlots[1] = ["CurrentTransform"];
        w.ReservedSlots[2] = ["Fuel"];
    }

    [Theory]
    [InlineData("without=DynamicShelter", "without=StaticTransform", "without")]
    [InlineData("readOptional=CurrentTransform", "read=CurrentTransform", "read")]
    [InlineData("flags=00000010", "flags=00000090", "flags")]
    [InlineData("flags=00000010", "flags=00000410", "flags")]
    [InlineData("interval=1", "interval=2", "interval")]
    [InlineData("with=", "with=Fuel", "with")]
    [InlineData("anyOf=", "anyOf=Fuel", "anyOf")]
    // "world" is part of the reservation (0.2.0-dev.29): the same lists reserved for a client's worlds are not this program
    [InlineData("interval=1\n", "interval=1\nworld=client\n", "world")]
    [InlineData("interval=1\n", "interval=1\nworld=both\n", "world")]
    [InlineData("interval=1\n", "interval=1\nworld=prediction\n", "world")]
    public void Changed_boot_declaration_is_refused_before_publication(string old, string changed, string difference)
    {
        using var w = World();
        var mod = NewMod("changed");
        Reserve(w, "changed", BarrelsKey);
        w.ReservationOverride = FakeWorld.Snapshot(true).Replace(old, changed);
        var r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { }));
        Assert.False(r.Ok);
        Assert.Contains(difference, r.Reason);
        Assert.Contains("rebuild is in the game only after a server restart: this reservation was made at boot from the earlier declaration", r.Reason);
        Assert.Equal(0, w.Binds);
    }

    [Theory]
    [InlineData(0u)]
    [InlineData(16u)]
    public void Missing_reservation_member_never_binds_unchecked(uint size)
    {
        using var w = World();
        w.SetReservationSize(size);
        var mod = NewMod("old-core");
        Reserve(w, "old-core", BarrelsKey);
        var e = Assert.Throws<KilnUnavailableException>(() => InMod(mod, () => Systems.Define(BarrelsKey,
            new ProgramOptions { HostOnly = true }, (ref Barrels b) => { })));
        Assert.Equal("systems.define", e.Capability);
        Assert.Contains("program_reservation", e.Message);
        Assert.Equal(0, w.Binds);
    }

    [Fact]
    public void Unchanged_query_binds_after_dispose_and_reload()
    {
        using var w = World();
        w.Reserved.Add(("reload", "my_wells")); w.ReservedSlots[1] = ["Fuel"];
        var first = NewMod("reload");
        var calls = 0;
        var a = InMod(first, () => Systems.Define("my_wells", new ProgramOptions(), (ref Wells b) => calls++));
        Assert.True(a.Ok);
        first.Retire();
        Assert.Equal(1, w.Unbinds);
        w.RunDefined(); Assert.Equal(0, calls);
        a.Dispose(); // already drained by retirement
        var b = InMod(NewMod("reload", 999), () => Systems.Define("my_wells", new ProgramOptions(), (ref Wells b) => calls += 10));
        Assert.True(b.Ok);
        w.RunDefined(); Assert.Equal(10 * w.EntityIds.Length, calls);
        b.Dispose();
        Assert.Equal(2, w.Binds);
    }

    [Fact]
    public void Defined_handler_refuses_tick_APIs_even_on_world_thread_and_restores_guard()
    {
        using var w = World();
        var mod = NewMod("define-guard");
        w.Reserved.Add(("define-guard", "my_wells"));
        w.ReservedSlots[1] = ["Fuel"];
        var blocked = 0;
        var r = InMod(mod, () => Systems.Define("my_wells", new ProgramOptions(), (ref Wells e) =>
        {
            try { Services.RequireTickThread("test mutation"); }
            catch (InvalidOperationException) { blocked++; }
        }));
        Assert.True(r.Ok);
        w.RunDefined();
        Assert.Equal(w.EntityIds.Length, blocked);
        Services.RequireTickThread("after callback");
        r.Dispose();
    }

    [Fact]
    public void Define_binds_its_query_s_boot_reservation_and_runs_once_per_entity_with_the_typed_query()
    {
        using var w = World();
        var mod = NewMod("define-a");
        Reserve(w, "define-a", BarrelsKey);
        var seen = new List<uint>();
        var r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true },
            (ref Barrels b) => seen.Add(b.Context.Entity.Value)));
        Assert.True(r.Ok, r.Reason);
        Assert.Null(r.Reason);
        Assert.Equal(BarrelsKey, r.Key);
        w.RunDefined();
        Assert.Equal(w.EntityIds, seen);
        r.Dispose();
        Assert.Equal(1, w.Unbinds);
        r.Dispose();  // once
        Assert.Equal(1, w.Unbinds);
    }

    [Fact]
    public void A_query_s_key_is_its_Key_or_its_full_name_with_underscores()
    {
        using var w = World();
        var mod = NewMod("define-b");
        w.Reserved.Add(("define-b", "my_wells"));
        w.ReservedSlots[1] = ["Fuel"];
        uint first = 0;
        var r = InMod(mod, () => Systems.Define("my_wells", new ProgramOptions(), (ref Wells e) => { if (first == 0) first = e.Entity.Value; }));
        Assert.True(r.Ok, r.Reason);
        w.RunDefined();
        Assert.Equal(w.EntityIds[0], first);
    }

    [Fact]
    public void A_key_kiln_world_did_not_reserve_at_boot_is_Ok_false_and_says_rebuild_and_restart()
    {
        using var w = World();
        var mod = NewMod("define-c");  // nothing reserved for it
        var r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { }));
        Assert.False(r.Ok);
        Assert.Equal("not reserved at boot: rebuild the mod and restart the server", r.Reason);
        w.BindResult = SystemsNative.InvalidArgument;  // another binding holds it
        Reserve(w, "define-c", BarrelsKey);
        r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { }));
        Assert.False(r.Ok);
        Assert.Contains("another binding", r.Reason);
    }

    [Fact]
    public void A_reservation_whose_lists_are_not_the_query_s_throws_and_is_unbound_again()
    {
        using var w = World();
        var mod = NewMod("define-d");
        Reserve(w, "define-d", BarrelsKey);
        w.ReservedSlots[2] = ["Health"];  // the running reservation writes Health, the [Query] Fuel: built from other sources
        var r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { }));
        Assert.False(r.Ok);
        Assert.Contains("write", r.Reason);
        Assert.Contains("server restart", r.Reason);
        Assert.Equal(0, w.Binds);
        Assert.Equal(0, w.Unbinds);
    }

    [Fact]
    public void Wrong_keys_options_queries_and_arguments_throw()
    {
        using var w = World();
        var mod = NewMod("define-e");
        Reserve(w, "define-e", BarrelsKey);
        var key = Assert.Throws<ArgumentException>(() =>
            InMod(mod, () => Systems.Define("barrels", new ProgramOptions { HostOnly = true }, (ref Barrels b) => { })));
        Assert.Contains($"reserved at boot as '{BarrelsKey}'", key.Message);
        var flags = Assert.Throws<ArgumentException>(() =>
            InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions(), (ref Barrels b) => { })));
        Assert.Contains("hostOnly", flags.Message);
        Assert.Throws<ArgumentException>(() => InMod(mod, () => Systems.Define("Unmarked", new ProgramOptions(), (ref Unmarked u) => { })));
        Assert.Throws<ArgumentException>(() => InMod(mod, () => Systems.Define("", new ProgramOptions(), (ref Barrels b) => { })));
        Assert.Throws<ArgumentNullException>(() => InMod(mod, () => Systems.Define<Barrels>(BarrelsKey, new ProgramOptions { HostOnly = true }, null!)));
        Assert.Equal(0, w.Unbinds);
        Assert.True(w.Bound == null);
    }

    [Fact]
    public void Define_throws_when_systems_define_is_off_and_off_the_tick_thread()
    {
        using var w = World(define: false);
        var mod = NewMod("define-f");
        Reserve(w, "define-f", BarrelsKey);
        var off = Assert.Throws<KilnUnavailableException>(() =>
            InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { })));
        Assert.Equal("systems.define", off.Capability);
        ProgramRegistration? r = null;
        Exception? thrown = null;
        var t = new Thread(() =>
        {
            try { r = InMod(mod, () => Systems.Define(BarrelsKey, new ProgramOptions { HostOnly = true }, (ref Barrels b) => { })); }
            catch (Exception e) { thrown = e; }
        });
        t.Start();
        t.Join();
        Assert.Null(r);
        Assert.IsType<InvalidOperationException>(thrown);
    }
}
