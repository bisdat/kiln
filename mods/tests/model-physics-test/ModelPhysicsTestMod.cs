// SPDX-License-Identifier: MIT
using System.Security.Cryptography;
using Kiln;

namespace ModelPhysicsTest;

/// <summary>
/// Lab test of cloth and bone chains in the model cook, on a dedicated server with no player (docs/kiln/modding/models.md#physics). The
/// cooked banner (cloth from assets/physics.json) and pendant (a chain from blocks/pendant.json) come from build-assets.py. A server draws
/// nothing and simulates nothing: what it can prove is that the cooked bytes travel through kiln.assets unchanged and that the game's own
/// reflection reads the arrays the cook meant.
/// <list type="number">
/// <item><c>manifest-zero-errors</c>: the cook's manifest loads with no error; its four model entries are the clients' (skipped here), the
/// pendant's skeleton loads.</item>
/// <item><c>key</c> and <c>blob</c> lines: each model registered with its render data reads back byte for byte, hashes as the cook's report says,
/// and its render data reference names its blob.</item>
/// <item><c>arrays-match-golden</c>: the banner's fabric, masses and maps and the pendant's chain read through the game's reflection equal the
/// research's golden and the cook's report.</item>
/// <item><c>physics-off-build-refused</c>: build-assets.py's cook without the recipe's "experimental" key was refused by name and wrote nothing.</item>
/// <item><c>held-row-warned-once</c>: the pendant body carries a chain while modelcook.chains2 is held: one warning for the mod.</item>
/// <item><c>cleanup</c>: what the test registered is gone again, and a game resource reads as before.</item>
/// </list>
/// </summary>
public sealed class ModelPhysicsTestMod : Mod
{
    private const string Tag = "model-physics";
    private static readonly Guid BronzeSword = Guid.Parse("1d622bd9-8170-41b1-822e-d3984b386d95");  // keen::ItemInfo on both sides
    private static readonly string[] Models = ["banner", "models_pendant"];
    private int _pass, _fail;

    public override void OnLoad()
    {
        if (Mods.Side != ModSide.Server)
        {
            Log.Info($"{Tag}: a game client: this lab test runs on a dedicated server");
            return;
        }
        if (!Assets.Available)
        {
            Log.Info($"{Tag} done: 0 PASS, 1 FAIL (kiln.assets unavailable)");
            return;
        }
        _ = RunAsync();
    }

    private async Task RunAsync()
    {
        for (var i = 0; i < 5; i++) await Context.Scheduler.NextTick();
        try { Run(); }
        catch (Exception e) { Check(false, $"the test stopped: {e.GetType().Name}: {e.Message}"); }
        // 5. the held row's warning: once for the mod, whatever the number of bodies. Kiln.Blocks reads blocks/*.json after OnLoad: wait for the body.
        for (var i = 0; i < 600 && Blocks.Find("pendant", Context.Info.Id) == null; i++) await Context.Scheduler.NextTick();
        var held = Capabilities.IsHeld("modelcook.chains2");
        var warned = Blocks.Warnings(Context.Info.Id).Count(w => w.StartsWith("modelcook.chains2:", StringComparison.Ordinal));
        Check(Blocks.Find("pendant", Context.Info.Id) != null && (held ? warned == 1 : warned == 0),
              $"held-row-warned-once (the pendant body {(Blocks.Find("pendant", Context.Info.Id) != null ? "defined" : "NOT defined")}; modelcook.chains2 {(held ? "held" : "open")}, {warned} warning)");
        Log.Info($"{Tag} done: {_pass} PASS, {_fail} FAIL");
    }

    private void Check(bool ok, string what)
    {
        if (ok) _pass++; else _fail++;
        Log.Info($"{(ok ? "PASS" : "FAIL")} {Tag} {what}");
    }

    private static string Sha(byte[] b) => Convert.ToHexStringLower(SHA256.HashData(b));

    private static string HashAt(byte[] bin, int at) =>
        $"{BitConverter.ToUInt32(bin, at)}-{BitConverter.ToUInt32(bin, at + 4):x8}-{BitConverter.ToUInt32(bin, at + 8):x8}-{BitConverter.ToUInt32(bin, at + 12):x8}";

    private void Run()
    {
        var cooked = Path.Combine(Context.Info.Directory, "assets", "cooked");
        if (!File.Exists(Path.Combine(cooked, "golden.json")))
        {
            Check(false, "assets/cooked is missing: run build-assets.py before building the test");
            return;
        }
        using var golden = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(cooked, "golden.json")));
        var g = golden.RootElement;
        var vanillaKey = new AssetKey(BronzeSword, AssetKey.ItemInfoType);
        var vanillaBefore = Assets.ReadResource(vanillaKey) is { } vb ? Sha(vb) : null;

        // 1. the cook's manifest, as a mod's boot would load it (copied into the test's data folder: the copy is the test's to load)
        var dir = Path.Combine(Context.DataDirectory, "manifest");
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        Directory.CreateDirectory(Path.Combine(dir, "assets", "cooked"));
        foreach (var f in Directory.GetFiles(cooked))
            File.Copy(f, Path.Combine(dir, "assets", "cooked", Path.GetFileName(f)));
        File.Move(Path.Combine(dir, "assets", "cooked", "manifest.json"), Path.Combine(dir, "assets", "assets.json"));
        var report = Assets.LoadManifest(dir);
        Check(report.Contains("errors 0", StringComparison.Ordinal) && report.Contains("(4 entries for clients only, skipped on this server)", StringComparison.Ordinal)
              && report.Contains("resources 1", StringComparison.Ordinal), $"manifest-zero-errors: {report}");
        var skeletonGuid = Guid.Parse(System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(cooked, "pendant.skeleton.json"))).RootElement.GetProperty("guid").GetString()!);
        var skeletonKey = new AssetKey(skeletonGuid, "keen::ModelHierarchyResource", 0);
        var skeletonFile = File.ReadAllBytes(Path.Combine(cooked, "pendant.skeleton.bin"));
        Check(Assets.ReadResource(skeletonKey) is { } sk && sk.AsSpan().SequenceEqual(skeletonFile),
              $"key {skeletonGuid}:keen::ModelHierarchyResource sha256 {Sha(skeletonFile)} (the pendant's skeleton, from the manifest)");

        // 2. each model and its render data, registered as kiln.assets serves them to a client, read back
        var keys = new List<AssetKey>();
        var blobs = new List<ContentHash>();
        var outputs = g.GetProperty("outputs");
        foreach (var (stem, guid) in new[] { ("banner", g.GetProperty("banner").GetProperty("guid").GetString()!), ("models_pendant", g.GetProperty("pendant").GetProperty("guid").GetString()!) })
        {
            var bin = File.ReadAllBytes(Path.Combine(cooked, $"{stem}.bin"));
            var blob = File.ReadAllBytes(Path.Combine(cooked, $"{stem}.blob"));
            var hash = Assets.AddBlob(blob);
            blobs.Add(hash);
            var key = new AssetKey(Guid.Parse(guid), "keen::RenderModel", 0);
            Assets.Register(key, bin, mustBeNew: true);
            keys.Add(key);
            var back = Assets.ReadResource(key);
            var blobBack = Assets.ReadBlob(hash);
            Check(back != null && back.AsSpan().SequenceEqual(bin) && Sha(bin) == outputs.GetProperty($"cooked/{stem}.bin").GetString(),
                  $"key {guid}:keen::RenderModel sha256 {Sha(bin)} reads back as cooked");
            Check(blobBack != null && blobBack.AsSpan().SequenceEqual(blob) && Sha(blob) == outputs.GetProperty($"cooked/{stem}.blob").GetString()
                  && HashAt(bin, 0x110) == hash.ToString(),
                  $"blob {hash} sha256 {Sha(blob)} reads back, and the model's renderData names it");
        }

        // 3. the arrays, read by the game's own reflection of keen::RenderModel
        var banner = Assets.Reflect(keys[0]);
        var pendant = Assets.Reflect(keys[1]);
        var gb = g.GetProperty("banner");
        var gp = g.GetProperty("pendant");
        var problems = new List<string>();
        void Same(ReflectedResource? r, string path, System.Text.Json.JsonElement want, bool floats)
        {
            if (r == null) { problems.Add($"{path}: no reflection"); return; }
            var n = r.Count(path);
            var expect = want.EnumerateArray().ToList();
            if (n != expect.Count) { problems.Add($"{path}: {n} elements, {expect.Count} expected"); return; }
            for (var i = 0; i < n; i++)
            {
                var got = Convert.ToDouble(r[$"{path}[{i}]"], System.Globalization.CultureInfo.InvariantCulture);
                var exp = expect[i].GetDouble();
                if (floats ? (float)got != (float)exp : got != exp) problems.Add($"{path}[{i}] = {got}, {exp} expected");
            }
        }
        const string fabric = "cloth.meshes[0].data.fabric";
        Same(banner, $"{fabric}.anchors", gb.GetProperty("anchors"), false);
        Same(banner, $"{fabric}.sets", gb.GetProperty("sets"), false);
        Same(banner, $"{fabric}.indices", gb.GetProperty("indices"), false);
        Same(banner, $"{fabric}.tetherLengths", gb.GetProperty("tetherLengths"), true);
        Same(banner, $"{fabric}.restValues", gb.GetProperty("restValues"), true);
        Same(banner, "cloth.meshes[0].data.simulationInvMasses", gb.GetProperty("inverseMasses"), true);
        Same(banner, "cloth.meshes[0].data.skinnedVertexIndices", gb.GetProperty("skinnedVertexIndices"), false);
        Same(banner, "cloth.meshes[0].data.renderSimulationVertexIndices", gb.GetProperty("renderSimulationVertexIndices"), false);
        if (banner != null && Convert.ToInt64(banner["meshes[0].clothMeshIndex"]) != 1) problems.Add("meshes[0].clothMeshIndex is not 1");
        Same(pendant, "chain.joints", gp.GetProperty("joints"), false);
        if (pendant != null)
        {
            if (Convert.ToInt64(pendant["chain.chains2[0].config.count"]) != gp.GetProperty("count").GetInt64()) problems.Add("chain count");
            if (pendant.Count("chain.chains2[0].config.restPosition") != gp.GetProperty("count").GetInt32() - 1) problems.Add("chain restPosition count");
            if (Convert.ToInt64(pendant["chain.chains2[0].config.extentRestPosition"]) != gp.GetProperty("extentRestPosition").GetInt64()) problems.Add("extentRestPosition");
            if (Convert.ToInt64(pendant["chain.chains2[0].config.extentMaxAllowedDist"]) != gp.GetProperty("extentMaxAllowedDist").GetInt64()) problems.Add("extentMaxAllowedDist");
            if (pendant["chain.chains2[0].guid"]?.ToString() != gp.GetProperty("chainGuid").GetString()) problems.Add($"chain guid {pendant["chain.chains2[0].guid"]}");
        }
        Check(problems.Count == 0, $"arrays-match-golden ({(problems.Count == 0 ? "banner fabric, masses, maps; pendant chain" : string.Join("; ", problems.Take(6)))})");

        // 4. the cook without the key
        using var off = System.Text.Json.JsonDocument.Parse(File.ReadAllText(Path.Combine(cooked, "physics-off.json")));
        var o = off.RootElement;
        Check(o.GetProperty("refused").GetBoolean() && o.GetProperty("files").GetArrayLength() == 0
              && o.GetProperty("message").GetString()!.Contains("needs cook feature modelcook.cloth", StringComparison.Ordinal),
              $"physics-off-build-refused (exit {o.GetProperty("exit").GetInt32()}, {o.GetProperty("files").GetArrayLength()} files)");

        // 6. cleanup
        foreach (var k in keys) Assets.RemoveResource(k);
        foreach (var h in blobs) Assets.RemoveBlob(h);
        Assets.RemoveResource(skeletonKey);
        var gone = keys.All(k => Assets.ReadResource(k) == null) && blobs.All(h => Assets.ReadBlob(h) == null) && Assets.ReadResource(skeletonKey) == null;
        var vanillaAfter = Assets.ReadResource(vanillaKey) is { } va ? Sha(va) : null;
        Check(gone && vanillaBefore != null && vanillaBefore == vanillaAfter, $"cleanup (the test's keys and blobs gone; {BronzeSword}:keen::ItemInfo sha256 {vanillaAfter} as before)");
    }
}
