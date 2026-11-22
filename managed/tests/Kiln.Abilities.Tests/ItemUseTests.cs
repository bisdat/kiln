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

    // ---- UseResolved against a fake item_use ----
    private static AbilitiesNative.ItemUseRequest _sent;
    private static int _rc;
    private static uint _status;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FakeItemUse(AbilitiesNative.ItemUseRequest* u)
    {
        _sent = *u;
        u->Status = _status;
        u->Count = 5;
        u->TriggerTime = 1234;
        return _rc;
    }

    private static ItemUseResult Run(int rc, uint status, out AbilitiesNative.ItemUseRequest sent)
    {
        var t = (AbilitiesNative.TableV2*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesNative.TableV2));
        try
        {
            t->V1.Header = new ServiceHeader { Size = (uint)sizeof(AbilitiesNative.TableV2), Version = 1 };
            t->ItemUseFn = &FakeItemUse;
            _rc = rc;
            _status = status;
            var r = Abilities.UseResolved(t, new EntityId(7), SlotId.Of(new EntityId(100), 2), new ItemId(0x1234), Eat);
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
