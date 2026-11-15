// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.BlockTests;

/// <summary>06c S6: EffectBlock.Run, a mod's [ImpactNative] called first in the effect's program with the trigger's variables bound by
/// parameter name (self, target, attacker/caster, position, attackMod, damage); a false result skips the rest of the effect.</summary>
public sealed class BlocksRunTests
{
    private const string Mod = "run-mod";

    private static NativeParameter P(string name, string type, NativeArgDirection dir = NativeArgDirection.In, bool optional = false, int size = 0) =>
        new(name, new NativeArg(type, dir, optional, false, size));

    private static readonly NativeRef OnHit = new("OnHit",
        [P("target", "ecs.EntityId", size: 4), P("attacker", "ecs.EntityId", size: 4), P("attackMod", "float32", NativeArgDirection.InOut, size: 4)]);

    private static ModBlocks NewMod() => new(Mod, 1) { ManifestClients = "mod" };

    private static string Listing(ModBlocks mod) => string.Join("\n", mod.Programs.Values.Select(p => p.Listing));

    private static WeaponBlock Sword(EffectBlock e) => new() { Name = "Run Sword", Class = WeaponClass.Sword, OnHit = [e] };

    [Fact]
    public void Run_is_called_first_with_the_bound_variables_and_its_attackMod_goes_to_the_hit()
    {
        var mod = NewMod();
        Compiler.Lower(mod, "runsword", Sword(new EffectBlock { Run = OnHit, Damage = new DamageBlock { Fire = 5 }, Then = [new EffectBlock { Heal = 1 }] }));
        var lines = Listing(mod).Split('\n').ToList();
        var run = lines.FindIndex(l => l.Contains($"{Mod}.OnHit(target, caster, attackMod1)"));
        var hit = lines.FindIndex(l => l.Contains("applyDamage(") && l.Contains("attackMod1"));
        var heal = lines.FindIndex(l => l.Contains("changeAttributeValue("));
        Assert.True(run >= 0, Listing(mod));
        Assert.True(hit > run, "the hit comes after Run and takes its attackMod");
        Assert.True(heal > hit, "Then comes after");
    }

    [Fact]
    public void Without_Run_the_hit_takes_1_as_before()
    {
        var mod = NewMod();
        Compiler.Lower(mod, "plainsword", Sword(new EffectBlock { Damage = new DamageBlock { Fire = 5 } }));
        Assert.Contains(Listing(mod).Split('\n'), l => l.Contains("applyDamage(") && l.Contains(", one, zero, zero,"));
        Assert.DoesNotContain("attackMod", Listing(mod));
    }

    [Fact]
    public void Self_position_and_damage_bind_and_an_optional_one_may_stay_unbound()
    {
        var mod = NewMod();
        var all = new NativeRef("All", [P("self", "ecs.EntityId"), P("position", "float3"), P("damage", "impact.DamageSetup", NativeArgDirection.InOut),
            P("caster", "ecs.EntityId"), P("extra", "float32", optional: true)]);
        // "extra" is no binding name: an error even when optional
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(mod, "allsword", Sword(new EffectBlock { Run = all, Damage = new DamageBlock { Ice = 2 } })));
        Assert.Contains("parameter 'extra' names nothing a Run native gets", e.Message);
        Assert.Contains("KL340", e.Message);
        var ok = new NativeRef("Ok", [P("self", "ecs.EntityId"), P("position", "float3"), P("damage", "impact.DamageSetup", NativeArgDirection.InOut),
            P("caster", "ecs.EntityId"), P("attackMod", "float32", NativeArgDirection.InOut, optional: true)]);
        var mod2 = NewMod();
        Compiler.Lower(mod2, "oksword", Sword(new EffectBlock { Run = ok, Damage = new DamageBlock { Ice = 2 } }));
        Assert.Contains(Listing(mod2).Split('\n'), l => l.Contains($"{Mod}.Ok(self, hitAt1,") && l.Contains("caster, attackMod1)"));
        // with no damage in the effect an optional attackMod is passed unconnected
        var mod3 = NewMod();
        Compiler.Lower(mod3, "healsword", Sword(new EffectBlock { Run = new NativeRef("Heal", [P("target", "ecs.EntityId"), P("attackMod", "float32", optional: true)]), Heal = 3 }));
        Assert.Contains(Listing(mod3).Split('\n'), l => l.Contains($"{Mod}.Heal(target, _)"));
    }

    [Fact]
    public void A_required_binding_with_nothing_here_and_a_wrong_type_are_errors()
    {
        var needsDamage = new NativeRef("NeedsDamage", [P("damage", "impact.DamageSetup", NativeArgDirection.InOut)]);
        var e1 = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "a", Sword(new EffectBlock { Run = needsDamage, Heal = 1 })));
        Assert.Contains("this effect deals no damage", e1.Message);
        var wrongType = new NativeRef("Wrong", [P("target", "float32")]);
        var e2 = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "b", Sword(new EffectBlock { Run = wrongType, Heal = 1 })));
        Assert.Contains("parameter 'target' is float32", e2.Message);
    }

    [Fact]
    public void Json_names_the_native_and_its_parameters_come_from_the_mods_natives_json()
    {
        RunNatives.Declarations = full => full == "other-mod.Freeze" ? [P("target", "ecs.EntityId"), P("caster", "ecs.EntityId")] : null;
        try
        {
            var block = (WeaponBlock)typeof(BlockJson).GetMethod("Parse", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!
                .MakeGenericMethod(typeof(WeaponBlock)).Invoke(null, ["""{ "name": "Json Sword", "class": "sword", "onHit": [ { "run": "other-mod.Freeze", "heal": 1 } ] }"""])!;
            var mod = NewMod();
            Compiler.Lower(mod, "jsonsword", block);
            Assert.Contains(Listing(mod).Split('\n'), l => l.Contains("other-mod.Freeze(target, caster)"));
            var missing = (WeaponBlock)typeof(BlockJson).GetMethod("Parse", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Static)!
                .MakeGenericMethod(typeof(WeaponBlock)).Invoke(null, ["""{ "name": "Json Sword", "class": "sword", "onHit": [ { "run": "other-mod.Nope", "heal": 1 } ] }"""])!;
            var e = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "jsonsword2", missing));
            Assert.Contains("no [ImpactNative] other-mod.Nope in other-mod's impact/natives.json", e.Message);
        }
        finally
        {
            RunNatives.Declarations = null;
        }
    }
}
