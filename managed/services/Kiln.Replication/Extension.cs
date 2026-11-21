// SPDX-License-Identifier: MIT
// Replication.Extension: a mod's own value on entities, sent to the Kiln clients that have it (the dossier's chapter 15
// M2, "sidecars"; DESIGN "Public managed API", "Wire"). This file: the registration (the value type's rule, the name, the
// version, the id this process gives it) and the process's list of extensions, which the handshake property repl.ext
// advertises and the server's DEFS name.
using System.Text.RegularExpressions;

namespace Kiln;

public static partial class Replication
{
    /// <summary>
    /// Registers a replicated extension: a value of <typeparamref name="T"/> on entities, which the server computes
    /// (<see cref="ReplicatedExtension{T}.On{TComponent}"/>) and sends on Kiln's channel to every Kiln client that has the
    /// same extension, and which those clients read with <see cref="ReplicatedExtension{T}.TryGet"/>. Register it on both
    /// sides with the same name, version and type, in OnLoad. A vanilla client never gets anything.
    /// </summary>
    /// <typeparam name="T">The value: an unmanaged struct with <c>[StructLayout(LayoutKind.Sequential, Pack = 1)]</c> made
    /// only of byte, sbyte, short, ushort, int, uint, long, ulong, float, double, enums of those and nested structs under
    /// the same rule; at most 1,024 bytes (analyzer rule KILN199).</typeparam>
    /// <param name="name">Unique in the process, 1 to 64 characters of letters, digits and <c>_ . : / -</c>; by convention
    /// "&lt;mod id&gt;:&lt;name&gt;".</param>
    /// <param name="version">1 to 65,535. Give the type a new version when it changes: a client with another version, size or
    /// layout of the extension counts as not having it.</param>
    /// <returns>The extension. It is removed when its mod unloads, or when it is disposed.</returns>
    /// <exception cref="ArgumentException"><typeparamref name="T"/> breaks the rule above, the name is malformed or taken
    /// by another extension of this process, the version is out of range, or 1,024 extensions are registered.</exception>
    /// <remarks>The definition is kept whether or not values can travel here: capability <c>repl.sidecar</c> says whether
    /// they do (the server's tracking and kiln.net's transport; on a client, its receive side).</remarks>
    [TickThreadOnly]
    public static ReplicatedExtension<T> Extension<T>(string name, int version) where T : unmanaged =>
        Sidecars.Register<T>(name, version);
}

/// <summary>The process's replicated extensions (registration order) and their wire ids.</summary>
internal static partial class Sidecars
{
    internal const int MaxExtensions = 1024;
    private static readonly Regex NameRule = new("^[A-Za-z0-9_.:/-]{1,64}$", RegexOptions.CultureInvariant);
    private static readonly object Gate = new();
    private static ReplicatedExtension[] _live = [];
    private static ushort _lastId;
    private static int _version;

    /// <summary>The registered extensions, in registration order (a snapshot).</summary>
    internal static ReplicatedExtension[] Registered() => Volatile.Read(ref _live);

    /// <summary>Changes whenever an extension comes or goes (the server sends DEFS again).</summary>
    internal static int Version => Volatile.Read(ref _version);

    internal static ReplicatedExtension? ById(ushort id)
    {
        foreach (var e in Registered())
            if (e.Id == id) return e;
        return null;
    }

    internal static ReplicatedExtension? ByName(string name)
    {
        foreach (var e in Registered())
            if (e.Name == name) return e;
        return null;
    }

    internal static ReplicatedExtension<T> Register<T>(string name, int version) where T : unmanaged
    {
        if (name is null || !NameRule.IsMatch(name))
            throw new ArgumentException($"extension name '{name}' must be 1 to 64 letters, digits or _ . : / -", nameof(name));
        if (version is < 1 or > ushort.MaxValue) throw new ArgumentException($"extension version {version} must be 1 to 65535", nameof(version));
        var (size, hash) = LayoutHash.Of<T>();
        var mod = ModContext.CurrentMod;
        ReplicatedExtension<T> ext;
        lock (Gate)
        {
            if (Array.Find(_live, e => e.Name == name) is { } taken)
                throw new ArgumentException($"extension name {name} is taken by {taken.Owner?.Info.Id ?? "kiln"}", nameof(name));
            if (_live.Length >= MaxExtensions) throw new ArgumentException($"{MaxExtensions} extensions are registered already", nameof(name));
            ext = new ReplicatedExtension<T>(name, version, size, hash, mod) { Id = NextId() };
            _live = [.. _live, ext];
            _version++;
        }
        if (mod is not null) mod.Retiring += ext.Dispose;
        Started();
        return ext;
    }

    /// <summary>The next free id (1 .. 65,535; ids of live extensions are skipped after a wrap).</summary>
    private static ushort NextId()
    {
        for (var i = 0; i < ushort.MaxValue; i++)
        {
            _lastId = (ushort)(_lastId == ushort.MaxValue ? 1 : _lastId + 1);
            if (Array.Find(_live, e => e.Id == _lastId) is null) return _lastId;
        }
        throw new ArgumentException("no free extension id");
    }

    internal static void Unregister(ReplicatedExtension ext)
    {
        lock (Gate)
        {
            if (!ext.IsRegistered) return;
            ext.MarkUnregistered();
            _live = Array.FindAll(_live, e => !ReferenceEquals(e, ext));
            _version++;
        }
        Removed(ext);
    }

    /// <summary>The value of the handshake property repl.ext: what this process has.</summary>
    internal static string AdvertisedValue() => ExtensionAdvert.Join(Registered());

    /// <summary>The DEFS of this process, now.</summary>
    internal static List<SidecarWire.Def> Defs() =>
        Registered().Select(e => new SidecarWire.Def(e.Id, e.Name, (ushort)e.Version, (ushort)e.Size, e.LayoutHash)).ToList();

    // ---- the channel and the handshake property ----
    private static int _started;
    private static NetChannel? _channel;

    /// <summary>kiln:repl v1 (null until the first extension, or when kiln.net could not open it).</summary>
    internal static NetChannel? Channel => Volatile.Read(ref _channel);

    /// <summary>The first registration opens kiln:repl (required: false: a client without it is let in and gets nothing)
    /// and advertises repl.ext; both are Kiln's own and outlive the mod that came first.</summary>
    private static void Started()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        var outer = ModContext.CurrentMod;
        try
        {
            ModContext.CurrentMod = null;
            Network.SetHandshakeProperty(ReplicatedExtension.Property, AdvertisedValue);
            var ch = Network.Channel(SidecarWire.Channel, SidecarWire.ChannelVersion, required: false);
            ch.OnRaw(OnMessage);
            Volatile.Write(ref _channel, ch);
        }
        catch (Exception e)
        {
            Log(LogLevel.Warn, $"repl: kiln:repl could not be opened ({e.GetType().Name}: {e.Message}); extension values do not travel");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
        StartParts();
    }

    /// <summary>A kiln:repl message (server: tick thread; client: main thread).</summary>
    private static void OnMessage(NetContext context, ushort id, ReadOnlySpan<byte> body) => Received(context, id, body);

    static partial void StartParts();

    static partial void Received(NetContext context, ushort id, ReadOnlySpan<byte> body);

    /// <summary>An extension went: its values go with it.</summary>
    static partial void Removed(ReplicatedExtension ext);

    /// <summary>A provider came or went.</summary>
    internal static void ProvidersChanged(ReplicatedExtension ext) => ProvidersMoved(ext);

    static partial void ProvidersMoved(ReplicatedExtension ext);

    /// <summary>Tests: kiln.repl.net's capabilities as the providers' check sees them (null: the real ones).</summary>
    internal static uint? TestCaps;

    /// <summary>On&lt;TComponent&gt;'s capability check: a server (or a hosted world) needs the send side's tracking; a game
    /// client that does not host keeps the provider without it.</summary>
    internal static void RequireProviders()
    {
        if (TestCaps is { } test)
        {
            if ((test & ReplNetNative.CapTrack) == 0) throw new KilnUnavailableException(ReplicationCaps.Sidecar, "off in this test");
            return;
        }
        ReplicationCaps.Rows.Ensure();
        if ((ReplNetNative.Capabilities() & ReplNetNative.CapTrack) != 0 && Network.Available) return;
        if (Mods.Side == ModSide.Client) return;
        throw ReplicationCaps.Off(ReplicationCaps.Sidecar, "the server cannot track what each peer was sent on this build");
    }

    internal static void Log(LogLevel level, string line)
    {
        try
        {
            Backend.Current.Log(0, level, line);
        }
        catch
        {
            // no host (tests): the line goes nowhere
        }
    }

    /// <summary>A mod's handler or provider threw: its log says so (or Kiln's).</summary>
    internal static void Report(ModContext? mod, string what, Exception e)
    {
        try
        {
            if (mod is not null) mod.Log.Error(e, what);
            else Backend.Current.Log(0, LogLevel.Error, $"{what}: {e}");
        }
        catch
        {
            // logging must never unwind into the tick
        }
    }
}
