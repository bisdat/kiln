// SPDX-License-Identifier: MIT
// A mod's own value on entities, sent to the Kiln clients that have it on Kiln's channel kiln:repl (the dossier's chapter 15
// M2, "sidecars"; DESIGN "Wire"). This file: what every extension is whatever its value type (name, version, size, layout
// hash, the id this process gave it) and the per-peer "has it" decision from the handshake property repl.ext.
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

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

    /// <summary>The id this process gave it on the wire (kiln:repl DEFS); a new one after a hot reload.</summary>
    internal ushort Id { get; init; }

    /// <summary>The mod that registered it (null: Kiln's own code).</summary>
    internal ModContext? Owner { get; init; }

    /// <summary>The values this process knows, by entity (a remote client: what the server sent for the generation it
    /// has; a server or a hosted world: what the providers gave last). Replaced, never changed: TryGet reads it from any
    /// thread.</summary>
    private IReadOnlyDictionary<uint, byte[]> _view = EmptyView;

    private static readonly IReadOnlyDictionary<uint, byte[]> EmptyView = new Dictionary<uint, byte[]>();

    /// <summary>Publishes a new view (once per frame, by the part that owns the values here).</summary>
    internal void Publish(IReadOnlyDictionary<uint, byte[]> view) => Volatile.Write(ref _view, view);

    /// <summary>The value bytes of an entity in the current view.</summary>
    internal bool TryView(uint entity, out byte[] bytes)
    {
        if (Volatile.Read(ref _view).TryGetValue(entity, out var b) && b.Length == Size)
        {
            bytes = b;
            return true;
        }
        bytes = [];
        return false;
    }

    /// <summary>The providers (server): component hash, name, and how to call one.</summary>
    internal abstract IReadOnlyList<SidecarProvider> Providers { get; }

    /// <summary>Raises Changed (the owner's mod as the current mod; a handler's exception is logged, never thrown on).
    /// <paramref name="value"/> empty: the value went.</summary>
    internal abstract void RaiseChanged(EntityId entity, uint createTick, ReadOnlySpan<byte> value, NetPeer? peer);

    /// <summary>Marks it gone (dispose, its mod's unload); its view empties.</summary>
    internal void MarkUnregistered()
    {
        Volatile.Write(ref _registered, false);
        Volatile.Write(ref _view, EmptyView);
    }

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

/// <summary>
/// Computes an extension's value for one entity on the server (tick thread). Fill <paramref name="value"/> and return
/// true to send it; return false when the entity has no value (a value sent before is cleared on the clients).
/// <paramref name="value"/> starts as default.
/// </summary>
public delegate bool ExtensionProvider<T>(EntityId entity, ref T value) where T : unmanaged;

/// <summary>An extension's value changed: on a server, a value was sent to <see cref="Peer"/> (or cleared there); on a
/// client, a value arrived from the server or went (<see cref="Value"/> null).</summary>
/// <param name="Entity">The entity (the server's id).</param>
/// <param name="CreateTick">The entity's generation on that peer (the server tick of its create; 0 when the server's
/// tracking began after it).</param>
/// <param name="Value">The new value; null when it went (cleared, the entity was destroyed or left the peer's view).</param>
/// <param name="Peer">Server: the peer it was sent to (null for a hosted world's own client). Client: null.</param>
public sealed record ReplExtensionChanged<T>(EntityId Entity, uint CreateTick, T? Value, NetPeer? Peer) where T : struct;

/// <summary>One provider of an extension (server).</summary>
internal sealed class SidecarProvider(uint componentHash, string componentName, Func<EntityId, byte[], bool> provide, ModContext? mod)
{
    public uint ComponentHash { get; } = componentHash;
    public string ComponentName { get; } = componentName;
    public ModContext? Mod { get; } = mod;
    public int Faults;
    public volatile bool Removed;

    /// <summary>Runs the provider into <paramref name="into"/> (Size bytes, zeroed first). False: no value.</summary>
    public bool Provide(EntityId entity, byte[] into) => provide(entity, into);
}

/// <summary>
/// A replicated extension with values of <typeparamref name="T"/> (<see cref="Replication.Extension{T}"/>). The server
/// computes them (<see cref="On{TComponent}"/>) for the entities each Kiln client with the extension was sent, and
/// sends a value again only when it changed; a client reads them with <see cref="TryGet"/>.
/// </summary>
public sealed class ReplicatedExtension<T> : ReplicatedExtension, IDisposable where T : unmanaged
{
    private SidecarProvider[] _providers = [];
    private readonly object _gate = new();
    private Action<ReplExtensionChanged<T>>? _changed;

    internal ReplicatedExtension(string name, int version, int size, uint layoutHash, ModContext? owner)
        : base(name, version, size, layoutHash) => Owner = owner;

    internal override IReadOnlyList<SidecarProvider> Providers => Volatile.Read(ref _providers);

    /// <summary>
    /// Computes the value on entities that have component <typeparamref name="TComponent"/>: Kiln calls
    /// <paramref name="provider"/> on the server's tick thread once per step for every such entity that some Kiln client with
    /// this extension was sent, and sends the value to those clients when it differs from the last one they got. With
    /// several providers, the first registered whose component the entity has gives the value.
    /// </summary>
    /// <typeparam name="TComponent">A game component (a Kiln.Game type, such as <c>Kiln.Ecs.Fuel</c>).</typeparam>
    /// <returns>Dispose it to remove the provider (the values it gave are cleared on the clients).</returns>
    /// <remarks>On a game client that does not host, the provider is kept and never called (only a server computes
    /// values), so one OnLoad can serve both sides.</remarks>
    /// <exception cref="ArgumentNullException"><paramref name="provider"/> is null.</exception>
    /// <exception cref="ObjectDisposedException">The extension was disposed or its mod unloaded.</exception>
    /// <exception cref="KilnUnavailableException"><c>repl.sidecar</c> is off on this server (its tracking or kiln.net's
    /// transport).</exception>
    [ServerOnly]
    [TickThreadOnly]
    [RequiresCapability(ReplicationCaps.Sidecar)]
    public IDisposable On<TComponent>(ExtensionProvider<T> provider) where TComponent : unmanaged, IGameComponent<TComponent>
    {
        ArgumentNullException.ThrowIfNull(provider);
        ObjectDisposedException.ThrowIf(!IsRegistered, this);
        Sidecars.RequireProviders();
        var mod = ModContext.CurrentMod ?? Owner;
        var entry = new SidecarProvider(TComponent.Hash, TComponent.Name, (entity, into) =>
        {
            var v = default(T);
            if (!provider(entity, ref v)) return false;
            MemoryMarshal.Write(into, in v);
            return true;
        }, mod);
        lock (_gate) Volatile.Write(ref _providers, [.. _providers, entry]);
        Sidecars.ProvidersChanged(this);
        return new ProviderRegistration(this, entry);
    }

    private sealed class ProviderRegistration(ReplicatedExtension<T> ext, SidecarProvider entry) : IDisposable
    {
        private int _done;

        public void Dispose()
        {
            if (Interlocked.Exchange(ref _done, 1) != 0) return;
            entry.Removed = true;
            lock (ext._gate) Volatile.Write(ref ext._providers, Array.FindAll(ext._providers, p => !ReferenceEquals(p, entry)));
            Sidecars.ProvidersChanged(ext);
        }
    }

    /// <summary>
    /// The value of <paramref name="entity"/> as this process knows it: on a Kiln client, the last value the server sent
    /// for the generation of the entity the client has; on a server or a hosted world, the last value the providers gave.
    /// The values are swapped in once per frame, so a read never sees a half-written value.
    /// </summary>
    /// <returns>False when there is none (no provider gives one, the server has not sent it yet, the entity is gone, or the
    /// extension is off here).</returns>
    [AnyThread]
    public bool TryGet(EntityId entity, out T value)
    {
        if (TryView(entity.Value, out var b))
        {
            value = MemoryMarshal.Read<T>(b);
            return true;
        }
        value = default;
        return false;
    }

    /// <summary>A value changed. Server: raised on the tick thread for every value sent to a peer (and cleared there).
    /// Client: raised on the main thread when a value from the server arrives or goes. A handler's exception is logged.</summary>
    public event Action<ReplExtensionChanged<T>>? Changed
    {
        add { lock (_gate) _changed += value; }
        remove { lock (_gate) _changed -= value; }
    }

    internal override void RaiseChanged(EntityId entity, uint createTick, ReadOnlySpan<byte> value, NetPeer? peer)
    {
        var handlers = Volatile.Read(ref _changed);
        if (handlers is null) return;
        T? v = value.Length == Size ? MemoryMarshal.Read<T>(value) : null;
        var e = new ReplExtensionChanged<T>(entity, createTick, v, peer);
        var outer = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = Owner;
            foreach (var h in handlers.GetInvocationList())
            {
                try
                {
                    ((Action<ReplExtensionChanged<T>>)h)(e);
                }
                catch (Exception ex)
                {
                    Sidecars.Report(Owner, $"a Changed handler of extension {this} threw", ex);
                }
            }
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>Removes the extension: the server stops sending it (clients drop its values at the next DEFS) and its
    /// name is free again. Also done when its mod unloads. Idempotent.</summary>
    public void Dispose() => Sidecars.Unregister(this);
}
