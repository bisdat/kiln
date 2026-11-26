// SPDX-License-Identifier: MIT
// What joining players need follows a `reload` at once: the required-mods recompute is keyed on each mod's id, version, state
// and folder, so a reload that changed only "clients" in kiln.mod.json (its version kept) went unseen for up to a minute, and
// a vanilla client could still join a server whose reloaded mod now needs Kiln on the client.
using System.Reflection;
using Xunit;
namespace Kiln.Network.Tests;

[Collection(nameof(SteamGuardRelayTests))]  // ModsBackend and NetAdvertise's statics: never at once with the engine tests
public sealed class AdvertiseSignatureTests
{
    [Fact]
    public void A_rewritten_manifest_changes_the_signature_with_the_version_kept()
    {
        var dir = Path.Combine(Path.GetTempPath(), "kiln-advert-sig-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        try
        {
            var manifest = Path.Combine(dir, "kiln.mod.json");
            File.WriteAllText(manifest, """{"id": "bigpack", "version": "1.0.0", "clients": "none"}""");
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 0, DateTimeKind.Utc));
            (string, string, ModState, string)[] mods = [("bigpack", "1.0.0", ModState.Loaded, dir)];
            var before = NetAdvertise.Signature(mods);
            Assert.Equal(before, NetAdvertise.Signature(mods));  // nothing changed: no recompute
            File.WriteAllText(manifest, """{"id": "bigpack", "version": "1.0.0", "clients": "kiln"}""");  // a reload's new manifest
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 5, DateTimeKind.Utc));
            Assert.NotEqual(before, NetAdvertise.Signature(mods));
            Assert.Equal(NetAdvertise.Signature([("x", "1", ModState.Loaded, "")]), NetAdvertise.Signature([("x", "1", ModState.Loaded, "")]));  // no folder
        }
        finally { Directory.Delete(dir, recursive: true); }
    }

    // The server's once-a-second Tick, as a reload leaves it: the mod list the same (id, version, state, folder), only the
    // manifest's "clients" rewritten. What the vanilla gate reads (KilnClientMods) has the mod at the next Tick, not up to a
    // minute later at the full recompute.
    [Fact]
    public void A_reload_that_changes_only_clients_reaches_the_vanilla_gate_at_the_next_tick()
    {
        var dir = Path.Combine(Path.GetTempPath(), "kiln-advert-tick-" + Guid.NewGuid());
        Directory.CreateDirectory(dir);
        var manifest = Path.Combine(dir, "kiln.mod.json");
        try
        {
            File.WriteAllText(manifest, """{"id": "bigpack-tick", "version": "1.0.0", "side": "both", "clients": "none"}""");
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 0, DateTimeKind.Utc));
            ModsBackend.Current = new FakeMods(new ModEntry("bigpack-tick", "Big Pack", "1.0.0", dir, ModSide.Both, ModState.Loaded, null, [], 0, null));
            NetAdvertise.Tick();
            Settle();
            Assert.DoesNotContain("bigpack-tick", NetAdvertise.KilnClientMods);  // "clients": "none": vanilla clients may join
            File.WriteAllText(manifest, """{"id": "bigpack-tick", "version": "1.0.0", "side": "both", "clients": "kiln"}""");  // the reload's manifest, version kept
            File.SetLastWriteTimeUtc(manifest, new DateTime(2026, 11, 25, 12, 0, 5, DateTimeKind.Utc));
            NetAdvertise.Tick();  // the next second's Tick, well within the minute of the full recompute
            Settle();
            Assert.Contains("bigpack-tick", NetAdvertise.KilnClientMods);  // vanilla clients are refused from now on
        }
        finally
        {
            ModsBackend.Current = null;
            Settle();
            Directory.Delete(dir, recursive: true);
        }
    }

    // A recompute runs on the thread pool (NetAdvertise.Refresh): wait until none runs.
    private static void Settle()
    {
        var computing = typeof(NetAdvertise).GetField("_computing", BindingFlags.NonPublic | BindingFlags.Static)!;
        var until = DateTime.UtcNow.AddSeconds(10);
        while ((int)computing.GetValue(null)! != 0 && DateTime.UtcNow < until) Thread.Sleep(5);
        Assert.Equal(0, (int)computing.GetValue(null)!);
    }

    private sealed class FakeMods(params ModEntry[] mods) : IModsBackend
    {
        public IReadOnlyList<ModEntry> All => mods;
        public ModSide Side => ModSide.Server;
        public ModEntry? Get(string id) => mods.FirstOrDefault(m => m.Id == id);
        public ModStats Stats(string id) => default;
        public bool Send(ModContext? sender, string targetId, string method, object? payload) => false;
        public Task<ModOperationResult> Request(ModOperation op, string arg) => throw new NotSupportedException();
        public void Request(ModOperation op, string arg, Action<ModOperationResult> done, ModContext? caller) => throw new NotSupportedException();
    }
}
