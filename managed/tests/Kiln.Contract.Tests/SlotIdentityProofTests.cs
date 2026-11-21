// SPDX-License-Identifier: MIT
// ui-hooks.c2/c3's identity rule (mods/tests/client-pass/SlotIdentityProof.cs). The owner's dev.30 pass (T1 462d1755,
// hosted) Ctrl+clicked Archaic Essence IV x10 in the backpack and Springlands Dirt x12 in a chest: plain stacks, drawn by the
// game without an item entity, so ItemEntity 0 is the documented answer and the checks' old "its entity" assertion was wrong.
using ClientPass;
using Kiln;
using Xunit;

namespace Kiln.Contract.Tests;

public sealed class SlotIdentityProofTests
{
    private static readonly ItemId Essence = new(0xd4480073), Dirt = new(0x5cdef690), Sword = new(0xAA);

    [Fact]
    public void APlainStackWithoutAnEntityIsConsistent()
    {
        var chest = new[] { (Dirt, EntityId.None), (Essence, EntityId.None) };
        Assert.Equal(SlotIdentity.PlainStack, SlotIdentityProof.Judge(Dirt, EntityId.None, chest));
        Assert.True(SlotIdentityProof.Consistent(SlotIdentityProof.Judge(Dirt, EntityId.None, chest)));
    }

    [Fact]
    public void GearMustCarryTheServersEntity()
    {
        var pack = new[] { (Sword, new EntityId(900)), (Sword, new EntityId(901)), (Essence, EntityId.None) };
        Assert.Equal(SlotIdentity.Match, SlotIdentityProof.Judge(Sword, new EntityId(901), pack));
        Assert.Equal(SlotIdentity.EntityMissing, SlotIdentityProof.Judge(Sword, EntityId.None, pack));
        Assert.Equal(SlotIdentity.Mismatch, SlotIdentityProof.Judge(Sword, new EntityId(902), pack));
        Assert.False(SlotIdentityProof.Consistent(SlotIdentity.EntityMissing));
        Assert.False(SlotIdentityProof.Consistent(SlotIdentity.Mismatch));
    }

    [Fact]
    public void AnItemTheServerDoesNotHoldIsNeverConsistent()
    {
        Assert.Equal(SlotIdentity.NotHeld, SlotIdentityProof.Judge(Sword, new EntityId(900), null));
        Assert.Equal(SlotIdentity.NotHeld, SlotIdentityProof.Judge(new ItemId(0), EntityId.None, [(new ItemId(0), EntityId.None)]));
        Assert.Equal(SlotIdentity.NotHeld, SlotIdentityProof.Judge(Dirt, EntityId.None, [(Essence, EntityId.None)]));
        Assert.False(SlotIdentityProof.Consistent(SlotIdentity.NotHeld));
    }
}
