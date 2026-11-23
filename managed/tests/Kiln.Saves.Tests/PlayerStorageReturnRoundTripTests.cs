// SPDX-License-Identifier: MIT
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

/// <summary>The offline return end to end: the sidecar exactly as the launcher writes it (tests/fixtures/player-storage,
/// made by the launcher's planner and its Rust disk boundary, both pinned there by their own golden tests), read by
/// Kiln.Saves at a join, for every crash point of the launcher's two writes, every kind of server copy and every client.
/// A returned character never gets the returned stacks back from storage; a character the return never reached keeps
/// all of them (live, or parked by a hold).</summary>
public sealed partial class PlayerStorageStoreTests
{
    private static string Fixtures([CallerFilePath] string source = "")
    {
        var d = Path.GetDirectoryName(source);
        while (d is not null && !File.Exists(Path.Combine(d, "VERSION"))) d = Path.GetDirectoryName(d);
        return Path.Combine(d!, "tests", "fixtures", "player-storage");
    }

    private sealed record Golden(Guid Character, string Before, string After, string OldWitness, string NewWitness, uint[] Returned, uint[] Kept);

    private static Golden Load()
    {
        var request = JsonNode.Parse(File.ReadAllText(Path.Combine(Fixtures(), "return-request.json")))!;
        var before = JsonNode.Parse(request["expectedSidecar"]!.GetValue<string>())!;
        var after = JsonNode.Parse(File.ReadAllText(Path.Combine(Fixtures(), "return-sidecar.json")))!;
        return new(Guid.Parse(before["character"]!.GetValue<string>()),
            before["mods"]!["kiln-items"]!["storage"]!.ToJsonString(), after["mods"]!["kiln-items"]!["storage"]!.ToJsonString(),
            request["oldWitness"]!.GetValue<string>(), request["newWitness"]!.GetValue<string>(),
            request["returnedItems"]!.AsArray().Select(i => i!.GetValue<uint>()).ToArray(),
            request["keptItems"]!.AsArray().Select(i => i!.GetValue<uint>()).ToArray());
    }

    public static TheoryData<string, string, string> ReturnCases()
    {
        var data = new TheoryData<string, string, string>();
        foreach (var crash in new[] { "before-sidecar", "after-sidecar", "after-character" })
            foreach (var server in new[] { "none", "in-sync", "newer-fork", "older" })
                foreach (var client in new[] { "kiln-trusted", "kiln-untrusted", "vanilla" })
                    data.Add(crash, server, client);
        return data;
    }

    private static string ServerCopy(Golden g, string kind)
    {
        var doc = JsonNode.Parse(g.Before)!.AsObject();
        switch (kind)
        {
            case "newer-fork": // the character played here from another PC whose sidecar never saw this return
                doc["revision"] = 1000;
                doc["current"]!["witness"] = "xxh3:00000000000000f0";
                doc["history"] = new JsonArray();
                doc.Remove("returns");
                break;
            case "older":
                doc["revision"] = 5;
                doc["current"]!["witness"] = "xxh3:00000000000000e0";
                doc["current"]!["storages"]!["quiver-mod:quiver"]!["records"]!.AsArray().RemoveAt(2);
                doc["history"] = new JsonArray();
                doc.Remove("returns");
                break;
        }
        return PlayerStorageDocument.Read(doc.ToJsonString(), g.Character).Write(32, PlayerStorageDocument.ServerMaxBytes);
    }

    private static IEnumerable<uint> Items(JsonObject? bags) => (bags ?? []).SelectMany(b => b.Value!["records"]!.AsArray()).Select(r => r!["item"]!.GetValue<uint>());

    [Theory]
    [MemberData(nameof(ReturnCases))]
    public void Launcher_written_return_never_duplicates_and_never_loses_at_a_join(string crash, string server, string client)
    {
        var g = Load();
        var returned = crash == "after-character";                       // the character file holds the returned stacks
        var witness = returned ? g.NewWitness : g.OldWitness;
        var sidecar = crash == "before-sidecar" ? g.Before : g.After;
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        // The launcher adds main pages, so only the returned character shows the extra-main-pages signal.
        var persist = new Persist { Definitions = new() { ["quiver-mod:quiver"] = 18 }, ExtraPages = returned };
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, g.Character);
            if (server != "none")
                InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                    new WorldStore.Slot { Json = ServerCopy(g, server) };
            store.Join(character, new EntityId(1), witness, client == "vanilla" ? null : sidecar, trustClient: client == "kiln-trusted");
            var live = Items(persist.Restored).ToList();
            var all = live.Concat(Items(persist.Parked)).ToList();
            // What this server may use: the player's copy when it is trusted (it pairs), else only its own copy. A copy it
            // does not use stays where it is (an untrusted or vanilla client's sidecar is never written here).
            var usable = client == "kiln-trusted" ? g.Returned.Concat(g.Kept)
                : server == "none" ? [] : Items(JsonNode.Parse(ServerCopy(g, server))!["current"]!["storages"]!.AsObject());
            if (returned)
                Assert.DoesNotContain(live, item => g.Returned.Contains(item)); // never twice: in the character AND in storage
            else
                foreach (var item in usable) Assert.Contains(item, all);       // never lost: live, or parked by a hold
            if (!returned && server is "none" && client != "kiln-trusted") Assert.Empty(all);
        }
        finally { store.Native = null; }
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(false, true)]
    [InlineData(true, false)]
    [InlineData(true, true)]
    public void Launcher_written_return_through_the_offer_holds_a_newer_fork_whatever_the_server_settings(bool progress, bool trusted)
    {
        // The same file through CharacterSync's offer handler: progress imports and storage trust never bring the returned
        // stacks back next to the returned character.
        var g = Load();
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { Definitions = new() { ["quiver-mod:quiver"] = 18 }, JoinWitness = trusted ? "xxh3:00000000000000cc" : g.NewWitness };
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, g.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = ServerCopy(g, "newer-fork") };
            Offer(character, PlayerStorageDocument.Read(g.After, g.Character), progress, trusted);
            Assert.DoesNotContain(Items(persist.Restored), item => g.Returned.Contains(item));
            Assert.Equal("returned outside Kiln", persist.Parked!["quiver-mod:quiver"]!["parkReason"]!.GetValue<string>());
        }
        finally { store.Native = null; }
    }
}
