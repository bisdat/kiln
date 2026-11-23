// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

internal sealed record StorageChoice(JsonObject Pair, long Revision, bool Paired, string Source);

/// <summary>The Kiln-owned v1 storage value. Item bytes are authoritative; the mirror is for tools. Malformed entries
/// are isolated with their original JSON, and unsupported future documents are never rewritten.</summary>
internal sealed class PlayerStorageDocument
{
    internal const int MaxBytes = 64 * 1024, ServerMaxBytes = 256 * 1024, MaxStorages = 32;
    internal JsonObject Data { get; }
    internal bool ReadOnly { get; }
    internal string? Original { get; }
    internal PlayerStorageDocument(JsonObject data, bool readOnly = false, string? original = null) => (Data, ReadOnly, Original) = (data, readOnly, original);
    internal long Revision => Number(Data["revision"]);

    internal static PlayerStorageDocument Empty(Guid character) => new(new JsonObject
    {
        ["v"] = 1, ["character"] = character.ToString(), ["revision"] = 0,
        ["current"] = new JsonObject { ["witness"] = "", ["worldRev"] = 0, ["storages"] = new JsonObject(), ["parked"] = new JsonObject() },
        ["history"] = new JsonArray(), ["quarantine"] = new JsonArray(),
    });

    internal static PlayerStorageDocument Read(string json, Guid character, int maxBytes = MaxBytes)
    {
        if (Encoding.UTF8.GetByteCount(json) > maxBytes) throw new InvalidDataException($"storage document exceeds {maxBytes} bytes before decode");
        try
        {
            var data = JsonNode.Parse(json, documentOptions: new JsonDocumentOptions { MaxDepth = 32 }) as JsonObject
                ?? throw new InvalidDataException("storage document must be an object");
            var version = data["v"]?.GetValue<int>() ?? throw new InvalidDataException("storage version missing");
            if (version > 1) return new(data, true, json);
            if (version < 0) throw new InvalidDataException("invalid storage version");
            if (!Guid.TryParse(data["character"]?.GetValue<string>(), out var id) || id != character)
                throw new InvalidDataException("storage belongs to another character");
            if (Number(data["revision"]) < 0) throw new InvalidDataException("negative storage revision");
            // v0 used the same item representation, before history and quarantine were required.
            data["v"] = 1;
            if (data["quarantine"] is not JsonArray) data["quarantine"] = new JsonArray();
            var doc = new PlayerStorageDocument(data);
            if (data["current"] is not JsonObject current)
            {
                doc.Quarantine("current", data["current"], "pair must be an object");
                data["current"] = Empty(character).Data["current"]!.DeepClone();
            }
            else
            {
                try { doc.CleanPair(current, false); }
                catch (Exception e) when (Malformed(e))
                {
                    doc.Quarantine("current", current, e.Message);
                    data["current"] = Empty(character).Data["current"]!.DeepClone();
                }
            }
            var history = data["history"] as JsonArray ?? new JsonArray();
            var clean = new JsonArray();
            foreach (var entry in history)
            {
                if (entry is not JsonObject pair)
                {
                    doc.Quarantine("history", entry, "pair must be an object");
                    continue;
                }
                try
                {
                    var clone = (JsonObject)pair.DeepClone();
                    doc.CleanPair(clone, true);
                    clean.Add(clone);
                }
                catch (Exception e) when (Malformed(e)) { doc.Quarantine("history", pair, e.Message); }
            }
            data["history"] = clean;
            return doc;
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or FormatException or OverflowException)
        {
            throw new InvalidDataException("invalid storage document: " + e.Message, e);
        }
    }

    private static bool Malformed(Exception e) => e is InvalidDataException or InvalidOperationException or FormatException or OverflowException or JsonException;
    private static long Number(JsonNode? node)
    {
        if (node is null) return 0;
        if (node is JsonValue value)
        {
            if (value.TryGetValue<long>(out var l)) return l;
            if (value.TryGetValue<int>(out var i)) return i;
            if (value.TryGetValue<uint>(out var u)) return u;
        }
        throw new InvalidDataException("expected integer");
    }
    private static string Text(JsonNode? node) => node?.GetValue<string>() ?? "";

    internal void Quarantine(string storage, JsonNode? record, string reason)
    {
        var raw = record?.ToJsonString() ?? "null";
        var bytes = record is JsonObject o && o["b64"] is JsonValue b && b.TryGetValue<string>(out var encoded) ? encoded : Convert.ToBase64String(Encoding.UTF8.GetBytes(raw));
        Data["quarantine"]!.AsArray().Add(new JsonObject { ["storage"] = storage, ["record"] = bytes, ["raw"] = raw, ["reason"] = reason });
    }

    private void CleanPair(JsonObject pair, bool historical)
    {
        if (pair["witness"] is not JsonValue witness || !witness.TryGetValue<string>(out _)) throw new InvalidDataException("witness missing");
        if (historical && (pair["revision"] is null || Number(pair["revision"]) < 0)) throw new InvalidDataException("invalid pair revision");
        if (pair["pairs"] is JsonArray aliases)
        {
            var cleanAliases = new JsonArray();
            foreach (var alias in aliases)
            {
                try
                {
                    if (alias is not JsonObject a || Text(a["witness"]).Length == 0 || Number(a["revision"]) < 0)
                        throw new InvalidDataException("invalid witness alias");
                    cleanAliases.Add(a.DeepClone());
                }
                catch (Exception e) when (Malformed(e)) { Quarantine("pairs", alias, e.Message); }
            }
            pair["pairs"] = cleanAliases;
        }
        var storages = pair["storages"] as JsonObject ?? new JsonObject();
        var parked = pair["parked"] as JsonObject ?? new JsonObject();
        if (storages.Select(x => x.Key).Union(parked.Select(x => x.Key)).Count() > MaxStorages)
        {
            Quarantine("pair", pair, "more than 32 storages before decode");
            pair["storages"] = new JsonObject();
            pair["parked"] = new JsonObject();
            return;
        }
        pair["storages"] = CleanStorages(storages);
        pair["parked"] = CleanStorages(parked);
    }

    private JsonObject CleanStorages(JsonObject input)
    {
        var output = new JsonObject();
        foreach (var (key, value) in input)
        {
            try
            {
                if (!key.Contains(':') || value is not JsonObject bag) throw new InvalidDataException("invalid storage key or object");
                var slots = Number(bag["slots"]);
                if (slots is < 1 or > 64 || bag["records"] is not JsonArray records || records.Count > slots)
                    throw new InvalidDataException("invalid slots or records exceed slots before decode");
                var copy = (JsonObject)bag.DeepClone();
                var kept = new JsonArray();
                var used = new HashSet<int>();
                foreach (var record in records)
                {
                    try
                    {
                        if (record is not JsonObject row) throw new InvalidDataException("record must be an object");
                        var slot = checked((int)Number(row["s"]));
                        if (slot < 0 || slot >= slots || used.Contains(slot)) throw new InvalidDataException("invalid or duplicate slot before decode");
                        var bytes = Convert.FromBase64String(Text(row["b64"]));
                        ValidateRecord(bytes, slot);
                        kept.Add(Mirror(row, bytes));
                        used.Add(slot);
                    }
                    catch (Exception e) when (Malformed(e)) { Quarantine(key, record, e.Message); }
                }
                copy["records"] = kept;
                if (copy["refs"] is JsonObject refs)
                    foreach (var (name, index) in refs.ToArray())
                    {
                        try
                        {
                            if (name is not ("arrow" or "spell" or "rangedWeapon" or "fishingBait") || Number(index) < 0 || Number(index) >= slots)
                                throw new InvalidDataException("invalid reference");
                        }
                        catch (Exception e) when (Malformed(e)) { Quarantine(key + "/refs/" + name, index, e.Message); refs.Remove(name); }
                    }
                output[key] = copy;
            }
            catch (Exception e) when (Malformed(e)) { Quarantine(key, value, e.Message); }
        }
        return output;
    }

    internal static JsonObject Mirror(JsonObject row, byte[] bytes)
    {
        var mirror = (JsonObject)row.DeepClone();
        mirror["item"] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(1));
        mirror["n"] = (int)BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(5));
        foreach (var name in new[] { "level", "durability", "maxDurability", "liquid", "maxLiquid", "appearance", "maxUpgrades", "upgrades", "socket" }) mirror.Remove(name);
        var at = 8;
        if ((bytes[7] & 1) != 0) mirror["level"] = (int)bytes[at++];
        if ((bytes[7] & 2) != 0)
        {
            mirror["durability"] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(at)); at += 4;
            mirror["maxDurability"] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(at)); at += 4;
        }
        if ((bytes[7] & 4) != 0)
        {
            mirror["liquid"] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(at)); at += 4;
            mirror["maxLiquid"] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(at)); at += 4;
        }
        if ((bytes[7] & 8) != 0) { mirror["appearance"] = Convert.ToHexStringLower(bytes.AsSpan(at, 4)); at += 4; }
        if ((bytes[7] & 16) != 0) { mirror["maxUpgrades"] = (int)bytes[at++]; mirror["upgrades"] = (int)bytes[at++]; }
        if ((bytes[7] & 32) != 0) mirror["socket"] = Convert.ToHexStringLower(bytes.AsSpan(at, 5));
        return mirror;
    }

    internal static void ValidateRecord(byte[] b, int slot)
    {
        if (b.Length < 8 || b[0] != slot || (b[7] & 0xc0) != 0) throw new InvalidDataException("invalid game item record");
        var size = 8 + ((b[7] & 1) != 0 ? 1 : 0) + ((b[7] & 2) != 0 ? 8 : 0) + ((b[7] & 4) != 0 ? 8 : 0)
            + ((b[7] & 8) != 0 ? 4 : 0) + ((b[7] & 16) != 0 ? 2 : 0) + ((b[7] & 32) != 0 ? 5 : 0);
        if (b.Length != size) throw new InvalidDataException("truncated or trailing game item record bytes");
        if (BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(1)) == 0 || BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(5)) == 0)
            throw new InvalidDataException("zero item or count");
    }

    // The primary pair always participates, even when older writers supplied aliases without repeating it.
    internal static IEnumerable<(string Witness, long Revision, long WorldRev)> Pairs(JsonObject pair, long revision)
    {
        yield return (Text(pair["witness"]), revision, Number(pair["worldRev"]));
        if (pair["pairs"] is JsonArray aliases)
            foreach (var a in aliases.OfType<JsonObject>()) yield return (Text(a["witness"]), Number(a["revision"]), Number(a["worldRev"]));
    }

    private IEnumerable<StorageChoice> Candidates(string source, string witness)
    {
        if (ReadOnly) yield break;
        foreach (var (pair, revision) in Entries())
            foreach (var p in Pairs(pair, revision))
            {
                if (p.Witness.Length == 0) continue;
                var view = (JsonObject)pair.DeepClone();
                view["witness"] = p.Witness;
                view["worldRev"] = p.WorldRev;
                yield return new(view, p.Revision, p.Witness == witness, source);
            }
    }

    private IEnumerable<(JsonObject Pair, long Revision)> Entries()
    {
        yield return (Data["current"]!.AsObject(), Revision);
        foreach (var pair in Data["history"]!.AsArray().OfType<JsonObject>()) yield return (pair, Number(pair["revision"]));
    }

    internal static StorageChoice? Choose(string witness, PlayerStorageDocument? server, PlayerStorageDocument? client, bool trustClient = true) =>
        (server?.Candidates("server", witness) ?? []).Concat(trustClient ? client?.Candidates("client", witness) ?? [] : [])
            .OrderByDescending(x => x.Paired).ThenByDescending(x => x.Revision).FirstOrDefault();

    internal void Capture(string witness, uint worldRev, JsonObject storages, JsonObject parked)
    {
        if (ReadOnly) throw new InvalidOperationException("future storage document is read-only");
        var old = Data["current"]!.AsObject();
        var equal = JsonNode.DeepEquals(old["storages"], storages) && JsonNode.DeepEquals(old["parked"], parked);
        if (Text(old["witness"]) == witness && equal && Number(old["worldRev"]) == worldRev) return;
        var revision = Entries().SelectMany(e => Pairs(e.Pair, e.Revision)).Select(p => p.Revision).Append(Revision).Max();
        var next = new JsonObject { ["witness"] = witness, ["worldRev"] = worldRev, ["storages"] = storages.DeepClone(), ["parked"] = parked.DeepClone() };
        if (Text(old["witness"]).Length != 0)
        {
            if (JsonNode.DeepEquals(old["storages"], storages) && JsonNode.DeepEquals(old["parked"], parked))
            {
                var aliases = new JsonArray();
                foreach (var p in Pairs(old, Revision).Distinct()) aliases.Add(Alias(p));
                next["pairs"] = aliases;
            }
            else
            {
                var previous = (JsonObject)old.DeepClone();
                previous["revision"] = Revision;
                Data["history"]!.AsArray().Insert(0, previous);
            }
        }
        Data["revision"] = checked(revision + 1);
        Data["current"] = next;
    }

    private static JsonObject Alias((string Witness, long Revision, long WorldRev) p) => new() { ["witness"] = p.Witness, ["revision"] = p.Revision, ["worldRev"] = p.WorldRev };

    private static void Trim(JsonObject data, int historyLimit)
    {
        var remaining = historyLimit;
        var current = data["current"]!.AsObject();
        if (current["pairs"] is JsonArray aliases)
        {
            var kept = new JsonArray();
            var reserve = data["history"]!.AsArray().Count > 0 && remaining > 0 ? 1 : 0;
            foreach (var a in aliases.OfType<JsonObject>().OrderByDescending(a => Number(a["revision"])).Take(Math.Max(0, remaining - reserve))) kept.Add(a.DeepClone());
            remaining -= kept.Count;
            current["pairs"] = kept;
        }
        var history = new JsonArray();
        foreach (var entry in data["history"]!.AsArray().OfType<JsonObject>())
        {
            if (remaining == 0) break;
            var copy = (JsonObject)entry.DeepClone();
            var pairs = Pairs(entry, Number(entry["revision"])).Distinct().OrderByDescending(p => p.Revision).Take(remaining).ToArray();
            if (pairs.Length == 0) continue;
            copy["witness"] = pairs[0].Witness;
            copy["revision"] = pairs[0].Revision;
            copy["worldRev"] = pairs[0].WorldRev;
            copy.Remove("pairs");
            if (pairs.Length > 1) copy["pairs"] = new JsonArray(pairs.Skip(1).Select(p => (JsonNode)Alias(p)).ToArray());
            history.Add(copy);
            remaining -= pairs.Length;
        }
        data["history"] = history;
    }

    private static int PairCount(JsonObject data) => Pairs(data["current"]!.AsObject(), Number(data["revision"])).Count()
        + data["history"]!.AsArray().OfType<JsonObject>().Sum(p => Pairs(p, Number(p["revision"])).Count());

    private static bool DropOldest(JsonObject data)
    {
        var choices = new List<(long Revision, Action Remove)>();
        if (data["current"]!["pairs"] is JsonArray currentAliases)
            foreach (var alias in currentAliases.OfType<JsonObject>())
            {
                var captured = alias;
                choices.Add((Number(alias["revision"]), () => currentAliases.Remove(captured)));
            }
        var history = data["history"]!.AsArray();
        // The whole newest historical entry is pinned: the character file can still be that pre-write state.
        for (var i = 1; i < history.Count; i++)
        {
            var entry = history[i]!.AsObject();
            if (entry["pairs"] is JsonArray aliases && aliases.Count > 0)
                foreach (var alias in aliases.OfType<JsonObject>())
                {
                    var captured = alias;
                    choices.Add((Number(alias["revision"]), () => aliases.Remove(captured)));
                }
            choices.Add((Number(entry["revision"]), () =>
            {
                if (entry["pairs"] is JsonArray { Count: > 0 } remaining)
                {
                    var first = remaining[0]!.AsObject();
                    entry["witness"] = first["witness"]?.DeepClone();
                    entry["revision"] = first["revision"]?.DeepClone();
                    entry["worldRev"] = first["worldRev"]?.DeepClone();
                    remaining.RemoveAt(0);
                }
                else history.Remove(entry);
            }));
        }
        if (choices.Count == 0) return false;
        choices.OrderBy(c => c.Revision).First().Remove();
        return true;
    }

    internal string Write(int historyLimit, int maxBytes = MaxBytes)
    {
        if (ReadOnly) return Original!;
        var copy = (JsonObject)Data.DeepClone();
        var before = PairCount(copy);
        Trim(copy, historyLimit);
        var json = copy.ToJsonString();
        var bytes = Encoding.UTF8.GetByteCount(json);
        while (bytes > maxBytes && DropOldest(copy))
        {
            json = copy.ToJsonString();
            bytes = Encoding.UTF8.GetByteCount(json);
        }
        if (bytes > maxBytes) throw new InvalidDataException($"storage current, newest history and quarantine exceed {maxBytes} bytes; previous copy kept");
        var dropped = before - PairCount(copy);
        if (dropped > 0) SavesRuntime.Log(LogLevel.Info, $"player-storage: {Data["character"]}: trimmed {dropped} old history pairs; {bytes} bytes");
        return json;
    }

    internal static (JsonObject Active, JsonObject Parked) Partition(JsonObject storages, JsonObject parked, IReadOnlyDictionary<string, int> registrations)
    {
        var active = new JsonObject();
        var waiting = new JsonObject();
        foreach (var key in storages.Select(x => x.Key).Union(parked.Select(x => x.Key)))
        {
            var first = (storages[key] ?? parked[key])!.AsObject();
            var merged = (JsonObject)first.DeepClone();
            if (storages[key] is JsonObject live && parked[key] is JsonObject overflow)
            {
                merged["slots"] = Math.Max(Number(live["slots"]), Number(overflow["slots"]));
                merged["records"] = new JsonArray(live["records"]!.AsArray().Concat(overflow["records"]!.AsArray()).Select(r => r!.DeepClone()).ToArray());
                var refs = live["refs"] as JsonObject ?? new JsonObject();
                var allRefs = (JsonObject)refs.DeepClone();
                if (overflow["refs"] is JsonObject more) foreach (var (name, value) in more) allRefs[name] = value?.DeepClone();
                merged["refs"] = allRefs;
            }
            if (merged["parkReason"] is not null || !registrations.TryGetValue(key, out var slots)) { waiting[key] = merged; continue; }
            var usable = (JsonObject)merged.DeepClone();
            var excess = (JsonObject)merged.DeepClone();
            usable["slots"] = slots;
            usable["records"] = new JsonArray(merged["records"]!.AsArray().Where(r => Number(r!["s"]) < slots).Select(r => r!.DeepClone()).ToArray());
            excess["records"] = new JsonArray(merged["records"]!.AsArray().Where(r => Number(r!["s"]) >= slots).Select(r => r!.DeepClone()).ToArray());
            if (merged["refs"] is JsonObject savedRefs)
            {
                usable["refs"] = new JsonObject();
                excess["refs"] = new JsonObject();
                foreach (var (name, index) in savedRefs)
                    (Number(index) < slots ? usable : excess)["refs"]!.AsObject()[name] = index?.DeepClone();
            }
            active[key] = usable;
            if (excess["records"]!.AsArray().Count != 0) waiting[key] = excess;
        }
        return (active, waiting);
    }
}
