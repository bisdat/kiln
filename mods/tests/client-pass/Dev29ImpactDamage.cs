// SPDX-License-Identifier: MIT
// 06c-impact-damage (S6/S7): what a player sees of EffectBlock.Run. The fixture is the FrostMark sample: its sword carries an
// additional hit effect (Ice 10, made by the effect's own generated program, besides the weapon's ordinary strike) that runs
// frostmark.OnHit first (an [ImpactNative] that reads the target's Health through NativeContext and makes that additional hit
// 1.5 times as strong below a quarter of the health). Two Vukahs, the right one set to 20 % health; the player hits each up to
// three times.
// What the check asserts (each an Expect): the fixture loaded (it fails closed without it), the sword given, the right one at
// 20 %, at least one Ice-10 damage event of the effect's hit on each Vukah, attackMod 1 on the full one's first such event and
// 1.5 on every one on the hurt one. What only the player can judge: the extra ice damage number beside the strike's own
// number, about half again as large on the hurt Vukah (crits and rounding move it; the strike's own number is not changed).
// A Vukah at 20 % may die before three hits: that is fine (one event each is enough); a timeout or a failure is not proof that
// the native was wrong: run the check again for fresh targets. Joined to a dedicated server (a hosting client has no
// kiln.abilities natives).
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    [ClientCheck("06c-impact-damage", "A sword's additional hit effect runs a mod's native first: its ice damage on a hurt enemy", Area = "Impact natives",
        Since = Dev29, Tags = [ClientCheckTags.Visual], Needs = ClientCheckNeeds.Joined, Order = ImpactNativesOrder + 1)]
    public async Task ImpactDamage(CheckContext c)
    {
        if (!c.Expect(Mods.IsLoaded("frostmark"), "the fixture frostmark (mods/samples/FrostMark) is loaded",
                Mods.IsLoaded("frostmark") ? "loaded" : "not loaded: add it to the profile (docs/kiln/client-pass.md)"))
        {
            await c.Verdict();
            return;
        }
        if (!await NeedPlayer(c)) return;
        foreach (var row in new[] { "abilities.natives.sync", "abilities.natives.ecs" })
            if (!Capabilities.Is(row))
            {
                await c.Skip($"{row} is unavailable: {Capabilities.Why(row)}");
                return;
            }
        await GiveBlock(c, "frostmark", "frostmark-sword");
        if (Ahead(c, 6, -2) is not { } left || Ahead(c, 6, 2) is not { } right)
        {
            c.Expect(false, "the player's place is known", "no transform");
            await c.Verdict();
            return;
        }
        var full = await SpawnHere(c, "Enemy_Vukah_Melee", left);
        var low = await SpawnHere(c, "Enemy_Vukah_Melee", right);
        await c.Delay(TimeSpan.FromSeconds(2));
        var set = await World.Update<Ecs.Health>(low, (ref Ecs.Health h) => h.Values[0] = Math.Max(1, h.Values[7] / 5));
        c.Expect(set.Ok, "the right Vukah set to 20 % of its health", set.ToString());
        var hits = new ConcurrentQueue<ImpactDamageEvent>();
        var sub = Events.On<ImpactDamageEvent>(e =>
        {
            if (e.Attacker == c.Player && Math.Abs(e.Damage.Ice - 10) < 1e-3 && (e.Target == full || e.Target == low)) hits.Enqueue(e);
        });
        c.OnEnd(() => sub.Dispose());
        c.Instruct("Take the Frost Mark Sword in hand. Two Vukahs stand in front of you: the left one has its full health, the right one " +
                   "20 %. Hit each of them up to three times, the left one first. Each hit shows two numbers: the sword's own strike and the " +
                   "Frost Mark's extra ice damage. Watch the ice number: on the right one it should be about one and a half times the left " +
                   "one's first (a critical hit or rounding moves it a little). The right one may die before your third hit: that is fine. " +
                   "At the end you are asked what you saw; if a Vukah wanders off or the check times out, run it again for fresh ones.");
        bool Done(EntityId e) => hits.Count(h => h.Target == e) >= 3 || !World.Exists(e) ||
                                 Ecs.Get<Ecs.Health>(e) is { } hp && hp.Values[0] <= 0;
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.Elapsed < TimeSpan.FromSeconds(180) && !(Done(full) && Done(low)))
            await c.Delay(TimeSpan.FromSeconds(1));
        var onFull = hits.Where(h => h.Target == full).ToList();
        var onLow = hits.Where(h => h.Target == low).ToList();
        c.Log.Info($"client-pass: 06c-impact-damage: {onFull.Count} hit(s) on the full one (attackMod {string.Join(", ", onFull.Select(h => h.AttackMod))}), " +
                   $"{onLow.Count} on the hurt one (attackMod {string.Join(", ", onLow.Select(h => h.AttackMod))})");
        c.Expect(onFull.Count >= 1 && onLow.Count >= 1, "the sword's additional effect hit (Ice 10) landed on each Vukah", $"{onFull.Count} and {onLow.Count}");
        // the full one's first hit came at full health (later ones may find it below a quarter: then 1.5 is right too)
        c.Expect(onFull.Count > 0 && Math.Abs(onFull[0].AttackMod - 1f) < 1e-3, "the native left the full one's first hit at attackMod 1",
            string.Join(", ", onFull.Select(h => h.AttackMod)));
        c.Expect(onLow.Count > 0 && onLow.All(h => Math.Abs(h.AttackMod - 1.5f) < 1e-3), "the native made the effect's hits on the hurt one attackMod 1.5",
            string.Join(", ", onLow.Select(h => h.AttackMod)));
        await c.Verdict();
    }
}
