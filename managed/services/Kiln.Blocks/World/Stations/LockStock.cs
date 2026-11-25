// SPDX-License-Identifier: MIT
// GitHub #147, since 0.2.0-dev.31.12: locks (the game's InteractionLock with a required item: a lockpick, a key) count and
// pay that item from the player's links registered with CraftSourceOptions.Locks. The game's client refuses a lock its
// bag cannot open on its own, before the server ever hears of the press, so a Kiln client needs the server's counts:
//  - the server sends a player with a lock-opted link, on the channel kiln:craftstock (required: false), whether it
//    supports locks from links and, for the lock items that player's client asked about, what the links hold (the bag
//    not included: the client counts its own bag), again whenever a count changes;
//  - the client's lock checks (the press and the locked prompt, native kiln-items) count the bag plus that row, and note
//    an item with no row; the client asks the server about those (at most 16, again after 5 s while unanswered);
//  - not authority: the server's own lock check counts the links itself, and its payment rechecks every link, the bag
//    first, inside the game's interaction transaction (native, docs/re/locks.md).
// A vanilla client never asks and is never sent anything; it refuses such a lock as the game does.
// docs/kiln/services/stations.md, "Locks from linked sources".
using System.Collections.Concurrent;
using System.Text;

namespace Kiln;

/// <summary>Server to client: whether the server counts lock items from the player's links, and per asked item what the
/// links hold. Unsupported: the game's own lock checks.</summary>
internal sealed record LockStockRows(bool Supported, CraftStockRow[] Rows)
{
    internal const ushort Id = 7;

    internal static void Encode(LockStockRows m, NetWriter w)
    {
        var n = Math.Min(m.Rows.Length, LockStock.MaxRows);
        w.Bool(m.Supported).UInt16((ushort)n);
        for (var i = 0; i < n; i++) w.UInt32(m.Rows[i].Item).UInt32(m.Rows[i].Count);
    }

    internal static LockStockRows Decode(ref NetReader r)
    {
        var supported = r.Bool();
        var n = r.UInt16();
        if (n > LockStock.MaxRows) throw new InvalidDataException("too many lock rows");
        var rows = new CraftStockRow[n];
        var seen = new HashSet<uint>();
        for (var i = 0; i < n; i++)
        {
            var item = r.UInt32();
            var count = r.UInt32();
            if (item == 0 || !seen.Add(item)) throw new InvalidDataException("invalid lock row");
            rows[i] = new(item, count);
        }
        return new(supported, supported ? rows : []);
    }
}

/// <summary>Client to server: the lock items the client's lock checks met with no row (at most 16).</summary>
internal sealed record LockStockAsk(uint[] Items)
{
    internal const ushort Id = 8;

    internal static void Encode(LockStockAsk m, NetWriter w)
    {
        var n = Math.Min(m.Items.Length, LockStock.MaxAsks);
        w.Byte((byte)n);
        for (var i = 0; i < n; i++) w.UInt32(m.Items[i]);
    }

    internal static LockStockAsk Decode(ref NetReader r)
    {
        var n = Math.Min((int)r.Byte(), LockStock.MaxAsks);
        var items = new uint[n];
        for (var i = 0; i < n; i++) items[i] = r.UInt32();
        return new(items);
    }
}

internal static class LockStock
{
    internal const int MaxRows = StationsNative.MaxLockStock, MaxAsks = StationsNative.MaxLockAsks;
    internal const ulong RefreshSteps = 30;  // ~1.2 s, as the crafting menu's snapshot
    internal static readonly TimeSpan AskPoll = TimeSpan.FromMilliseconds(250);
    internal static readonly TimeSpan AskAgain = TimeSpan.FromSeconds(5);
    internal static readonly LockStockRows Off = new(false, []);

    private sealed class Session
    {
        public int Links;                         // live lock-opted handles of this player
        public readonly List<uint> Asked = [];    // lock items the client asked about, oldest first (at most MaxRows)
        public string? Sent;                      // the key of the last message that went out; null: none or failed
        public bool Due = true;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<uint, Session> Sessions = [];
    private static readonly ConcurrentQueue<(EntityId Player, uint[] Items)> Inbox = new();
    private static NetChannel? _channel;
    private static IDisposable? _tick;
    private static bool _attached, _playersWatched;
    private static ulong _lastStep;

    /// <summary>Test hooks: the links' counts (instead of native source_stock), a delivery, the client's native calls.</summary>
    internal static Func<EntityId, (int Rc, StationsNative.Stack[] Rows)>? RowsHook;
    internal static Func<EntityId, LockStockRows, bool>? SendHook;
    internal static Func<bool>? SupportedHook;
    internal static Func<bool, StationsNative.Stack[], int>? ApplyHook;
    internal static Func<(int Rc, uint[] Items)>? AsksHook;
    internal static Action<uint[]>? AskHook;
    internal static Func<bool>? ServerSideHook;

    internal static void Attach(NetChannel channel)
    {
        if (_attached) return;
        _attached = true;
        _channel = channel;
        // An older peer has no such message ids and drops them.
        channel.Register<LockStockRows>(LockStockRows.Id, LockStockRows.Encode,
            static (ref NetReader r) => LockStockRows.Decode(ref r), static (m, _) => { if (!Client.IsHosting) ClientRows(m); },
            NetDirection.ToClient);
        channel.Register<LockStockAsk>(LockStockAsk.Id, LockStockAsk.Encode,
            static (ref NetReader r) => LockStockAsk.Decode(ref r), static (m, ctx) => Enqueue(ctx.Player, m.Items), NetDirection.ToServer);
        try
        {
            if (Client.IsClient) Ui.Screens.UiTick += ClientTick;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or TypeInitializationException)
        {
            ModLog.Warn($"crafting: this client's lock checks do not count linked sources: {e.Message}");
        }
    }

    // ================================================================ server ====

    /// <summary>A lock-opted link was registered for the player (tick thread). Returns the session it counts in: the
    /// handle gives it back to <see cref="Untrack"/>, so a handle of an earlier login never ends a later login's session.</summary>
    internal static object? Track(EntityId player)
    {
        if (player.IsNone) return null;
        Session s;
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var found)) Sessions[player.Value] = found = new Session();
            s = found;
            s.Links++;
            s.Due = true;
            _tick ??= ServiceTick.Add("crafting lock sources", static (in TickInfo t) => Pump(t.Step));
        }
        WatchPlayers();
        return s;
    }

    /// <summary>A lock-opted link was disposed (any thread). After the last one the server's next tick sends the player
    /// "unsupported" and ends the session. A handle of a session that already ended (the player left) changes nothing.</summary>
    internal static void Untrack(EntityId player, object? session)
    {
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var s) || !ReferenceEquals(s, session) || s.Links == 0) return;
            if (--s.Links == 0) s.Due = true;
        }
    }

    /// <summary>A client's asks (any thread): the server's tick takes them.</summary>
    internal static void Enqueue(EntityId player, uint[] items)
    {
        if (player.IsNone || items.Length == 0 || Inbox.Count > 1024) return;
        Inbox.Enqueue((player, items));
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
            Events.On<PlayerJoinedEvent>(static e => Joined(e.Player.Entity));
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or TypeInitializationException or FileNotFoundException)
        {
            ModLog.Warn($"crafting: lock sources do not see players leave ({e.Message})");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>A player left: that entity's links are another login's (the native registry no longer lets them pay),
    /// and its session goes; the old login's handles no longer count in any session.</summary>
    internal static void Gone(EntityId player)
    {
        lock (Gate) Sessions.Remove(player.Value);
    }

    /// <summary>Someone joined into the slot. Its session is kept: a mod's own join handler may already have linked a
    /// chest for the new login (handlers run in priority order, Kiln's is not first). The new client has asked nothing
    /// and been sent nothing: its asks start over and the next tick sends it the current message.</summary>
    internal static void Joined(EntityId player)
    {
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var s)) return;
            s.Asked.Clear();
            s.Sent = null;
            s.Due = true;
        }
    }

    /// <summary>Asks (tick thread): added to the player's session (oldest dropped past <see cref="MaxRows"/>).</summary>
    internal static void Ask(EntityId player, IReadOnlyList<uint> items)
    {
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var s)) return;  // no lock-opted link: nothing to answer
            foreach (var item in items)
            {
                if (item == 0) continue;
                s.Due = true;  // asked again: the client has no row, so the rows go out again
                if (s.Asked.Contains(item)) continue;
                if (s.Asked.Count >= MaxRows) s.Asked.RemoveAt(0);
                s.Asked.Add(item);
            }
        }
    }

    /// <summary>The server's tick (while any player has a lock-opted link): the asks, then the sessions due.</summary>
    internal static void Pump(ulong step)
    {
        var n = 0;
        while (n++ < 256 && Inbox.TryDequeue(out var a)) Ask(a.Player, a.Items);
        List<(EntityId Player, Session S, uint[] Asked)>? due = null;
        lock (Gate)
        {
            // A new world restarts the step count: links of the old one were cleared natively, and their players left
            // (Gone); a mod's new registrations are tracked again, so nothing is cleared here.
            var refresh = step >= _lastStep + RefreshSteps || step < _lastStep;
            if (refresh) _lastStep = step;
            foreach (var (p, s) in Sessions)
                if (s.Due || refresh) (due ??= []).Add((new EntityId(p), s, [.. s.Asked]));
        }
        if (due is null) return;
        foreach (var (player, s, asked) in due)
        {
            LockStockRows m;
            bool closing;
            lock (Gate) closing = s.Links == 0;
            if (closing)
            {
                // the last lock-opted link went: the client's lock checks are the game's own again
                try { Deliver(player, Off); }
                catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException) { }
                lock (Gate)
                    if (Sessions.TryGetValue(player.Value, out var live) && ReferenceEquals(live, s) && s.Links == 0) Sessions.Remove(player.Value);
                continue;
            }
            try
            {
                m = Build(player, asked);
            }
            catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or ArgumentException)
            {
                m = Off;
            }
            var key = Key(m);
            lock (Gate)
            {
                if (!Sessions.TryGetValue(player.Value, out var live) || !ReferenceEquals(live, s)) continue;
                if (!s.Due && s.Sent == key) continue;
            }
            var ok = Deliver(player, m);
            lock (Gate)
            {
                s.Due = false;
                s.Sent = ok ? key : null;
            }
        }
    }

    /// <summary>What the player is sent now: unsupported without the capability or the links' counts; else a row per
    /// asked item (0 when the links hold none).</summary>
    internal static LockStockRows Build(EntityId player, IReadOnlyList<uint> asked)
    {
        var supported = SupportedHook?.Invoke() ?? (Capabilities.Is(Crafting.LockSourcesCapability) && CraftingCore.HasLocks);
        if (!supported) return Off;
        var (rc, rows) = RowsHook?.Invoke(player) ?? CraftingCore.LockStock(player);
        if (rc != StationsNative.Ok) return Off;
        return Compose(asked, rows);
    }

    /// <summary>One message on plain values: a row per asked item, sorted, with what the links hold of it.</summary>
    internal static LockStockRows Compose(IReadOnlyList<uint> asked, IReadOnlyList<StationsNative.Stack> held)
    {
        var have = new Dictionary<uint, uint>();
        foreach (var r in held) if (r.Item != 0) have[r.Item] = r.Count;
        var rows = asked.Where(i => i != 0).Distinct().Order().Take(MaxRows)
            .Select(i => new CraftStockRow(i, have.GetValueOrDefault(i))).ToArray();
        return new LockStockRows(true, rows);
    }

    internal static string Key(LockStockRows m)
    {
        var b = new StringBuilder(m.Supported ? "on;" : "off;");
        foreach (var r in m.Rows) b.Append(r.Item).Append('=').Append(r.Count).Append(';');
        return b.ToString();
    }

    // To the player's Kiln client: in this process for the hosting player, else over the channel to a peer that has it.
    private static bool Deliver(EntityId player, LockStockRows m)
    {
        if (SendHook is { } hook) return hook(player, m);
        if (Client.IsHosting && Client.HasLocalPlayer && Client.LocalPlayer == player) return ClientApply(m);
        if (_channel is null || Network.PeerOf(player) is not { } peer || !peer.HasChannel(CraftStock.ChannelName)) return false;
        return _channel.SendTo(player, m);
    }

    /// <summary>Tests: the sessions' asks and whether one is open.</summary>
    internal static (bool Open, int Links, IReadOnlyList<uint> Asked) SessionOf(EntityId player)
    {
        lock (Gate) return Sessions.TryGetValue(player.Value, out var s) ? (true, s.Links, [.. s.Asked]) : (false, 0, []);
    }

    // ================================================================ client ====

    private static volatile bool _on;
    private static long _lastPoll, _lastAskSent;
    private static string _lastAsked = "";

    private static bool ServerSide() => ServerSideHook?.Invoke() ?? (Client.IsHosting || Network.ServerHasKiln);

    // The server's message (the client's main thread).
    internal static void ClientRows(LockStockRows m) => ClientApply(m);

    /// <summary>Hands the server's message to kiln-items: on with its rows, or off (the game's own lock checks).</summary>
    internal static bool ClientApply(LockStockRows m)
    {
        var on = m.Supported && ServerSide();
        _on = on;
        if (!on) _lastAsked = "";
        var rows = on ? m.Rows.Take(MaxRows).Select(r => new StationsNative.Stack { Item = r.Item, Count = r.Count }).ToArray() : [];
        var rc = ApplyHook?.Invoke(on, rows) ?? CraftingCore.SetLockStock(on, rows);
        return rc == StationsNative.Ok;
    }

    // Every UI frame: the asks every 250 ms while the server's snapshot is on.
    internal static void ClientTick(GameScreen now) => PollAsks(Environment.TickCount64);

    /// <summary>The lock items the client's lock checks met with no row go to the server once per set, again after 5 s
    /// (an ask or its answer may have been lost).</summary>
    internal static void PollAsks(long nowMs)
    {
        if (!_on || nowMs - _lastPoll < (long)AskPoll.TotalMilliseconds) return;
        _lastPoll = nowMs;
        var (rc, items) = AsksHook?.Invoke() ?? CraftingCore.LockAsks();
        if (rc != StationsNative.Ok || items.Length == 0) return;
        var text = string.Join(';', items);
        if (text == _lastAsked && nowMs - _lastAskSent < (long)AskAgain.TotalMilliseconds) return;
        _lastAsked = text;
        _lastAskSent = nowMs;
        if (AskHook is { } hook)
        {
            hook(items);
            return;
        }
        if (Client.IsHosting && Client.HasLocalPlayer) Enqueue(Client.LocalPlayer, items);  // the server half in this process
        else _channel?.SendToServer(new LockStockAsk(items));
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
        }
        while (Inbox.TryDequeue(out _)) { }
        _on = false;
        _lastPoll = _lastAskSent = 0;
        _lastAsked = "";
        RowsHook = null;
        SendHook = null;
        SupportedHook = null;
        ApplyHook = null;
        AsksHook = null;
        AskHook = null;
        ServerSideHook = null;
    }
}
