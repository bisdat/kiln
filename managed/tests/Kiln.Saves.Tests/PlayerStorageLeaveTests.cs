// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

public sealed partial class PlayerStorageStoreTests
{
    private const BindingFlags Hidden = BindingFlags.NonPublic | BindingFlags.Public | BindingFlags.Instance | BindingFlags.Static;

    /// <summary>A CharacterSync session of this machine's player (a hosted game): its sidecar is written directly.</summary>
    private sealed class HostedSession : IDisposable
    {
        private readonly string _root = Path.Combine(AppContext.BaseDirectory, "storage-leave", Guid.NewGuid().ToString("N"));
        private readonly string _oldRoot = KilnPaths.Root;
        private readonly bool _oldStarted;
        internal readonly CharacterKey Character = new(1, PlayerStorageDocumentTests.Character);
        internal readonly object Session;

        internal HostedSession()
        {
            typeof(KilnPaths).GetProperty("Root")!.SetValue(null, _root);
            var started = typeof(CharacterSync).GetField("_started", Hidden)!;
            _oldStarted = (bool)started.GetValue(null)!;
            started.SetValue(null, true);
            var type = typeof(CharacterSync).GetNestedType("Session", BindingFlags.NonPublic)!;
            Session = Activator.CreateInstance(type, Hidden, null, [Character, new EntityId(1), "hosted player"], null)!;
            type.GetField("Local", Hidden)!.SetValue(Session, true);
            type.GetField("Speaks", Hidden)!.SetValue(Session, true);
            Sessions.Clear();
            Sessions[1u] = Session;
        }

        internal static System.Collections.IDictionary Sessions =>
            (System.Collections.IDictionary)typeof(CharacterSync).GetField("Sessions", Hidden)!.GetValue(null)!;

        internal string? SidecarWitness()
        {
            var mods = CharacterSidecar.Load(Character.Id).Mods;
            return mods.TryGetValue("kiln-items", out var keys) && keys.TryGetValue("storage", out var json)
                ? JsonNode.Parse(json)!["current"]!["witness"]!.GetValue<string>() : null;
        }

        public void Dispose()
        {
            Sessions.Clear();
            typeof(CharacterSync).GetField("_started", Hidden)!.SetValue(null, _oldStarted);
            typeof(KilnPaths).GetProperty("Root")!.SetValue(null, _oldRoot);
            if (Directory.Exists(_root)) Directory.Delete(_root, true);
        }
    }

    [Fact]
    public void The_final_save_of_a_closing_hosted_world_reaches_the_players_sidecar()
    {
        // The game client closes its hosted world: Kiln.Saves closes the character sessions first, then drains the
        // world's last events, whose world-written event commits the final save (with the last storage capture).
        using var h = new InstanceHarness();
        using var hosted = new HostedSession();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            store.Join(hosted.Character, new EntityId(1), "join", null, true, local: true);
            h.Save(1);
            CharacterSync.OnWorldSaving(SaveReason.Request);
            Assert.Equal("saved", hosted.SidecarWitness());
            persist.Frozen = new("final", PlayerStorageDocumentTests.Storages(2));
            h.StageOnly(2);                       // the game's shutdown save captured storage with the character
            CharacterSync.OnWorldClosing();       // sessions close before the world's last events are drained
            InstanceHarness.Store.Commit(2, 1002, SaveReason.Shutdown);
            InstanceHarness.Written();
            CharacterSync.OnWorldSaving(SaveReason.Shutdown);
            Assert.Equal("final", hosted.SidecarWitness());
        }
        finally { store.Native = null; }
    }

    [Fact]
    public void A_late_leave_save_reaches_the_player_after_the_session_closed()
    {
        // The player leaves: CharacterSync sees them leaving and closes the session; the game's leave save (and the
        // storage captured with it) commits after that.
        using var h = new InstanceHarness();
        using var hosted = new HostedSession();
        var persist = new Persist();
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            store.Join(hosted.Character, new EntityId(1), "join", null, true, local: true);
            h.Save(1);
            CharacterSync.OnWorldSaving(SaveReason.Request);
            var close = typeof(CharacterSync).GetMethod("Close", Hidden)!;
            close.Invoke(null, [hosted.Session]);
            HostedSession.Sessions.Clear();
            persist.Frozen = new("leave", PlayerStorageDocumentTests.Storages(3));
            h.Save(2);
            CharacterSync.OnWorldSaving(SaveReason.PlayerLeft);
            Assert.Equal("leave", hosted.SidecarWitness());
        }
        finally { store.Native = null; }
    }
}

public sealed partial class PlayerStorageStoreTests
{
    [Fact]
    public void A_late_offer_after_the_join_timeout_never_swaps_live_storage_and_its_pairs_are_kept()
    {
        // The player's offer came after the 15-second window: storage was restored from this server's copy. The late copy
        // must not replace live storage, but this server's next carried update replaces the player's sidecar value, so the
        // late copy's pairs ride on as history instead of disappearing (it may be the only copy, e.g. of a deleted world).
        using var h = new InstanceHarness();
        var persist = new Persist { JoinWitness = "elsewhere" };
        var store = InstanceHarness.Store.Storage;
        store.Native = persist;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("server", 10, slot: 1).Write(32) };
            var type = typeof(CharacterSync).GetNestedType("Session", BindingFlags.NonPublic)!;
            var session = Activator.CreateInstance(type, Hidden, null, [character, new EntityId(1), "late player"], null)!;
            var window = (StorageOfferWindow)type.GetField("StorageWindow", Hidden)!.GetValue(session)!;
            Assert.True(window.Poll(ulong.MaxValue / 2, vanilla: false));
            Assert.True(window.TimedOut);
            typeof(CharacterSync).GetMethod("RestoreStorage", Hidden)!.Invoke(null, [session]);   // server copy only
            var restored = persist.Restored!.ToJsonString();
            var late = PlayerStorageDocumentTests.Doc("elsewhere", 20, slot: 5);                  // pairs with the character
            var offered = new Dictionary<string, Dictionary<string, string>> { ["kiln-items"] = new() { ["storage"] = late.Write(8) } };
            typeof(CharacterSync).GetMethod("Decide", Hidden)!.Invoke(null, [session, 1L, offered]);
            Assert.Equal(restored, persist.Restored!.ToJsonString());                             // no live swap
            h.Save(1);
            var carried = PlayerStorageDocument.Read(store.Carried(character)!, character.Id);
            var kept = carried.Data["history"]!.AsArray().OfType<JsonObject>().Single(p => p["witness"]!.GetValue<string>() == "elsewhere");
            Assert.Equal(5, kept["storages"]!["mod:bag"]!["records"]![0]!["s"]!.GetValue<int>());
            Assert.Contains(store.Read(character)!.Data["history"]!.AsArray().OfType<JsonObject>(), p => p["witness"]!.GetValue<string>() == "elsewhere");
        }
        finally { store.Native = null; }
    }
}
