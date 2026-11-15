// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 07-cancelable-events (0.2.0-dev.29): the guided checks of chapter 07. Orders 2900-2999 (lab port 28090).
//
// - 07-events.decision-rollback: a refused death, walk-over pickup and equip leave the player's own client in step with
//   the server (no death screen, no ghost item, no ring shown as worn), and the allowed controls work. It confirms
//   events.death and events.pickup in a real game: both are on since 0.2.0-dev.29 without it (nothing ships held);
//   events.equip shares the verdict of PlayerClientActionEvent (shipped in dev.26) and is checked here as well.
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

        // 2. a walk-over pickup, refused: the item stays on the ground. Two drops of the same item and amount, each a real
        //    drop with the walk-over components added at its spawn and nothing else (no receiver, no players mask: the
        //    game decides who is in range). The first is an unheld control: walking over it must give an item by walking
        //    (the game's PickupEvent from that drop, no interaction), and how many it gave is the grant to expect (the
        //    server's resourceDropStackAmountFactor scales a stack). Without that contact the check fails: it cannot
        //    judge a refusal it never saw. Every event, outcome and grant below is matched to its exact drop.
        var item = new ItemId(EventsGround);
        var grants = new List<ItemPickupEvent>();   // the game's PickupEvent ("+N"), after the fact
        var taken = new List<LootEvent>();          // an interaction pickup (the interact key): not a walk-over one
        subs.Add(Events.On<ItemPickupEvent>(e => { if (e.Receiver == p) grants.Add(e); }));
        subs.Add(Events.On<LootEvent>(e => { if (e.Guest == p) taken.Add(e); }));
        async Task<EntityId> WalkOverDrop(WorldPosition at)
        {
            try
            {
                var d = await Spawn.Item(EventsGround, 3, at, options: new SpawnOptions
                {
                    Tag = "client-pass",
                    Flags = SpawnFlags.Transient,
                    Components = [SpawnComponent.Of("PickupItemZone", new byte[8]), SpawnComponent.Of("DynamicIsPlayerInRange", new byte[16])],
                });
                drops.Add(d);
                return d;
            }
            catch (Exception e) when (e is KilnUnavailableException or SpawnException)
            {
                c.Expect(false, "a walk-over drop could be spawned in front of you", e.Message);
                return default;
            }
        }
        int Inventory() => Items.Read(p)?.Count(item) ?? 0;
        int GrantedBy(EntityId d) => grants.Where(g => g.Source == d && g.ItemId == item.Value).Sum(g => g.Count);
        bool Interacted(EntityId d) => taken.Any(t => t.Host == d);

        var control = await WalkOverDrop(Ahead(c, 4, right: -2) ?? World.Position(p)!.Value);
        var grant = 0;
        if (!control.IsNone)
        {
            var start = Inventory();
            c.Instruct("Step 2 of 6, first part: an item lies about four metres ahead of you, a little to the left. Walk over it " +
                       "WITHOUT pressing the interact key. Kiln does not hold this one: it is the control, and it should go into your " +
                       "inventory the usual way, by walking.");
            await c.Until(() => GrantedBy(control) > 0 || Interacted(control), wait, "the control item picked up by walking over it");
            await c.Ticks(30);
            grant = GrantedBy(control);
            var contact = c.Expect(grant > 0 && !Interacted(control) && !Spawn.Exists(control) && Inventory() - start == grant,
                "walking over the control item picked it up (no interact key): the game gave it by walking",
                Interacted(control) ? "it was taken with the interact key, which is not a walk-over pickup"
                : grant == 0 ? "no item came by walking over it within the time: no walk-over contact happened (or this drop is not a " +
                               "walk-over pickup on this build); the refusal below cannot be judged"
                : $"the game's grant {grant}, inventory {start} -> {Inventory()}, item still there {Spawn.Exists(control)}");
            if (!contact) control = default;
        }
        EntityId drop = default;
        if (!control.IsNone) drop = await WalkOverDrop(Ahead(c, 4, right: 2) ?? World.Position(p)!.Value);
        var have = Inventory();
        if (!drop.IsNone)
        {
            // only this drop is held (its own filter), and only its events count
            subs.Add(Events.On<ItemPickingUpEvent>(e =>
            {
                if (e.GroundEntity != drop) return;
                picking.Add(e);
                if (!allow) e.Cancel();
            }, EventFilter.ForEntity(drop)));
            c.Instruct("Step 2 of 6, second part: another item of the same kind lies about four metres ahead of you, a little to the " +
                       "right. Walk over it the same way, WITHOUT pressing the interact key, and stand on it for a few seconds. Kiln " +
                       "refuses this pickup: it should stay on the ground, and nothing should flash into your inventory or vanish and " +
                       "come back.");
            await c.Until(() => picking.Count > 0 && picking.All(e => outcomes.Any(o => o.Id == e.Id)), wait, "a walk-over pickup decision for that item");
            await c.Ticks(60);
            var mine = picking.All(e => e.GroundEntity == drop && e.Receiver == p && e.Item == item && e.Amount == 3);
            var ends = picking.Select(e => outcomes.FirstOrDefault(o => o.Id == e.Id)).ToList();
            c.Expect(picking.Count > 0 && mine && ends.All(o => o is { Status: DecisionTerminal.Prevented } && o.Subject == drop && o.Actor == p) &&
                     Spawn.Exists(drop) && Inventory() == have && GrantedBy(drop) == 0 && !Interacted(drop),
                "the walk-over pickup of that item was asked for and refused; it is still on the server's ground and nothing of it reached you",
                picking.Count == 0
                    ? "no walk-over pickup of that item was asked for, although walking over the control gave one"
                    : $"{picking.Count} event(s) for drop {drop} (all for it and you: {mine}), outcomes {string.Join(", ", ends.Select(o => o?.Status.ToString() ?? "none"))}; " +
                      $"item there {Spawn.Exists(drop)}, inventory {have} -> {Inventory()}, game grant {GrantedBy(drop)}, interact {Interacted(drop)}");
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
                   "ground; the ring not worn. If a second player is joined (best a vanilla one, without Kiln), ask them too: they " +
                   "should see the item on the ground and you alive. Note anything that looked wrong for a moment.");
        await c.Ticks(300);

        // 4-6. the allowed controls
        allow = true;
        if (!drop.IsNone && Spawn.Exists(drop))
        {
            var re = Decisions.ReevaluatePickup(drop, p);
            c.Expect(re.Ok, "the refused pickup is asked again", re.Reason);
            c.Instruct("Step 4 of 6: now the pickup is allowed. Walk over the same item again (step off and back on if you are " +
                       "standing on it). It should go into your inventory once, as much as the control gave you.");
            var before = picking.Count;
            await c.Until(() => GrantedBy(drop) > 0 && picking.Skip(before).Any(e => outcomes.Any(o => o.Id == e.Id && o.Status == DecisionTerminal.Allowed)),
                wait, "the allowed pickup of that item and its grant");
            await c.Ticks(60);  // a second grant would show by now
            var allowed = picking.Skip(before).Select(e => outcomes.FirstOrDefault(o => o.Id == e.Id)).Where(o => o is not null).ToList();
            var got = GrantedBy(drop);
            c.Expect(allowed.Any(o => o!.Status == DecisionTerminal.Allowed) && got == grant && Inventory() - have == grant && !Spawn.Exists(drop) &&
                     grants.Count(g => g.Source == drop) == 1 && !Interacted(drop),
                $"the allowed pickup gave exactly what the control gave ({grant}), once, and took the item off the ground",
                $"outcomes {string.Join(", ", allowed.Select(o => o!.Status))}; game grant {got} in {grants.Count(g => g.Source == drop)} event(s), " +
                $"inventory {have} -> {Inventory()}, item there {Spawn.Exists(drop)}, interact {Interacted(drop)}");
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
