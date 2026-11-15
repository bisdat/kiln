// SPDX-License-Identifier: MIT
// Sample: cancelable decisions (docs/kiln/services/decisions.md). The game is about to let you die, give you an item
// you walked over, or equip a ring: a handler refuses it, and every decision ends in one DecisionCompletedEvent.
//
//   /survive          you cannot die: a lethal loss leaves you at 1 health (again to turn it off)
//   /nopickup <item>  walk-over pickups of that item (name or id) are refused for you (again to turn it off)
//   /noring           equipping a ring is refused for you (again to turn it off)
//
// Each player's choice applies to that player only: the handlers filter on the players who asked, so nothing is held
// for anyone else. /survive and /nopickup require events.death and events.pickup respectively.
using Kiln;

namespace DecisionsSample;

public sealed class DecisionsSampleMod : Mod
{
    private readonly HashSet<EntityId> _survive = [];
    private readonly Dictionary<EntityId, ItemId> _noPickup = [];
    private readonly HashSet<EntityId> _noRing = [];
    private readonly List<IDisposable> _subs = [];

    public override void OnLoad()
    {
        // every decision's end: what the handlers asked for, and what came of it
        _subs.Add(Events.On<DecisionCompletedEvent>(e =>
            Log.Info($"decision {e.Result.Id.Request}: {e.Result.Status} (subject {e.Result.Subject}){(e.Result.Reason is { } r ? ": " + r : "")}")));
    }

    public override void OnUnload()
    {
        foreach (var s in _subs) s.Dispose();
    }

    // A handler that filters on the players in `who`: subscribed again when the set changes, so the service holds the
    // actions of exactly those players (an unfiltered handler would hold every one in the world).
    private IDisposable? _dying, _picking, _equipping;

    [Command("survive", Description = "toggle: a lethal loss leaves you at 1 health instead of killing you")]
    private void Survive(CommandContext c)
    {
        var p = Me(c);
        if (!Capabilities.Is("events.death")) throw new CommandException($"refusing a death is off here: {Capabilities.Why("events.death")}");
        var on = _survive.Add(p) || !_survive.Remove(p);
        _dying?.Dispose();
        _dying = _survive.Count == 0 ? null : Events.On<EntityDyingEvent>(e =>
        {
            e.SurvivalHealth = 1;
            e.Cancel();
        }, EventFilter.ForEntities(_survive));
        c.Reply(on ? "you cannot die now: a lethal loss leaves you at 1 health" : "you can die again");
    }

    [Command("nopickup", Description = "toggle: walking over this item does not pick it up for you")]
    private void NoPickup(CommandContext c, string item)
    {
        var p = Me(c);
        if (!Capabilities.Is("events.pickup")) throw new CommandException($"refusing a pickup is off here: {Capabilities.Why("events.pickup")}");
        var info = Items.Lookup(item) ?? throw new CommandException($"no item {item}");
        bool on;
        if (_noPickup.TryGetValue(p, out var had) && had == info.Id)
        {
            _noPickup.Remove(p);
            on = false;
            // what was refused for this player is asked again at the next attempt
            _ = Decisions.ReevaluatePickup(EntityId.None, p);  // Ok = false (NoActiveExposure) when nothing was refused: fine
        }
        else
        {
            _noPickup[p] = info.Id;
            on = true;
        }
        _picking?.Dispose();
        _picking = _noPickup.Count == 0 ? null : Events.On<ItemPickingUpEvent>(e =>
        {
            if (_noPickup.TryGetValue(e.Receiver, out var refused) && e.Item == refused) e.Cancel();
        }, EventFilter.ForEntities(_noPickup.Keys));
        c.Reply(on ? $"walking over {info.Label} does not pick it up for you" : $"you pick up {info.Label} again");
    }

    [Command("noring", Description = "toggle: you cannot put on a ring")]
    private void NoRing(CommandContext c)
    {
        var p = Me(c);
        if (!Capabilities.Is("events.equip")) throw new CommandException($"refusing an equip is off here: {Capabilities.Why("events.equip")}");
        var on = _noRing.Add(p) || !_noRing.Remove(p);
        _equipping?.Dispose();
        _equipping = _noRing.Count == 0 ? null : Events.On<PlayerEquippingEvent>(e =>
        {
            if (e.Change is SingleEquipmentChange { Operation: EquipmentChangeKind.Equip, Slot: EquipmentSlot.Ring0 or EquipmentSlot.Ring1 }) e.Cancel();
        }, EventFilter.ForEntities(_noRing));
        c.Reply(on ? "you cannot put on a ring now" : "you can put on rings again");
    }

    private static EntityId Me(CommandContext c) =>
        c.Source.Player.IsNone ? throw new CommandException("only a player in the world can do this") : c.Source.Player;
}
