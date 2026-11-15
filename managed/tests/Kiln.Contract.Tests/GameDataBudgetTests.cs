// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Contract.Tests;

/// <summary>The buff budget and result-bearing buff definitions (package 17b S4, chapter 17 I1 and I5) on fake
/// kiln.gamedata tables: an older table names its own row, another registry is a bad argument before any native call,
/// and the native answers map to results without ever turning a refusal into room or success.</summary>
public sealed unsafe class GameDataBudgetTests : FakeTableTest
{
    private static int _calls;
    private static uint _readiness, _kiln;
    private static int _defineRc;
    private static uint _ok, _disposition, _slots, _code;
    private static string _report = "{\"reason\":null,\"warnings\":[]}";
    private static uint _timing;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeBudget(KilnCapacityInfo* o)
    {
        _calls++;
        var size = o->Size;
        *o = default;
        o->Size = size;
        o->Readiness = _readiness;
        o->Flags = KilnCapacityInfo.FlagProcess | KilnCapacityInfo.FlagExpanded | (_readiness == 1 ? KilnCapacityInfo.FlagReady : 0);
        o->WorldKind = KilnCapacityInfo.NoWorldKind;
        if (_readiness == 1)
        {
            o->Capacity = 4096;
            o->KilnUsed = _kiln;
        }
        o->Revision = 7;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeDefine(ulong owner, NativeStr json, uint timing, KilnDefinitionReply* r, byte* report, nuint cap, nuint* len)
    {
        _calls++;
        _timing = timing;
        if (cap < 4096)
        {
            *len = 4096;
            return 5;
        }
        var size = r->Size;
        *r = default;
        r->Size = size;
        r->Ok = _ok;
        r->Disposition = _disposition;
        r->SlotsConsumed = _slots;
        r->ReasonCode = _code;
        r->Kind = 1;
        r->RegistrationId[0] = 0x42;
        var g = Guid.Parse("615e6c08-7d7e-5de3-a6b3-7fa8f90bd2c1").ToByteArray();  // the native side copies the definition's GUID
        for (var i = 0; i < 16; i++) r->DefinitionGuid[i] = g[i];
        r->SnapshotRevision = 9;
        var bytes = Encoding.UTF8.GetBytes(_report);
        for (var i = 0; i < bytes.Length && i < (int)cap - 1; i++) report[i] = bytes[i];
        *len = (nuint)bytes.Length;
        return _defineRc;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeReceipt(ulong owner, byte* id, KilnDefinitionReply* r, byte* report, nuint cap, nuint* len)
    {
        _calls++;
        var size = r->Size;
        *r = default;
        r->Size = size;
        r->Ok = 0;
        r->ReasonCode = 9;  // NotFound
        r->Kind = 1;
        for (var i = 0; i < 16; i++) r->RegistrationId[i] = id[i];
        var bytes = Encoding.UTF8.GetBytes("{\"reason\":\"no buff definition of this process has this registration id\",\"warnings\":[]}");
        for (var i = 0; i < bytes.Length && i < (int)cap - 1; i++) report[i] = bytes[i];
        *len = (nuint)bytes.Length;
        return 0;
    }

    private static GameDataClone Clone() => new() { Guid = Guid.Parse("615e6c08-7d7e-5de3-a6b3-7fa8f90bd2c1"), Name = "Budget_Test", Base = "0x5f425faf" };

    private void ServeTable(bool appended)
    {
        _calls = 0;
        if (appended)
        {
            var t = Alloc<GameDataNative.BuffTable>();
            t->V1.Header = new ServiceHeader { Size = (uint)sizeof(GameDataNative.BuffTable), Version = GameDataNative.Version };
            t->BuffBudget = &FakeBudget;
            t->DefineBuffResult = &FakeDefine;
            t->BuffRegistration = &FakeReceipt;
            Serve(GameDataNative.Service, (GameDataNative.Table*)t, typeof(Content), "_gd");
        }
        else
        {
            var t = Alloc<GameDataNative.Table>();
            t->Header = new ServiceHeader { Size = (uint)sizeof(GameDataNative.Table), Version = GameDataNative.Version };
            Serve(GameDataNative.Service, t, typeof(Content), "_gd");
        }
        Swap(typeof(Content), "_gdTried", false);
    }

    [Fact]
    public void An_unused_buff_room_does_not_add_a_mandatory_content_join_requirement()
    {
        ServeTable(appended: true);
        _readiness = 1; _kiln = 0;
        Assert.False(ContentSync.RequiresBuffAdmission());
        _kiln = 1;
        Assert.True(ContentSync.RequiresBuffAdmission());
    }

    [Fact]
    public void The_appended_table_is_the_v1_table_and_three_pointers()
    {
        Assert.Equal(136, sizeof(GameDataNative.Table));
        Assert.Equal(160, sizeof(GameDataNative.BuffTable));
        Assert.Equal(136, (int)Marshal.OffsetOf<GameDataNative.BuffTable>(nameof(GameDataNative.BuffTable.BuffBudget)));
        Assert.Equal(152, (int)Marshal.OffsetOf<GameDataNative.BuffTable>(nameof(GameDataNative.BuffTable.BuffRegistration)));
        Assert.Equal(4096, GameDataNative.AnswerReport);
    }

    [Fact]
    public void An_older_kiln_gamedata_names_each_members_row()
    {
        ServeTable(appended: false);
        AssertUnavailable("gamedata.buff_budget", () => Content.Buffs.GetBudget());
        AssertUnavailable("gamedata.buff_registration", () => Content.Buffs.Define(Clone(), new DefinitionRegistrationOptions()));
        AssertUnavailable("gamedata.buff_registration", () => Content.Buffs.GetRegistration(Guid.NewGuid()));
    }

    [Fact]
    public void Another_registry_is_a_bad_argument_before_any_native_call()
    {
        ServeTable(appended: true);
        var e = Assert.Throws<ArgumentException>(() => Content.Perks.GetBudget());
        Assert.Contains("Perks", e.Message);
        Assert.Throws<ArgumentException>(() => Content.Templates.Define(Clone(), new DefinitionRegistrationOptions()));
        Assert.Throws<ArgumentException>(() => Content.Templates.GetRegistration(Guid.NewGuid()));
        Assert.Equal(0, _calls);
        Assert.Throws<ArgumentNullException>(() => Content.Buffs.Define(Clone(), null!));
        Assert.Throws<ArgumentException>(() => Content.Buffs.Define(Clone(), new DefinitionRegistrationOptions((DefinitionTiming)7)));
        Assert.Throws<ArgumentException>(() => Content.Buffs.GetRegistration(Guid.Empty));
        Assert.Equal(0, _calls);
    }

    [Fact]
    public void A_budget_maps_readiness_to_Ok_and_never_room_while_not_ready()
    {
        ServeTable(appended: true);
        _readiness = 1;
        _kiln = 96;
        var r = Content.Buffs.GetBudget();
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal(4096u, r.Budget.Capacity);
        Assert.Equal(4000u, r.Budget.Available);
        Assert.True(r.Budget.Expanded);
        Assert.Equal(CapacityScope.Process, r.Budget.Scope);
        _readiness = 0;
        r = Content.Buffs.GetBudget();
        Assert.False(r.Ok);
        Assert.Equal("NotReady", r.Reason);
        Assert.Null(r.Budget.Available);
        Assert.Equal(0u, r.Budget.Capacity);
    }

    [Fact]
    public void Define_answers_registered_refused_or_throws_for_a_malformed_definition()
    {
        ServeTable(appended: true);
        _defineRc = 0;
        (_ok, _disposition, _slots, _code) = (1, 1, 1, 0);
        _report = "{\"reason\":null,\"warnings\":[\"a note\"]}";
        var r = Content.Buffs.Define(Clone(), new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Assert.Equal(1u, _timing);
        Assert.True(r.Ok);
        Assert.Equal(DefinitionRegistrationState.Registered, r.State);
        Assert.Equal(1u, r.SlotsConsumed);
        Assert.Equal(DefinitionRegistrationKind.Buff, r.Kind);
        Assert.Equal(Guid.Parse("615e6c08-7d7e-5de3-a6b3-7fa8f90bd2c1"), r.Definition);
        Assert.Equal(["a note"], r.Warnings);

        (_ok, _disposition, _slots, _code) = (0, 0, 0, 1);
        _report = "{\"reason\":\"Kiln's buff collection is full (4096 clones)\",\"warnings\":[]}";
        r = Content.Buffs.Define(new JsonObject { ["guid"] = Guid.NewGuid().ToString(), ["name"] = "Full", ["base"] = "0x5f425faf" },
            new DefinitionRegistrationOptions());
        Assert.False(r.Ok);
        Assert.Equal(DefinitionRefusalCode.CapacityExceeded, r.Code);
        Assert.Equal("Kiln's buff collection is full (4096 clones)", r.Reason);
        Assert.Equal(0u, r.SlotsConsumed);

        (_ok, _disposition, _slots, _code) = (1, 2, 1, 0);  // "accepted for the next start" with a slot: not a valid answer
        r = Content.Buffs.Define(Clone(), new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
        Assert.False(r.Ok);

        _defineRc = 1;
        _report = "{\"reason\":\"\\\"name\\\": letters, digits, _ - . (at most 128)\",\"warnings\":[]}";
        var e = Assert.Throws<ArgumentException>(() => Content.Buffs.Define(new JsonObject { ["guid"] = Guid.NewGuid().ToString(), ["name"] = 7 },
            new DefinitionRegistrationOptions()));
        Assert.Contains("letters, digits", e.Message);
        _defineRc = 0;
    }

    [Fact]
    public void An_unknown_receipt_is_NotFound_not_an_exception()
    {
        ServeTable(appended: true);
        var id = Guid.NewGuid();
        var r = Content.Buffs.GetRegistration(id);
        Assert.False(r.Ok);
        Assert.Equal(DefinitionRefusalCode.NotFound, r.Code);
        Assert.Equal(id, r.RegistrationId);
        Assert.Contains("registration id", r.Reason);
    }

    [Fact]
    public void The_new_members_carry_their_thread_and_capability_attributes()
    {
        var budget = Method(typeof(GameDataRegistry), nameof(GameDataRegistry.GetBudget));
        Assert.NotNull(budget.GetCustomAttributes(typeof(AnyThreadAttribute), false).SingleOrDefault());
        Assert.Equal("gamedata.buff_budget", ((RequiresCapabilityAttribute)budget.GetCustomAttributes(typeof(RequiresCapabilityAttribute), false).Single()).Name);
        var define = Method(typeof(GameDataRegistry), nameof(GameDataRegistry.Define), typeof(GameDataClone), typeof(DefinitionRegistrationOptions));
        Assert.NotNull(define.GetCustomAttributes(typeof(TickThreadOnlyAttribute), false).SingleOrDefault());
        var receipt = Method(typeof(GameDataRegistry), nameof(GameDataRegistry.GetRegistration), typeof(Guid));
        Assert.NotNull(receipt.GetCustomAttributes(typeof(AnyThreadAttribute), false).SingleOrDefault());
    }
}
