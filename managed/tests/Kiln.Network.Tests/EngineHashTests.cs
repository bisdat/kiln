// SPDX-License-Identifier: MIT
// The hash a server advertises for another engine's mod stays the same while the mod runs (AdvertiseIdentity.cs,
// inbox 84): Kiln's copy is advertised by its Library entry, whatever the mod writes into its folder and whatever the
// player sets; a folder Kiln did not place by its files without logs and settings.
using System.Security.Cryptography;
using System.Text.Json;
using Xunit;

namespace Kiln.Network.Tests;

public sealed class EngineHashTests : IDisposable
{
    private const string Library = "kdh1:a516c609cfc5672f9ac9484f92f28b0280024635510c7c0630ec9f6b33fc2668";
    private readonly string _game = Path.Combine(Path.GetTempPath(), "kiln-engine-hash-" + Guid.NewGuid().ToString("N"));
    private string Mod => Path.Combine(_game, "mods", "unifiedemltweaks");

    public EngineHashTests()
    {
        Directory.CreateDirectory(Path.Combine(Mod, "bin"));
        Directory.CreateDirectory(Path.Combine(Mod, "src"));
        Directory.CreateDirectory(Path.Combine(_game, "kiln", "state"));
        File.WriteAllText(Path.Combine(Mod, "mod.json"), """{"id":"unifiedemltweaks","name":"Unified EML Tweaks","version":"2.1.1"}""");
        File.WriteAllText(Path.Combine(Mod, "extended.mod.json"), Extended(false));
        File.WriteAllText(Path.Combine(Mod, "native-plugin.ini"), "[Plugin]\nEnabled=1\nDll=bin/ut_native.dll\n");
        File.WriteAllBytes(Path.Combine(Mod, "bin", "ut_native.dll"), [0x4d, 0x5a, 1, 2, 3]);
        File.WriteAllText(Path.Combine(Mod, "src", "mod.lua"), "return {}");
    }

    public void Dispose()
    {
        try { Directory.Delete(_game, true); }
        catch (IOException) { }
    }

    private static string Extended(bool stacks) =>
        "{\"schemaVersion\":1,\"enabled\":true,\"settings\":{\"Enable_StackSize\":{\"label\":\"Bigger stacks\",\"control\":\"toggle\",\"value\":" +
        (stacks ? "true" : "false") + "}}}";

    private static string Sha(string path) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path)));

    /// <summary>The files Kiln placed, as its receipts list them (extended.mod.json by a hash of its own; folders "").</summary>
    private Dictionary<string, string> Placed() => new()
    {
        ["bin/"] = "", ["src/"] = "", ["mod.json"] = Sha(Path.Combine(Mod, "mod.json")),
        ["extended.mod.json"] = "0000000000000000000000000000000000000000000000000000000000000000",
        ["native-plugin.ini"] = Sha(Path.Combine(Mod, "native-plugin.ini")), ["bin/ut_native.dll"] = Sha(Path.Combine(Mod, "bin", "ut_native.dll")),
        ["src/mod.lua"] = Sha(Path.Combine(Mod, "src", "mod.lua")),
    };

    private static object Marker => new { format = 1, kind = "shroudforge", id = "unifiedemltweaks", version = "2.1.1", hash = Library };

    /// <summary>The launcher placed it (activation): the marker, and the receipt naming the folder by the launcher's own
    /// path (a Steam Deck's SD card, where the game sees D:\).</summary>
    private void PlacedByActivation()
    {
        File.WriteAllText(Path.Combine(Mod, ".kiln-library.json"), JsonSerializer.Serialize(Marker));
        File.WriteAllText(Path.Combine(_game, "kiln", "state", "library-placements.json"), JsonSerializer.Serialize(new[]
        {
            new { path = "/run/media/deck/GF8S5/steamapps/common/Enshrouded/mods/unifiedemltweaks", marker = Marker, files = Placed() },
        }));
    }

    private string Hash() => NetAdvertise.EngineHash("shroudforge", Mod, _game);

    /// <summary>What the mod and the player do while the game runs.</summary>
    private void Run()
    {
        File.WriteAllText(Path.Combine(Mod, "bin", "ut_native.log"), "started\n");
        File.WriteAllText(Path.Combine(Mod, "bin", "native-status.txt"), "tick 1\n");
        File.WriteAllText(Path.Combine(Mod, "extended.mod.json"), Extended(true));
        File.AppendAllText(Path.Combine(Mod, "bin", "ut_native.log"), "more\n");
        File.WriteAllText(Path.Combine(Mod, "bin", "native-status.txt"), "tick 2\n");
    }

    [Fact]
    public void Kilns_placed_copy_is_advertised_by_its_library_entry_while_it_runs()
    {
        PlacedByActivation();
        Assert.Equal(Library, Hash());
        Run();
        Assert.Equal(Library, Hash());
        // its code changed: no longer the Library's files, so not advertised as them
        File.WriteAllText(Path.Combine(Mod, "src", "mod.lua"), "return { edited = true }");
        Assert.NotEqual(Library, Hash());
    }

    [Fact]
    public void A_copy_synced_to_a_server_is_advertised_by_its_library_entry()
    {
        var library = new { kind = "shroudforge", id = "unifiedemltweaks", version = "2.1.1", hash = Library };
        File.WriteAllText(Path.Combine(Mod, ".kiln-source.json"), JsonSerializer.Serialize(new { format = 1, library, synced = "now", by = "kiln-launcher" }));
        var receipt = new Dictionary<string, object>
        {
            ["/home/me/servers/isles/mods/unifiedemltweaks"] = new { library, payload = "x", files = Placed() },
        };
        File.WriteAllText(Path.Combine(_game, "kiln", "state", "server-library.json"), JsonSerializer.Serialize(receipt));
        Assert.Equal(Library, Hash());
        Run();
        Assert.Equal(Library, Hash());
        // a receipt from before its file list cannot say the files are still the synced ones
        receipt["/home/me/servers/isles/mods/unifiedemltweaks"] = new { library, payload = "x" };
        File.WriteAllText(Path.Combine(_game, "kiln", "state", "server-library.json"), JsonSerializer.Serialize(receipt));
        Assert.NotEqual(Library, Hash());
    }

    [Fact]
    public void A_marker_without_kilns_receipt_is_not_trusted()
    {
        // a package can carry its own marker; only a receipt outside the folder makes it Kiln's copy
        File.WriteAllText(Path.Combine(Mod, ".kiln-library.json"), JsonSerializer.Serialize(Marker));
        Assert.NotEqual(Library, Hash());
        // a receipt for another folder of the same name elsewhere is not this folder's
        File.WriteAllText(Path.Combine(_game, "kiln", "state", "library-placements.json"), JsonSerializer.Serialize(new[]
        {
            new { path = "/elsewhere/shroudforge-mods/unifiedemltweaks", marker = Marker, files = Placed() },
        }));
        Assert.NotEqual(Library, Hash());
    }

    [Fact]
    public void A_players_own_folder_leaves_out_its_logs_and_settings()
    {
        var before = Hash();
        Assert.StartsWith("kdh1:", before);
        File.WriteAllText(Path.Combine(Mod, "bin", "ut_native.log"), "started\n");
        File.WriteAllText(Path.Combine(Mod, "extended.mod.json"), Extended(true));
        File.AppendAllText(Path.Combine(Mod, "bin", "ut_native.log"), "more\n");
        Assert.Equal(before, Hash());
        // the mod's own files still count
        File.WriteAllText(Path.Combine(Mod, "src", "mod.lua"), "return { edited = true }");
        Assert.NotEqual(before, Hash());
    }

    [Theory]
    [InlineData("eml", "config.txt", false, true)]
    [InlineData("eml", "CONFIG.INI", false, true)]
    [InlineData("eml", "extended.mod.json", false, true)]
    [InlineData("set", "settings.json", false, true)]
    [InlineData("shroudforge", "settings.json", false, false)]
    [InlineData("shroudforge", "sub/extended.mod.json", false, false)]
    [InlineData("eml", "data/table.ini", true, true)]
    [InlineData("eml", "data/table.bin", true, false)]
    [InlineData("eml", "mod.json", true, false)]
    [InlineData("eml", "native-plugin.ini", true, false)]
    [InlineData("eml", "src/data.json", true, false)]
    public void What_may_change_matches_kiln_manage(string kind, string rel, bool gameFolder, bool leftOut) =>
        Assert.Equal(leftOut, NetAdvertise.LeftOut(kind, rel, gameFolder));

    [Fact]
    public void The_advertised_hash_follows_the_folder_and_its_receipts()
    {
        var before = NetAdvertise.EngineSignature(Mod, _game);
        PlacedByActivation();
        Assert.NotEqual(before, NetAdvertise.EngineSignature(Mod, _game));
    }
}
