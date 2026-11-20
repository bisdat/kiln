// SPDX-License-Identifier: MIT
// The server's half of the stats stream (chapter 05b section 4.4) for one association (a peer and its character):
// a nonce, full then delta transfers of the allowed state, one immutable transfer in flight, a bounded dirty set,
// pacing and retries, and the once-per-nonce carry offer. Pure: the clock, the state and the sends come from outside.
using System.Text;
using System.Text.Json;

namespace Kiln;

internal sealed class ServerSession
{
    internal static readonly TimeSpan AckTimeout = TimeSpan.FromSeconds(10), RetryFullAfter = TimeSpan.FromSeconds(10),
        Cadence = TimeSpan.FromMilliseconds(250), OfferDeadline = TimeSpan.FromSeconds(15);
    internal const int MaxRetries = 2, DirtyLimit = 1024;
    internal const double BytesPerSecond = 32 * 1024, Burst = 16 * 1024;

    public CharacterKey Character { get; }
    public Guid World { get; }
    public string Nonce { get; private set; }
    public bool TooLarge { get; private set; }

    private readonly Func<string> _newNonce;
    private ulong _nextSeq = 1, _lastAcked;
    private Dictionary<string, (string Def, string? Value)> _acked = new(StringComparer.Ordinal);
    private Flight? _flight;
    private bool _needFull = true, _changed = true;
    private readonly HashSet<string> _dirty = new(StringComparer.Ordinal);
    private DateTime _nextTransferAt, _retryFullAt;
    private double _tokens = Burst;
    private DateTime _tokensAt;

    // the carry offer: one per nonce
    private OfferAssembly? _offer;
    private (string Transfer, string Sha, ModValuesWire.CarryResult Result)? _offerDone;

    private sealed class Flight
    {
        public required ulong Seq;
        public required bool Full;
        public required List<ModValuesWire.StatePart> Parts;
        public required Dictionary<string, (string Def, string? Value)> State;
        public int Sent;
        public DateTime LastPartAt;
        public int Retries;
    }

    private sealed class OfferAssembly
    {
        public required string Transfer, Sha;
        public required int Parts, Total;
        public required DateTime Started;
        public readonly Dictionary<int, byte[]> Have = [];
    }

    public ServerSession(CharacterKey character, Guid world, DateTime now, Func<string>? newNonce = null)
    {
        Character = character;
        World = world;
        _newNonce = newNonce ?? ModValuesWire.NewNonce;
        Nonce = _newNonce();
        _tokensAt = now;
    }

    public ModValuesWire.Hello Hello() =>
        new(Nonce, World.ToString("D"), Character.Text, TooLarge ? ModValuesWire.TooLarge : ModValuesWire.Ready, ModValuesWire.MaxStateBytes, ModValuesWire.PartBytes);

    /// <summary>A new nonce: the client clears its copy at the next Hello, the stream starts over with a full, and the
    /// carry offer may come again. Definitions or policies changed, the sequence ran out.</summary>
    public ModValuesWire.Hello Rotate()
    {
        Nonce = _newNonce();
        _nextSeq = 1;
        _lastAcked = 0;
        _acked = new Dictionary<string, (string, string?)>(StringComparer.Ordinal);
        _flight = null;
        _needFull = _changed = true;
        _dirty.Clear();
        _offer = null;
        _offerDone = null;
        TooLarge = false;
        _retryFullAt = default;
        return Hello();
    }

    public void MarkDirty(string key)
    {
        _changed = true;
        if (_dirty.Count < DirtyLimit) _dirty.Add(key);
        else _needFull = true;  // overflow: the next transfer replaces everything, so no removal is lost
    }

    public ulong LastAcked => _lastAcked;
    public bool InFlight => _flight is not null;

    /// <summary>One step: sends parts within the budget, retries, or starts the next transfer. <paramref name="state"/>
    /// is asked only when a transfer starts.</summary>
    public void Pump(DateTime now, Func<IReadOnlyList<(Def Def, ModValueSnapshot? Value)>> state, Action<object> send)
    {
        _tokens = Math.Min(Burst, _tokens + (now - _tokensAt).TotalSeconds * BytesPerSecond);
        _tokensAt = now;
        if (_flight is { } f)
        {
            if (f.Sent < f.Parts.Count) { SendParts(f, now, send); return; }
            if (now - f.LastPartAt < AckTimeout) return;
            if (f.Retries < MaxRetries)
            {
                f.Retries++;
                f.Sent = 0;  // the same transfer, sequence and digest again
                SendParts(f, now, send);
                return;
            }
            _flight = null;  // given up: a new full after a pause, never an endless retry
            _needFull = _changed = true;
            _retryFullAt = now + RetryFullAfter;
            return;
        }
        if (now < _retryFullAt || now < _nextTransferAt || !_changed && !_needFull) return;
        if (_nextSeq == ulong.MaxValue)
        {
            send(Rotate());
            return;
        }
        var current = Build(state());
        _changed = false;
        _dirty.Clear();
        var full = _needFull || _lastAcked == 0;
        byte[]? doc = null;
        if (!full)
        {
            doc = Delta(current);
            if (doc is null) return;  // nothing the client lacks
            if (doc.Length > ModValuesWire.MaxStateBytes) full = true;
        }
        if (full)
        {
            var values = current.Count(kv => kv.Value.Value is not null);
            doc = Encode(current, null);
            if (values > ModValuesWire.MaxValues || doc.Length > ModValuesWire.MaxStateBytes)
            {
                // never truncated: presentation stops, the ledger and the connection go on; looked at again later
                if (!TooLarge)
                {
                    TooLarge = true;
                    send(Hello());
                }
                _retryFullAt = now + RetryFullAfter;
                _needFull = _changed = true;
                return;
            }
            if (TooLarge)
            {
                TooLarge = false;
                send(Hello());
            }
        }
        var seq = _nextSeq++;
        var sha = ModValuesWire.Sha(doc!);
        var pieces = ModValuesWire.Cut(doc!);
        var parts = new List<ModValuesWire.StatePart>(pieces.Count);
        for (var i = 0; i < pieces.Count; i++)
            parts.Add(new ModValuesWire.StatePart(Nonce, ModValuesWire.U64(seq), ModValuesWire.U64(seq), ModValuesWire.U64(full ? 0 : _lastAcked), full, i,
                pieces.Count, doc!.Length, sha, Convert.ToBase64String(pieces[i])));
        _flight = new Flight { Seq = seq, Full = full, Parts = parts, State = current };
        _needFull = false;
        _nextTransferAt = now + Cadence;
        SendParts(_flight, now, send);
    }

    private void SendParts(Flight f, DateTime now, Action<object> send)
    {
        while (f.Sent < f.Parts.Count)
        {
            var cost = f.Parts[f.Sent].PayloadB64.Length + 256;
            if (_tokens < cost && _tokens < Burst) return;  // a full bucket always lets one part go
            _tokens -= cost;
            send(f.Parts[f.Sent++]);
            f.LastPartAt = now;
        }
    }

    private static Dictionary<string, (string Def, string? Value)> Build(IReadOnlyList<(Def Def, ModValueSnapshot? Value)> state)
    {
        var d = new Dictionary<string, (string, string?)>(StringComparer.Ordinal);
        foreach (var (def, v) in state)
            d[def.Qualified] = (def.PublicJson, v is null ? null : JsonSerializer.Serialize(ValueOf(def.Qualified, v), ModValuesWire.Options));
        return d;
    }

    internal static ModValuesWire.Value ValueOf(string key, ModValueSnapshot v) => new()
    {
        Key = key, Raw = ModValuesWire.I64(v.Value), Level = v.Level, ValueRevision = ModValuesWire.U64(v.ValueRevision),
        PartitionRevision = ModValuesWire.U64(v.PartitionRevision),
    };

    private byte[]? Delta(Dictionary<string, (string Def, string? Value)> current)
    {
        var defs = new List<string>();
        var values = new List<string>();
        var removed = new List<string>();
        foreach (var (k, (def, value)) in current)
        {
            var had = _acked.TryGetValue(k, out var a);
            if (had && a.Value is not null && value is null)
            {
                removed.Add(k);  // a value cannot leave alone: remove both, send the definition again
                defs.Add(k);
                continue;
            }
            if (!had || a.Def != def) defs.Add(k);
            if (value is not null && (!had || a.Value != value || a.Def != def)) values.Add(k);
        }
        foreach (var k in _acked.Keys)
            if (!current.ContainsKey(k)) removed.Add(k);
        if (defs.Count == 0 && values.Count == 0 && removed.Count == 0) return null;
        return Encode(current, (defs, values, removed));
    }

    private static byte[] Encode(Dictionary<string, (string Def, string? Value)> state, (List<string> Defs, List<string> Values, List<string> Removed)? delta)
    {
        var p = new ModValuesWire.Payload();
        IEnumerable<string> defKeys = delta?.Defs ?? (IEnumerable<string>)state.Keys;
        IEnumerable<string> valueKeys = delta?.Values ?? state.Where(kv => kv.Value.Value is not null).Select(kv => kv.Key);
        foreach (var k in defKeys.Order(StringComparer.Ordinal)) p.Definitions.Add(JsonSerializer.Deserialize<ModValuesWire.Definition>(state[k].Def, ModValuesWire.Options)!);
        foreach (var k in valueKeys.Order(StringComparer.Ordinal)) p.Values.Add(JsonSerializer.Deserialize<ModValuesWire.Value>(state[k].Value!, ModValuesWire.Options)!);
        if (delta is { } dl) p.Removed.AddRange(dl.Removed.Order(StringComparer.Ordinal));
        return JsonSerializer.SerializeToUtf8Bytes(p, ModValuesWire.Options);
    }

    public void OnAck(ModValuesWire.Ack a)
    {
        if (a.Nonce != Nonce || _flight is not { } f || f.Sent < f.Parts.Count) return;
        if (!ModValuesWire.TryU64(a.Seq, out var seq) || seq != f.Seq || a.Transfer != ModValuesWire.U64(f.Seq)) return;
        _lastAcked = f.Seq;
        _acked = f.State;
        _flight = null;
    }

    /// <summary>The client lost the thread: whatever it claims, the answer is a new full.</summary>
    public void OnResync(ModValuesWire.Resync r)
    {
        if (r.Nonce != Nonce) return;
        _flight = null;
        _needFull = _changed = true;
    }

    // ---------------------------------------------------------------------------------------------- carry offer ----

    /// <summary>One part of the client's carried offer. Returns the whole document when the last part arrived and its
    /// digest holds (the caller applies it and calls <see cref="OfferDone"/>); a repeated completed offer re-sends
    /// its cached result; a second different offer is refused.</summary>
    public byte[]? OnOfferPart(ModValuesWire.CarryOfferPart m, DateTime now, Action<object> send)
    {
        if (m.Nonce != Nonce) return null;
        ModValuesWire.CarryResult Refuse(string why) =>
            new(Nonce, m.Transfer, false, 0, 0, 0, new Dictionary<string, int> { [why] = 1 });
        if (_offerDone is { } done)
        {
            send(done.Transfer == m.Transfer && done.Sha == m.Sha256 ? done.Result : Refuse("one-offer-per-stream"));
            return null;
        }
        if (m.Parts is < 1 or > ModValuesWire.MaxOfferParts || m.TotalBytes is < 1 or > ModValuesWire.MaxOfferBytes || m.Part < 0 || m.Part >= m.Parts)
        {
            _offer = null;
            send(Refuse("malformed"));
            return null;
        }
        byte[] bytes;
        try
        {
            bytes = Convert.FromBase64String(m.PayloadB64);
        }
        catch (FormatException)
        {
            _offer = null;
            send(Refuse("malformed"));
            return null;
        }
        if (bytes.Length > ModValuesWire.PartBytes) { _offer = null; send(Refuse("malformed")); return null; }
        if (_offer is { } o && (now - o.Started > OfferDeadline)) _offer = null;
        if (_offer is null) _offer = new OfferAssembly { Transfer = m.Transfer, Sha = m.Sha256, Parts = m.Parts, Total = m.TotalBytes, Started = now };
        var a = _offer;
        if (a.Transfer != m.Transfer || a.Sha != m.Sha256 || a.Parts != m.Parts || a.Total != m.TotalBytes)
        {
            _offer = null;
            send(Refuse("inconsistent"));
            return null;
        }
        if (a.Have.TryGetValue(m.Part, out var had))
        {
            if (!had.AsSpan().SequenceEqual(bytes)) { _offer = null; send(Refuse("conflicting-duplicate")); }
            return null;
        }
        a.Have[m.Part] = bytes;
        if (a.Have.Count < a.Parts) return null;
        var doc = Enumerable.Range(0, a.Parts).SelectMany(i => a.Have[i]).ToArray();
        _offer = null;
        if (doc.Length != a.Total || ModValuesWire.Sha(doc) != a.Sha) { send(Refuse("digest")); return null; }
        return doc;
    }

    /// <summary>The offer was handled: remember its result (repeats get it back), and send a full when it changed values.</summary>
    public ModValuesWire.CarryResult OfferDone(string transfer, string sha, ImportSummary s)
    {
        var reasons = new Dictionary<string, int>(s.ReasonCounts, StringComparer.Ordinal);
        if (!s.Ok && s.Reason is { } why) reasons[why] = reasons.GetValueOrDefault(why) + 1;
        var result = new ModValuesWire.CarryResult(Nonce, transfer, s.Ok, s.Applied, s.Unchanged, s.Rejected, reasons);
        if (Encoding.UTF8.GetByteCount(JsonSerializer.Serialize(result, ModValuesWire.Options)) > ModValuesWire.MaxResultBytes)
            result = result with { ReasonCounts = new Dictionary<string, int> { ["many"] = s.Rejected } };
        _offerDone = (transfer, sha, result);
        if (s.Applied > 0) _needFull = _changed = true;
        return result;
    }

    /// <summary>Reads an offer document; null when malformed (the whole offer is refused).</summary>
    internal static List<OfferEntry>? ParseOffer(byte[] doc)
    {
        try
        {
            var p = JsonSerializer.Deserialize<ModValuesWire.OfferPayload>(doc, ModValuesWire.Options);
            if (p is not { Schema: 1 } || p.Entries is null || p.Entries.Count > 4 * ModValueRules.MaxDefinitionsPerOwner * 8) return null;
            var list = new List<OfferEntry>(p.Entries.Count);
            foreach (var e in p.Entries)
            {
                if (e is null || !ModValuesWire.TryI64(e.Raw, out var raw) || e.Kind > 2 || e.Scale > ModValueRules.MaxScale) return null;
                list.Add(new OfferEntry(e.Key ?? "", raw, (ModValueKind)e.Kind, (int)e.Scale, e.DefinitionVersion, e.CarryEpoch));
            }
            return list;
        }
        catch (JsonException)
        {
            return null;
        }
    }
}
