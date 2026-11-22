// SPDX-License-Identifier: MIT
// The client's half of the stats stream (chapter 05b section 4.4): one assembly at a time, every part checked, the
// whole document's digest and schema checked, then one reference swap of an immutable snapshot, then the Ack. Old,
// duplicate and foreign-nonce messages change nothing; anything that does not fit asks for a full (Resync, at most
// one per 2 s). Pure: the clock, the sends and the events come from outside.
using System.Text.Json;

namespace Kiln;

/// <summary>What the client has: immutable, swapped whole.</summary>
internal sealed class SyncedState
{
    public static readonly SyncedState Empty = new();
    public string? Nonce { get; init; }
    public Guid World { get; init; }
    public CharacterKey Character { get; init; }
    public bool Available { get; init; }
    public ulong Seq { get; init; }
    public IReadOnlyDictionary<string, ModValuesWire.Definition> Definitions { get; init; } = new Dictionary<string, ModValuesWire.Definition>();
    public IReadOnlyDictionary<string, ModValuesWire.Value> Values { get; init; } = new Dictionary<string, ModValuesWire.Value>();

    public ModValueSnapshot? Snapshot(string key)
    {
        if (!Definitions.TryGetValue(key, out var d) || !Values.TryGetValue(key, out var v) || !ModValueKey.TryParse(key, out var k)) return null;
        ModValuesWire.TryI64(v.Raw, out var raw);
        ModValuesWire.TryU64(v.ValueRevision, out var vr);
        ModValuesWire.TryU64(v.PartitionRevision, out var pr);
        return new ModValueSnapshot(World, Character, k, raw, v.Level, (int)d.Scale, (ModValueKind)d.Kind, (ModValuePersistence)d.Persistence,
            d.Version, d.CarryEpoch, vr, pr);
    }
}

internal interface ISyncedEvents
{
    void Synced(ModValueSyncedEvent e);
    void Published(ModValuesSnapshotPublishedEvent e);
}

internal sealed class ClientStream(Func<DateTime> clock, Action<object> send, ISyncedEvents events)
{
    internal static readonly TimeSpan AssemblyDeadline = TimeSpan.FromSeconds(90), IdleTimeout = TimeSpan.FromSeconds(10),
        DuplicateMemory = TimeSpan.FromSeconds(90), ResyncGap = TimeSpan.FromSeconds(2);

    private SyncedState _state = SyncedState.Empty;
    private Assembly? _assembly;
    private (string Transfer, string Sha, ulong Seq, DateTime At)? _completed;
    private DateTime _lastResync = DateTime.MinValue;

    public SyncedState State => Volatile.Read(ref _state);
    public long Resyncs { get; private set; }
    public long Commits { get; private set; }

    private sealed class Assembly
    {
        public required ulong Seq;
        public required string Transfer, Sha, BaseSeq;
        public required bool Full;
        public required int Parts, Total;
        public required DateTime Started;
        public DateTime LastPart;
        public readonly Dictionary<int, byte[]> Have = [];
    }

    /// <summary>A new stream (or a status change of this one). A new nonce clears the copy at once.</summary>
    public void OnHello(ModValuesWire.Hello h)
    {
        if (h.Nonce is not { Length: 32 } || !Guid.TryParse(h.World, out var world) || !CharacterKey.TryParse(h.Character, out var c)) return;
        var available = h.Status == ModValuesWire.Ready;
        if (h.Nonce == _state.Nonce)
        {
            Swap(new SyncedState { Nonce = h.Nonce, World = _state.World, Character = _state.Character, Available = available, Seq = _state.Seq,
                Definitions = available ? _state.Definitions : new Dictionary<string, ModValuesWire.Definition>(),
                Values = available ? _state.Values : new Dictionary<string, ModValuesWire.Value>() }, full: true);
            return;
        }
        _assembly = null;
        _completed = null;
        Swap(new SyncedState { Nonce = h.Nonce, World = world, Character = c, Available = available }, full: true);
    }

    /// <summary>Left the world: nothing of the old copy stays readable.</summary>
    public void Clear()
    {
        _assembly = null;
        _completed = null;
        Swap(SyncedState.Empty, full: true);
    }

    public void OnPart(ModValuesWire.StatePart m)
    {
        var s = _state;
        if (s.Nonce is null || m.Nonce != s.Nonce) return;  // another stream's message: dropped
        var now = clock();
        if (m.PayloadB64 is null || m.Sha256 is null || m.Transfer is null ||
            !ModValuesWire.TryU64(m.Seq, out var seq) || !ModValuesWire.TryU64(m.BaseSeq, out var baseSeq) || m.Transfer != m.Seq ||
            m.Parts is < 1 or > ModValuesWire.MaxParts || m.TotalBytes is < 0 or > ModValuesWire.MaxStateBytes || m.Part < 0 || m.Part >= m.Parts ||
            m.Full != (baseSeq == 0) || seq == 0)
        {
            Resync(s, "bounds");
            return;
        }
        if (seq <= s.Seq)
        {
            // old: a duplicate of the transfer applied last is acknowledged again (its Ack may have been lost), never re-applied
            if (_completed is { } c && c.Seq == seq && c.Transfer == m.Transfer && c.Sha == m.Sha256 && now - c.At < DuplicateMemory && m.Part == m.Parts - 1)
                send(new ModValuesWire.Ack(s.Nonce, m.Seq, m.Transfer));
            return;
        }
        if (!m.Full && baseSeq != s.Seq)
        {
            Resync(s, "gap");  // a delta on another base is never published
            return;
        }
        if (_assembly is { } a)
        {
            if (seq < a.Seq) return;  // an older transfer than the one being assembled
            if (seq > a.Seq || now - a.Started > AssemblyDeadline || now - a.LastPart > IdleTimeout) _assembly = null;  // superseded or stale
        }
        byte[] bytes;
        try
        {
            bytes = Convert.FromBase64String(m.PayloadB64);
        }
        catch (FormatException)
        {
            Resync(s, "base64");
            return;
        }
        if (bytes.Length > ModValuesWire.PartBytes)
        {
            Resync(s, "part-size");
            return;
        }
        _assembly ??= new Assembly { Seq = seq, Transfer = m.Transfer, Sha = m.Sha256, BaseSeq = m.BaseSeq, Full = m.Full, Parts = m.Parts, Total = m.TotalBytes, Started = now };
        var asm = _assembly;
        if (asm.Transfer != m.Transfer || asm.Sha != m.Sha256 || asm.BaseSeq != m.BaseSeq || asm.Full != m.Full || asm.Parts != m.Parts || asm.Total != m.TotalBytes)
        {
            _assembly = null;
            Resync(s, "inconsistent");
            return;
        }
        asm.LastPart = now;
        if (asm.Have.TryGetValue(m.Part, out var had))
        {
            if (had.AsSpan().SequenceEqual(bytes)) return;  // the same part again
            _assembly = null;
            Resync(s, "conflicting-duplicate");
            return;
        }
        asm.Have[m.Part] = bytes;
        if (asm.Have.Count < asm.Parts) return;
        _assembly = null;
        var doc = Enumerable.Range(0, asm.Parts).SelectMany(i => asm.Have[i]).ToArray();
        if (doc.Length != asm.Total || ModValuesWire.Sha(doc) != asm.Sha)
        {
            Resync(s, "digest");
            return;
        }
        var next = Apply(s, doc, asm.Full, seq);
        if (next is null)
        {
            Resync(s, "schema");
            return;
        }
        Commits++;
        _completed = (asm.Transfer, asm.Sha, seq, now);
        Swap(next, asm.Full);
        send(new ModValuesWire.Ack(s.Nonce, m.Seq, m.Transfer));
    }

    /// <summary>The new state, or null when the document breaks the schema (then nothing is published).</summary>
    private static SyncedState? Apply(SyncedState s, byte[] doc, bool full, ulong seq)
    {
        ModValuesWire.Payload? p;
        try
        {
            p = JsonSerializer.Deserialize<ModValuesWire.Payload>(doc, ModValuesWire.Options);
        }
        catch (JsonException)
        {
            return null;
        }
        if (p is not { Schema: 1 } || p.Definitions is null || p.Values is null || p.Removed is null) return null;
        var defs = full ? new Dictionary<string, ModValuesWire.Definition>(StringComparer.Ordinal) : new Dictionary<string, ModValuesWire.Definition>(s.Definitions, StringComparer.Ordinal);
        var values = full ? new Dictionary<string, ModValuesWire.Value>(StringComparer.Ordinal) : new Dictionary<string, ModValuesWire.Value>(s.Values, StringComparer.Ordinal);
        foreach (var k in p.Removed)
        {
            if (!ModValueKey.TryParse(k, out _)) return null;
            defs.Remove(k);
            values.Remove(k);
        }
        foreach (var d in p.Definitions)
        {
            if (d is null || !ModValueKey.TryParse(d.Key, out _) || d.Kind > 2 || d.Persistence > 1 || d.Scale > ModValueRules.MaxScale || d.ReadPolicy > 2 || d.Sync > 2 ||
                d.Label is null || d.NumberFormat is null || d.ReadMods is null)
                return null;
            defs[d.Key] = d;
        }
        foreach (var v in p.Values)
        {
            if (v is null || !defs.ContainsKey(v.Key) || !ModValuesWire.TryI64(v.Raw, out _) || !ModValuesWire.TryU64(v.ValueRevision, out _) ||
                !ModValuesWire.TryU64(v.PartitionRevision, out _))
                return null;  // values never appear without their definition
            values[v.Key] = v;
        }
        if (values.Count > ModValuesWire.MaxValues) return null;
        return new SyncedState { Nonce = s.Nonce, World = s.World, Character = s.Character, Available = s.Available, Seq = seq, Definitions = defs, Values = values };
    }

    private void Resync(SyncedState s, string why)
    {
        var now = clock();
        if (now - _lastResync < ResyncGap) return;
        _lastResync = now;
        Resyncs++;
        send(new ModValuesWire.Resync(s.Nonce!, ModValuesWire.U64(s.Seq), why));
    }

    private void Swap(SyncedState next, bool full)
    {
        var old = Interlocked.Exchange(ref _state, next);
        // events for real changes only, after the whole state is in place
        var keys = new HashSet<string>(old.Values.Keys, StringComparer.Ordinal);
        keys.UnionWith(next.Values.Keys);
        var nonce = next.Nonce ?? old.Nonce ?? "";
        foreach (var k in keys.Order(StringComparer.Ordinal))
        {
            var before = old.Nonce == next.Nonce ? old.Snapshot(k) : old.Snapshot(k);
            var after = next.Snapshot(k);
            if (before == after) continue;
            if (!ModValueKey.TryParse(k, out var key)) continue;
            events.Synced(new ModValueSyncedEvent(key, before, after, nonce, next.Seq));
        }
        if (next.Nonce is not null && (next.Seq > 0 || full)) events.Published(new ModValuesSnapshotPublishedEvent(nonce, next.Seq, full));
    }

    // ---------------------------------------------------------------------------------------------- carry offer ----

    private string? _offeredNonce;

    /// <summary>Once per nonce: the parts of an offer of the carried projections this client keeps for its character
    /// (from the CharacterSync sidecar, <c>mods/&lt;mod&gt;/mod-values.carried.v1</c>). Empty when there is nothing to offer.</summary>
    public IReadOnlyList<ModValuesWire.CarryOfferPart> Offer(long sidecarRevision, IReadOnlyDictionary<string, string> projections)
    {
        var s = _state;
        if (s.Nonce is null || !s.Available || _offeredNonce == s.Nonce) return [];
        _offeredNonce = s.Nonce;
        var payload = new ModValuesWire.OfferPayload { SidecarRevision = ModValuesWire.U64((ulong)Math.Max(0, sidecarRevision)) };
        foreach (var (owner, json) in projections.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            if (!ModValueRules.IsOwner(owner)) continue;
            ValueRecord? rec;
            try
            {
                rec = JsonSerializer.Deserialize<ValueRecord>(json, Ledger.Json);
            }
            catch (JsonException)
            {
                continue;
            }
            if (rec?.Entries is null) continue;
            foreach (var (id, e) in rec.Entries)
                if (ModValueRules.IsId(id) && e is not null)
                    payload.Entries.Add(new ModValuesWire.OfferValue
                    {
                        Key = owner + ":" + id, Raw = ModValuesWire.I64(e.Raw), Kind = (uint)e.Kind, Scale = (uint)Math.Clamp(e.Scale, 0, 6),
                        DefinitionVersion = e.DefinitionVersion, CarryEpoch = e.CarryEpoch,
                    });
        }
        if (payload.Entries.Count == 0) return [];
        var doc = JsonSerializer.SerializeToUtf8Bytes(payload, ModValuesWire.Options);
        if (doc.Length > ModValuesWire.MaxOfferBytes) return [];
        var sha = ModValuesWire.Sha(doc);
        var pieces = ModValuesWire.Cut(doc);
        var transfer = "1";
        return pieces.Select((b, i) => new ModValuesWire.CarryOfferPart(s.Nonce, transfer, i, pieces.Count, doc.Length, sha, Convert.ToBase64String(b))).ToList();
    }
}
