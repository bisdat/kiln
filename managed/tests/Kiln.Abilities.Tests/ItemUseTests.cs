// SPDX-License-Identifier: MIT
// Abilities.UseItem (since 0.2.0-dev.30.3, GitHub issue #110) offline: the native table's layout against kiln_abilities.h,
// item_use's answers as ItemUseResult, the item's sequence picked from kiln.gamedata's JSON, the consume check, and
// UseResolved against a fake item_use (what it is sent, what it answers). The native checks themselves are
// native/services/abilities/tests/item_use_test.cpp; what the game does is the real-game check list in
// docs/kiln/services/abilities.md "Using an item".
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Kiln;
using Xunit;

namespace Kiln.AbilitiesTests;

public sealed unsafe class ItemUseTests
{
    [Fact]
    public void TheTableAndTheRequestHaveTheHeadersLayout()
    {
        // kiln_abilities.h: item_use at 312, the table 320; KilnItemUse 48 bytes (abilities.cpp's static_asserts)
        Assert.Equal(312, (int)Marshal.OffsetOf<AbilitiesNative.TableV2>(nameof(AbilitiesNative.TableV2.ItemUseFn)));
        Assert.Equal(320, sizeof(AbilitiesNative.TableV2));
        Assert.Equal(48, sizeof(AbilitiesNative.ItemUseRequest));
        Assert.Equal(8, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Slot)));
        Assert.Equal(16, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Item)));
        Assert.Equal(20, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Sequence)));
        Assert.Equal(24, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Flags)));
        Assert.Equal(28, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Status)));
        Assert.Equal(32, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.Count)));
        Assert.Equal(40, (int)Marshal.OffsetOf<AbilitiesNative.ItemUseRequest>(nameof(AbilitiesNative.ItemUseRequest.TriggerTime)));
    }

    [Fact]
    public void AnOlderTableHasNoItemUse()
    {
        var t = (AbilitiesNative.Table*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.TableV2));
        try
        {
            t->Header = new ServiceHeader { Size = 312, Version = 1 };  // a dev.29 / dev.30.2 kiln-abilities.dll
            Assert.False(AbilitiesNative.Has(t, nameof(AbilitiesNative.TableV2.ItemUseFn)));
            Assert.True(AbilitiesNative.Has(t, nameof(AbilitiesNative.TableV2.NativeEcsWrite)));
            t->Header = new ServiceHeader { Size = 320, Version = 1 };
            Assert.True(AbilitiesNative.Has(t, nameof(AbilitiesNative.TableV2.ItemUseFn)));
        }
        finally
        {
            NativeMemory.Free(t);
        }
    }

    [Theory]
    [InlineData(AbilitiesNative.UseStarted, ItemUseResult.Started)]
    [InlineData(AbilitiesNative.UseNotOwnSlot, ItemUseResult.NotOwnSlot)]
    [InlineData(AbilitiesNative.UseEmptySlot, ItemUseResult.EmptySlot)]
    [InlineData(AbilitiesNative.UseOtherItem, ItemUseResult.SlotChanged)]
    [InlineData(AbilitiesNative.UseNotAPlayer, ItemUseResult.NotAPlayer)]
    [InlineData(AbilitiesNative.UseDead, ItemUseResult.Dead)]
    [InlineData(AbilitiesNative.UseNotReady, ItemUseResult.NotReady)]
    [InlineData(AbilitiesNative.UseBusy, ItemUseResult.Busy)]
    public void EveryStatusHasItsResult(uint status, ItemUseResult result) =>
        Assert.Equal(result, Abilities.Outcome(AbilitiesNative.Ok, status));

    [Fact]
    public void NoServerWorldIsServerOnlyAndOtherErrorsAreNoResult()
    {
        Assert.Equal(ItemUseResult.ServerOnly, Abilities.Outcome(AbilitiesNative.WrongThread, 0));
        Assert.Null(Abilities.Outcome(AbilitiesNative.Ok, 99));  // a status this Kiln does not know
        Assert.Null(Abilities.Outcome(AbilitiesNative.InvalidArgument, 0));
        Assert.Null(Abilities.Outcome(7 /* KILN_ERR_INTERNAL */, 0));
    }

    private static readonly Guid Eat = new("2a9f0c1e-4b6d-4e8a-9f00-0000000000a1"), Drink = new("2a9f0c1e-4b6d-4e8a-9f00-0000000000a2");

    [Fact]
    public void TheUseIsThePrimarySequenceWithNoRequirements()
    {
        // kiln.gamedata's node: {path, type, value: [...]}
        var node = JsonNode.Parse($$"""
            {"path": "sequences", "value": [
              {"triggeredBy": "Secondary", "requirements": [], "sequence": "{{Drink}}"},
              {"triggeredBy": "Primary", "requirements": ["IsUnderWater"], "sequence": "{{Drink}}"},
              {"triggeredBy": "Primary", "requirements": [], "sequence": "{{Eat}}"}
            ]}
            """);
        Assert.Equal(Eat, Abilities.PrimarySequence(node));
        // only a Primary with requirements: that one
        Assert.Equal(Drink, Abilities.PrimarySequence(JsonNode.Parse($$"""[{"triggeredBy": "Primary", "requirements": ["IsUnderWater"], "sequence": "{{Drink}}"}]""")));
        // a plain array, requirements as a flag string
        Assert.Equal(Eat, Abilities.PrimarySequence(JsonNode.Parse($$"""[{"triggeredBy": "Primary", "requirements": "", "sequence": "{{Eat}}"}]""")));
    }

    [Fact]
    public void NoPrimarySequenceIsNone()
    {
        Assert.Null(Abilities.PrimarySequence(null));
        Assert.Null(Abilities.PrimarySequence(JsonNode.Parse("[]")));
        Assert.Null(Abilities.PrimarySequence(JsonNode.Parse($$"""[{"triggeredBy": "Tool", "sequence": "{{Eat}}"}]""")));
        Assert.Null(Abilities.PrimarySequence(JsonNode.Parse("""[{"triggeredBy": "Primary", "sequence": "00000000-0000-0000-0000-000000000000"}]""")));
        Assert.Null(Abilities.PrimarySequence(JsonNode.Parse("""[{"triggeredBy": "Primary", "sequence": "not a guid"}]""")));
    }

    [Fact]
    public void ConsumingEventsAreThoseThatUseUpTheItemButNotTheCostPayer()
    {
        // UseItem_Food_Buff's ApplyBuffEvent consumes; a staff's PayUsageCost has consume on but checks for no item
        Assert.Equal("keen::actor::ApplyBuffEvent", SequenceGuard.ConsumingEventIn(JsonNode.Parse("""
            [{"$type": "keen::actor::PlayAnimationEvent"}, {"$type": "keen::actor::ApplyBuffEvent", "consume": true}]
            """)));
        Assert.Null(SequenceGuard.ConsumingEventIn(JsonNode.Parse("""[{"$type": "keen::actor::PayUsageCost", "consume": true}]""")));
        Assert.Null(SequenceGuard.ConsumingEventIn(JsonNode.Parse("""[{"$type": "keen::actor::ApplyBuffEvent", "consume": false}]""")));
        Assert.Null(SequenceGuard.ConsumingEventIn(null));
        Assert.Equal("keen::actor::SpawnImpact", SequenceGuard.ConsumingEventIn(JsonNode.Parse("""[{"$type": "keen::actor::SpawnImpact", "consume": true}]""")));
    }

    // ---- GitHub #118: the repair kit, used up by its PayUsageCost (the game's data, Consumable_RepairKit_T3) ----
    private static readonly Guid RepairKit = new("e46613ca-9ca9-4398-b29b-76097fd720d1");  // UseItem_Consumable_RepairKit

    [Fact]
    public void APayUsageCostThatConsumesIsThePayingEventButNotAConsumingOne()
    {
        // UseItem_Consumable_RepairKit, its events as kiln.gamedata gives them (fields trimmed)
        var kit = JsonNode.Parse("""
            [{"$type": "keen::actor::SetStateEvent"}, {"$type": "keen::SfxNotifierEvent"},
             {"$type": "keen::actor::PayUsageCost", "time": 0.0667, "impactValues": {"simple": [], "scaled": []}, "ignoreCostMask": [], "consume": true},
             {"$type": "keen::actor::RepairAllItems", "time": 0.533, "percentage": 0.08}]
            """);
        Assert.Equal("keen::actor::PayUsageCost", SequenceGuard.PayingEventIn(kit));
        Assert.Null(SequenceGuard.ConsumingEventIn(kit));  // PlaySequence's guard is unchanged
        // a scroll's PayUsageCost pays its mana but consumes nothing; its ApplyBuffEvent consumes
        var scroll = JsonNode.Parse("""
            [{"$type": "keen::actor::PayUsageCost", "consume": false}, {"$type": "keen::actor::ApplyBuffEvent", "consume": true}]
            """);
        Assert.Null(SequenceGuard.PayingEventIn(scroll));
        Assert.Equal("keen::actor::ApplyBuffEvent", SequenceGuard.ConsumingEventIn(scroll));
        Assert.Null(SequenceGuard.PayingEventIn(JsonNode.Parse("""[{"$type": "keen::actor::ApplyBuffEvent", "consume": true}]""")));
        Assert.Null(SequenceGuard.PayingEventIn(null));
    }

    [Fact]
    public void ThePrimaryEntrySaysWhetherItNamesACostOverride()
    {
        Assert.Equal(new Abilities.ItemUseEntry(RepairKit, false), Abilities.PrimaryUse(JsonNode.Parse($$"""
            {"path": "sequences", "value": [{"triggeredBy": "Primary", "requirements": [], "sequence": "{{RepairKit}}",
              "costOverride": "00000000-0000-0000-0000-000000000000"}]}
            """)));
        Assert.Equal(new Abilities.ItemUseEntry(Eat, true), Abilities.PrimaryUse(JsonNode.Parse($$"""
            [{"triggeredBy": "Primary", "requirements": [], "sequence": "{{Eat}}", "costOverride": "{{Drink}}"}]
            """)));
        Assert.False(Abilities.HasCostOverride(null));
        Assert.False(Abilities.HasCostOverride(JsonValue.Create("")));
        Assert.False(Abilities.HasCostOverride(JsonValue.Create("0")));
        Assert.False(Abilities.HasCostOverride(JsonValue.Create(Guid.Empty.ToString())));
        Assert.True(Abilities.HasCostOverride(JsonValue.Create(Drink.ToString())));
        Assert.True(Abilities.HasCostOverride(JsonValue.Create("Ability_Blink_Cost")));
        Assert.True(Abilities.HasCostOverride(JsonNode.Parse("""{"guid": "x"}""")));
        Assert.Equal(Eat, Abilities.PrimarySequence(JsonNode.Parse($$"""[{"triggeredBy": "Primary", "sequence": "{{Eat}}", "costOverride": "{{Drink}}"}]""")));
    }

    [Fact]
    public void OnlyItemsThatAreNoAmmunitionPayWithThemselves()
    {
        static JsonNode V(string s) => JsonValue.Create(s);
        Assert.True(Abilities.IsUsable(V("Consumables"), V("Invalid")));   // the repair kit, the scrolls, food
        Assert.False(Abilities.IsUsable(V("Ammunition"), V("Spell")));     // 102 spells: staff casts with a consuming PayUsageCost
        Assert.False(Abilities.IsUsable(V("Consumables"), V("Arrow")));
        Assert.False(Abilities.IsUsable(null, V("Invalid")));               // unreadable: as in dev.30.3
        Assert.False(Abilities.IsUsable(V("Consumables"), null));
    }

    // ---- UseResolved against a fake item_use ----
    private static AbilitiesNative.ItemUseRequest _sent;
    private static int _rc;
    private static uint _status;
    private static int _rcWithFlags = -1;  // >= 0: answer this when a flag is set (an older kiln.abilities: InvalidArgument)
    private static readonly List<uint> _flags = new();

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeItemUse(AbilitiesNative.ItemUseRequest* u)
    {
        _sent = *u;
        _flags.Add(u->Flags);
        if (u->Flags != 0 && _rcWithFlags >= 0) return _rcWithFlags;
        u->Status = _status;
        u->Count = 5;
        u->TriggerTime = 1234;
        return _rc;
    }

    private static ItemUseResult Run(int rc, uint status, out AbilitiesNative.ItemUseRequest sent, bool payWithItem = false,
        bool consumes = true, int rcWithFlags = -1)
    {
        var t = (AbilitiesNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.TableV2));
        try
        {
            t->V1.Header = new ServiceHeader { Size = (uint)sizeof(AbilitiesNative.TableV2), Version = 1 };
            t->ItemUseFn = &FakeItemUse;
            _rc = rc;
            _status = status;
            _rcWithFlags = rcWithFlags;
            _flags.Clear();
            var r = Abilities.UseResolved(t, new EntityId(7), SlotId.Of(new EntityId(100), 2), new ItemId(0x1234), Eat, payWithItem, consumes);
            sent = _sent;
            return r;
        }
        finally
        {
            NativeMemory.Free(t);
        }
    }

    [Fact]
    public void ItemUseIsSentThePlayerTheSlotTheItemAndTheSequencesId()
    {
        Assert.Equal(ItemUseResult.Started, Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out var sent));
        Assert.Equal((uint)sizeof(AbilitiesNative.ItemUseRequest), sent.Size);
        Assert.Equal(7u, sent.Entity);
        Assert.Equal(100ul | (2ul << 32), sent.Slot);
        Assert.Equal(0x1234u, sent.Item);
        Assert.Equal(Abilities.IdOf(Eat), sent.Sequence);
        Assert.Equal(0u, sent.Flags);
        Assert.Equal(ItemUseResult.Busy, Run(AbilitiesNative.Ok, AbilitiesNative.UseBusy, out _));
        Assert.Equal(ItemUseResult.ServerOnly, Run(AbilitiesNative.WrongThread, 0, out _));
    }

    [Fact]
    public void AUseThatPaysWithTheItemAsksForTheCostItem()
    {
        // the repair kit: no consume event of its own, the item is the cost item
        Assert.Equal(ItemUseResult.Started, Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out var sent, payWithItem: true, consumes: false));
        Assert.Equal(AbilitiesNative.UseCostItem, sent.Flags);
        Assert.Equal([AbilitiesNative.UseCostItem], _flags);
        // food, a scroll: their own consume event, the item the cost item too (as the hand use)
        Assert.Equal(ItemUseResult.Started, Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out sent, payWithItem: true));
        Assert.Equal(AbilitiesNative.UseCostItem, sent.Flags);
        // no cost item (an entry with a costOverride, ammunition): flags 0, as in dev.30.3
        Assert.Equal(ItemUseResult.Started, Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out sent));
        Assert.Equal(0u, sent.Flags);
    }

    [Fact]
    public void AnOlderKilnAbilitiesNeverStartsTheRepairKitWithoutItsCostItem()
    {
        // dev.30.3 to 31.4 answer InvalidArgument for any flag: the kit would repair and never be taken, so it is not used
        Assert.Equal(ItemUseResult.NotConsumable,
            Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out _, payWithItem: true, consumes: false, rcWithFlags: AbilitiesNative.InvalidArgument));
        Assert.Equal([AbilitiesNative.UseCostItem], _flags);  // one call, never one without the flag
        // a build with no costItem in its reflection: unavailable, again never started without it
        Assert.Throws<KilnUnavailableException>(() =>
            Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out _, payWithItem: true, consumes: false, rcWithFlags: AbilitiesNative.Unavailable));
        Assert.Equal([AbilitiesNative.UseCostItem], _flags);
        // a scroll consumes by itself: used as in dev.30.3 (its mana unpaid), the second call without the flag
        Assert.Equal(ItemUseResult.Started,
            Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out var sent, payWithItem: true, rcWithFlags: AbilitiesNative.InvalidArgument));
        Assert.Equal([AbilitiesNative.UseCostItem, 0u], _flags);
        Assert.Equal(0u, sent.Flags);
        Assert.Equal(ItemUseResult.Started,
            Run(AbilitiesNative.Ok, AbilitiesNative.UseStarted, out _, payWithItem: true, rcWithFlags: AbilitiesNative.Unavailable));
        Assert.Equal([AbilitiesNative.UseCostItem, 0u], _flags);
    }

    [Fact]
    public void ANativeHandlerCannotUseAnItem()
    {
        // a sync native's or an attachment's handler runs inside the world's step: refused before anything is read
        var inside = typeof(Abilities).Assembly.GetType("Kiln.AbilitiesDispatch")!
            .GetField("_inside", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!;
        inside.SetValue(null, 1);
        try
        {
            var ex = Assert.Throws<InvalidOperationException>(() => Abilities.UseItem(new EntityId(7), SlotId.Of(new EntityId(100), 2)));
            Assert.Contains("native handler", ex.Message);
        }
        finally
        {
            inside.SetValue(null, 0);
        }
    }

    [Fact]
    public void AnUnknownSequenceIsNotConsumableAndErrorsThrow()
    {
        Assert.Equal(ItemUseResult.NotConsumable, Run(AbilitiesNative.NotFound, 0, out _));
        Assert.Throws<InvalidOperationException>(() => Run(7 /* KILN_ERR_INTERNAL */, 0, out _));
        Assert.Throws<InvalidOperationException>(() => Run(AbilitiesNative.Ok, 42, out _));
    }
}
