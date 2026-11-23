// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

[Collection("instance data")]
public sealed class PlayerStorageSidecarTests : IDisposable
{
    private readonly string _root = Path.Combine(AppContext.BaseDirectory, "storage-sidecars", Guid.NewGuid().ToString("N"));
    private readonly string _oldRoot = KilnPaths.Root;
    private readonly string _oldConfig = KilnPaths.Config;
    public PlayerStorageSidecarTests()
    {
        typeof(KilnPaths).GetProperty("Root")!.SetValue(null, _root);
        typeof(KilnPaths).GetProperty("Config")!.SetValue(null, Path.Combine(_root, "config"));
    }
    public void Dispose()
    {
        typeof(KilnPaths).GetProperty("Root")!.SetValue(null, _oldRoot);
        typeof(KilnPaths).GetProperty("Config")!.SetValue(null, _oldConfig);
        if (Directory.Exists(_root)) Directory.Delete(_root, true);
    }

    [Fact]
    public void Storage_setting_defaults_true_is_independent_and_unreadable_config_refuses_both()
    {
        Assert.True(CharacterSettings.Load().Storage.TrustClient);
        Directory.CreateDirectory(KilnPaths.Config);
        var path = Path.Combine(KilnPaths.Config, "characters.json");
        File.WriteAllText(path, "{\"importProgress\":false,\"storage\":{\"trustClient\":true}}");
        Assert.False(CharacterSettings.Load().ImportProgress);
        Assert.True(CharacterSettings.Load().Storage.TrustClient);
        File.WriteAllText(path, "{broken");
        Assert.False(CharacterSettings.Load().ImportProgress);
        Assert.False(CharacterSettings.Load().Storage.TrustClient);
    }

    private static Dictionary<string, Dictionary<string, JsonElement?>> Change(string storage) => new()
    { ["kiln-items"] = new() { ["storage"] = JsonSerializer.Deserialize<JsonElement>(storage) } };

    [Fact]
    public void Unparseable_file_is_archived_with_exact_bytes_before_replacement()
    {
        var id = PlayerStorageDocumentTests.Character;
        var path = CharacterSidecar.PathOf(id);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var broken = Encoding.UTF8.GetBytes("{broken");
        File.WriteAllBytes(path, broken);
        CharacterSidecar.Apply(id, 1, Change(PlayerStorageDocumentTests.Doc("a", 1).Write(8)), "test");
        var archive = Assert.Single(Directory.EnumerateFiles(Path.GetDirectoryName(path)!, "*.corrupt-*"));
        Assert.Equal(broken, File.ReadAllBytes(archive));
        Assert.NotNull(JsonNode.Parse(File.ReadAllText(path)));
    }

    [Fact]
    public void Future_storage_value_survives_even_if_server_sends_a_replacement()
    {
        var id = PlayerStorageDocumentTests.Character;
        CharacterSidecar.Apply(id, 1, Change("{\"v\":99,\"future\":[1,2]}"), null);
        CharacterSidecar.Apply(id, 2, Change(PlayerStorageDocumentTests.Doc("a", 1).Write(8)), null);
        var value = CharacterSidecar.Load(id).Mods["kiln-items"]["storage"];
        Assert.Equal(99, JsonNode.Parse(value)!["v"]!.GetValue<int>());
    }

    [Fact]
    public void Good_previous_file_is_kept_and_unknown_mod_keys_ride_updates()
    {
        var id = PlayerStorageDocumentTests.Character;
        var changes = Change(PlayerStorageDocumentTests.Doc("a", 1).Write(8));
        changes["missing-mod"] = new() { ["key"] = JsonSerializer.Deserialize<JsonElement>("{\"untouched\":true}") };
        CharacterSidecar.Apply(id, 1, changes, null);
        var before = File.ReadAllBytes(CharacterSidecar.PathOf(id));
        var forced = WorldStore.FilesForced;
        CharacterSidecar.Apply(id, 2, Change(PlayerStorageDocumentTests.Doc("b", 2).Write(8)), null);
        Assert.Equal(before, File.ReadAllBytes(CharacterSidecar.PathOf(id) + ".prev"));
        Assert.Contains("untouched", CharacterSidecar.Load(id).Mods["missing-mod"]["key"]);
        Assert.True(WorldStore.FilesForced > forced);
    }
}
