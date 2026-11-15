// SPDX-License-Identifier: MIT
// World.GetProgramBudget (package 17b S6, chapter 17.4 I3) over fake kiln.world.budget tables: the mirror is the
// header's 152 bytes, the prediction kind is a bad argument before any native call, a missing table names its row, a
// world that is not there or not read yet promises nothing, and a reply that breaks the contract is never room.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.WorldTests;

[Collection("kiln.world backend")]
public sealed unsafe class ProgramBudgetTests
{
    private sealed class BudgetBackend(nint table) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion) => name == ProgramBudgetNative.Service && table != 0 ? table : 0;
    }

    private static int _calls;
    private static uint _readiness, _generation;
    private static bool _tokenless;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Fake(uint kind, ProgramBudgetNative.Reply* r)
    {
        _calls++;
        var size = r->Size;
        *r = default;
        r->Size = size;
        r->WorldKind = kind;
        r->Readiness = _readiness;
        r->Generation = _tokenless ? 0 : _generation;
        r->PerFileMax = 32;
        r->Access = new ProgramBudgetNative.AccessLimits
        {
            Templates = 8, ReadWrite = 42, SingletonReads = 64, SingletonWrites = 64, OtherTemplates = 9, OtherAccessors = 92,
            EventsInOut = 24, IteratorBytes = 4096,
        };
        Fill(ref r->KilnSlots, 64, 0, 15, 2, KilnCapacityInfo.FlagProcess | KilnCapacityInfo.FlagExpanded, KilnCapacityInfo.NoWorldKind, 0);
        Fill(ref r->EngineSlots, 896, 817, 13, 0, KilnCapacityInfo.FlagWorld, kind, _generation);
        return 0;
    }

    private static void Fill(ref KilnCapacityInfo i, uint cap, uint game, uint kiln, uint reserved, uint scope, uint kind, uint gen)
    {
        i.Size = (uint)sizeof(KilnCapacityInfo);
        i.WorldKind = kind;
        i.Generation = gen;
        i.Revision = 5;
        i.Readiness = _readiness;
        i.Flags = scope;
        if (_readiness != 1) return;
        i.Flags |= KilnCapacityInfo.FlagReady;
        (i.Capacity, i.GameUsed, i.KilnUsed, i.Reserved) = (cap, game, kiln, reserved);
    }

    private static T With<T>(bool table, Func<T> body)
    {
        var t = (ProgramBudgetNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(ProgramBudgetNative.Table));
        t->Header = new ServiceHeader { Size = (uint)sizeof(ProgramBudgetNative.Table), Version = ProgramBudgetNative.Version };
        t->ProgramBudget = &Fake;
        var old = Backend.Current;
        Backend.Current = new BudgetBackend(table ? (nint)t : 0);
        ProgramBudgetNative.Reset();
        _calls = 0;
        try
        {
            return body();
        }
        finally
        {
            Backend.Current = old;
            ProgramBudgetNative.Reset();
        }
    }

    [Fact]
    public void The_mirror_is_the_headers_152_bytes()
    {
        Assert.Equal(152, sizeof(ProgramBudgetNative.Reply));
        Assert.Equal(24, (int)Marshal.OffsetOf<ProgramBudgetNative.Reply>(nameof(ProgramBudgetNative.Reply.Access)));
        Assert.Equal(56, (int)Marshal.OffsetOf<ProgramBudgetNative.Reply>(nameof(ProgramBudgetNative.Reply.KilnSlots)));
        Assert.Equal(104, (int)Marshal.OffsetOf<ProgramBudgetNative.Reply>(nameof(ProgramBudgetNative.Reply.EngineSlots)));
        Assert.Equal(32, sizeof(ProgramBudgetNative.AccessLimits));
        Assert.Equal(16, sizeof(ProgramBudgetNative.Table));
    }

    [Fact]
    public void The_prediction_kind_is_a_bad_argument_and_a_missing_table_names_its_row()
    {
        With(true, () =>
        {
            Assert.Throws<ArgumentException>(() => World.GetProgramBudget((CapacityWorldKind)1));
            Assert.Equal(0, _calls);
            return 0;
        });
        With(false, () =>
        {
            Assert.Equal("world.program_budget", Assert.Throws<KilnUnavailableException>(() => World.GetProgramBudget()).Capability);
            return 0;
        });
    }

    [Fact]
    public void A_ready_pair_maps_both_budgets_and_the_world()
    {
        _readiness = 1;
        _generation = 3;
        _tokenless = false;
        var r = With(true, () => World.GetProgramBudget());
        Assert.True(r.Ok);
        Assert.Null(r.Reason);
        Assert.Equal(new CapacityWorldToken(CapacityWorldKind.Server, 3), r.World);
        Assert.Equal((64u, 15u, 2u, 47u), (r.KilnSlots.Capacity, r.KilnSlots.KilnUsed, r.KilnSlots.Reserved, r.KilnSlots.Available!.Value));
        Assert.True(r.KilnSlots.Expanded);
        Assert.Equal(CapacityScope.Process, r.KilnSlots.Scope);
        Assert.Equal((896u, 817u, 13u, 66u), (r.EngineSlots.Capacity, r.EngineSlots.GameUsed, r.EngineSlots.KilnUsed, r.EngineSlots.Available!.Value));
        Assert.Equal(CapacityScope.World, r.EngineSlots.Scope);
        Assert.Equal(32u, r.PerFileMax);
        Assert.Equal(new ProgramAccessLimits(8, 42, 64, 64, 9, 92, 24, 4096), r.AccessLimits);
    }

    [Fact]
    public void No_world_and_not_read_yet_promise_nothing_but_keep_the_limits()
    {
        _readiness = 2;  // NoWorld
        _generation = 0;
        _tokenless = false;
        var r = With(true, () => World.GetProgramBudget(CapacityWorldKind.Client));
        Assert.False(r.Ok);
        Assert.Equal("NoWorld", r.Reason);
        Assert.Null(r.World);
        Assert.Null(r.KilnSlots.Available);
        Assert.Null(r.EngineSlots.Available);
        Assert.Equal(0u, r.KilnSlots.Capacity + r.EngineSlots.Capacity);
        Assert.Equal(32u, r.PerFileMax);
        Assert.Equal(4096u, r.AccessLimits.IteratorBytes);
        _readiness = 0;  // NotReady, the live world named
        _generation = 4;
        r = With(true, () => World.GetProgramBudget());
        Assert.False(r.Ok);
        Assert.Equal("NotReady", r.Reason);
        Assert.Equal(new CapacityWorldToken(CapacityWorldKind.Server, 4), r.World);
        Assert.Null(r.EngineSlots.Available);
    }

    [Fact]
    public void A_ready_reply_without_its_world_is_a_mismatch_never_room()
    {
        _readiness = 1;
        _generation = 3;
        _tokenless = true;
        var r = With(true, () => World.GetProgramBudget());
        _tokenless = false;
        Assert.False(r.Ok);
        Assert.Equal("AccountingMismatch", r.Reason);
        Assert.Null(r.KilnSlots.Available);
        Assert.Null(r.EngineSlots.Available);
    }

    [Fact]
    public void GetProgramBudget_is_any_thread_and_names_its_capability()
    {
        var m = typeof(World).GetMethod(nameof(World.GetProgramBudget))!;
        Assert.NotNull(m.GetCustomAttributes(typeof(AnyThreadAttribute), false).SingleOrDefault());
        Assert.Equal("world.program_budget", ((RequiresCapabilityAttribute)m.GetCustomAttributes(typeof(RequiresCapabilityAttribute), false).Single()).Name);
    }
}
