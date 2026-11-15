// SPDX-License-Identifier: MIT
// FrostMark: a sword that runs a native on every hit. The native is a plain static method: the build turns it into a
// same-step Impact native ("frostmark.OnHit"), and the sword's hit effect calls it first, before its own Ice damage, with the hit's
// target and attackMod bound by parameter name.
using Kiln;

namespace FrostMark;

public static class Frost
{
    /// <summary>On a hit: an enemy below a quarter of its health takes 1.5 times the hit. Runs inside the hit's program, on
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
        // the sword's own hits (a weapon's OnHit needs no enhancement unlocked; a PerkBlock's OnHit takes the same effect).
        // Where the game's resources are not there (a unit test's harness), the native and the attachment still are.
        try
        {
            Blocks.Define("frostmark-sword", new WeaponBlock
            {
                Name = "Frost Mark Sword",
                Description = "Hits on a badly hurt enemy bite half again as deep.",
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
