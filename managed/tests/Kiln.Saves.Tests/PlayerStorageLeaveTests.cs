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
