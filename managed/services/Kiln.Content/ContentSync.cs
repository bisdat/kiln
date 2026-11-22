// SPDX-License-Identifier: MIT
// Server -> client content sync (Forge: registry sync at login, server data packs on the client). A Kiln server sends
// its effective content changes (every changed item / recipe field and recipe list, from data packs and mods' code
// layers: kiln.content's export_changes) to Kiln clients when they join and whenever they change; a client in a joined
// game applies them to its own registries as one owner layer ("server sync") on top of its own, and takes them off
// on leave. A hosting player needs nothing (one process, the same item data). Channel "kiln:content" v1, not required.
// Protocol, limits and checks: docs/kiln/sync.md.
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>Client side of the content sync, for status displays.</summary>
public enum ContentSyncState
{
    /// <summary>Not connected to a Kiln server with content sync.</summary>
    Off = 0,
    /// <summary>Receiving, or waiting for this game's registries to load.</summary>
    Waiting = 1,
    /// <summary>The server's changes are applied.</summary>
    Applied = 2,
    /// <summary>Hosting: the server is this process; nothing to apply.</summary>
    Hosting = 3,
    /// <summary>The server's content could not be used (too big, bad data, kiln.content unavailable here).</summary>
    Failed = 4,
}

/// <summary>Result of <see cref="ContentSync.CheckRoundTrip"/>.</summary>
public sealed record ContentRoundTrip(bool Ok, int Fields, int Applied, int Reset, double Milliseconds, IReadOnlyList<string> Problems)
{
    public override string ToString() =>
        $"{(Ok ? "ok" : "FAILED")}: {Fields} change(s) exported, {Applied} re-applied unchanged, {Reset} put back for a vanilla server, " +
        $"{Milliseconds:F1} ms{(Problems.Count > 0 ? "; " + string.Join("; ", Problems) : "")}";
}

/// <summary>Client side of the content sync, as of now.</summary>
public readonly record struct ContentSyncStatus(ContentSyncState State, uint StateId, int ServerFields, int Applied,
    int Reset, int Errors, int Bytes, int CompressedBytes, DateTime? AppliedAt, string Detail);

/// <summary>
/// Client: a Kiln server's content changes were applied to this game's registries (<see cref="Restored"/> false), or
/// taken off again after leaving (true). Fires on the tick thread (the client's main thread in a joined game) after
/// the registries changed. Not cancelable; nothing to change. Code that caches registry values (max stacks, recipe
/// lists) should re-read them here.
/// </summary>
public sealed class ContentSyncedEvent : KilnEvent
{
    internal ContentSyncedEvent(bool restored, int applied, int reset, IReadOnlyList<string> errors, uint stateId)
    {
        Restored = restored;
        Applied = applied;
        Reset = reset;
        Errors = errors;
        StateId = stateId;
    }

    public bool Restored { get; }

    /// <summary>Field / list changes taken from the server (on restore: fields put back).</summary>
    public int Applied { get; }

    /// <summary>Fields this game had changed on its own that were put back to the game's original to match the server.</summary>
    public int Reset { get; }

    public IReadOnlyList<string> Errors { get; }

    /// <summary>The server's content state (hash of its document).</summary>
    public uint StateId { get; }
}

/// <summary>Kiln's content sync (runs by itself with Kiln.Content and Kiln.Network).</summary>
public static unsafe class ContentSync
{
    public const string ChannelName = "kiln:content";
    public const string ChannelVersion = "1";
    internal const ushort Format = 1;
    internal const byte CodecDeflate = 1;
    private const ushort MsgPart = 0, MsgAck = 1;
    // Compressed bytes per message at most. The server cuts parts to fit one kiln.net frame (its max frame minus the
    // headers), so the state never depends on Kiln.Network's fragmentation; up to PartBytes a client accepts any size.
    internal const int PartBytes = 96 * 1024;
    internal const int MaxParts = 4096;
    // items, recipes and (kiln.gamedata, when the build has it) its sections and item config paths
    private const uint Kinds = (1u << (int)ContentKind.Items) | (1u << (int)ContentKind.Recipes) | (1u << 16);
    private static readonly string[] ResetSections = ["items", "recipes", "templates", "arsenals", "sequences", "buffs", "perks", "skills", "itemdata"];

    /// <summary>One message: part <c>Part</c> of <c>Parts</c> of the server's compressed document. Parts = 0: the
    /// server does not sync its content (Data is the reason, UTF-8).</summary>
    internal sealed record PartMsg(ushort Format, uint StateId, ulong Generation, uint RawLength, uint Fields, ushort Part,
        ushort Parts, byte Codec, byte[] Data)
    {
        public void Encode(NetWriter w) =>
            w.UInt16(Format).UInt32(StateId).UInt64(Generation).UInt32(RawLength).UInt32(Fields).UInt16(Part).UInt16(Parts)
                .Byte(Codec).Bytes(Data);

        public static PartMsg Decode(ref NetReader r) =>
            new(r.UInt16(), r.UInt32(), r.UInt64(), r.UInt32(), r.UInt32(), r.UInt16(), r.UInt16(), r.Byte(), r.Bytes(PartBytes + 1024));
    }

    /// <summary>Client to server: what the client did with a state. Result 0 applied, 1 failed, 2 hosting (skipped),
    /// 3 refused (too big / disabled, no added-buff room), 4 deferred (<see cref="AckDeferred"/>).</summary>
    internal sealed record AckMsg(uint StateId, byte Result, uint Applied, uint Reset, uint Errors, uint Millis, string Detail)
    {
        public void Encode(NetWriter w) =>
            w.UInt32(StateId).Byte(Result).VarUInt(Applied).VarUInt(Reset).VarUInt(Errors).VarUInt(Millis).String(Detail);

        public static AckMsg Decode(ref NetReader r) =>
            new(r.UInt32(), r.Byte(), (uint)r.VarUInt(), (uint)r.VarUInt(), (uint)r.VarUInt(), (uint)r.VarUInt(), r.String(4096));
    }

    /// <summary>Ack result: a client that cannot apply the state yet (its world does not run: a server holds it at the join,
    /// and a joining game's registries load only in its world's steps) received it and applies it once its world runs. That
    /// later ack decides (a failure refuses it then). Since 0.2.0-dev.30, sent only to a server whose WELCOME has
    /// <see cref="HeldAnswerProperty"/>: an older server reads any result but 0 and 2 as a failure.</summary>
    internal const byte AckDeferred = 4;

    /// <summary>Handshake property, value "1", on both sides. A client's HELLO: it answers a content state while the server
    /// holds it at the join (<see cref="AckDeferred"/> or a refusal); a server holds only such clients for its added-buff
    /// check (an older client answers once its world runs, which the hold prevents: every join was refused after 25 s). A
    /// server's WELCOME: it takes that answer.</summary>
    internal const string HeldAnswerProperty = "kiln.content.held-answer";

    private static readonly object Lock = new();
    private static NetChannel? _channel;
    private static int _started;
    private static ulong _owner;

    // ---- server role (tick thread) ----
    private sealed class Export
    {
        public required uint StateId;   // what clients see: the document's hash (another value when it is not sent)
        public required uint RawId;     // the document's hash
        public required ulong Generation;
        public required int Changed;
        public required uint Fields;
        public required int RawLength;
        public required byte[][] Parts;
        public string? NotSynced;
        public int MaxKiB;
        public bool SendContent;
        public uint KilnTemplates;      // Kiln-only templates when it was built: what a client that acks it can have
    }

    private static Export? _export;
    private static DateTime _exportAt;
    private static readonly Dictionary<uint, uint> Sent = new();      // peer -> state id it has completely
    private static readonly Dictionary<uint, uint> Acked = new();     // peer -> state id it acknowledged last (under the Sent lock)
    private static readonly Dictionary<uint, (uint StateId, int Next)> Sending = new();  // peer -> parts sent so far
    private const int PartsPerTick = 48;  // per peer and server tick (~1 s): about 380 KB/s with 8 KB parts

    // ---- client role (main thread; received data under Lock) ----
    private sealed class Incoming(uint stateId, ushort parts)
    {
        public readonly uint StateId = stateId;
        public readonly byte[]?[] Parts = new byte[]?[parts];
        public int Have;
        public readonly DateTime Started = DateTime.UtcNow;
    }

    private static Incoming? _incoming;
    private static (uint StateId, string Json, uint Fields, int Compressed)? _pending;
    private static uint _appliedState;
    private static ulong _appliedGeneration;
    private static volatile bool _dirty, _leavePending;
    private static ContentSyncStatus _status = new(ContentSyncState.Off, 0, 0, 0, 0, 0, 0, 0, null, "");

    /// <summary>Client side: what the content sync did last.</summary>
    public static ContentSyncStatus Status => _status;

    /// <summary>Server side: the content state being served (id, fields, document bytes), or null.</summary>
    /// <summary>The content state id a peer acknowledged last, or null (Kiln.Replication's item remap: a peer that has not
    /// acked the state served now lacks its Kiln-only items).</summary>
    internal static uint? AckedStateOf(uint peer)
    {
        lock (Sent) return Acked.TryGetValue(peer, out var s) ? s : null;
    }

    public static (uint StateId, uint Fields, int Bytes, int Parts)? Serving =>
        _export is { } e && ServerRole ? (e.StateId, e.Fields, e.RawLength, e.Parts.Length) : null;

    /// <summary>Started by Kiln.Content's initialisation (service autostart). Idempotent.</summary>
    internal static void Start()
    {
        if (Interlocked.CompareExchange(ref _started, 1, 0) != 0) return;
        try
        {
            var prev = ModContext.CurrentMod;
            ModContext.CurrentMod = null;  // Kiln's own channel and handlers
            try
            {
                _channel = Network.Channel(ChannelName, ChannelVersion, required: false, maxMessageBytes: PartBytes + 4096)
                    .Register<PartMsg>(MsgPart, static (m, w) => m.Encode(w), static (ref NetReader r) => PartMsg.Decode(ref r), OnPart, NetDirection.ToClient)
                    .Register<AckMsg>(MsgAck, static (m, w) => m.Encode(w), static (ref NetReader r) => AckMsg.Decode(ref r), OnAck, NetDirection.ToServer);
                Network.Disconnected += _ => OnLeave("left the server");
                Network.ServerReady += _ => { OnLeave(null); EnsureContentAdmission(); };
                Network.SetHandshakeProperty(HeldAnswerProperty, static () => "1");
                EnsureContentAdmission();
            }
            finally
            {
                ModContext.CurrentMod = prev;
            }
            _owner = CreateOwner("content-sync");
            if (Content.T != null && _owner != 0)
            {
                using var s = new ContentNative.Utf8("server sync");
                Content.T->LabelOwner(_owner, s.Str);
            }
            ServiceTick.Add("kiln content sync", Tick);
        }
        catch (Exception e)
        {
            Log(LogLevel.Warn, $"content sync unavailable: {e.Message}");
        }
    }

    private static void Log(LogLevel level, string text) => Backend.Current.Log(0, level, "sync: " + text);

    /// <summary>A native owner for the sync layer (Kiln's own, never revoked).</summary>
    private static ulong CreateOwner(string id)
    {
        try
        {
            return Backend.Current.OwnerCreate(id);
        }
        catch (Exception e)
        {
            Log(LogLevel.Warn, $"no owner for the content sync layer: {e.Message}");
            return 0;
        }
    }

    private static bool HasExport => Content.T != null && Content.T->Header.Version >= 2 && Content.T->ExportChanges != null;

    private static void Tick(in TickInfo tick)
    {
        if (_leavePending) Leave("left the server");
        if (_dirty || tick.Step % 25 == 0) ClientTick();
        if (tick.Step % 25 == 0) ServerTick(tick.Step);
    }

    // ------------------------------------------------------------------------------------------------- server ----

    // The server role: a dedicated server, or a client while it hosts (its server world runs here). A client in the
    // menu or joined to another machine (REMOTE) has no server world: its only peer is the remote server, never a
    // client of ours. The hosted server world's session number tells hosted games apart.
    internal static bool RequiresBuffAdmission()
    {
        if (!Content.GameDataAvailable) return false;
        try
        {
            var room = Content.Buffs.GetBudget();
            if (room.Ok) return room.Budget.KilnUsed != 0 || room.Budget.Reserved != 0;
        }
        catch (KilnUnavailableException) { }
        return Content.Buffs.Definitions().Count != 0;
    }

    internal static TimeSpan ContentAdmissionDeadline => NetAdmission.MaxDeadline;
    private static IDisposable? _contentAdmission;
    private static readonly Dictionary<uint, AdmissionHold> ContentHolds = new();
    /// <summary>The decider lives only while added buffs need the join check: any decider puts the gate in "hold all"
    /// (every Kiln client held until Kiln.Network lets it in), which a server without added buffs has no need for.</summary>
    private static void EnsureContentAdmission()
    {
        if (!ServerRole || ((NetNative.ReadStatus()?.Roles ?? 0) & NetNative.RoleServer) == 0) return;
        bool required;
        try
        {
            required = RequiresBuffAdmission();
        }
        catch (Exception)
        {
            required = true;  // cannot tell: keep the check, as before
        }
        if (!required)
        {
            if (_contentAdmission != null && Game.IsTickThread) DropContentAdmission();
            return;
        }
        if (_contentAdmission != null) return;
        _contentAdmission = NetAdmission.OnJoin("kiln.content", DecideJoin);
    }

    internal const string HoldReason = "Waiting for this game's answer to the server's content and added-buff check";
    internal const string HoldTimeoutRefusal = "Your game did not answer this server's content and added-buff check in time. " +
                                               "Try joining again; if it happens again, update Kiln on your game and on the server.";

    /// <summary>The join decision (tick thread): a Kiln client that answers while held (<see cref="HeldAnswerProperty"/>) is held
    /// until its answer: a refusal refuses it at the join, a deferral or an applied state lets it in. An older client is not
    /// held: it applies the state once its world runs and could not answer before (a hold on it only ended at its deadline,
    /// refused); a failure of that later apply refuses it then (<see cref="OnAck"/>).</summary>
    internal static AdmissionHold? DecideJoin(NetPeer peer)
    {
        if (!peer.HasChannel(ChannelName) || !RequiresBuffAdmission()) return null;
        if (ContentHolds.Remove(peer.Id, out var previous)) previous.End();
        if (!AnswersWhileHeld(peer))
        {
            Log(LogLevel.Info, $"{peer.Name} (0x{peer.Id:x}): its Kiln answers the content and added-buff check only once its world runs: " +
                               "not held for it; a failure then refuses it");
            return null;
        }
        var hold = NetAdmission.HoldPeer(peer, "kiln.content", HoldReason, ContentAdmissionDeadline, HoldTimeoutRefusal);
        if (!hold.Ended) ContentHolds[peer.Id] = hold;
        return hold;
    }

    internal static bool AnswersWhileHeld(NetPeer peer) => peer.Properties.TryGetValue(HeldAnswerProperty, out var v) && v == "1";
    private static uint _serverSession;
    private static bool _serving;

    /// <summary>Whether this process serves content states now (dedicated server, or a hosting client).</summary>
    internal static bool ServerRole => !Kiln.Client.IsClient || Kiln.Client.IsHosting;

    /// <summary>Drops everything the server side holds (the state, which peer has which parts). Under no lock.</summary>
    /// <summary>The join decider and its holds go (tick thread).</summary>
    private static void DropContentAdmission()
    {
        _contentAdmission?.Dispose(); _contentAdmission = null;
        foreach (var hold in ContentHolds.Values) hold.End();
        ContentHolds.Clear();
    }

    private static void ResetServer(string why)
    {
        DropContentAdmission();

        int peers;
        lock (Sent)
        {
            peers = Sent.Count + Sending.Count;
            Sent.Clear();
            Sending.Clear();
            Acked.Clear();
        }
        var had = _export != null;
        _export = null;
        _exportAt = default;
        if (had || peers > 0) Log(LogLevel.Info, $"content sync: {why}: the served state and {peers} peer record(s) dropped");
    }

    private static void ServerTick(ulong step)
    {
        var role = ServerRole;
        var session = Kiln.Client.IsClient ? Kiln.Client.ServerWorldSession : 0u;
        if (!role || session != _serverSession)
        {
            if (_serving || session != _serverSession)
                ResetServer(!role ? "no server world here (menu or joined to another server)" : "a new hosted world");
            _serverSession = session;
            _serving = role;
            if (!role) return;
        }
        _serving = true;
        EnsureContentAdmission();
        var peers = Network.Peers;
        foreach (var entry in ContentHolds.ToArray())
            if (entry.Value.Ended || !peers.Any(p => p.Id == entry.Key))
            {
                ContentHolds.Remove(entry.Key);
                entry.Value.End();
            }
        if (_channel is null || peers.Count == 0 || !peers.Any(p => p.State == NetPeerState.Ready && p.HasChannel(ChannelName)))
        {
            lock (Sent)
            {
                Sent.Clear();
                Sending.Clear();
                Acked.Clear();
            }
            return;
        }
        if (!Content.Available || !HasExport) return;
        var st = Content.Status();
        if (st is not { Ready: true }) return;  // the registries are not loaded yet (players cannot be in game either)
        var settings = SyncSettings.Current;
        // A Kiln-only template added (a hot load) is due at once: until a client acks a state built after it, kiln.net keeps
        // that template's entities from it (docs/kiln/network.md "Clients without Kiln").
        var templates = NetEngine.KilnTemplateCount;
        // value-only changes (no new field, same generation) show up at the periodic export: every 10 s of wall time
        var due = _export is null || _export.Generation != st.Generation || _export.Changed != st.Changed ||
                  (DateTime.UtcNow - _exportAt).TotalSeconds >= 10 ||
                  _export.MaxKiB != settings.MaxContentKiB || _export.SendContent != settings.SendContent ||
                  _export.KilnTemplates != templates;
        if (due)
        {
            _exportAt = DateTime.UtcNow;
            Build(st, templates);
        }
        var e = _export;
        if (e is null) return;
        var host = Kiln.Client.IsClient && Kiln.Client.IsHosting ? Kiln.Client.LocalPlayer : EntityId.None;
        lock (Sent)
        {
            foreach (var p in peers)
            {
                if (p.State != NetPeerState.Ready || !p.HasChannel(ChannelName)) continue;
                if (Sent.TryGetValue(p.Id, out var have) && have == e.StateId) continue;
                if (!host.IsNone && p.Player == host)
                {
                    Sent[p.Id] = e.StateId;  // the host's own client shares this process's registries
                    continue;
                }
                if (Send(p, e)) Sent[p.Id] = e.StateId;
            }
            foreach (var gone in Sent.Keys.Where(k => !peers.Any(p => p.Id == k)).ToList()) Sent.Remove(gone);
            foreach (var gone in Sending.Keys.Where(k => !peers.Any(p => p.Id == k)).ToList()) Sending.Remove(gone);
            foreach (var gone in Acked.Keys.Where(k => !peers.Any(p => p.Id == k)).ToList()) Acked.Remove(gone);
        }
    }

    private static void Build(ContentStatus st, uint templates)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var json = ExportJson(Kinds, out var rc);
        if (json is null)
        {
            Log(LogLevel.Warn, $"content export failed: {ContentNative.Message(rc)}");
            return;
        }
        json = WithoutMachineLocal(json, MachineLocal(), out _);
        json = WithDefinitions(json, out var defCount);
        json = WithPrograms(json, out var programCount);
        var exportMs = sw.Elapsed.TotalMilliseconds;
        var raw = Encoding.UTF8.GetBytes(json);
        var settings = SyncSettings.Current;
        var id = Fnv(raw);
        if (_export is { } old && old.RawId == id && old.MaxKiB == settings.MaxContentKiB && old.SendContent == settings.SendContent)
        {
            _export = new Export
            {
                StateId = old.StateId, RawId = id, Generation = st.Generation, Changed = st.Changed, Fields = old.Fields,
                RawLength = old.RawLength, Parts = old.Parts, NotSynced = old.NotSynced, MaxKiB = old.MaxKiB, SendContent = old.SendContent,
                KilnTemplates = templates,
            };
            // The same document with more Kiln-only templates: they came from a mod the server sends nothing of (a client has
            // it or not). A client that acked this document has all it can get.
            if (templates != old.KilnTemplates) ClearAcked(old.StateId, templates);
            return;
        }
        uint fields = 0;
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.TryGetProperty("@fields", out var f)) fields = f.GetUInt32();
        }
        catch (JsonException)
        {
        }
        string? notSynced = null;
        byte[][] parts = [];
        if (!settings.SendContent) notSynced = "the server does not send its content (sendContent is off)";
        else if (raw.Length > settings.MaxContentKiB * 1024)
            notSynced = $"the server's content changes are {raw.Length / 1024} KiB, over its limit of {settings.MaxContentKiB} KiB (maxContentKiB)";
        else
        {
            var packed = Compress(raw);
            var partBytes = PartSize();
            var n = (packed.Length + partBytes - 1) / partBytes;
            if (n > MaxParts) notSynced = $"the server's content changes are too big ({packed.Length / 1024} KiB compressed)";
            else
            {
                n = Math.Max(1, n);
                parts = new byte[n][];
                for (var i = 0; i < n; i++)
                {
                    var off = i * partBytes;
                    parts[i] = packed.AsSpan(off, Math.Min(partBytes, packed.Length - off)).ToArray();
                }
            }
        }
        _export = new Export
        {
            StateId = notSynced is null ? id : id ^ 0x5EED5EEDu, RawId = id, Generation = st.Generation, Changed = st.Changed,
            Fields = fields, RawLength = raw.Length, Parts = parts, NotSynced = notSynced, MaxKiB = settings.MaxContentKiB,
            SendContent = settings.SendContent, KilnTemplates = templates,
        };
        var compressed = parts.Sum(p => p.Length);
        Log(notSynced is null ? LogLevel.Info : LogLevel.Warn,
            $"content state {_export.StateId:x8}: {fields} change(s), {defCount} definition(s), {programCount} program(s) (generation {st.Generation}), {raw.Length / 1024.0:F1} KiB JSON -> " +
            $"{compressed / 1024.0:F1} KiB deflate in {parts.Length} part(s); export {exportMs:F1} ms, total {sw.Elapsed.TotalMilliseconds:F1} ms" +
            (notSynced is null ? "" : $"; NOT SENT: {notSynced}"));
    }

    // ---- machine-local fields: a clone's fields only the machine that draws reads ----

    private static readonly MachineLocalFields Marked = new();

    /// <summary>Marks one field of a clone as machine-local: only the machine that draws reads it, so each machine keeps its own value. The server leaves it
    /// out of what it sends, and a client's matchServer does not put its own back; the clone's other fields sync as any. Kiln.Blocks marks the model and
    /// the graph of a held model with moving parts, which only a machine that cooked has (Items/AnimatedHeld.cs). Nothing else is marked.</summary>
    internal static void MarkMachineLocal(string section, Guid clone, string path) => Marked.Add(section, clone, path);

    /// <summary>What is marked now (a copy).</summary>
    internal static MachineLocalFields MachineLocal() => Marked.Copy();

    /// <summary>Server: the exported document without the machine-local fields (an entry left with nothing is left out, and <c>@fields</c> follows).
    /// With none of them in it, the document is returned as it was, byte for byte. Pure (tests).
    /// A clone's definition in a section's <c>add</c> list keeps the fields: a definition only gives a clone its fields while the client has none of its
    /// own, and stripped it would leave a clone the server defined first with its base's values until the client's mod defines it.</summary>
    internal static string WithoutMachineLocal(string json, MachineLocalFields marks, out int removed)
    {
        removed = 0;
        if (marks.IsEmpty || JsonNode.Parse(json) is not JsonObject doc) return json;
        foreach (var (section, node) in doc.ToList())
        {
            if (node is not JsonObject sec) continue;
            foreach (var (id, body) in sec.ToList())
            {
                if (body is not JsonObject b) continue;
                foreach (var key in b.Select(x => x.Key).Where(k => !k.StartsWith('@') && marks.Has(section, id, k)).ToList())
                {
                    b.Remove(key);
                    removed++;
                }
                if (b.All(x => x.Key.StartsWith('@'))) sec.Remove(id);
            }
        }
        if (removed == 0) return json;
        if (doc["@fields"] is JsonValue f && f.TryGetValue<int>(out var n)) doc["@fields"] = Math.Max(0, n - removed);
        return doc.ToJsonString();
    }

    // ---- definitions (A5): new items / recipes of mods with "clients": "kiln" (and placeholders) ----

    private static readonly Dictionary<string, (string Dir, bool Kiln)> ClientsCache = new(StringComparer.OrdinalIgnoreCase);

    /// <summary>The mod's manifest says <c>"clients": "kiln"</c>: Kiln clients get its definitions from the server and
    /// do not need the mod. (Default from <c>side</c>: server -> none, client / both -> mod.)</summary>
    internal static bool ClientsKiln(string? modId)
    {
        if (string.IsNullOrEmpty(modId)) return false;
        var e = Kiln.Mods.All.FirstOrDefault(m => string.Equals(m.Id, modId, StringComparison.OrdinalIgnoreCase));
        if (e is null) return false;
        if (ClientsCache.TryGetValue(modId, out var c) && c.Dir == e.Directory) return c.Kiln;
        var kiln = false;
        try
        {
            // ModInfo.Clients once the manifest field reaches Kiln.Api; until then the manifest itself
            var prop = e.Info?.GetType().GetProperty("Clients");
            if (prop?.GetValue(e.Info) is { } v) kiln = string.Equals(v.ToString(), "kiln", StringComparison.OrdinalIgnoreCase);
            else
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(e.Directory, "kiln.mod.json")),
                    new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                kiln = doc.RootElement.TryGetProperty("clients", out var cl) && cl.ValueKind == JsonValueKind.String &&
                       string.Equals(cl.GetString(), "kiln", StringComparison.OrdinalIgnoreCase);
            }
        }
        catch (Exception)
        {
            kiln = false;
        }
        ClientsCache[modId] = (e.Directory, kiln);
        return kiln;
    }

    /// <summary>Server: adds <c>"@definitions"</c> (the definitions Kiln clients need) to the exported document.</summary>
    private static string WithDefinitions(string json, out int count)
    {
        count = 0;
        if (!NewContent.Available) return json;
        var send = NewContent.Definitions()
            .Where(d => d.State != "orphaned" && d.DefinitionJson != null && d.Worlds.Contains("server") && (d.IsPlaceholder || ClientsKiln(d.Mod)))
            .ToList();
        if (send.Count == 0) return json;
        if (JsonNode.Parse(json) is not JsonObject doc) return json;
        var arr = new JsonArray();
        foreach (var d in send)
        {
            if (JsonNode.Parse(d.DefinitionJson!) is not JsonObject body) continue;
            // a PNG icon is a file on the server: clients show the base's (or the item:<name>) icon
            if (body["icon"] is JsonValue iv && iv.TryGetValue<string>(out var icon) && icon.EndsWith(".png", StringComparison.OrdinalIgnoreCase))
                body.Remove("icon");
            arr.Add(new JsonObject { ["kind"] = d.Kind == ContentKind.Items ? "item" : "recipe", ["mod"] = d.Mod, ["definition"] = body });
        }
        count = arr.Count;
        doc["@definitions"] = arr;
        return doc.ToJsonString();
    }

    /// <summary>Server: adds <c>"@programs"</c>, the new Impact programs the document's definitions name (SyncPrograms).</summary>
    private static string WithPrograms(string json, out int count)
    {
        count = 0;
        if (JsonNode.Parse(json) is not JsonObject doc) return json;
        count = SyncPrograms.Add(doc);
        return count > 0 ? doc.ToJsonString() : json;
    }

    /// <summary>Client: defines the server's definitions as the sync owner (skipping GUIDs this game defines itself).</summary>
    private static (int Defined, int Local, List<string> Errors) DefineFromServer(JsonObject server)
    {
        var errors = new List<string>();
        var t = Content.T;
        if (!ContentNative.Has(t, 4) || t->UndefineOwner == null) return (0, 0, errors);
        t->UndefineOwner(_owner);  // the previous server state's definitions are set aside, the current ones adopted again
        if (server["@definitions"] is not JsonArray arr || arr.Count == 0) return (0, 0, errors);
        var local = NewContent.Definitions().Where(d => d.Owner != "server sync" && d.State != "orphaned").Select(d => d.Guid).ToHashSet();
        int defined = 0, skipped = 0;
        foreach (var n in arr.OfType<JsonObject>().OrderBy(o => (string?)o["kind"] == "item" ? 0 : 1))
        {
            if (n["definition"] is not JsonObject body || !Guid.TryParse((string?)body["guid"], out var g)) continue;
            if (local.Contains(g))
            {
                skipped++;
                continue;
            }
            try
            {
                NewContent.Define((string?)n["kind"] == "recipe" ? ContentKind.Recipes : ContentKind.Items, (JsonObject)body.DeepClone(), _owner);
                defined++;
            }
            catch (Exception e) when (e is ArgumentException or NotSupportedException)
            {
                errors.Add($"definition {(string?)body["name"]}: {e.Message}");
            }
        }
        return (defined, skipped, errors);
    }

    /// <summary>Compressed bytes per part: one frame (kiln.net's max frame minus the DATA and part headers).</summary>
    private static int PartSize()
    {
        var max = (int)Math.Max(1024, Network.Status.MaxFrame);
        return Math.Clamp(max - 64, 512, PartBytes);
    }

    /// <summary>Sends the next parts of <paramref name="e"/> to <paramref name="p"/> (at most PartsPerTick per call);
    /// true when the peer has the whole state. Under the Sent lock.</summary>
    private static bool Send(NetPeer p, Export e)
    {
        NetEngine.PeerAwaitsDefinitions(p);
        if (e.NotSynced is { } why)
            return _channel!.SendTo(p, new PartMsg(Format, e.StateId, e.Generation, (uint)e.RawLength, e.Fields, 0, 0, 0, Encoding.UTF8.GetBytes(why)));
        var from = Sending.TryGetValue(p.Id, out var prog) && prog.StateId == e.StateId ? prog.Next : 0;
        var to = Math.Min(e.Parts.Length, from + PartsPerTick);
        for (var i = from; i < to; i++)
            if (!_channel!.SendTo(p, new PartMsg(Format, e.StateId, e.Generation, (uint)e.RawLength, e.Fields, (ushort)i,
                    (ushort)e.Parts.Length, CodecDeflate, e.Parts[i])))
            {
                Log(LogLevel.Warn, $"content state {e.StateId:x8}: sending part {i + 1}/{e.Parts.Length} to {p.Name} failed; retrying at the next tick");
                Sending[p.Id] = (e.StateId, i);
                return false;
            }
        if (to < e.Parts.Length)
        {
            Sending[p.Id] = (e.StateId, to);
            return false;
        }
        Sending.Remove(p.Id);
        Log(LogLevel.Info, $"content state {e.StateId:x8} ({e.Fields} change(s), {e.Parts.Sum(x => x.Length)} bytes in {e.Parts.Length} part(s)) sent to {p.Name}");
        return true;
    }

    /// <summary>The one error a Kiln client before 0.2.0-dev.30 reports for a state with programs (a StatusBlock's, any
    /// clone with a program) although it applied the state whole: kiln.abilities' program_refresh answers WRONG_THREAD (4)
    /// on a joined client, which runs no Impact programs (<see cref="SyncPrograms.RefreshProblem"/>).</summary>
    internal const string OlderClientProgramRefresh = "kiln.abilities did not register the programs (4)";

    /// <summary>An older Kiln client's failed ack whose only error is <see cref="OlderClientProgramRefresh"/>: its apply
    /// was whole (any other error, or a native apply failure, adds an error of its own), so it counts as applied. Without
    /// this a client before 0.2.0-dev.30 was refused by a server with a StatusBlock, at the join or at a hot load. Pure (tests).</summary>
    internal static bool OlderClientAppliedWhole(AckMsg a, NetPeer sender) =>
        a.Result == 1 && a.Errors == 1 && a.Detail == OlderClientProgramRefresh && !AnswersWhileHeld(sender);

    private static void OnAck(AckMsg a, NetContext ctx)
    {
        if (OlderClientAppliedWhole(a, ctx.Sender))
        {
            Log(LogLevel.Info, $"{ctx.Sender.Name}: content state {a.StateId:x8}: its older Kiln reports \"{a.Detail}\", " +
                               "which a joined game answers for programs it never runs: taken as applied");
            a = a with { Result = 0, Errors = 0, Detail = "" };
        }
        var what = a.Result switch
        {
            0 => "applied", 1 => "FAILED", 2 => "skipped (hosting)", 3 => "refused", AckDeferred => "received while joining (applied once its world runs)",
            _ => $"result {a.Result}",
        };
        Log(a.Result is 0 or 2 or AckDeferred ? LogLevel.Info : LogLevel.Warn,
            $"{ctx.Sender.Name}: content state {a.StateId:x8} {what}: {a.Applied} change(s), {a.Reset} local reset, {a.Errors} error(s) in {a.Millis} ms" +
            (a.Detail.Length > 0 ? $"; {a.Detail}" : ""));
        if (a.Result == AckDeferred)
        {
            // held at the join: its world cannot run before it is let in, nor its registries load. Its apply then decides.
            if (ContentHolds.Remove(ctx.Sender.Id, out var held)) held.End();
            return;
        }
        if (a.Result is not (0 or 2))
        {
            lock (Sent) Acked.Remove(ctx.Sender.Id);
            var required = RequiresBuffAdmission();
            if (ContentHolds.Remove(ctx.Sender.Id, out var refused))
            {
                if (required) refused.Refuse(a.Detail); else refused.End();
            }
            else if (required) NetAdmission.RefusePeer(ctx.Sender, a.Detail, "kiln.content");
            return;
        }
        if (_export is not { } current || current.StateId != a.StateId) return;
        if (ContentHolds.Remove(ctx.Sender.Id, out var accepted)) accepted.End();
        lock (Sent) Acked[ctx.Sender.Id] = a.StateId;
        // docs/kiln/network.md "Clients without Kiln": only an ack of the state served now clears the client. An older one may
        // lack a template added since; the current one is on its way to it.
        if (_export is { } e && e.StateId == a.StateId) NetEngine.PeerHasDefinitions(ctx.Sender, e.KilnTemplates);
        else Log(LogLevel.Info, $"{ctx.Sender.Name} (0x{ctx.Sender.Id:x}): content state {a.StateId:x8} is not the one served now ({(_export is { } c ? c.StateId.ToString("x8") : "none")}): its Kiln-only entities wait for that one");
    }

    /// <summary>Clears the clients whose last ack was of <paramref name="stateId"/>, now that it is known to cover
    /// <paramref name="templates"/> Kiln-only templates. Tick thread.</summary>
    private static void ClearAcked(uint stateId, uint templates)
    {
        List<uint> ids;
        lock (Sent) ids = Acked.Where(kv => kv.Value == stateId).Select(kv => kv.Key).ToList();
        foreach (var p in Network.Peers)
            if (ids.Contains(p.Id) && p.State == NetPeerState.Ready) NetEngine.PeerHasDefinitions(p, templates);
    }

    // ------------------------------------------------------------------------------------------------- client ----

    private static void OnPart(PartMsg m, NetContext ctx)
    {
        if (m.Format != Format)
        {
            Log(LogLevel.Warn, $"content format {m.Format} from the server is not supported ({Format}); ignored");
            return;
        }
        if (Kiln.Client.IsClient && Kiln.Client.IsHosting)
        {
            if (m.Part == 0) _channel?.SendToServer(new AckMsg(m.StateId, 2, 0, 0, 0, 0, "hosting: same process"));
            _status = _status with { State = ContentSyncState.Hosting, StateId = m.StateId, Detail = "hosting: the server's content is this game's" };
            return;
        }
        if (m.Parts == 0)
        {
            var why = Encoding.UTF8.GetString(m.Data);
            Log(LogLevel.Warn, $"the server's content is not synced: {why}");
            lock (Lock)
            {
                _incoming = null;
                _pending = null;
            }
            _status = _status with { State = ContentSyncState.Failed, StateId = m.StateId, Detail = why };
            _channel?.SendToServer(new AckMsg(m.StateId, 3, 0, 0, 0, 0, "server: " + why));
            return;
        }
        if (m.Parts > MaxParts || m.Part >= m.Parts || m.Codec != CodecDeflate) return;
        lock (Lock)
        {
            if (_incoming is null || _incoming.StateId != m.StateId || _incoming.Parts.Length != m.Parts) _incoming = new Incoming(m.StateId, m.Parts);
            var inc = _incoming;
            if (inc.Parts[m.Part] is null)
            {
                inc.Parts[m.Part] = m.Data;
                inc.Have++;
            }
            _status = _status with { State = ContentSyncState.Waiting, StateId = m.StateId, Detail = $"receiving {inc.Have}/{m.Parts}" };
            if (inc.Have < inc.Parts.Length) return;
            _incoming = null;
            var packed = new byte[inc.Parts.Sum(p => p!.Length)];
            var off = 0;
            foreach (var p in inc.Parts)
            {
                p!.CopyTo(packed, off);
                off += p.Length;
            }
            var limit = SyncSettings.Current.MaxContentKiB * 1024;
            string? error = null;
            byte[]? raw = null;
            if (m.RawLength > limit) error = $"{m.RawLength / 1024} KiB of content changes, over this game's limit of {limit / 1024} KiB (maxContentKiB)";
            else
            {
                try
                {
                    raw = Decompress(packed, (int)m.RawLength);
                    if (raw.Length != m.RawLength) error = $"the document is {raw.Length} bytes, not {m.RawLength}";
                    else if (Fnv(raw) != m.StateId) error = "the document does not match its id";
                }
                catch (Exception e) when (e is InvalidDataException or IOException)
                {
                    error = "bad compressed data: " + e.Message;
                }
            }
            if (error is not null || raw is null)
            {
                Log(LogLevel.Warn, $"content state {m.StateId:x8} from the server refused: {error}");
                _status = _status with { State = ContentSyncState.Failed, Detail = error ?? "?" };
                _channel?.SendToServer(new AckMsg(m.StateId, 3, 0, 0, 0, 0, error ?? ""));
                return;
            }
            _pending = (m.StateId, Encoding.UTF8.GetString(raw), m.Fields, packed.Length);
            Log(LogLevel.Info, $"content state {m.StateId:x8} from the server: {m.Fields} change(s), {raw.Length / 1024.0:F1} KiB ({packed.Length / 1024.0:F1} KiB compressed, {m.Parts} part(s))");
        }
        _dirty = true;
        if (Game.IsTickThread) ClientTick();
        AnswerWhileWaiting();
    }

    private static uint _heldAnswered;  // the state this game answered before it could apply it

    /// <summary>Client, any thread: a state this game cannot apply yet because its registries are not loaded (a joining game
    /// loads them in its world's steps, and a server holding it at the join sends it no world data until it answers) is
    /// answered at once, once per state: refused when this game's added-buff room is known and too small, else deferred
    /// (<see cref="AckDeferred"/>). It is applied once the world runs, and that ack decides.</summary>
    private static void AnswerWhileWaiting()
    {
        (uint StateId, string Json, uint Fields, int Compressed) p;
        lock (Lock)
        {
            // _appliedState is the last connection's while the Leave of a new handshake still waits for a step
            if (_pending is not { } pp || _heldAnswered == pp.StateId || (!_leavePending && _appliedState == pp.StateId)) return;
            p = pp;
        }
        if (Kiln.Client.IsClient && Kiln.Client.IsHosting) return;
        // No Kiln tick runs in a held game (its world does not step and kiln.ui.tick's menu tick waits while a world is
        // there), so ClientTick cannot answer what needs no registries either: answered here, as ClientTick would.
        AckMsg ack;
        if (!SyncSettings.Current.AcceptContent) ack = new AckMsg(p.StateId, 3, 0, 0, 0, 0, AcceptContentOff);
        else if (!Content.Available || !HasExport || _owner == 0) ack = new AckMsg(p.StateId, 1, 0, 0, 0, 0, ContentUnavailable);
        else if (Content.Status() is { Ready: true }) return;  // ClientTick applies it
        else ack = HeldAnswerNow(p.StateId, p.Json);
        // a deferral only to a server whose WELCOME takes it (an older one reads any result but 0 and 2 as a failure); a
        // refusal is an answer every Kiln server reads, and there it gives the player its reason before the hold's deadline
        if (ack.Result == AckDeferred && !ServerTakesHeldAnswers) return;
        lock (Lock)
        {
            if (_heldAnswered == p.StateId || _pending?.StateId != p.StateId) return;  // answered, or left meanwhile
            _heldAnswered = p.StateId;
        }
        if (ack.Result == AckDeferred)
        {
            Log(LogLevel.Info, $"content state {p.StateId:x8}: this game's registries load once its world runs: told the server, applied then");
            _status = _status with { State = ContentSyncState.Waiting, StateId = p.StateId, Detail = "applies once this game's world runs" };
        }
        else
        {
            Log(LogLevel.Warn, $"content state {p.StateId:x8} refused while joining: {ack.Detail}");
            _status = _status with { State = ContentSyncState.Failed, StateId = p.StateId, Detail = ack.Detail };
        }
        _channel?.SendToServer(ack);
    }

    internal const string AcceptContentOff = "client: this game does not take a server's content (acceptContent is off in its kiln/config/sync.json)";
    internal const string ContentUnavailable = "client: kiln.content is not available in this game (update or repair Kiln)";

    /// <summary>The answer to a state this game cannot apply yet (<see cref="HeldAnswer"/>), from this game's buff room now.</summary>
    private static AckMsg HeldAnswerNow(uint stateId, string json)
    {
        AckMsg ack;
        try
        {
            var server = JsonNode.Parse(json) as JsonObject;
            CapacityInfo? room = null;
            IReadOnlySet<Guid> existing = new HashSet<Guid>();
            if (server?["buffs"] is JsonObject { } buffs && buffs["add"] is JsonArray { Count: > 0 })
            {
                var budget = Content.Buffs.GetBudget();
                if (budget.Budget is { Ready: true } known)
                {
                    room = known;
                    existing = Content.Buffs.Definitions().Where(d => d.State is "registered" or "orphaned").Select(d => d.Guid).ToHashSet();
                }
            }
            ack = HeldAnswer(stateId, server, room, existing);
        }
        catch (Exception)
        {
            ack = HeldAnswer(stateId, null, null, new HashSet<Guid>());  // what cannot be checked now is checked when it applies
        }
        return ack;
    }

    internal const string DeferredDetail = "client: this game applies the server's content once its world runs";

    /// <summary>The server this game joined said in its WELCOME that it takes an answer while it holds this game.</summary>
    private static bool ServerTakesHeldAnswers => Network.Server is { } server && AnswersWhileHeld(server);

    /// <summary>The answer to a state this game cannot apply yet: refused (3) with the numbers when its added-buff room
    /// (<paramref name="room"/>, null when not known yet) cannot hold the server's added buffs, else deferred. Pure (tests).</summary>
    internal static AckMsg HeldAnswer(uint stateId, JsonObject? server, CapacityInfo? room, IReadOnlySet<Guid> existing)
    {
        if (server is not null && room is { Ready: true } r && BuffRoomRefusal(server, r, existing) is { } reason)
            return new AckMsg(stateId, 3, 0, 0, 1, 0, reason);
        return new AckMsg(stateId, AckDeferred, 0, 0, 0, 0, DeferredDetail);
    }

    /// <summary>Applies a received state once this game's registries are ready (tick thread).</summary>
    private static void ClientTick()
    {
        if (!Game.IsTickThread) return;
        _dirty = false;
        (uint StateId, string Json, uint Fields, int Compressed) p;
        lock (Lock)
        {
            if (_incoming is { } inc && (DateTime.UtcNow - inc.Started).TotalSeconds > 60)
            {
                Log(LogLevel.Warn, $"content state {inc.StateId:x8}: {inc.Have}/{inc.Parts.Length} part(s) in 60 s; dropped");
                _incoming = null;
            }
            if (_pending is not { } pp) return;
            p = pp;
        }
        if (Kiln.Client.IsClient && Kiln.Client.IsHosting) return;
        if (!SyncSettings.Current.AcceptContent)
        {
            if (_appliedState != p.StateId)
            {
                _appliedState = p.StateId;
                Log(LogLevel.Info, "the server's content changes are not used (acceptContent is off in kiln/config/sync.json)");
                _channel?.SendToServer(new AckMsg(p.StateId, 3, 0, 0, 0, 0, AcceptContentOff));
                _status = _status with { State = ContentSyncState.Failed, Detail = "acceptContent is off" };
            }
            return;
        }
        if (!Content.Available || !HasExport || _owner == 0)
        {
            if (_status.State != ContentSyncState.Failed)
            {
                Log(LogLevel.Warn, "kiln.content (v2) is not available in this game: the server's content changes cannot be applied");
                _channel?.SendToServer(new AckMsg(p.StateId, 1, 0, 0, 0, 0, ContentUnavailable));
                _status = _status with { State = ContentSyncState.Failed, Detail = "kiln.content unavailable" };
            }
            return;
        }
        var st = Content.Status();
        if (st is not { Ready: true })
        {
            _status = _status with { State = ContentSyncState.Waiting, StateId = p.StateId, Detail = "waiting for this game's registries" };
            return;
        }
        if (_appliedState == p.StateId && _appliedGeneration == st.Generation) return;
        try
        {
            Apply(p.StateId, p.Json, p.Fields, p.Compressed, st.Generation);
        }
        catch (Exception e)
        {
            // never silent: a server that holds this game, or waits for its content ack, gets the reason
            _appliedState = p.StateId;
            _appliedGeneration = st.Generation;
            var why = $"client: applying the server's content failed: {e.GetType().Name}: {e.Message}";
            Log(LogLevel.Warn, $"content state {p.StateId:x8}: {why}");
            _status = _status with { State = ContentSyncState.Failed, StateId = p.StateId, Detail = why };
            _channel?.SendToServer(new AckMsg(p.StateId, 1, 0, 0, 1, 0, why));
        }
    }

    internal static string? BuffRoomRefusal(JsonObject server, CapacityInfo room, IReadOnlySet<Guid> existing)
    {
        if (server["buffs"] is not JsonObject section || section["add"] is not JsonArray adds || adds.Count == 0) return null;
        var guids = adds.OfType<JsonObject>().Select(x => Guid.TryParse((string?)x["guid"], out var g) ? g : Guid.Empty).ToHashSet();
        if (guids.Contains(Guid.Empty)) return "This server sent an invalid added buff GUID; update the server's mods.";
        if (!room.Ready || room.Available is not { } available) return "This game's added-buff room is not ready; rejoin after content loads.";
        var needed = guids.Count(g => !existing.Contains(g));
        return needed <= available ? null : $"This server uses {guids.Count} added buffs; this game has room for {room.Capacity} " +
            $"({available} slots free). Restart the game to retry its larger reservation, or use fewer buff mods on the server.";
    }
    internal static bool ApplySucceeded(int rc, IReadOnlyList<string> errors) => rc == ContentNative.Ok && errors.Count == 0;

    private static void Apply(uint stateId, string json, uint fields, int compressed, ulong generation)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var t = Content.T;
        JsonObject server;
        try
        {
            server = JsonNode.Parse(json) as JsonObject ?? throw new JsonException("not an object");
        }
        catch (JsonException e)
        {
            Log(LogLevel.Warn, $"content state {stateId:x8}: not JSON ({e.Message})");
            _channel?.SendToServer(new AckMsg(stateId, 1, 0, 0, 1, 0, "client: not JSON"));
            _status = _status with { State = ContentSyncState.Failed, Detail = "bad document" };
            _appliedState = stateId;
            _appliedGeneration = generation;
            return;
        }
        if (server["buffs"] is JsonObject buffs && buffs["add"] is JsonArray { Count: > 0 })
        {
            var reason = BuffRoomRefusal(server, Content.Buffs.GetBudget().Budget,
                Content.Buffs.Definitions().Where(d => d.State is "registered" or "orphaned").Select(d => d.Guid).ToHashSet());
            if (reason is not null)
            {
                _status = _status with { State = ContentSyncState.Failed, StateId = stateId, Detail = reason };
                _channel?.SendToServer(new AckMsg(stateId, 3, 0, 0, 1, 0, reason));
                return;
            }
        }
        t->RevertOwner(_owner);
        // the programs the definitions name first: kiln.gamedata checks a clone's program reference against what is served
        var (programs, localPrograms, programErrors) = SyncPrograms.Serve(server, _owner);
        if (programs > 0 || localPrograms > 0)
            Log(LogLevel.Info, $"content state {stateId:x8}: {programs} program(s) from the server served here" +
                               (localPrograms > 0 ? $" ({localPrograms} this game has itself)" : ""));
        server.Remove("@programs");
        var (defined, localDefs, defErrors) = DefineFromServer(server);
        if (defined > 0 || localDefs > 0)
            Log(LogLevel.Info, $"content state {stateId:x8}: {defined} new item / recipe definition(s) from the server" +
                               (localDefs > 0 ? $" ({localDefs} defined by this game's own mods)" : ""));
        server.Remove("@definitions");
        var reset = SyncSettings.Current.MatchServer ? AddResets(server, MachineLocal()) : 0;
        // Load sequences and their effective rules first. No item may select them until this client's registry/graph accepts them.
        var nonSlots = server.DeepClone().AsObject();
        nonSlots.Remove("itemdata");
        var (applied, errors, rc) = ApplyJson(nonSlots.ToJsonString(), "server sync");
        SyncedMoves.Filter(server, Content.GameDataDefinitions(), Content.SequenceRegistrationReader(), SyncedMoves.ReadStates,
            AnimationGraphReferences.Find, text => Log(LogLevel.Warn, text));
        if (server["itemdata"] is { } slots)
        {
            var slotDocument = new JsonObject { ["format"] = 1, ["itemdata"] = slots.DeepClone() };
            var (slotApplied, slotErrors, slotRc) = ApplyJson(slotDocument.ToJsonString(), "server sync");
            applied += slotApplied;
            errors.AddRange(slotErrors);
            if (rc == ContentNative.Ok) rc = slotRc;
        }
        errors.InsertRange(0, defErrors);
        errors.InsertRange(0, programErrors);
        _appliedState = stateId;
        _appliedGeneration = Content.Status()?.Generation ?? generation;
        var ms = (uint)sw.ElapsedMilliseconds;
        var ack = MakeApplyAck(stateId, rc, applied, reset, errors, ms);
        var ok = ack.Result == 0;
        var examples = Examples(server, 4);
        Log(errors.Count == 0 && ok ? LogLevel.Info : LogLevel.Warn,
            $"content state {stateId:x8} applied: {applied} change(s) from the server ({fields} sent{(reset > 0 ? $", {reset} local-only change(s) put back" : "")}), " +
            $"{errors.Count} error(s), in {ms} ms{(examples.Length > 0 ? "; e.g. " + examples : "")}");
        foreach (var e in errors.Take(8)) Log(LogLevel.Warn, "content sync: " + e);
        _status = new ContentSyncStatus(ok ? ContentSyncState.Applied : ContentSyncState.Failed, stateId, (int)fields, applied, reset,
            errors.Count, Encoding.UTF8.GetByteCount(json), compressed, DateTime.UtcNow,
            ok ? $"{applied} change(s) from the server" : ContentNative.Message(rc));
        _channel?.SendToServer(ack);
        if (Events.HasSubscribers<ContentSyncedEvent>()) Events.Post(new ContentSyncedEvent(false, applied, reset, errors, stateId));
    }

    /// <summary>Match the server: what this game changed on its own (local packs and mods; the sync layer must be off)
    /// and the server did not goes back to the game's original ({"mul": 1} = the original value / list) in the
    /// server's document. Returns how many entries were added.</summary>
    private static int AddResets(JsonObject server, MachineLocalFields? keep = null)
    {
        if (ExportJson(Kinds, out _) is not { } localJson || JsonNode.Parse(localJson) is not JsonObject local) return 0;
        return AddResets(server, local, keep);
    }

    /// <summary><see cref="AddResets(JsonObject, MachineLocalFields?)"/> on this game's exported document: pure (tests). A machine-local field
    /// (<paramref name="keep"/>) stays as this game has it.</summary>
    internal static int AddResets(JsonObject server, JsonObject local, MachineLocalFields? keep = null)
    {
        var reset = 0;
        foreach (var section in ResetSections)
        {
            if (local[section] is not JsonObject ls) continue;
            foreach (var (id, body) in ls)
            {
                if (body is not JsonObject lb) continue;
                foreach (var (key, _) in lb)
                {
                    if (key.StartsWith('@') || keep?.Has(section, id, key) == true) continue;
                    if (server[section]?[id]?[key] is not null) continue;
                    if (server[section] is not JsonObject ss) server[section] = ss = new JsonObject();
                    if (ss[id] is not JsonObject sb) ss[id] = sb = new JsonObject { ["@name"] = lb["@name"]?.DeepClone() };
                    sb[key] = new JsonObject { ["mul"] = 1 };
                    reset++;
                }
            }
        }
        return reset;
    }

    /// <summary>
    /// A self-check of the sync format on this process's own registries (tick thread; a lab / diagnostic tool, also
    /// usable on a server): (1) applies this process's exported state as the sync layer on top and checks the effective
    /// state is unchanged; (2) applies an empty server state with "match the server" and checks that every change is
    /// gone (what a client with local-only changes gets from a vanilla server); (3) takes the layer off and checks the
    /// state is back. Everything happens inside this call: the game sees no step in between.
    /// </summary>
    [TickThreadOnly]
    [RequiresCapability("kiln.content")]
    public static ContentRoundTrip CheckRoundTrip()
    {
        Content.Require("ContentSync.CheckRoundTrip");
        if (_owner == 0 || !HasExport) return new ContentRoundTrip(false, 0, 0, 0, 0, ["content sync is not available"]);
        var problems = new List<string>();
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var t = Content.T;
        t->RevertOwner(_owner);
        var e1 = ExportJson(Kinds, out _) ?? "";
        var fields = JsonNode.Parse(e1)?["@fields"]?.GetValue<int>() ?? 0;
        var (a1, err1, rc1) = ApplyJson(e1, "sync round-trip: same state");
        var e2 = ExportJson(Kinds, out _) ?? "";
        if (rc1 != ContentNative.Ok || err1.Count > 0) problems.Add($"applying the exported state: {string.Join("; ", err1.Take(4))}");
        if (e2 != e1) problems.Add($"the state changed when applied on top: {Difference(e1, e2)}");
        t->RevertOwner(_owner);
        var empty = new JsonObject { ["format"] = 1 };
        var reset = AddResets(empty);
        var (a2, err2, _) = ApplyJson(empty.ToJsonString(), "sync round-trip: vanilla server");
        var e3 = ExportJson(Kinds, out _) ?? "";
        var left = JsonNode.Parse(e3)?["@fields"]?.GetValue<int>() ?? -1;
        if (err2.Count > 0) problems.Add($"matching a vanilla server: {string.Join("; ", err2.Take(4))}");
        if (left != 0) problems.Add($"matching a vanilla server left {left} change(s): {Difference("{\"format\":1}", Unadded(e3))}");
        t->RevertOwner(_owner);
        var e4 = ExportJson(Kinds, out _) ?? "";
        if (e4 != e1) problems.Add($"taking the layer off did not restore the state: {Difference(e1, e4)}");
        var r = new ContentRoundTrip(problems.Count == 0, fields, a1, reset, sw.Elapsed.TotalMilliseconds, problems);
        Log(r.Ok ? LogLevel.Info : LogLevel.Warn, $"round trip: {r}");
        return r;
    }

    // What differs between two exported documents, by path ("templates.<name>.<field>: a -> b"), the first few; the text's first difference when
    // they do not parse. The "add" lists of definitions come first in a document and hide the field that changed.
    private static string Difference(string a, string b)
    {
        JsonNode? x, y;
        try
        {
            x = JsonNode.Parse(a);
            y = JsonNode.Parse(b);
        }
        catch (System.Text.Json.JsonException)
        {
            return FirstDifference(a, b);
        }
        var found = new List<string>();
        void Walk(string path, JsonNode? p, JsonNode? q)
        {
            if (found.Count >= 4) return;
            if (p is JsonObject po && q is JsonObject qo)
            {
                foreach (var k in po.Select(e => e.Key).Union(qo.Select(e => e.Key)))
                    if (k != "@fields") Walk(path.Length == 0 ? k : $"{path}.{k}", po[k], qo[k]);
                return;
            }
            if (p is JsonArray pa && q is JsonArray qa && pa.Count == qa.Count)
            {
                for (var i = 0; i < pa.Count; i++) Walk($"{path}[{i}]", pa[i], qa[i]);
                return;
            }
            var ps = p?.ToJsonString() ?? "(none)";
            var qs = q?.ToJsonString() ?? "(none)";
            if (ps != qs) found.Add($"{path}: {Cut(ps)} -> {Cut(qs)}");
        }
        static string Cut(string s) => s.Length <= 160 ? s : s[..160] + "...";
        Walk("", x, y);
        return found.Count > 0 ? string.Join("; ", found) : FirstDifference(a, b);
    }

    // A document without its definitions' "add" lists and its options: the changes it holds.
    private static string Unadded(string doc)
    {
        if (JsonNode.Parse(doc) is not JsonObject o) return doc;
        o.Remove("options");
        foreach (var (name, section) in o.ToList())
            if (section is JsonObject s)
            {
                s.Remove("add");
                if (s.Count == 0) o.Remove(name);
            }
        return o.ToJsonString();
    }

    private static string FirstDifference(string a, string b)
    {
        var n = Math.Min(a.Length, b.Length);
        var i = 0;
        while (i < n && a[i] == b[i]) i++;
        if (i == n && a.Length == b.Length) return "none";
        var from = Math.Max(0, i - 60);
        return $"at {i}: '{a.Substring(from, Math.Min(120, a.Length - from))}' vs '{b.Substring(from, Math.Min(120, b.Length - from))}'";
    }

    /// <summary>"Material_T1_Wood_Logs.maxStackSize 250 -> 1000": a few changes as this game had them before.</summary>
    private static string Examples(JsonObject doc, int max)
    {
        var list = new List<string>();
        foreach (var (section, kind) in new[] { ("items", ContentKind.Items), ("recipes", ContentKind.Recipes) })
        {
            if (doc[section] is not JsonObject s) continue;
            foreach (var (id, body) in s)
            {
                if (list.Count >= max) break;
                if (body is not JsonObject b) continue;
                var name = b["@name"]?.GetValue<string>() ?? id;
                foreach (var (key, val) in b)
                {
                    if (key.StartsWith('@') || list.Count >= max) continue;
                    if (val is JsonObject) continue;  // a reset
                    try
                    {
                        var e = Content.Registry(kind).Get(id);
                        if (key is "inputs" or "outputs")
                        {
                            var stacks = key == "inputs" ? e?.Inputs : e?.Outputs;
                            list.Add($"{name}.{key} = {(stacks is null ? "?" : string.Join(", ", stacks.Select(x => x.ToString())))}");
                        }
                        else
                            list.Add($"{name}.{key} {e?.Original(key)} -> {e?.Get(key)}");
                    }
                    catch (Exception)
                    {
                        list.Add($"{name}.{key} = {val?.ToJsonString()}");
                    }
                }
            }
        }
        return string.Join(", ", list);
    }

    private static void OnLeave(string? why)
    {
        lock (Lock)
        {
            _incoming = null;
            _pending = null;
            // the next connection's state is answered again. Leave() runs at the next Kiln tick, which a game held at its
            // next join may not get first (no menu tick without kiln.ui.tick, nor while a world is there)
            _heldAnswered = 0;
        }
        if (Game.IsTickThread) Leave(why);
        else _leavePending = true;
    }

    private static void Leave(string? why)
    {
        _leavePending = false;
        var had = _appliedState != 0;
        _appliedState = 0;
        _appliedGeneration = 0;
        var n = 0;
        if (had && _owner != 0 && Content.T != null)
        {
            n = (int)Content.T->RevertOwner(_owner);
            if (ContentNative.Has(Content.T, 4) && Content.T->UndefineOwner != null) Content.T->UndefineOwner(_owner);
        }
        SyncPrograms.Release(_owner);
        if (had)
        {
            Log(LogLevel.Info, $"content sync: {why ?? "a new server"}: {n} field(s) back to this game's own values");
            if (Events.HasSubscribers<ContentSyncedEvent>()) Events.Post(new ContentSyncedEvent(true, n, 0, [], _status.StateId));
        }
        _status = new ContentSyncStatus(ContentSyncState.Off, 0, 0, 0, 0, 0, 0, 0, null, why ?? "");
    }

    // ------------------------------------------------------------------------------------------------- native ----

    /// <summary>kiln.content's export_changes (null on failure; <paramref name="rc"/> = the native result).</summary>
    internal static string? ExportJson(uint kinds, out int rc)
    {
        var t = Content.Require("ContentSync.Export");
        if (t->Header.Version < 2 || t->ExportChanges == null)
        {
            rc = ContentNative.Unavailable;
            return null;
        }
        var cap = 1 << 20;
        for (var attempt = 0; attempt < 3; attempt++)
        {
            var buf = (byte*)NativeMemory.Alloc((nuint)cap);
            try
            {
                nuint len = 0;
                rc = t->ExportChanges(kinds, buf, (nuint)cap, &len);
                if (rc == ContentNative.Ok) return Encoding.UTF8.GetString(buf, (int)len);
                if (rc != ContentNative.BufferTooSmall) return null;
                cap = (int)Math.Min(len + 1024, int.MaxValue / 2);
            }
            finally
            {
                NativeMemory.Free(buf);
            }
        }
        rc = ContentNative.BufferTooSmall;
        return null;
    }

    internal static AckMsg MakeApplyAck(uint stateId, int rc, int applied, int reset, IReadOnlyList<string> errors, uint millis) =>
        new(stateId, ApplySucceeded(rc, errors) ? (byte)0 : (byte)1, (uint)applied, (uint)reset, (uint)errors.Count, millis,
            string.Join("; ", errors.Take(4)));

    internal static (int Applied, List<string> Errors, int Rc) ApplyJson(string json, string source)
    {
        var t = Content.Require("ContentSync.Apply");
        const int cap = 1 << 16;
        var buf = new byte[cap];
        nuint len = 0;
        int rc;
        using (var j = new ContentNative.Utf8(json))
        using (var s = new ContentNative.Utf8(source))
            fixed (byte* b = buf)
                rc = t->ApplyJson(_owner, j.Str, s.Str, b, cap, &len);
        var (applied, errors) = ReadApplyReport(Encoding.UTF8.GetString(buf, 0, (int)Math.Min(len, (nuint)(cap - 1))));
        if (rc != ContentNative.Ok && errors.Count == 0) errors.Add(ContentNative.Message(rc));
        return (applied, errors, rc);
    }

    internal static (int Applied, List<string> Errors) ReadApplyReport(string text)
    {
        var errors = new List<string>();
        var applied = 0;
        try
        {
            using var doc = JsonDocument.Parse(text);
            if (doc.RootElement.ValueKind != JsonValueKind.Object ||
                !doc.RootElement.TryGetProperty("applied", out var count) || count.ValueKind != JsonValueKind.Number || !count.TryGetInt32(out applied) || applied < 0 ||
                !doc.RootElement.TryGetProperty("errors", out var entries) || entries.ValueKind != JsonValueKind.Array)
                throw new JsonException("invalid content apply report");
            foreach (var entry in entries.EnumerateArray())
            {
                if (entry.ValueKind != JsonValueKind.String) throw new JsonException("invalid content error entry");
                errors.Add(entry.GetString() ?? "");
            }
        }
        catch (JsonException)
        {
            applied = 0;
            errors.Add("The native content apply report is incomplete or invalid; update Kiln and rejoin.");
        }
        return (applied, errors);
    }

    // ------------------------------------------------------------------------------------------------- codec -----

    internal static uint Fnv(ReadOnlySpan<byte> data)
    {
        var h = 2166136261u;
        foreach (var b in data) h = (h ^ b) * 16777619u;
        return h == 0 ? 1 : h;
    }

    internal static byte[] Compress(byte[] raw)
    {
        using var ms = new MemoryStream(raw.Length / 4 + 64);
        using (var z = new DeflateStream(ms, CompressionLevel.Optimal, leaveOpen: true)) z.Write(raw);
        return ms.ToArray();
    }

    internal static byte[] Decompress(byte[] packed, int expected)
    {
        using var z = new DeflateStream(new MemoryStream(packed), CompressionMode.Decompress);
        var out_ = new byte[expected];
        var off = 0;
        while (off < expected)
        {
            var n = z.Read(out_, off, expected - off);
            if (n == 0) break;
            off += n;
        }
        if (off < expected) return out_.AsSpan(0, off).ToArray();
        if (z.ReadByte() >= 0) throw new InvalidDataException("more data than announced");
        return out_;
    }
}

/// <summary>Machine-local fields of clones, by section, clone GUID and path (<see cref="ContentSync.MarkMachineLocal"/>). Any thread.</summary>
internal sealed class MachineLocalFields
{
    private readonly object _gate = new();
    private readonly HashSet<(string Section, Guid Clone, string Path)> _set = new();

    public void Add(string section, Guid clone, string path)
    {
        lock (_gate) _set.Add((section, clone, path));
    }

    public bool IsEmpty
    {
        get { lock (_gate) return _set.Count == 0; }
    }

    /// <summary>The export's entry key is the clone's GUID (<c>"&lt;guid&gt;#k"</c> for a sub-sequence of it).</summary>
    public bool Has(string section, string entry, string path)
    {
        var hash = entry.IndexOf('#');
        if (!Guid.TryParse(hash < 0 ? entry : entry[..hash], out var g)) return false;
        lock (_gate) return _set.Contains((section, g, path));
    }

    public MachineLocalFields Copy()
    {
        var c = new MachineLocalFields();
        lock (_gate) c._set.UnionWith(_set);
        return c;
    }
}
