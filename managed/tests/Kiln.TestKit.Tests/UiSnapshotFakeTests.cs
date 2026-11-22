// SPDX-License-Identifier: MIT
// The kit's fake kiln.ui.snapshot (Fakes/UiSnapshot/): Kiln.Blocks' BlockUi.ReadWorld and DemandWorld on it. Demands
// whose union is what a publication carries, whole publications (a second copy for a long inventory gets the same one),
// the 512-slot budget, a world change that revokes tokens, the hour on a joined and a hosting client, the budgets.
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiSnapshotFake")]
public sealed unsafe class UiSnapshotFakeTests
{
    private static UiSnapshotAbi.Table* T => (UiSnapshotAbi.Table*)KitBackend.Instance.Service(UiSnapshotAbi.Service, 1);

    private const UiWorldFields Local = UiWorldFields.LocalVitals | UiWorldFields.LocalPosition | UiWorldFields.LocalInventory;

    private static UiWorldSnapshot Read(KilnHarness kiln)
    {
        var r = kiln.Run(BlockUi.ReadWorld);
        Assert.True(r.Ok, r.Reason);
        return r.Snapshot!;
    }

    [Fact]
    public void TheTableIsTheHeadersV1()
    {
        Assert.Equal(40, sizeof(UiSnapshotAbi.Acquire));
        Assert.Equal(152, sizeof(UiSnapshotAbi.Values));
        Assert.Equal(24, sizeof(UiSnapshotAbi.InventoryRow));
        Assert.Equal(24, sizeof(UiSnapshotAbi.DemandResult));
        Assert.Equal(48, sizeof(UiSnapshotAbi.Table));
        Assert.True(T != null);
        Assert.Equal((48u, 1u), (((uint*)T)[0], ((uint*)T)[1]));
        Assert.Contains(Coverage.All, r => r.Capabilities.Contains("kiln.ui.snapshot") && r.Kind == CoverageKind.RecordingFake);
    }

    [Fact]
    public void ReadWorldGetsOneWholePublicationOfWhatWasDemanded()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        using var demand = kiln.Run(() => BlockUi.DemandWorld(Local));
        Assert.True(demand.Ok, demand.Reason);
        Assert.True(kiln.Run(() => Capabilities.Is("ui.blocks.world")));  // Kiln.Blocks reports it once it found the table
        Assert.Equal(Local, kiln.UiSnapshot.Demanded);
        var id = kiln.UiSnapshot.Publish(new FakeWorldSample
        {
            LocalPlayer = 77,
            Vitals = new FakeVitals(413, 500, 80, 100, 5, 60),
            Position = new FakeWorldPosition(1.5, -2, 3.25),
            Inventory = [new FakeWorldSlot(0xBB, 60, 0x5000, 3), new FakeWorldSlot(0xAA, 1, 0x5000, 2, Pide: 9), new FakeWorldSlot(0xCC, 4, 0x4000, 7)],
            HostHours = 13.5,  // nobody asked for the hour
        });
        var s = Read(kiln);
        Assert.Equal(id, s.Publication);
        Assert.True(s.Has(Local));
        Assert.False(s.Has(UiWorldFields.HostTime));
        Assert.Equal(new string?[] { null, null, null, "NoSnapshot" }, s.UnavailableReasons);
        Assert.Equal((77u, 413u, 500u, 80u, 100u, 5u, 60u), (s.LocalPlayer, s.Health, s.HealthMax, s.Stamina, s.StaminaMax, s.Mana, s.ManaMax));
        Assert.Equal((3L << 31, -2L << 32, 13L << 30), (s.XFixed, s.YFixed, s.ZFixed));  // 32.32 fixed point
        Assert.Equal(new[] { "l:00004000:7", "l:00005000:2", "l:00005000:3" }, s.Inventory.Select(i => i.Key));  // sorted by page and slot
        Assert.Equal(new UiInventoryValue("l:00005000:2", 77, 0x5000, 2, 0xAA, 1, 9), s.Inventory[1]);
        Assert.Equal(0, kiln.UiSnapshot.Tokens);  // the read released its token
        demand.Dispose();
        Assert.Equal(UiWorldFields.None, kiln.UiSnapshot.Demanded);
        Assert.Equal(0, kiln.UiSnapshot.Demands);
    }

    [Fact]
    public void WhatNobodyDemandedOrNobodySampledIsNoSnapshotAndASamplesReasonWins()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        Assert.Equal("NoSnapshot", kiln.Run(BlockUi.ReadWorld).Reason);  // no publication yet
        kiln.UiSnapshot.Publish(new FakeWorldSample { Vitals = new FakeVitals(10, 20) });
        var none = Read(kiln);
        Assert.False(none.Has(UiWorldFields.LocalVitals));
        Assert.Equal(0u, none.Health);  // fields of a missing group are zero
        Assert.Equal(new string?[] { "NoSnapshot", "NoSnapshot", "NoSnapshot", "NoSnapshot" }, none.UnavailableReasons);
        using var demand = kiln.Run(() => BlockUi.DemandWorld(Local));
        kiln.UiSnapshot.Publish(new FakeWorldSample
        {
            Vitals = new FakeVitals(10, 20),
            Position = new FakeWorldPosition(0, 0, 0),
            Unavailable = new Dictionary<UiWorldFields, string> { [UiWorldFields.LocalPosition] = "StaleSource" },
        });
        var s = Read(kiln);
        Assert.True(s.Has(UiWorldFields.LocalVitals));
        Assert.Equal(new string?[] { null, "StaleSource", "NoSnapshot", "NoSnapshot" }, s.UnavailableReasons);
        Assert.Throws<ArgumentException>(() => kiln.UiSnapshot.Publish(new FakeWorldSample
        {
            Unavailable = new Dictionary<UiWorldFields, string> { [UiWorldFields.LocalVitals] = "Sleepy" },
        }));
        Assert.Throws<ArgumentException>(() => kiln.UiSnapshot.Publish(new FakeWorldSample
        {
            Unavailable = new Dictionary<UiWorldFields, string> { [Local] = "StaleSource" },
        }));
        Assert.Throws<ArgumentOutOfRangeException>(() => kiln.UiSnapshot.Publish(new FakeWorldSample { Position = new FakeWorldPosition(double.NaN, 0, 0) }));
        Assert.Throws<ArgumentException>(() => kiln.Run(() => BlockUi.DemandWorld(UiWorldFields.None)));
    }

    [Fact]
    public void ALongInventoryIsCopiedWholeAndMoreThan512SlotsIsBudget()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        using var demand = kiln.Run(() => BlockUi.DemandWorld(UiWorldFields.LocalInventory));
        // 40 slots: more than Kiln.Blocks' first buffer, so it copies again with the same token
        kiln.UiSnapshot.Publish(new FakeWorldSample { Inventory = Enumerable.Range(0, 40).Select(i => new FakeWorldSlot(1, 1, 0, (uint)i)).ToArray() });
        var s = Read(kiln);
        Assert.Equal(40, s.Inventory.Count);
        Assert.Equal("l:00000000:39", s.Inventory[^1].Key);
        kiln.UiSnapshot.Publish(new FakeWorldSample { Inventory = Enumerable.Range(0, 513).Select(i => new FakeWorldSlot(1, 1, (uint)i / 200, (uint)i % 200)).ToArray() });
        var over = Read(kiln);
        Assert.False(over.Has(UiWorldFields.LocalInventory));
        Assert.Empty(over.Inventory);
        Assert.Equal("Budget", over.UnavailableReasons[2]);
    }

    [Fact]
    public void AnUndersizedCopyWritesNothingAndTheRetryGetsTheSamePublication()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        using var demand = kiln.Run(() => BlockUi.DemandWorld(UiWorldFields.LocalInventory));
        var first = kiln.UiSnapshot.Publish(new FakeWorldSample { Inventory = [new FakeWorldSlot(1, 1), new FakeWorldSlot(2, 2, SlotIndex: 1)] });
        var a = new UiSnapshotAbi.Acquire { Size = (uint)sizeof(UiSnapshotAbi.Acquire) };
        Assert.Equal(0, T->AcquireSnapshot(77, &a));
        Assert.Equal((1u, first), (a.Ok, a.Publication));
        kiln.UiSnapshot.Publish(new FakeWorldSample { Inventory = [new FakeWorldSlot(3, 3)] });  // a newer one meanwhile
        var v = new UiSnapshotAbi.Values { Size = (uint)sizeof(UiSnapshotAbi.Values), Publication = 0xDEAD };
        var row = new UiSnapshotAbi.InventoryRow { Item = 0xBEEF };
        uint required = 0;
        Assert.Equal(FakeTables.BufferTooSmall, T->Copy(a.Token, &v, &row, 1, &required));
        Assert.Equal((2u, 0xDEADul, 0xBEEFu), (required, v.Publication, row.Item));  // nothing written
        var rows = stackalloc UiSnapshotAbi.InventoryRow[2];
        Assert.Equal(0, T->Copy(a.Token, &v, rows, 2, &required));
        Assert.Equal((first, 2u, 2u), (v.Publication, v.InventoryCount, rows[1].Item));
        Assert.Equal(0, T->Release(a.Token));
        Assert.Equal(FakeTables.NotFound, T->Release(a.Token));
        Assert.Equal(FakeTables.NotFound, T->Copy(a.Token, &v, rows, 2, &required));
        Assert.Equal(FakeTables.InvalidArgument, T->Copy(0, &v, rows, 2, &required));
    }

    [Fact]
    public void AWorldChangeRevokesEveryTokenAndReadsNoWorldUntilTheNextPublication()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        using var demand = kiln.Run(() => BlockUi.DemandWorld(UiWorldFields.LocalVitals));
        kiln.UiSnapshot.Publish(new FakeWorldSample { Vitals = new FakeVitals(1, 2) });
        var a = new UiSnapshotAbi.Acquire { Size = (uint)sizeof(UiSnapshotAbi.Acquire) };
        Assert.Equal(0, T->AcquireSnapshot(77, &a));
        kiln.UiSnapshot.EndWorld();
        var v = new UiSnapshotAbi.Values { Size = (uint)sizeof(UiSnapshotAbi.Values) };
        Assert.Equal(UiSnapshotAbi.Revoked, T->Copy(a.Token, &v, null, 0, null));
        Assert.Equal(0ul, kiln.UiSnapshot.Publication);
        Assert.Equal("NoWorld", kiln.Run(BlockUi.ReadWorld).Reason);
        Assert.Equal(1, kiln.UiSnapshot.Demands);  // demands stay
        kiln.UiSnapshot.Publish(new FakeWorldSample { Vitals = new FakeVitals(3, 4) });
        Assert.Equal(3u, Read(kiln).Health);
        Assert.Equal(0, T->Release(a.Token));  // the revoked pin can still be freed
    }

    [Fact]
    public void TheHourIsThereOnAHostAndDeferredOnAJoinedWorld()
    {
        using (var host = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.HostingClient }))
        {
            host.Boot();
            using var demand = host.Run(() => BlockUi.DemandWorld(UiWorldFields.HostTime));
            host.UiSnapshot.Publish(new FakeWorldSample { HostHours = 13.5 });
            var s = Read(host);
            Assert.True(s.Has(UiWorldFields.HostTime));
            Assert.Equal(13.5m, s.ServerHours);
        }
        using var joined = new KilnHarness(new KilnHarnessOptions { Side = HarnessSide.JoinedClient });
        joined.Boot();
        using var asked = joined.Run(() => BlockUi.DemandWorld(UiWorldFields.HostTime));
        Assert.True(asked.Ok, asked.Reason);  // accepted, and nothing will come
        joined.UiSnapshot.Publish(new FakeWorldSample { HostHours = 13.5 });
        var j = Read(joined);
        Assert.False(j.Has(UiWorldFields.HostTime));
        Assert.Equal("DeferredSource", j.UnavailableReasons[3]);
        Assert.Equal(0m, j.ServerHours);
    }

    [Fact]
    public void DemandsAndTokensKeepTheirBudgets()
    {
        using var kiln = new KilnHarness();
        kiln.Boot();
        kiln.Run(() =>
        {
            var r = new UiSnapshotAbi.DemandResult { Size = (uint)sizeof(UiSnapshotAbi.DemandResult) };
            for (var i = 0; i < 64; i++)
            {
                Assert.Equal(0, T->DemandAdd(77, 1, &r));
                Assert.Equal(1u, r.Ok);
            }
            Assert.Equal(0, T->DemandAdd(77, 1, &r));
            Assert.Equal((0u, UiSnapshotAbi.Budget, 0ul), (r.Ok, r.Reason, r.Handle));
            Assert.Equal(0, T->DemandAdd(78, 2, &r));  // another owner has its own 64
            Assert.Equal(1u, r.Ok);
            Assert.Equal(0, T->DemandRemove(r.Handle));
            Assert.Equal(FakeTables.NotFound, T->DemandRemove(r.Handle));
            Assert.Equal(FakeTables.InvalidArgument, T->DemandAdd(77, 0, &r));
            Assert.Equal(FakeTables.InvalidArgument, T->DemandAdd(77, 16, &r));
            Assert.Equal(FakeTables.InvalidArgument, T->DemandAdd(0, 1, &r));
            return 0;
        });
        kiln.UiSnapshot.Publish(new FakeWorldSample { Vitals = new FakeVitals(1, 2) });
        var a = new UiSnapshotAbi.Acquire { Size = (uint)sizeof(UiSnapshotAbi.Acquire) };
        Assert.Equal(0, T->AcquireSnapshot(77, &a));
        Assert.Equal(0, T->AcquireSnapshot(77, &a));
        Assert.Equal(0, T->AcquireSnapshot(77, &a));
        Assert.Equal((0u, UiSnapshotAbi.Budget, 0ul), (a.Ok, a.Reason, a.Token));  // 2 tokens per owner
        Assert.Equal(2, kiln.UiSnapshot.Tokens);
    }

    [Fact]
    public void ASwitchedOffServiceAnswersUnavailableAndReadWorldThrows()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override("ui.blocks.world", false, "switched off by the test");
        kiln.Boot();
        Assert.Throws<KilnUnavailableException>(() => kiln.Run(BlockUi.ReadWorld));
    }
}
