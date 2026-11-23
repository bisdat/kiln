// SPDX-License-Identifier: MIT
// The authored NPC bodies this process's boot build reports (package 16f, chapter 16f §5.4): kiln.knowledge's native_manifest
// (native/services/quests/kiln_knowledge.h) carries a top-level "bodies" array, one row per NPC block with a body of its own:
// {"mod","npc","key","state":"ready"|"failed"|"removed","reason","template","model","graph","closure","generation"}. ready: published
// by this start's boot build; failed: that mod's body build refused it (reason says why); removed: Ready at the previous start (the
// native ledger kiln-quests/native/npc-bodies.ledger.json) and gone now. A manifest without "bodies" is an older native build: no rows.
// Kiln.Blocks (NpcBodies.Status) and the handshake (NpcBodyGate) both read it here, so neither needs the other's assembly; this reads
// the table's front itself, as PlayerGraphGate reads kiln.playeranim, so Kiln.Network needs no reference to Kiln.Quests.
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Kiln;

/// <summary>One row of the boot build's "bodies" array. <paramref name="State"/> is the manifest's word (ready, failed, removed).</summary>
internal sealed record NpcBodyRow(string Mod, string Npc, string Key, string State, string Reason, string Template, string Model, string Graph,
    string Closure, ulong Generation)
{
    public const string Ready = "ready", Failed = "failed", Removed = "removed";
    /// <summary>"mod:npc", the qualified block key NpcBodies.Status takes.</summary>
    public string BlockKey => $"{Mod}:{Npc}";
}

internal static unsafe class NpcBodyRows
{
    /// <summary>The front of KilnKnowledgeApi up to native_manifest (the header and 24 members before it): all this reads.</summary>
    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public uint Size, Version;
        public fixed ulong Before[24];
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> NativeManifest;
    }

    /// <summary>Tests and the test kit's fake put the manifest's JSON here (null: the native table's).</summary>
    internal static Func<string?>? TestSource;

    private static readonly object Lock = new();
    private static string? _native;      // the native manifest once read (it does not change after the boot build)
    private static string? _parsedFrom;  // the text _rows were parsed from
    private static IReadOnlyList<NpcBodyRow> _rows = [];
    private static readonly ConcurrentDictionary<string, byte> LateKeys = new(StringComparer.Ordinal);

    /// <summary>Every row of the boot build's "bodies" array, in the manifest's order; empty without a manifest or the key.</summary>
    public static IReadOnlyList<NpcBodyRow> All
    {
        get
        {
            var raw = TestSource is { } source ? source() : Native();
            lock (Lock)
            {
                if (!string.Equals(raw, _parsedFrom, StringComparison.Ordinal))
                {
                    _rows = Parse(raw);
                    _parsedFrom = raw;
                }
                return _rows;
            }
        }
    }

    /// <summary>The rows this start's boot build published (state ready).</summary>
    public static IEnumerable<NpcBodyRow> Ready => All.Where(r => r.State == NpcBodyRow.Ready);

    /// <summary>Kiln.Blocks' bodies defined from code after this start's boot build (PendingRestart), as "mod:npc": the handshake refuses
    /// a client that has any, since its game runs without them until it restarts.</summary>
    public static IReadOnlyList<string> Late => LateKeys.Keys.Order(StringComparer.Ordinal).ToList();

    public static void SetLate(string blockKey, bool late)
    {
        if (late) LateKeys[blockKey] = 0;
        else LateKeys.TryRemove(blockKey, out _);
    }

    internal static void ResetForTests()
    {
        LateKeys.Clear();
        lock (Lock)
        {
            _parsedFrom = null;
            _rows = [];
        }
    }

    private static string? Native()
    {
        if (_native != null) return _native;
        var t = Services.Get<Table>("kiln.knowledge", 1);
        if (t == null || t->NativeManifest == null) return null;
        nuint n = 0;
        t->NativeManifest(null, 0, &n);
        if (n > int.MaxValue - 1) return null;
        var buf = new byte[(int)n + 1];
        fixed (byte* p = buf)
            if (t->NativeManifest(p, (nuint)buf.Length, &n) != 0 || n > (nuint)buf.Length) return null;
        return _native = System.Text.Encoding.UTF8.GetString(buf, 0, (int)n);
    }

    /// <summary>The rows of a manifest. A missing "bodies" key (an older native build), a document that is not JSON and a row without a
    /// mod or an NPC name give no rows; a field of the wrong type reads as empty (or 0).</summary>
    internal static IReadOnlyList<NpcBodyRow> Parse(string? json)
    {
        if (string.IsNullOrWhiteSpace(json)) return [];
        var rows = new List<NpcBodyRow>();
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("bodies", out var bodies) ||
                bodies.ValueKind != JsonValueKind.Array) return [];
            foreach (var b in bodies.EnumerateArray())
            {
                if (b.ValueKind != JsonValueKind.Object) continue;
                string Text(string name) => b.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
                var mod = Text("mod");
                var npc = Text("npc");
                if (mod.Length == 0 || npc.Length == 0) continue;
                ulong generation = 0;
                if (b.TryGetProperty("generation", out var g))
                {
                    if (g.ValueKind == JsonValueKind.Number) g.TryGetUInt64(out generation);
                    else if (g.ValueKind == JsonValueKind.String) ulong.TryParse(g.GetString(), System.Globalization.NumberStyles.None,
                        System.Globalization.CultureInfo.InvariantCulture, out generation);
                }
                var key = Text("key");
                rows.Add(new NpcBodyRow(mod, npc, key.Length > 0 ? key : npc, Text("state"), Text("reason"), Text("template"), Text("model"),
                    Text("graph"), Text("closure").ToLowerInvariant(), generation));
            }
        }
        catch (JsonException)
        {
            return [];
        }
        return rows;
    }
}
