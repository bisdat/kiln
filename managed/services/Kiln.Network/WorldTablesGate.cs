// SPDX-License-Identifier: MIT
// Client and server have to agree on building-material ids: cells replicate as raw bytes with no table, so a different list would show the wrong walls. Each side's kiln.worldtables
// has a hash over its live materials' ids and GUIDs; HELLO carries the client's, and the server refuses a difference by name (docs/kiln/services/worldtables.md). Kept materials of absent
// mods aren't in it: they come after every live id, and no client draws them.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln;

internal static unsafe class WorldTablesGate
{
    public const string KeyHash = "kiln.worldtables", KeyIds = "kiln.worldtables.ids";
    /// <summary>Since 0.2.0-dev.29: each custom material pool's digest, "mod:key=digest" joined by commas. An older peer sends none
    /// and reads today's text.</summary>
    public const string KeyPools = "kiln.worldtables.pools";
    /// <summary>A side with no live material, or with no service at all (an older Kiln).</summary>
    public const string EmptyHash = "0000000000000000";

    [StructLayout(LayoutKind.Sequential)]
    private struct Str
    {
        public byte* Ptr;
        public nuint Len;
        public readonly string Text() => Ptr == null || Len == 0 ? "" : Encoding.UTF8.GetString(Ptr, (int)Len);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Material
    {
        public uint Size, State;
        public fixed byte Guid[16];
        public uint CellId, Reused, ItemId, Reserved;
        public Str Mod, Block, ModDir, Definition;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Table
    {
        public uint Size, Version;
        public delegate* unmanaged[Cdecl]<int> State;
        public delegate* unmanaged[Cdecl]<Str> TableHash;
        public delegate* unmanaged[Cdecl]<uint> MaterialCount;
        public delegate* unmanaged[Cdecl]<uint, Material*, int> MaterialAt;
        public void* MaterialId, FirstNewId, AddWriter, DefineJson, PrunePending;
        public delegate* unmanaged[Cdecl]<byte*, nuint, nuint*, int> Status;
    }

    private static Table* Native => Services.Get<Table>("kiln.worldtables", 1);

    /// <summary>This side's live materials, by id: (id, GUID without dashes, "block (mod x)").</summary>
    private static List<(int Id, string Guid, string Label)> Live()
    {
        var list = new List<(int, string, string)>();
        var t = Native;
        if (t == null) return list;
        var n = t->MaterialCount();
        for (uint i = 0; i < n; i++)
        {
            Material m = default;
            m.Size = (uint)sizeof(Material);
            if (t->MaterialAt(i, &m) != 0 || m.State != 0) continue;
            list.Add(((int)m.CellId, new Guid(new ReadOnlySpan<byte>(m.Guid, 16)).ToString("N"), $"{m.Block.Text()} (mod {m.Mod.Text()})"));
        }
        return list;
    }

    private static string OwnHash()
    {
        var t = Native;
        return t == null ? EmptyHash : t->TableHash().Text();
    }

    /// <summary>What goes in HELLO and WELCOME.</summary>
    public static Dictionary<string, string> Properties()
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal) { [KeyHash] = OwnHash() };
        var live = Live();
        if (live.Count > 0) d[KeyIds] = string.Join(',', live.Select(m => $"{m.Id}={m.Guid}"));
        // Empty is a current peer with no pools. Absent is an older peer that cannot describe them.
        d[KeyPools] = string.Join(',', Pools().OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => $"{p.Key}={p.Value}"));
        return d;
    }

    /// <summary>This side's custom pools: "mod:key" to a digest of what a player's menu gets from it (its GUID, roof, members in
    /// order, and the shapes bound to it). Read from kiln.worldtables' status once a handshake asks; no pool, no entry.</summary>
    private static Dictionary<string, string> Pools()
    {
        var t = Native;
        var pools = new Dictionary<string, string>(StringComparer.Ordinal);
        if (t == null || t->Size < 88 || t->Status == null) return pools;  // status_json is at +80
        try
        {
            var status = System.Text.Json.Nodes.JsonNode.Parse(Status(t)) as System.Text.Json.Nodes.JsonObject;
            var bindings = (status?["shapeBindings"] as System.Text.Json.Nodes.JsonArray ?? [])
                .Select(b => ((string?)b?["pool"] ?? "", (string?)b?["item"] ?? "")).ToList();
            foreach (var row in status?["customPools"] as System.Text.Json.Nodes.JsonArray ?? [])
            {
                var guid = (string?)row?["guid"] ?? "";
                var items = (row?["items"] as System.Text.Json.Nodes.JsonArray ?? []).Select(i => (uint?)i ?? 0);
                var bound = bindings.Where(b => b.Item1 == guid).Select(b => b.Item2).Order(StringComparer.Ordinal);
                pools[(string?)row?["name"] ?? guid] = PoolDigest(guid, (bool?)row?["roof"] ?? false, items, bound);
            }
        }
        catch (System.Text.Json.JsonException) { }
        return pools;
    }

    private static string Status(Table* t)
    {
        nuint n = 0;
        for (var attempt = 0; attempt < 8; attempt++)
        {
            var buffer = new byte[(int)Math.Min(n + 1, 64u << 20)];
            nuint got = 0;
            int rc;
            fixed (byte* b = buffer) rc = t->Status(b, (nuint)buffer.Length, &got);
            if (rc == 0 && got < (nuint)buffer.Length) return Encoding.UTF8.GetString(buffer, 0, (int)got);
            if (got <= n) break;
            n = got;
        }
        return "{}";
    }

    /// <summary>FNV-1a 64 over what a pool gives a player's menu, as hex: the same on both sides for the same pool.</summary>
    internal static string PoolDigest(string guid, bool roof, IEnumerable<uint> items, IEnumerable<string> boundShapes)
    {
        var h = 0xcbf29ce484222325ul;
        void Add(string text) { foreach (var b in Encoding.UTF8.GetBytes(text + "\n")) h = (h ^ b) * 0x100000001b3ul; }
        Add(guid);
        Add(roof ? "roof" : "wall");
        foreach (var i in items) Add(i.ToString(System.Globalization.CultureInfo.InvariantCulture));
        Add("shapes");
        foreach (var s in boundShapes) Add(s);
        return h.ToString("x16");
    }

    /// <summary>Null when a client with these handshake properties has this server's ids; else what a player reads.</summary>
    public static string? Difference(IReadOnlyDictionary<string, string> client)
    {
        var mine = OwnHash();
        var theirs = client.GetValueOrDefault(KeyHash, EmptyHash);
        if (mine == theirs) return null;
        return Describe(Live(), client.GetValueOrDefault(KeyIds, ""), Pools(), client.GetValueOrDefault(KeyPools));
    }

    /// <summary>The difference in words: which materials, by name where this side knows it. Plain text, no game state: the handshake tests read it.</summary>
    internal static string Describe(IReadOnlyList<(int Id, string Guid, string Label)> server, string clientIds) => Describe(server, clientIds, null, null);

    /// <summary>The same, and the custom pools by name when the client sent its pools (a peer older than dev.29 sends none: then
    /// the pools say nothing of their own and the text is as before). <paramref name="clientPools"/>: "mod:key=digest,...", "" for a
    /// client with no pool.</summary>
    internal static string Describe(IReadOnlyList<(int Id, string Guid, string Label)> server, string clientIds,
        IReadOnlyDictionary<string, string>? serverPools, string? clientPools)
    {
        var theirs = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var part in clientIds.Split(',', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = part.IndexOf('=');
            if (eq > 0 && int.TryParse(part[..eq], out var id)) theirs[part[(eq + 1)..]] = id;
        }
        var notes = new List<string>();
        foreach (var m in server)
        {
            if (!theirs.TryGetValue(m.Guid, out var id)) notes.Add($"you don't have {m.Label}");
            else if (id != m.Id) notes.Add($"{m.Label} is id {m.Id} here and {id} on your game");
            theirs.Remove(m.Guid);
        }
        foreach (var (guid, id) in theirs.OrderBy(x => x.Value)) notes.Add($"your game has material {guid} (id {id}), and this server doesn't");
        if (serverPools != null && clientPools != null)
        {
            var yours = new Dictionary<string, string>(StringComparer.Ordinal);
            foreach (var part in clientPools.Split(',', StringSplitOptions.RemoveEmptyEntries))
                if (part.LastIndexOf('=') is var eq and > 0) yours[part[..eq]] = part[(eq + 1)..];
            string ModOf(string name) => name.Contains(':') ? name[..name.IndexOf(':')] : name;
            foreach (var (name, digest) in serverPools.OrderBy(p => p.Key, StringComparer.Ordinal))
            {
                if (!yours.TryGetValue(name, out var other))
                    notes.Add($"you don't have the material pool {name} (mod {ModOf(name)} is missing, another version, or your Kiln refused the pool: your kiln.log says why)");
                else if (other != digest)
                    notes.Add($"the material pool {name} differs between this server and your game (another version of mod {ModOf(name)}, or a game material one side lacks)");
                yours.Remove(name);
            }
            foreach (var name in yours.Keys.Order(StringComparer.Ordinal))
                notes.Add($"your game has the material pool {name}, and this server doesn't (another version of mod {ModOf(name)}, or a pool your game kept from an earlier version)");
        }
        var what = notes.Count == 0 ? "the lists differ" : string.Join("; ", notes.Take(6)) + (notes.Count > 6 ? $"; and {notes.Count - 6} more" : "");
        return $"This server's building and terrain materials don't match your game's: {what}. Join with the same mods, and restart if you just changed them.";
    }
}
