// SPDX-License-Identifier: MIT
// FrostMark: a sword with an additional hit effect that runs a native first. The native is a plain static method: the build turns
// it into a same-step Impact native ("frostmark.OnHit"), and the effect's program calls it before its own Ice damage, with the
// target and the attackMod of that additional hit bound by parameter name. The weapon's ordinary strike is the game's, unchanged.
using Kiln;

namespace FrostMark;

public static class Frost
{
    /// <summary>On the sword's additional effect hit: an enemy below a quarter of its health takes 1.5 times that hit (the
    /// weapon's own strike is not this native's). Runs inside the effect's program, on
    /// the simulation's thread (no tick-thread API, no blocking); Health is read through impact_simulation's own accessor.</summary>
    [ImpactNative("OnHit")]
    public static bool OnHit(in NativeContext ctx, EntityId target, ref float attackMod)
    {
        if (ctx.TryRead<Ecs.Health>(target, out var h) && h.Values[7] > 0 && h.Values[0] * 4 < h.Values[7]) attackMod *= 1.5f;
        return true;  // false would skip the rest of the effect
    }
}

public sealed class FrostMarkMod : Mod
{
    private long _fireballHits;

    public override void OnLoad()
    {
        // an additional effect on each of the sword's hits: Run, then Ice 10 (a weapon's OnHit needs no enhancement unlocked; a
        // PerkBlock's OnHit takes the same effect). The weapon's own strike is dealt by the game as before.
        // Where the game's resources are not there (a unit test's harness), the native and the attachment still are.
        try
        {
            Blocks.Define("frostmark-sword", new WeaponBlock
            {
                Name = "Frost Mark Sword",
                Description = "Each hit adds a bite of frost, half again as deep on a badly hurt enemy.",
                Class = WeaponClass.Sword,
                Stats = new WeaponStats { Level = 5, Power = 20, Rarity = Rarity.Uncommon },
                OnHit = [new EffectBlock { Run = Natives.OnHit, Damage = Damage.Ice(10), Flinch = Flinch.None }],
            });
        }
        catch (KilnUnavailableException e)
        {
            Log.Info($"frostmark: no sword here: {e.Message}");
        }
        // an attachment to the vanilla Fireball explosion's applyDamage, counted. The row is decided before OnLoad; made
        // here, the attachment is Pending until the world's programs are in, then Active (or Off, with a log line).
        if (Capabilities.Is("abilities.natives.attach"))
        {
            var a = Abilities.Natives.Attach(0x7a120ee8, "applyDamage", NativeStage.Before, (ref NativeCall c) => Interlocked.Increment(ref _fireballHits));
            Log.Info($"frostmark: a Before on the Fireball explosion's applyDamage: {(a.Ok ? "attached" : a.Reason)}");
        }
        else
        {
            Log.Info($"frostmark: no attachments here: {Capabilities.Why("abilities.natives.attach")}");
        }
    }

    [Command("frostmark", Description = "Fireball explosion hits counted by Frost Mark's attachment")]
    private void Count(CommandContext c) => c.Reply($"Fireball explosion hits: {Interlocked.Read(ref _fireballHits)}");
}
