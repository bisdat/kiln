// SPDX-License-Identifier: MIT
// 06c-impact-damage (S6/S7): what a player sees of EffectBlock.Run. The fixture is the FrostMark sample: its sword's hit effect
// runs frostmark.OnHit first (an [ImpactNative] that reads the target's Health through NativeContext and makes the hit 1.5 times
// as strong below a quarter of the health), then deals its Ice 10. Two Vukahs, the right one set to 20 % health; the player hits
// each three times. Every Expect is a requirement: the fixture loaded (it fails closed without it), the sword given, the right one
// at 20 %, three of the sword's effect hits on each, the native's attackMod 1.5 on the hurt one and 1 on the other in the
// damage events, and the player's own reading of the damage numbers. Joined to a dedicated server (a hosting client has no
// kiln.abilities natives).
using System.Collections.Concurrent;
using Kiln;
using Kiln.Testing;

namespace ClientPass;

public sealed partial class ClientPassMod
{
    [ClientCheck("06c-impact-damage", "A sword's hit effect runs a mod's native first: damage numbers on a hurt enemy", Area = "Impact natives",
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
                   "20 %. Hit each of them three times with the sword, the left one first, and watch the damage numbers. At the end you are " +
                   "asked whether the numbers on the right one were about one and a half times the left one's first.");
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.Elapsed < TimeSpan.FromSeconds(180) && (hits.Count(h => h.Target == full) < 3 || hits.Count(h => h.Target == low) < 3))
            await c.Delay(TimeSpan.FromSeconds(1));
        var onFull = hits.Where(h => h.Target == full).ToList();
        var onLow = hits.Where(h => h.Target == low).ToList();
        c.Log.Info($"client-pass: 06c-impact-damage: {onFull.Count} hit(s) on the full one (attackMod {string.Join(", ", onFull.Select(h => h.AttackMod))}), " +
                   $"{onLow.Count} on the hurt one (attackMod {string.Join(", ", onLow.Select(h => h.AttackMod))})");
        c.Expect(onFull.Count >= 3 && onLow.Count >= 3, "three of the sword's effect hits on each Vukah", $"{onFull.Count} and {onLow.Count}");
        // the full one's first hit came at full health (later ones may find it below a quarter: then 1.5 is right too)
        c.Expect(onFull.Count > 0 && Math.Abs(onFull[0].AttackMod - 1f) < 1e-3, "the native left the full one's first hit at attackMod 1",
            string.Join(", ", onFull.Select(h => h.AttackMod)));
        c.Expect(onLow.Count > 0 && onLow.All(h => Math.Abs(h.AttackMod - 1.5f) < 1e-3), "the native made the hurt one's hits attackMod 1.5",
            string.Join(", ", onLow.Select(h => h.AttackMod)));
        await c.Verdict();
    }
}
