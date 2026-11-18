// SPDX-License-Identifier: MIT
// The hash a server advertises for another engine's mod (AdvertiseIdentity.cs) against the shared fixture
// schema/fixtures/engine-identity/cases.json: Kiln's placed and synced copies, the same after a run that writes logs and
// changes settings, a changed code file, a forged marker, the player's own folder, odd names and line endings. A Python
// reference (engine_identity.py) wrote each case's hash, and kiln-manage's tests (engine_identity_fixture.rs) read the
// same file: the launcher's modpacks carry what the server advertises.
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Network.Tests;

public sealed class EngineIdentityFixtureTests
{
    private static string Fixture([CallerFilePath] string self = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(self)!, "..", "..", "..", "schema", "fixtures", "engine-identity", "cases.json"));

    private static JsonObject Doc() => JsonNode.Parse(File.ReadAllText(Fixture()))!.AsObject();

    public static TheoryData<string> Cases()
    {
        var data = new TheoryData<string>();
        foreach (var c in Doc()["cases"]!.AsArray()) data.Add(c!["name"]!.GetValue<string>());
        return data;
    }

    /// <summary><paramref name="v"/> with the root placeholder in its strings (keys too) replaced by <paramref name="root"/>.</summary>
    private static JsonNode? Rooted(JsonNode? v, string placeholder, string root) => v switch
    {
        JsonValue s when s.GetValueKind() == JsonValueKind.String => JsonValue.Create(s.GetValue<string>().Replace(placeholder, root)),
        JsonArray a => new JsonArray([.. a.Select(x => Rooted(x, placeholder, root))]),
        JsonObject o => new JsonObject(o.Select(p => KeyValuePair.Create(p.Key.Replace(placeholder, root), Rooted(p.Value, placeholder, root)))),
        _ => v?.DeepClone(),
    };

    [Theory]
    [MemberData(nameof(Cases))]
    public void Each_fixture_tree_is_advertised_with_the_reference_hash(string name)
    {
        var doc = Doc();
        var placeholder = doc["root"]!.GetValue<string>();
        var c = doc["cases"]!.AsArray().Single(x => x!["name"]!.GetValue<string>() == name)!;
        var root = Path.Combine(Path.GetTempPath(), "kiln-engine-identity-" + Guid.NewGuid().ToString("N"));
        try
        {
            var dir = Path.Combine(root, c["folder"]!.GetValue<string>().Replace('/', Path.DirectorySeparatorChar));
            foreach (var (rel, v) in c["files"]!.AsObject())
            {
                var path = Path.Combine(dir, rel.Replace('/', Path.DirectorySeparatorChar));
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                File.WriteAllBytes(path, v!["text"] is { } t ? System.Text.Encoding.UTF8.GetBytes(t.GetValue<string>()) : Convert.FromHexString(v["hex"]!.GetValue<string>()));
            }
            var state = Path.Combine(root, "kiln", "state");
            foreach (var (file, v) in c["state"]!.AsObject())
            {
                Directory.CreateDirectory(state);
                File.WriteAllText(Path.Combine(state, file), Rooted(v, placeholder, root)!.ToJsonString());
            }
            var kind = c["kind"]!.GetValue<string>();
            // listed as the server lists it (AdvertiseEngines.cs): one that does not run is not advertised at all
            var listed = c["listed"]?.GetValue<bool>() ?? true;
            Assert.Equal(listed, NetAdvertise.EngineKindOf(kind, dir) == true);
            if (listed) Assert.Equal(c["expect"]!.GetValue<string>(), NetAdvertise.EngineHash(kind, dir, root));
        }
        finally
        {
            try { Directory.Delete(root, true); }
            catch (IOException) { }
        }
    }

    [Fact]
    public void The_fixture_has_the_cases_both_sides_need()
    {
        var names = Doc()["cases"]!.AsArray().Select(c => c!["name"]!.GetValue<string>()).ToHashSet();
        Assert.True(names.Count >= 30, $"{names.Count} cases");
        foreach (var n in new[] { "sf-placed", "sf-placed-run", "sf-placed-lua-changed", "sf-placed-dll-updated", "sf-forged-marker", "sf-own",
                     "sf-own-run", "sf-placed-odd-names", "sf-own-odd-names", "sf-synced-run", "eml-game-folder-copy" })
            Assert.Contains(n, names);
    }
}
