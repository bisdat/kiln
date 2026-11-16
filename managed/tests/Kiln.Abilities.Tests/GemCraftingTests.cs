// SPDX-License-Identifier: MIT
// Gem wave G3 offline: the forge actions (Gems.Socket, Unsocket, Upgrade, Recycle) and their events on a fake
// kiln.gemcraft: the native mirrors' layouts, the request built from an inventory, results and events from native records,
// the pump that holds, posts and answers players' actions and reports what ran, and the argument and capability errors.
// What the game does is the lab test (mods/tests/gems-forge-test); the native logic is native/services/items/tests/gems_test.cpp.
using Kiln;
using Kiln.TestKit;
using Xunit;

[assembly: CollectionBehavior(DisableTestParallelization = true)]
namespace Kiln.AbilitiesTests;

public sealed unsafe class GemCraftingTests : IDisposable
{
    private static readonly ItemId Sword = new(0x4c0d3e02), Bleeding = new(0x5adb2644), Essence = new(0x2b0cf1d5);

    // A fake kiln.gemcraft: what Kiln asked, and what it answers.
    internal sealed class Fake : IGemcraftBackend
    {
        public bool Available = true;
        public string Detail = "";
        public readonly List<uint> Holds = [];
        public readonly List<bool> Watches = [];
        public readonly List<(uint Player, uint Version, bool Cancel)> Decisions = [];
        public readonly Queue<GemcraftNative.Craft> HeldQueue = new(), CraftedQueue = new();
        public readonly List<(GemcraftNative.Request Request, long Key)> Submitted = [];
        public int SubmitResult;

        public (bool Available, string Detail)? Status() => (Available, Detail);

        public int Submit(in GemcraftNative.Request r, long key, out ulong ticket)
        {
            ticket = (ulong)Submitted.Count + 1;
            if (SubmitResult != 0) return SubmitResult;
            Submitted.Add((r, key));
            return 0;
        }

        public int Hold(uint steps)
        {
            Holds.Add(steps);
            return 0;
        }

        public int Held(Span<GemcraftNative.Craft> into, out uint count) => Drain(HeldQueue, into, out count);

        public int Decide(uint player, uint version, bool cancel)
        {
            Decisions.Add((player, version, cancel));
            return 0;
        }

        public int Watch(bool on)
        {
            Watches.Add(on);
            return 0;
        }

        public int Crafted(Span<GemcraftNative.Craft> into, out uint count) => Drain(CraftedQueue, into, out count);

        private static int Drain(Queue<GemcraftNative.Craft> q, Span<GemcraftNative.Craft> into, out uint count)
        {
            var n = 0;
            while (n < into.Length && q.Count > 0) into[n++] = q.Dequeue();
            count = (uint)n;
            return q.Count > 0 ? GemcraftNative.BufferTooSmall : 0;
        }
    }

    // The calls return a task but throw before one exists: run them as a plain call.
    private static void Call(Func<Task> f) => _ = f();

    private readonly Fake _fake = new();

    public GemCraftingTests() => GemCrafting.UseBackend(_fake);

    public void Dispose() => GemCrafting.UseBackend(null);

    private static GemcraftNative.Craft Craft(GemCraftAction action, int status = 0, uint flags = 0, string message = "")
    {
        var c = new GemcraftNative.Craft
        {
            Size = (uint)sizeof(GemcraftNative.Craft), Action = (uint)action, Status = status, Flags = flags, Player = 1, Forge = 3300,
            Carrier = 2554, CarrierItem = Sword.Value, Gem = 2560, GemItem = Bleeding.Value, GemSlot = 0x0000000000000009FA,
            LevelBefore = 9, LevelAfter = 10, LevelCap = 105, ForgeMaxLevel = 10, CostItem = Essence.Value, CostCount = 27, Version = 41,
        };
        var bytes = System.Text.Encoding.UTF8.GetBytes(message);
        for (var i = 0; i < bytes.Length && i < 127; i++) c.Message[i] = bytes[i];
        return c;
    }

    private static InventorySlot Slot(SlotId at, ItemId item, uint entity) =>
        new(at, item, 1, new EntityId(entity), 1, ItemRarity.Common, default, 0, true, false, false, 0);

    private static ItemInstanceRef Ref(uint entity, ItemId item) => new(new EntityRef(new EntityId(entity), 0), item);

    [Fact]
    public void TheMirrorsKeepTheNativeLayouts()
    {
        // native/core/abi/services/kiln_gemcraft.h; gems.cpp static_asserts the same sizes
        Assert.Equal(56, sizeof(GemcraftNative.Request));
        Assert.Equal(232, sizeof(GemcraftNative.Craft));
        Assert.Equal(188, sizeof(GemcraftNative.Status));
        Assert.Equal(8 + 8 * 8, sizeof(GemcraftNative.Table));
        Assert.Equal(24, (int)System.Runtime.InteropServices.Marshal.OffsetOf<GemcraftNative.Request>("GemSlot"));
        Assert.Equal(40, (int)System.Runtime.InteropServices.Marshal.OffsetOf<GemcraftNative.Craft>("GemSlot"));
        Assert.Equal(88, (int)System.Runtime.InteropServices.Marshal.OffsetOf<GemcraftNative.Craft>("Ticket"));
        Assert.Equal(104, (int)System.Runtime.InteropServices.Marshal.OffsetOf<GemcraftNative.Craft>("Message"));
    }

    [Fact]
    public void SocketNamesTheGemsSlotAndTheCarrier()
    {
        var gemSlot = SlotId.Of(new EntityId(2546), 3);
        var slots = new[] { Slot(SlotId.Of(new EntityId(2546), 0), Sword, 2554), Slot(gemSlot, Bleeding, 2560) };
        var (r, refusal) = GemCrafting.Build(GemCraftAction.Socket, new EntityId(1), Ref(2554, Sword), Ref(2560, Bleeding),
            new EntityId(3300), null, slots, id => id == Bleeding);
        Assert.Null(refusal);
        Assert.Equal((uint)GemCraftAction.Socket, r.Action);
        Assert.Equal(2554u, r.Carrier);
        Assert.Equal(gemSlot.Value, r.GemSlot);
        Assert.Equal(2560u, r.Gem);
        Assert.Equal(3300u, r.Forge);
        Assert.Equal(0u, r.Flags);
        Assert.Equal(0u, r.Holder);
    }

    [Fact]
    public void SocketRefusesAGemThatIsNotThereOrIsNoGem()
    {
        var slots = new[] { Slot(SlotId.Of(new EntityId(2546), 0), Sword, 2554), Slot(SlotId.Of(new EntityId(2546), 1), Sword, 2555) };
        var (_, missing) = GemCrafting.Build(GemCraftAction.Socket, new EntityId(1), Ref(2554, Sword), Ref(2560, Bleeding),
            new EntityId(3300), null, slots, id => id == Bleeding);
        Assert.Equal(GemCraftStatus.NotFound, missing!.Status);
        Assert.Contains("not in the player's", missing.Message);
        var (_, notGem) = GemCrafting.Build(GemCraftAction.Socket, new EntityId(1), Ref(2554, Sword), Ref(2555, Sword),
            new EntityId(3300), null, slots, id => id == Bleeding);
        Assert.Equal(GemCraftStatus.NotFound, notGem!.Status);
        Assert.Contains("not a gem", notGem.Message);
        IKilnResult k = notGem;
        Assert.False(k.Ok);
        Assert.NotNull(k.Reason);
    }

    [Fact]
    public void UpgradeAndRecycleTellALooseGemFromACarrier()
    {
        var gemSlot = SlotId.Of(new EntityId(2546), 5);
        var slots = new[] { Slot(SlotId.Of(new EntityId(2546), 0), Sword, 2554), Slot(gemSlot, Bleeding, 2560) };
        var (loose, _) = GemCrafting.Build(GemCraftAction.Upgrade, new EntityId(1), Ref(2560, Bleeding), default, new EntityId(3300),
            null, slots, id => id == Bleeding);
        Assert.Equal(gemSlot.Value, loose.GemSlot);
        Assert.Equal(0u, loose.Carrier);
        var (socketed, _) = GemCrafting.Build(GemCraftAction.Recycle, new EntityId(1), Ref(2554, Sword), default, new EntityId(3300),
            null, slots, id => id == Bleeding);
        Assert.Equal(2554u, socketed.Carrier);
        Assert.Equal(0ul, socketed.GemSlot);
        // a carrier that is not in the list (worn on an Equipment link Kiln did not read, or in another holder): by its entity
        var (elsewhere, refusal) = GemCrafting.Build(GemCraftAction.Upgrade, new EntityId(1), Ref(9999, Sword), default,
            new EntityId(3300), null, slots, id => id == Bleeding);
        Assert.Null(refusal);
        Assert.Equal(9999u, elsewhere.Carrier);
    }

    [Fact]
    public void ACostOptionBecomesTheRequestsFlags()
    {
        var slots = Array.Empty<InventorySlot>();
        var (free, _) = GemCrafting.Build(GemCraftAction.Unsocket, new EntityId(1), Ref(2554, Sword), default, new EntityId(3300),
            new GemCraftOptions { Cost = GemCost.Free, Holder = new EntityId(1127) }, slots, _ => false);
        Assert.Equal(GemcraftNative.Free, free.Flags);
        Assert.Equal(1127u, free.Holder);
        var (paid, _) = GemCrafting.Build(GemCraftAction.Unsocket, new EntityId(1), Ref(2554, Sword), default, new EntityId(3300),
            new GemCraftOptions { Cost = new GemCost(Essence, 3) }, slots, _ => false);
        Assert.Equal(GemcraftNative.Cost, paid.Flags);
        Assert.Equal(Essence.Value, paid.CostItem);
        Assert.Equal(3u, paid.CostCount);
        Assert.Throws<ArgumentException>(() => GemCrafting.Build(GemCraftAction.Unsocket, new EntityId(1), Ref(2554, Sword), default,
            new EntityId(3300), new GemCraftOptions { Cost = new GemCost(Essence, -1) }, slots, _ => false));
        Assert.Throws<ArgumentException>(() => GemCrafting.Build(GemCraftAction.Unsocket, new EntityId(1), Ref(2554, Sword), default,
            new EntityId(3300), new GemCraftOptions { Cost = new GemCost(Essence, 100001) }, slots, _ => false));
        Assert.True(GemCost.Free.IsFree);
        Assert.Equal("nothing", GemCost.Free.ToString());
        Assert.Equal("3 x 0x2b0cf1d5", new GemCost(Essence, 3).ToString());
    }

    [Fact]
    public void ANativeRecordIsAResult()
    {
        var done = GemCrafting.ResultOf(Craft(GemCraftAction.Upgrade));
        Assert.True(done.Ok);
        Assert.Null(done.Reason);
        Assert.Equal(GemCraftAction.Upgrade, done.Action);
        Assert.Equal((9, 10), (done.LevelBefore, done.LevelAfter));
        Assert.Equal(new GemCost(Essence, 27), done.Paid);
        Assert.True(done.Received.IsFree);
        Assert.Equal(new EntityId(2554), done.Carrier.Entity.Id);
        Assert.Equal(Sword, done.Carrier.Item);
        Assert.Equal(Bleeding, done.Gem.Item);
        var stamped = new[] { Slot(SlotId.Of(new EntityId(2546), 0), Sword, 2554) with { Ref = new ItemInstanceRef(new EntityRef(new EntityId(2554), 77), Sword) } };
        Assert.Equal(77ul, GemCrafting.ResultOf(Craft(GemCraftAction.Upgrade), stamped).Carrier.Entity.Serial);
        var refused = GemCrafting.ResultOf(Craft(GemCraftAction.Upgrade, status: 6, message: "the gem is at its cap or the forge's maximum level"));
        Assert.Equal(GemCraftStatus.MaxLevel, refused.Status);
        Assert.Equal("the gem is at its cap or the forge's maximum level", refused.Reason);
        Assert.Equal(GemCraftStatus.Refused, GemCrafting.ResultOf(Craft(GemCraftAction.Socket, status: 99)).Status);  // unknown: refused
        var ev = GemCrafting.EventOf(Craft(GemCraftAction.Upgrade));
        Assert.Equal((GemCraftAction.Upgrade, 9, 41u), (ev.Action, ev.GemLevel, ev.Version));
        Assert.Equal(new GemCost(Essence, 27), ev.Cost);
        Assert.True(ev.Involves(new EntityId(3300)) && ev.Involves(new EntityId(2560)) && !ev.Involves(new EntityId(5)));
    }

    [Fact]
    public void HeldActionsArePostedAndAnsweredAndHoldingFollowsTheSubscription()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var seen = new List<GemCraftingEvent>();
        IDisposable? sub = null;
        kiln.Run(() => sub = Events.On<GemCraftingEvent>(e =>
        {
            seen.Add(e);
            if (e.Action == GemCraftAction.Recycle) e.Cancel();
        }));
        var upgrade = Craft(GemCraftAction.Upgrade);
        var recycle = Craft(GemCraftAction.Recycle);
        recycle.Version = 42;
        recycle.Player = 2;
        _fake.HeldQueue.Enqueue(upgrade);
        _fake.HeldQueue.Enqueue(recycle);
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal(4u, _fake.Holds.Last());
        Assert.Equal(2, seen.Count);
        Assert.Equal([(1u, 41u, false), (2u, 42u, true)], _fake.Decisions);
        // the handler goes: the hold stops (what is held runs) and nothing more is posted
        kiln.Run(() => sub!.Dispose());
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal(0u, _fake.Holds.Last());
        Assert.Equal(2, seen.Count);
    }

    [Fact]
    public void WithoutAHandlerAHeldActionIsAllowedAtOnce()
    {
        using var kiln = new KilnHarness();
        _fake.HeldQueue.Enqueue(Craft(GemCraftAction.Socket));
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal([(1u, 41u, false)], _fake.Decisions);
        Assert.Empty(_fake.Holds);  // nobody listens: nothing is held
    }

    [Fact]
    public void WhatRanIsPostedOnceAndRefusalsAreNot()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var seen = new List<GemCraftedEvent>();
        kiln.Run(() => Events.On<GemCraftedEvent>(seen.Add));
        _fake.CraftedQueue.Enqueue(Craft(GemCraftAction.Upgrade));
        _fake.CraftedQueue.Enqueue(Craft(GemCraftAction.Recycle, flags: GemcraftNative.ByCode));
        _fake.CraftedQueue.Enqueue(Craft(GemCraftAction.Upgrade, status: 1));
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal(2, seen.Count);
        Assert.False(seen[0].ByCode);
        Assert.True(seen[1].ByCode);
        Assert.Equal((9, 10), (seen[0].LevelBefore, seen[0].LevelAfter));
        Assert.Contains(true, _fake.Watches);
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal(2, seen.Count);
    }

    [Fact]
    public void ManyRecordsAreDrainedInOnePre()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var n = 0;
        kiln.Run(() => Events.On<GemCraftedEvent>(_ => n++));
        for (var i = 0; i < 40; i++) _fake.CraftedQueue.Enqueue(Craft(GemCraftAction.Upgrade));
        kiln.Run(GemCrafting.PumpOnce);
        Assert.Equal(40, n);
    }

    [Fact]
    public void ASubscriptionIsRefusedByNameWhileTheForgeActionsAreOff()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        _fake.Available = false;
        _fake.Detail = "equipment_actions' list 10 is [...], not the one Kiln builds the handler's iterator for";
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Events.On<GemCraftingEvent>(_ => { })));
        Assert.Equal("gems.craft", off.Capability);
        Assert.Contains("list 10", off.Message);
    }

    [Fact]
    public void TheCallsCheckTheirArgumentsAndTheCapability()
    {
        using var kiln = new KilnHarness();
        var player = new EntityId(1);
        var forge = new EntityId(3300);
        var off = kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Gems.Upgrade(player, Ref(2560, Bleeding), forge))));
        Assert.Equal("gems.craft", off.Capability);
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Gems.Upgrade(EntityId.None, Ref(2560, Bleeding), forge))));
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Gems.Upgrade(player, Ref(2560, Bleeding), EntityId.None))));
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Gems.Socket(player, ItemInstanceRef.None, Ref(2560, Bleeding), forge))));
        kiln.Run(() => Assert.Throws<ArgumentException>(() => Call(() => Gems.Unsocket(player, ItemInstanceRef.None, forge))));
        Assert.Throws<InvalidOperationException>(() => Call(() => Gems.Recycle(player, Ref(2560, Bleeding), forge)));  // not the tick thread
    }

    internal static GemcraftNative.Craft NewCraft(GemCraftAction action, uint flags = 0) => Craft(action, flags: flags);
    internal static ItemInstanceRef NewRef(uint entity, ItemId item) => Ref(entity, item);
    internal static readonly ItemId SwordId = Sword;
    internal Fake Backend => _fake;
}

// Submissions complete their tasks (await needs a safe context: this class is not unsafe).
public sealed class GemCraftingSubmitTests : IDisposable
{
    private static void Call(Func<Task> f) => _ = f();
    private readonly GemCraftingTests _fixture = new();
    private GemCraftingTests.Fake _fake => _fixture.Backend;

    public void Dispose() => _fixture.Dispose();

    [Fact]
    public async Task ASubmittedActionCompletesWithItsResult()
    {
        using var kiln = new KilnHarness();
        kiln.Capabilities.Override(Gems.CraftCapability, true);
        var task = kiln.Run(() => Gems.Unsocket(new EntityId(1), GemCraftingTests.NewRef(2554, GemCraftingTests.SwordId), new EntityId(3300), new GemCraftOptions { Cost = GemCost.Free }));
        var (r, key) = Assert.Single(_fake.Submitted);
        Assert.Equal((uint)GemCraftAction.Unsocket, r.Action);
        Assert.Equal(GemcraftNative.Free, r.Flags);
        Assert.False(task.IsCompleted);
        var done = GemCraftingTests.NewCraft(GemCraftAction.Unsocket, GemcraftNative.ByCode);
        kiln.Run(() => GemCrafting.Complete(key, done));
        var result = await task.WaitAsync(TimeSpan.FromSeconds(5), TestContext.Current.CancellationToken);
        Assert.Equal(GemCraftStatus.Done, result.Status);
        // a full queue is a result, not an exception
        _fake.SubmitResult = GemcraftNative.BufferTooSmall;
        var full = kiln.Run(() => Gems.Unsocket(new EntityId(1), GemCraftingTests.NewRef(2554, GemCraftingTests.SwordId), new EntityId(3300)));
        var refused = await full;
        Assert.Equal(GemCraftStatus.Refused, refused.Status);
        Assert.Contains("256", refused.Message);
        _fake.SubmitResult = GemcraftNative.Unavailable;
        _fake.Detail = "gems.craft was switched off (see kiln.log)";
        kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => Call(() => Gems.Unsocket(new EntityId(1), GemCraftingTests.NewRef(2554, GemCraftingTests.SwordId), new EntityId(3300)))));
    }
}
