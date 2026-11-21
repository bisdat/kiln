// SPDX-License-Identifier: MIT
// Kiln.Blocks' binding of kiln.ui.snapshot (Ui/UiWorldSnapshotNative.cs) on a fake table: one read is acquire, copy
// (again with the same token when the inventory needs more room) and release; the value is deep-immutable; fields of
// an unavailable group read as zero with their reason; the one error rule of ReadWorld and DemandWorld; the rows; the
// runtime seam; UiWorldSnapshotChangedEvent once per new publication while someone listens.
using System.Collections.Immutable;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Xunit;
using N = Kiln.UiWorldSnapshotNative;

namespace Kiln.BlockTests;

public sealed unsafe class UiWorldSnapshotBindingTests : IDisposable
{
    private sealed class FakeBackend(nint table) : IKilnBackend
    {
        public string Version => "test";
        public bool IsTickThread => true;
        public bool HasFeature(string name) => false;
        public nint Service(string name, uint minVersion) => name == N.Service ? table : 0;
        public void Log(ulong owner, LogLevel level, string text) { }
        public ulong OwnerCreate(string id) => 77;
    }

    // ---- the fake kiln.ui.snapshot: static, as its slots are unmanaged entry points ----
    private const ulong Token = 0x5554_0000_0000_0007;
    private static int _acquires, _copies, _releases, _adds, _removes;
    private static readonly List<(ulong Token, uint Capacity)> CopyCalls = [];
    private static ulong _lastOwner, _lastRemoved, _publication;
    private static uint _lastMask, _acquireOk, _acquireReason, _mask, _demandOk, _demandReason;
    private static int _copyRc, _rows;
    private static bool _wrongPublication;
    private static uint[] _reasons = new uint[4];

    private static void ResetFake()
    {
        _acquires = _copies = _releases = _adds = _removes = 0;
        CopyCalls.Clear();
        _lastOwner = _lastRemoved = 0;
        _publication = 42;
        _lastMask = 0;
        _acquireOk = 1;
        _acquireReason = 0;
        _mask = 15;
        _demandOk = 1;
        _demandReason = 0;
        _copyRc = 0;
        _rows = 40;
        _wrongPublication = false;
        _reasons = new uint[4];
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeDemandAdd(ulong owner, uint mask, N.DemandResult* r)
    {
        _adds++;
        _lastOwner = owner;
        _lastMask = mask;
        if (r == null || r->Size < 24) return N.InvalidArgument;
        r->Ok = _demandOk;
        r->Reason = _demandReason;
        r->Handle = _demandOk != 0 ? 0x5544_0000_0000_00A0UL + (ulong)_adds : 0;
        return N.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeDemandRemove(ulong handle)
    {
        _removes++;
        _lastRemoved = handle;
        return N.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeAcquire(ulong owner, N.Acquire* a)
    {
        _acquires++;
        _lastOwner = owner;
        if (a == null || a->Size < 40) return N.InvalidArgument;
        a->Ok = _acquireOk;
        a->Reason = _acquireReason;
        a->Token = _acquireOk != 0 ? Token : 0;
        a->Publication = _acquireOk != 0 ? _publication : 0;
        a->CaptureMs = 5000;
        return N.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeCopy(ulong token, N.Values* v, N.InventoryRow* rows, uint capacity, uint* required)
    {
        _copies++;
        CopyCalls.Add((token, capacity));
        if (token != Token) return N.NotFound;
        if (_copyRc != 0) return _copyRc;
        if (v == null || v->Size < 152 || (capacity > 0 && rows == null)) return N.InvalidArgument;
        var n = (uint)_rows;
        if (capacity < n)
        {
            if (required != null) *required = n;
            return N.BufferTooSmall;  // writes nothing else
        }
        var size = v->Size;
        *v = default;
        v->Size = size;
        v->AvailableMask = _mask;
        v->RequestedMask = 15;
        v->InventoryCount = n;
        for (var i = 0; i < 4; i++) v->UnavailableReason[i] = (_mask & (1u << i)) != 0 ? 0 : _reasons[i];
        v->Publication = _wrongPublication ? _publication + 1 : _publication;
        v->LocalSession = 3;
        v->LocalStep = 77;
        v->LocalCaptureMs = 5000;
        v->LocalPlayer = 7;
        (v->Health, v->HealthMax, v->Stamina, v->StaminaMax, v->Mana, v->ManaMax) = (413u, 500u, 90u, 100u, 30u, 60u);
        (v->XFixed, v->YFixed, v->ZFixed) = (5L << 32, (7L << 32) + (1L << 31), -(3L << 32));
        (v->ServerSession, v->ServerStep, v->ServerCaptureMs, v->ServerHours) = (2ul, 40ul, 4990ul, 13.25);
        for (var i = 0; i < n; i++)
            rows[i] = new N.InventoryRow
            {
                Holder = 7, InventoryPage = (uint)(i / 16), SlotIndex = (uint)(i % 16), Item = 100 + (uint)i, Count = 1 + (uint)i,
                Pide = 9000 + (uint)i,
            };
        if (required != null) *required = n;
        return N.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeRelease(ulong token)
    {
        _releases++;
        return token == Token ? N.Ok : N.NotFound;
    }

    private readonly N.Table* _table;
    private readonly IKilnBackend _previous = Backend.Current;

    public UiWorldSnapshotBindingTests()
    {
        ResetFake();
        _table = (N.Table*)NativeMemory.AllocZeroed((nuint)sizeof(N.Table));
        _table->Header = new ServiceHeader { Size = (uint)sizeof(N.Table), Version = N.Version };
        _table->DemandAdd = &FakeDemandAdd;
        _table->DemandRemove = &FakeDemandRemove;
        _table->AcquireSnapshot = &FakeAcquire;
        _table->Copy = &FakeCopy;
        _table->Release = &FakeRelease;
        Use(_table);
    }

    private static void Use(N.Table* table)
    {
        Backend.Current = new FakeBackend((nint)table);
        Capabilities.Reset();
        N.ResetForTests();
        UiWorldFeed.ResetForTests();
    }

    public void Dispose()
    {
        Backend.Current = _previous;
        Capabilities.Reset();
        N.ResetForTests();
        UiWorldFeed.ResetForTests();
        NativeMemory.Free(_table);
    }

    [Fact]
    public void The_records_have_the_C_header_layout()
    {
        N.Values v = default;
        var at = (byte*)&v;
        Assert.Equal(152, sizeof(N.Values));
        Assert.Equal(16, (int)((byte*)v.UnavailableReason - at));
        Assert.Equal(88, (int)((byte*)&v.ManaMax - at));
        Assert.Equal(96, (int)((byte*)&v.XFixed - at));
        Assert.Equal(120, (int)((byte*)&v.ServerSession - at));
        Assert.Equal(144, (int)((byte*)&v.ServerHours - at));
        N.Acquire a = default;
        Assert.Equal((40, 16, 24), (sizeof(N.Acquire), (int)((byte*)&a.Token - (byte*)&a), (int)((byte*)&a.Publication - (byte*)&a)));
        Assert.Equal(24, sizeof(N.InventoryRow));
        Assert.Equal(24, sizeof(N.DemandResult));
        Assert.Equal(8 + 5 * IntPtr.Size, sizeof(N.Table));
    }

    [Fact]
    public void ReadWorld_copies_one_publication_with_one_token_and_releases_it()
    {
        var r = BlockUi.ReadWorld();
        Assert.True(r.Ok, r.Reason);
        // 40 records: the first copy (room for 32) wrote nothing; the second, with the same token, had room for all
        Assert.Equal(new[] { (Token, 32u), (Token, 40u) }, CopyCalls);
        Assert.Equal((1, 1), (_acquires, _releases));
        Assert.Equal(77ul, _lastOwner);  // Kiln.Blocks' own owner reads
        var s = r.Snapshot!;
        Assert.Equal(42ul, s.Publication);
        Assert.True(s.Has(UiWorldFields.LocalVitals | UiWorldFields.LocalPosition | UiWorldFields.LocalInventory | UiWorldFields.HostTime));
        Assert.Equal(new UiSnapshotStamp(3, 77, 5000), s.Local);
        Assert.Equal(new UiSnapshotStamp(2, 40, 4990), s.Server);
        Assert.Equal((7u, 413u, 500u, 90u, 100u, 30u, 60u), (s.LocalPlayer, s.Health, s.HealthMax, s.Stamina, s.StaminaMax, s.Mana, s.ManaMax));
        Assert.Equal((5L << 32, (7L << 32) + (1L << 31), -(3L << 32)), (s.XFixed, s.YFixed, s.ZFixed));
        Assert.Equal(13.25m, s.ServerHours);
        Assert.Equal(40, s.Inventory.Count);
        Assert.Equal(new UiInventoryValue("l:00000001:1", 7, 1, 1, 117, 18, 9017), s.Inventory[17]);
        Assert.Equal(new string?[] { null, null, null, null }, s.UnavailableReasons);
    }

    [Fact]
    public void The_snapshot_is_deep_immutable()
    {
        var s = BlockUi.ReadWorld().Snapshot!;
        Assert.IsType<ImmutableArray<UiInventoryValue>>(s.Inventory);
        Assert.IsType<ImmutableArray<string?>>(s.UnavailableReasons);
        Assert.True(((ICollection<UiInventoryValue>)s.Inventory).IsReadOnly);
        Assert.Throws<NotSupportedException>(() => ((IList<UiInventoryValue>)s.Inventory).Add(s.Inventory[0]));
        Assert.Throws<NotSupportedException>(() => ((IList<string?>)s.UnavailableReasons)[0] = "x");
        Assert.False(s.Inventory is List<UiInventoryValue>);
        // two reads are two values: the first one never changes under its reader
        _publication = 43;
        var t = BlockUi.ReadWorld().Snapshot!;
        Assert.Equal((42ul, 43ul), (s.Publication, t.Publication));
    }

    [Fact]
    public void Fields_of_an_unavailable_group_read_as_zero_with_their_reason()
    {
        _mask = (uint)UiWorldFields.LocalPosition;
        _rows = 0;
        _reasons = [N.ReasonStaleSource, 0, N.ReasonBudget, N.ReasonDeferredSource];
        var s = BlockUi.ReadWorld().Snapshot!;
        Assert.Single(CopyCalls);
        Assert.Equal((uint)UiWorldFields.LocalPosition, s.AvailableMask);
        Assert.Equal((0u, 0u, 0u), (s.Health, s.HealthMax, s.Mana));  // the fake wrote 413: never consumed
        Assert.Empty(s.Inventory);
        Assert.Equal(0m, s.ServerHours);
        Assert.Equal(new UiSnapshotStamp(0, 0, 0), s.Server);
        Assert.Equal(new string?[] { "StaleSource", null, "Budget", "DeferredSource" }, s.UnavailableReasons);
        Assert.Equal(5L << 32, s.XFixed);
    }

    [Fact]
    public void Nothing_to_show_is_a_refusal_with_the_reason_not_an_exception()
    {
        _acquireOk = 0;
        _acquireReason = N.ReasonNoWorld;
        var r = BlockUi.ReadWorld();
        Assert.Equal((false, "NoWorld"), (r.Ok, r.Reason));
        Assert.Null(r.Snapshot);
        Assert.Equal((0, 0), (_copies, _releases));
        _acquireReason = N.ReasonExpired;
        Assert.Equal("Expired", BlockUi.ReadWorld().Reason);
    }

    [Fact]
    public void A_token_the_world_revoked_or_that_expired_refuses_and_is_released()
    {
        _copyRc = N.Revoked;
        var r = BlockUi.ReadWorld();
        Assert.Equal((false, "NoWorld", 1), (r.Ok, r.Reason, _releases));
        _copyRc = N.NotFound;
        Assert.Equal("Expired", BlockUi.ReadWorld().Reason);
        _copyRc = 0;
        _wrongPublication = true;  // a header that is not the token's publication is never shown
        r = BlockUi.ReadWorld();
        Assert.False(r.Ok);
        Assert.Equal(3, _releases);
    }

    [Fact]
    public void DemandWorld_checks_the_fields_first()
    {
        Assert.Throws<ArgumentException>(() => BlockUi.DemandWorld(UiWorldFields.None));
        Assert.Throws<ArgumentException>(() => BlockUi.DemandWorld((UiWorldFields)16));
        Assert.Throws<ArgumentException>(() => BlockUi.DemandWorld(UiWorldFields.LocalVitals | (UiWorldFields)32));
        Assert.Equal(0, _adds);
    }

    [Fact]
    public void DemandWorld_adds_one_handle_and_disposing_removes_it_once()
    {
        var d = BlockUi.DemandWorld(UiWorldFields.LocalVitals | UiWorldFields.HostTime);
        Assert.True(d.Ok, d.Reason);
        Assert.NotEqual(0ul, d.Handle);
        Assert.Equal((9u, 77ul), (_lastMask, _lastOwner));  // no mod on this thread: Kiln.Blocks' own owner
        d.Dispose();
        d.Dispose();
        Assert.Equal((1, d.Handle), (_removes, _lastRemoved));
    }

    [Fact]
    public void A_refused_demand_is_Ok_false_with_its_reason()
    {
        _demandOk = 0;
        _demandReason = N.ReasonBudget;
        var d = BlockUi.DemandWorld(UiWorldFields.LocalPosition);
        Assert.Equal((false, "Budget", 0ul), (d.Ok, d.Reason, d.Handle));
        d.Dispose();
        Assert.Equal(0, _removes);
    }

    [Fact]
    public void Without_the_table_both_members_throw_KilnUnavailableException_naming_ui_blocks_world()
    {
        Use(null);
        var e = Assert.Throws<KilnUnavailableException>(() => BlockUi.ReadWorld());
        Assert.Equal("ui.blocks.world", e.Capability);
        e = Assert.Throws<KilnUnavailableException>(() => BlockUi.DemandWorld(UiWorldFields.LocalVitals));
        Assert.Equal("ui.blocks.world", e.Capability);
        Assert.Throws<ArgumentException>(() => BlockUi.DemandWorld(UiWorldFields.None));  // arguments before the capability
        foreach (var row in new[] { "ui.blocks.world", "world.vitals", "world.position", "world.inventory", "world.host_time" })
            Assert.False(Capabilities.Is(row), row);
    }

    [Fact]
    public void The_rows_follow_the_table_the_stage_and_kiln_time()
    {
        UiWorldCaps.Require();
        Assert.True(Capabilities.Is("ui.blocks.world"));
        Assert.True(Capabilities.Is("world.position"));
        Assert.False(Capabilities.Is("world.vitals"));     // ui.values.hud (05a's update hook) is not here
        Assert.False(Capabilities.Is("world.inventory"));
        Assert.False(Capabilities.Is("world.host_time"));  // kiln.time is not here
        Capabilities.Report("ui.values.hud", CapabilityState.Available);
        UiWorldCaps.Rows.Refresh();
        Assert.True(Capabilities.Is("world.vitals"));
        Assert.True(Capabilities.Is("world.inventory"));
    }

    [Fact]
    public void The_runtime_seam_reads_through_the_table_and_never_throws()
    {
        var r = UiWorldSource.Read();
        Assert.True(r.Ok, r.Reason);
        Assert.Equal(42ul, r.Snapshot!.Publication);
        var d = UiWorldSource.Demand("not-loaded-mod", UiWorldFields.LocalPosition);
        Assert.True(d.Ok);
        Assert.Equal(77ul, _lastOwner);  // a mod that is not loaded: Kiln.Blocks' own owner
        var bad = UiWorldSource.Demand("not-loaded-mod", UiWorldFields.None);
        Assert.Equal((false, "InvalidArgument"), (bad.Ok, bad.Reason));
        Use(null);
        Assert.Equal((false, "Unavailable"), (UiWorldSource.Read().Ok, UiWorldSource.Read().Reason));
        Assert.False(UiWorldSource.Demand("not-loaded-mod", UiWorldFields.LocalPosition).Ok);
        Assert.False(UiWorldSource.Hosting());  // no kiln.client here
    }

    [Fact]
    public void The_changed_event_comes_once_per_new_publication_while_someone_listens()
    {
        var raised = new List<UiWorldSnapshotChangedEvent>();
        UiWorldFeed.Poll(force: true);
        Assert.Equal(0, _acquires);  // nobody listens: no read
        using var sub = Events.On<UiWorldSnapshotChangedEvent>(_ => { });
        UiWorldFeed.Raise = e => raised.Add((UiWorldSnapshotChangedEvent)e);
        UiWorldFeed.Poll(force: true);
        Assert.Single(raised);
        Assert.Equal((0ul, 42ul), (raised[0].OldPublication, raised[0].NewSnapshot.Publication));
        var copies = _copies;
        UiWorldFeed.Poll(force: true);
        Assert.Single(raised);  // the same publication: no event, and no copy (acquire and release only)
        Assert.Equal(copies, _copies);
        Assert.Equal(_acquires, _releases);
        _publication = 43;
        UiWorldFeed.Poll(force: true);
        Assert.Equal(2, raised.Count);
        Assert.Equal((42ul, 43ul), (raised[1].OldPublication, raised[1].NewSnapshot.Publication));
    }
}
