// SPDX-License-Identifier: MIT
// 06c S6: the native a perk's hit effect runs first (EffectBlock.Run). It counts its calls with the impact they came from and makes the
// hit's attackMod 1.5 before the effect's own damage (Ice 13) is dealt with it.
using System.Collections.Concurrent;
using System.Numerics;
using Kiln;

namespace BlocksAbilitiesTest;

public static class RunNatives
{
    internal static readonly ConcurrentQueue<(uint Impact, EntityId Target, float AttackModSeen)> Calls = new();

    [ImpactNative("Frost")]
    public static bool Frost(in NativeContext ctx, EntityId target, EntityId attacker, ref float attackMod)
    {
        Calls.Enqueue((ctx.ImpactId, target, attackMod));
        attackMod = 1.5f;
        return !attacker.IsNone;
    }

    internal static readonly ConcurrentQueue<(uint Impact, Vector3 Seen)> Lifts = new();

    /// <summary>06c last read: a ref position the native changes is the position the effect's own hit uses (applyDamage).</summary>
    [ImpactNative("Lift")]
    public static bool Lift(in NativeContext ctx, ref Vector3 position)
    {
        Lifts.Enqueue((ctx.ImpactId, position));
        position.Y += 2;
        return true;
    }
}
