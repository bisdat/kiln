# Gem Forge candidates

`Ui.Values.GemForgeCandidates()` observes the game's returned **GEMS** list for the local player's inventories.
It includes rows outside the visible scroll area. It does not search nearby Magic Chests. Membership does not say
whether an action is allowed, compatible, enabled, or will succeed. These client identities are not authority for
server actions.

```csharp
if (Capabilities.Is(Ui.Values.GemForgeCandidatesCapability))
{
    var result = Ui.Values.GemForgeCandidates();
    if (result.Ok)
        foreach (var row in result.Snapshot!.Candidates)
            Log.Info($"{row.Index}: {row.DisplayedGem.Item}");
}
```

The API requires `ui.gemforge.inspect`, is client only, and may be called from any thread. Missing capability,
service or appended native table throws `KilnUnavailableException`. A closed or undrawn GEMS grid returns
`Ok = false`, a reason, and no snapshot. An empty returned list is a successful snapshot with zero candidates.

**Which gems are listed.** The GEMS grid lists the gems in your backpack and action bars, then the weapons there that
hold a gem (shown with the gem), in every forge mode; on a Kiln client where a mod allowed sockets on other gear
(`Gems.AllowSockets`), also the rings, armour pieces and shields there that hold a gem, as carrier rows like a weapon
([services/gems-sockets.md](services/gems-sockets.md)). Gems in chests and in worn items are not listed. (The game's
list builder has one caller, and it always asks for socketed weapons too: there is no mode that hides them. A Recycle
removes a row for good; an Insert keeps the count, the loose gem becomes its weapon's row.) More on gems:
[services/gems.md](services/gems.md).

Each immutable `GemForgeCandidate` distinguishes:

- `SourceRow`: the returned row view and its selection key (page and slot).
- `DisplayedGem`: the displayed gem and its actual source page and slot.
- `WeaponCarrier`: the weapon view when the gem is already socketed; otherwise null. Its row key is the weapon's
  inventory slot, while the gem source is the weapon's client entity with slot zero.

The snapshot includes the selected row (`SlotId?`), associated selected weapon (`UiItemView?`), the native mode byte
(0 through 4), local player, session, frame, capture time, and a Kiln generation. All snapshots describe the GEMS role;
the WEAPONS grid is not captured. Selection describes the context at grid entry. Eligibility and disabled state
are unknown. Native producer allocation failures are not exposed by the game, so this is not a certified complete
inventory enumeration.

`SubscribeGemForgeCandidates(onFrame)` returns a `UiValueRegistration`. It calls `onFrame` on the client main thread
after each completed UI frame, including unavailable frames. Keep callbacks short. The copied snapshot is yours to
keep. Dispose the registration when finished; mod unload also removes it.

Kiln retains two copied frames for a sizing retry. A mode, selected identity, list, screen or session change invalidates
old generations. A selection change in either grid or a later menu action discards that frame; the next coherent draw can replace it.
Closing the selector, an absent GEMS draw, capability loss, a newer completed frame without a matching capture, or an
age over 1000 ms makes the current result unavailable. Saved snapshots remain historical values.

This feature initially supports only measured client build 26430625
(SHA-256 `5d9ef2f140b8ed49e30dbee655841b89d08baec20f799a1817e171a18a850c77`). It needs `ui.screens` and Kiln's immutable
item catalog to distinguish loose gems from weapon carriers. Hooks install on first read or subscription. Unsupported
signatures, layouts, call edges or hook contracts leave the feature off. An unreadable or incoherent list disables
only this inspection feature for the process and records a reason in `kiln.caps` and `kiln/state/capabilities.json`.
There is no chest extension or mutation API here. Real-client acceptance is still required before release.
