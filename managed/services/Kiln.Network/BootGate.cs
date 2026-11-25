// SPDX-License-Identifier: MIT
// Client and server must have made the same boot content of a mod both run (docs/kiln/network.md "Boot content at the join"). Mod.OnBoot
// runs before the connection, so a setting it reads (a resource patch's value: More Flame Altars' altar count) gives each process its own
// game data, and config sync comes too late to rebuild the one-shot boot generation. Kiln.Host fingerprints what each side-both boot mod's
// generation committed (managed/Kiln.Host/BootFingerprint.cs, read here by reflection: Kiln.Network does not reference Kiln.Host); HELLO
// and WELCOME carry the fingerprints as one handshake property, and the server's mod check treats a difference of a mod both have in the
// same version as it treats a version difference (NetEngine.HandleHello). A side that sends no fingerprint for a mod (an older Kiln, a mod
// that is not a boot mod there, one left out of its generation) compares nothing.
using System.Reflection;
using System.Text;

namespace Kiln;

internal static class BootGate
{
    /// <summary>The handshake property: <c>id=&lt;16 hex&gt;</c> or <c>id=out</c>, comma-separated, ids in ordinal order.</summary>
    public const string Key = "kiln.boot";
    internal const string RegistryKey = "kiln.registry.boot";
    private static Func<string?>? _registryHost;

    private static string? RegistryOwn()
    {
        try
        {
            if (_registryHost is null)
            {
                var host = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "Kiln.Host");
                var method = host?.GetType("Kiln.Host.BootFingerprint")?.GetMethod("RegistryCurrent", BindingFlags.Static | BindingFlags.NonPublic, Type.EmptyTypes);
                if (method?.ReturnType == typeof(string)) _registryHost = method.CreateDelegate<Func<string?>>();
            }
            return RegistryValue(_registryHost?.Invoke());
        }
        catch (Exception) { return null; }
    }

    private static string? RegistryValue(string? value) => value is { Length: 131 } && value.StartsWith("1:", StringComparison.Ordinal) &&
        value[66] == ':' && value.AsSpan(2, 64).IndexOfAnyExcept("0123456789abcdef") < 0 &&
        value.AsSpan(67, 64).IndexOfAnyExcept("0123456789abcdef") < 0 ? value : null;

    /// <summary>The registry part's message, or null when it is not compared or agrees. It is one hash over every participating mod, so it
    /// cannot honour the mod-list rules per mod: it is compared only when both sides run the same side-both mods in the same versions
    /// (<see cref="SameSharedMods"/>). Otherwise the missing/versionMatch/optionalMods/extra rules decide alone, as before.</summary>
    internal static string? RegistryDifference(IReadOnlyDictionary<string, string> properties, IEnumerable<Wire.ModEntry> serverMods,
        IEnumerable<Wire.ModEntry> clientMods)
    {
        if (!SameSharedMods(serverMods, clientMods)) return null;
        var own = RegistryOwn();
        var peer = RegistryValue(properties.GetValueOrDefault(RegistryKey));
        if (own is null || peer is null) return null; // additive: old peers compare nothing
        if (!own.AsSpan(2, 64).SequenceEqual(peer.AsSpan(2, 64)))
            return "The registry boot values differ from this server's. Use the server's modpack and settings, then restart the game.";
        if (!own.AsSpan(67, 64).SequenceEqual(peer.AsSpan(67, 64)))
            return "The registry boot edits or load order differ from this server's. Use the server's modpack and load order, then restart the game.";
        return null;
    }

    /// <summary>Both lists hold the same side-both mods (ids and exact versions). Side client and server mods are not compared: each runs
    /// on one side only, and the native stage leaves the fields they write out of the registry part.</summary>
    internal static bool SameSharedMods(IEnumerable<Wire.ModEntry> serverMods, IEnumerable<Wire.ModEntry> clientMods)
    {
        static Dictionary<string, string> Shared(IEnumerable<Wire.ModEntry> mods) => mods.Where(m => m.Side == ModSide.Both)
            .GroupBy(m => m.Id, StringComparer.Ordinal).ToDictionary(g => g.Key, g => g.First().Version, StringComparer.Ordinal);
        var s = Shared(serverMods);
        var c = Shared(clientMods);
        return s.Count == c.Count && s.All(kv => c.TryGetValue(kv.Key, out var v) && string.Equals(v, kv.Value, StringComparison.Ordinal));
    }

    /// <summary>A boot mod in this process's generation that was left out of it (or whose content faulted since).</summary>
    public const string Out = "out";

    /// <summary>Hex digits of the SHA-256 sent (64 bits: the other gates' size).</summary>
    public const int HashChars = 16;

    /// <summary>The value's budget. HELLO and WELCOME are single frames (WELCOME's 7 KB budget, NetAdvertise.FrameBudget, counts it),
    /// and a decoder of 0.2.0-dev.28 refuses a value over 4096 bytes. A mod that does not fit is not sent, so it is not compared.</summary>
    public const int MaxValueBytes = 2048;

    /// <summary>Tests: this process's fingerprints instead of Kiln.Host's (id -> 64 hex digits or <see cref="Out"/>).</summary>
    internal static Func<IReadOnlyDictionary<string, string>>? TestSource;

    private static volatile Func<IReadOnlyDictionary<string, string>>? _host;
    private static int _cutLogged, _seamLogged;

    /// <summary>Kiln.Host's BootFingerprint.Current among the loaded assemblies, kept once found. In a game Kiln.Host is there before
    /// any service; a process without it (a unit test) looks again at each handshake and finds nothing.</summary>
    private static Func<IReadOnlyDictionary<string, string>>? Host()
    {
        if (_host is { } found) return found;
        try
        {
            var host = AppDomain.CurrentDomain.GetAssemblies().FirstOrDefault(a => a.GetName().Name == "Kiln.Host");
            var current = host?.GetType("Kiln.Host.BootFingerprint")?.GetMethod("Current", BindingFlags.Static | BindingFlags.NonPublic, Type.EmptyTypes);
            if (current?.ReturnType == typeof(IReadOnlyDictionary<string, string>))
                return _host = current.CreateDelegate<Func<IReadOnlyDictionary<string, string>>>();
            if (host != null && Interlocked.Exchange(ref _seamLogged, 1) == 0)
                NetEngine.Log(LogLevel.Warn, "boot content: this Kiln.Host has no boot fingerprints: boot content is not compared at the join");
        }
        catch (Exception e)
        {
            if (Interlocked.Exchange(ref _seamLogged, 1) == 0)
                NetEngine.Log(LogLevel.Warn, $"boot content: Kiln.Host's fingerprints cannot be read ({e.Message}): boot content is not compared at the join");
        }
        return null;
    }

    /// <summary>This process's side-both boot mods: id -> the first <see cref="HashChars"/> hex digits of its fingerprint, or
    /// <see cref="Out"/>. Empty without a boot generation (no boot mod, or no boot phase).</summary>
    public static IReadOnlyDictionary<string, string> Own()
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        IReadOnlyDictionary<string, string>? raw;
        try
        {
            raw = (TestSource ?? Host())?.Invoke();
        }
        catch (Exception)
        {
            raw = null;
        }
        if (raw is null) return d;
        foreach (var (id, fp) in raw)
        {
            if (!BootGeneration.IdPattern.IsMatch(id)) continue;
            if (fp == Out) d[id] = Out;
            else if (fp is { Length: >= HashChars } && fp.AsSpan(0, HashChars).IndexOfAnyExcept("0123456789abcdef") < 0) d[id] = fp[..HashChars];
        }
        return d;
    }

    /// <summary>What goes in HELLO and WELCOME: nothing without a boot generation, so a process without boot mods sends what it sent
    /// before.</summary>
    public static Dictionary<string, string> Properties()
    {
        var own = Own();
        var value = Encode(own, out var left);
        if (left > 0 && Interlocked.Exchange(ref _cutLogged, 1) == 0)
            NetEngine.Log(LogLevel.Warn, $"boot content: {left} boot mod(s) do not fit the handshake ({MaxValueBytes} bytes): their boot content is not compared at the join");
        var properties = new Dictionary<string, string>(StringComparer.Ordinal);
        if (value.Length != 0) properties[Key] = value;
        if (RegistryOwn() is { } registry) properties[RegistryKey] = registry;
        return properties;
    }

    /// <summary>The property's value: <c>id=fp</c> pairs in ordinal id order, within <see cref="MaxValueBytes"/>; <paramref name="left"/>:
    /// how many did not fit. Ids and fingerprints are ASCII, so characters are bytes.</summary>
    internal static string Encode(IReadOnlyDictionary<string, string> fingerprints, out int left)
    {
        var sb = new StringBuilder();
        left = 0;
        foreach (var (id, fp) in fingerprints.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            var cost = (sb.Length > 0 ? 1 : 0) + id.Length + 1 + fp.Length;
            if (sb.Length + cost > MaxValueBytes)
            {
                left++;
                continue;
            }
            if (sb.Length > 0) sb.Append(',');
            sb.Append(id).Append('=').Append(fp);
        }
        return sb.ToString();
    }

    /// <summary>A peer's property back: id -> 16 lower-case hex digits or <see cref="Out"/>. Absent (an older Kiln) or malformed entries
    /// give nothing; of an id sent twice the first counts.</summary>
    internal static Dictionary<string, string> Decode(string? value)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        if (string.IsNullOrEmpty(value)) return d;
        foreach (var part in value.Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            var eq = part.IndexOf('=');
            if (eq <= 0) continue;
            var id = part[..eq].Trim();
            var fp = part[(eq + 1)..].Trim().ToLowerInvariant();
            if (!BootGeneration.IdPattern.IsMatch(id) || d.ContainsKey(id)) continue;
            if (fp == Out || (fp.Length == HashChars && fp.AsSpan().IndexOfAnyExcept("0123456789abcdef") < 0)) d[id] = fp;
        }
        return d;
    }

    /// <summary>Whether mod <paramref name="id"/>'s boot content differs: both sides sent a fingerprint of a generation that committed it,
    /// and they are not the same. Anything missing on either side is no difference.</summary>
    internal static bool Differs(string id, IReadOnlyDictionary<string, string> server, IReadOnlyDictionary<string, string> client) =>
        server.TryGetValue(id, out var s) && client.TryGetValue(id, out var c) && s != Out && c != Out && s != c;

    /// <summary>The mods one side left out of its boot generation and the other committed, as notes for the server's log line. The
    /// decision stays with the other rules: the side that left one out has not loaded it.</summary>
    internal static List<string> LeftOut(IReadOnlyDictionary<string, string> server, IReadOnlyDictionary<string, string> client)
    {
        var notes = new List<string>();
        foreach (var (id, s) in server.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            if (!client.TryGetValue(id, out var c) || (s == Out) == (c == Out)) continue;
            notes.Add(s == Out
                ? $"{id} is left out of this server's boot generation and committed on the client"
                : $"{id} is left out of the client's boot generation and committed here");
        }
        return notes;
    }

    /// <summary>The mismatch as one entry of the hello's missing list (REJECT details, NetClientHandshakeEvent.Missing, the log). Not
    /// "(missing)" or "(client has ...)": the client has the mod, so NetRefusal does not count it as one to install.</summary>
    internal static string Detail(string id, string version) =>
        $"{id} {version} (boot content differs: its settings or files differ from the server's; use the server's settings and restart the game)";

    /// <summary>The WELCOME warning for a mismatch the policy lets in.</summary>
    internal static string Warning(string name, string id, string version) =>
        $"boot content of {name} ({id} {version}) differs from the server's: its settings or files differ on this game. " +
        $"Use the server's settings of {name} and restart the game";

    /// <summary>The REJECT reason for mismatches the policy refuses, by the mods' names.</summary>
    internal static string Reason(IReadOnlyList<string> names)
    {
        var shown = names.Take(4).ToList();
        var list = shown.Count == 1 ? shown[0] : string.Join(", ", shown.Take(shown.Count - 1)) + " and " + shown[^1];
        if (names.Count > shown.Count) list += $" (and {names.Count - shown.Count} more)";
        var its = names.Count == 1 ? "its" : "their";
        return $"The boot content of {list} on your game differs from this server's: {its} settings (or {its} files) differ. " +
               $"Use this server's settings of {list} and restart the game.";
    }
}
