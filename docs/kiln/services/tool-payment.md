<!-- SPDX-License-Identifier: MIT -->
# Paid terrain placement

Since 0.2.0-dev.30.5.

A building tool's material pool defines the placement item, terrain material and `VoxelsPerItem` for each entry.
An Add action resolves the current terrain selection against that tool's immutable pool on the authoritative host.
The selection is not a caller-supplied price. An absent, stale, ambiguous or non-terrain selection refuses the add.

## Cost and sources

The game's own building operation supplies the terrain volume, in voxels. Kiln replaces only the material-cost list:

```text
quotient = double(floatVolume) / VoxelsPerItem
tolerance = max(0.000001, abs(quotient) * 4 * FLT_EPSILON)
zero volume: refuse without calling the cost builder
positive volume: max(1, ceil(quotient - tolerance)) items
```

The engine supplies a single-precision float volume; division is performed in double precision. The tolerance is at
least one millionth of an item and scales with the quotient to absorb a few float ULPs near exact multiples, including
larger strokes. Excess within that tolerance is treated as rounding noise; excess beyond it charges the next item.
A positive amount never becomes free. Invalid volumes, zero rates and counts beyond the supported integer range refuse.
The rate belongs to the placement item, not the material's mining drop.

The game pays that list **once**, from the player's normal building sources: backpack first, then connected magic-chest
stock permitted for that player and base. Ordinary chests and production-station outputs are not additional sources.
Kiln does not make a separate inventory withdrawal and does not route payment through a crafting recipe.

The normal building operation can reduce a placement when stock covers only part of its cost. This is not an
all-or-nothing stock reservation: the game's affordability calculation returns a fraction, discards its initial cost
list and rebuilds the list using the reduced volume. Kiln applies the same formula to that second evaluation. The
replaced initial list is neither paid nor recorded, so there is no double debit. With no eligible supplies, nothing
is placed or charged. With no added volume, no item is consumed.

## Refusal and undo

An allocation/list-append failure, an invalid cost or a fault while preparing costs makes the cost wrapper return zero
before the game applies the materials, records history or writes terrain. A changed final list also refuses; skipping
the cost builder is not a free-placement fallback. Vanilla tools and Remove actions retain their normal cost path.

The successful final list is the list the game pays and copies to the player's normal building-history entry. Normal
building undo therefore refunds the recorded placement item and count, not a newly calculated mining drop. The history
integration must also be available; a cost hook alone does not enable a tool's Add action.

These are the game's own building-operation semantics. Kiln does not promise additional atomic recovery from an engine
fault after the game's terrain writer has started. It never attempts to implement paid placement by joining independent
`Build.Add` and `Items.Take` calls, or by refunding a partially failed low-level voxel write.

## Capabilities and execution

`build.paid_terrain` reports whether the terrain-cost builder, affordability wrapper, material-list layout and both
scoped cost hooks are available on this executable. Missing mappings or a refused hook installation report an unavailable
capability with a reason; there is no unrestricted-edit fallback. Tool dispatch separately checks authority, an active
world/session, permissions, selection and the other required tool/history hooks. A joined client's selection or claimed
count is never payment authority.

The native hooks read the invocation's immutable policy on the game's worker thread. They do not call managed mod code,
retain borrowed game views or keep game pointers after the operation returns. The action binding must supply the selected
cell in its authoritative, invocation-local operation description; the payment hook only accepts a unique entry for that
cell in the immutable server policy, never an item, rate or count supplied by a client.

Installation checks the original executable prologues and accepts only original or Kiln-managed code state across the
qualified cost and append routines. Foreign, unknown or unreadable code refuses; retained Kiln detours do not prevent a
retry after a partial installation or a reinstall. Both links belong to a dedicated payment owner, not the tools service
owner. Teardown runs only after policy dispatch has stopped and drained: it clears the cost capability, revokes the
wrapper then builder, removes the payment owner's hooks, and retires that owner. The tools DLL stays resident and saved
hook continuations remain valid. This teardown is not a promise that revoking an owner drains worker-thread callbacks.

## Verification in a game

Only the player should run these checks, using a disposable test area inside a permitted building zone.

1. Place each pool material with backpack-only, magic-chest-only and mixed supplies. Confirm the pool's placement item,
   not its mining drop, is consumed at the configured rate.
2. Repeat over already filled terrain and a partly filled edge. Check that unchanged volume is not charged.
3. Leave fewer supplies than a full stroke needs. Confirm the normal reduced placement and its reduced cost; then remove
   all eligible supplies and confirm no placement. Supplies in an ordinary chest or a station output must not help.
4. Undo each successful add with the normal building-undo action, including after an interleaved vanilla build. Confirm
   the exact charged item/count returns, and a repeated undo cannot refund that operation twice.
5. Repeat on a hosted session with another player, including denied magic-chest access and a stale selection after
   switching tools. Confirm refusals change neither terrain nor supplies.
