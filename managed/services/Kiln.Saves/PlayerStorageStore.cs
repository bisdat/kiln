// SPDX-License-Identifier: MIT
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>The save bridge's narrow internal face. The engine must return the frozen records and witness of the SAME
/// character capture, never a later live inventory read. Until that capture is ready Freeze returns null.</summary>
internal interface IPlayerStoragePersistence
{
    StorageFreeze? Freeze(EntityId player, uint generation);
    string? WitnessOf(EntityId player);
    IReadOnlyDictionary<string, int> Registrations { get; }
    void Restore(EntityId player, JsonObject storages, bool paired, string source, long revision);
    void Park(EntityId player, JsonObject parked);
    IReadOnlyDictionary<string, EntityId> Holders(EntityId player) => new Dictionary<string, EntityId>();
}

internal sealed record StorageFreeze(string Witness, JsonObject Storages);

/// <summary>One world's server copies and pending carried updates. Pending generations remain on a failed world write;
/// only a matching world-written event publishes the carried copy.</summary>
internal sealed class PlayerStorageStore(WorldStore owner)
{
    internal const string Prefix = "player-storage/", Mod = "kiln-items", Key = "storage";
    internal IPlayerStoragePersistence? Native;
    private readonly Dictionary<CharacterKey, Visit> _visits = [];
    private readonly Dictionary<uint, Dictionary<CharacterKey, string>> _pending = [];
    private readonly Dictionary<CharacterKey, string> _committed = [];
    private sealed class Visit(EntityId player, PlayerStorageDocument document)
    {
        internal readonly EntityId Player = player;
        internal PlayerStorageDocument Document = document;
        internal bool Restored, Withheld, Local;
        internal string Registration = "";
    }

    internal void Clear() { _visits.Clear(); _pending.Clear(); _committed.Clear(); }
    internal string? Carried(CharacterKey character) => _committed.GetValueOrDefault(character);

    internal PlayerStorageDocument? Read(CharacterKey character)
    {
        var json = owner.FindBundle(Prefix + character.Text)?.Keys.GetValueOrDefault(Key)?.Json;
        if (json is null) return null;
        try { return PlayerStorageDocument.Read(json, character.Id); }
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
        var server = Read(character);
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
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: unsupported document parked whole, storage read-only");
            return null;
        }
        var choice = PlayerStorageDocument.Choose(witness, server, client, trustClient);
        var selected = choice?.Source == "client" ? client : server;
        var document = selected is null ? PlayerStorageDocument.Empty(character.Id) : PlayerStorageDocument.Read(selected.Write(32), character.Id);
        if (choice is not null)
        {
            // Keep the chosen copy's history, including its old current when a historical pair won.
            var previous = (JsonObject)document.Data["current"]!.DeepClone();
            previous["revision"] = document.Revision;
            document.Data["history"]!.AsArray().Insert(0, previous);
            document.Data["current"] = choice.Pair.DeepClone();
            document.Data["current"]!.AsObject().Remove("pairs");
            document.Data["revision"] = Math.Max(server?.Revision ?? 0, client?.Revision ?? 0);
            // Pair aliases describe this content only, and must not be carried over from another selected candidate.
            SavesRuntime.Log(choice.Paired ? LogLevel.Info : LogLevel.Warn,
                $"player-storage: {character}: {(choice.Paired ? "paired" : "unpaired: newest revision; character changed outside Kiln")}, {choice.Source}, revision {choice.Revision}");
        }
        var visit = _visits[character] = new Visit(player, document) { Local = local };
        if (!trustClient && choice is { Paired: false } && client is { ReadOnly: false } && client.Revision > choice.Revision)
        {
            visit.Withheld = true;
            SavesRuntime.Log(LogLevel.Warn, $"player-storage: {character}: changed elsewhere since this server's copy; server storage parked, client cannot add storage here");
        }
        Restore(visit, choice?.Paired ?? true, choice?.Source ?? "new");
        return choice;
    }

    private void Restore(Visit visit, bool paired, string source)
    {
        if (Native is null || visit.Document.ReadOnly) return;
        var current = visit.Document.Data["current"]!.AsObject();
        EnsureBands(current);
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
        foreach (var (character, visit) in _visits)
        {
            if (visit.Document.ReadOnly) continue;
            var registration = string.Join(";", Native.Registrations.OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Value}"));
            if (!visit.Restored || visit.Registration != registration) Restore(visit, true, "registration");
            if (Native.Freeze(visit.Player, generation) is not { } frozen) continue;
            var current = visit.Document.Data["current"]!.AsObject();
            var next = PlayerStorageDocument.Read(visit.Document.Write(32), character.Id);
            var frozenStorages = (JsonObject)frozen.Storages.DeepClone();
            foreach (var (key, bag) in frozenStorages)
                if (bag is JsonObject b && (current["storages"]?[key] ?? current["parked"]?[key]) is JsonObject old && old["instanceBand"] is { } band)
                    b["instanceBand"] = band.DeepClone();
            next.Capture(frozen.Witness, generation, frozenStorages, current["parked"]!.AsObject());
            next.Data["current"]!["source"] = visit.Local && current["source"]?.GetValue<string>() != "server" ? "local" : "server";
            var serverJson = next.Write(32);
            var clientJson = next.Write(8);
            owner.GetBundleRaw(Prefix + character.Text, Mod).Keys[Key] = new WorldStore.Slot { Json = serverJson };
            visit.Document = next;
            if (!_pending.TryGetValue(generation, out var pending)) _pending[generation] = pending = [];
            pending[character] = clientJson;
        }
    }

    private static void EnsureBands(JsonObject current)
    {
        var bags = current["storages"]!.AsObject().Concat(current["parked"]!.AsObject()).GroupBy(x => x.Key).OrderBy(x => x.Key, StringComparer.Ordinal).ToArray();
        var assigned = new Dictionary<string, int>(StringComparer.Ordinal);
        var used = new HashSet<int>();
        foreach (var group in bags)
            if (group.Select(x => x.Value?["instanceBand"]).OfType<System.Text.Json.Nodes.JsonValue>().FirstOrDefault() is { } band
                && band.TryGetValue<int>(out var b) && b is >= 0 and < 32 && used.Add(b)) assigned[group.Key] = b;
        foreach (var group in bags)
        {
            if (!assigned.TryGetValue(group.Key, out var band))
            {
                band = Enumerable.Range(0, 32).First(b => !used.Contains(b));
                used.Add(band);
            }
            foreach (var bag in group.Select(x => x.Value).OfType<JsonObject>()) bag["instanceBand"] = band;
        }
    }

    private IEnumerable<(Visit Visit, string Key, EntityId Holder, int Offset)> InstanceHolders()
    {
        if (Native is null) yield break;
        foreach (var visit in _visits.Values.Where(v => v.Restored && !v.Document.ReadOnly))
        {
            var current = visit.Document.Data["current"]!;
            foreach (var (key, holder) in Native.Holders(visit.Player))
                if (!holder.IsNone && current["storages"]?[key]?["instanceBand"] is JsonValue band && band.TryGetValue<int>(out var b))
                    yield return (visit, key, holder, 1024 + b * 64);
        }
    }

    internal InstancePlace NormalizeInstance(InstancePlace place)
    {
        foreach (var row in InstanceHolders())
            if (row.Holder == place.Holder) return new(row.Visit.Player, row.Offset + place.FlatSlot);
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
        return visit.Restored && visit.Document.Data["current"]!["storages"]!.AsObject().Select(x => x.Key).All(Native.Holders(player).ContainsKey);
    }

    internal void Committed(uint generation)
    {
        if (!_pending.Remove(generation, out var pending)) return;
        foreach (var (character, json) in pending) _committed[character] = json;
        foreach (var older in _pending.Keys.Where(g => g < generation).ToArray()) _pending.Remove(older);
    }
}
