// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>The save bridge's narrow internal face. The engine must return the frozen records and witness of the SAME
/// character capture, never a later live inventory read. Until that capture is ready Freeze returns null.</summary>
internal interface IPlayerStoragePersistence
{
    StorageFreeze? Freeze(CharacterKey character, EntityId player, uint generation);
    bool AutomaticRegistration => false;
    string? WitnessOf(EntityId player);
    /// <summary>The character in game on this entity now (null: none, or not known).</summary>
    CharacterKey? CharacterOf(EntityId player) => null;
    bool ExtraMainPages(EntityId player) => false;
    IReadOnlyDictionary<string, int> Registrations { get; }
    void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision);
    void Park(EntityId player, JsonObject parked);
    void Lock(EntityId player) { }
    IReadOnlyDictionary<string, EntityId> Holders(EntityId player) => new Dictionary<string, EntityId>();
    IReadOnlyList<StorageInstanceHolder> InstanceHolders(EntityId player) => Holders(player).Select(h => new StorageInstanceHolder(h.Key, h.Value)).ToArray();
}

/// <summary>A storage restore that cannot run in this service step (the writer is busy): try again at the next one.</summary>
internal sealed class StorageRestoreDeferredException(string message) : InvalidOperationException(message);

internal readonly record struct StorageInstanceHolder(string Key, EntityId Holder, bool Temporary = false);
internal sealed record StorageFreeze(string Witness, JsonObject Storages, JsonObject? Parked = null, JsonArray? Quarantine = null);

/// <summary>One world's server copies and pending carried updates. Pending generations remain on a failed world write;
/// only a matching world-written event publishes the carried copy.</summary>
internal sealed class PlayerStorageStore(WorldStore owner)
{
    internal const string Prefix = "player-storage/", Mod = "kiln-items", Key = "storage";
    internal IPlayerStoragePersistence? Native;
    private readonly Dictionary<CharacterKey, Visit> _visits = [];
    private readonly Dictionary<uint, Dictionary<CharacterKey, string>> _pending = [];
    private readonly Dictionary<CharacterKey, string> _committed = [];
    private readonly Dictionary<CharacterKey, string?> _committedServer = [];
    private readonly Dictionary<uint, Dictionary<CharacterKey, string>> _pendingServer = [];
    private readonly Dictionary<uint, (EntityId Player, int Offset)> _instanceOwners = [];
    private readonly HashSet<uint> _ordinaryHolders = [];
    private readonly Dictionary<uint, SaveWriter.StageJob> _stages = [];
    private readonly Dictionary<uint, string> _carriedBlocked = [];
    private readonly System.Collections.Concurrent.ConcurrentQueue<(uint Generation, bool Success, SaveWriter.StageJob Job)> _outcomes = new();
    /// <summary>Generations whose newest staging could not be written (its tries failed): a world write of one is a failure.</summary>
    private readonly HashSet<uint> _unwritten = [];
    /// <summary>World writes that completed before their staging was on disk (since 0.2.0-dev.31.5): a delay, not a failure,
    /// as long as the writer catches up (DurabilityLag). Tests give it their own clock.</summary>
    internal DurabilityLag Lag { get; set; } = new(DurabilityLag.SystemClock);
    internal string? Failure { get; private set; }
    internal void Failed(string reason)
    {
        Failure = reason;
        SavesRuntime.Log(LogLevel.Error, "player-storage: persistence degraded: " + reason + "; pairs kept in memory, next save retries");
        ReportCapability();
    }
    internal void ReportCapability()
    {
        try
        {
            Capabilities.Report("saves.player_storage", Failure is null ? Native is null ? CapabilityState.Unavailable : CapabilityState.Available : CapabilityState.Unavailable,
                Failure is not null ? CapabilityReason.Faulted : Native is null ? CapabilityReason.ServiceMissing : CapabilityReason.None,
                Failure ?? (Native is null ? "items.storage.persist is unavailable" : ""));
        }
        catch (Exception e) when (e is ArgumentException or InvalidOperationException)
        {
            SavesRuntime.Log(LogLevel.Warn, "player-storage: capability report refused: " + e.Message);
        }
    }
    internal void Track(SaveWriter.StageJob job)
    {
        if (!_pending.ContainsKey(job.Gen)) return;
        _stages[job.Gen] = job;
        _unwritten.Remove(job.Gen);  // an earlier staging of it that failed is not the one its world write waits for now
        job.Finished = success => _outcomes.Enqueue((job.Gen, success, job));
        foreach (var older in _stages.Keys.OrderByDescending(x => x).Skip(2).ToArray()) _stages.Remove(older);
    }
    internal void PollWrites()
    {
        while (_outcomes.TryDequeue(out var result))
        {
            Say(Lag.Landed(result.Generation, result.Success, result.Job));
            if (result.Success) continue;
            // the generation's world write is a failure when the staging that failed is the one it waits for (the newest)
            if (!_stages.TryGetValue(result.Generation, out var newest) || ReferenceEquals(newest, result.Job))
            {
                _unwritten.Add(result.Generation);
                while (_unwritten.Count > 16) _unwritten.Remove(_unwritten.Min());
            }
            Failed($"pending generation {result.Generation} could not be written");
        }
        foreach (var (generation, age) in Lag.Stuck())
            Failed($"world generation {generation} was written {age.TotalSeconds:F0} s ago and Kiln.Saves' writer has still not put its storage on disk");
        Say(Lag.Summary());
    }

    private static void Say((LogLevel Level, string Text)? line)
    {
        if (line is var (level, text)) SavesRuntime.Log(level, text);
    }
    private sealed class Visit(EntityId player, PlayerStorageDocument document)
    {
        internal readonly EntityId Player = player;
        internal PlayerStorageDocument Document = document;
        internal bool Restored, Withheld, Local;
        internal string HeldReason = "changed elsewhere since this server's copy";
        internal CharacterKey Character;
        internal Dictionary<string, int> Bands = new(StringComparer.Ordinal);
        internal string Registration = "";
        internal string? Notice;
        internal string? Name;
        /// <summary>How an admin names this player in /playerstorage: never the entity id, which the next player in the
        /// slot gets. The name (quoted when it has spaces or quotes), else the character id.</summary>
        internal string Token => Name is { Length: > 0 } n
            ? n.Any(c => char.IsWhiteSpace(c) || c is '"' or '\'' or '\\') ? "\"" + n.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"" : n
            : Character.Id.ToString();
    }

    internal void Clear()
    {
        _visits.Clear(); _pending.Clear(); _committed.Clear(); _committedServer.Clear(); _pendingServer.Clear(); _stages.Clear(); _carriedBlocked.Clear();
        _instanceOwners.Clear(); _ordinaryHolders.Clear();
        while (_outcomes.TryDequeue(out _)) { }
        _unwritten.Clear();
        Lag.Clear();
        Failure = null;
    }
    internal string? Carried(CharacterKey character) => _committed.GetValueOrDefault(character);

    /// <summary>The /playerstorage listing of the character a player joined with, or null when this world has none.</summary>
    internal IReadOnlyList<string>? Listing(EntityId player)
    {
        if (_visits.FirstOrDefault(v => v.Value.Player == player) is not { Value: { } visit, Key: var character }) return null;
        var lines = new List<string>();
        if (visit.Document.ReadOnly)
        {
            lines.Add($"player {visit.Name ?? "(name unknown)"}, character {character}: storage saved by a newer Kiln, kept whole and read-only here");
            return lines;
        }
        var current = visit.Document.Data["current"]!;
        lines.Add($"player {visit.Name ?? "(name unknown)"}, character {character}: revision {visit.Document.Revision}{(visit.Withheld ? $", held: {visit.HeldReason}" : "")}");
        foreach (var (key, bag) in current["storages"]!.AsObject())
            lines.Add($"  {key}: {bag?["records"]?.AsArray().Count ?? 0} stack(s) in storage");
        foreach (var (key, bag) in current["parked"]!.AsObject())
        {
            var reason = bag?["parkReason"]?.GetValue<string>() ?? PlayerStorageDocument.ParkReason(bag!.AsObject()) switch
            {
                2 => "more items than slots", 3 => "the storage's filter refuses them", _ => "its mod is not loaded",
            };
            lines.Add($"  {key}: {bag?["records"]?.AsArray().Count ?? 0} stack(s) parked ({reason}); release: /{PlayerStorageCommand.Name} release {visit.Token} {key}");
        }
        if (visit.Document.Data["quarantine"]!.AsArray().Count is var quarantined and > 0)
            lines.Add($"  {quarantined} quarantined record(s): bytes kept, not releasable by this command");
        return lines;
    }

    /// <summary>A player of this world's storage visits by name (the /playerstorage token) or character id. One account's
    /// characters share its platform name, and a visit stays after its character left: the visit whose character is in
    /// game on its entity now wins, else the first match.</summary>
    internal EntityId? FindPlayer(string text)
    {
        EntityId? first = null;
        foreach (var (character, visit) in _visits)
            if ((visit.Name is { Length: > 0 } n && string.Equals(n, text, StringComparison.OrdinalIgnoreCase))
                || (Guid.TryParse(text, out var id) && id == character.Id))
            {
                if (Native?.CharacterOf(visit.Player) == character) return visit.Player;
                first ??= visit.Player;
            }
        return first;
    }

    /// <summary>Whether the player's storage visit is still the character in game on that entity (entity ids are reused by
    /// the next player in the slot).</summary>
    internal bool InGameAs(EntityId player)
    {
        if (Native is null || _visits.FirstOrDefault(v => v.Value.Player == player) is not { Value: not null, Key: var character }) return false;
        return Native.CharacterOf(player) is { } now ? now == character : Native.WitnessOf(player) is not null;
    }

    /// <summary>Whether the character a player joined with has records parked under the key (a hold included).</summary>
    internal bool IsParked(EntityId player, string key) =>
        _visits.Values.FirstOrDefault(v => v.Player == player) is { Document.ReadOnly: false } visit
        && visit.Document.Data["current"]?["parked"]?[key]?["records"] is System.Text.Json.Nodes.JsonArray { Count: > 0 };
    internal string? Notice(CharacterKey character) => _visits.GetValueOrDefault(character)?.Notice;

    internal PlayerStorageDocument? Read(CharacterKey character)
    {
        var json = owner.FindBundle(Prefix + character.Text)?.Keys.GetValueOrDefault(Key)?.Json;
        if (json is null) return null;
        try { return PlayerStorageDocument.Read(json, character.Id, PlayerStorageDocument.ServerMaxBytes); }
        catch (InvalidDataException e)
        {
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: server copy for {character} is unusable and kept: {e.Message}");
            // Preserve the whole unparseable value rather than replacing it with an empty document.
            return new PlayerStorageDocument(new JsonObject(), true, json);
        }
    }

    internal StorageChoice? Join(CharacterKey character, EntityId player, string witness, string? clientJson, bool trustClient, bool local = false,
        string? name = null)
    {
        owner.EnsureOpen();
        if (character.IsNone || character.IsAccountOnly || owner.LoadDeferred) return null;
        _committed.Remove(character); // A travelling offer must never be overwritten by this world's old carried update.
        var servers = new List<PlayerStorageDocument>();
        if (Read(character) is { } stored) servers.Add(stored);
        if (_committedServer.TryGetValue(character, out var durable) && durable is not null)
            servers.Add(PlayerStorageDocument.Read(durable, character.Id, PlayerStorageDocument.ServerMaxBytes));
        foreach (var json in owner.PendingStorageValues(Prefix + character.Text))
        {
            try { servers.Add(PlayerStorageDocument.Read(json, character.Id, PlayerStorageDocument.ServerMaxBytes)); }
            catch (InvalidDataException e) { SavesRuntime.Log(LogLevel.Warn, $"player-storage: pending copy for {character} unusable: {e.Message}"); }
        }
        var server = servers.FirstOrDefault(d => d.ReadOnly) ?? servers
            .Select(d => (Document: d, Choice: PlayerStorageDocument.Choose(witness, d, null)))
            .OrderByDescending(x => x.Choice?.Paired ?? false).ThenByDescending(x => x.Choice?.Revision ?? -1).FirstOrDefault().Document;
        PlayerStorageDocument? client = null;
        if (clientJson is not null)
        {
            try { client = PlayerStorageDocument.Read(clientJson, character.Id); }
            catch (InvalidDataException e) { SavesRuntime.Log(LogLevel.Warn, $"player-storage: client copy for {character} refused and kept by client: {e.Message}"); }
        }
        if (server?.ReadOnly == true || client?.ReadOnly == true)
        {
            _visits[character] = new Visit(player, client?.ReadOnly == true ? client : server!);
            _committed.Remove(character);
            Native?.Lock(player);
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: unsupported document parked whole, storage read-only");
            return null;
        }
        var choice = PlayerStorageDocument.Choose(witness, server, client, trustClient);
        var selected = choice?.Source == "client" ? client : server;
        var document = selected is null ? PlayerStorageDocument.Empty(character.Id)
            : PlayerStorageDocument.Read(selected.Write(32, PlayerStorageDocument.ServerMaxBytes), character.Id, PlayerStorageDocument.ServerMaxBytes);
        if (choice is not null)
        {
            var previous = (JsonObject)document.Data["current"]!.DeepClone();
            var promoted = (JsonObject)choice.Pair.DeepClone();
            promoted.Remove("revision");
            if (JsonNode.DeepEquals(previous["storages"], promoted["storages"]) && JsonNode.DeepEquals(previous["parked"], promoted["parked"]))
            {
                // Share the payload without discarding recovery information: an older character may need any alias.
                var selectedWitness = promoted["witness"]!.GetValue<string>();
                var aliases = new JsonArray();
                foreach (var pair in PlayerStorageDocument.Pairs(previous, document.Revision).Concat(PlayerStorageDocument.Pairs(choice.Pair, choice.Revision))
                    .Where(p => p.Witness != selectedWitness).GroupBy(p => p.Witness).Select(g => g.OrderByDescending(p => p.Revision).First()))
                    aliases.Add(new JsonObject { ["witness"] = pair.Witness, ["revision"] = pair.Revision, ["worldRev"] = pair.WorldRev });
                if (aliases.Count > 0) promoted["pairs"] = aliases;
                else promoted.Remove("pairs");
            }
            else
            {
                // The chosen historical payload differs: keep the old current and the chosen copy's history.
                previous["revision"] = document.Revision;
                document.Data["history"]!.AsArray().Insert(0, previous);
                promoted.Remove("pairs");
            }
            document.Data["current"] = promoted;
            document.Data["revision"] = Math.Max(servers.Select(d => d.Revision).DefaultIfEmpty(0).Max(), trustClient ? client?.Revision ?? 0 : 0);
            SavesRuntime.Log(choice.Paired ? LogLevel.Info : LogLevel.Warn,
                $"player-storage: {character}: {(choice.Paired ? "paired" : "unpaired: newest revision; character changed outside Kiln")}, {choice.Source}, revision {choice.Revision}");
        }
        foreach (var copy in servers) document.MergeReturns(copy);
        document.MergeReturns(client); // Evidence can withhold an untrusted copy, never add its item records or counter.
        foreach (var old in _visits.Where(v => v.Key != character && v.Value.Player == player).Select(v => v.Key).ToArray()) _visits.Remove(old);
        var visit = _visits[character] = new Visit(player, document) { Local = local, Character = character, Name = name,
            Notice = choice is { Paired: false } ? "Storage restored without a matching character save (newest revision). See kiln.log for the source." : null };
        var returned = choice is { Paired: false, Source: "server" }
            && ((client is not null && client.LatestReturnRevision > 0 && !client.AcknowledgesAfterReturn(choice))
                || (client is null && Native?.ExtraMainPages(player) == true));
        if (returned)
        {
            visit.Withheld = true;
            visit.HeldReason = "returned outside Kiln";
        }
        else if (!trustClient && choice is { Paired: false } && client is { ReadOnly: false } && client.Revision > choice.Revision)
            visit.Withheld = true;
        if (visit.Withheld)
            visit.Notice = $"Your stored items on this server are held because your character changed outside Kiln. An admin can review /playerstorage list {visit.Token} and release them with /playerstorage release {visit.Token} <key>.";
        Restore(visit, choice?.Paired ?? true, choice?.Source ?? "new");
        if (visit.Withheld)
            foreach (var (key, bag) in visit.Document.Data["current"]!["parked"]!.AsObject())
                SavesRuntime.Log(LogLevel.Warn, $"player-storage: player {name ?? "(name unknown)"}, character {character}, key {key}: {bag!["records"]!.AsArray().Count} stacks held ({bag["parkReason"]}); admin release: /playerstorage release {visit.Token} {key}");
        return choice;
    }

    /// <summary>A trusted player's copy that came after the join decided without it: never swapped in, but its pairs ride
    /// on as the oldest history, since this server's carried updates replace the player's sidecar value from now on.</summary>
    internal void KeepLate(CharacterKey character, string clientJson)
    {
        if (!_visits.TryGetValue(character, out var visit) || visit.Document.ReadOnly) return;
        PlayerStorageDocument late;
        try { late = PlayerStorageDocument.Read(clientJson, character.Id); }
        catch (InvalidDataException e)
        {
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: late client copy unusable, the player keeps it: {e.Message}");
            return;
        }
        var added = visit.Document.KeepAsHistory(late);
        SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: client copy came after the join timeout; not restored, {added} pair(s) kept as history");
    }

    private void Restore(Visit visit, bool paired, string source)
    {
        if (Native is null || visit.Document.ReadOnly) return;
        var current = visit.Document.Data["current"]!.AsObject();
        EnsureBands(visit);
        var (active, parked) = PlayerStorageDocument.Partition(current["storages"]!.AsObject(), current["parked"]!.AsObject(),
            visit.Withheld ? new Dictionary<string, int>() : Native.Registrations, visit.Document.Data["quarantine"]!.AsArray(), visit.Player.Value);
        if (visit.Withheld)
            foreach (var bag in parked.Select(x => x.Value).OfType<JsonObject>())
                if (PlayerStorageDocument.ParkReason(bag) <= (visit.HeldReason == "returned outside Kiln" ? 19 : 17))
                {
                    bag["parkReason"] = visit.HeldReason;
                    bag["nativeParkReason"] = visit.HeldReason == "returned outside Kiln" ? 19 : 17;
                }
        Native.Restore(visit.Player, active, paired, source, visit.Document.Revision);
        Native.Park(visit.Player, parked);
        current["storages"] = active;
        current["parked"] = parked;
        visit.Restored = true;
        visit.Registration = string.Join(";", Native.Registrations.OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Value}"));
    }

    internal void Stage(uint generation)
    {
        if (Native is null || owner.LoadDeferred) return;
        _carriedBlocked.Remove(generation);
        foreach (var (character, visit) in _visits)
        {
            if (visit.Document.ReadOnly) continue;
            try { StageVisit(generation, character, visit); }
            catch (Exception e) when (e is InvalidDataException or InvalidOperationException or FormatException or OverflowException
                or System.Text.Json.JsonException or ArgumentException or KeyNotFoundException)
            {
                // One character's storage must never stop the world's mod data from being staged. Its previous server
                // and carried copies stay as they were; the next save tries again, and this save's commit says so.
                _carriedBlocked[generation] = $"character {character}: storage not staged with generation {generation}: {e.Message}";
                Failed(_carriedBlocked[generation]);
            }
        }
    }

    private void StageVisit(uint generation, CharacterKey character, Visit visit)
    {
        var native = Native!;
        var registration = string.Join(";", native.Registrations.OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Value}"));
        if (!visit.Restored || (!native.AutomaticRegistration && visit.Registration != registration)) Restore(visit, true, "registration");
        if (native.Freeze(character, visit.Player, generation) is not { } frozen) return;
        var current = visit.Document.Data["current"]!.AsObject();
        var next = PlayerStorageDocument.Read(visit.Document.Write(32, PlayerStorageDocument.ServerMaxBytes), character.Id, PlayerStorageDocument.ServerMaxBytes);
        var frozenStorages = (JsonObject)frozen.Storages.DeepClone();
        var frozenParked = frozen.Parked is null ? current["parked"]!.AsObject() : PreserveParked(current["parked"]!.AsObject(), frozen.Parked);
        next.Capture(frozen.Witness, generation, frozenStorages, frozenParked);
        if (frozen.Quarantine is not null)
            foreach (var entry in frozen.Quarantine)
                if (!next.Data["quarantine"]!.AsArray().Any(old => JsonNode.DeepEquals(old, entry))) next.Data["quarantine"]!.AsArray().Add(entry?.DeepClone());
        next.Data["current"]!["source"] = visit.Local && current["source"]?.GetValue<string>() != "server" ? "local" : "server";
        var serverJson = next.Write(32, PlayerStorageDocument.ServerMaxBytes);
        string? clientJson = null;
        try { clientJson = next.Write(8); }
        catch (InvalidDataException e)
        {
            _carriedBlocked[generation] = $"character {character}: carried storage unchanged: {e.Message}";
            Failed(_carriedBlocked[generation]);
        }
        var bundle = owner.GetBundleRaw(Prefix + character.Text, Mod);
        // Seed once, including a missing durable copy; a later failed stage must never seed itself as committed.
        _committedServer.TryAdd(character, bundle.Keys.GetValueOrDefault(Key)?.Json);
        bundle.Keys[Key] = new WorldStore.Slot { Json = serverJson };
        if (!_pendingServer.TryGetValue(generation, out var serverPending)) _pendingServer[generation] = serverPending = [];
        serverPending[character] = serverJson;
        visit.Document = next;
        EnsureBands(visit);
        if (!_pending.TryGetValue(generation, out var pending)) _pending[generation] = pending = [];
        if (clientJson is not null) pending[character] = clientJson;
        else pending.Remove(character);
        foreach (var older in _pending.Keys.OrderByDescending(g => g).Skip(2).ToArray())
        { _pending.Remove(older); _pendingServer.Remove(older); _carriedBlocked.Remove(older); }
    }

    private static JsonObject PreserveParked(JsonObject previous, JsonObject captured)
    {
        var kept = (JsonObject)captured.DeepClone();
        foreach (var (key, value) in captured)
        {
            if (value is not JsonObject now || previous[key] is not JsonObject old) continue;
            var before = old["records"]!.AsArray();
            var after = now["records"]!.AsArray();
            static string Bytes(JsonNode? record) => record?["b64"]?.GetValue<string>() ?? "";
            var same = before.Select(Bytes).SequenceEqual(after.Select(Bytes));
            var merged = (JsonObject)old.DeepClone();
            merged["slots"] = now["slots"]!.DeepClone();
            if (!same) merged["records"] = new JsonArray(after.Select(r => before.FirstOrDefault(o => Bytes(o) == Bytes(r))?.DeepClone() ?? r!.DeepClone()).ToArray());
            if (now["parkReason"] is { } held) merged["parkReason"] = held.DeepClone();
            else merged.Remove("parkReason");
            if (now["nativeParkReason"] is { } reason) merged["nativeParkReason"] = reason.DeepClone();
            else merged.Remove("nativeParkReason");
            kept[key] = merged;
        }
        foreach (var (key, value) in previous)
            if (!kept.ContainsKey(key) && value?["records"] is JsonArray { Count: 0 }) kept[key] = value.DeepClone();
        return kept;
    }

    private void EnsureBands(Visit visit)
    {
        var current = visit.Document.Data["current"]!;
        var keys = current["storages"]!.AsObject().Select(x => x.Key).Union(current["parked"]!.AsObject().Select(x => x.Key))
            .Union(Native?.Registrations.Keys ?? []).Order(StringComparer.Ordinal).ToArray();
        var bundle = owner.GetBundleRaw(Prefix + visit.Character.Text, Mod);
        var old = visit.Bands;
        if (old.Count == 0 && bundle.Keys.GetValueOrDefault("instanceBands")?.Json is { } json)
        {
            try { old = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json) ?? old; }
            catch (System.Text.Json.JsonException) { /* A damaged map never authorizes a guessed item match. */ }
        }
        var assigned = new Dictionary<string, int>(StringComparer.Ordinal);
        var used = new HashSet<int>();
        foreach (var key in keys)
            if (old.TryGetValue(key, out var band) && band is >= 0 and < MaxBands && used.Add(band)) assigned[key] = band;
        foreach (var key in keys)
            if (!assigned.ContainsKey(key))
            {
                var band = Enumerable.Range(0, MaxBands).FirstOrDefault(b => !used.Contains(b), -1);
                if (band < 0)
                {
                    // Not reachable within the document (32 keys) and registration (32 pages) limits; never a reason
                    // to stop a join or a save. The key's items are then kept with their holder, not the character.
                    SavesRuntime.Log(LogLevel.Warn, $"player-storage: {visit.Character}: no InstanceData band left for {key}");
                    continue;
                }
                used.Add(band);
                assigned[key] = band;
            }
        visit.Bands = assigned;
        bundle.Keys["instanceBands"] = new WorldStore.Slot { Json = System.Text.Json.JsonSerializer.Serialize(assigned) };
    }

    internal const int BandBase = 1024, BandWidth = 128, MaxBands = 64;

    /// <summary>Whether a character's InstanceData record in a storage band belongs to an item whose game record still
    /// waits in the storage document: parked (its mod is off, a hold) or quarantined. Such metadata must not age out
    /// with InstanceStore's missed sessions while the record it belongs to is kept.</summary>
    internal bool HoldsParked(CharacterKey character, int slot, ItemId item)
    {
        if (slot < BandBase || character.IsNone || character.IsAccountOnly) return false;
        var visit = _visits.GetValueOrDefault(character);
        var document = visit?.Document ?? Read(character);
        if (document is null) return false;
        if (document.ReadOnly) return true; // parked whole by an unsupported version: everything in it waits
        IReadOnlyDictionary<string, int>? bands = visit is { Bands.Count: > 0 } ? visit.Bands : null;
        if (bands is null && owner.FindBundle(Prefix + character.Text)?.Keys.GetValueOrDefault("instanceBands")?.Json is { } json)
        {
            try { bands = System.Text.Json.JsonSerializer.Deserialize<Dictionary<string, int>>(json); }
            catch (System.Text.Json.JsonException) { bands = null; }
        }
        var band = (slot - BandBase) / BandWidth;
        // Without a usable band map any parked key may be the item's: keep rather than lose.
        bool KeyOf(string key) => bands is null || !bands.ContainsKey(key) || bands[key] == band;
        static uint ItemOf(string? b64)
        {
            try { return b64 is null ? 0 : Convert.FromBase64String(b64) is { Length: >= 5 } b ? System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(1)) : 0; }
            catch (FormatException) { return 0; }
        }
        if (document.Data["current"]?["parked"] is JsonObject parked)
            foreach (var (key, bag) in parked)
                if (KeyOf(key) && bag?["records"] is JsonArray records && records.OfType<JsonObject>().Any(r => ItemOf(r["b64"]?.GetValue<string>()) == item.Value))
                    return true;
        return document.Data["quarantine"] is JsonArray quarantine && quarantine.OfType<JsonObject>().Any(q =>
            q["storage"]?.GetValueKind() == System.Text.Json.JsonValueKind.String && KeyOf(q["storage"]!.GetValue<string>())
            && q["record"]?.GetValueKind() == System.Text.Json.JsonValueKind.String && ItemOf(q["record"]!.GetValue<string>()) == item.Value);
    }

    private IEnumerable<(Visit Visit, string Key, EntityId Holder, int Offset)> InstanceHolders()
    {
        if (Native is null) yield break;
        foreach (var visit in _visits.Values.Where(v => v.Restored && !v.Document.ReadOnly))
        {
            foreach (var row in Native.InstanceHolders(visit.Player))
                if (!row.Holder.IsNone && visit.Bands.TryGetValue(row.Key, out var band))
                {
                    var offset = BandBase + band * BandWidth + (row.Temporary ? 64 : 0);
                    _instanceOwners[row.Holder.Value] = (visit.Player, offset);
                    yield return (visit, row.Key, row.Holder, offset);
                }
        }
    }

    internal void Housekeeping(CharacterKey character, EntityId player, string key, EntityId holder, bool parked)
    {
        if (holder.IsNone || !_visits.TryGetValue(character, out var visit) || visit.Player != player || visit.Document.ReadOnly) return;
        EnsureBands(visit);
        if (!visit.Bands.TryGetValue(key, out var band)) return;
        var offset = BandBase + band * BandWidth;
        // A retired ordinary holder has handle0 too. Its cached band wins; only an actual temporary row adds 64.
        if (_instanceOwners.TryGetValue(holder.Value, out var cached) && cached.Player == player) offset = cached.Offset;
        else if (Native?.InstanceHolders(player).Any(row => row.Holder == holder && row.Temporary) == true) offset += 64;
        _instanceOwners[holder.Value] = (player, offset);
        _ordinaryHolders.Remove(holder.Value);
        if (parked) owner.Instances.StorageParked(character, player, holder, offset, key);
        else owner.Instances.StorageRestored(character, player);
    }

    internal InstancePlace NormalizeInstance(InstancePlace place)
    {
        if (_instanceOwners.TryGetValue(place.Holder.Value, out var cached)) return new(cached.Player, cached.Offset + place.FlatSlot);
        if (_ordinaryHolders.Contains(place.Holder.Value)) return place;
        foreach (var row in InstanceHolders())
            if (row.Holder == place.Holder) return new(row.Visit.Player, row.Offset + place.FlatSlot);
        _ordinaryHolders.Add(place.Holder.Value);
        return place;
    }

    internal IReadOnlyList<EntityId> InstanceParts(EntityId player) => new[] { player }
        .Concat(InstanceHolders().Where(h => h.Visit.Player == player).Select(h => h.Holder)).Distinct().ToArray();

    internal bool InstancesReady(EntityId player)
    {
        if (Native is null) return true;
        var visit = _visits.Values.FirstOrDefault(v => v.Player == player);
        if (visit is null) return Native.WitnessOf(player) is null;
        if (visit.Document.ReadOnly) return true; // no storage instantiated on this session
        var parked = visit.Document.Data["current"]!["parked"]!;
        return visit.Restored && Native.Registrations.Keys.Where(key => parked[key]?["parkReason"] is null).All(Native.Holders(player).ContainsKey);
    }

    internal void Committed(uint generation)
    {
        PollWrites();
        if (!_pending.TryGetValue(generation, out var pending)) return;
        if (!_stages.TryGetValue(generation, out var stage) || !Volatile.Read(ref stage.Written))
        {
            // Durability as before in both cases: the pairs stay in memory, nothing undurable is carried, the next save retries.
            if (stage is not null && !_unwritten.Contains(generation))
            {
                // The world write's bounded wait ran out while the writer still had this staging queued or being written: a
                // delay, not a failure (since 0.2.0-dev.31.5). Said once per world session and summed up; it becomes a failure
                // when its staging cannot be written (PollWrites) or is still not on disk after DurabilityLag.StuckAfter.
                Say(Lag.Late(generation, stage));
                return;
            }
            Failed($"world generation {generation} completed before storage became durable ({(stage is null ? "no staging of it was tracked" : "its staging could not be written")})");
            return;
        }
        _pending.Remove(generation);
        if (_pendingServer.Remove(generation, out var serverDocuments))
            foreach (var (character, json) in serverDocuments) _committedServer[character] = json;
        if (_carriedBlocked.Remove(generation, out var blocked)) Failed(blocked);
        else Failure = null;
        ReportCapability();
        foreach (var (character, json) in pending) _committed[character] = json;
        foreach (var older in _pending.Keys.Where(g => g < generation).ToArray())
        { _pending.Remove(older); _pendingServer.Remove(older); _carriedBlocked.Remove(older); }
        _unwritten.RemoveWhere(g => g <= generation);
    }
}
