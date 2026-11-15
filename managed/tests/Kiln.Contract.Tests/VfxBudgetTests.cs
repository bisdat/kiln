// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>The VFX resource budget and result-bearing VFX definitions (package 17a S2, chapter 17.1 §5, I1 and I5) on fake
/// kiln.vfx tables: an older or missing table names its own row, bad input throws before any native call, and the native
/// answers map to results without ever turning a refusal into room or success. The definition fake follows the native rule
/// (native/services/vfx/ledger.cpp): a VFX the start built registers once the game's table was read; anything else is
/// RegistryClosed under RequireRegistered or stored under AllowNextStart, unless a new VFX does not fit.</summary>
public sealed unsafe class VfxBudgetTests : FakeTableTest
{
    private static int _calls;
    private static uint _readiness = 1, _capacity = 2048, _game = 1575, _kiln, _reserved;
    private static int _budgetRc, _defineRc;
    private static string _refusal = "";
    private static bool _tableRead = true;
    private static uint _timing;
    private static ulong _owner;
    private static string _name = "", _json = "";
    private static readonly HashSet<Guid> Built = [];
    private static readonly HashSet<Guid> Accepted = [];
    private static readonly Dictionary<Guid, (uint Disposition, uint Slots, uint Code, string? Reason, Guid Definition)> Receipts = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeBudget(KilnCapacityInfo* o)
    {
        _calls++;
        if (_budgetRc != 0) return _budgetRc;
        var size = o->Size;
        *o = default;
        o->Size = size;
        o->Readiness = _readiness;
        o->Flags = KilnCapacityInfo.FlagProcess | (_readiness == 1 ? KilnCapacityInfo.FlagReady : 0);
        o->WorldKind = KilnCapacityInfo.NoWorldKind;
        if (_readiness == 1)
        {
            o->Capacity = _capacity;
            o->GameUsed = _game;
            o->KilnUsed = _kiln;
            o->Reserved = _reserved;
        }
        o->Revision = 11 + _kiln;
        return 0;
    }

    private static string Text(GameDataMirror.Str s) => Encoding.UTF8.GetString(s.Ptr, (int)s.Len);

    private static void Write(byte* report, nuint cap, nuint* len, string? reason)
    {
        var bytes = Encoding.UTF8.GetBytes(System.Text.Json.JsonSerializer.Serialize(new Dictionary<string, object?> { ["reason"] = reason, ["warnings"] = Array.Empty<string>() }));
        for (var i = 0; i < bytes.Length && i < (int)cap - 1; i++) report[i] = bytes[i];
        *len = (nuint)bytes.Length;
    }

    private static void Fill(KilnDefinitionReply* r, Guid id, Guid definition, uint disposition, uint slots, uint code)
    {
        var size = r->Size;
        *r = default;
        r->Size = size;
        r->Ok = code == 0 ? 1u : 0u;
        r->Disposition = disposition;
        r->SlotsConsumed = slots;
        r->ReasonCode = code;
        r->Kind = (uint)DefinitionRegistrationKind.Vfx;
        r->SnapshotRevision = 11 + _kiln;
        DefinitionRegistration.IdBytes(id).CopyTo(new Span<byte>(r->RegistrationId, 16));
        definition.ToByteArray().CopyTo(new Span<byte>(r->DefinitionGuid, 16));
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeDefine(ulong owner, GameDataMirror.Str name, GameDataMirror.Str json, uint timing, KilnDefinitionReply* r, byte* report,
        nuint cap, nuint* len)
    {
        _calls++;
        *len = 4096;
        if (cap < 4096) return 5;  // BUFFER_TOO_SMALL before anything is read
        _owner = owner;
        _timing = timing;
        _name = Text(name);
        _json = Text(json);
        if (_defineRc != 0)
        {
            Write(report, cap, len, _refusal);
            return _defineRc;
        }
        var doc = JsonNode.Parse(_json)!.AsObject();
        var mod = (string)doc["mod"]!;
        var guid = VfxId.Named(mod, _name).Guid;
        var id = DefinitionRegistration.DeriveId(mod, DefinitionRegistrationKind.Vfx, guid, SHA256.HashData(Encoding.UTF8.GetBytes(doc["block"]!.ToJsonString())));
        uint disposition = 0, slots = 0, code = 0;
        string? reason = null;
        if (Built.Contains(guid))
        {
            if (_tableRead) (disposition, slots) = (1, 1);
            else if (timing == 1) disposition = 2;
            else (code, reason) = (7, "built at this start; the game's VFX table is read at the first update of a client world");
        }
        else if (Accepted.Contains(guid))
        {
            if (timing == 1) disposition = 2;
            else (code, reason) = (5, "the game lists its VFX once, before mods load: this definition is stored for the next start");
        }
        else if (_game + _kiln + _reserved + (uint)Accepted.Count + 1 > _capacity)
        {
            (code, reason) = (1, $"the game's VFX table holds {_capacity} resources: this one does not fit");
        }
        else if (timing == 0)
        {
            (code, reason) = (5, "the game lists its VFX once, before mods load: a new VFX is there from the next start");
        }
        else
        {
            Accepted.Add(guid);
            disposition = 2;
        }
        Receipts[id] = (disposition, slots, code, reason, guid);
        Fill(r, id, guid, disposition, slots, code);
        Write(report, cap, len, reason);
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeReceipt(ulong owner, byte* id, KilnDefinitionReply* r, byte* report, nuint cap, nuint* len)
    {
        _calls++;
        *len = 4096;
        if (cap < 4096) return 5;
        var key = new Guid(new ReadOnlySpan<byte>(id, 16), bigEndian: true);
        if (Receipts.TryGetValue(key, out var x))
        {
            Fill(r, key, x.Definition, x.Disposition, x.Slots, x.Code);
            Write(report, cap, len, x.Reason);
        }
        else
        {
            Fill(r, key, Guid.Empty, 0, 0, 9);
            Write(report, cap, len, "no VFX definition of this process has this registration id");
        }
        return 0;
    }

    private static readonly VfxBlock Spark = new() { Particles = [new ParticleBlock { Burst = 8, Lifetime = 0.5f }] };

    private void ServeTable(bool appended)
    {
        _calls = 0;
        (_readiness, _capacity, _game, _kiln, _reserved, _budgetRc, _defineRc, _tableRead) = (1, 2048, 1575, 0, 0, 0, 0, true);
        Built.Clear();
        Accepted.Clear();
        Receipts.Clear();
        if (appended)
        {
            var t = Alloc<VfxNative.BudgetTable>();
            t->V1.Header = new ServiceHeader { Size = (uint)sizeof(VfxNative.BudgetTable), Version = VfxNative.Version };
            t->ResourceBudget = &FakeBudget;
            t->DefineVfxResult = &FakeDefine;
            t->VfxRegistration = &FakeReceipt;
            Serve(VfxNative.Name, (VfxNative.Table*)t, typeof(VfxNative), "_table");
        }
        else
        {
            var t = Alloc<VfxNative.Table>();
            t->Header = new ServiceHeader { Size = (uint)sizeof(VfxNative.Table), Version = VfxNative.Version };
            Serve(VfxNative.Name, t, typeof(VfxNative), "_table");
        }
        Swap(typeof(VfxNative), "_looked", 0);
    }

    private static string Name() => "spark-" + Guid.NewGuid().ToString("N")[..8];

    [Fact]
    public void The_appended_table_is_the_v1_table_and_three_pointers()
    {
        Assert.Equal(112, sizeof(VfxNative.Table));
        Assert.Equal(136, sizeof(VfxNative.BudgetTable));
        Assert.Equal(112, (int)Marshal.OffsetOf<VfxNative.BudgetTable>(nameof(VfxNative.BudgetTable.ResourceBudget)));
        Assert.Equal(120, (int)Marshal.OffsetOf<VfxNative.BudgetTable>(nameof(VfxNative.BudgetTable.DefineVfxResult)));
        Assert.Equal(128, (int)Marshal.OffsetOf<VfxNative.BudgetTable>(nameof(VfxNative.BudgetTable.VfxRegistration)));
        Assert.Equal(4096, VfxNative.AnswerReport);
    }

    [Fact]
    public void An_older_kiln_vfx_names_each_members_row_and_its_rows_say_older()
    {
        ServeTable(appended: false);
        AssertUnavailable("vfx.budget", () => _ = Vfx.ResourceBudget);
        AssertUnavailable("blocks.vfx_registration", () => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions()));
        AssertUnavailable("blocks.vfx_registration", () => Blocks.GetVfxRegistration(Guid.NewGuid()));
        Assert.Equal(0, _calls);
        foreach (var row in (string[])["vfx.budget", "blocks.vfx_registration"])
        {
            Assert.Equal(CapabilityState.Unavailable, Capabilities.Get(row).State);
            Assert.Equal(CapabilityReason.ServiceOlder, Capabilities.Get(row).Reason);
        }
    }

    [Fact]
    public void Without_kiln_vfx_the_rows_are_off_and_name_it()
    {
        ServeTable(appended: false);
        Swap(typeof(VfxNative), "_table", Box<VfxNative.Table>(null));
        Backend.Tables.Remove(VfxNative.Name);
        AssertUnavailable("vfx.budget", () => _ = Vfx.ResourceBudget);
        AssertUnavailable("blocks.vfx_registration", () => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart)));
        StartRows(typeof(Vfx).Assembly);
        StartRows(typeof(Blocks).Assembly);
        foreach (var row in (string[])["vfx.budget", "blocks.vfx_registration"])
        {
            var c = Capabilities.Get(row);
            Assert.Equal(CapabilityState.Unavailable, c.State);
            Assert.NotEqual(CapabilityReason.Unknown, c.Reason);
            Assert.StartsWith("needs kiln.vfx", c.ReasonText);
        }
    }

    [Fact]
    public void A_budget_maps_readiness_to_Ok_and_never_room_while_not_ready()
    {
        ServeTable(appended: true);
        _kiln = 12;
        var r = Vfx.ResourceBudget;
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal((2048u, 1575u, 12u, 0u), (r.Budget.Capacity, r.Budget.GameUsed, r.Budget.KilnUsed, r.Budget.Reserved));
        Assert.Equal(461u, r.Budget.Available);
        Assert.False(r.Budget.Expanded);
        Assert.Equal(CapacityScope.Process, r.Budget.Scope);
        _readiness = 0;
        r = Vfx.ResourceBudget;
        Assert.False(r.Ok);
        Assert.Equal("NotReady", r.Reason);
        Assert.Null(r.Budget.Available);
        Assert.Equal(0u, r.Budget.Capacity);
        // the native side says the member is off on this build: the capability's exception, not a budget
        _budgetRc = 3;
        AssertUnavailable("vfx.budget", () => _ = Vfx.ResourceBudget);
        _budgetRc = 7;
        Assert.Throws<InvalidOperationException>(() => _ = Vfx.ResourceBudget);
    }

    [Fact]
    public void Bad_arguments_throw_before_any_native_call()
    {
        ServeTable(appended: true);
        Assert.Throws<ArgumentNullException>(() => Blocks.Define(Name(), (VfxBlock)null!, new DefinitionRegistrationOptions()));
        Assert.Throws<ArgumentNullException>(() => Blocks.Define(Name(), Spark, null!));
        Assert.Throws<ArgumentException>(() => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions((DefinitionTiming)7)));
        Assert.Throws<ArgumentException>(() => Blocks.GetVfxRegistration(Guid.Empty));
        Assert.Throws<BlockError>(() => Blocks.Define(Name(), new VfxBlock(), new DefinitionRegistrationOptions()));  // nothing to draw
        Assert.Throws<BlockError>(() => Blocks.Define("bad name", Spark, new DefinitionRegistrationOptions()));
        Assert.Equal(0, _calls);
        Backend.TickThread = false;
        Assert.Throws<InvalidOperationException>(() => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions()));
        Backend.TickThread = true;
        Assert.Equal(0, _calls);
    }

    [Fact]
    public void A_built_vfx_is_registered_and_the_block_is_the_mods()
    {
        ServeTable(appended: true);
        var name = Name();
        Built.Add(VfxId.Named("writer", name).Guid);
        var r = Blocks.Define(name, Spark, new DefinitionRegistrationOptions());
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(DefinitionRegistrationState.Registered, r.State);
        Assert.Equal(1u, r.SlotsConsumed);
        Assert.Equal(DefinitionRegistrationKind.Vfx, r.Kind);
        Assert.Equal(VfxId.Named("writer", name).Guid, r.Definition);
        Assert.Equal(0u, _timing);
        Assert.Equal(77ul, _owner);
        Assert.Equal(name, _name);
        var doc = JsonNode.Parse(_json)!.AsObject();
        Assert.Equal("writer", (string?)doc["mod"]);
        Assert.Equal(1, (int)doc["format"]!);
        Assert.NotNull(doc["block"]!["particles"]);
        var handle = Assert.IsType<VfxBlockHandle>(Blocks.Find(name, "writer"));
        Assert.Equal(VfxBlockStatus.Registered, handle.Status);
        // its receipt reads the same
        var again = Blocks.GetVfxRegistration(r.RegistrationId);
        Assert.Equal(DefinitionRegistrationState.Registered, again.State);
        Assert.Equal(r.RegistrationId, again.RegistrationId);
    }

    [Fact]
    public void A_late_new_vfx_is_RegistryClosed_under_RequireRegistered_and_AcceptedNextStart_with_no_slot_under_AllowNextStart()
    {
        ServeTable(appended: true);
        var name = Name();
        var closed = Blocks.Define(name, Spark, new DefinitionRegistrationOptions(DefinitionTiming.RequireRegistered));
        Assert.False(closed.Ok);
        Assert.Equal(DefinitionRegistrationState.Refused, closed.State);
        Assert.Equal(DefinitionRefusalCode.RegistryClosed, closed.Code);
        Assert.Equal(0u, closed.SlotsConsumed);
        Assert.Contains("next start", closed.Reason);
        Assert.Null(Blocks.Find(name, "writer"));  // a refusal leaves nothing
        var later = Blocks.Define(name, Spark, new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Assert.True(later.Ok);
        Assert.Equal(DefinitionRegistrationState.AcceptedNextStart, later.State);
        Assert.Equal(0u, later.SlotsConsumed);
        Assert.Equal(1u, _timing);
        Assert.Equal(VfxBlockStatus.Pending, Assert.IsType<VfxBlockHandle>(Blocks.Find(name, "writer")).Status);
        // built, the game's table not read yet: NotReady under RequireRegistered
        var built = Name();
        Built.Add(VfxId.Named("writer", built).Guid);
        _tableRead = false;
        var notYet = Blocks.Define(built, Spark, new DefinitionRegistrationOptions());
        Assert.Equal(DefinitionRefusalCode.NotReady, notYet.Code);
        Assert.False(notYet.Ok);
    }

    [Fact]
    public void The_474th_new_vfx_is_CapacityExceeded_and_never_stored()
    {
        ServeTable(appended: true);
        _capacity = 1576 + 2;  // room for two
        var a = Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        var b = Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Assert.True(a.Ok && b.Ok);
        _kiln = 1;
        var full = Name();
        var c = Blocks.Define(full, Spark, new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Assert.False(c.Ok);
        Assert.Equal(DefinitionRefusalCode.CapacityExceeded, c.Code);
        Assert.Equal(DefinitionRegistrationState.Refused, c.State);
        Assert.Equal(0u, c.SlotsConsumed);
        Assert.Null(Blocks.Find(full, "writer"));
    }

    [Fact]
    public void A_block_the_builder_refuses_throws_BlockError_and_an_off_member_names_its_row()
    {
        ServeTable(appended: true);
        _defineRc = 1;
        _refusal = "writer: code: vfx.broken.particles[0].maxCount: 4097 is outside 1..4096";
        var e = Assert.Throws<BlockError>(() => Blocks.Define("broken", Spark, new DefinitionRegistrationOptions()));
        Assert.Equal("particles[0].maxCount", e.Field);
        Assert.Contains("4097 is outside", e.Problem);
        _defineRc = 3;
        _refusal = "blocks.vfx_registration is off in this process";
        AssertUnavailable("blocks.vfx_registration", () => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions()));
        _defineRc = 7;
        Assert.Throws<InvalidOperationException>(() => Blocks.Define(Name(), Spark, new DefinitionRegistrationOptions()));
    }

    [Fact]
    public void An_unknown_receipt_is_NotFound_not_an_exception()
    {
        ServeTable(appended: true);
        var id = Guid.NewGuid();
        var r = Blocks.GetVfxRegistration(id);
        Assert.False(r.Ok);
        Assert.Equal(DefinitionRefusalCode.NotFound, r.Code);
        Assert.Equal(id, r.RegistrationId);
        Assert.Contains("registration id", r.Reason);
    }

    [Fact]
    public void A_definition_posts_its_change_and_the_budget_its_own()
    {
        ServeTable(appended: true);
        _ = Vfx.ResourceBudget;  // the budget a later change is measured from (a read with nobody listening posts nothing)
        var registrations = new List<DefinitionRegistrationChangedEvent>();
        var budgets = new List<CapacityChangedEvent>();
        using var s1 = Events.On<DefinitionRegistrationChangedEvent>(registrations.Add);
        using var s2 = Events.On<CapacityChangedEvent>(budgets.Add);
        var name = Name();
        Built.Add(VfxId.Named("writer", name).Guid);
        _kiln = 1;
        var r = Blocks.Define(name, Spark, new DefinitionRegistrationOptions());
        Step();
        var ev = Assert.Single(registrations);
        Assert.Equal("writer", ev.ModId);
        Assert.Null(ev.PreviousState);
        Assert.Equal(DefinitionRegistrationState.Registered, ev.Current.State);
        var cap = Assert.Single(budgets);
        Assert.Equal("vfx.budget", cap.Quota);
        Assert.Equal(1u, cap.Current.KilnUsed);
        // the same definition again: nothing new
        Assert.Equal(r.RegistrationId, Blocks.Define(name, Spark, new DefinitionRegistrationOptions()).RegistrationId);
        Step();
        Assert.Single(registrations);
    }

    [Fact]
    public void The_new_members_carry_their_side_thread_and_capability_attributes()
    {
        var budget = Property(typeof(Vfx), nameof(Vfx.ResourceBudget));
        AssertHas<ClientOnlyAttribute>(budget);
        AssertHas<AnyThreadAttribute>(budget);
        AssertRequires(budget, "vfx.budget");
        var define = Method(typeof(Blocks), nameof(Blocks.Define), typeof(string), typeof(VfxBlock), typeof(DefinitionRegistrationOptions));
        AssertHas<ClientOnlyAttribute>(define);
        AssertHas<TickThreadOnlyAttribute>(define);
        AssertRequires(define, "blocks.vfx_registration");
        var receipt = Method(typeof(Blocks), nameof(Blocks.GetVfxRegistration), typeof(Guid));
        AssertHas<ClientOnlyAttribute>(receipt);
        AssertHas<AnyThreadAttribute>(receipt);
        AssertRequires(receipt, "blocks.vfx_registration");
    }
}
