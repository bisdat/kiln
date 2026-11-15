// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;

namespace ClientPass;

// 07-cancelable-events S7 (0.2.0-dev.29): named component watches (ComponentChanges.Watch, kiln.component_changes).
// Orders 2910-2919.
//
// - 07-watch.hosted: in a hosted world with the real player, a watch of every player in the game follows the player
//   (a real player session, not a lab stand-in), reports the game's own stamina use as Unknown (never Game, never Kiln),
//   reports Kiln's own health write as kiln.combat's and an item Kiln gives as kiln.items' change of a linked inventory
//   page, and reports the player's own inventory action as not Kiln's. None of this ran with a connected player or in a
//   game client before this pass: the lab used spawned 1_Player stand-ins on a dedicated server. events.component_net is
//   on without it (nothing ships held). Every Expect is a requirement but the two that pass `true` (shown, not judged).
public sealed partial class ClientPassMod
{
    private const string WatchGift = "Consumable_Grenade_Explosive_PowderBall_T1";  // as component-watch-test gives it

    [ClientCheck("07-watch.hosted", "Component watches follow a real player", Area = "Cancelable events", Since = Dev29,
        Needs = ClientCheckNeeds.Hosted, Order = 2910,
        Confirm = "This uses your character: it lowers your health by 10, gives you 3 powder balls and takes them back. " +
                  "Use a disposable character in a copied world.")]
    public async Task WatchHosted(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedOpen(c, ComponentChanges.Capability)) return;
        var p = c.Player;
        if (!c.Expect(Capabilities.Is(ComponentChanges.Capability), "named component watches are on in this hosted world",
                Capabilities.Why(ComponentChanges.Capability)))
        {
            await c.Verdict();
            return;
        }
        var changes = new List<ComponentValueChangedEvent>();
        var typed = new List<AttributeValueChangedEvent>();
        var states = new List<ComponentWatchStateChangedEvent>();
        var subs = new List<IDisposable>
        {
            Events.On<ComponentValueChangedEvent>(e => changes.Add(e), EventFilter.ForEntity(p)),
            Events.On<AttributeValueChangedEvent>(e => typed.Add(e), EventFilter.ForEntity(p)),
            Events.On<ComponentWatchStateChangedEvent>(e => states.Add(e)),
        };
        var r = ComponentChanges.Watch(new(ComponentWatchScope.Players, [],
            ObservedComponents.Health | ObservedComponents.Stamina | ObservedComponents.Inventory, ComponentWatchExpansion.PlayerInventoryAndItems));
        c.OnEnd(() =>
        {
            foreach (var s in subs) s.Dispose();
            r.Watch?.Dispose();
        });
        if (!c.Expect(r.Ok && r.Pairs >= 3, "a watch of every player in the game covers you: your health, stamina and inventory pages",
                $"ok {r.Ok}, {r.Pairs} pair(s): {r.Reason}"))
        {
            await c.Verdict();
            return;
        }
        var wait = TimeSpan.FromSeconds(120);
        await c.Ticks(5);

        // 1. the game's own write: stamina spent by sprinting, which no Kiln writer reports
        changes.Clear();
        typed.Clear();
        c.Instruct("Step 1 of 4: sprint (hold the sprint key while running) for about three seconds, until your stamina bar drops.");
        await c.Until(() => changes.Any(e => e.Entity == p && e.Component == "Stamina"), wait, "a change of your stamina");
        var stamina = changes.Where(e => e.Entity == p && e.Component == "Stamina").ToArray();
        c.Expect(stamina.Length > 0 && stamina.All(e => e.Origin == ChangeOrigin.Unknown && e.SourceMod is null && e.Key.PlayerHandle != 0),
            "the game's own stamina use is reported for your character as Unknown: never Kiln's, never called the game's without proof",
            string.Join(", ", stamina.Take(4).Select(e => $"step {e.Step}: {e.Origin}/{e.SourceMod ?? "none"}, session {e.Key.PlayerHandle}")));
        var spent = typed.FirstOrDefault(a => a.Attribute == ObservedAttribute.Stamina);
        c.Expect(true, "the typed stamina values (shown, not required)",
            spent is null ? "none (this build reads no typed stamina)" : $"{spent.Before} -> {spent.After} of {spent.MaximumAfter}");

        // 2. Kiln's own write: kiln.combat lowers your health by 10
        changes.Clear();
        typed.Clear();
        var hp = Combat.GetHealth(p)?.Current ?? 0;
        c.Instruct("Step 2 of 4: stand still. Kiln lowers your health by 10 now; you should see the health bar drop once.");
        Combat.SetHealth(p, Math.Max(1, hp - 10));
        await c.Until(() => changes.Any(e => e.Entity == p && e.Component == "Health"), wait, "the change of your health");
        var health = changes.Where(e => e.Entity == p && e.Component == "Health").ToArray();
        c.Expect(health.Any(e => e.Origin == ChangeOrigin.Kiln && e.SourceMod == "kiln.combat"),
            "Kiln's own health write is reported as kiln.combat's",
            string.Join(", ", health.Take(4).Select(e => $"step {e.Step}: {e.Origin}/{e.SourceMod ?? "none"}")));

        // 3. Kiln gives an item: a change of a linked inventory page, kiln.items' write, seen through your own filter
        changes.Clear();
        c.Instruct("Step 3 of 4: Kiln gives you 3 powder balls now. Open your inventory (B) and check that they are there.");
        var given = await Items.Give(p, WatchGift, 3);
        await c.Until(() => changes.Any(e => e.Component == "Inventory"), wait, "the change of an inventory page of yours");
        var pages = changes.Where(e => e.Component == "Inventory").ToArray();
        c.Expect(given.Succeeded && pages.Any(e => e.Entity != p && e.RootEntities.Contains(p) && e.Origin == ChangeOrigin.Kiln && e.SourceMod == "kiln.items"),
            "the item Kiln gave you is reported as kiln.items' change of one of your inventory pages",
            $"give {given.Status}; " + string.Join(", ", pages.Take(4).Select(e => $"page {e.Entity.Value}: {e.Origin}/{e.SourceMod ?? "none"}")));

        // 4. your own inventory action: done by the game, not by Kiln
        changes.Clear();
        c.Instruct("Step 4 of 4: in your inventory, drag the powder balls to another empty slot (or drop them on the ground). " +
                   "Then close the inventory.");
        await c.Until(() => changes.Any(e => e.Component == "Inventory"), wait, "the change of your inventory by your own action");
        var own = changes.Where(e => e.Component == "Inventory").ToArray();
        c.Expect(own.Length > 0 && own.All(e => e.Origin != ChangeOrigin.Kiln),
            "your own inventory action is not reported as Kiln's",
            string.Join(", ", own.Take(4).Select(e => $"page {e.Entity.Value}: {e.Origin}/{e.SourceMod ?? "none"}")));
        c.Expect(!states.Any(s => s.WatchId == r.Watch!.Id), "the watch stayed ready all along (no suspension)",
            string.Join(", ", states.Select(s => $"{s.State}: {s.Reason}")));
        c.Expect(true, "what the watch covered at the end (shown, not required)", $"{r.Watch!.Pairs} pair(s), last complete step {r.Watch.LastCompleteStep}");
        await Items.Take(p, given.Item, 3);
        await c.Verdict();
    }
}
