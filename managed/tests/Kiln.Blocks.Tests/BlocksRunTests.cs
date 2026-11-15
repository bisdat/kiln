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
    public void A_matching_type_with_the_wrong_size_is_a_compile_error()
    {
        var native = new NativeRef("WrongSize", [P("target", "ecs.EntityId", size: 8)]);
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "wrongsize", Sword(new EffectBlock { Run = native, Heal = 1 })));
        Assert.Contains("size", e.Message, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("Bad.Mod.Native")]
    [InlineData("other-mod.Bad Name")]
    [InlineData("other-mod.é")]
    public void Invalid_full_native_names_are_rejected(string full)
    {
        var native = new NativeRef(full, [P("target", "ecs.EntityId")]);
        Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "badname", Sword(new EffectBlock { Run = native, Heal = 1 })));
    }

    [Fact]
    public void An_overlong_native_name_is_rejected_and_a_dotted_mod_id_is_valid()
    {
        var native = new NativeRef(new string('a', 91), []);
        Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "longname", Sword(new EffectBlock { Run = native, Heal = 1 })));
        var dotted = new NativeRef("org.example.mod.Ok", [P("target", "ecs.EntityId")]);
        Compiler.Lower(NewMod(), "dotted", Sword(new EffectBlock { Run = dotted, Heal = 1 }));
    }

    [Fact]
    public void A_ref_position_is_not_overwritten_before_applyDamage()
    {
        var native = new NativeRef("MoveHit", [P("position", "float3", NativeArgDirection.InOut, size: 12)]);
        var mod = NewMod();
        Compiler.Lower(mod, "position", Sword(new EffectBlock { Run = native, Damage = new DamageBlock { Ice = 1 } }));
        var lines = Listing(mod).Split('\n').ToList();
        var run = lines.FindIndex(l => l.Contains($"{Mod}.MoveHit(hitAt1)"));
        var hit = lines.FindIndex(l => l.Contains("applyDamage("));
        Assert.True(run >= 0 && hit > run, Listing(mod));
        Assert.DoesNotContain(lines.Skip(run + 1).Take(hit - run - 1), l => l.Contains("getTransform(") && l.Contains("hitAt1"));
    }

    [Theory]
    [InlineData("self", "ecs.EntityId", 4)]
    [InlineData("target", "ecs.EntityId", 4)]
    [InlineData("attacker", "ecs.EntityId", 4)]
    [InlineData("caster", "ecs.EntityId", 4)]
    [InlineData("position", "float3", 12)]
    [InlineData("attackMod", "float32", 4)]
    [InlineData("damage", "impact.DamageSetup", 52)]
    public void Every_binding_preserves_its_type_and_size_for_in_out_and_ref(string binding, string type, int size)
    {
        foreach (var dir in Enum.GetValues<NativeArgDirection>())
        {
            var native = new NativeRef("Bound", [P(binding, type, dir, optional: true, size: size)]);
            Compiler.Lower(NewMod(), "bound", Sword(new EffectBlock { Run = native, Damage = new DamageBlock { Ice = 2 } }));
        }
    }

    [Theory]
    [InlineData("Time", 8)]
    [InlineData("sint32", 4)]
    [InlineData("uint32", 4)]
    [InlineData("bool", 1)]
    public void Types_without_a_matching_binding_are_compile_errors(string type, int size)
    {
        var native = new NativeRef("Unsupported", [new NativeParameter("target", new NativeArg(type, Raw: type == "uint32", Size: size))]);
        Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "unsupported", Sword(new EffectBlock { Run = native, Heal = 1 })));
    }

    [Fact]
    public void A_false_Run_skips_every_part_and_Then_but_preserves_following_effects_and_query_cleanup()
    {
        var mod = NewMod();
        var program = EffectEmitter.PointProgram(new BlockContext(mod, "guard"), "guard", [
            new EffectBlock { On = EffectOn.Caster, Run = new NativeRef("Guard", []), Damage = new DamageBlock { Ice = 2 }, Heal = 1,
                Area = new AreaBlock { Radius = 2, Effects = [new EffectBlock { Heal = 2 }] }, Then = [new EffectBlock { Heal = 3 }] },
            new EffectBlock { On = EffectOn.Caster, Heal = 4 }
        ], "effects", primary: false);
        uint[] Words(int field)
        {
            var start = field + BitConverter.ToInt32(program.Blob, field);
            return Enumerable.Range(0, BitConverter.ToInt32(program.Blob, field + 4)).Select(i => BitConverter.ToUInt32(program.Blob, start + i * 4)).ToArray();
        }
        var calls = new List<uint>();
        void Execute(uint[] code)
        {
            var stack = new Stack<uint>();
            for (var pc = 0; pc < code.Length;)
                switch (code[pc++])
                {
                    case 11: stack.Push(code[pc++]); break;
                    case 12: stack.Push(0); break;
                    case 10: var to = code[pc++]; if (stack.Pop() == 0) pc = (int)to; break;
                    case 27: stack.Pop(); break;
                    case 19:
                        pc++;
                        var hash = code[pc++]; calls.Add(hash);
                        var argc = hash == RunNatives.Fnv($"{Mod}.Guard") ? 0 : hash switch {
                            0x01d09759 or 0xbf5ad13b => 1, 0xa18f5959 => 3, 0x044e4187 or 0x93e40aa9 => 6,
                            _ => throw new InvalidOperationException($"unexpected native 0x{hash:x8}") };
                        for (var i = 0; i < argc; i++) stack.Pop();
                        stack.Push(hash == RunNatives.Fnv($"{Mod}.Guard") ? 0u : 1u);
                        break;
                    case 30: Assert.Empty(stack); return;
                    default: throw new InvalidOperationException("unexpected opcode on the false path");
                }
        }
        Execute(Words(0x18));
        Assert.Equal(1, calls.Count(h => h == 0x044e4187));
        Assert.DoesNotContain(0xb04d0e7au, calls);
        Assert.DoesNotContain(0x19143416u, calls);
        Execute(Words(0x20));
        Assert.Contains(0x93e40aa9u, calls);
    }

    private sealed class FileMods(ModEntry entry) : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => [entry];
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => id == entry.Id ? entry : null;
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }

    [Theory]
    [InlineData(false, 1)]
    [InlineData(true, 2)]
    [InlineData(true, 1)]
    public void Json_Run_requires_a_sync_declaration_with_the_same_argc_as_its_parameters(bool sync, int argc)
    {
        var dir = Path.Combine(Path.GetTempPath(), "kiln-run-declaration-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(dir, "impact"));
        var previous = ModsBackend.Current;
        try
        {
            ModsBackend.Current = new FileMods(new ModEntry(Mod, Mod, "1.0", dir, ModSide.Server, ModState.Loaded, null, [], 0, new ModInfo(Mod, Mod, "1.0", dir)));
            File.WriteAllText(Path.Combine(dir, "impact", "natives.json"), System.Text.Json.JsonSerializer.Serialize(new {
                format = 1, natives = new[] { new { name = $"{Mod}.FileNative", argc, sync,
                    args = new[] { new { name = "target", type = "ecs.EntityId", size = 4 } } } } }));
            void Compile() => Compiler.Lower(NewMod(), "file-native", Sword(new EffectBlock { Run = new NativeRef($"{Mod}.FileNative", []), Heal = 1 }));
            if (sync && argc == 1) Compile();
            else Assert.Contains(sync ? "declares argc 2 for 1 named parameter(s)" : "is not \"sync\": true", Assert.Throws<BlockError>(Compile).Message);
        }
        finally { ModsBackend.Current = previous; Directory.Delete(dir, true); }
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
