// SPDX-License-Identifier: MIT
// EffectBlock.Run (since 0.2.0-dev.29, chapter 06c 9.5): a mod's [ImpactNative] called first in the effect's own program, with the trigger's
// variables bound by the method's parameter names; a false (0) result skips the rest of the effect, Then included. The binding table:
//   self -> the effect's own entity (getSelf); target -> what the trigger gives it; attacker, caster -> the caster (the cast's root);
//   position -> the target's position (float3), or the place; attackMod -> this effect's hit's AttackMod (float32, inout: applyDamage
//   takes it); damage -> this effect's damage setup (impact.DamageSetup, inout: applyDamage takes it).
// Any other name, or a required one with nothing to bind here, is a BlockError (kiln lint KL340 says the same before the build).
using System.Text.Json;

namespace Kiln;

internal sealed partial class EffectEmitter
{
    internal static readonly string[] RunBindings = ["self", "target", "attacker", "caster", "position", "attackMod", "damage"];

    private void RunNative(NativeRef run, EffectBlock e, int? target, int? origin, int id, ProgramBuilder.Label end, string path)
    {
        var at = $"{path}.run";
        var (full, parameters) = RunNatives.Resolve(_ctx, run, at);
        var args = new int?[parameters.Count];
        for (var i = 0; i < parameters.Count; i++)
        {
            var p = parameters[i];
            int? v = p.Name switch
            {
                "self" => Self(),
                "target" => target,
                "attacker" or "caster" => Caster,
                "position" => target is { } t ? PositionOf(t, id) : origin is null ? null : _originPosition,
                "attackMod" => e.Damage != null || e.Knockback != Knockback.None ? AttackMod(id) : null,
                "damage" => e.Damage != null ? DamagePart(e, id).Damage : null,
                _ => throw new BlockError(_ctx.Block, at, $"{full}: parameter '{p.Name}' names nothing a Run native gets: its parameters are " +
                    $"named {string.Join(", ", RunBindings)} (KL340)"),
            };
            if (v is null)
            {
                if (!p.Arg.Optional)
                    throw new BlockError(_ctx.Block, at, $"{full}: parameter '{p.Name}' has nothing to bind to here " +
                        (p.Name switch
                        {
                            "target" or "position" => "(this place has no target: make it Optional, or use a trigger with a target)",
                            "attackMod" or "damage" => "(this effect deals no damage: give it damage, or make the parameter Optional)",
                            _ => "",
                        }) + " (KL340)");
                args[i] = null;
                continue;
            }
            var have = _b.TypeOf(v.Value);
            if (p.Arg.Type != "any" && RunNatives.Fnv(p.Arg.Type) != have.Hash)
                throw new BlockError(_ctx.Block, at, $"{full}: parameter '{p.Name}' is {p.Arg.Type}; what it binds to is another type (KL340)");
            args[i] = v;
        }
        _b.CallNative(full, RunNatives.Fnv(full), $"run{id}", args);
        _b.IfFalse(end);
    }

    private int Self()
    {
        var self = _b.V("self", ImpactType.Entity);
        _b.Call("getSelf", null, self);
        _b.Pop();
        return self;
    }

    private int PositionOf(int target, int id)
    {
        var pos = _b.V($"hitAt{id}", ImpactType.Float3);
        _b.Call("getTransform", null, null, pos, target);
        _b.Pop();
        return pos;
    }

    private int AttackMod(int id)
    {
        if (!_attackMods.TryGetValue(id, out var v)) _attackMods[id] = v = _b.VFloat($"attackMod{id}", 1);
        return v;
    }
}

/// <summary>Where a Run native's parameters come from: the <see cref="NativeRef"/> the build generated (C#), or for a JSON block's
/// <c>"run": "&lt;modid&gt;.&lt;Name&gt;"</c>, that mod's <c>impact/natives.json</c>, which its build wrote.</summary>
internal static class RunNatives
{
    /// <summary>Tests: the declarations of a full name (null: read the mod's impact/natives.json).</summary>
    internal static Func<string, IReadOnlyList<NativeParameter>?>? Declarations;

    internal static uint Fnv(string s)
    {
        var h = 0x811c9dc5u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(s)) h = (h ^ b) * 0x01000193u;
        return h;
    }

    internal static (string Full, IReadOnlyList<NativeParameter> Parameters) Resolve(BlockContext ctx, NativeRef run, string at)
    {
        // C#: the build's NativeRef (Natives.OnHit), a short name of this mod with its parameters
        if (!run.Name.Contains('.')) return (run.FullName ?? $"{ctx.Mod.Id}.{run.Name}", run.Parameters);
        // JSON: "<modid>.<Name>"; its parameters from that mod's impact/natives.json
        var full = run.Name;
        if (run.Parameters.Count > 0) return (full, run.Parameters);
        var dot = full.LastIndexOf('.');
        var mod = full[..dot];
        var found = Declarations?.Invoke(full) ?? Read(mod, full);
        return found is null
            ? throw new BlockError(ctx.Block, at, $"run: no [ImpactNative] {full} in {mod}'s impact/natives.json (the build writes it; KL340)")
            : (full, found);
    }

    private static IReadOnlyList<NativeParameter>? Read(string mod, string full)
    {
        var dir = Kiln.Mods.Get(mod)?.Info?.Directory;
        var file = dir is null ? null : Path.Combine(dir, "impact", "natives.json");
        if (file is null || !File.Exists(file)) return null;
        using var doc = JsonDocument.Parse(File.ReadAllText(file));
        if (!doc.RootElement.TryGetProperty("natives", out var natives)) return null;
        foreach (var n in natives.EnumerateArray())
        {
            if (!n.TryGetProperty("name", out var name) || name.GetString() != full) continue;
            var list = new List<NativeParameter>();
            if (n.TryGetProperty("args", out var args))
                foreach (var a in args.EnumerateArray())
                {
                    var dir2 = a.TryGetProperty("dir", out var d) ? d.GetString() : "in";
                    list.Add(new NativeParameter(a.GetProperty("name").GetString() ?? "",
                        new NativeArg(a.TryGetProperty("type", out var ty) ? ty.GetString() ?? "any" : "any",
                            dir2 == "out" ? NativeArgDirection.Out : dir2 == "inout" ? NativeArgDirection.InOut : NativeArgDirection.In,
                            a.TryGetProperty("optional", out var o) && o.GetBoolean(), a.TryGetProperty("raw", out var r) && r.GetBoolean(),
                            a.TryGetProperty("size", out var sz) ? sz.GetInt32() : 0)));
                }
            return list;
        }
        return null;
    }
}

/// <summary>JSON <c>"run": "&lt;modid&gt;.&lt;Name&gt;"</c>: a named native, its parameters read from that mod's impact/natives.json.</summary>
internal sealed class NativeRefJsonConverter : System.Text.Json.Serialization.JsonConverter<NativeRef>
{
    public override NativeRef Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        var s = r.GetString();
        if (string.IsNullOrWhiteSpace(s) || s.LastIndexOf('.') <= 0 || s.EndsWith('.'))
            throw new JsonException($"run: \"{s}\" is not \"<modid>.<Name>\"");
        return new NativeRef(s, []);
    }

    public override void Write(Utf8JsonWriter w, NativeRef v, JsonSerializerOptions o) => w.WriteStringValue(v.FullName ?? v.Name);
}
