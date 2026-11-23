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
    IReadOnlyDictionary<string, int> Registrations { get; }
    void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision);
    void Park(EntityId player, JsonObject parked);
    void Lock(EntityId player) { }
    IReadOnlyDictionary<string, EntityId> Holders(EntityId player) => new Dictionary<string, EntityId>();
    IReadOnlyList<StorageInstanceHolder> InstanceHolders(EntityId player) => Holders(player).Select(h => new StorageInstanceHolder(h.Key, h.Value)).ToArray();
}

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
    private readonly Dictionary<uint, (EntityId Player, int Offset)> _instanceOwners = [];
    private readonly HashSet<uint> _ordinaryHolders = [];
    private readonly Dictionary<uint, SaveWriter.StageJob> _stages = [];
    private readonly Dictionary<uint, string> _carriedBlocked = [];
    private readonly System.Collections.Concurrent.ConcurrentQueue<(uint Generation, bool Success)> _outcomes = new();
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
        job.Finished = success => _outcomes.Enqueue((job.Gen, success));
        foreach (var older in _stages.Keys.OrderByDescending(x => x).Skip(2).ToArray()) _stages.Remove(older);
    }
    internal void PollWrites()
    {
        while (_outcomes.TryDequeue(out var result))
            if (!result.Success) Failed($"pending generation {result.Generation} could not be written");
    }
    private sealed class Visit(EntityId player, PlayerStorageDocument document)
    {
        internal readonly EntityId Player = player;
        internal PlayerStorageDocument Document = document;
        internal bool Restored, Withheld, Local;
        internal CharacterKey Character;
        internal Dictionary<string, int> Bands = new(StringComparer.Ordinal);
        internal string Registration = "";
        internal string? Notice;
    }

    internal void Clear()
    {
        _visits.Clear(); _pending.Clear(); _committed.Clear(); _stages.Clear(); _carriedBlocked.Clear();
        _instanceOwners.Clear(); _ordinaryHolders.Clear();
        while (_outcomes.TryDequeue(out _)) { }
        Failure = null;
    }
    internal string? Carried(CharacterKey character) => _committed.GetValueOrDefault(character);
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

    internal StorageChoice? Join(CharacterKey character, EntityId player, string witness, string? clientJson, bool trustClient, bool local = false)
    {
        owner.EnsureOpen();
        if (character.IsNone || character.IsAccountOnly || owner.LoadDeferred) return null;
        _committed.Remove(character); // A travelling offer must never be overwritten by this world's old carried update.
        var servers = new List<PlayerStorageDocument>();
        if (Read(character) is { } stored) servers.Add(stored);
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
            // Keep the chosen copy's history, including its old current when a historical pair won.
            var previous = (JsonObject)document.Data["current"]!.DeepClone();
            previous["revision"] = document.Revision;
            document.Data["history"]!.AsArray().Insert(0, previous);
            document.Data["current"] = choice.Pair.DeepClone();
            document.Data["current"]!.AsObject().Remove("pairs");
            document.Data["revision"] = trustClient ? Math.Max(server?.Revision ?? 0, client?.Revision ?? 0) : server?.Revision ?? 0;
            // Pair aliases describe this content only, and must not be carried over from another selected candidate.
            SavesRuntime.Log(choice.Paired ? LogLevel.Info : LogLevel.Warn,
                $"player-storage: {character}: {(choice.Paired ? "paired" : "unpaired: newest revision; character changed outside Kiln")}, {choice.Source}, revision {choice.Revision}");
        }
        foreach (var old in _visits.Where(v => v.Key != character && v.Value.Player == player).Select(v => v.Key).ToArray()) _visits.Remove(old);
        var visit = _visits[character] = new Visit(player, document) { Local = local, Character = character,
            Notice = choice is { Paired: false } ? "Storage restored without a matching character save (newest revision). See kiln.log for the source." : null };
        if (!trustClient && choice is { Paired: false } && client is { ReadOnly: false } && client.Revision > choice.Revision)
        {
            visit.Withheld = true;
            visit.Notice = "Storage changed elsewhere since this server's copy. This server's old copy is parked; ask an admin to review or return it.";
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: changed elsewhere since this server's copy; server storage parked, client cannot add storage here");
        }
        Restore(visit, choice?.Paired ?? true, choice?.Source ?? "new");
        return choice;
    }

    private void Restore(Visit visit, bool paired, string source)
    {
        if (Native is null || visit.Document.ReadOnly) return;
        var current = visit.Document.Data["current"]!.AsObject();
        EnsureBands(visit);
        var (active, parked) = PlayerStorageDocument.Partition(current["storages"]!.AsObject(), current["parked"]!.AsObject(),
            visit.Withheld ? new Dictionary<string, int>() : Native.Registrations);
        if (visit.Withheld)
            foreach (var bag in parked.Select(x => x.Value).OfType<JsonObject>()) bag["parkReason"] = "changed elsewhere since this server's copy";
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
            var registration = string.Join(";", Native.Registrations.OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Value}"));
            if (!visit.Restored || (!Native.AutomaticRegistration && visit.Registration != registration)) Restore(visit, true, "registration");
            if (Native.Freeze(character, visit.Player, generation) is not { } frozen) continue;
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
            owner.GetBundleRaw(Prefix + character.Text, Mod).Keys[Key] = new WorldStore.Slot { Json = serverJson };
            visit.Document = next;
            EnsureBands(visit);
            if (!_pending.TryGetValue(generation, out var pending)) _pending[generation] = pending = [];
            if (clientJson is not null) pending[character] = clientJson;
            else pending.Remove(character);
            foreach (var older in _pending.Keys.OrderByDescending(g => g).Skip(2).ToArray()) { _pending.Remove(older); _carriedBlocked.Remove(older); }
        }
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
            if (!same) merged["records"] = new JsonArray(after.Select(r => before.FirstOrDefault(o => Bytes(o) == Bytes(r))?.DeepClone() ?? r!.DeepClone()).ToArray());
            if (now["parkReason"] is { } held) merged["parkReason"] = held.DeepClone();
            else merged.Remove("parkReason");
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
        if (keys.Length > 32) throw new InvalidDataException("too many character storage instance bands");
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
            if (old.TryGetValue(key, out var band) && band is >= 0 and < 32 && used.Add(band)) assigned[key] = band;
        foreach (var key in keys)
            if (!assigned.ContainsKey(key))
            {
                var band = Enumerable.Range(0, 32).First(b => !used.Contains(b));
                used.Add(band);
                assigned[key] = band;
            }
        visit.Bands = assigned;
        bundle.Keys["instanceBands"] = new WorldStore.Slot { Json = System.Text.Json.JsonSerializer.Serialize(assigned) };
    }

    private IEnumerable<(Visit Visit, string Key, EntityId Holder, int Offset)> InstanceHolders()
    {
        if (Native is null) yield break;
        foreach (var visit in _visits.Values.Where(v => v.Restored && !v.Document.ReadOnly))
        {
            foreach (var row in Native.InstanceHolders(visit.Player))
                if (!row.Holder.IsNone && visit.Bands.TryGetValue(row.Key, out var band))
                {
                    var offset = 1024 + band * 128 + (row.Temporary ? 64 : 0);
                    _instanceOwners[row.Holder.Value] = (visit.Player, offset);
                    yield return (visit, row.Key, row.Holder, offset);
                }
        }
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
            Failed($"world generation {generation} completed before storage became durable (write failed or bounded barrier wait expired)");
            return;
        }
        _pending.Remove(generation);
        if (_carriedBlocked.Remove(generation, out var blocked)) Failed(blocked);
        else Failure = null;
        ReportCapability();
        foreach (var (character, json) in pending) _committed[character] = json;
        foreach (var older in _pending.Keys.Where(g => g < generation).ToArray()) _pending.Remove(older);
    }
}
