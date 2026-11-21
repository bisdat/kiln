// SPDX-License-Identifier: MIT
// The owner-only stats protocol (chapter 05b section 4.4): channel kiln:mod-values v1, optional, 16 KiB messages, six
// JSON message types registered in this exact order on both sides, camelCase, 64-bit numbers as base-10 strings.
//
//   0 Hello           server -> client  a new stream: nonce, world, character, status
//   1 StatePart       server -> client  one part (<= 8 KiB decoded) of a full or delta state document
//   2 Ack             client -> server  a whole transfer is applied
//   3 Resync          client -> server  something did not fit: send a full
//   4 CarryOfferPart  client -> server  one part of the client's carried projection (progress from other worlds)
//   5 CarryResult     server -> client  what the offer did
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.Json.Serialization.Metadata;

namespace Kiln;

internal static class ModValuesWire
{
    internal const string Channel = "kiln:mod-values", Version = "1";
    internal const int MaxMessageBytes = 16 * 1024, PartBytes = 8192, MaxStateBytes = 1024 * 1024, MaxParts = 128, MaxValues = 1024;
    internal const int MaxOfferBytes = 64 * 1024, MaxOfferParts = 8, MaxResultBytes = 1024;
    internal const string Ready = "ready", TooLarge = "snapshotTooLarge";

    internal static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.Never,
        TypeInfoResolver = new DefaultJsonTypeInfoResolver(),
        MaxDepth = 16,
    };

    internal static JsonTypeInfo<T> Info<T>() => (JsonTypeInfo<T>)Options.GetTypeInfo(typeof(T));

    internal sealed record Hello(string Nonce, string World, string Character, string Status, int MaxStateBytes, int PartBytes);

    internal sealed record StatePart(string Nonce, string Transfer, string Seq, string BaseSeq, bool Full, int Part, int Parts, int TotalBytes,
        string Sha256, string PayloadB64);

    internal sealed record Ack(string Nonce, string Seq, string Transfer);

    internal sealed record Resync(string Nonce, string LastApplied, string Reason);

    internal sealed record CarryOfferPart(string Nonce, string Transfer, int Part, int Parts, int TotalBytes, string Sha256, string PayloadB64);

    internal sealed record CarryResult(string Nonce, string Transfer, bool Ok, int Applied, int Unchanged, int Rejected, Dictionary<string, int> ReasonCounts);

    internal sealed class Definition
    {
        public string Key { get; set; } = "";
        public uint Version { get; set; }
        public uint Kind { get; set; }
        public uint Persistence { get; set; }
        public uint Scale { get; set; }
        public uint CarryEpoch { get; set; }
        public IDictionary<string, string> Label { get; set; } = new SortedDictionary<string, string>(StringComparer.Ordinal);
        public IDictionary<string, string> NumberFormat { get; set; } = new SortedDictionary<string, string>(StringComparer.Ordinal);
        public uint ReadPolicy { get; set; }
        public string[] ReadMods { get; set; } = [];
        public uint Sync { get; set; }
    }

    internal sealed class Value
    {
        public string Key { get; set; } = "";
        public string Raw { get; set; } = "0";
        public int Level { get; set; }
        public string ValueRevision { get; set; } = "0";
        public string PartitionRevision { get; set; } = "0";
    }

    internal sealed class Payload
    {
        public int Schema { get; set; } = 1;
        public List<Definition> Definitions { get; set; } = [];
        public List<Value> Values { get; set; } = [];
        public List<string> Removed { get; set; } = [];
    }

    internal sealed class OfferPayload
    {
        public int Schema { get; set; } = 1;
        public string SidecarRevision { get; set; } = "0";
        public List<OfferValue> Entries { get; set; } = [];
    }

    internal sealed class OfferValue
    {
        public string Key { get; set; } = "";
        public string Raw { get; set; } = "0";
        public uint Kind { get; set; }
        public uint Scale { get; set; }
        public uint DefinitionVersion { get; set; }
        public uint CarryEpoch { get; set; }
    }

    internal static string U64(ulong v) => v.ToString(System.Globalization.CultureInfo.InvariantCulture);

    internal static string I64(long v) => v.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// <summary>Canonical base-10 only: no sign on unsigned, no leading zeros, no spaces.</summary>
    internal static bool TryU64(string? s, out ulong v)
    {
        v = 0;
        return s is { Length: > 0 and <= 20 } && (s == "0" || s[0] != '0') && s.All(char.IsAsciiDigit) &&
               ulong.TryParse(s, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out v);
    }

    internal static bool TryI64(string? s, out long v)
    {
        v = 0;
        if (s is null || s.Length is 0 or > 20) return false;
        var digits = s[0] == '-' ? s[1..] : s;
        if (digits.Length == 0 || digits.Length > 1 && digits[0] == '0' || !digits.All(char.IsAsciiDigit) || s == "-0") return false;
        return long.TryParse(s, System.Globalization.NumberStyles.AllowLeadingSign, System.Globalization.CultureInfo.InvariantCulture, out v);
    }

    internal static string NewNonce() => Convert.ToHexStringLower(RandomNumberGenerator.GetBytes(16));

    internal static string Sha(ReadOnlySpan<byte> doc) => Convert.ToHexStringLower(SHA256.HashData(doc));

    /// <summary>A document cut into parts of at most <paramref name="partBytes"/> decoded bytes.</summary>
    internal static List<byte[]> Cut(byte[] doc, int partBytes = PartBytes)
    {
        var parts = new List<byte[]>();
        for (var i = 0; i < doc.Length; i += partBytes) parts.Add(doc[i..Math.Min(doc.Length, i + partBytes)]);
        if (parts.Count == 0) parts.Add([]);
        return parts;
    }
}
