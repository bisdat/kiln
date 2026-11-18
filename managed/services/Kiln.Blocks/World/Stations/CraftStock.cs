// SPDX-License-Identifier: MIT
// GitHub #68 step 2, the crafting menu. While a Kiln client's crafting menu is open, the server tells it what the opted-in
// stations of the base it stands in add to its crafting stock, so the menu's counts and its craft button allow a craft
// that only those outputs can complete. Channel "kiln:craftstock", required: false: a vanilla client never waits on it,
// never says its menu opened and is never sent anything.
//  - Small: only the items of the recipes that take a lent item (native craft_items), only while the menu is open, and
//    again only when a count changed.
//  - Bound to the session: a player who leaves, a player who joins into the slot, a menu that closes or stops saying it
//    is open (5 s keep-alive, 15 s at most), and a new world end it. A send that did not go out is tried again.
//  - Only the base the player may craft from by the game's own right (CanAccessInventories, the payment's own test): a
//    visitor without it is sent nothing.
//  - Not authority: the craft's payment recounts everything (native kiln.stations, docs/re/stations.md §10).
// docs/kiln/services/stations.md, "What each client shows".
using System.Collections.Concurrent;
using System.Text;

namespace Kiln;

/// <summary>Server to client: one snapshot, per item the bag, the base's Magic chests and the opted-in outputs together.
/// Empty: the game's own counts again.</summary>
internal sealed record CraftStockRows(CraftStockRow[] Rows)
{
    public const ushort Id = 1;

    public static void Encode(CraftStockRows m, NetWriter w)
    {
        var n = Math.Min(m.Rows.Length, CraftStock.MaxRows);
        w.UInt16((ushort)n);
        for (var i = 0; i < n; i++) w.UInt32(m.Rows[i].Item).UInt32(m.Rows[i].Count);
    }

    public static CraftStockRows Decode(ref NetReader r)
    {
        var n = Math.Min((int)r.UInt16(), CraftStock.MaxRows);
        var rows = new CraftStockRow[n];
        for (var i = 0; i < n; i++) rows[i] = new CraftStockRow(r.UInt32(), r.UInt32());
        return new CraftStockRows(rows);
    }
}

internal readonly record struct CraftStockRow(uint Item, uint Count);

/// <summary>Client to server: a crafting menu opened (and, every 5 s, is still open) or closed.</summary>
internal sealed record CraftStockHello(bool Open)
{
    public const ushort Id = 2;
    public static void Encode(CraftStockHello m, NetWriter w) => w.Bool(m.Open);
    public static CraftStockHello Decode(ref NetReader r) => new(r.Bool());
}

/// <summary>Started at the first service tick (ServiceTick.AutoStart), so the channel is there before anyone joins. It
/// registers the channel and, on a client, watches the screen; nothing else runs until a Kiln client opens a crafting
/// menu (Kiln.Blocks' data tick does not start here).</summary>
internal static class CraftStockStartup
{
    static CraftStockStartup() => CraftStock.Start();
}

internal static class CraftStock
{
    public const string ChannelName = "kiln:craftstock";
    public const string ChannelVersion = "1";
    internal const int MaxRows = StationsNative.MaxCraftStock;
    internal const ulong RefreshSteps = 30;  // ~1.2 s: an open menu follows the stations' outputs
    internal static readonly TimeSpan KeepAlive = TimeSpan.FromSeconds(5);
    internal static readonly TimeSpan SessionLife = TimeSpan.FromSeconds(15);
    internal static readonly CraftStockRows Off = new([]);

    private sealed class Session
    {
        public string? Sent;  // the key of the last snapshot that went out; null: none yet, or the last send failed
        public long Seen;     // ms of the last hello
        public bool Due = true;
    }

    private static readonly object Gate = new();
    private static NetChannel? _channel;
    private static bool _started, _playersWatched;
    private static IDisposable? _tick;
    private static ulong _lastStep;
    private static int _warned;
    private static readonly Dictionary<uint, Session> Sessions = new();
    private static readonly ConcurrentQueue<(EntityId Player, bool Open)> Inbox = new();
    private static readonly Dictionary<BaseId, IReadOnlyDictionary<ItemId, long>> StockCache = new();

    /// <summary>Test hooks: the clock (ms), what a player is sent (null: off), the delivery (true: it went out).</summary>
    internal static Func<long> Clock = static () => Environment.TickCount64;
    internal static Func<EntityId, CraftStockRows?>? BuildHook;
    internal static Func<EntityId, CraftStockRows, bool>? SendHook;

    /// <summary>Registers the channel (and on a client the screen watch). Idempotent; a refused channel is not retried.</summary>
    internal static void Start()
    {
        lock (Gate)
        {
            if (_started) return;
            _started = true;
        }
        if (!Network.Available) return;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own channel, not a mod's
        try
        {
            _channel = Network.Channel(ChannelName, ChannelVersion, required: false)
                .Register<CraftStockRows>(CraftStockRows.Id, static (m, w) => CraftStockRows.Encode(m, w),
                    static (ref NetReader r) => CraftStockRows.Decode(ref r), static (m, _) => { if (!Client.IsHosting) ClientRows(m); },
                    NetDirection.ToClient)
                .Register<CraftStockHello>(CraftStockHello.Id, static (m, w) => CraftStockHello.Encode(m, w),
                    static (ref NetReader r) => CraftStockHello.Decode(ref r), static (m, ctx) => Enqueue(ctx.Player, m.Open),
                    NetDirection.ToServer);
        }
        catch (Exception e)
        {
            ModLog.Warn($"kiln.stations: the channel {ChannelName} is unavailable: {e.Message}");
            return;
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
        try
        {
            if (Client.IsClient) StartClient();
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or TypeInitializationException)
        {
            ModLog.Warn($"kiln.stations: the crafting menu does not count station outputs on this client: {e.Message}");
        }
    }

    // ================================================================ server ====

    /// <summary>A hello from a client's menu (any thread): the server's tick takes it.</summary>
    internal static void Enqueue(EntityId player, bool open)
    {
        if (player.IsNone || Inbox.Count > 1024) return;
        Inbox.Enqueue((player, open));
        EnsureTick();
    }

    private static void EnsureTick()
    {
        lock (Gate) _tick ??= ServiceTick.Add("kiln.stations craft stock", static (in TickInfo t) => Pump(t.Step));
    }

    private static void WatchPlayers()
    {
        lock (Gate)
        {
            if (_playersWatched) return;
            _playersWatched = true;
        }
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own subscription
        try
        {
            Events.On<PlayerLeftEvent>(static e => Gone(e.Player.Entity));
            Events.On<PlayerJoinedEvent>(static e => Gone(e.Player.Entity));
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or TypeInitializationException or FileNotFoundException)
        {
            // kiln.players is not here: a session still ends 15 s after its last hello
            ModLog.Warn($"kiln.stations: crafting stock does not see players leave ({e.Message}); a session ends 15 s after its last hello");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>A player left, or someone joined into the slot (tick thread): that entity's session goes.</summary>
    internal static void Gone(EntityId player)
    {
        lock (Gate) Sessions.Remove(player.Value);
    }

    /// <summary>A hello (tick thread). Open: a new session is sent a snapshot at once; a keep-alive only extends it.
    /// Closed: the session goes (the client clears its own counts).</summary>
    internal static void Hello(EntityId player, bool open)
    {
        if (player.IsNone) return;
        lock (Gate)
        {
            if (!open)
            {
                Sessions.Remove(player.Value);
                return;
            }
            if (!Sessions.TryGetValue(player.Value, out var s)) Sessions[player.Value] = s = new Session();
            s.Seen = Clock();
        }
        WatchPlayers();
    }

    /// <summary>The server's tick (every step, once a client said hello): the hellos, then the sessions due.</summary>
    internal static void Pump(ulong step)
    {
        var n = 0;
        while (n++ < 256 && Inbox.TryDequeue(out var h)) Hello(h.Player, h.Open);
        List<(EntityId Player, Session S)>? due = null;
        var now = Clock();
        lock (Gate)
        {
            if (step < _lastStep) Sessions.Clear();  // a new world: its entity ids name other things
            _lastStep = step;
            if (Sessions.Count == 0) return;
            foreach (var p in Sessions.Where(kv => now - kv.Value.Seen > (long)SessionLife.TotalMilliseconds).Select(kv => kv.Key).ToList())
                Sessions.Remove(p);
            foreach (var (p, s) in Sessions)
                if (s.Due || step % RefreshSteps == 0) (due ??= []).Add((new EntityId(p), s));
        }
        if (due is null) return;
        StockCache.Clear();  // each base's chests read once per pass
        foreach (var (p, s) in due) Refresh(p, s);
    }

    private static void Refresh(EntityId player, Session s)
    {
        CraftStockRows rows;
        try
        {
            rows = Build(player) ?? Off;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or KeyNotFoundException or ArgumentException)
        {
            rows = Off;
        }
        var key = Key(rows);
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var live) || !ReferenceEquals(live, s)) return;  // it ended meanwhile
            if (!s.Due && s.Sent == key) return;
        }
        var ok = Deliver(player, rows);
        lock (Gate)
        {
            s.Due = false;
            s.Sent = ok ? key : null;  // not sent: the next pass tries again
        }
    }

    /// <summary>What a player is sent now: null when nothing of theirs can be raised (no right to the base's
    /// inventories, no base, nothing opted in that holds outputs, or more items than one snapshot carries).</summary>
    internal static CraftStockRows? Build(EntityId player)
    {
        if (BuildHook is { } hook) return hook(player);
        if (!Capabilities.Is(Stations.CraftSourcesCapability) || !Capabilities.Is(Stations.OutputSourcesCapability)) return null;
        if (!Stations.Available || !Players.Available || !StationsCore.Backend.HasCraftStock) return null;
        if (Players.Find(player) is not { InGame: true } who) return null;
        var mayAccess = StationsCore.CanAccessInventories(who);
        var home = mayAccess is true ? Bases.Of(player) : null;  // a visitor's base is not even read
        var lent = new Dictionary<uint, long>();
        if (home is not null)
            foreach (var station in Stations.OutputSources(home.Id))
                if (Stations.Read(station) is { } info)
                    foreach (var slot in info.Outputs)
                        if (slot.Item.Value != 0 && slot.Count > 0) lent[slot.Item.Value] = lent.GetValueOrDefault(slot.Item.Value) + slot.Count;
        var rows = From(mayAccess, home is not null, lent, StationsCore.Backend.CraftItems, () =>
        {
            var bag = new Dictionary<uint, long>();
            if (Items.Read(player) is { } inv)
                foreach (var slot in inv.Slots)
                    if (slot.Item.Value != 0 && slot.Count > 0) bag[slot.Item.Value] = bag.GetValueOrDefault(slot.Item.Value) + slot.Count;
            return bag;
        }, () =>
        {
            if (!StockCache.TryGetValue(home!.Id, out var stock)) StockCache[home.Id] = stock = Bases.Stock(home.Id);
            var chests = new Dictionary<uint, long>();
            foreach (var (item, count) in stock) chests[item.Value] = count;
            return chests;
        }, out var tooMany);
        if (tooMany && Interlocked.Exchange(ref _warned, 1) == 0)
            ModLog.Warn($"kiln.stations: base {home!.Id}'s crafting stock names more than {MaxRows} items: its crafting menus show the game's counts");
        return rows;
    }

    /// <summary>The decision behind <see cref="Build"/> on plain values: nothing unless the player may access the base's
    /// inventories (the game's own CanAccessInventories, the test the payment relies on), stands in a base and some
    /// opted-in station there holds outputs; then the items craft_items names, counted.</summary>
    internal static CraftStockRows? From(bool? mayAccess, bool inBase, IReadOnlyDictionary<uint, long> lent,
        Func<uint[], (int Rc, uint[] Items)> craftItems, Func<IReadOnlyDictionary<uint, long>> bag,
        Func<IReadOnlyDictionary<uint, long>> chests, out bool tooMany)
    {
        tooMany = false;
        if (mayAccess is not true || !inBase || lent.Count == 0) return null;
        var (rc, items) = craftItems([.. lent.Keys.Order()]);
        if (rc != StationsNative.Ok) return null;
        var rows = Compose(items, lent, bag(), chests());
        tooMany = rows is null;
        return rows;
    }

    /// <summary>One snapshot: per item of <paramref name="items"/> the bag, the chests and the lent outputs together,
    /// sorted, without empty rows. Null when that is more than <see cref="MaxRows"/> rows (never a cut snapshot: a
    /// missing item would count as none).</summary>
    internal static CraftStockRows? Compose(IReadOnlyCollection<uint> items, IReadOnlyDictionary<uint, long> lent,
        IReadOnlyDictionary<uint, long> bag, IReadOnlyDictionary<uint, long> chests)
    {
        var rows = new List<CraftStockRow>();
        foreach (var item in items.Distinct().Order())
        {
            if (item == 0) continue;
            var n = lent.GetValueOrDefault(item) + bag.GetValueOrDefault(item) + chests.GetValueOrDefault(item);
            if (n > 0) rows.Add(new CraftStockRow(item, (uint)Math.Min(n, uint.MaxValue)));
        }
        return rows.Count > MaxRows ? null : new CraftStockRows([.. rows]);
    }

    internal static string Key(CraftStockRows rows)
    {
        var b = new StringBuilder();
        foreach (var r in rows.Rows) b.Append(r.Item).Append('=').Append(r.Count).Append(';');
        return b.ToString();
    }

    // To the player's Kiln client: in this process for the hosting player, else over the channel to a peer that has it.
    private static bool Deliver(EntityId player, CraftStockRows rows)
    {
        if (SendHook is { } hook) return hook(player, rows);
        if (Client.IsHosting && Client.HasLocalPlayer && Client.LocalPlayer == player) return Apply(rows);
        if (_channel is null || Network.PeerOf(player) is not { } peer || !peer.HasChannel(ChannelName)) return false;
        return _channel.SendTo(player, rows);
    }

    /// <summary>Tests: everything back to the start.</summary>
    internal static void ResetForTests()
    {
        lock (Gate)
        {
            Sessions.Clear();
            _tick?.Dispose();
            _tick = null;
            _lastStep = 0;
            _warned = 0;
            _open = _ticking = false;
        }
        while (Inbox.TryDequeue(out _)) { }
        StockCache.Clear();
        Clock = static () => Environment.TickCount64;
        BuildHook = null;
        SendHook = null;
        ServerSideHook = null;
        HelloHook = null;
        ApplyHook = null;
    }

    /// <summary>Tests: whether a session is open for the player, and the key last sent to it.</summary>
    internal static (bool Open, string? Sent) SessionOf(EntityId player)
    {
        lock (Gate) return Sessions.TryGetValue(player.Value, out var s) ? (true, s.Sent) : (false, null);
    }

    // ================================================================ client ====

    private static volatile bool _open, _ticking;
    private static long _lastHello;

    /// <summary>Test hooks: whether the server takes hellos (hosting, or Kiln there); a hello sent; a snapshot applied.</summary>
    internal static Func<bool>? ServerSideHook;
    internal static Action<bool>? HelloHook;
    internal static Func<CraftStockRows, bool>? ApplyHook;

    private static void StartClient()
    {
        if (!StationsCore.Backend.HasCraftStock) return;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try
        {
            Ui.Screens.Changed += ClientChanged;  // screen changes only: no work in other frames
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    private static bool ServerSide() => ServerSideHook?.Invoke() ?? (Client.IsHosting || Network.ServerHasKiln);

    /// <summary>A screen change (the game's UI thread): a crafting menu opened (the server is told, the keep-alive starts)
    /// or closed (the server is told, the counts go back to the game's).</summary>
    internal static void ClientChanged(GameScreen before, GameScreen now)
    {
        var open = now.Kind == ScreenKind.Crafting && ServerSide() && Capabilities.Is(Stations.CraftUiCapability);
        if (open == _open) return;
        _open = open;
        SwitchTick(open);
        _lastHello = Environment.TickCount64;
        SendHello(open);
        if (!open) Apply(Off);
    }

    private static void SwitchTick(bool on)
    {
        if (HelloHook is not null) return;  // tests drive the keep-alive themselves
        if (on == _ticking) return;
        _ticking = on;
        var outer = ModContext.CurrentMod;
        ModContext.CurrentMod = null;
        try
        {
            if (on) Ui.Screens.UiTick += ClientTick;
            else Ui.Screens.UiTick -= ClientTick;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    // Every UI frame while a crafting menu is open: the keep-alive every 5 s.
    internal static void ClientTick(GameScreen now)
    {
        if (!_open || now.Kind != ScreenKind.Crafting) return;  // Changed ends the watch
        var tick = Environment.TickCount64;
        if (tick - _lastHello < (long)KeepAlive.TotalMilliseconds) return;
        _lastHello = tick;
        SendHello(true);
    }

    private static void SendHello(bool open)
    {
        if (HelloHook is { } hook)
        {
            hook(open);
            return;
        }
        if (Client.IsHosting && Client.HasLocalPlayer)
        {
            Enqueue(Client.LocalPlayer, open);  // the server half in this process takes it on its tick
            return;
        }
        _channel?.SendToServer(new CraftStockHello(open));
    }

    // The server's snapshot (the client's main thread). One that arrives after the menu closed is not kept.
    internal static void ClientRows(CraftStockRows m)
    {
        if (!_open && m.Rows.Length > 0) return;
        Apply(m);
    }

    /// <summary>Hands a snapshot to kiln.stations (native logs its arrival); an empty one clears it.</summary>
    internal static bool Apply(CraftStockRows rows)
    {
        if (ApplyHook is { } hook) return hook(rows);
        if (rows.Rows.Length > 0 && !_open) rows = Off;  // the hosting player's menu closed meanwhile
        if (!StationsCore.Backend.HasCraftStock) return false;
        var stacks = new StationsNative.Stack[rows.Rows.Length];
        for (var i = 0; i < stacks.Length; i++)
            stacks[i] = new StationsNative.Stack { Item = rows.Rows[i].Item, Count = rows.Rows[i].Count };
        return StationsCore.Backend.SetCraftStock(stacks) == StationsNative.Ok;
    }
}
