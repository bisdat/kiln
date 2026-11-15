// SPDX-License-Identifier: MIT
// Equipment sets on the managed side: kiln.items v6 (native/core/abi/services/kiln_items.h "equipment sets"; chapter 10
// of the dev.28 dossier). CaptureSet reads a player's 20 equipment slots; the native snapshot (set_capture) is what a
// plan and an apply are checked against; EquipSet submits one set as one transaction through Kiln's own owner (0) and
// names the asking mod as the snapshot's owner, so a commit is reported (EquipmentSetChangedEvent) even when that mod
// unloads first. Capability items.equip_sets.
using System.ComponentModel;
using System.Diagnostics.CodeAnalysis;
#pragma warning disable KILNX001  // LabEquipSet and its LabSetFault are this file's own lab members
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Kiln;

/// <summary>Lab only: a fault injected into a real run of an equipment set (<see cref="Items.LabEquipSet"/>). Needs
/// <c>kiln/data/kiln.items/sets-lab</c> when the server starts; never on a player's server.</summary>
[Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
[EditorBrowsable(EditorBrowsableState.Never)]
public enum LabSetFault
{
    /// <summary>No fault.</summary>
    None = 0,
    /// <summary>The game's equip at the given step is called with an empty source slot: a real refusal mid-way.</summary>
    RefuseStep = 1,
    /// <summary>The commit's own rollback bit (0x40) is set after the steps: refused before the commit.</summary>
    GuardBit = 2,
    /// <summary>The bit is set and the commit is called anyway: the game's commit rolls back by itself.</summary>
    CommitAnyway = 3,
    /// <summary>After the commit the request is read as unmet: InvariantFailure (equipment sets go off until the server
    /// restarts).</summary>
    FakeMismatch = 4,
    /// <summary>The set is canceled right after it was queued (set_cancel, as when the asking mod unloads).</summary>
    CancelAtOnce = 5,
}

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

    /// <summary>
    /// Changes <paramref name="player"/>'s equipment to <paramref name="set"/> in one inventory transaction: every slot
    /// the set names changes, or none does. Slots the set leaves out stay as they are; <see cref="EmptyEquipment"/> takes
    /// off what a slot holds (into a free backpack slot). Takes a snapshot now and plans from it (as
    /// <see cref="PlanSet"/>); the transaction runs a step or two later inside the server's own equip program, after one
    /// <see cref="PlayerEquippingEvent"/> for the whole set (<see cref="SetEquipmentChange"/>): a handler that cancels it
    /// stops it. The change reaches the player as their own equips do.
    /// </summary>
    /// <param name="player">The player character (the set's own player).</param>
    /// <param name="set">The set (from <see cref="CaptureSet"/>, a subset of one, or one built with its scope).</param>
    /// <param name="options"><see cref="EquipOptions.DryRun"/>: run the transaction and roll it back; nothing changes.</param>
    /// <returns>Completes on the tick thread with the result, once. <see cref="EquipmentSetResult.Ok"/> for Done, NoChange
    /// and Simulated; anything else changed nothing, except <see cref="EquipmentSetStatus.InvariantFailure"/>
    /// (<see cref="EquipmentSetResult.MayHaveMutated"/>, and equipment sets go off until the server restarts). A stale
    /// scope or snapshot, a full budget or queue and a refusal of the planner or the game are results, not exceptions.</returns>
    /// <remarks>Order: FIFO with the player's single <see cref="Equip"/> and <see cref="Unequip"/> requests, at most one set
    /// per player and step. A set that did not run within 250 steps ends <see cref="EquipmentSetStatus.Dropped"/>, as does
    /// a set whose server world stops, or whose mod unloads before it ran (a set that already ran is still reported).
    /// The player's hotbar, main hand and building materials are never part of a set. Sets are for the current session
    /// only: nothing is saved, and a set of another session is refused.</remarks>
    /// <exception cref="ArgumentException"><paramref name="player"/> is none or not the set's player, unknown options, or
    /// the set breaks the rules of a set.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, inside the server world's step, or the calling
    /// mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.equip_sets</c> is off.</exception>
    /// <example><code>
    /// EquipmentSetResult r = await Items.EquipSet(player, sets["night"]);
    /// if (!r.Ok) Log.Info($"not applied: {r.Status} {r.Reason}");
    /// </code></example>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.EquipSets)]
    public static Task<EquipmentSetResult> EquipSet(EntityId player, EquipmentSet set, EquipOptions options = EquipOptions.None) =>
        ApplySet(player, set, options, LabSetFault.None, 0, "Items.EquipSet");

    /// <summary>Lab only: <see cref="EquipSet(EntityId, EquipmentSet, EquipOptions)"/> with a fault injected into the real
    /// run (<see cref="LabSetFault"/>). Throws <see cref="ArgumentException"/> unless <c>kiln/data/kiln.items/sets-lab</c>
    /// existed when the server started.</summary>
    [Experimental("KILNX001", UrlFormat = "https://github.com/frostdev-ops/kiln/blob/main/docs/kiln/modding/analyzers.md#kilnx001")]
    [EditorBrowsable(EditorBrowsableState.Never)]
    [ServerOnly]
    [TickThreadOnly]
    public static Task<EquipmentSetResult> LabEquipSet(EntityId player, EquipmentSet set, LabSetFault fault, int step = 0, EquipOptions options = EquipOptions.None)
    {
        if (!Enum.IsDefined(fault)) throw new ArgumentException($"unknown fault {fault}", nameof(fault));
        if (step is < 0 or > 255) throw new ArgumentOutOfRangeException(nameof(step));
        return ApplySet(player, set, options, fault, step, "Items.LabEquipSet");
    }

    private static Task<EquipmentSetResult> ApplySet(EntityId player, EquipmentSet set, EquipOptions options, LabSetFault fault, int step, string api)
    {
        Services.RequireTickThread(api);
        if (player.IsNone) throw new ArgumentException("no player", nameof(player));
        var entries = EquipmentSetRules.Check(set, nameof(set));
        if (set.Scope.Player.Entity != player.Value) throw new ArgumentException($"the set is player {set.Scope.Player.Entity}'s, not {player}'s", nameof(set));
        CheckOptions(options);
        var dry = options.HasFlag(EquipOptions.DryRun);
        var t = RequireSets();
        var w = WindowNow("Items.EquipSet");
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var owner = mod?.Owner ?? 0;
        var snap = Snapshot(t, owner, player, set.Scope, w, api);
        if (!snap.Ok) return Task.FromResult(EquipmentSetResult.NotRun(snap.Status, snap.Reason!, player, dry));
        var lab = fault switch
        {
            LabSetFault.RefuseStep => ItemsNative.SetRequestLabFailStep | (uint)step << 16,
            LabSetFault.GuardBit => ItemsNative.SetRequestLabGuardBit,
            LabSetFault.CommitAnyway => ItemsNative.SetRequestLabCommitAnyway,
            LabSetFault.FakeMismatch => ItemsNative.SetRequestLabFakeMismatch,
            _ => 0u,
        };
        return SubmitSet(t, owner, mod, player, set.Scope, entries, snap.Token, snap.WorldEpoch, dry, null, w, lab, fault == LabSetFault.CancelAtOnce);
    }

    /// <summary>
    /// Applies a plan of <see cref="PlanSet"/>: the set it was made for, checked against the snapshot it holds (anything
    /// that changed the inventory since: <see cref="EquipmentSetStatus.StaleSnapshot"/>, nothing changed). The plan is
    /// used up (a plan applies once); its steps are not replayed, the transaction plans again from the snapshot.
    /// Otherwise as <see cref="EquipSet(EntityId, EquipmentSet, EquipOptions)"/>.
    /// </summary>
    /// <param name="plan">The plan (of the calling mod).</param>
    /// <param name="options"><see cref="EquipOptions.DryRun"/>: run the transaction and roll it back.</param>
    /// <returns>As the other overload; a plan applied or disposed before is <see cref="EquipmentSetStatus.StaleSnapshot"/>.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="plan"/> is null.</exception>
    /// <exception cref="ArgumentException">The plan's set breaks the rules of a set, its player is not the set's, it is
    /// another mod's plan, or unknown options.</exception>
    /// <exception cref="InvalidOperationException">Not on the tick thread, inside the server world's step, or the calling
    /// mod is unloading.</exception>
    /// <exception cref="KilnUnavailableException">The capability <c>items.equip_sets</c> is off.</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(Caps.EquipSets)]
    public static Task<EquipmentSetResult> EquipSet(EquipmentSetPlan plan, EquipOptions options = EquipOptions.None)
    {
        Services.RequireTickThread("Items.EquipSet");
        ArgumentNullException.ThrowIfNull(plan);
        var entries = EquipmentSetRules.Check(plan.Set, nameof(plan));
        if (plan.Player.IsNone || plan.Set.Scope.Player.Entity != plan.Player.Value)
            throw new ArgumentException($"the plan is for player {plan.Player}, its set for {plan.Set.Scope.Player.Entity}", nameof(plan));
        CheckOptions(options);
        var dry = options.HasFlag(EquipOptions.DryRun);
        var t = RequireSets();
        var w = WindowNow("Items.EquipSet");
        var mod = ModContext.CurrentMod;
        if (mod is { Alive: false }) throw new InvalidOperationException("the mod is unloading");
        var owner = mod?.Owner ?? 0;
        if (plan.Lease is { } lease && lease.Owner != owner) throw new ArgumentException("the plan is another mod's", nameof(plan));
        var rc = DecisionBroker.ResolveEntity(plan.Player, out var key, out _);
        if (rc == DecisionsNative.WrongThread) throw new InvalidOperationException("Items.EquipSet: the server world is inside its step (call it in Pre or Post)");
        var stale = rc != DecisionsNative.Ok ? $"no player {plan.Player} in the world with an identity kiln.decisions can tell apart (the player left)"
            : EquipmentSetRules.Stale(plan.Set.Scope, w.ProcessSession, w.WorldEpoch, key);
        if (stale is not null) return Task.FromResult(EquipmentSetResult.NotRun(EquipmentSetStatus.StaleSnapshot, "StaleSnapshot: " + stale, plan.Player, dry));
        if (plan.Lease is { } held && !held.TryUse())
            return Task.FromResult(EquipmentSetResult.NotRun(EquipmentSetStatus.StaleSnapshot, "StaleSnapshot: the plan was applied or disposed already (plan again)", plan.Player, dry));
        return SubmitSet(t, owner, mod, plan.Player, plan.Set.Scope, entries, plan.Snapshot.Value, plan.Snapshot.WorldEpoch, dry, plan.Lease, w);
    }

    // ---- the bridge: one pending entry per accepted set, under a key that is never reused ----

    private sealed class PendingSet(TaskCompletionSource<EquipmentSetResult> tcs, ModContext? mod, EntityId player, EquipmentSetScope scope,
        EquipmentSetEntry[] entries, bool dry, ulong token, ulong epoch, ulong step)
    {
        public readonly TaskCompletionSource<EquipmentSetResult> Tcs = tcs;
        public readonly ModContext? Mod = mod;
        public readonly EntityId Player = player;
        public readonly EquipmentSetScope Scope = scope;
        public readonly EquipmentSetEntry[] Entries = entries;
        public readonly bool DryRun = dry;
        public readonly ulong Token = token, WorldEpoch = epoch, SubmittedStep = step;
        public ulong Ticket;
        public bool Retired;
    }

    private static readonly object SetGate = new();
    private static readonly Dictionary<long, PendingSet> SetsInFlight = new();
    private static readonly HashSet<ModContext> SetsWatched = new();
    private static long _nextSet;

    /// <summary>How long Kiln.Items waits for kiln.items' result past its own 250-step limit before it ends the task by
    /// itself and switches sets off (a lost result must not hang a mod).</summary>
    internal const ulong SetHeartbeatSteps = 400;

    private static Task<EquipmentSetResult> SubmitSet(ItemsNative.TableV6* t, ulong owner, ModContext? mod, EntityId player, EquipmentSetScope scope,
        EquipmentSetEntry[] entries, ulong token, ulong epoch, bool dry, EquipmentSetLease? lease, WindowState w, uint lab = 0, bool cancelAtOnce = false)
    {
        EnsureSetDecisions();
        var ops = OpsOf(entries);
        var tcs = new TaskCompletionSource<EquipmentSetResult>(TaskCreationOptions.RunContinuationsAsynchronously);
        var key = TrackSet(tcs, mod, player, scope, entries, dry, token, epoch, w.Step);
        var req = new ItemsNative.SetRequest
        {
            Size = (uint)sizeof(ItemsNative.SetRequest),
            Player = player.Value,
            Flags = (dry ? ItemsNative.SetRequestDryRun : 0) | lab,
            Count = (uint)ops.Length,
            SnapshotToken = token,
            OriginOwner = owner,
        };
        w.ProcessSession.TryWriteBytes(new Span<byte>(req.ProcessSession, 16));
        ulong ticket = 0;
        int rc;
        fixed (EquipDecisions.SetOp* e = ops)
        {
            req.Entries = e;
            // owner 0: Kiln's own, so the asking mod's unload does not erase the report of a set already asked for
            rc = t->EquipSet(0, &req, &OnSetDone, (void*)key, &ticket);
        }
        if (rc == ItemsNative.Ok)
        {
            lock (SetGate)
                if (SetsInFlight.TryGetValue(key, out var p)) p.Ticket = ticket;
            if (cancelAtOnce) t->SetCancel(ticket);  // lab: as a mod's unload does
            return tcs.Task;
        }
        lock (SetGate) SetsInFlight.Remove(key);
        // not taken: give the snapshot back (a plan's stays usable when the call was refused before it was looked up)
        if (rc is ItemsNative.BufferTooSmall or ItemsNative.InvalidArgument or ItemsNative.Unavailable && lease != null) lease.Unuse();
        else ReleaseSnapshot(owner, token);
        return rc switch
        {
            ItemsNative.NotFound => Task.FromResult(EquipmentSetResult.NotRun(EquipmentSetStatus.StaleSnapshot,
                "StaleSnapshot: the snapshot is gone (used, expired, released, another world or process) or the scope is another process's", player, dry)),
            ItemsNative.BufferTooSmall => Task.FromResult(EquipmentSetResult.NotRun(EquipmentSetStatus.Refused, "Refused: 256 sets are waiting: try again in a step", player, dry)),
            ItemsNative.InvalidArgument => throw new ArgumentException("Items.EquipSet: kiln.items refused the set's arguments"),
            _ => throw new KilnUnavailableException(Caps.EquipSets, SetStatusOf(t)?.Detail is { Length: > 0 } d ? d : $"kiln.items refused the set (error {rc})"),
        };
    }

    /// <summary>A set in flight, by the key its native request carries (never reused in the process).</summary>
    internal static long TrackSet(TaskCompletionSource<EquipmentSetResult> tcs, ModContext? mod, EntityId player, EquipmentSetScope scope,
        EquipmentSetEntry[] entries, bool dry, ulong token, ulong epoch, ulong step, ulong ticket = 0)
    {
        lock (SetGate)
        {
            var key = ++_nextSet;
            SetsInFlight[key] = new PendingSet(tcs, mod, player, scope, entries, dry, token, epoch, step) { Ticket = ticket };
            if (mod != null && SetsWatched.Add(mod)) mod.Retiring += () => RetireSets(mod);
            return key;
        }
    }

    /// <summary>Sets in flight (tests).</summary>
    internal static int SetsPending
    {
        get
        {
            lock (SetGate) return SetsInFlight.Count;
        }
    }

    // The mod unloads: its sets that did not start end Dropped (kiln.items keeps the callback, the result still comes and
    // is reported); its tasks are left alone (their continuations are the unloaded mod's code).
    private static void RetireSets(ModContext mod)
    {
        List<ulong> tickets = [];
        lock (SetGate)
        {
            SetsWatched.Remove(mod);
            foreach (var p in SetsInFlight.Values)
                if (p.Mod == mod)
                {
                    p.Retired = true;
                    if (p.Ticket != 0) tickets.Add(p.Ticket);
                }
        }
        var t = V6;
        if (t == null) return;
        foreach (var ticket in tickets) t->SetCancel(ticket);  // NotFound: it already ran or is running; its result is reported
    }

    /// <summary>A native result arrived (tick thread, Pre, or the world's stop): the events hear of every committed
    /// change, the asking mod's task when it is still loaded.</summary>
    internal static void CompleteSet(long key, EquipmentSetResult result)
    {
        PendingSet? p;
        lock (SetGate) SetsInFlight.Remove(key, out p);
        if (result.Status == EquipmentSetStatus.InvariantFailure) SetsFaulted(result.Reason);
        try
        {
            // native-owned facts: posted also when the asking mod unloaded; on the thread that stops the world they come at
            // the start of the next step
            var tick = Game.IsTickThread;
            void Send(KilnEvent ev)
            {
                if (tick) Events.Post(ev);
                else Events.Enqueue(ev);
            }
            if (result.Changes.Count > 0 && Events.HasSubscribers<EquipmentSetChangedEvent>())
                Send(new EquipmentSetChangedEvent(result.Player, result.Ticket, result.Status, result.Changes));
            if (result.Status == EquipmentSetStatus.InvariantFailure && Events.HasSubscribers<EquipmentSetFaultedEvent>())
                Send(new EquipmentSetFaultedEvent(result));
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Error, $"kiln.items: posting an equipment set's events failed: {e.Message}");
        }
        if (p is { Retired: false }) p.Tcs.TrySetResult(result);  // the task completes whatever the events did
    }

    /// <summary>The managed result of a native one (pure: the unit tests build native results by hand).</summary>
    internal static EquipmentSetResult ToSetResult(in ItemsNative.SetResult n)
    {
        var status = n.Status <= (uint)EquipmentSetStatus.Unavailable ? (EquipmentSetStatus)n.Status : EquipmentSetStatus.InvariantFailure;
        string text;
        fixed (byte* r = n.Reason) text = ItemsNative.Text(r, 256);
        var ok = status is EquipmentSetStatus.Done or EquipmentSetStatus.NoChange or EquipmentSetStatus.Simulated;
        var reason = ok ? null
            : status is EquipmentSetStatus.Refused or EquipmentSetStatus.StaleSnapshot ? ReasonOf(n.Refusal != 0 ? n.Refusal : status == EquipmentSetStatus.StaleSnapshot ? 5u : 0u, text)
            : text.Length > 0 ? $"{status}: {text}" : status.ToString();
        var changes = new EquipmentSlotChange[Math.Min(n.ChangeCount, 20)];
        fixed (byte* c = n.Changes)
            for (var i = 0; i < changes.Length; i++)
            {
                var raw = ((ItemsNative.SetChange*)c)[i];
                if (SlotOfGame((byte)raw.EquipmentSlot) is not { } slot) continue;
                changes[i] = new EquipmentSlotChange(slot, ValueOf(raw.Before), ValueOf(raw.After));
            }
        return new EquipmentSetResult(status, reason, new EntityId(n.Player), n.Ticket, n.DecisionId != 0 ? new DecisionId(n.WorldEpoch, n.DecisionId) : null,
            (n.Flags & ItemsNative.SetResultDryRun) != 0, (n.Flags & ItemsNative.SetResultCommitted) != 0, (n.Flags & ItemsNative.SetResultMayHaveMutated) != 0,
            n.FailedOperation == uint.MaxValue ? null : (int)n.FailedOperation, (int)n.GameError, changes.Where(x => x is not null).ToArray());
    }

    private static EquipmentSlotValue ValueOf(in ItemsNative.SetSlotValue v) =>
        new(new ItemId(v.ItemId), ItemRefs.Of(v.ItemEntity, v.ItemId, (uint)v.InstanceStamp), new SlotId(v.InventorySlot), (int)v.Count);

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static void OnSetDone(ItemsNative.SetResult* r, void* user)
    {
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own code: the events set each handler's mod, the task its awaiter's
        try
        {
            CompleteSet((long)user, ToSetResult(*r));
        }
        catch (Exception e)
        {
            // never let an exception cross into native code
            try { Backend.Current.Log(0, LogLevel.Error, $"kiln.items: completing an equipment set failed: {e}"); } catch { }
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    // An invariant failure (or a lost result): kiln.items switched sets off; the row follows now, not in 25 steps.
    private static void SetsFaulted(string? why)
    {
        _setsOff = true;
        Off(Caps.EquipSets, why ?? "an equipment set failed its check", Caps.Equip, Caps.Instances, Caps.Transaction, Caps.Read);
    }

    // Every 25 steps (the capability tick): a set kiln.items never answered past its own limit ends here, once.
    internal static void SetHeartbeat(ulong step)
    {
        List<(long Key, PendingSet P)> lost = [];
        lock (SetGate)
            foreach (var (k, p) in SetsInFlight)
                if (p.Ticket != 0 && step > p.SubmittedStep + SetHeartbeatSteps) lost.Add((k, p));
        if (lost.Count == 0) return;
        SetsFaulted($"kiln.items never reported {lost.Count} equipment set(s) within {SetHeartbeatSteps} steps");
        foreach (var (k, p) in lost)
            CompleteSet(k, EquipmentSetResult.NotRun(EquipmentSetStatus.Dropped, $"Dropped: kiln.items never reported the set within {SetHeartbeatSteps} steps",
                p.Player, p.DryRun, p.Ticket));
    }

    // ---- the decision: one PlayerEquippingEvent per set (kiln.decisions kind 18, filed by kiln.items in Pre) ----

    private static int _setDecisions;

    internal static void EnsureSetDecisions()
    {
        if (Interlocked.Exchange(ref _setDecisions, 1) == 0) DecisionBroker.Register(SetDecisionProducer.Instance);
    }

    /// <summary>Test hook: the kind-18 producer goes away, the sets' row state and the pending sets are forgotten.</summary>
    internal static void ResetSetsForTests()
    {
        DecisionBroker.Unregister(SetDecisionProducer.Instance);
        Volatile.Write(ref _setDecisions, 0);
        _setsOff = false;
        _setsSession = (CapabilityState.Available, CapabilityReason.None, "");
        lock (SetGate)
        {
            SetsInFlight.Clear();
            SetsWatched.Clear();
        }
    }

    /// <summary>Test hook: the set under <paramref name="key"/> belongs to a mod that unloaded (its task is left alone).</summary>
    internal static void RetireSetForTests(long key)
    {
        lock (SetGate)
            if (SetsInFlight.TryGetValue(key, out var p)) p.Retired = true;
    }

    /// <summary>The <see cref="SetEquipmentChange"/> of a kind-18 request: the set Kiln.Items submitted under that ticket,
    /// or (a native caller's set) the set the payload names.</summary>
    internal static SetEquipmentChange SetChangeOf(DecisionRequest request, in ItemsNative.SetDecisionPayload payload)
    {
        lock (SetGate)
            foreach (var p in SetsInFlight.Values)
                if (p.Ticket == payload.ItemsTicket)
                    return new SetEquipmentChange(p.Ticket, new InventorySnapshotToken(p.Token, p.WorldEpoch, p.Player), new EquipmentSet(p.Scope, p.Entries), p.DryRun);
        var entries = new List<EquipmentSetEntry>();
        fixed (byte* e = payload.Entries)
            for (var i = 0; i < Math.Min(payload.Count, 20u); i++)
            {
                var op = ((EquipDecisions.SetOp*)e)[i];
                if (SlotOfGame(op.EquipmentSlot) is not { } slot) continue;
                EquipmentSetSelection sel = op.Kind switch
                {
                    1 => new InstanceEquipment(ItemRefs.Of(op.ItemEntity, op.ItemId, (uint)op.InstanceStamp)),
                    2 => new StackEquipment(new ItemId(op.ItemId), new SlotId(op.SourceSlot), (int)op.ExpectedCount),
                    _ => new EmptyEquipment(),
                };
                entries.Add(new EquipmentSetEntry(slot, sel));
            }
        var session = DecisionBroker.Window()?.ProcessSession ?? Guid.Empty;
        var player = new EntityId(request.Subject.Entity);
        return new SetEquipmentChange(payload.ItemsTicket, new InventorySnapshotToken(payload.SnapshotToken, request.Id.WorldEpoch, player),
            new EquipmentSet(new EquipmentSetScope(session, request.Id.WorldEpoch, request.Subject), entries), (payload.Flags & 1) != 0);
    }

    /// <summary>Kind 18 for the managed dispatcher: one <see cref="PlayerEquippingEvent"/> per set, its verdict the set's.
    /// It registers no event type: subscribing to <see cref="PlayerEquippingEvent"/> stays 07's <c>events.equip</c>.</summary>
    internal sealed class SetDecisionProducer : DecisionProducer
    {
        internal static readonly SetDecisionProducer Instance = new();
        public override uint Kind => DecisionsNative.KindEquipSet;
        public override string Capability => Caps.EquipSets;
        public override IReadOnlyList<Type> EventTypes { get; } = [];
        public override DecisionInterest Interest() => DecisionInterest.Nothing;  // kiln.items files every set itself

        public override DecisionVerdict Decide(DecisionRequest request)
        {
            if (!Events.HasSubscribers<PlayerEquippingEvent>()) return DecisionVerdict.Allowed;
            var payload = request.Read<ItemsNative.SetDecisionPayload>();
            var e = Events.Post(new PlayerEquippingEvent(request.Id, request.Subject, SetChangeOf(request, payload), false, null));
            return e.Canceled ? DecisionVerdict.Canceled() : DecisionVerdict.Allowed;
        }
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
    internal readonly record struct Snap(bool Ok, string? Reason, ulong Token, ulong WorldEpoch, ulong CapturedStep,
        EquipmentSetStatus Status = EquipmentSetStatus.StaleSnapshot);

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

    private static Snap Refused(string reason) =>
        new(false, reason, 0, 0, 0, reason.StartsWith("StaleSnapshot", StringComparison.Ordinal) ? EquipmentSetStatus.StaleSnapshot : EquipmentSetStatus.Refused);

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
