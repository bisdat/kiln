// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 07-cancelable-events (0.2.0-dev.29): the guided checks of chapter 07. Orders 2900-2999 (lab port 28090).
//
// - 07-events.decision-rollback: a refused death, walk-over pickup and equip leave the player's own client in step with
//   the server (no death screen, no ghost item, no ring shown as worn), and the allowed controls work. It opens the
//   held rows events.death and events.pickup (native/core/gates/dev28-07-cancelable-events.json); events.equip is not
//   held (it shares the verdict of PlayerClientActionEvent, which shipped in dev.26) but is checked here as well.
// - 07-piece-rollback: player build pieces; blocked: events.build.pieces is not built in this release (its replay
//   gate S0 and the adapter S9 wait for 06a's program wraps). The check says so and skips: it can never pass.
// - 07-native-terraform: the existing native terraform policy of Gameplay.Protect (not a managed event): a refused dig,
//   add or flatten inside a protected zone converges on the client. It gates nothing of 07.
public sealed partial class ClientPassMod
{
    private const uint EventsRing = 0x79442800;    // Ring_T3_Ring_of_Light
    private const uint EventsGround = 0x2b0cf1d5;  // the walk-over lab's item (decisions-api-test)

    [ClientCheck("07-events.decision-rollback", "Refused death, pickup and equip converge", Area = "Cancelable events", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [Visual], Order = 2900,
        Confirm = "This uses your character: it sets your health to 0 twice (the second time you really die and respawn), " +
                  "drops an item in front of you and gives you a ring. Use a disposable character in a copied world.")]
    public async Task EventsDecisionRollback(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, "events.death", "events.pickup")) return;
        var p = c.Player;
        var outcomes = new List<DecisionResult>();
        var dying = new List<EntityDyingEvent>();
        var picking = new List<ItemPickingUpEvent>();
        var equipping = new List<PlayerEquippingEvent>();
        var allow = false;
        var subs = new List<IDisposable>
        {
            Events.On<DecisionCompletedEvent>(e => outcomes.Add(e.Result), EventFilter.ForEntity(p)),
            Events.On<EntityDyingEvent>(e =>
            {
                dying.Add(e);
                if (allow) return;
                e.SurvivalHealth = 1;
                e.Cancel();
            }, EventFilter.ForEntity(p)),
            Events.On<ItemPickingUpEvent>(e =>
            {
                picking.Add(e);
                if (!allow) e.Cancel();
            }, EventFilter.ForEntity(p)),
        };
        var equipOn = Capabilities.Is("events.equip");
        if (equipOn)
            subs.Add(Events.On<PlayerEquippingEvent>(e =>
            {
                equipping.Add(e);
                if (!allow) e.Cancel();
            }, EventFilter.ForEntity(p)));
        var drops = new List<EntityId>();
        c.OnEnd(() =>
        {
            foreach (var s in subs) s.Dispose();
            foreach (var d in drops)
                if (Spawn.Exists(d)) Spawn.Destroy(d);
        });
        DecisionResult? EndOf<T>(IReadOnlyList<T> seen, Func<T, DecisionId> id) =>
            seen.Count == 0 ? null : outcomes.LastOrDefault(o => o.Id == id(seen[^1]));
        uint RingWorn() => PlayerStates.EquipmentOf(p) is { } q ? Math.Max(q[PlayerEquipSlot.Ring0].Id == EventsRing ? EventsRing : 0, q[PlayerEquipSlot.Ring1].Id == EventsRing ? EventsRing : 0) : 0;
        var wait = TimeSpan.FromSeconds(120);

        // 1. a lethal loss, refused: kept alive at 1
        var max = Combat.GetHealth(p)?.Max ?? 0;
        c.Instruct("Step 1 of 6: your health goes to 0 now and Kiln refuses the death, keeping you at 1 health. Watch your screen " +
                   "for about five seconds: there should be no death screen, no fall-over animation and no respawn button.");
        Combat.SetHealth(p, 0);
        await c.Until(() => EndOf(dying, d => d.Id) is not null, wait, "the refused death's outcome");
        var death = EndOf(dying, d => d.Id);
        c.Expect(dying.Count == 1 && death is { Status: DecisionTerminal.Prevented, AppliedHealth: 1 } && Combat.GetHealth(p)?.Current == 1,
            "one death decision, Prevented, and the server keeps you at 1 health",
            $"{dying.Count} event(s), outcome {death?.Status} applied {death?.AppliedHealth}: {death?.Reason}; health now {Combat.GetHealth(p)?.Current} of {max}");
        await c.Ticks(150);

        // 2. a walk-over pickup, refused: the item stays on the ground. A real drop with the walk-over components added at
        //    its spawn; the receiver is left to the game (nothing forces it, unlike the lab's fixture).
        var ahead = Ahead(c, 4) ?? World.Position(p)!.Value;
        EntityId drop = default;
        try
        {
            drop = await Spawn.Item(EventsGround, 3, ahead, options: new SpawnOptions
            {
                Tag = "client-pass",
                Flags = SpawnFlags.Transient,
                Components = [SpawnComponent.Of("PickupItemZone", new byte[8]), SpawnComponent.Of("DynamicIsPlayerInRange", new byte[16])],
            });
            drops.Add(drop);
        }
        catch (Exception e) when (e is KilnUnavailableException or SpawnException)
        {
            c.Expect(false, "a walk-over drop could be spawned in front of you", e.Message);
        }
        var have = Items.Read(p)?.Count(new ItemId(EventsGround)) ?? 0;
        if (!drop.IsNone)
        {
            c.Instruct("Step 2 of 6: an item lies about four metres in front of you. Walk over it, WITHOUT pressing the interact " +
                       "key, and stand on it for a few seconds. Kiln refuses the pickup: it should stay on the ground, and nothing " +
                       "should flash into your inventory or vanish and come back.");
            await c.Until(() => EndOf(picking, e => e.Id) is not null, wait, "a walk-over pickup decision for the item");
            var pick = EndOf(picking, e => e.Id);
            c.Expect(pick is { Status: DecisionTerminal.Prevented } && Spawn.Exists(drop) && (Items.Read(p)?.Count(new ItemId(EventsGround)) ?? 0) == have,
                "the walk-over pickup was asked for and refused; the item is still on the server's ground and not in your inventory",
                picking.Count == 0
                    ? "no walk-over pickup was asked for: the game did not pick the item up by walking (the drop may not be a walk-over one on this build)"
                    : $"{picking.Count} event(s), outcome {pick?.Status}: {pick?.Reason}; item there {Spawn.Exists(drop)}, inventory {have} -> {Items.Read(p)?.Count(new ItemId(EventsGround))}");
        }

        // 3. an equip, refused: the ring stays in the bag
        if (!equipOn) c.Expect(false, "events.equip is on", Capabilities.Why("events.equip"));
        else
        {
            await Items.Give(p, new ItemId(EventsRing), 1);
            await c.Ticks(10);
            c.Instruct("Step 3 of 6: you got a Ring of Light. Open your inventory and equip it in a ring slot the way you usually do. " +
                       "Kiln refuses it: the ring should go back to (or stay in) the bag, and your character should not show it worn.");
            await c.Until(() => EndOf(equipping, e => e.Id) is not null, wait, "an equipment decision for the ring");
            var eq = EndOf(equipping, e => e.Id);
            c.Expect(eq is { Status: DecisionTerminal.Prevented } && RingWorn() == 0,
                "the equip was asked for and refused; the server has the ring in no ring slot",
                equipping.Count == 0
                    ? "no equipment decision came: the inventory screen may have moved the ring another way (not changeEquipmentAction)"
                    : $"{equipping.Count} event(s) ({equipping[^1].Change}), outcome {eq?.Status}: {eq?.Reason}; ring worn 0x{RingWorn():x8}");
        }
        c.Instruct("Look at your screen and inventory for a few seconds: health bar above 0, no death screen; the item still on the " +
                   "ground; the ring not worn. If a second viewer is joined, ask them too. Note anything that looked wrong for a moment.");
        await c.Ticks(300);

        // 4-6. the allowed controls
        allow = true;
        if (!drop.IsNone && Spawn.Exists(drop))
        {
            var re = Decisions.ReevaluatePickup(drop, p);
            c.Expect(re.Ok, "the refused pickup is asked again", re.Reason);
            c.Instruct("Step 4 of 6: now the pickup is allowed. Walk over the item again (step off and back on if you are standing on it). " +
                       "It should go into your inventory once.");
            var before = picking.Count;
            await c.Until(() => picking.Count > before && EndOf(picking, e => e.Id) is not null, wait, "the allowed pickup's outcome");
            c.Expect(EndOf(picking, e => e.Id) is { Status: DecisionTerminal.Allowed } && !Spawn.Exists(drop) && (Items.Read(p)?.Count(new ItemId(EventsGround)) ?? 0) > have,
                "the allowed pickup put the item into your inventory and off the ground", $"inventory {have} -> {Items.Read(p)?.Count(new ItemId(EventsGround))}");
        }
        if (equipOn)
        {
            c.Instruct("Step 5 of 6: now equipping is allowed. Equip the ring again. It should show as worn.");
            var before = equipping.Count;
            await c.Until(() => equipping.Count > before && EndOf(equipping, e => e.Id) is not null && RingWorn() != 0, wait, "the allowed equip");
            c.Expect(EndOf(equipping, e => e.Id) is { Status: DecisionTerminal.Allowed } && RingWorn() == EventsRing, "the allowed equip put the ring on");
        }
        c.Instruct("Step 6 of 6: last, your health goes to 0 again and this time the death is allowed: you die as usual and respawn. " +
                   "Respawn, then come back here.");
        var deaths = dying.Count;
        Combat.SetHealth(p, 0);
        await c.Until(() => dying.Count > deaths && EndOf(dying, d => d.Id) is not null, wait, "the allowed death's outcome");
        c.Expect(EndOf(dying, d => d.Id) is { Status: DecisionTerminal.Allowed }, "the allowed death went on as the game's own");
        c.Instruct("Pass when every refused step looked right on your screen (no death screen, the item stayed on the ground, the ring " +
                   "stayed in the bag, nothing flickered or got stuck) and every allowed step worked. Leave and rejoin once if you can and " +
                   "say in the note whether the inventory and health were the same afterwards. A ghost item, a stuck death screen or a " +
                   "ring that shows as worn while the server says no is a fail, even if every line above says yes.");
        await c.Verdict();
    }

    [ClientCheck("07-piece-rollback", "Supported building primitive correction", Area = "Cancelable events", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [Visual, VanillaClient], Order = 2901)]
    public async Task EventsPieceRollback(CheckContext c)
    {
        // events.build.pieces is not built in this release: its replay research gate (07 S0) and the adapter (S9) wait for
        // 06a's program wraps. Nothing here could refuse a piece, so the check can never pass yet.
        c.Instruct("This check tests refusing a player's building piece. That part is not in this build of Kiln yet " +
                   $"(events.build.pieces: {Capabilities.Why("events.build.pieces")}). Nothing to do: it is skipped.");
        await c.Skip("events.build.pieces is not built in this release");
    }

    [ClientCheck("07-native-terraform", "Separate native policy presentation", Area = "Cancelable events", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Tags = [Visual], Order = 2902,
        Confirm = "This makes a protected zone around you and asks you to dig, add terrain and flatten in it and beside it. " +
                  "Use a copied world.")]
    public async Task EventsNativeTerraform(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        if (!Capabilities.Is("kiln.gameplay"))
        {
            c.Expect(false, "kiln.gameplay is on", Capabilities.Why("kiln.gameplay"));
            await c.Verdict();
            return;
        }
        var here = World.Position(c.Player)!.Value;
        var zone = Gameplay.Protect(new ProtectionZone
        {
            Name = "client-pass 07-native-terraform",
            Deny = GameplayActions.Terraform,
            Shape = ZoneShape.Sphere,
            Center = here,
            Radius = 8,
        });
        c.OnEnd(zone.Dispose);
        var built = new List<BuildEvent>();
        var sub = Events.On<BuildEvent>(e => { if (e.Kind == BuildKind.Terraform) built.Add(e); });
        c.OnEnd(sub.Dispose);
        c.Instruct("A protected zone of 8 metres now surrounds the spot where you stand (terraforming refused). Inside it, do one " +
                   "normal dig with a pickaxe, one terrain add from the hammer's menu and one flatten. Watch each for about five " +
                   "seconds: the ground should not change, or should snap back at once, and no material should be charged or refunded. " +
                   "Then walk more than 10 metres away and do the same three once each: there they work as usual.");
        await c.Until(() => built.Count >= 3, TimeSpan.FromSeconds(300), "three terraform edits seen by the server");
        c.Expect(built.Count >= 3, "the server saw your terraform edits", $"{built.Count} edit(s): {string.Join(", ", built.Take(8).Select(b => b.ToString()))}");
        c.Instruct("Pass when inside the zone nothing changed for good (no hole that stays, no added terrain, no ghost that you can walk on) " +
                   "and outside it every edit worked with its normal cost. Rejoin once if you can and say whether both patches look the same then.");
        await c.Verdict();
    }
}
