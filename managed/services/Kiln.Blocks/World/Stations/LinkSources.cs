// SPDX-License-Identifier: MIT
// GitHub #132, since 0.2.0-dev.31.12: production-station fills and block placements from the player's linked sources,
// the managed halves.
//  - Fills (CraftSourceOptions.Fills): LinkPurposes.Fills knows which players have a fill-opted link, so the station
//    screen's "+1" / "+10" (StationsCore.SourceFill) and a Kiln client's fill answers (StationsCore.FillCrafts) ask the
//    native fill for those links (FILL_LINKED). The fill itself, its per-input shares and every check are native.
//  - Building (CraftSourceOptions.Building): the server counts the links itself in its own build cursor and pays the
//    blueprint placement from them (native). A Kiln client's own cursor needs the counts too: the server sends a player
//    with a building link, on the channel kiln:craftstock (required: false), whether it supports building from links and
//    what those links hold per item (the bag and the base not included), again when a count changes (every 30 steps it
//    looks). Not authority: the server checks the placement again and pays bag and base first, all or nothing.
// docs/kiln/services/stations.md, "Fills and building from linked sources".
using System.Text;

namespace Kiln;

/// <summary>How many live handles of each player carry one purpose (Fills): a hint for where to ask the native fill to
/// use links. The native registry stays the authority (login, access, filter).</summary>
internal sealed class LinkPurposeTracker
{
    private sealed class Session
    {
        public int Links;
    }

    private readonly object _gate = new();
    private readonly Dictionary<uint, Session> _sessions = [];

    /// <summary>A handle with the purpose was registered (tick thread). The session it counts in goes back to
    /// <see cref="Untrack"/>, so a handle of an earlier login never ends a later login's.</summary>
    internal object? Track(EntityId player)
    {
        if (player.IsNone) return null;
        Session s;
        lock (_gate)
        {
            if (!_sessions.TryGetValue(player.Value, out var found)) _sessions[player.Value] = found = new Session();
            s = found;
            s.Links++;
        }
        BuildStock.WatchPlayers();  // a leaving player's links end with it
        return s;
    }

    internal void Untrack(EntityId player, object? session)
    {
        lock (_gate)
        {
            if (!_sessions.TryGetValue(player.Value, out var s) || !ReferenceEquals(s, session) || s.Links == 0) return;
            if (--s.Links == 0) _sessions.Remove(player.Value);
        }
    }

    internal bool Has(EntityId player)
    {
        lock (_gate) return _sessions.TryGetValue(player.Value, out var s) && s.Links > 0;
    }

    internal bool Any
    {
        get { lock (_gate) return _sessions.Count > 0; }
    }

    /// <summary>The player left: that entity's links are another login's now.</summary>
    internal void Gone(EntityId player)
    {
        lock (_gate) _sessions.Remove(player.Value);
    }

    internal void Reset()
    {
        lock (_gate) _sessions.Clear();
    }
}

internal static class LinkPurposes
{
    internal static readonly LinkPurposeTracker Fills = new();
}

/// <summary>Server to client: whether the server counts and pays block placements from the player's building links,
/// and what those links hold per item. Unsupported: the game's own material checks.</summary>
internal sealed record BuildStockRows(bool Supported, CraftStockRow[] Rows)
{
    internal const ushort Id = 9;  // after #147's 7 and 8

    internal static void Encode(BuildStockRows m, NetWriter w)
    {
        var n = Math.Min(m.Rows.Length, BuildStock.MaxRows);
        w.Bool(m.Supported).UInt16((ushort)n);
        for (var i = 0; i < n; i++) w.UInt32(m.Rows[i].Item).UInt32(m.Rows[i].Count);
    }

    internal static BuildStockRows Decode(ref NetReader r)
    {
        var supported = r.Bool();
        var n = r.UInt16();
        if (n > BuildStock.MaxRows) throw new InvalidDataException("too many building rows");
        var rows = new CraftStockRow[n];
        var seen = new HashSet<uint>();
        for (var i = 0; i < n; i++)
        {
            var item = r.UInt32();
            var count = r.UInt32();
            if (item == 0 || !seen.Add(item)) throw new InvalidDataException("invalid building row");
            rows[i] = new(item, count);
        }
        return new(supported, supported ? rows : []);
    }
}

internal static class BuildStock
{
    internal const int MaxRows = StationsNative.MaxCraftStock;
    internal const ulong RefreshSteps = 30;  // ~1.2 s, as the crafting menu's snapshot
    internal static readonly BuildStockRows Off = new(false, []);

    private sealed class Session
    {
        public int Links;     // live building-opted handles of this player
        public string? Sent;  // the key of the last message that went out; null: none or failed
        public bool Due = true;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<uint, Session> Sessions = [];
    private static NetChannel? _channel;
    private static IDisposable? _tick;
    private static bool _attached, _playersWatched;
    private static ulong _lastStep;

    /// <summary>Test hooks: the links' counts (instead of native source_stock), a delivery, the client's native call.</summary>
    internal static Func<EntityId, (int Rc, StationsNative.Stack[] Rows)>? RowsHook;
    internal static Func<EntityId, BuildStockRows, bool>? SendHook;
    internal static Func<bool>? SupportedHook;
    internal static Func<bool, StationsNative.Stack[], int>? ApplyHook;
    internal static Func<bool>? ServerSideHook;

    internal static void Attach(NetChannel channel)
    {
        if (_attached) return;
        _attached = true;
        _channel = channel;
        // An older peer has no such message id and drops it.
        channel.Register<BuildStockRows>(BuildStockRows.Id, BuildStockRows.Encode,
            static (ref NetReader r) => BuildStockRows.Decode(ref r), static (m, _) => { if (!Client.IsHosting) ClientApply(m); },
            NetDirection.ToClient);
    }

    // ================================================================ server ====

    /// <summary>A building-opted link was registered for the player (tick thread); returns the session it counts in.</summary>
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
            _tick ??= ServiceTick.Add("crafting build sources", static (in TickInfo t) => Pump(t.Step));
        }
        WatchPlayers();
        return s;
    }

    /// <summary>A building-opted link was disposed (any thread). After the last one the next tick sends "unsupported".</summary>
    internal static void Untrack(EntityId player, object? session)
    {
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var s) || !ReferenceEquals(s, session) || s.Links == 0) return;
            if (--s.Links == 0) s.Due = true;
        }
    }

    internal static void WatchPlayers()
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
            ModLog.Warn($"crafting: fill and building sources do not see players leave ({e.Message})");
        }
        finally
        {
            ModContext.CurrentMod = outer;
        }
    }

    /// <summary>A player left: that entity's links are another login's (the native registry no longer lets them pay).</summary>
    internal static void Gone(EntityId player)
    {
        lock (Gate) Sessions.Remove(player.Value);
        LinkPurposes.Fills.Gone(player);
    }

    /// <summary>Someone joined into the slot: a mod's join handler may already have linked for the new login; the new client
    /// was sent nothing, so the next tick sends it the current message.</summary>
    internal static void Joined(EntityId player)
    {
        lock (Gate)
        {
            if (!Sessions.TryGetValue(player.Value, out var s)) return;
            s.Sent = null;
            s.Due = true;
        }
    }

    /// <summary>The server's tick (while any player has a building link): the sessions due, at most every 30 steps.</summary>
    internal static void Pump(ulong step)
    {
        List<(EntityId Player, Session S)>? due = null;
        lock (Gate)
        {
            var refresh = step >= _lastStep + RefreshSteps || step < _lastStep;
            if (refresh) _lastStep = step;
            foreach (var (p, s) in Sessions)
                if (s.Due || refresh) (due ??= []).Add((new EntityId(p), s));
        }
        if (due is null) return;
        foreach (var (player, s) in due)
        {
            bool closing;
            lock (Gate) closing = s.Links == 0;
            if (closing)
            {
                try { Deliver(player, Off); }
                catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException) { }
                lock (Gate)
                    if (Sessions.TryGetValue(player.Value, out var live) && ReferenceEquals(live, s) && s.Links == 0) Sessions.Remove(player.Value);
                continue;
            }
            BuildStockRows m;
            try
            {
                m = Build(player);
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

    /// <summary>What the player is sent now: unsupported without the capability or the links' counts, else every item the
    /// building links hold (sorted, at most <see cref="MaxRows"/>).</summary>
    internal static BuildStockRows Build(EntityId player)
    {
        var supported = SupportedHook?.Invoke() ?? (Capabilities.Is(Crafting.BuildSourcesCapability) && CraftingCore.HasPurposes);
        if (!supported) return Off;
        var (rc, rows) = RowsHook?.Invoke(player) ?? CraftingCore.BuildStockRows(player);
        if (rc != StationsNative.Ok) return Off;
        return new BuildStockRows(true, rows.Where(r => r.Item != 0 && r.Count != 0).DistinctBy(r => r.Item).OrderBy(r => r.Item)
            .Take(MaxRows).Select(r => new CraftStockRow(r.Item, r.Count)).ToArray());
    }

    internal static string Key(BuildStockRows m)
    {
        var b = new StringBuilder(m.Supported ? "on;" : "off;");
        foreach (var r in m.Rows) b.Append(r.Item).Append('=').Append(r.Count).Append(';');
        return b.ToString();
    }

    // To the player's Kiln client: in this process for the hosting player, else over the channel to a peer that has it.
    private static bool Deliver(EntityId player, BuildStockRows m)
    {
        if (SendHook is { } hook) return hook(player, m);
        if (Client.IsHosting && Client.HasLocalPlayer && Client.LocalPlayer == player) return ClientApply(m);
        if (_channel is null || Network.PeerOf(player) is not { } peer || !peer.HasChannel(CraftStock.ChannelName)) return false;
        return _channel.SendTo(player, m);
    }

    /// <summary>Tests: whether a session is open and how many links it counts.</summary>
    internal static (bool Open, int Links) SessionOf(EntityId player)
    {
        lock (Gate) return Sessions.TryGetValue(player.Value, out var s) ? (true, s.Links) : (false, 0);
    }

    // ================================================================ client ====

    private static bool ServerSide() => ServerSideHook?.Invoke() ?? (Client.IsHosting || Network.ServerHasKiln);

    /// <summary>Hands the server's message to kiln-items: on with its rows, or off (the game's own material checks).</summary>
    internal static bool ClientApply(BuildStockRows m)
    {
        var on = m.Supported && ServerSide();
        var rows = on ? m.Rows.Take(MaxRows).Select(r => new StationsNative.Stack { Item = r.Item, Count = r.Count }).ToArray() : [];
        var rc = ApplyHook?.Invoke(on, rows) ?? CraftingCore.SetBuildStock(on, rows);
        return rc == StationsNative.Ok;
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
        LinkPurposes.Fills.Reset();
        RowsHook = null;
        SendHook = null;
        SupportedHook = null;
        ApplyHook = null;
        ServerSideHook = null;
    }
}
