// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace Kiln;

/// <summary>sigdb symbol lookup and image scanning for advanced mods: core symbols, the mod's own shipped fragment,
/// and ad-hoc pattern scans in the running executable.</summary>
[RequiresCapability(HooksCaps.Service)]
public static unsafe class Symbols
{
    /// <summary>The image base of the running executable.</summary>
    public static nuint ImageBase => HooksNative.Require("Symbols.ImageBase")->ImageBase();

    /// <summary>The image size in bytes.</summary>
    public static nuint ImageSize => HooksNative.Require("Symbols.ImageSize")->ImageSize();

    /// <summary>Absolute address of the sigdb symbol <paramref name="name"/> (a core name, or
    /// "&lt;modid&gt;/name" from the mod's shipped fragment). Throws when it is unresolved on this build.</summary>
    public static nuint Get(string name)
        => TryGet(name, out var addr) ? addr
            : throw new KeyNotFoundException($"sigdb symbol '{name}' is unresolved on this build");

    public static bool TryGet(string name, out nuint address)
    {
        HooksGate.RequireAdvanced("Symbols.Get");
        var t = HooksNative.Require("Symbols.Get");
        nuint a = 0;
        var rc = HooksNative.WithUtf8(name, s => { nuint o; int r = t->Symbol(s, &o); a = o; return r; });
        address = a;
        return rc == HooksNative.Ok;
    }

    /// <summary>The single address matching <paramref name="pattern"/> (sigdb syntax; holes act as wildcards).
    /// Throws when there is not exactly one match.</summary>
    public static nuint Find(string pattern)
    {
        var all = FindAll(pattern, 2);
        return all.Length switch
        {
            1 => all[0],
            0 => throw new KeyNotFoundException($"pattern '{pattern}' matched nothing"),
            _ => throw new InvalidOperationException($"pattern '{pattern}' matched {all.Length}+ times; narrow it"),
        };
    }

    /// <summary>Up to <paramref name="max"/> addresses matching <paramref name="pattern"/>.</summary>
    public static nuint[] FindAll(string pattern, int max = 64)
    {
        HooksGate.RequireAdvanced("Symbols.Find");
        var t = HooksNative.Require("Symbols.Find");
        if (max < 1) max = 1;
        var buf = new nuint[max];
        uint count = 0;
        int rc;
        var patBytes = System.Text.Encoding.UTF8.GetBytes(pattern);
        fixed (nuint* p = buf)
        fixed (byte* pat = patBytes)
            rc = t->Scan(new NativeStr { Ptr = pat, Len = (nuint)patBytes.Length }, p, (uint)max, &count);
        if (rc == HooksNative.NotFound) return [];
        if (rc != HooksNative.Ok && rc != HooksNative.BufferTooSmall)
            throw new InvalidOperationException($"Symbols.Find('{pattern}'): {HooksNative.Describe(rc)}");
        return buf[..(int)count];
    }

    /// <summary>Value of a named hole in <paramref name="pattern"/> matched at <paramref name="match"/> (a rel32 hole
    /// gives the absolute target).</summary>
    public static long Hole(string pattern, nuint match, string hole)
    {
        HooksGate.RequireAdvanced("Symbols.Hole");
        var t = HooksNative.Require("Symbols.Hole");
        long v = 0;
        var rc = HooksNative.WithUtf8(pattern, ps =>
            HooksNative.WithUtf8(hole, hs => { long o; int r = t->ScanHole(ps, match, hs, &o); v = o; return r; }));
        if (rc != HooksNative.Ok) throw new KeyNotFoundException($"hole '{hole}' not found in a match of '{pattern}'");
        return v;
    }

    /// <summary>Loads the calling mod's shipped symbol fragment (<c>&lt;modDir&gt;/sigdb/server.json</c>) against the
    /// running exe, so its "&lt;modid&gt;/name" symbols resolve like Kiln's own (and coremods survive game updates the
    /// same way). Call once in <c>OnLoad</c>. Returns the resolution summary. Unloaded automatically on mod unload.</summary>
    public static SigSummary LoadShipped()
    {
        var mod = HooksGate.RequireAdvanced("Symbols.LoadShipped");
        var t = HooksNative.Require("Symbols.LoadShipped");
        // Pick the fragment for the side this process runs as, so a coremod ships server.json and/or client.json and
        // the right one resolves (the client hosts a server world, so client coremods are real).
        var side = Mods.Side == ModSide.Client ? "client" : "server";
        var path = Path.Combine(mod.Info.Directory, "sigdb", side + ".json");
        if (!File.Exists(path)) throw new FileNotFoundException($"mod '{mod.Info.Id}' ships no sigdb/{side}.json", path);
        HooksNative.SigSummary s;
        s.Size = (uint)sizeof(HooksNative.SigSummary);
        var sp = &s;
        var rc = HooksNative.WithUtf8(mod.Info.Id, idp =>
            HooksNative.WithUtf8(path, pp => t->LoadSymbols(mod.Owner, idp, pp, sp)));
        if (rc != HooksNative.Ok)
            throw new InvalidOperationException($"Symbols.LoadShipped: {HooksNative.Describe(rc)} (see kiln.log)");
        HooksGate.Track(mod).SymbolsLoaded = true;
        ReadFragmentSigs(mod);
        var r = new SigSummary((int)s.Symbols, (int)s.SymbolsOk, (int)s.Constants, (int)s.ConstantsOk,
            (int)s.Features, (int)s.FeaturesOk);
        mod.Log.Info($"hooks: loaded shipped symbols ({r.SymbolsOk}/{r.Symbols} symbols, {r.ConstantsOk}/{r.Constants} constants)");
        return r;
    }

    internal static void UnloadShipped(ModContext mod)
    {
        FragmentSigs.TryRemove(mod.Info.Id, out _);
        var t = HooksNative.T;
        if (t == null) return;
        HooksNative.WithUtf8(mod.Info.Id, s => t->UnloadSymbols(s));
    }

    // ---- typed descriptors of a mod's fragment (dev.29) ----
    // A core descriptor's status is a kiln.caps row ("sig/<symbol>"); a fragment's descriptors have none. kiln.hooks
    // writes the fragment's resolution, descriptors included ("sigs": {"<modid>/name": {"status": "match", ...}}), to
    // kiln/logs/sigdb-<modid>.json while load_symbols runs; it is read once right after, and dropped with the fragment.
    private static readonly System.Collections.Concurrent.ConcurrentDictionary<string, IReadOnlyDictionary<string, bool>> FragmentSigs =
        new(StringComparer.Ordinal);

    /// <summary>The report kiln.hooks writes for a fragment (seam: unit tests point it elsewhere).</summary>
    internal static Func<string, string> FragmentReport = id => Path.Combine(KilnPaths.Root, "logs", $"sigdb-{id}.json");

    private static void ReadFragmentSigs(ModContext mod)
    {
        IReadOnlyDictionary<string, bool> sigs;
        try
        {
            sigs = ParseFragmentSigs(File.ReadAllText(FragmentReport(mod.Info.Id)));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            // no report: every descriptor of the fragment stays off (Invoke throws naming its row)
            mod.Log.Warn($"hooks: the report of the shipped symbols was not readable ({e.Message}); typed calls of the fragment's descriptors are off");
            sigs = new Dictionary<string, bool>();
        }
        FragmentSigs[mod.Info.Id] = sigs;
        foreach (var (name, ok) in sigs)
            if (!ok) mod.Log.Warn($"hooks: {name}: the descriptor does not fit the function's code on this game build (kiln.log says why); typed hooks and calls of it are off");
    }

    /// <summary>The descriptors of a fragment's report: name to "its status is match".</summary>
    internal static IReadOnlyDictionary<string, bool> ParseFragmentSigs(string json)
    {
        var result = new Dictionary<string, bool>(StringComparer.Ordinal);
        using var doc = JsonDocument.Parse(json);
        if (doc.RootElement.ValueKind != JsonValueKind.Object ||
            !doc.RootElement.TryGetProperty("sigs", out var sigs) || sigs.ValueKind != JsonValueKind.Object)
            return result;
        foreach (var p in sigs.EnumerateObject())
            result[p.Name] = p.Value.ValueKind == JsonValueKind.Object && p.Value.TryGetProperty("status", out var st) &&
                             st.ValueKind == JsonValueKind.String && st.GetString() == "match";
        return result;
    }

    /// <summary>True when <paramref name="symbol"/> is a descriptor of a loaded mod fragment whose code fit it at load
    /// (its "&lt;modid&gt;/" prefix names the fragment).</summary>
    internal static bool FragmentSigMatches(string symbol)
    {
        var slash = symbol.IndexOf('/');
        if (slash <= 0) return false;
        return FragmentSigs.TryGetValue(symbol[..slash], out var sigs) && sigs.TryGetValue(symbol, out var ok) && ok;
    }
}

/// <summary>sigdb layout constants for advanced mods.</summary>
[RequiresCapability(HooksCaps.Service)]
public static unsafe class Constants
{
    /// <summary>The sigdb constant <paramref name="name"/> (core, or "&lt;modid&gt;/name").</summary>
    public static long Get(string name)
        => TryGet(name, out var v) ? v
            : throw new KeyNotFoundException($"sigdb constant '{name}' is unresolved on this build");

    public static bool TryGet(string name, out long value)
    {
        HooksGate.RequireAdvanced("Constants.Get");
        var t = HooksNative.Require("Constants.Get");
        long v = 0;
        var rc = HooksNative.WithUtf8(name, s => { long o; int r = t->Constant(s, &o); v = o; return r; });
        value = v;
        return rc == HooksNative.Ok;
    }
}

/// <summary>Result of resolving a mod's shipped symbol fragment.</summary>
public readonly record struct SigSummary(int Symbols, int SymbolsOk, int Constants, int ConstantsOk, int Features,
    int FeaturesOk);
