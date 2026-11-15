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

    /// <summary>
    /// Checks <paramref name="set"/> against a snapshot of <paramref name="player"/>'s inventory taken now and returns the
    /// plan: the game's equip and unequip calls the one transaction would run, in order, and how many free backpack slots
    /// it needs on the way. Slots the set leaves out stay as they are; <see cref="EmptyEquipment"/> takes off what a slot
    /// holds. Nothing changes. Apply the plan with <c>Items.EquipSet(plan)</c> before
    /// <see cref="EquipmentSetPlan.ValidThroughStep"/>, or dispose it.
    /// </summary>
    /// <param name="player">The player character (the set's own player).</param>
    /// <param name="set">The set (from <see cref="CaptureSet"/>, a subset of one, or one built with its scope).</param>
    /// <returns>Ok with the plan (no steps when every slot holds already). Not Ok, with the reason and nothing held, when
    /// the set's scope is stale (another process, server world or session of the player), the snapshot budget is used up
    /// (16 per mod, 256 in all), an item or stack is not where the set says, an item does not fit its slot, an unequip
    /// finds no free backpack slot (<c>NoRoom</c>), items would move between equipment slots in a cycle
    /// (<c>UnsupportedCycle</c>), the backpack would change, or an item is worn in a slot the set leaves out. A reason
    /// starts with its code (<c>NoRoom: ...</c>).</returns>
    /// <remarks>The plan holds a snapshot until it is applied, disposed, 250 steps pass, the mod unloads or the world
    /// stops. Its steps are advisory: the apply plans again from its own snapshot inside the transaction.</remarks>
    /// <exception cref="ArgumentException"><paramref name="player"/> is none or not the set's player, or the set breaks
    /// the rules of a set (a zero scope, a null entry, a slot named twice or not an equipment slot, an item or stack
    /// selected twice, a malformed selection).</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, inside the server world's step, or the calling
    /// mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.equip_sets</c> is off.</exception>
    /// <example><code>
    /// using var plan = Items.PlanSet(player, set).Plan;
    /// if (plan is { Steps.Count: > 0 }) Log.Info($"{plan.Steps.Count} steps, {plan.RequiredFreeSlots} free slot(s)");
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.EquipSets)]
    public static EquipmentSetPlanResult PlanSet(EntityId player, EquipmentSet set)
    {
        Services.RequireTickThread("Items.PlanSet");
        if (player.IsNone) throw new ArgumentException("no player", nameof(player));
        var entries = EquipmentSetRules.Check(set, nameof(set));
        if (set.Scope.Player.Entity != player.Value) throw new ArgumentException($"the set is player {set.Scope.Player.Entity}'s, not {player}'s", nameof(set));
        var t = RequireSets();
        var w = WindowNow("Items.PlanSet");
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var owner = mod?.Owner ?? 0;
        var snap = Snapshot(t, owner, player, set.Scope, w, "Items.PlanSet");
        if (!snap.Ok) return new EquipmentSetPlanResult(false, snap.Reason, null);
        var ops = OpsOf(entries);
        var plan = new ItemsNative.SetPlan { Size = (uint)sizeof(ItemsNative.SetPlan) };
        int rc;
        fixed (EquipDecisions.SetOp* e = ops) rc = t->SetPlanOf(owner, snap.Token, e, (uint)ops.Length, &plan);
        if (rc != ItemsNative.Ok)
        {
            ReleaseSnapshot(owner, snap.Token);
            if (rc == ItemsNative.InvalidArgument) throw new ArgumentException("Items.PlanSet: kiln.items refused the set's entries", nameof(set));
            throw new KilnUnavailableException(Caps.EquipSets, SetStatusOf(t)?.Detail is { Length: > 0 } d ? d : $"kiln.items refused the plan (error {rc})");
        }
        if (plan.Status is not (ItemsNative.SetPlanPlanned or ItemsNative.SetPlanNoChange))
        {
            ReleaseSnapshot(owner, snap.Token);
            return new EquipmentSetPlanResult(false, ReasonOf(plan.Refusal, ItemsNative.Text(plan.Reason, 256)), null);
        }
        var steps = new EquipmentSetStep[plan.StepCount];
        var raw = (ItemsNative.SetStep*)plan.Steps;
        for (var i = 0; i < steps.Length; i++) steps[i] = StepOf(raw[i]);
        var lease = Lease(snap.Token, owner, plan.ValidThroughStep, mod);
        return new EquipmentSetPlanResult(true, null, new EquipmentSetPlan(player, new EquipmentSet(set.Scope, entries),
            new InventorySnapshotToken(snap.Token, snap.WorldEpoch, player), plan.ValidThroughStep, steps, (int)plan.RequiredFreeSlots) { Lease = lease });
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

    // A snapshot for a set: the scope checked against now, then kiln.items' set_capture. Not Ok: the reason (nothing held).
    internal readonly record struct Snap(bool Ok, string? Reason, ulong Token, ulong WorldEpoch, ulong CapturedStep);

    internal static Snap Snapshot(ItemsNative.TableV6* t, ulong owner, EntityId player, EquipmentSetScope scope, WindowState w, string api)
    {
        var rc = DecisionBroker.ResolveEntity(player, out var key, out _);
        if (rc == DecisionsNative.WrongThread) throw new InvalidOperationException($"{api}: the server world is inside its step (call it in Pre or Post)");
        if (rc == DecisionsNative.Unavailable) throw new KilnUnavailableException(Caps.EquipSets, "kiln.decisions is off: the player's identity cannot be read");
        if (rc != DecisionsNative.Ok) return Refused($"StaleSnapshot: no player {player} in the world with an identity kiln.decisions can tell apart (the player left)");
        if (EquipmentSetRules.Stale(scope, w.ProcessSession, w.WorldEpoch, key) is { } stale) return Refused("StaleSnapshot: " + stale);
        rc = NativeCapture(owner, player, out var snap, out var reason);
        switch (rc)
        {
            case ItemsNative.Ok:
                if (snap.Identity.ToPublic() != scope.Player || snap.WorldEpoch != scope.WorldEpoch)
                {
                    ReleaseSnapshot(owner, snap.Token);
                    return Refused("StaleSnapshot: the player's session changed while the snapshot was taken");
                }
                return new Snap(true, null, snap.Token, snap.WorldEpoch, snap.CapturedStep);
            case ItemsNative.BufferTooSmall:
                return Refused(ReasonOf(ItemsNative.SetRefusalBudget, reason));
            case ItemsNative.NotFound or ItemsNative.InternalError:
                return Refused(ReasonOf(snap.Refusal, reason.Length > 0 ? reason : $"kiln.items refused the snapshot ({rc})"));
            case ItemsNative.WrongThread:
                throw new InvalidOperationException($"{api}: the server world is inside its step (call it in Pre or Post)");
            case ItemsNative.InvalidArgument:
                throw new ArgumentException($"{api}: kiln.items refused the player", nameof(player));
            default:
                throw new KilnUnavailableException(Caps.EquipSets, SetStatusOf(t)?.Detail is { Length: > 0 } d ? d : $"kiln.items refused the snapshot (error {rc})");
        }
    }

    private static Snap Refused(string reason) => new(false, reason, 0, 0, 0);

    /// <summary>A refusal's code name and text: <c>NoRoom: ...</c>.</summary>
    internal static string ReasonOf(uint refusal, string text)
    {
        var name = refusal switch
        {
            ItemsNative.SetRefusalNoPlayer => "NoPlayer",
            ItemsNative.SetRefusalNoIdentity => "StaleSnapshot",
            ItemsNative.SetRefusalShape => "UnsupportedInventory",
            ItemsNative.SetRefusalBudget => "SnapshotBudgetExceeded",
            5 => "StaleSnapshot",
            6 => "NotHere",
            7 => "SlotType",
            8 => "NoRoom",
            9 => "UnsupportedCycle",
            10 => "UnsupportedBackpack",
            11 => "OmittedSlotWouldChange",
            12 => "StackMerge",
            13 => "TooManySteps",
            _ => "Refused",
        };
        return text.Length > 0 ? $"{name}: {text}" : name;
    }

    /// <summary>The wire entries of a set (<c>KilnSetOpV1</c>): an item by its entity, id and stamp; a stack by its item,
    /// source slot and count; an empty slot by its slot alone.</summary>
    internal static EquipDecisions.SetOp[] OpsOf(IReadOnlyList<EquipmentSetEntry> entries)
    {
        var ops = new EquipDecisions.SetOp[entries.Count];
        for (var i = 0; i < ops.Length; i++)
        {
            var e = entries[i];
            ref var op = ref ops[i];
            op.Size = (uint)sizeof(EquipDecisions.SetOp);
            op.EquipmentSlot = GameSlot(e.Slot);
            switch (e.Selection)
            {
                case InstanceEquipment inst:
                    op.Kind = 1;
                    op.ItemEntity = inst.Item.Entity.Id.Value;
                    op.ItemId = inst.Item.Item.Value;
                    op.ExpectedCount = 1;
                    op.InstanceStamp = EquipmentSetRules.StampOf(inst.Item);
                    break;
                case StackEquipment stack:
                    op.Kind = 2;
                    op.ItemId = stack.Item.Value;
                    op.ExpectedCount = (uint)stack.ExpectedCount;
                    op.SourceSlot = stack.Source.Value;
                    break;
            }
        }
        return ops;
    }

    /// <summary>A plan step from the native one.</summary>
    internal static EquipmentSetStep StepOf(in ItemsNative.SetStep s)
    {
        var slot = SlotOfGame(s.EquipmentSlot) ?? throw new InvalidOperationException($"kiln.items planned a step for {s.EquipmentSlot}, which is no equipment slot");
        EquipmentSetSelection selection = s.Op == ItemsNative.SetStepUnequip || s.Kind == 0 ? new EmptyEquipment()
            : s.Kind == 1 ? new InstanceEquipment(ItemRefs.Of(s.ItemEntity, s.ItemId, (uint)s.InstanceStamp))
            : new StackEquipment(new ItemId(s.ItemId), new SlotId(s.SourceSlot), (int)s.Count);
        return new EquipmentSetStep((int)s.Index, slot, selection);
    }

    // ---- plans' snapshots and the mods that hold them ----

    private static readonly object LeaseGate = new();
    private static readonly Dictionary<ModContext, HashSet<EquipmentSetLease>> LeasesByMod = new();

    /// <summary>A plan's hold on a snapshot. A mod's held snapshots are released when it unloads.</summary>
    internal static EquipmentSetLease Lease(ulong token, ulong owner, ulong validThrough, ModContext? mod)
    {
        var lease = new EquipmentSetLease(token, owner, ReleaseSnapshot, validThrough);
        if (mod == null) return lease;
        var now = DecisionBroker.Window()?.Step ?? 0;
        lock (LeaseGate)
        {
            if (!LeasesByMod.TryGetValue(mod, out var held))
            {
                LeasesByMod[mod] = held = [];
                mod.Retiring += () => ReleaseLeases(mod);
            }
            held.RemoveWhere(l => !l.Held || l.ValidThrough < now);  // used, released or gone natively
            held.Add(lease);
        }
        return lease;
    }

    private static void ReleaseLeases(ModContext mod)
    {
        EquipmentSetLease[] held;
        lock (LeaseGate)
        {
            if (!LeasesByMod.Remove(mod, out var set)) return;
            held = [.. set];
        }
        foreach (var l in held) l.Release();
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
