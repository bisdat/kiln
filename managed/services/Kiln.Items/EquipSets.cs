// SPDX-License-Identifier: MIT
// Equipment sets on the managed side: kiln.items v6 (native/core/abi/services/kiln_items.h "equipment sets"; chapter 10
// of the dev.28 dossier). CaptureSet reads a player's 20 equipment slots; the native snapshot (set_capture) is what a
// plan and an apply are checked against. Capability items.equip_sets.
namespace Kiln;

public static unsafe partial class Items
{
    private static ItemsNative.TableV6* _v6;

    // The v6 table, or null when the native service is older.
    internal static ItemsNative.TableV6* V6
    {
        get
        {
            if (_v6 != null) return _v6;
            var t = T;
            if (t == null || t->Header.Size < (uint)sizeof(ItemsNative.TableV6)) return null;
            return _v6 = (ItemsNative.TableV6*)t;
        }
    }

    /// <summary>
    /// Reads all 20 equipment slots of <paramref name="player"/> as an <see cref="EquipmentSet"/>: the item worn in each
    /// (<see cref="InstanceEquipment"/> for an item with its own entity, <see cref="StackEquipment"/> for a plain stack such
    /// as arrows or bait) and <see cref="EmptyEquipment"/> for an empty slot. The set is scoped to this process, this
    /// server world and this session of the player (<see cref="EquipmentSet.Scope"/>): apply it, or a subset of it, while
    /// the player stays; it is refused (<see cref="EquipmentSetStatus.StaleSnapshot"/>) after a restart, in another world,
    /// or after the player left or rejoined. Keep sets for the current session only; nothing is saved.
    /// </summary>
    /// <param name="player">The player character.</param>
    /// <returns>Ok with the set; not Ok with the reason when the entity is not a player character with an identity
    /// kiln.decisions can tell apart, or a slot points outside the player's inventories.</returns>
    /// <remarks>Reads only; changes nothing and holds no snapshot. Hotbar, main hand and the building materials are not
    /// part of a set.</remarks>
    /// <exception cref="ArgumentException"><paramref name="player"/> is none.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, or called inside the server world's step (call
    /// it in Pre or Post: a mod's tick and event handlers run there).</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.equip_sets</c> is off.</exception>
    /// <example><code>
    /// var saved = Items.CaptureSet(player);
    /// if (saved.Ok) sets["day"] = saved.Set!;  // this session only
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.EquipSets)]
    public static EquipmentSetCaptureResult CaptureSet(EntityId player)
    {
        Services.RequireTickThread("Items.CaptureSet");
        if (player.IsNone) throw new ArgumentException("no player", nameof(player));
        RequireSets();
        var w = WindowNow("Items.CaptureSet");
        var rc = DecisionBroker.ResolveEntity(player, out var key, out _);
        if (rc == DecisionsNative.WrongThread) throw new InvalidOperationException("Items.CaptureSet: the server world is inside its step (call it in Pre or Post)");
        if (rc == DecisionsNative.Unavailable) throw new KilnUnavailableException(Caps.EquipSets, "kiln.decisions is off: the player's identity cannot be read");
        if (rc != DecisionsNative.Ok) return new EquipmentSetCaptureResult(false, $"no player {player} with an identity kiln.decisions can tell apart (no such entity, or none with a session)", null);
        var sel = PlayerSlots.Selection(player);
        if (sel == null) return new EquipmentSetCaptureResult(false, $"entity {player} has no equipment (not a player character)", null);
        var inventory = Read(player);
        if (inventory == null) return new EquipmentSetCaptureResult(false, $"entity {player} has no inventory", null);
        var entries = new EquipmentSetEntry[EquipmentSetRules.SlotCount];
        foreach (var slot in Enum.GetValues<EquipmentSlot>())
        {
            var (selection, why) = SelectionAt(PlayerSlots.Equipment(sel, slot), inventory.Slots);
            if (selection is null) return new EquipmentSetCaptureResult(false, $"{slot}: {why}", null);
            entries[(int)slot] = new EquipmentSetEntry(slot, selection);
        }
        return new EquipmentSetCaptureResult(true, null, new EquipmentSet(new EquipmentSetScope(w.ProcessSession, w.WorldEpoch, key), entries));
    }

    /// <summary>What an equipment slot that points at <paramref name="at"/> holds, from a read of the player's inventory
    /// (pure: the unit tests build the slots by hand). Null with the reason when it cannot be named.</summary>
    internal static (EquipmentSetSelection? Selection, string? Why) SelectionAt(SlotId at, IReadOnlyList<InventorySlot> slots)
    {
        if (at.IsNone) return (new EmptyEquipment(), null);
        InventorySlot? s = null;
        foreach (var x in slots)
            if (x.Slot == at)
            {
                s = x;
                break;
            }
        if (s is null) return (null, $"the equipment slot points at {at}, which is not one of the player's inventories");
        if (s.IsEmpty) return (new EmptyEquipment(), null);
        if (!s.Ref.IsNone) return (new InstanceEquipment(s.Ref), null);
        if (!s.ItemEntity.IsNone) return (null, $"the item in {at} has an item entity kiln.items gave no stamp (items.instances)");
        return (new StackEquipment(s.Item, at, s.Count), null);
    }

    // ---- the native snapshot (PlanSet, EquipSet) ----

    /// <summary>The calling mod's native owner (Kiln's own, 0, outside mod code): snapshots belong to it.</summary>
    internal static ulong CallerOwner => ModContext.CurrentMod?.Owner ?? 0;

    /// <summary>kiln.items' set_capture for <paramref name="owner"/>. Returns the KilnResult and the snapshot (with the
    /// refusal reason when it was refused).</summary>
    internal static int NativeCapture(ulong owner, EntityId player, out ItemsNative.SetSnapshot snap, out string reason)
    {
        snap = default;
        snap.Size = (uint)sizeof(ItemsNative.SetSnapshot);
        reason = "";
        var t = RequireSets();
        int rc;
        fixed (ItemsNative.SetSnapshot* p = &snap) rc = t->SetCapture(owner, player.Value, p);
        fixed (byte* r = snap.Reason) reason = ItemsNative.Text(r, 160);
        return rc;
    }

    /// <summary>Gives a snapshot back (a plan's Dispose). Any thread.</summary>
    internal static void ReleaseSnapshot(ulong owner, ulong token)
    {
        var t = V6;
        if (t != null && token != 0) t->SetRelease(owner, token);
    }

    // ---- the capability ----

    /// <summary>kiln.items' set_status; null when it cannot be read.</summary>
    internal static (bool Available, bool Broken, string Detail)? SetStatusOf(ItemsNative.TableV6* t)
    {
        ItemsNative.SetStatus s = default;
        s.Size = (uint)sizeof(ItemsNative.SetStatus);
        if (t->GetSetStatus(&s) != ItemsNative.Ok) return null;
        return (s.Available != 0, s.Broken != 0, ItemsNative.Text(s.Detail, 160));
    }

    private static ItemsNative.TableV6* RequireSets()
    {
        var t = V6;
        if (t == null)
            throw new KilnUnavailableException(Caps.EquipSets, T == null ? "kiln.items is not loaded" : $"kiln.items v{T->Header.Version} has no equipment sets (update Kiln)");
        if (!Can(Caps.EquipSets)) throw new KilnUnavailableException(Caps.EquipSets, Capabilities.Get(Caps.EquipSets).ReasonText);
        if (SetStatusOf(t) is { Available: false } s) throw new KilnUnavailableException(Caps.EquipSets, s.Detail);
        return t;
    }

    // The window clock now, in a phase the world may be read in.
    private static WindowState WindowNow(string api)
    {
        if (DecisionBroker.Window() is not { } w) throw new KilnUnavailableException(Caps.EquipSets, "kiln.window_clock is not here (an older core)");
        if (w.WorldEpoch == 0 || w.Phase is DecisionsNative.PhaseNone or DecisionsNative.PhaseStopping)
            throw new KilnUnavailableException(Caps.EquipSets, "no server world is running in this process");
        if (!w.WorldSafePhase) throw new InvalidOperationException($"{api}: the server world is inside its step (call it in Pre or Post)");
        return w;
    }
}
