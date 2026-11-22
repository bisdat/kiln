// SPDX-License-Identifier: MIT
// GameEvents.Each/Post and GameTypes.Find over a fake kiln.types with a game-sized event list (#114, thanks Fuzen777):
// the same results as before (by short or qualified name, or by a type resolved once), an unknown name and a type that
// is not an event refused, and the per-call cost pinned by counting the native calls: a name is resolved once, the
// event list is read once per table, and a step without events costs one EventEach. Before the fix every Each read the
// whole event list again (2,000 EventTypeAt calls and as many Services.Get per call here).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Api.Tests;

public sealed unsafe class GameEventLookupTests : IDisposable
{
    const int Ok = 0, NotFound = 2, Fillers = 2000;
    const int Int32Row = 0, HealthRow = 1, DepositRow = 2;
    const uint DepositHash = 0xD0D0;

    sealed class Backend : IKilnBackend
    {
        public nint Types, World;
        public int Services;
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public void Log(ulong owner, LogLevel level, string text) { }
        public nint Service(string name, uint minVersion)
        {
            Services++;
            return name switch { "kiln.types" => Types, "kiln.world" => World, _ => 0 };
        }
    }

    // the counters and the fake's state (static: the fake's functions are UnmanagedCallersOnly)
    static int _byName, _eventCount, _eventAt, _info, _field, _each, _posts, _postValue;
    static bool _off;
    static int _deliver;  // events EventEach delivers for the deposit event
    static string[] _names = [];
    static readonly Dictionary<string, int> ByName = new(StringComparer.Ordinal);
    static nint _base;
    static (nint Ptr, nuint Len)[] _short = [], _qualified = [];
    static (nint Ptr, nuint Len) _fieldName;
    static readonly List<nint> Allocations = [];

    readonly Backend _backend = new();
    readonly IKilnBackend _previous = Kiln.Backend.Current;

    public GameEventLookupTests()
    {
        _byName = _eventCount = _eventAt = _info = _field = _each = _posts = _postValue = 0;
        _off = false;
        _deliver = 0;
        _names = ["keen::int32", "keen::ecs::Health", "keen::ecs::AreaDepositEvent",
                  .. Enumerable.Range(0, Fillers).Select(i => $"keen::ecs::FillerEvent{i}")];
        ByName.Clear();
        for (var i = 0; i < _names.Length; i++) ByName[_names[i]] = i;
        _base = Alloc(_names.Length);
        _short = _names.Select(n => Utf8(n[(n.LastIndexOf("::", StringComparison.Ordinal) + 2)..])).ToArray();
        _qualified = _names.Select(Utf8).ToArray();
        _fieldName = Utf8("amount");
        GameTypes.ResetImpactIndex();
        Publish();
        Kiln.Backend.Current = _backend;
    }

    public void Dispose()
    {
        var cache = (Dictionary<nint, GameType>)typeof(GameTypes).GetField("Cache", System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic)!.GetValue(null)!;
        lock (cache) for (var i = 0; i < _names.Length; i++) cache.Remove(_base + i);
        GameTypes.ResetImpactIndex();
        Kiln.Backend.Current = _previous;
        foreach (var a in Allocations) Marshal.FreeHGlobal(a);
        Allocations.Clear();
    }

    static nint Alloc(int bytes)
    {
        var p = Marshal.AllocHGlobal(Math.Max(1, bytes));
        new Span<byte>((void*)p, Math.Max(1, bytes)).Clear();
        Allocations.Add(p);
        return p;
    }

    static (nint, nuint) Utf8(string s)
    {
        var bytes = System.Text.Encoding.UTF8.GetBytes(s);
        var p = Alloc(bytes.Length);
        Marshal.Copy(bytes, 0, p, bytes.Length);
        return (p, (nuint)bytes.Length);
    }

    void Publish()
    {
        var t = (TypesNative.Table*)Alloc(sizeof(TypesNative.Table));
        t->Header = new ServiceHeader { Size = (uint)sizeof(TypesNative.Table), Version = 1 };
        t->TypeCount = &TypeCount;
        t->TypeAt = &TypeAt;
        t->TypeByName = &TypeByName;
        t->TypeInfo = &TypeInfo;
        t->TypeField = &TypeField;
        t->EventTypeCount = &EventTypeCount;
        t->EventTypeAt = &EventTypeAt;
        t->EventEach = &EventEach;
        t->EventPost = &EventPost;
        _backend.Types = (nint)t;
        var w = (WorldPrefix*)Alloc(sizeof(WorldPrefix));
        w->Header = new ServiceHeader { Size = (uint)sizeof(WorldPrefix), Version = 1 };
        w->ServerWorld = &ServerWorld;
        _backend.World = (nint)w;
    }

    static int Row(void* handle)
    {
        var i = (nint)handle - _base;
        return i >= 0 && i < _names.Length ? (int)i : -1;
    }

    static bool IsEventRow(int i) => i >= DepositRow;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static uint TypeCount() => _off ? 0 : (uint)_names.Length;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static void* TypeAt(uint i) => !_off && i < _names.Length ? (void*)(_base + (nint)i) : null;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static void* TypeByName(NativeStr name)
    {
        _byName++;
        if (_off) return null;
        var n = name.ToString();
        if (ByName.TryGetValue(n, out var i) || (!n.Contains("::", StringComparison.Ordinal) && ByName.TryGetValue("keen::ecs::" + n, out i)))
            return (void*)(_base + i);
        return null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int TypeInfo(void* handle, TypesNative.TypeInfo* info)
    {
        _info++;
        var i = Row(handle);
        if (i < 0) return NotFound;
        info->Index = (uint)i;
        info->Name = new NativeStr { Ptr = (byte*)_short[i].Ptr, Len = _short[i].Len };
        info->QualifiedName = new NativeStr { Ptr = (byte*)_qualified[i].Ptr, Len = _qualified[i].Len };
        info->TypeSize = 4;
        info->Alignment = 4;
        info->Kind = (byte)(i == Int32Row ? GameTypeKind.S32 : GameTypeKind.Struct);
        info->Hash = i == DepositRow ? DepositHash : (uint)i + 1;
        info->FieldCount = i is HealthRow or DepositRow ? 1u : 0u;
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int TypeField(void* handle, uint index, TypesNative.TypeField* field)
    {
        _field++;
        var i = Row(handle);
        if (i is not (HealthRow or DepositRow) || index != 0) return NotFound;
        field->Name = new NativeStr { Ptr = (byte*)_fieldName.Ptr, Len = _fieldName.Len };
        field->Type = (void*)(_base + Int32Row);
        field->Offset = 0;
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static uint EventTypeCount()
    {
        _eventCount++;
        return _off ? 0 : (uint)(_names.Length - DepositRow);
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static void* EventTypeAt(uint i)
    {
        _eventAt++;
        return !_off && i < _names.Length - DepositRow ? (void*)(_base + DepositRow + (nint)i) : null;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int EventEach(nuint world, uint hash, delegate* unmanaged[Cdecl]<void*, uint, void*, void> fn, void* user)
    {
        _each++;
        if (hash != DepositHash) return Ok;
        for (var n = 0; n < _deliver; n++)
        {
            var value = 40 + n;
            fn(&value, 4, user);
        }
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static int EventPost(nuint world, uint hash, void* data, uint size)
    {
        _posts++;
        _postValue = *(int*)data;
        return Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    static nuint ServerWorld() => 1;

    static List<long> Amounts(Func<Action<ReflectedValue>, int> each)
    {
        var seen = new List<long>();
        var n = each(e => seen.Add((long)e["amount"]!));
        Assert.Equal(n, seen.Count);
        return seen;
    }

    [Fact]
    public void Each_gives_the_same_events_by_short_name_qualified_name_and_resolved_type()
    {
        _deliver = 2;
        var deposit = GameTypes.Find("AreaDepositEvent");
        Assert.NotNull(deposit);
        Assert.Same(deposit, GameTypes.Find("keen::ecs::AreaDepositEvent"));
        Assert.Equal([40L, 41L], Amounts(h => GameEvents.Each("AreaDepositEvent", h)));
        Assert.Equal([40L, 41L], Amounts(h => GameEvents.Each("keen::ecs::AreaDepositEvent", h)));
        Assert.Equal([40L, 41L], Amounts(h => GameEvents.Each(deposit!, h)));
        Assert.Contains(deposit, GameTypes.Events);
        _deliver = 0;
        Assert.Equal(0, GameEvents.Each("AreaDepositEvent", _ => Assert.Fail("no event this step")));
        Assert.Equal(0, GameEvents.Each(deposit!, _ => Assert.Fail("no event this step")));
    }

    [Fact]
    public void Post_by_name_and_by_resolved_type_post_the_same_value()
    {
        Assert.True(GameEvents.Post("AreaDepositEvent", new Dictionary<string, object?> { ["amount"] = 5 }));
        Assert.Equal(5, _postValue);
        Assert.True(GameEvents.Post(GameTypes.Find("AreaDepositEvent")!, new Dictionary<string, object?> { ["amount"] = 6 }));
        Assert.Equal(6, _postValue);
        Assert.Equal(2, _posts);
    }

    [Fact]
    public void An_unknown_name_is_refused_every_time_and_asks_kiln_types_again()
    {
        for (var i = 1; i <= 3; i++)
        {
            var e = Assert.Throws<ArgumentException>(() => GameEvents.Each("NoSuchEvent", _ => { }));
            Assert.Contains("unknown type 'NoSuchEvent'", e.Message, StringComparison.Ordinal);
            Assert.Equal(i, _byName);  // a miss is not remembered: a table can still gain the type (a test's)
        }
        Assert.Throws<ArgumentException>(() => GameEvents.Post("NoSuchEvent", new Dictionary<string, object?>()));
        Assert.Null(GameTypes.Find("NoSuchEvent"));
        Assert.Equal(0, _each);
    }

    [Fact]
    public void A_type_that_is_not_an_event_is_refused_by_name_and_by_type()
    {
        var health = GameTypes.Find("Health")!;
        Assert.Contains("'Health' is not an event type", Assert.Throws<ArgumentException>(() => GameEvents.Each("Health", _ => { })).Message, StringComparison.Ordinal);
        Assert.Contains("is not an event type", Assert.Throws<ArgumentException>(() => GameEvents.Each(health, _ => { })).Message, StringComparison.Ordinal);
        Assert.Throws<ArgumentException>(() => GameEvents.Post("Health", new Dictionary<string, object?>()));
        Assert.Throws<ArgumentException>(() => GameEvents.Post(health, new Dictionary<string, object?>()));
        Assert.Throws<ArgumentNullException>(() => GameEvents.Each((GameType)null!, _ => { }));
        Assert.Throws<ArgumentNullException>(() => GameEvents.Each(health, null!));
        Assert.Throws<ArgumentNullException>(() => GameEvents.Post((GameType)null!, new Dictionary<string, object?>()));
        Assert.DoesNotContain(health, GameTypes.Events);
        Assert.Equal(0, _each);
        Assert.Equal(0, _posts);
    }

    [Fact]
    public void Each_per_step_resolves_the_name_once_and_reads_the_event_list_once()
    {
        const int Steps = 200;
        Assert.Equal(0, GameEvents.Each("AreaDepositEvent", _ => { }));  // the first call builds the lookups
        int byName = _byName, at = _eventAt, info = _info, services = _backend.Services;
        Assert.Equal(1, byName);
        Assert.Equal(Fillers + 1, at);
        for (var i = 0; i < Steps; i++)
            Assert.Equal(0, GameEvents.Each("AreaDepositEvent", _ => { }));
        Assert.Equal(byName, _byName);            // no name lookup after the first
        Assert.Equal(at, _eventAt);               // the event list is not read again (was 2,001 calls per Each)
        Assert.Equal(info, _info);                // no type is described again
        Assert.Equal(Steps + 1, _each);           // one EventEach per call: the only per-step native work
        Assert.True(_backend.Services - services <= 3 * Steps, $"{_backend.Services - services} service lookups for {Steps} calls");
        var deposit = GameTypes.Find("AreaDepositEvent")!;
        for (var i = 0; i < Steps; i++) GameEvents.Each(deposit, _ => { });
        Assert.Equal(byName, _byName);
        Assert.Equal(at, _eventAt);
    }

    [Fact]
    public void Find_Events_All_and_field_paths_are_built_once_and_read_only()
    {
        Assert.Same(GameTypes.Events, GameTypes.Events);
        Assert.Equal(Fillers + 1, GameTypes.Events.Count);
        Assert.Equal(_names.Length, GameTypes.All.Count());
        Assert.Same(GameTypes.All, GameTypes.All);
        Assert.Throws<NotSupportedException>(() => ((IList<GameType>)GameTypes.Events).Add(GameTypes.Find("Health")!));
        var at = _eventAt;
        for (var i = 0; i < 50; i++) Assert.NotNull(GameTypes.Find("FillerEvent7"));
        Assert.Equal(at, _eventAt);
        var deposit = GameTypes.Find("AreaDepositEvent")!;
        Assert.Same(deposit.AllFields, deposit.AllFields);
        Assert.Equal(0, deposit.Field("amount")!.Value.Offset);
        Assert.Null(deposit.Field("missing"));
        Assert.Null(deposit.Field(null!));
        var fields = _field;
        var value = ReflectedValue.Owned(deposit, new byte[4]);
        for (var i = 0; i < 50; i++) value["amount"] = i;
        Assert.Equal(49L, value["amount"]);
        Assert.Equal(fields, _field);
    }

    [Fact]
    public void A_table_switched_off_or_another_backend_starts_afresh()
    {
        Assert.NotNull(GameTypes.Find("AreaDepositEvent"));
        _off = true;  // as the test kit's table answers when switched off: no types
        Assert.Null(GameTypes.Find("AreaDepositEvent"));
        Assert.Empty(GameTypes.Events);
        Assert.Throws<ArgumentException>(() => GameEvents.Each("AreaDepositEvent", _ => { }));
        _off = false;
        _deliver = 1;
        Assert.Equal([40L], Amounts(h => GameEvents.Each("AreaDepositEvent", h)));
        var at = _eventAt;
        var other = new Backend { Types = _backend.Types, World = _backend.World };
        Kiln.Backend.Current = other;
        try
        {
            Assert.Equal([40L], Amounts(h => GameEvents.Each("AreaDepositEvent", h)));
            Assert.Equal(at + Fillers + 1, _eventAt);  // another backend reads its table's lists again
        }
        finally { Kiln.Backend.Current = _backend; }
    }
}
