// SPDX-License-Identifier: MIT
// A mod's own value on entities, sent to the Kiln clients that have it on Kiln's channel kiln:repl (the dossier's chapter 15
// M2, "sidecars"; DESIGN "Wire"). This file: what every extension is whatever its value type (name, version, size, layout
// hash, the id this process gave it) and the per-peer "has it" decision from the handshake property repl.ext.
using System.Globalization;
using System.Runtime.CompilerServices;

namespace Kiln;

/// <summary>
/// A replicated extension: a mod's own value on entities (a struct of numbers), sent by the server to every Kiln client
/// that has the same extension, beside the game's own replication (<see cref="Replication.Extension{T}"/>). This base type
/// is what every extension is, whatever its value type: <see cref="Peers.With"/> and <see cref="Peers.Without"/> take it.
/// </summary>
/// <remarks>
/// <para>Two sides have the same extension when its <see cref="Name"/>, <see cref="Version"/>, <see cref="Size"/> and
/// <see cref="LayoutHash"/> are all equal. A Kiln client advertises its extensions when it joins (the handshake property
/// <c>repl.ext</c>); an extension that differs counts as missing for that peer, and the server says so once in its log
/// (<c>repl: extension &lt;name&gt; differs on peer 0x..: ...</c>).</para>
/// </remarks>
public abstract class ReplicatedExtension
{
    /// <summary>The handshake property a Kiln client advertises its extensions in: <c>name@version:size:hash8</c>
    /// joined by <c>;</c>.</summary>
    internal const string Property = "repl.ext";

    internal ReplicatedExtension(string name, int version, int size, uint layoutHash)
    {
        Name = name;
        Version = version;
        Size = size;
        LayoutHash = layoutHash;
    }

    /// <summary>The extension's name, unique in the process (by convention "&lt;mod id&gt;:&lt;name&gt;").</summary>
    public string Name { get; }

    /// <summary>Its version (1 or more): a value type that changes gets a new version.</summary>
    public int Version { get; }

    /// <summary>The value's size in bytes on the wire (at most 1,024).</summary>
    public int Size { get; }

    /// <summary>The value type's layout hash: FNV-1a-32 over every leaf field's path, type and offset
    /// (docs/kiln/services/replication.md "Sidecars").</summary>
    public uint LayoutHash { get; }

    /// <summary>The extension is registered: not disposed, and its mod is loaded.</summary>
    public bool IsRegistered => Volatile.Read(ref _registered);

    private bool _registered = true;

    /// <summary>The id this process gave it on the wire (kiln:repl DEFS); a new one after a hot reload. Tick thread.</summary>
    internal ushort Id { get; set; }

    /// <summary>Marks it gone (dispose, its mod's unload).</summary>
    internal void MarkUnregistered() => Volatile.Write(ref _registered, false);

    /// <summary>
    /// Whether <paramref name="peer"/> has this extension: a Kiln client whose handshake advertised the same name,
    /// version, size and layout hash. A vanilla client, a client still connecting and one without the extension (or with
    /// another version, size or layout of it) do not; the server logs a difference once per peer.
    /// </summary>
    /// <exception cref="ArgumentNullException"><paramref name="peer"/> is null.</exception>
    [ServerOnly]
    [TickThreadOnly]
    public bool IsAvailableFor(NetPeer peer)
    {
        ArgumentNullException.ThrowIfNull(peer);
        return IsRegistered && peer.State == NetPeerState.Ready && AvailableFor(peer);
    }

    /// <summary>The has-it decision without the state check (selectors check the state themselves).</summary>
    internal bool AvailableFor(NetPeer peer)
    {
        var advert = ExtensionAdvert.Of(peer);
        if (!advert.TryGetValue(Name, out var theirs)) return false;
        var difference = ExtensionAdvert.Difference(this, theirs);
        if (difference is null) return true;
        ExtensionAdvert.WarnOnce(peer.Id, Name, difference);
        return false;
    }

    /// <summary>What this side advertises for it: <c>name@version:size:hash8</c>.</summary>
    internal string Advertised => ExtensionAdvert.Format(Name, Version, Size, LayoutHash);

    /// <inheritdoc/>
    public override string ToString() => $"{Name}@{Version.ToString(CultureInfo.InvariantCulture)}";
}

/// <summary>One extension as the other side advertised it.</summary>
internal readonly record struct AdvertisedExtension(int Version, int Size, uint LayoutHash);

/// <summary>The handshake property <c>repl.ext</c>: <c>name@version:size:hash8</c> joined by <c>;</c>.</summary>
internal static class ExtensionAdvert
{
    private static readonly ConditionalWeakTable<IReadOnlyDictionary<string, string>, IReadOnlyDictionary<string, AdvertisedExtension>> Cache = new();
    private static readonly HashSet<(uint Peer, string Name, string What)> Warned = [];
    private static readonly IReadOnlyDictionary<string, AdvertisedExtension> None = new Dictionary<string, AdvertisedExtension>();

    internal static string Format(string name, int version, int size, uint hash) =>
        string.Create(CultureInfo.InvariantCulture, $"{name}@{version}:{size}:{hash:x8}");

    /// <summary>The value of the property for these extensions (empty: none).</summary>
    internal static string Join(IEnumerable<ReplicatedExtension> extensions) => string.Join(";", extensions.Select(e => e.Advertised));

    /// <summary>What a peer advertised (parsed once per handshake).</summary>
    internal static IReadOnlyDictionary<string, AdvertisedExtension> Of(NetPeer peer)
    {
        var props = peer.Properties;
        if (!props.TryGetValue(ReplicatedExtension.Property, out var value) || string.IsNullOrEmpty(value)) return None;
        lock (Cache) return Cache.GetValue(props, _ => Parse(value));
    }

    /// <summary>The entries of a property value; malformed entries are skipped (a later Kiln may add fields after them).</summary>
    internal static IReadOnlyDictionary<string, AdvertisedExtension> Parse(string value)
    {
        var d = new Dictionary<string, AdvertisedExtension>(StringComparer.Ordinal);
        foreach (var entry in value.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var at = entry.LastIndexOf('@');
            if (at <= 0) continue;
            var parts = entry[(at + 1)..].Split(':');
            if (parts.Length < 3 ||
                !int.TryParse(parts[0], NumberStyles.None, CultureInfo.InvariantCulture, out var version) ||
                !int.TryParse(parts[1], NumberStyles.None, CultureInfo.InvariantCulture, out var size) ||
                !uint.TryParse(parts[2], NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out var hash))
                continue;
            d[entry[..at]] = new AdvertisedExtension(version, size, hash);
        }
        return d;
    }

    /// <summary>Why the peer's entry is not this extension (null: it is).</summary>
    internal static string? Difference(ReplicatedExtension mine, AdvertisedExtension theirs) =>
        theirs.Version != mine.Version ? $"version {mine.Version} here, {theirs.Version} there" :
        theirs.Size != mine.Size ? $"size {mine.Size} here, {theirs.Size} there" :
        theirs.LayoutHash != mine.LayoutHash ? $"layout {mine.LayoutHash:x8} here, {theirs.LayoutHash:x8} there" :
        null;

    /// <summary>"repl: extension &lt;name&gt; differs on peer 0x..: ..." once per peer, extension and difference.</summary>
    internal static void WarnOnce(uint peer, string name, string what)
    {
        lock (Warned)
            if (!Warned.Add((peer, name, what))) return;
        try
        {
            Backend.Current.Log(0, LogLevel.Warn, $"repl: extension {name} differs on peer 0x{peer:x}: {what}");
        }
        catch
        {
            // a log that cannot be written must not change the decision
        }
    }

    /// <summary>Tests: forget the warnings given.</summary>
    internal static void ResetWarnings()
    {
        lock (Warned) Warned.Clear();
    }
}
