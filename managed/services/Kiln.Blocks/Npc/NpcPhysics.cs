// SPDX-License-Identifier: MIT
// Cloth and bone chains on a body's model (docs/kiln/modding/npc-blocks.md#model-physics): the records, the checks a body's are given
// at Define, the spec hash the cook writes into <stem>.model.json (tools/modelcook/cook/physics.py spec_bytes, byte for byte), and the warning a
// mod gets while the cook features that write them wait for their game test. Nothing here cooks: a body asks for what its build cooked.
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A shape a body's cloth does not pass through: a <see cref="ClothSphereBlock"/> or a <see cref="ClothCapsuleBlock"/>. In blocks/*.json
/// <c>{ "type": "sphere", ... }</c> or <c>{ "type": "capsule", "a": { ... }, "b": { ... } }</c>.</summary>
[JsonPolymorphic(TypeDiscriminatorPropertyName = "type", UnknownDerivedTypeHandling = JsonUnknownDerivedTypeHandling.FailSerialization)]
[JsonDerivedType(typeof(ClothSphereBlock), "sphere")]
[JsonDerivedType(typeof(ClothCapsuleBlock), "capsule")]
public abstract record ClothColliderBlock;

/// <summary>A sphere collider of cloth, in the model's bind pose: metres, glTF axes (y up), as the model file has them.</summary>
/// <param name="X">Its centre.</param>
/// <param name="Y">Its centre.</param>
/// <param name="Z">Its centre.</param>
/// <param name="Radius">Metres, above 0 and at most 8.</param>
/// <param name="Joint">The skeleton joint it follows with all its weight (the joint's name, or <c>j&lt;index&gt;</c>). Null: rigid on the model,
/// which only a model without a skeleton has.</param>
public sealed record ClothSphereBlock(double X, double Y, double Z, double Radius, string? Joint = null) : ClothColliderBlock;

/// <summary>A capsule collider of cloth: two spheres and the space between them. Both ends follow one joint (the game keeps one skin record per
/// capsule), and their centres differ.</summary>
public sealed record ClothCapsuleBlock(ClothSphereBlock A, ClothSphereBlock B) : ClothColliderBlock;

/// <summary>
/// One primitive of a body's model the game simulates as cloth. The cook writes it into the model (<c>kiln build</c>, <c>kiln cook</c>); it needs
/// the cook feature <c>modelcook.cloth</c> (on since 0.2.0-dev.31; not yet confirmed in a game client, guided check <c>16e-cc16e</c>). Every field
/// is checked when the block is defined, and again by the cook against the model's vertices.
/// </summary>
public sealed record ModelClothBlock
{
    /// <summary>The glTF mesh, by its index in the file.</summary>
    public int Mesh { get; init; }
    /// <summary>The primitive of that mesh, by its index.</summary>
    public int Primitive { get; init; }
    /// <summary>One id (0 or more) per POSITION element of the primitive. Equal ids weld the copies a UV or normal seam made: they sit within
    /// 1e-6 m and carry the same skin. Particles are numbered in the order their id first appears. Null: every vertex is its own particle.</summary>
    public IReadOnlyList<int>? ParticleIds { get; init; }
    /// <summary>One value in [0, 1] per particle, in particle order: 1 holds it on the model (it follows its joint), 0 lets it hang, between makes
    /// it heavier and limits its motion. Every connected piece of cloth needs a particle with 1. Required.</summary>
    public IReadOnlyList<double> Pin { get; init; } = [];
    /// <summary>The whole cloth's mass in kg, [0.001, 100], spread over its particles by the area around each. Default 1.</summary>
    public double MassKg { get; init; } = 1;
    /// <summary>m/s² downwards, [0, 50]. Default 9.81.</summary>
    public double Gravity { get; init; } = 9.81;
    /// <summary>[0, 0.99]. Default 0.4.</summary>
    public double Damping { get; init; } = .4;
    /// <summary>One stiffness for stretch and bend, above 0 and at most 1. Default 0.8.</summary>
    public double Stiffness { get; init; } = .8;
    /// <summary>What it does not pass through. The colliders of all cloth of a model are one list: at most 32 spheres, a capsule counting as two,
    /// a collider named twice counting once.</summary>
    public IReadOnlyList<ClothColliderBlock> Colliders { get; init; } = [];
}

/// <summary>
/// A chain of the skeleton's joints the game swings (its chains2): from <see cref="RootJoint"/>, whose parent stays animated, to
/// <see cref="TipJoint"/> along one branch, 2 to 8 joints. The cook writes it into the model; it needs the cook feature <c>modelcook.chains2</c>
/// (on since 0.2.0-dev.31; not yet confirmed in a game client, guided check <c>16e-cc16e</c>). The joints of a chain carry no animation.
/// </summary>
public sealed record ModelChainBlock
{
    /// <summary>The first joint that swings: a name of the model's skeleton.</summary>
    public string RootJoint { get; init; } = "";
    /// <summary>The last joint, below the root.</summary>
    public string TipJoint { get; init; } = "";
    /// <summary>The mass of one moving joint in kg (not of the chain), [0.001, 100]. Default 1.</summary>
    public double ParticleMassKg { get; init; } = 1;
    /// <summary>[0, 0.99]. Default 0.01.</summary>
    public double Damping { get; init; } = .01;
    /// <summary>The game's own coefficient, [1, 1000000]. Default 10000.</summary>
    public double Stiffness { get; init; } = 10000;
    /// <summary>The game's own coefficient, [0, 1000000]. Default 5.</summary>
    public double BendStiffness { get; init; } = 5;
    /// <summary>Metres, [0, 1] (stored in millimetres). Default 0.</summary>
    public double ParticleRadius { get; init; }
}

internal static class NpcPhysics
{
    internal const string ClothRow = "modelcook.cloth", ChainsRow = "modelcook.chains2";
    private const int MaxClothMeshes = 255, MaxParticles = 65535, MaxColliderSpheres = 32;
    private const double Weld = 1e-6;
    private const string Spec = "kiln-physics-1;";

    /// <summary>Whether a capability is held until its game test (a test sets its own).</summary>
    internal static Func<string, bool> IsHeld = Capabilities.IsHeld;

    internal static bool Has(BodyBlock b) => b.Cloth is { Count: > 0 } || b.Chains is { Count: > 0 };

    private static string Join(string path, string field) => string.IsNullOrEmpty(path) ? field : $"{path}.{field}";

    private static string N(double v) => v.ToString("R", CultureInfo.InvariantCulture);

    private static void Range(BlockContext ctx, double v, double lo, double hi, string field, bool openLow = false)
    {
        if (!double.IsFinite(v) || (openLow ? v <= lo : v < lo) || v > hi)
            throw new BlockError(ctx.Block, field, $"{N(v)} is outside {(openLow ? "(" : "[")}{N(lo)}, {N(hi)}]");
    }

    /// <summary>The checks of a body's cloth and chains that need no model file (the cook checks the rest against the vertices and the skeleton).</summary>
    internal static void Check(BlockContext ctx, BodyBlock b, string path)
    {
        if (!Has(b)) return;
        var field = Join(path, b.Cloth is { Count: > 0 } ? "cloth" : "chains");
        if (b.Model == null) throw new BlockError(ctx.Block, field, "cloth and chains are cooked into a model of yours: a game body has its own");
        if (b.Scale is { } scale && scale != 1)
            throw new BlockError(ctx.Block, Join(path, "scale"), $"a body with cloth or chains keeps scale 1 in this version ({N(scale)} given): the game scales the cloth's colliders by the model's own size, not by this");
        var cloth = b.Cloth ?? [];
        if (cloth.Count > MaxClothMeshes) throw new BlockError(ctx.Block, Join(path, "cloth"), $"{cloth.Count} cloth meshes: a model has at most {MaxClothMeshes}");
        var seen = new Dictionary<(int, int), int>();
        var all = new List<ClothColliderBlock>();
        for (var i = 0; i < cloth.Count; i++)
        {
            var c = cloth[i] ?? throw new BlockError(ctx.Block, Join(path, $"cloth[{i}]"), "a cloth entry, not null");
            var at = Join(path, $"cloth[{i}]");
            if (c.Mesh < 0) throw new BlockError(ctx.Block, $"{at}.mesh", $"{c.Mesh} is outside [0, {int.MaxValue}]");
            if (c.Primitive < 0) throw new BlockError(ctx.Block, $"{at}.primitive", $"{c.Primitive} is outside [0, {int.MaxValue}]");
            if (seen.TryGetValue((c.Mesh, c.Primitive), out var first))
                throw new BlockError(ctx.Block, at, $"mesh {c.Mesh} primitive {c.Primitive} is already cloth in {Join(path, $"cloth[{first}]")}");
            seen[(c.Mesh, c.Primitive)] = i;
            if (c.ParticleIds is { } ids)
            {
                if (ids.Count == 0) throw new BlockError(ctx.Block, $"{at}.particleIds", "one integer per POSITION element of the primitive");
                if (ids.Count > MaxParticles) throw new BlockError(ctx.Block, $"{at}.particleIds", $"{ids.Count} vertices: a cloth mesh has at most {MaxParticles}");
                for (var k = 0; k < ids.Count; k++)
                    if (ids[k] < 0) throw new BlockError(ctx.Block, $"{at}.particleIds[{k}]", $"{ids[k]} is outside [0, {int.MaxValue}]");
            }
            var pin = c.Pin ?? [];
            if (pin.Count == 0) throw new BlockError(ctx.Block, $"{at}.pin", "one value in [0, 1] per particle");
            if (pin.Count > MaxParticles) throw new BlockError(ctx.Block, $"{at}.pin", $"{pin.Count} particles: a cloth mesh has at most {MaxParticles}");
            for (var k = 0; k < pin.Count; k++) Range(ctx, pin[k], 0, 1, $"{at}.pin[{k}]");
            if (c.ParticleIds is { } pids && pids.Distinct().Count() != pin.Count)
                throw new BlockError(ctx.Block, $"{at}.pin", $"{pin.Count} values for {pids.Distinct().Count()} particles (distinct particleIds)");
            Range(ctx, c.MassKg, .001, 100, $"{at}.massKg");
            Range(ctx, c.Gravity, 0, 50, $"{at}.gravity");
            Range(ctx, c.Damping, 0, .99, $"{at}.damping");
            Range(ctx, c.Stiffness, 0, 1, $"{at}.stiffness", openLow: true);
            var cols = c.Colliders ?? [];
            for (var k = 0; k < cols.Count; k++)
            {
                var cw = $"{at}.colliders[{k}]";
                switch (cols[k])
                {
                    case ClothSphereBlock s:
                        CheckSphere(ctx, s, cw);
                        break;
                    case ClothCapsuleBlock cap:
                        if (cap.A == null || cap.B == null) throw new BlockError(ctx.Block, cw, "a capsule has two ends, a and b");
                        CheckSphere(ctx, cap.A, $"{cw}.a");
                        CheckSphere(ctx, cap.B, $"{cw}.b");
                        if (cap.A.Joint != cap.B.Joint)
                            throw new BlockError(ctx.Block, $"{cw}.b.joint", $"'{cap.B.Joint}': both ends of a capsule follow one joint ('{cap.A.Joint}'), the format has one skin record per capsule");
                        var d = Math.Sqrt(Math.Pow(cap.A.X - cap.B.X, 2) + Math.Pow(cap.A.Y - cap.B.Y, 2) + Math.Pow(cap.A.Z - cap.B.Z, 2));
                        if (d <= Weld) throw new BlockError(ctx.Block, cw, "both ends of the capsule are at one point: make it a sphere");
                        break;
                    default:
                        throw new BlockError(ctx.Block, cw, "a collider is a ClothSphereBlock or a ClothCapsuleBlock");
                }
            }
            if (Spheres(cols) > MaxColliderSpheres)
                throw new BlockError(ctx.Block, $"{at}.colliders", $"{Spheres(cols)} spheres (a capsule is two): the client takes at most {MaxColliderSpheres}");
            foreach (var x in cols) if (!all.Contains(x)) all.Add(x);
        }
        if (Spheres(all) > MaxColliderSpheres)
            throw new BlockError(ctx.Block, Join(path, "cloth"), $"the model's colliders come to {Spheres(all)} spheres (a capsule is two, each collider counted once): the client takes at most {MaxColliderSpheres}");
        var chains = b.Chains ?? [];
        var pairs = new Dictionary<(string, string), int>();
        for (var i = 0; i < chains.Count; i++)
        {
            var c = chains[i] ?? throw new BlockError(ctx.Block, Join(path, $"chains[{i}]"), "a chain, not null");
            var at = Join(path, $"chains[{i}]");
            if (string.IsNullOrEmpty(c.RootJoint)) throw new BlockError(ctx.Block, $"{at}.rootJoint", "the first joint that swings: a joint's name");
            if (string.IsNullOrEmpty(c.TipJoint)) throw new BlockError(ctx.Block, $"{at}.tipJoint", "the last joint of the chain: a joint's name");
            if (c.RootJoint == c.TipJoint) throw new BlockError(ctx.Block, $"{at}.tipJoint", $"'{c.TipJoint}' is the root too: a chain has 2 to 8 joints");
            if (pairs.TryGetValue((c.RootJoint, c.TipJoint), out var first))
                throw new BlockError(ctx.Block, at, $"the chain '{c.RootJoint}' to '{c.TipJoint}' is already declared in {Join(path, $"chains[{first}]")}");
            pairs[(c.RootJoint, c.TipJoint)] = i;
            Range(ctx, c.ParticleMassKg, .001, 100, $"{at}.particleMassKg");
            Range(ctx, c.Damping, 0, .99, $"{at}.damping");
            Range(ctx, c.Stiffness, 1, 1_000_000, $"{at}.stiffness");
            Range(ctx, c.BendStiffness, 0, 1_000_000, $"{at}.bendStiffness");
            Range(ctx, c.ParticleRadius, 0, 1, $"{at}.particleRadius");
        }
    }

    private static void CheckSphere(BlockContext ctx, ClothSphereBlock s, string at)
    {
        foreach (var (v, f) in new[] { (s.X, "x"), (s.Y, "y"), (s.Z, "z") })
            if (!double.IsFinite(v)) throw new BlockError(ctx.Block, $"{at}.{f}", $"{N(v)} is not a finite number");
        Range(ctx, s.Radius, 0, 8, $"{at}.radius", openLow: true);
        if (s.Joint is "") throw new BlockError(ctx.Block, $"{at}.joint", "'' is not a joint name (or null for a rigid collider)");
    }

    private static int Spheres(IEnumerable<ClothColliderBlock> cols) => cols.Sum(c => c is ClothCapsuleBlock ? 2 : 1);

    // ---- the spec hash: tools/modelcook/cook/physics.py spec_bytes, the same bytes -------------------------------------------

    private sealed class SpecWriter
    {
        private readonly List<byte> _b = [.. Encoding.ASCII.GetBytes(Spec)];

        private void Ascii(string s) => _b.AddRange(Encoding.ASCII.GetBytes(s));
        public void Int(int v) => Ascii($"i{v.ToString(CultureInfo.InvariantCulture)};");
        public void Double(double v) => Ascii($"d{unchecked((ulong)BitConverter.DoubleToInt64Bits(v)):x16};");
        public void Null() => Ascii("n;");

        public void Str(string? s)
        {
            if (s == null) { Null(); return; }
            var b = Encoding.UTF8.GetBytes(s);
            Ascii($"s{b.Length.ToString(CultureInfo.InvariantCulture)}:");
            _b.AddRange(b);
            Ascii(";");
        }

        public void List(int count) => Ascii($"l{count.ToString(CultureInfo.InvariantCulture)}[");
        public void End() => Ascii("]");
        public byte[] Bytes() => [.. _b];
    }

    private static void SphereFields(SpecWriter w, ClothSphereBlock s)
    {
        w.Double(s.X); w.Double(s.Y); w.Double(s.Z); w.Double(s.Radius); w.Str(s.Joint);
    }

    /// <summary>The spec form of a body's cloth and chains (null: it has none).</summary>
    internal static byte[]? SpecBytes(BodyBlock b)
    {
        if (!Has(b)) return null;
        var w = new SpecWriter();
        var cloth = b.Cloth ?? [];
        var chains = b.Chains ?? [];
        w.List(2);
        w.List(cloth.Count);
        foreach (var c in cloth)
        {
            w.List(9);
            w.Int(c.Mesh);
            w.Int(c.Primitive);
            if (c.ParticleIds is { } ids)
            {
                w.List(ids.Count);
                foreach (var id in ids) w.Int(id);
                w.End();
            }
            else w.Null();
            w.List(c.Pin.Count);
            foreach (var p in c.Pin) w.Double(p);
            w.End();
            w.Double(c.MassKg); w.Double(c.Gravity); w.Double(c.Damping); w.Double(c.Stiffness);
            w.List(c.Colliders.Count);
            foreach (var x in c.Colliders)
            {
                if (x is ClothSphereBlock s)
                {
                    w.List(6); w.Str("sphere"); SphereFields(w, s); w.End();
                }
                else if (x is ClothCapsuleBlock cap)
                {
                    w.List(3); w.Str("capsule");
                    w.List(5); SphereFields(w, cap.A); w.End();
                    w.List(5); SphereFields(w, cap.B); w.End();
                    w.End();
                }
            }
            w.End();
            w.End();
        }
        w.End();
        w.List(chains.Count);
        foreach (var c in chains)
        {
            w.List(7);
            w.Str(c.RootJoint); w.Str(c.TipJoint);
            w.Double(c.ParticleMassKg); w.Double(c.Damping); w.Double(c.Stiffness); w.Double(c.BendStiffness); w.Double(c.ParticleRadius);
            w.End();
        }
        w.End();
        w.End();
        return w.Bytes();
    }

    /// <summary>sha256 of <see cref="SpecBytes"/>, lower-case hex: what the cook writes into <c>&lt;stem&gt;.model.json</c> as <c>physics.spec</c>.</summary>
    internal static string? SpecHash(BodyBlock b) => SpecBytes(b) is { } bytes ? Convert.ToHexStringLower(SHA256.HashData(bytes)) : null;

    /// <summary>A body that carries cloth or chains asks for what its build cooked into the model: the side file's spec hash is its own, or the
    /// block is refused with the build step that is missing. Nothing is cooked while the game runs.</summary>
    internal static void CheckCooked(BlockContext ctx, BodyBlock b, JsonNode side, string model, string path)
    {
        var want = SpecHash(b);
        if (want == null) return;
        var field = Join(path, b.Cloth is { Count: > 0 } ? "cloth" : "chains");
        string? got = null;
        try { got = side["physics"]?["spec"]?.GetValue<string>(); }
        catch (Exception e) when (e is InvalidOperationException or FormatException) { }
        if (got == null)
            throw new BlockError(ctx.Block, field, $"its model {model} was cooked without cloth or chains: they are cooked into the model when the mod builds (kiln build, or kiln cook in the mod's folder), never while the game runs. A body written in C# is not seen by the cook: put it in blocks/*.json, or the model's cloth in assets/physics.json, and build again");
        if (!string.Equals(got, want, StringComparison.Ordinal))
            throw new BlockError(ctx.Block, field, $"its model {model} was cooked with other cloth or chains than this body asks for: build the mod again (kiln build, or kiln cook in the mod's folder)");
    }

    /// <summary>One warning per mod and row while a body carries what a held cook feature wrote: the mod's build turned the feature on, and
    /// the game has not been seen to show it right yet. <c>"preview"</c> in kiln.json lifts the row and so this warning. Since 0.2.0-dev.31 no
    /// release holds the two rows, so this says nothing unless a release holds one again (a gate entry in native/core/gates).</summary>
    internal static void WarnHeld(BlockContext ctx, Lowered low, BodyBlock b)
    {
        foreach (var (row, has, what) in new[] { (ClothRow, b.Cloth is { Count: > 0 }, "cloth"), (ChainsRow, b.Chains is { Count: > 0 }, "bone chains") })
        {
            if (!has || !IsHeld(row)) continue;
            var mod = ctx.Mod;
            var text = $"{row}: a body's model carries {what}, cooked with \"experimental\": [\"{row}\"] in assets/cook.json. It has not passed its game test yet, so players may see it wrong or not at all (\"preview\" in kiln.json lifts the row)";
            low.AfterCommit(() =>
            {
                lock (mod.Warnings)
                {
                    if (mod.Warnings.Contains(text)) return;
                    mod.Warnings.Add(text);
                }
                mod.ReportDirty = true;
                BlocksBridge.Log(LogLevel.Warn, $"blocks: {mod.Id}: {text}");
            });
        }
    }
}
