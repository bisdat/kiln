// SPDX-License-Identifier: MIT
// Pure predicate for ui-hooks.c2 and c3 (the identity of a clicked slot): compiled into client-pass and Contract.Tests.
// The game draws an item entity (pide) only for items that have one (weapons, tools, armour, cans: HasPIDE); a plain
// stack (materials, consumables) has none, so its SlotClick.ItemEntity is 0 as documented (docs/kiln/ui.md, "The item's
// identity"; docs/re/game-ui.md: "Stackable views can have pide=0"). A click is judged against the holder's inventory as
// the server world holds it (hosted: the same process).
using Kiln;
namespace ClientPass;

internal enum SlotIdentity
{
    /// <summary>The click's entity is an entity of that item in the server's inventory.</summary>
    Match,
    /// <summary>No entity, and the server holds the item only as plain stacks (no item entity): right for a stack.</summary>
    PlainStack,
    /// <summary>No entity, but the server holds that item with an item entity: the client lost it.</summary>
    EntityMissing,
    /// <summary>An entity the server's inventory does not hold for that item.</summary>
    Mismatch,
    /// <summary>The server's inventory holds no such item (or none was read).</summary>
    NotHeld,
}

internal static class SlotIdentityProof
{
    public static SlotIdentity Judge(ItemId item, EntityId entity, IEnumerable<(ItemId Item, EntityId Entity)>? server)
    {
        var same = (server ?? []).Where(s => item.Value != 0 && s.Item == item).ToArray();
        if (same.Length == 0) return SlotIdentity.NotHeld;
        if (entity.Value != 0) return same.Any(s => s.Entity == entity) ? SlotIdentity.Match : SlotIdentity.Mismatch;
        return same.All(s => s.Entity.Value == 0) ? SlotIdentity.PlainStack : SlotIdentity.EntityMissing;
    }

    /// <summary>What the API documents: an item entity where the item has one, none for a plain stack.</summary>
    public static bool Consistent(SlotIdentity v) => v is SlotIdentity.Match or SlotIdentity.PlainStack;
}
