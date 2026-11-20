// SPDX-License-Identifier: MIT
// Kiln.Replication's item remap (the dossier's chapter 15 §4 M3 "Item-id remap", "What a vanilla peer can safely be
// sent"; DESIGN "Vanilla safety"; owner decision 2026-11-20: an item without a declared look-alike shows as an empty
// slot). Kiln runs it by itself on every server and hosted world: while Kiln-only items exist, every peer that lacks them
// (Peers.LackingContent: vanilla clients, Kiln clients without the server's content) is sent each Kiln-only item id as its
// look-alike, or 0. The native step (remap.cpp) fails closed; this side builds its table: the item-id sites of every
// component that can travel (from the live registry and the game's reflected types), the game's own item ids, and each
// Kiln-only item's look-alike (ItemDefinition.VanillaLook, else the game item it is based on when that one is a game
// item of the same kind, else none). While Kiln-only items exist and the remap is not working, clients without Kiln are
// refused at the door (NetAdmission), as letting them in could crash them.
namespace Kiln;

internal static unsafe class ItemRemap
{
    private static readonly object Gate = new();
    private static readonly List<PeerSelector> Selectors = [];
    private static bool _started, _required;
    private static IDisposable? _admission;
    private static int _tick;
    private static ulong _tableKey;          // what the installed table was built from
    private static bool _active;
    private static int _sites, _kilnOnly, _withoutLook;
    private static uint[] _lastPeers = [uint.MaxValue];
    private static ReplNetNative.ItemSite[]? _siteCache;

    /// <summary>Kiln's own rule plus the mods' (RemapItems).</summary>
    internal static IDisposable AddSelector(PeerSelector peers)
    {
        lock (Gate) Selectors.Add(peers);
        return new Unregister(() => { lock (Gate) Selectors.Remove(peers); });
    }

    private sealed class Unregister(Action a) : IDisposable
    {
        private Action? _a = a;
        public void Dispose() => Interlocked.Exchange(ref _a, null)?.Invoke();
    }

    internal static ReplRemapStats Stats()
    {
        var s = new ReplNetNative.RemapStats { Size = (uint)sizeof(ReplNetNative.RemapStats) };
        var t = ReplNetNative.T;
        if (t != null) t->RemapStatsFn(&s);
        return new ReplRemapStats(_active, _sites, _kilnOnly, _withoutLook, (long)s.Snapshots, (long)s.Remapped, (long)s.Zeroed, (long)s.Faults);
    }

    /// <summary>Started with the filters' service tick (ReplicationStartup starts it on servers and hosts).</summary>
    internal static void Start()
    {
        if (_started) return;
        _started = true;
        FilterHost.EnsureTick();
    }

    /// <summary>Every step (tick thread): the table every 2 s (content can grow at any time), the peers every step.</summary>
    internal static void Tick()
    {
        if (!_required) Require();  // also when the remap cannot run: then it refuses (a no-op where nobody joins)
        var t = ReplNetNative.T;
        if (_tick++ % 120 == 0) RefreshTable(t);
        if (!_active || t == null) return;
        // peers: Kiln's rule and the mods'
        List<PeerSelector> selectors;
        lock (Gate) selectors = [Peers.LackingContent, .. Selectors];
        var set = new SortedSet<uint>();
        foreach (var sel in selectors)
            foreach (var id in FilterHost.PeersOf(sel))
                if (id != 0) set.Add(id);
        var now = set.Count == 0 ? new uint[] { 0 } : set.ToArray();
        if (now.AsSpan().SequenceEqual(_lastPeers)) return;
        _lastPeers = now;
        fixed (uint* p = now) t->RemapPeers(p, (uint)now.Length);
    }

    /// <summary>A client without Kiln is let in only while no Kiln-only item exists or the remap works.</summary>
    private static void Require()
    {
        _required = true;
        try
        {
            _admission = NetAdmission.Require("repl.itemremap", static () => _kilnOnly == 0 || _active,
                "this server has items only Kiln has and cannot hide them from clients without Kiln");
        }
        catch (Exception e)
        {
            ReplNetLog.Warn($"repl: the item remap could not register its admission rule: {e.Message}");
        }
    }

    private static void RefreshTable(ReplNetNative.Table* t)
    {
        IReadOnlyList<ContentDefinition> defs;
        try
        {
            defs = NewContent.Definitions();
        }
        catch (Exception)
        {
            defs = [];
        }
        var kilnOnly = defs.Where(d => d.Kind == ContentKind.Items && d.State is "registered" or "orphaned").ToList();
        _kilnOnly = kilnOnly.Count;
        if (t == null || (t->Capabilities() & ReplNetNative.CapRemap) == 0)
        {
            if (kilnOnly.Count > 0) ReplNetLog.WarnOnce("repl: Kiln-only items exist and the item remap is not available on this build: clients without Kiln are refused");
            _active = false;
            return;
        }
        if (kilnOnly.Count == 0)
        {
            if (_active)
            {
                ReplNetNative.ItemSite none = default;
                t->RemapSet(&none, 0, null, 0, null, 0);
                _active = false;
                _tableKey = 0;
                ReplNetLog.Info("repl: item remap off (no Kiln-only items)");
            }
            return;
        }
        IReadOnlyList<ItemInfo> all;
        try
        {
            all = Items.All;
        }
        catch (Exception)
        {
            return;  // the item registry is not read yet
        }
        if (all.Count == 0) return;
        var kilnIds = new HashSet<uint>(kilnOnly.Select(d => d.Id));
        var key = (ulong)kilnIds.Count * 1000003UL ^ (ulong)all.Count;
        foreach (var id in kilnIds) key = key * 31 + id;
        foreach (var (g, look) in NewContent.VanillaLooks) key = key * 31 + (ulong)g.GetHashCode() + (ulong)look.Length;
        if (_active && key == _tableKey) return;
        var sites = _siteCache ?? BuildSites();
        if (sites.Length == 0)
        {
            ReplNetLog.WarnOnce("repl: item remap waits: no item-id site found yet (no world, or the game's reflected types are not here)");
            return;
        }
        _siteCache = sites;
        var gameIds = all.Select(i => i.Id.Value).Where(id => id != 0 && !kilnIds.Contains(id)).Distinct().OrderBy(x => x).ToArray();
        var byId = all.ToDictionary(i => i.Id.Value, i => i);
        var gameSet = new HashSet<uint>(gameIds);
        var map = new List<ReplNetNative.ItemMap>();
        var without = 0;
        foreach (var d in kilnOnly)
        {
            var look = LookAlike(d, byId, gameSet, out var why);
            if (look == 0)
            {
                without++;
                ReplNetLog.InfoOnce($"repl: {d.Name} has no vanilla look-alike ({why}): players without it see an empty slot");
            }
            map.Add(new ReplNetNative.ItemMap { From = d.Id, To = look });
        }
        var mapArr = map.OrderBy(m => m.From).ToArray();
        int rc;
        fixed (ReplNetNative.ItemSite* s = sites)
        fixed (uint* g = gameIds)
        fixed (ReplNetNative.ItemMap* m = mapArr)
            rc = t->RemapSet(s, (uint)sites.Length, g, (uint)gameIds.Length, m, (uint)mapArr.Length);
        _active = rc == ReplNative.Ok;
        _tableKey = _active ? key : 0;
        if (rc == ReplNative.Unavailable) return;  // no server world here (a game client that does not host)
        _sites = sites.Length;
        _withoutLook = without;
        _lastPeers = [uint.MaxValue];  // the peers go again with the new table
        if (_active)
            ReplNetLog.Info($"repl: item remap on: {kilnOnly.Count} Kiln-only item(s), {without} without a look-alike, {sites.Length} sites, {gameIds.Length} game items");
        else
            ReplNetLog.Warn($"repl: the item remap table was refused ({rc}): clients without Kiln are refused while Kiln-only items exist");
    }

    /// <summary>A Kiln-only item's look-alike: its declared VanillaLook, a game item of the same kind (category and
    /// equipment slot). 0 when there is none (owner decision 2026-11-20: no declared look-alike = an empty slot; the item
    /// it is based on is not used, so a mod decides what players without it see).</summary>
    internal static uint LookAlike(ContentDefinition d, IReadOnlyDictionary<uint, ItemInfo> byId, IReadOnlySet<uint> game, out string why)
    {
        why = "";
        byId.TryGetValue(d.Id, out var self);
        uint Pick(string? text, string what, ref string reason)
        {
            if (string.IsNullOrWhiteSpace(text)) return 0;
            ItemInfo? info;
            try
            {
                info = Items.Lookup(text);
            }
            catch (Exception)
            {
                info = null;
            }
            if (info is null || !game.Contains(info.Id.Value))
            {
                reason = $"{what} {text} is not a game item";
                return 0;
            }
            if (self is not null && (info.Category != self.Category || info.EquipSlot != self.EquipSlot))
            {
                reason = $"{what} {text} is another kind of item";
                return 0;
            }
            return info.Id.Value;
        }
        var reason = "it declares no VanillaLook";
        NewContent.VanillaLooks.TryGetValue(d.Guid, out var declared);
        var look = Pick(declared, "its VanillaLook", ref reason);
        if (look == 0 && declared is not null) ReplNetLog.WarnOnce($"repl: {d.Name}: {reason}");
        why = reason;
        return look;
    }

    /// <summary>Every keen::ItemId inside a component that may travel (its registry usage has a client or prediction bit),
    /// through structs and fixed arrays (tools/replinspect sites follows the same rule).</summary>
    internal static ReplNetNative.ItemSite[] BuildSites()
    {
        var list = new List<ReplNetNative.ItemSite>();
        IReadOnlyList<GameType> comps;
        try
        {
            comps = GameTypes.Components;
        }
        catch (Exception)
        {
            return [];
        }
        foreach (var c in comps)
        {
            var info = Replication.Component(c.QualifiedName);
            if (info is null || !(info.MayBeOnClientChannel || info.MayBeOnPredictionChannel)) continue;
            var found = new List<(int Offset, int Count, int Stride)>();
            Walk(c, 0, found, 0);
            foreach (var (off, count, stride) in found)
            {
                if (off < 0 || off > ushort.MaxValue || count < 1 || count > ushort.MaxValue || stride > ushort.MaxValue) continue;
                if (off + (count - 1) * stride + 4 > info.Size) continue;
                list.Add(new ReplNetNative.ItemSite
                {
                    ComponentHash = info.Hash, Offset = (ushort)off, Count = (ushort)count, Stride = (ushort)(count > 1 ? stride : 0),
                });
            }
        }
        return [.. list];
    }

    internal static bool IsItemId(GameType t) =>
        t.QualifiedName == "keen::ItemId" || (t.Kind != GameTypeKind.Struct && t.Inner is { QualifiedName: "keen::ItemId" });

    private static void Walk(GameType t, int baseOffset, List<(int, int, int)> found, int depth)
    {
        if (depth > 12) return;
        if (IsItemId(t))
        {
            found.Add((baseOffset, 1, 0));
            return;
        }
        if (t.Kind == GameTypeKind.StaticArray && t.Inner is { Size: > 0 } inner)
        {
            var n = t.ArrayLength;
            var sub = new List<(int Offset, int Count, int Stride)>();
            Walk(inner, 0, sub, depth + 1);
            foreach (var (o, c, s) in sub)
                if (c == 1) found.Add((baseOffset + o, n, inner.Size));
                else for (var i = 0; i < n; i++) found.Add((baseOffset + i * inner.Size + o, c, s));
            return;
        }
        if (t.Kind != GameTypeKind.Struct) return;
        foreach (var f in t.AllFields) Walk(f.Type, baseOffset + f.Offset, found, depth + 1);
    }
}

/// <summary>kiln.log lines of Kiln.Replication's network parts.</summary>
internal static class ReplNetLog
{
    private static readonly HashSet<string> Once = [];

    internal static void Info(string line) => Write(LogLevel.Info, line);
    internal static void Warn(string line) => Write(LogLevel.Warn, line);

    internal static void InfoOnce(string line)
    {
        lock (Once)
            if (!Once.Add(line)) return;
        Info(line);
    }

    internal static void WarnOnce(string line)
    {
        lock (Once)
            if (!Once.Add(line)) return;
        Warn(line);
    }

    private static void Write(LogLevel level, string line)
    {
        try
        {
            Backend.Current.Log(0, level, line);
        }
        catch
        {
            // a log that cannot be written changes nothing
        }
    }
}
