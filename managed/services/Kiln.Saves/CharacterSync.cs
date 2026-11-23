// SPDX-License-Identifier: MIT
// Carries CharacterData.Carried with the character, the way the game carries its character save (docs/re/characters.md):
// the client holds the character, offers it when it joins, and the server sends it back at each world save.
//
// The game's own path can't carry this: the character file has a fixed list of block types, and its player_knowledge drops
// ids the server's static map lacks at every load (a character passing through a server without the mod would lose them). So
// channel "kiln:character" v1 (not required), and a sidecar in the client's kiln/data/characters/, never in the save folder:
// the game's migration scan moves unknown files there, and Steam cloud owns the other set. In a hosted game the local player's
// sidecar is read and written directly.
//
//   server -> client  CharHello    the player is in game: which character, does this server take imports
//   client -> server  CharOffer    the sidecar: revision + mod -> key -> JSON
//   server -> client  CharResult   what happened to the offer
//   server -> client  CharUpdate   changes since what the client has, at a world save (also: the server's copy was newer)
//   client -> server  CharAck      written
//
// The newest revision wins; on a tie the server's copy stays. A vanilla client never answers: its data stays on the server.
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>Server to client, sent when the player is in game: which character this is and whether the server takes carried data.</summary>
/// <param name="Character">The game's character id.</param>
/// <param name="World">The world's id, or null when it is not known.</param>
/// <param name="Imports">The server takes carried data from clients (<c>importProgress</c> in <c>characters.json</c>).</param>
/// <param name="MaxBytes">The largest offer the server takes, in bytes of JSON values.</param>
/// <param name="Revision">The server's revision of this character's carried data.</param>
public sealed record CharHello(Guid Character, string? World, bool Imports, int MaxBytes, long Revision);

/// <summary>Client to server: the character's carried data as the client's sidecar holds it.</summary>
/// <param name="Character">The game's character id.</param>
/// <param name="Revision">The client's revision of the carried data.</param>
/// <param name="Mods">The data, by mod id and then key, as JSON values.</param>
public sealed record CharOffer(Guid Character, long Revision, Dictionary<string, Dictionary<string, JsonElement>> Mods);

/// <summary>Server to client: what happened to an offer.</summary>
/// <param name="Character">The game's character id.</param>
/// <param name="Accepted">The server took the offer.</param>
/// <param name="Reason">Why: <c>taken</c>, or the reason the offer was not taken.</param>
/// <param name="Revision">The server's revision of the carried data after the answer.</param>
/// <param name="Applied">Mod ids loaded on the server whose data came in.</param>
/// <param name="Kept">Mod ids not loaded on the server: their data is stored and passed on, nothing reads it.</param>
/// <param name="Dropped">Entries left out of the offer: a mod id that is not valid, or <c>mod/key</c> of a value that was broken or too big.</param>
public sealed record CharResult(Guid Character, bool Accepted, string Reason, long Revision, List<string> Applied, List<string> Kept, List<string> Dropped);

/// <summary>A null value removes the key.</summary>
/// <param name="Character">The game's character id.</param>
/// <param name="Revision">The revision the client has after applying the changes.</param>
/// <param name="Changes">The changed values by mod id and then key, as JSON; null removes the key.</param>
/// <param name="Final">True for the last update of a session: the character left, or the server is shutting down.</param>
public sealed record CharUpdate(Guid Character, long Revision, Dictionary<string, Dictionary<string, JsonElement?>> Changes, bool Final);

/// <summary>Client to server: an update was written to the client's sidecar.</summary>
/// <param name="Character">The game's character id.</param>
/// <param name="Revision">The revision the client wrote.</param>
public sealed record CharAck(Guid Character, long Revision);

/// <summary>Lab peers (tools/nettest) have no player: they say which character they pretend to be.</summary>
/// <param name="Character">The character the lab peer pretends to be.</param>
/// <param name="Account">The account key it pretends to play under.</param>
/// <param name="Local">True to act as the local player of a hosted game, whose sidecar is read and written directly.</param>
public sealed record CharLabJoin(Guid Character, ulong Account, bool Local = false);

/// <summary>A lab peer (tools/nettest) leaves: its character is no longer in game.</summary>
/// <param name="Character">The character that left.</param>
public sealed record CharLabLeave(Guid Character);

internal static class CharacterSync
{
    internal const string ChannelName = "kiln:character";
    private static NetChannel? _channel;
    private static bool _started;
    private static ulong _ticks;
    private static CharacterSettings _settings = new();
    private static Guid _clientCharacter;
    private static long _imports, _refused, _kept, _dropped, _updates, _updateBytes, _sidecarWrites;
    private static readonly Dictionary<uint, Session> Sessions = new();
    private static readonly Dictionary<uint, Session> LabSessions = new();

    /// <summary>Closed sessions whose character's last storage pair may still commit: the game's leave save (and a closing
    /// hosted world's final save) is written after Kiln saw the player leave. Storage only, never progress.</summary>
    private static readonly List<Session> Leaving = new();
    internal const ulong LeavingSteps = 120 * 25;

    private sealed class Session(CharacterKey key, EntityId entity, string name)
    {
        public readonly CharacterKey Key = key;
        public readonly EntityId Entity = entity;
        public readonly string Name = name;
        public NetPeer? Peer;
        public bool Local, HelloSent, Speaks, Offered;
        public bool ProgressAllowed = true, StorageTrusted = true, StorageJoined;
        public readonly StorageOfferWindow StorageWindow = new(_ticks);
        public string? StorageOffer;

        /// <summary>What the client has (per its last ack), what went out last.</summary>
        public Dictionary<(string Mod, string Key), string> Acked = new();
        public Dictionary<(string Mod, string Key), string>? Sent;
        public long SentRevision;
        public ulong LeavingUntil;
        public override string ToString() => $"{Name} character {Key}";
    }

    public static CharacterSyncStatus Status =>
        new(_started, _channel is not null, _settings.ImportProgress, Sessions.Count + LabSessions.Count, _imports, _refused, _kept, _dropped, _updates, _updateBytes, _sidecarWrites);

    public static void EnsureStarted()
    {
        if (_started) return;
        _started = true;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // the channel and the tick are Kiln's, not the calling mod's
        try
        {
            if (Network.Available)
                _channel = Network.Channel(ChannelName, "1", required: false, maxMessageBytes: 256 * 1024)
                    .RegisterJson<CharHello>(OnHello, NetDirection.ToClient)
                    .RegisterJson<CharOffer>(OnOffer, NetDirection.ToServer)
                    .RegisterJson<CharResult>(OnResult, NetDirection.ToClient)
                    .RegisterJson<CharUpdate>(OnUpdate, NetDirection.ToClient)
                    .RegisterJson<CharAck>(OnAck, NetDirection.ToServer)
                    .RegisterJson<CharLabJoin>(OnLabJoin, NetDirection.ToServer)
                    .RegisterJson<CharLabLeave>(OnLabLeave, NetDirection.ToServer);
            ServiceTick.Add("characters", Tick);
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or NotSupportedException)
        {
            SavesRuntime.Log(LogLevel.Warn, $"Kiln.Saves: channel {ChannelName}: {e.Message}");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    // ---------------------------------------------------------------------------------------------- server side ----
    private static void Tick(in TickInfo tick)
    {
        if (++_ticks % 25 != 0 || !Lifecycle.IsServerRunning || !Players.Available) return;
        var present = new HashSet<uint>();
        foreach (var p in Players.All)
        {
            if (!p.InGame || p.Entity.IsNone) continue;
            present.Add(p.Entity.Value);
            var key = CharacterStore.KeyOf(p);
            if (Sessions.TryGetValue(p.Entity.Value, out var s))
            {
                if (s.Key == key) continue;
                Close(s);
                Sessions.Remove(p.Entity.Value);
            }
            if (key.IsNone) continue;
            CharacterStore.RememberLast(key, CharacterStore.LegacyIds(p));
            if (key.IsAccountOnly) continue;  // no character id on this build: nothing can travel
            _settings = CharacterSettings.Load();
            Leaving.RemoveAll(l => l.Key == key);  // the character is back: its new session carries storage from now on
            s = Sessions[p.Entity.Value] = new Session(key, p.Entity, p.Name)
            { ProgressAllowed = _settings.ImportProgress, StorageTrusted = _settings.Storage?.TrustClient ?? false };
            s.Local = Client.Available && Client.IsHosting && (p.Slot == 0 || Client.LocalPlayer == p.Entity);
            SavesRuntime.Log(LogLevel.Info, $"characters: {s} in game{(s.Local ? " (this machine's player)" : "")}; imports {(_settings.ImportProgress ? "allowed" : "refused here")}");
            if (s.Local) OfferSidecar(s);
        }
        foreach (var gone in Sessions.Keys.Where(e => !present.Contains(e)).ToArray())
        {
            Close(Sessions[gone]);
            Sessions.Remove(gone);
        }
        Leaving.RemoveAll(l => _ticks > l.LeavingUntil);
        foreach (var s in Sessions.Values)
        {
            if (!s.Local)
            {
                var peer = Network.PeerOf(s.Entity);
                var vanilla = peer is { State: NetPeerState.Ready } && !peer.HasChannel(ChannelName);
                if (s.StorageWindow.Poll(_ticks, vanilla) && s.StorageWindow.TimedOut)
                    SavesRuntime.Log(LogLevel.Warn, $"player-storage: {s}: client offer late (15 second timeout); using server candidates, late storage offers ignored this session");
                if (!s.HelloSent && _channel is not null && peer is { State: NetPeerState.Ready } && !vanilla)
                {
                    s.Peer = peer;
                    SendHello(s);
                }
            }
            RestoreStorage(s);
        }
    }

    /// <summary>A hosted game: this machine's player. Client and server are one process, so the sidecar is read directly.</summary>
    private static void OfferSidecar(Session s)
    {
        var file = CharacterSidecar.Load(s.Key.Id);
        var r = Decide(s, file.Revision, file.Mods);
        SavesRuntime.Log(LogLevel.Info, $"characters: {s}: its sidecar (revision {file.Revision}): {r.Reason}");
    }

    private static void SendHello(Session s)
    {
        s.HelloSent = true;
        // Even an untrusted copy may withhold stale server storage, but can never add records here.
        _channel!.SendTo(s.Peer!, new CharHello(s.Key.Id, SavesRuntime.WorldId, s.ProgressAllowed || SavesRuntime.Store.Storage.Native is not null, _settings.MaxImportBytes, CharacterStore.RevisionOf(s.Key)));
    }

    private static void Close(Session s)
    {
        try
        {
            Push(s, final: true);
        }
        catch (Exception e)
        {
            SavesRuntime.Log(LogLevel.Warn, $"characters: closing {s}: {e.Message}");
        }
        if (s.Speaks && s.StorageTrusted && SavesRuntime.Store.Storage.Native is not null)
        {
            Leaving.RemoveAll(l => l.Key == s.Key);
            s.LeavingUntil = _ticks + LeavingSteps;
            Leaving.Add(s);
        }
        SavesRuntime.Log(LogLevel.Info, $"characters: {s} left");
    }

    /// <summary>A closed session's storage pair that committed after it closed (its leave save): sent once, alone. Nothing
    /// else of the character is sent or deleted, since the session no longer follows the character's progress.</summary>
    private static void PushLeavingStorage(Session s)
    {
        if (s.Key.Id == Guid.Empty || SavesRuntime.Store.Storage.Carried(s.Key) is not { } storage) return;
        var at = (Mod: PlayerStorageStore.Mod, Key: PlayerStorageStore.Key);
        if ((s.Sent is not null && s.Sent.TryGetValue(at, out var sent) && sent == storage) || (s.Acked.TryGetValue(at, out var acked) && acked == storage)) return;
        var revision = CharacterStore.RevisionOf(s.Key) + 1;
        CharacterStore.SetRevision(s.Key, revision);
        var changes = new Dictionary<string, Dictionary<string, JsonElement?>>(StringComparer.Ordinal)
        { [at.Mod] = new(StringComparer.Ordinal) { [at.Key] = Parse(storage) } };
        (s.Sent ??= new())[at] = storage;
        _updates++;
        _updateBytes += storage.Length;
        if (s.Local)
        {
            CharacterSidecar.Apply(s.Key.Id, revision, changes, SavesRuntime.WorldId);
            _sidecarWrites++;
            s.Acked[at] = storage;
            SavesRuntime.Log(LogLevel.Info, $"characters: {s}: its leave save's storage written to the sidecar (revision {revision})");
            return;
        }
        if (s.Peer is null || _channel is null) return;
        var delivered = _channel.SendTo(s.Peer, new CharUpdate(s.Key.Id, revision, changes, true));
        SavesRuntime.Log(delivered ? LogLevel.Info : LogLevel.Warn,
            $"characters: {s}: its leave save's storage {(delivered ? "sent after the session closed" : "could not be sent: the player is gone; the server keeps its copy")}");
    }

    /// <summary>A world save: where the game sends each character back. Tick thread.</summary>
    internal static void OnWorldSaving(SaveReason reason)
    {
        if (!_started) return;
        foreach (var s in Sessions.Values.Concat(LabSessions.Values))
        {
            try
            {
                Push(s, final: reason == SaveReason.Shutdown);
            }
            catch (Exception e)
            {
                SavesRuntime.Log(LogLevel.Warn, $"characters: sending {s} back: {e.Message}");
            }
        }
        foreach (var s in Leaving.ToArray())
        {
            try
            {
                PushLeavingStorage(s);
            }
            catch (Exception e)
            {
                SavesRuntime.Log(LogLevel.Warn, $"characters: sending {s}'s leave save storage: {e.Message}");
            }
        }
    }

    /// <summary>A hosted world closed: its save has run. The next world starts over.</summary>
    internal static void OnWorldClosing()
    {
        if (!_started) return;
        foreach (var s in Sessions.Values.Concat(LabSessions.Values)) Close(s);
        Sessions.Clear();
        LabSessions.Clear();
    }

    private static Dictionary<(string Mod, string Key), string> Flat(IReadOnlyDictionary<string, Dictionary<string, string>> mods)
    {
        var d = new Dictionary<(string, string), string>();
        foreach (var (m, keys) in mods)
            foreach (var (k, v) in keys)
                d[(m, k)] = v;
        return d;
    }

    /// <summary>One text per value, whoever wrote it, so a comparison sees real changes only.</summary>
    private static Dictionary<(string Mod, string Key), string> Canon(Dictionary<(string Mod, string Key), string> d)
    {
        foreach (var at in d.Keys.ToArray())
        {
            try
            {
                d[at] = JsonNode.Parse(d[at])?.ToJsonString() ?? "null";
            }
            catch (JsonException)
            {
                d.Remove(at);
            }
        }
        return d;
    }

    private static CharResult Answer(Session s, bool accepted, string reason, List<string>? applied = null, List<string>? kept = null, List<string>? dropped = null) =>
        new(s.Key.Id, accepted, reason, CharacterStore.RevisionOf(s.Key), applied ?? [], kept ?? [], dropped ?? []);

    private static void RestoreStorage(Session s)
    {
        if (s.StorageJoined || !s.StorageWindow.Decided || SavesRuntime.Store.Storage.Native is not { } native) return;
        if (native.WitnessOf(s.Entity) is not { } witness) return;
        try { SavesRuntime.Store.Storage.Join(s.Key, s.Entity, witness, s.StorageOffer, s.StorageTrusted, s.Local, s.Name); }
        catch (StorageRestoreDeferredException) { return; } // the next service step tries again
        catch (Exception e) when (e is InvalidDataException or InvalidOperationException or FormatException or OverflowException
            or JsonException or ArgumentException or KeyNotFoundException)
        {
            // Storage stays unrestored for this session (captures keep the stored copy); the progress offer and the
            // session's updates must go on regardless.
            s.StorageJoined = true;
            SavesRuntime.Log(LogLevel.Error, $"player-storage: {s}: restore failed, storage stays as stored: {e.Message}");
            return;
        }
        s.StorageJoined = true;
        if (SavesRuntime.Store.Storage.Notice(s.Key) is { } notice)
        {
            TellStorageNotice(s.Entity, notice); // Plain server chat also reaches vanilla peers without kiln:character.
            if (s.Local) ShowStorageNotice(notice);
            else if (s.Peer is not null && _channel is not null) _channel.SendTo(s.Peer, Answer(s, false, "player-storage: " + notice));
        }
    }

    /// <summary>Handles what a client (or this machine's sidecar) offers.</summary>
    private static CharResult Decide(Session s, long revision, Dictionary<string, Dictionary<string, string>> offered)
    {
        if (s.StorageWindow.Offer())
        {
            s.StorageOffer = offered.GetValueOrDefault(PlayerStorageStore.Mod)?.GetValueOrDefault(PlayerStorageStore.Key);
            RestoreStorage(s);
        }
        if (!s.ProgressAllowed)
        {
            _refused++;
            s.Speaks = s.StorageTrusted;
            s.Acked = Canon(Flat(offered));
            return Answer(s, false, "this server keeps its own progress (characters.json importProgress false); storage uses its independent trust setting");
        }
        var serverRevision = CharacterStore.RevisionOf(s.Key);
        if (revision <= serverRevision)
        {
            // Our copy stays: the client missed something (a crash, a lost sidecar), or there is nothing new. It gets what it lacks.
            s.Speaks = true;
            s.Acked = Canon(Flat(offered));
            Push(s, final: false);
            return Answer(s, false, $"the server's copy is as new or newer (revision {serverRevision} vs {revision})");
        }
        var clean = Validate(s, offered, out var dropped, out var refusal);
        if (refusal is not null)
        {
            _refused++;
            s.Speaks = false;
            SavesRuntime.Log(LogLevel.Warn, $"characters: {s}: offer refused: {refusal}");
            return Answer(s, false, refusal);
        }
        if (Events.HasSubscribers<CharacterImportingEvent>())
        {
            var view = clean.ToDictionary(kv => kv.Key, kv => (IReadOnlyList<string>)kv.Value.Keys.ToList(), StringComparer.Ordinal);
            var ev = Events.Post(new CharacterImportingEvent(s.Key, s.Entity, revision, serverRevision, view));
            if (ev.Canceled)
            {
                _refused++;
                s.Speaks = false;
                SavesRuntime.Log(LogLevel.Info, $"characters: {s}: offer refused: {ev.Reason}");
                return Answer(s, false, ev.Reason);
            }
        }
        var loaded = Mods.All.Where(m => m.IsLoaded).Select(m => m.Id).ToHashSet(StringComparer.Ordinal);
        var applied = clean.Keys.Where(m => loaded.Contains(m) || m.StartsWith("kiln-", StringComparison.Ordinal)).ToList();
        var kept = clean.Keys.Except(applied).ToList();
        // Storage has its own witness/revision decision, never the progress import rule.
        if (clean.TryGetValue(PlayerStorageStore.Mod, out var itemData))
        {
            itemData.Remove(PlayerStorageStore.Key);
            if (itemData.Count == 0) clean.Remove(PlayerStorageStore.Mod);
        }
        CharacterStore.Replace(s.Key, clean);
        CharacterStore.SetRevision(s.Key, revision);
        s.Speaks = true;
        s.Acked = Canon(Flat(clean));
        s.Sent = null;
        _imports++;
        _kept += kept.Count;
        _dropped += dropped.Count;
        SavesRuntime.Log(LogLevel.Info, $"characters: {s}: took its carried data, revision {serverRevision} -> {revision}: applied [{string.Join(", ", applied)}], " +
                                        $"kept unread [{string.Join(", ", kept)}], dropped [{string.Join(", ", dropped)}]");
        if (Events.HasSubscribers<CharacterImportedEvent>()) Events.Post(new CharacterImportedEvent(s.Key, s.Entity, revision, applied, kept));
        return Answer(s, true, "taken", applied, kept, dropped);
    }

    /// <summary>The offer's checks. A broken value is dropped from what the server takes (the client keeps its file); caps refuse the
    /// whole offer. Values are parsed here only to prove they are JSON; a mod reads them as its own type later.</summary>
    private static Dictionary<string, Dictionary<string, string>> Validate(Session s, Dictionary<string, Dictionary<string, string>> offered, out List<string> dropped, out string? refusal)
    {
        dropped = [];
        refusal = null;
        var clean = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
        long total = 0;
        var keys = 0;
        foreach (var (mod, map) in offered)
        {
            if (!ValidModId(mod))
            {
                dropped.Add(mod);
                continue;
            }
            foreach (var (k, json) in map)
            {
                keys++;
                total += json.Length;
                if (k.Length is 0 or > 200 || json.Length > _settings.MaxValueBytes || !IsJson(json))
                {
                    dropped.Add($"{mod}/{k}");
                    continue;
                }
                if (!clean.TryGetValue(mod, out var c)) clean[mod] = c = new Dictionary<string, string>(StringComparer.Ordinal);
                c[k] = json;
            }
        }
        if (total > _settings.MaxImportBytes) refusal = $"{total} bytes (limit {_settings.MaxImportBytes})";
        else if (keys > _settings.MaxKeys) refusal = $"{keys} keys (limit {_settings.MaxKeys})";
        else if (CharacterStore.CharactersOfAccount(s.Key.Account) >= _settings.MaxCharactersPerAccount && !CharacterStore.Snapshot(s.Key).Any())
            refusal = $"this account already has {_settings.MaxCharactersPerAccount} characters here";
        return clean;
    }

    internal static bool ValidModId(string id) =>
        id.Length is > 0 and <= 128 && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-' or ':');

    private static bool IsJson(string text)
    {
        try
        {
            using var doc = JsonDocument.Parse(text, new JsonDocumentOptions { MaxDepth = 32 });
            return true;
        }
        catch (JsonException)
        {
            return false;
        }
    }

    /// <summary>Sends the client what it lacks, as a new revision. Nothing is sent when nothing changed.</summary>
    private static void Push(Session s, bool final)
    {
        if (!s.Speaks || s.Key.Id == Guid.Empty) return;
        var snap = s.ProgressAllowed ? Canon(Flat(CharacterStore.Snapshot(s.Key))) : new Dictionary<(string Mod, string Key), string>();
        var storageAt = (Mod: PlayerStorageStore.Mod, Key: PlayerStorageStore.Key);
        snap.Remove(storageAt);
        var storage = s.StorageTrusted ? SavesRuntime.Store.Storage.Carried(s.Key) : null;
        if (storage is not null) snap[storageAt] = storage;
        bool MayDelete((string Mod, string Key) at) => at == storageAt ? storage is not null : s.ProgressAllowed;
        var changes = new Dictionary<string, Dictionary<string, JsonElement?>>(StringComparer.Ordinal);
        long bytes = 0;
        void Add((string Mod, string Key) at, JsonElement? v)
        {
            if (!changes.TryGetValue(at.Mod, out var m)) changes[at.Mod] = m = new Dictionary<string, JsonElement?>(StringComparer.Ordinal);
            m[at.Key] = v;
        }
        foreach (var (at, json) in snap)
        {
            if (s.Acked.TryGetValue(at, out var old) && old == json) continue;
            bytes += json.Length;
            Add(at, Parse(json));
        }
        foreach (var at in s.Acked.Keys)
            if (!snap.ContainsKey(at) && MayDelete(at)) Add(at, null);
        if (changes.Count == 0) return;
        if (bytes > _settings.MaxImportBytes)
        {
            SavesRuntime.Log(LogLevel.Error, $"characters: {s}: {bytes} bytes of changes exceed the limit {_settings.MaxImportBytes}; they stay on this server");
            return;
        }
        var revision = s.SentRevision;
        if (s.Sent is null || !Same(s.Sent, snap))
        {
            revision = CharacterStore.RevisionOf(s.Key) + 1;
            CharacterStore.SetRevision(s.Key, revision);
        }
        s.Sent = snap;
        s.SentRevision = revision;
        _updates++;
        _updateBytes += bytes;
        if (s.Local)
        {
            CharacterSidecar.Apply(s.Key.Id, revision, changes, SavesRuntime.WorldId);
            _sidecarWrites++;
            s.Acked = snap;
            SavesRuntime.Log(LogLevel.Debug, $"characters: {s}: sidecar at revision {revision}");
            return;
        }
        if (s.Peer is null || _channel is null) return;
        var sent = _channel.SendTo(s.Peer, new CharUpdate(s.Key.Id, revision, changes, final));
        SavesRuntime.Log(LogLevel.Debug, $"characters: {s}: update revision {revision}, {changes.Sum(c => c.Value.Count)} key(s), {bytes} bytes{(final ? ", final" : "")}{(sent ? "" : " (not sent)")}");
    }

    private static bool Same(Dictionary<(string, string), string> a, Dictionary<(string, string), string> b) =>
        a.Count == b.Count && a.All(kv => b.TryGetValue(kv.Key, out var v) && v == kv.Value);

    private static JsonElement Parse(string json)
    {
        using var doc = JsonDocument.Parse(json);
        return doc.RootElement.Clone();
    }

    private static Session? SessionOf(NetContext ctx)
    {
        if (ctx.Sender.IsLab && ctx.Player.IsNone) return LabSessions.GetValueOrDefault(ctx.Sender.Id);
        return ctx.Player.IsNone ? null : Sessions.GetValueOrDefault(ctx.Player.Value);
    }

    private static void OnOffer(CharOffer m, NetContext ctx)
    {
        if (SessionOf(ctx) is not { } s) return;
        if (m.Character != s.Key.Id)
        {
            _refused++;
            SavesRuntime.Log(LogLevel.Warn, $"characters: {s}: an offer for another character ({m.Character}) was refused");
            ctx.Reply(Answer(s, false, "that is not the character you joined with"));
            return;
        }
        if (s.Offered) return;  // one per session: the character came in once, it doesn't change under a running game
        s.Offered = true;
        s.Peer ??= ctx.Sender;
        var offered = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
        foreach (var (mod, keys) in m.Mods ?? [])
        {
            var d = offered[mod] = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var (k, v) in keys ?? []) d[k] = v.GetRawText();
        }
        ctx.Reply(Decide(s, m.Revision, offered));
    }

    private static void OnAck(CharAck m, NetContext ctx)
    {
        if (SessionOf(ctx) is not { } s || m.Character != s.Key.Id || s.Sent is null) return;
        if (m.Revision == s.SentRevision) s.Acked = s.Sent;
    }

    private static void OnLabJoin(CharLabJoin m, NetContext ctx)
    {
        if (!ctx.Sender.IsLab || _channel is null) return;
        var key = new CharacterKey(m.Account, m.Character);
        if (LabSessions.TryGetValue(ctx.Sender.Id, out var old)) Close(old);
        _settings = CharacterSettings.Load();
        var s = LabSessions[ctx.Sender.Id] = new Session(key, EntityId.None, "lab:" + ctx.Sender.Name)
        { Peer = ctx.Sender, Local = m.Local, ProgressAllowed = _settings.ImportProgress, StorageTrusted = _settings.Storage?.TrustClient ?? false };
        SavesRuntime.Log(LogLevel.Info, $"characters: {s} in game (lab peer{(m.Local ? ", as a hosted game's local player" : "")}); imports {(_settings.ImportProgress ? "allowed" : "refused here")}");
        if (m.Local) OfferSidecar(s);
        else SendHello(s);
    }

    private static void OnLabLeave(CharLabLeave m, NetContext ctx)
    {
        if (!ctx.Sender.IsLab || !LabSessions.Remove(ctx.Sender.Id, out var s)) return;
        Close(s);
    }

    // ---------------------------------------------------------------------------------------------- client side ----
    private static void OnHello(CharHello m, NetContext ctx)
    {
        _clientCharacter = m.Character;
        if (!m.Imports)
        {
            SavesRuntime.Log(LogLevel.Info, $"characters: this server keeps its own copy of character {m.Character}'s progress; your sidecar stays as it is");
            return;
        }
        var file = CharacterSidecar.Load(m.Character);
        var mods = new Dictionary<string, Dictionary<string, JsonElement>>(StringComparer.Ordinal);
        foreach (var (mod, keys) in file.Mods)
            mods[mod] = keys.ToDictionary(kv => kv.Key, kv => Parse(kv.Value), StringComparer.Ordinal);
        var size = file.Mods.Sum(x => x.Value.Sum(y => (long)y.Value.Length));
        if (size > m.MaxBytes)
        {
            // No answer: the server treats this client like a vanilla one and leaves the sidecar alone.
            SavesRuntime.Log(LogLevel.Warn, $"characters: character {m.Character}: the sidecar has {size} bytes, this server takes {m.MaxBytes}: not offered");
            return;
        }
        SavesRuntime.Log(LogLevel.Info, $"characters: joined as character {m.Character}: offering revision {file.Revision}, {mods.Count} mod(s) (the server has revision {m.Revision})");
        ctx.Reply(new CharOffer(m.Character, file.Revision, mods));
    }

    private static void OnResult(CharResult m, NetContext ctx)
    {
        if (m.Reason.StartsWith("player-storage: ", StringComparison.Ordinal))
        {
            ShowStorageNotice(m.Reason["player-storage: ".Length..]);
            return;
        }
        SavesRuntime.Log(m.Accepted ? LogLevel.Info : LogLevel.Debug, $"characters: the server {(m.Accepted ? "took" : "did not take")} the offer: {m.Reason} (revision {m.Revision}" +
            $"{(m.Kept.Count > 0 ? $", kept unread: {string.Join(", ", m.Kept)}" : "")}{(m.Dropped.Count > 0 ? $", dropped: {string.Join(", ", m.Dropped)}" : "")})");
    }

    private static void TellStorageNotice(EntityId player, string text)
    {
        SavesRuntime.Log(LogLevel.Warn, $"player-storage: player {player.Value}: {text}");
        try
        {
            var chat = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "Kiln.Chat")?.GetType("Kiln.Chat");
            var tell = chat?.GetMethod("Tell", [typeof(EntityId), typeof(string)]);
            if (tell?.Invoke(null, [player, text]) is not true)
                SavesRuntime.Log(LogLevel.Warn, $"player-storage: chat notice for player {player.Value} unavailable; notice retained in kiln.log");
        }
        catch (Exception e) when (e is System.Reflection.TargetInvocationException or ArgumentException or TypeLoadException)
        {
            SavesRuntime.Log(LogLevel.Warn, "player-storage: chat notice unavailable: " + e.Message);
        }
    }

    private static void ShowStorageNotice(string text)
    {
        SavesRuntime.Log(LogLevel.Warn, "player-storage: " + text);
        // As with /instances' optional chat binding, persistence must not pull the UI into a server process.
        try
        {
            var assembly = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "Kiln.Ui");
            var ui = assembly?.GetType("Kiln.Ui");
            var kind = assembly?.GetType("Kiln.ToastKind");
            if (ui is null || kind is null) return;
            ui.GetMethod("Toast", [typeof(string), kind, typeof(double)])?.Invoke(null, [text, Enum.Parse(kind, "Warning"), 10d]);
        }
        catch (Exception e) when (e is System.Reflection.TargetInvocationException or ArgumentException or TypeLoadException)
        {
            SavesRuntime.Log(LogLevel.Warn, "player-storage: in-game notice unavailable: " + e.Message);
        }
    }

    private static void OnUpdate(CharUpdate m, NetContext ctx)
    {
        if (m.Character != _clientCharacter || m.Changes is null) return;
        CharacterSidecar.Apply(m.Character, m.Revision, m.Changes, null);
        _sidecarWrites++;
        ctx.Reply(new CharAck(m.Character, m.Revision));
    }
}

/// <summary>The client's copy of what a character carries: <c>kiln/data/characters/&lt;id&gt;.json</c> (with one older copy,
/// <c>.prev</c>). Not in the game's save folder: the game's migration scan moves unknown files there, and Steam cloud owns the
/// other set of saves. A copy of a character made with the game's copy function has another id, so it starts without this.</summary>
internal static class CharacterSidecar
{
    internal sealed record Contents(long Revision, Dictionary<string, Dictionary<string, string>> Mods);

    private static string Dir => Path.Combine(KilnPaths.Root, "data", "characters");

    internal static string PathOf(Guid id) => Path.Combine(Dir, $"{id:N}.json");

    public static Contents Load(Guid id)
    {
        var path = PathOf(id);
        foreach (var p in new[] { path, path + ".prev" })
        {
            try
            {
                if (!File.Exists(p)) continue;
                if (JsonNode.Parse(File.ReadAllBytes(p)) is not JsonObject doc) continue;
                var mods = new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal);
                if (doc["mods"] is JsonObject ms)
                    foreach (var (mod, keys) in ms)
                    {
                        if (keys is not JsonObject km) continue;
                        var d = mods[mod] = new Dictionary<string, string>(StringComparer.Ordinal);
                        foreach (var (k, v) in km)
                            if (v is not null) d[k] = v.ToJsonString();
                    }
                return new Contents(doc["revision"]?.GetValue<long>() ?? 0, mods);
            }
            catch (Exception e) when (e is IOException or JsonException or InvalidOperationException or FormatException or UnauthorizedAccessException)
            {
                SavesRuntime.Log(LogLevel.Warn, $"characters: {Path.GetFileName(p)} is unusable: {e.Message}");
            }
        }
        return new Contents(0, new Dictionary<string, Dictionary<string, string>>(StringComparer.Ordinal));
    }

    private static bool Readable(string path)
    {
        try
        {
            if (!File.Exists(path) || JsonNode.Parse(File.ReadAllBytes(path)) is not JsonObject doc) return false;
            if (doc["revision"] is { } revision && revision.GetValue<long>() < 0) return false;
            if (doc["mods"] is not null && doc["mods"] is not JsonObject) return false;
            return doc["mods"] is not JsonObject mods || mods.All(m => m.Value is JsonObject);
        }
        catch (Exception e) when (e is IOException or JsonException or UnauthorizedAccessException or InvalidOperationException or FormatException or OverflowException)
        {
            return false;
        }
    }

    private static bool FutureStorage(string json)
    {
        try { return JsonNode.Parse(json)?["v"]?.GetValue<int>() > 1; }
        catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException) { return false; }
    }

    /// <summary>Merges changes (keys of mods this update doesn't name stay) and writes the file atomically.</summary>
    public static void Apply(Guid id, long revision, Dictionary<string, Dictionary<string, JsonElement?>> changes, string? world)
    {
        var path = PathOf(id);
        var now = Load(id);
        var storageReadOnly = now.Mods.TryGetValue(PlayerStorageStore.Mod, out var itemKeys) && itemKeys.TryGetValue(PlayerStorageStore.Key, out var storageJson)
            && FutureStorage(storageJson);
        foreach (var (mod, keys) in changes)
        {
            if (!CharacterSync.ValidModId(mod)) continue;
            if (!now.Mods.TryGetValue(mod, out var d)) now.Mods[mod] = d = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var (k, v) in keys)
            {
                if (storageReadOnly && mod == PlayerStorageStore.Mod && k == PlayerStorageStore.Key) continue;
                if (v is { ValueKind: not JsonValueKind.Null } e) d[k] = e.GetRawText();
                else d.Remove(k);
            }
            if (d.Count == 0) now.Mods.Remove(mod);
        }
        var doc = new JsonObject
        {
            ["format"] = 1,
            ["character"] = id.ToString(),
            ["revision"] = revision,
            ["updatedUnixMs"] = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
        };
        if (world is not null) doc["world"] = world;
        var mods = new JsonObject();
        foreach (var (mod, keys) in now.Mods)
        {
            var o = new JsonObject();
            foreach (var (k, v) in keys) o[k] = JsonNode.Parse(v);
            mods[mod] = o;
        }
        doc["mods"] = mods;
        Directory.CreateDirectory(Dir);
        if (Readable(path)) File.Copy(path, path + ".prev", overwrite: true);  // never a broken file over the good older copy
        else if (File.Exists(path))
        {
            // If preservation fails, abort the replacement: the only copy of the damaged bytes must not disappear.
            var archive = path + ".corrupt-" + DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() + "-" + Guid.NewGuid().ToString("N");
            WorldStore.WriteAtomic(archive, File.ReadAllBytes(path));
        }
        WorldStore.WriteAtomic(path, System.Text.Encoding.UTF8.GetBytes(doc.ToJsonString()));
    }
}
