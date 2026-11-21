// SPDX-License-Identifier: MIT
// The server's ledger (chapter 05b sections 4.1-4.3): per character and owning mod, two canonical records (World
// values, accepted Carried values) and the Carried projection that travels with the character. Every operation reads
// the record as JSON from the store and writes it back whole, so nothing here holds a reference into a bundle that
// CharacterStore.Replace (a generic carried import) can clear. Tick thread of the side that runs the world.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>Which record.</summary>
internal enum Partition
{
    /// <summary>CharacterData "mod-values.world.v1".</summary>
    World = 0,
    /// <summary>CharacterData "mod-values.carried-authority.v1".</summary>
    CarriedAuthority = 1,
    /// <summary>CharacterData.Carried "mod-values.carried.v1": the projection, never read as authority.</summary>
    Projection = 2,
}

/// <summary>Where records live (Kiln.Saves in the game; a dictionary in tests and the test kit).</summary>
internal interface IValueStore
{
    /// <summary>The open world's id; <see cref="Guid.Empty"/> while no world's data is open.</summary>
    Guid World { get; }

    string? Read(Partition p, CharacterKey c, string owner);

    void Write(Partition p, CharacterKey c, string owner, string json);

    /// <summary>Every (character, owner) that has a record of <paramref name="p"/>.</summary>
    IReadOnlyList<(CharacterKey Character, string Owner)> Find(Partition p);
}

/// <summary>Where the ledger's events go (Kiln's event bus in the game; a list in tests).</summary>
internal interface IValueEvents
{
    void Changed(ModValueChangedEvent e);
    void Imported(ModValueImportedEvent e);
    /// <summary>Null: no one canceled; else the reason.</summary>
    string? Importing(ModValueImportingEvent e);
}

internal sealed class Entry
{
    [JsonNumberHandling(JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString)]
    public long Raw { get; set; }

    [JsonNumberHandling(JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString)]
    public ulong ValueRevision { get; set; }

    public uint DefinitionVersion { get; set; }
    public ModValueKind Kind { get; set; }
    public int Scale { get; set; }
    public uint CarryEpoch { get; set; }
    public string DefinitionFingerprint { get; set; } = "";
    public int PriorLevel { get; set; }
}

internal sealed class ValueRecord
{
    /// <summary>1; a stored object without it is not a record of ours (another writer used the reserved key).</summary>
    public int Schema { get; set; }

    [JsonNumberHandling(JsonNumberHandling.WriteAsString | JsonNumberHandling.AllowReadingFromString)]
    public ulong PartitionRevision { get; set; }

    public SortedDictionary<string, Entry> Entries { get; set; } = new(StringComparer.Ordinal);
}

/// <summary>One entry of a client's carried offer (wire and sidecar projection agree on these fields).</summary>
internal sealed record OfferEntry(string Key, long Raw, ModValueKind Kind, int Scale, uint DefinitionVersion, uint CarryEpoch);

/// <summary>What an offer did.</summary>
internal sealed record ImportSummary(bool Ok, string? Reason, int Applied, int Unchanged, int Rejected, IReadOnlyDictionary<string, int> ReasonCounts);

internal enum ChangeOp : uint { Add = 0, Spend = 1, Set = 2, Reset = 3 }

internal sealed class Ledger(Registry defs, IValueStore store, Func<CharacterKey, bool> isLive, IValueEvents events)
{
    internal static readonly JsonSerializerOptions Json = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        Converters = { new JsonStringEnumConverter(JsonNamingPolicy.CamelCase, allowIntegerValues: false) },
        WriteIndented = false,
    };

    public Registry Definitions { get; } = defs;
    public IValueStore Store { get; } = store;

    /// <summary>A value of (character, key) changed, appeared or left: the stream marks it.</summary>
    public event Action<CharacterKey, ModValueKey>? Dirty;

    private static Partition PartitionOf(ModValuePersistence p) => p == ModValuePersistence.Carried ? Partition.CarriedAuthority : Partition.World;

    // ---------------------------------------------------------------------------------------------- records ----

    /// <summary>The record, a fresh object; an empty one when there is none. Null when the stored JSON does not read
    /// (another writer used the reserved key): the record is then held, never overwritten.</summary>
    internal ValueRecord? Load(Partition p, CharacterKey c, string owner)
    {
        var json = Store.Read(p, c, owner);
        if (json is null) return new ValueRecord { Schema = 1 };
        try
        {
            var r = JsonSerializer.Deserialize<ValueRecord>(json, Json);
            return r is { Schema: 1 } ? WithEntries(r) : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    private static ValueRecord WithEntries(ValueRecord r)
    {
        r.Entries = new SortedDictionary<string, Entry>(r.Entries ?? new(), StringComparer.Ordinal);
        return r;
    }

    private void Save(Partition p, CharacterKey c, string owner, ValueRecord r) => Store.Write(p, c, owner, JsonSerializer.Serialize(r, Json));

    private ModValueSnapshot Snap(Def d, CharacterKey c, Entry e, ValueRecord r) =>
        new(Store.World, c, d.Key, e.Raw, d.LevelOf(e.Raw), e.Scale, e.Kind, d.Persistence, e.DefinitionVersion, e.CarryEpoch, e.ValueRevision, r.PartitionRevision);

    private Entry Fresh(Def d) => new()
    {
        Raw = d.Initial, ValueRevision = 0, DefinitionVersion = d.Version, Kind = d.Kind, Scale = d.Scale, CarryEpoch = d.CarryEpoch,
        DefinitionFingerprint = d.Fingerprint, PriorLevel = d.LevelOf(d.Initial),
    };

    // ---------------------------------------------------------------------------------- reconcile (4.2, 4.3) ----

    private enum State { Ok, Missing, Held, Budget }

    /// <summary>Brings the saved entry of <paramref name="d"/> in line with it: migration by a higher version, the carry
    /// epoch, a partition move. Writes and posts what it changed. <paramref name="initialize"/>: create a missing entry.</summary>
    private State Reconcile(Def d, CharacterKey c, bool initialize, ulong requestId)
    {
        var p = PartitionOf(d.Persistence);
        var rec = Load(p, c, d.Owner);
        if (rec is null) return State.Held;
        if (rec.Entries.TryGetValue(d.Id, out var e))
        {
            if (e.DefinitionVersion == d.Version && e.DefinitionFingerprint == d.Fingerprint)
                return EpochCheck(d, c, p, rec, e, requestId);
            if (d.Version <= e.DefinitionVersion)
                return e.DefinitionFingerprint == d.Fingerprint ? EpochCheck(d, c, p, rec, e, requestId) : State.Held;
            return Migrate(d, c, p, rec, e, requestId);
        }
        // in the other partition: a persistence change
        var otherP = p == Partition.World ? Partition.CarriedAuthority : Partition.World;
        var other = Load(otherP, c, d.Owner);
        if (other is null) return State.Held;
        if (other.Entries.TryGetValue(d.Id, out var o))
        {
            if (d.Version <= o.DefinitionVersion || !d.AllowPartitionMove) return State.Held;
            if (rec.PartitionRevision == ulong.MaxValue || other.PartitionRevision == ulong.MaxValue || o.ValueRevision == ulong.MaxValue) return State.Budget;
            var before = Snapshot(d, c, o, other, otherP);
            var moved = Convert(d, o, fromCarried: otherP == Partition.CarriedAuthority, out var held);
            if (held) return State.Held;
            other.Entries.Remove(d.Id);
            other.PartitionRevision++;
            moved.ValueRevision = o.ValueRevision + 1;
            rec.Entries[d.Id] = moved;
            rec.PartitionRevision++;
            Save(otherP, c, d.Owner, other);
            Save(p, c, d.Owner, rec);
            if (otherP == Partition.CarriedAuthority || p == Partition.CarriedAuthority) Project(c, d.Owner);
            events.Changed(new ModValueChangedEvent(c, d.Key, before, Snap(d, c, moved, rec), ModValueCauses.Migrated, requestId));
            Dirty?.Invoke(c, d.Key);
            return State.Ok;
        }
        if (!initialize) return State.Missing;
        if (rec.PartitionRevision == ulong.MaxValue) return State.Budget;
        var fresh = Fresh(d);
        rec.Entries[d.Id] = fresh;
        rec.PartitionRevision++;
        Save(p, c, d.Owner, rec);
        if (p == Partition.CarriedAuthority) Project(c, d.Owner);
        events.Changed(new ModValueChangedEvent(c, d.Key, null, Snap(d, c, fresh, rec), ModValueCauses.Initialized, requestId));
        Dirty?.Invoke(c, d.Key);
        return State.Ok;
    }

    // a snapshot of an entry under its own saved metadata (the "before" of a migration)
    private ModValueSnapshot Snapshot(Def d, CharacterKey c, Entry e, ValueRecord r, Partition p) =>
        new(Store.World, c, d.Key, e.Raw, e.PriorLevel, e.Scale, e.Kind, p == Partition.CarriedAuthority ? ModValuePersistence.Carried : ModValuePersistence.World,
            e.DefinitionVersion, e.CarryEpoch, e.ValueRevision, r.PartitionRevision);

    private State EpochCheck(Def d, CharacterKey c, Partition p, ValueRecord rec, Entry e, ulong requestId)
    {
        if (p != Partition.CarriedAuthority || e.CarryEpoch == d.CarryEpoch) return State.Ok;
        if (d.CarryEpoch < e.CarryEpoch) return State.Held;
        if (e.ValueRevision == ulong.MaxValue || rec.PartitionRevision == ulong.MaxValue) return State.Budget;
        var before = Snapshot(d, c, e, rec, p);
        var reset = Fresh(d);
        reset.ValueRevision = e.ValueRevision + 1;
        rec.Entries[d.Id] = reset;
        rec.PartitionRevision++;
        Save(p, c, d.Owner, rec);
        Project(c, d.Owner);
        events.Changed(new ModValueChangedEvent(c, d.Key, before, Snap(d, c, reset, rec), ModValueCauses.EpochReset, requestId));
        Dirty?.Invoke(c, d.Key);
        return State.Ok;
    }

    private State Migrate(Def d, CharacterKey c, Partition p, ValueRecord rec, Entry e, ulong requestId)
    {
        if (e.ValueRevision == ulong.MaxValue || rec.PartitionRevision == ulong.MaxValue) return State.Budget;
        var next = Convert(d, e, fromCarried: p == Partition.CarriedAuthority, out var held);
        if (held) return State.Held;
        if (p == Partition.CarriedAuthority && e.CarryEpoch > d.CarryEpoch) return State.Held;
        var before = Snapshot(d, c, e, rec, p);
        next.ValueRevision = e.ValueRevision + 1;
        rec.Entries[d.Id] = next;
        rec.PartitionRevision++;
        Save(p, c, d.Owner, rec);
        if (p == Partition.CarriedAuthority) Project(c, d.Owner);
        var cause = p == Partition.CarriedAuthority && e.CarryEpoch < d.CarryEpoch ? ModValueCauses.EpochReset : ModValueCauses.Migrated;
        events.Changed(new ModValueChangedEvent(c, d.Key, before, Snap(d, c, next, rec), cause, requestId));
        Dirty?.Invoke(c, d.Key);
        return State.Ok;
    }

    /// <summary>The entry under the definition's current metadata (section 4.3): same kind rescales and clamps, a kind
    /// change resets only with resetOnKindChange, a higher carry epoch resets carried progress. A World value moved into
    /// Carried has no carry epoch of its own: it keeps its value and takes the definition's epoch.</summary>
    private Entry Convert(Def d, Entry e, bool fromCarried, out bool held)
    {
        held = false;
        var next = Fresh(d);
        if (e.Kind != d.Kind)
        {
            if (!d.ResetOnKindChange) held = true;
            return next;
        }
        if (fromCarried && d.Persistence == ModValuePersistence.Carried && e.CarryEpoch < d.CarryEpoch) return next;
        next.Raw = Clamp(Rescale(e.Raw, e.Scale, d.Scale), d.Min, d.Max);
        next.PriorLevel = d.LevelOf(next.Raw);
        return next;
    }

    internal static Int128 Rescale(long raw, int from, int to)
    {
        Int128 v = raw;
        if (to >= from)
        {
            for (var i = from; i < to; i++) v *= 10;
            return v;
        }
        for (var i = to; i < from; i++) v /= 10;  // toward zero
        return v;
    }

    internal static long Clamp(Int128 v, long min, long max) => v < min ? min : v > max ? max : (long)v;

    // ---------------------------------------------------------------------------------------------- entry points ----

    /// <summary>A character is live (in game): initialize and migrate every active definition for it.</summary>
    public void Touch(CharacterKey c)
    {
        if (c.IsNone || c.IsAccountOnly || Store.World == Guid.Empty) return;
        foreach (var d in Definitions.AllActive()) Reconcile(d, c, initialize: true, 0);
    }

    /// <summary>A definition became active: initialize it for the live characters.</summary>
    public void Activated(Def d, IEnumerable<CharacterKey> live)
    {
        if (Store.World == Guid.Empty) return;
        foreach (var c in live)
            if (!c.IsNone && !c.IsAccountOnly) Reconcile(d, c, initialize: true, 0);
    }

    private string? Lookup(ModValueKey key, out Def? d)
    {
        d = Definitions.Active(key);
        if (d is not null) return null;
        return Definitions.IsRetired(key) ? ModValueReasons.DefinitionRetired : ModValueReasons.Undefined;
    }

    private static string? Why(State s) => s switch
    {
        State.Ok => null,
        State.Missing => ModValueReasons.NotReady,
        State.Budget => ModValueReasons.Budget,
        _ => ModValueReasons.MigrationRequired,
    };

    /// <summary>Reads a value. <paramref name="reader"/>: the calling owner (the read policy applies).</summary>
    public ModValueReadResult Read(string reader, CharacterKey c, ModValueKey key)
    {
        if (c.IsNone) throw new ArgumentException("no character", nameof(c));
        if (Lookup(key, out var d) is { } missing) return new(false, missing, null);
        if (!d!.Readable(reader)) return new(false, ModValueReasons.AccessDenied, null);
        if (c.IsAccountOnly || Store.World == Guid.Empty) return new(false, ModValueReasons.CharacterUnavailable, null);
        // a read migrates a saved value it finds stale (deterministic, also for an offline character); it never
        // initializes one (a character gets its values when it is in game)
        if (Why(Reconcile(d, c, initialize: false, 0)) is { } why) return new(false, why, null);
        var p = PartitionOf(d.Persistence);
        var rec = Load(p, c, d.Owner);
        if (rec is null || !rec.Entries.TryGetValue(d.Id, out var e)) return new(false, rec is null ? ModValueReasons.MigrationRequired : ModValueReasons.NotReady, null);
        return new(true, null, Snap(d, c, e, rec));
    }

    /// <summary>Changes a value (server code of the owning mod; the character must be live).</summary>
    public ModValueResult Change(string writer, CharacterKey c, ModValueKey key, ChangeOp op, long amount, ModValueVersion? expected, ulong requestId)
    {
        if (c.IsNone) throw new ArgumentException("no character", nameof(c));
        if (op > ChangeOp.Reset) throw new ArgumentException($"unknown operation {(uint)op}", nameof(op));
        if (Lookup(key, out var d) is { } missing) return Refused(missing);
        if (key.Mod != writer) return Refused(ModValueReasons.AccessDenied);
        switch (op)
        {
            case ChangeOp.Spend when d!.Kind != ModValueKind.Currency:
                throw new ArgumentException($"{key}: Spend is for currencies; use Add with a negative amount on a counter", nameof(op));
            case ChangeOp.Spend when amount <= 0:
                throw new ArgumentException($"{key}: Spend takes a positive amount ({amount})", nameof(amount));
            case ChangeOp.Add when amount < 0 && d!.Kind != ModValueKind.Counter:
                throw new ArgumentException($"{key}: a {Def.Lower(d.Kind)} is not lowered with Add; use {(d.Kind == ModValueKind.Currency ? "Spend" : "Set")}", nameof(amount));
        }
        if (c.IsAccountOnly || Store.World == Guid.Empty || !isLive(c)) return Refused(ModValueReasons.CharacterUnavailable);
        if (Why(Reconcile(d!, c, initialize: true, requestId)) is { } why) return Refused(why);
        var p = PartitionOf(d!.Persistence);
        var rec = Load(p, c, d.Owner);
        if (rec is null || !rec.Entries.TryGetValue(d.Id, out var e)) return Refused(ModValueReasons.MigrationRequired);
        if (expected is { } x && (x.World != Store.World || x.ValueRevision != e.ValueRevision)) return Refused(ModValueReasons.StaleRevision);
        Int128 now = e.Raw, next;
        switch (op)
        {
            case ChangeOp.Add: next = now + amount; break;
            case ChangeOp.Spend:
                next = now - amount;
                if (next < d.Min) return Refused(ModValueReasons.InsufficientFunds);
                break;
            case ChangeOp.Set: next = amount; break;
            default:
                if (d.Persistence == ModValuePersistence.Carried) return Refused(ModValueReasons.EpochRequired);
                next = d.Initial;
                break;
        }
        if (next < d.Min || next > d.Max) return Refused(ModValueReasons.BoundExceeded);
        if (d.Persistence == ModValuePersistence.Carried && next < now) return Refused(ModValueReasons.NonMonotonic);
        var before = Snap(d, c, e, rec);
        if (next == now) return new(true, null, before, before);  // a no-op: no revision, no event
        if (e.ValueRevision == ulong.MaxValue || rec.PartitionRevision == ulong.MaxValue) return Refused(ModValueReasons.Budget);
        e.PriorLevel = d.LevelOf(e.Raw);
        e.Raw = (long)next;
        e.ValueRevision++;
        rec.PartitionRevision++;
        Save(p, c, d.Owner, rec);
        if (p == Partition.CarriedAuthority) Project(c, d.Owner);
        var after = Snap(d, c, e, rec);
        events.Changed(new ModValueChangedEvent(c, d.Key, before, after, op == ChangeOp.Reset ? ModValueCauses.Reset : ModValueCauses.Changed, requestId));
        Dirty?.Invoke(c, d.Key);
        return new(true, null, before, after);

        static ModValueResult Refused(string why) => new(false, why, null, null);
    }

    // ---------------------------------------------------------------------------------------- carried (4.2) ----

    /// <summary>Writes the carried copy of (character, owner) that travels with the character, from this world's canonical
    /// carried record. Only where this world has such a record: a copy of a mod this world does not hold, of a world that
    /// has not taken the player's offer, or of a refused offer passes through untouched (CharacterSync's promise). Where it
    /// has one, the player's own copy keeps what it has that is higher (same kind, scale and epoch): a freshly initialized
    /// record never lowers progress the player brought before its stats offer is merged. Entries naming a World value of
    /// this world are dropped (they never travel); entries this world does not know pass through.</summary>
    public void Project(CharacterKey c, string owner)
    {
        if (Store.Read(Partition.CarriedAuthority, c, owner) is null) return;
        var auth = Load(Partition.CarriedAuthority, c, owner);
        if (auth is null) return;
        var copy = LoadLenient(Store.Read(Partition.Projection, c, owner));
        if (copy is not null)
            foreach (var (id, e) in copy.Entries)
            {
                if (e is null || !ModValueRules.IsId(id)) continue;
                if (auth.Entries.TryGetValue(id, out var a))
                {
                    if (e.Kind == a.Kind && e.Scale == a.Scale && e.CarryEpoch == a.CarryEpoch && e.Raw > a.Raw) auth.Entries[id] = e;
                    continue;
                }
                var key = new ModValueKey(owner, id);
                var def = Definitions.Active(key) ?? Definitions.Described(key);
                if (def is { Persistence: ModValuePersistence.World }) continue;  // a World value never travels
                auth.Entries[id] = e;
            }
        var json = JsonSerializer.Serialize(auth, Json);
        if (Store.Read(Partition.Projection, c, owner) != json) Store.Write(Partition.Projection, c, owner, json);
    }

    private static ValueRecord? LoadLenient(string? json)
    {
        if (json is null) return null;
        try
        {
            var r = JsonSerializer.Deserialize<ValueRecord>(json, Json);
            return r is { Schema: 1 } ? WithEntries(r) : null;
        }
        catch (JsonException)
        {
            return null;
        }
    }

    /// <summary>Every copy again (start, before a save is staged, after a carried import): where this world has a
    /// canonical carried record only.</summary>
    public void ProjectAll()
    {
        if (Store.World == Guid.Empty) return;
        foreach (var (c, owner) in Store.Find(Partition.CarriedAuthority)) Project(c, owner);
    }

    /// <summary>A client's carried offer for a live character: per entry, rescale, clamp, then the exact maximum of
    /// the canonical value and the offered one (never a sum). Valid entries commit together; the rest are rejected
    /// one by one. Import policy and the importing events are the caller's (the stream), before this.</summary>
    public ImportSummary Import(CharacterKey c, IReadOnlyList<OfferEntry> offered)
    {
        int applied = 0, unchanged = 0, rejected = 0;
        var reasons = new SortedDictionary<string, int>(StringComparer.Ordinal);
        void Reject(string why)
        {
            rejected++;
            reasons[why] = reasons.GetValueOrDefault(why) + 1;
        }
        if (c.IsNone || c.IsAccountOnly || Store.World == Guid.Empty || !isLive(c))
            return new ImportSummary(false, ModValueReasons.CharacterUnavailable, 0, 0, offered.Count, reasons);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var plan = new List<(Def Def, long Value)>();
        foreach (var o in offered)
        {
            if (!seen.Add(o.Key)) { Reject("duplicate"); continue; }
            if (!ModValueKey.TryParse(o.Key, out var key)) { Reject("malformed"); continue; }
            var d = Definitions.Active(key);
            if (d is null) { Reject(ModValueReasons.Undefined); continue; }
            if (d.Persistence != ModValuePersistence.Carried || !d.TrustImports) { Reject("not-carried"); continue; }
            if (o.Kind != d.Kind) { Reject("kind"); continue; }
            if (o.CarryEpoch != d.CarryEpoch) { Reject("epoch"); continue; }
            if (o.Scale is < 0 or > ModValueRules.MaxScale) { Reject("malformed"); continue; }
            plan.Add((d, Clamp(Rescale(o.Raw, o.Scale, d.Scale), d.Min, d.Max)));
        }
        var touched = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (d, offeredValue) in plan)
        {
            var s = Reconcile(d, c, initialize: true, 0);
            if (s != State.Ok) { Reject(Why(s)!); continue; }
            var rec = Load(Partition.CarriedAuthority, c, d.Owner);
            if (rec is null || !rec.Entries.TryGetValue(d.Id, out var e)) { Reject(ModValueReasons.MigrationRequired); continue; }
            if (offeredValue <= e.Raw) { unchanged++; continue; }
            if (e.ValueRevision == ulong.MaxValue || rec.PartitionRevision == ulong.MaxValue) { Reject(ModValueReasons.Budget); continue; }
            var before = Snap(d, c, e, rec);
            e.PriorLevel = d.LevelOf(e.Raw);
            e.Raw = offeredValue;
            e.ValueRevision++;
            rec.PartitionRevision++;
            Save(Partition.CarriedAuthority, c, d.Owner, rec);
            touched.Add(d.Owner);
            applied++;
            events.Changed(new ModValueChangedEvent(c, d.Key, before, Snap(d, c, e, rec), ModValueCauses.Imported, 0));
            Dirty?.Invoke(c, d.Key);
        }
        foreach (var owner in touched) Project(c, owner);
        events.Imported(new ModValueImportedEvent(c, applied, unchanged, rejected));
        return new ImportSummary(true, null, applied, unchanged, rejected, reasons);
    }

    // ---------------------------------------------------------------------------------------------- the stream ----

    /// <summary>What the character's own client may receive: every active definition with sync, and its value when the
    /// character has one that is not held.</summary>
    public IReadOnlyList<(Def Def, ModValueSnapshot? Value)> Visible(CharacterKey c)
    {
        var list = new List<(Def, ModValueSnapshot?)>();
        if (c.IsNone || c.IsAccountOnly || Store.World == Guid.Empty) return list;
        var records = new Dictionary<(Partition, string), ValueRecord?>();
        foreach (var d in Definitions.AllActive())
        {
            if (d.Sync == ModValueSyncPolicy.None) continue;
            var p = PartitionOf(d.Persistence);
            if (!records.TryGetValue((p, d.Owner), out var rec)) records[(p, d.Owner)] = rec = Load(p, c, d.Owner);
            if (rec is null) continue;
            if (!rec.Entries.TryGetValue(d.Id, out var e)) { list.Add((d, null)); continue; }
            if (e.DefinitionVersion != d.Version || e.DefinitionFingerprint != d.Fingerprint || p == Partition.CarriedAuthority && e.CarryEpoch != d.CarryEpoch)
                continue;  // held for migration: neither definition nor value
            list.Add((d, Snap(d, c, e, rec)));
        }
        return list;
    }
}
