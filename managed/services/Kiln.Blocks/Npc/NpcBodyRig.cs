// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>Pure rig agreement shared by creatures and roster bodies; roster-only geometry and motion policy stays here, not in enemy lowering.</summary>
internal static class NpcBodyRig
{
    internal static readonly Guid FisherInteraction = new("81ed77f9-4c72-4730-8b05-a5ddee9c7164");

    internal static void SkeletonNames(BlockContext ctx, string body, string animation)
    {
        if (body != animation)
            throw new BlockError(ctx.Block, "animation", $"the body's skeleton is {body} and the animation's {animation}: the game poses a model only by the skeleton its graph names, so they must be one (the same glTF, or the same \"skeleton\")");
    }

    internal static void Cooked(BlockContext ctx, BodyBlock body, NpcCompiler.Graph graph, Guid modelSkeleton)
    {
        if (modelSkeleton != graph.Skeleton)
            throw new BlockError(ctx.Block, "body.model", $"the model is cooked on skeleton {modelSkeleton} and the graph on {graph.Skeleton}: the client would draw it unposed. Cook both from the same skeleton");
        foreach (var (slot, joint) in body.Slots ?? new Dictionary<Slot, string>())
            if (!graph.Slots.ContainsKey(slot.ToString()))
                throw new BlockError(ctx.Block, $"body.slots[{slot}]", $"the cooked graph {graph.Name} has no slot {slot} (joint {joint}): cook again");
    }

    internal static void Geometry(BlockContext ctx, BodyBlock body, JsonNode? box = null)
    {
        if (body.Template != null && body.Template != Bodies.Fisher.Template)
            throw new BlockError(ctx.Block, "body.template", "an authored roster body uses the Fisher/Villager chassis in this version");
        if (body.Scale is { } scale && (!double.IsFinite(scale) || scale != 1))
            throw new BlockError(ctx.Block, "body.scale", "an authored roster body keeps scale 1 in this version");
        if (body.Colliders is { Count: > 0 })
            throw new BlockError(ctx.Block, "body.colliders", "an authored roster body uses Size, not custom colliders in this version");
        if (body.Size is not { } size) throw new BlockError(ctx.Block, "body.size", "an authored roster body needs an explicit radius and height in metres");
        if (!double.IsFinite(size.Radius) || size.Radius is < 0.05 or > 4)
            throw new BlockError(ctx.Block, "body.size.radius", "a finite radius in [0.05, 4] metres");
        if (!double.IsFinite(size.Height) || size.Height is < 0.1 or > 8)
            throw new BlockError(ctx.Block, "body.size.height", "a finite height in [0.1, 8] metres");
        foreach (var slot in new[] { Slot.Root, Slot.Head, Slot.HandR })
            if (body.Slots == null || !body.Slots.TryGetValue(slot, out var joint) || string.IsNullOrWhiteSpace(joint))
                throw new BlockError(ctx.Block, $"body.slots[{slot}]", $"an authored roster body needs its {slot} joint");
        if (box == null) return;
        double Read(string end, string axis) => box[end]?[axis]?.GetValue<double>() ?? double.NaN;
        var min = new[] { Read("min", "x"), Read("min", "y"), Read("min", "z") };
        var max = new[] { Read("max", "x"), Read("max", "y"), Read("max", "z") };
        // Float32 model bounds may differ at the envelope boundary by a few ulps, never by a model-space margin.
        const double epsilon = 0.000001;
        if (min.Concat(max).Any(v => !double.IsFinite(v)) || min.Where((v, i) => v > max[i]).Any() ||
            Math.Abs(min[1]) > 0.05 + epsilon || max[1] > size.Height + epsilon ||
            Math.Abs(min[0]) > size.Radius + epsilon || Math.Abs(max[0]) > size.Radius + epsilon ||
            Math.Abs(min[2]) > size.Radius + epsilon || Math.Abs(max[2]) > size.Radius + epsilon)
            throw new BlockError(ctx.Block, "body.size", "the cooked model's floor must be within 0.05 m of Y0 and its bounds must fit Size's radius-height box");
    }

    internal static int Variants(NpcMotionRole role) => role is NpcMotionRole.Sit or NpcMotionRole.Sleep or NpcMotionRole.Door ? 3 : 1;
    internal static bool Required(NpcMotionRole role) => role is NpcMotionRole.Sit or NpcMotionRole.Sleep or NpcMotionRole.Inspect or NpcMotionRole.Jump or NpcMotionRole.Door;

    internal static void Shape(BlockContext ctx, NpcMotionsBlock motions, string path)
    {
        if (motions.Bindings == null) throw new BlockError(ctx.Block, $"{path}.bindings", "a binding list, not null");
        var seen = new HashSet<(NpcMotionRole, int)>();
        foreach (var (b, i) in motions.Bindings.Select((b, i) => (b, i)))
        {
            var at = $"{path}.bindings[{i}]";
            if (b == null) throw new BlockError(ctx.Block, at, "a motion binding, not null");
            if (!Enum.IsDefined(b.Role)) throw new BlockError(ctx.Block, $"{at}.role", "a known NPC motion role");
            if (b.Variant < 0 || b.Variant >= Variants(b.Role)) throw new BlockError(ctx.Block, $"{at}.variant", $"{b.Role} uses variants 0..{Variants(b.Role) - 1}");
            if (!seen.Add((b.Role, b.Variant))) throw new BlockError(ctx.Block, at, $"duplicate {b.Role} variant {b.Variant}");
            if (b.State == null) throw new BlockError(ctx.Block, $"{at}.state", "a graph state, not null");
            if (b.Role == NpcMotionRole.Interaction ? b.InteractionKey != FisherInteraction : b.InteractionKey != null)
                throw new BlockError(ctx.Block, $"{at}.interactionKey", b.Role == NpcMotionRole.Interaction ? "the Fisher chassis' supported nonzero interaction key" : "only Interaction takes an interaction key");
        }
        foreach (var role in Enum.GetValues<NpcMotionRole>())
            for (var v = 0; v < Variants(role); v++)
                if (!seen.Contains((role, v)) && (Required(role) || motions.OptionalFallback == null))
                    throw new BlockError(ctx.Block, $"{path}.bindings", $"missing {role} variant {v}" + (Required(role) ? "; required motions cannot fall back" : "; bind it or supply optionalFallback explicitly"));
    }

    /// <summary>Canonical 27-binding vector, with effective cooked clip durations. No sequence edits or game calls.</summary>
    internal static JsonArray Bindings(BlockContext ctx, NpcMotionsBlock motions, string mod, AnimationBlock animation,
        NpcCompiler.Graph graph, Func<Clip, bool, double?> length)
    {
        Shape(ctx, motions, "motions");
        JsonObject State(AnimState state, string path, bool fallback, NpcMotionRole role, int variant)
        {
            var guid = state.Guid(mod);
            if (guid == null || guid == Guid.Empty)
                throw new BlockError(ctx.Block, path, $"a sequence needs a state GUID; {state} has none usable in this cooked graph");
            var found = graph.States.FirstOrDefault(s => s.Value.Hash == state.Hash(mod));
            if (found.Key == null) throw new BlockError(ctx.Block, path, $"\"{state}\" is not a state of this body's cooked graph {graph.Name}");
            if (found.Value.Guid != guid)
                throw new BlockError(ctx.Block, path, $"a sequence needs a state GUID; {state} has none usable in this cooked graph");
            var clip = animation.States.FirstOrDefault(s => s.Key.Name == found.Key).Value ?? found.Key switch
            {
                "Idle" => animation.Idle, "Walk" => animation.Walk, "Run" => animation.Run, "Sprint" => animation.Sprint,
                "IdleCombat" => animation.IdleCombat ?? animation.Idle, "Fall" => animation.Fall ?? animation.Idle,
                "IdleSneak" => animation.IdleSneak, "Sneak" => animation.Sneak, "Dead" => animation.Dead ?? animation.Idle,
                "Stun" => animation.Stun ?? animation.Stagger ?? animation.Hit ?? animation.Idle,
                "Stagger" => animation.Stagger ?? animation.Hit ?? animation.Idle, _ => null,
            };
            if (clip == null || clip.Name == null || !double.IsFinite(clip.Speed ?? 1) || clip.Speed is <= 0)
                throw new BlockError(ctx.Block, path, "a motion state needs one cooked clip with finite positive speed (not a blend)");
            var loopDefault = found.Key is "Idle" or "Walk" or "Run" or "Sprint" or "IdleCombat" or "IdleSneak" or "Sneak";
            var loop = clip.Loop ?? loopDefault;
            if (!fallback && role is NpcMotionRole.Sit or NpcMotionRole.Sleep && loop != (variant == 1))
                throw new BlockError(ctx.Block, path, "Sit/Sleep entry and exit must not loop; their middle variant must loop");
            var seconds = length(clip, loopDefault);
            if (seconds == null || !double.IsFinite(seconds.Value) || seconds <= 0 || seconds > long.MaxValue / 1e9)
                throw new BlockError(ctx.Block, path, "the motion needs a finite positive duration from cooked clips.json; cook again");
            var ns = (long)Math.Round(seconds.Value * 1e9, MidpointRounding.AwayFromZero);
            if (ns <= 0) throw new BlockError(ctx.Block, path, "the effective motion duration rounds to zero nanoseconds");
            return new JsonObject { ["state"] = guid.Value.ToString(), ["durationNs"] = ns, ["loop"] = !fallback && loop, ["clamp"] = fallback };
        }
        // Even a fallback not needed by today's complete vector is still an authored reference and must be valid.
        if (motions.OptionalFallback is { } fb) State(fb, "motions.optionalFallback", true, NpcMotionRole.Greet, 0);
        var result = new JsonArray();
        foreach (var role in Enum.GetValues<NpcMotionRole>())
            for (var v = 0; v < Variants(role); v++)
            {
                var i = motions.Bindings.ToList().FindIndex(b => b.Role == role && b.Variant == v);
                var b = i >= 0 ? motions.Bindings[i] : null;
                var row = State(b?.State ?? motions.OptionalFallback!, i >= 0 ? $"motions.bindings[{i}].state" : "motions.optionalFallback", b == null, role, v);
                row["role"] = System.Text.Json.JsonNamingPolicy.CamelCase.ConvertName(role.ToString());
                row["variant"] = v;
                if (role == NpcMotionRole.Interaction) row["interactionKey"] = FisherInteraction.ToString();
                result.Add(row);
            }
        return result;
    }
}
