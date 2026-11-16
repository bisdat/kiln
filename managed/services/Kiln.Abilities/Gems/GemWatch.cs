// SPDX-License-Identifier: MIT
// Gem wave G1: GemSocketChangedEvent by polling players' items (as kiln.items' tracker polls followed items), and the
// impact -> gem map behind GemTriggeredEvent. docs/kiln/services/gems.md.
namespace Kiln;

/// <summary>What one read of a player's items keeps: every item's socket (none: empty or no socket) and how many loose
/// gems of each item the player carries.</summary>
internal sealed record GemSnapshot(IReadOnlyDictionary<ItemInstanceRef, (ItemId Gem, int Level)> Sockets,
    IReadOnlyDictionary<uint, int> LooseGems)
{
    internal static GemSnapshot Of(IReadOnlyList<GemItemRead> items, Func<ItemId, bool> isGem)
    {
        var sockets = new Dictionary<ItemInstanceRef, (ItemId, int)>(items.Count);
        var loose = new Dictionary<uint, int>();
        foreach (var i in items)
        {
            if (i.Item.IsNone) continue;
            sockets[i.Item] = (i.Gem, i.Gem.IsNone ? 0 : i.GemLevel);
            if (isGem(i.Item.Item)) loose[i.Item.Item.Value] = loose.GetValueOrDefault(i.Item.Item.Value) + 1;
        }
        return new GemSnapshot(sockets, loose);
    }
}

/// <summary>One socket change found by <see cref="GemWatch.Diff"/>.</summary>
internal readonly record struct GemSocketDiff(ItemInstanceRef Carrier, SocketedGem? Before, SocketedGem? After, GemSocketChange Change);

/// <summary>Polls players' items while <see cref="GemSocketChangedEvent"/> has subscribers.</summary>
internal static class GemWatch
{
    /// <summary>Players read per step.</summary>
    internal const int PlayersPerStep = 2;

    /// <summary>Steps between two looks for players.</summary>
    internal const int PlayerRefreshSteps = 50;

    private static readonly Dictionary<uint, GemSnapshot> Last = new();
    private static List<EntityId> _players = [];
    private static ulong _refreshed;
    private static int _next;
    private static IDisposable? _tick;
    private static int _generation = -1;
    private static bool _faultLogged;

    internal static void Hook()
    {
        lock (Last)
        {
            if (_tick is not null && _generation == ServiceTick.Generation) return;
            _generation = ServiceTick.Generation;
            _tick = ServiceTick.Add("kiln.abilities gem sockets", (in TickInfo t) => Step(GemReads.World, t.Step, post: true));
        }
    }

    /// <summary>Forgets every snapshot (tests).</summary>
    internal static void Reset()
    {
        Last.Clear();
        _players = [];
        _refreshed = 0;
        _next = 0;
    }

    /// <summary>One step: reads the next <see cref="PlayersPerStep"/> players and posts what changed. Returns the
    /// changes (tests). Tick thread.</summary>
    internal static List<(EntityId Owner, GemSocketDiff Diff)> Step(IGemWorld world, ulong step, bool post)
    {
        var found = new List<(EntityId, GemSocketDiff)>();
        if (post && !Events.HasSubscribers<GemSocketChangedEvent>())
        {
            if (Last.Count > 0) Reset();
            return found;
        }
        try
        {
            if (_players.Count == 0 || step >= _refreshed + PlayerRefreshSteps)
            {
                _refreshed = step;
                _players = world.Players().Take(1024).ToList();
                var alive = _players.Select(p => p.Value).ToHashSet();
                foreach (var gone in Last.Keys.Where(k => !alive.Contains(k)).ToList()) Last.Remove(gone);
            }
            for (var n = 0; n < PlayersPerStep && n < _players.Count; n++)
            {
                if (_next >= _players.Count) _next = 0;
                var player = _players[_next++];
                var items = world.ReadHolder(player);
                if (items is null) continue;
                var now = GemSnapshot.Of(items, g => GemReads.IsGem(world, g));
                if (Last.TryGetValue(player.Value, out var before))
                    foreach (var d in Diff(before, now, world.Read))
                    {
                        found.Add((player, d));
                        if (post) Events.Post(new GemSocketChangedEvent(player, d.Carrier, d.Before, d.After, d.Change, step));
                    }
                Last[player.Value] = now;
            }
        }
        catch (KilnUnavailableException e)
        {
            if (!_faultLogged) ModLog.Warn($"gems: socket changes are not watched: {e.Message}");
            _faultLogged = true;
        }
        return found;
    }

    /// <summary>The socket changes between two reads of one player's items. A carrier the second read does not have is
    /// a change only when it is gone (<paramref name="read"/> finds nothing): then its gem is Removed with it. Pure
    /// apart from <paramref name="read"/>.</summary>
    internal static List<GemSocketDiff> Diff(GemSnapshot before, GemSnapshot after, Func<ItemInstanceRef, GemItemRead?> read)
    {
        var list = new List<GemSocketDiff>();
        foreach (var (carrier, (gem, level)) in before.Sockets.OrderBy(k => k.Key.Entity.Id.Value))
        {
            var was = gem.IsNone ? (SocketedGem?)null : new SocketedGem(carrier, gem, level);
            if (after.Sockets.TryGetValue(carrier, out var now))
            {
                var nowGem = now.Gem.IsNone ? (SocketedGem?)null : new SocketedGem(carrier, now.Gem, now.Level);
                if (was is null && nowGem is null) continue;
                if (was is null) list.Add(new(carrier, null, nowGem, GemSocketChange.Socketed));
                else if (nowGem is null)
                {
                    var back = after.LooseGems.GetValueOrDefault(gem.Value) > before.LooseGems.GetValueOrDefault(gem.Value);
                    list.Add(new(carrier, was, null, back ? GemSocketChange.Unsocketed : GemSocketChange.Removed));
                }
                else if (now.Gem != gem) list.Add(new(carrier, was, nowGem, GemSocketChange.Socketed));
                else if (now.Level != level) list.Add(new(carrier, was, nowGem, GemSocketChange.LevelChanged));
                continue;
            }
            if (was is null) continue;
            if (read(carrier) is null) list.Add(new(carrier, was, null, GemSocketChange.Removed));
        }
        return list;
    }
}

/// <summary>Which gem a native event's impact belongs to (<see cref="GemTriggeredEvent"/>): the roots' equipped gems,
/// read once per root and step.</summary>
internal static class GemTriggers
{
    /// <summary>How far up the impact hierarchy a spawned impact is followed to its gem.</summary>
    internal const int ParentHops = 3;

    private static readonly Dictionary<uint, IReadOnlyList<EquippedGem>> PerRoot = new();
    private static ulong _step = ulong.MaxValue;

    /// <summary>The equipped gem whose impact is <paramref name="impact"/>'s self (or an ancestor of it); null when none.
    /// Tick thread.</summary>
    internal static EquippedGem? Find(IGemWorld world, ImpactRef impact, ulong step)
    {
        if (impact.Root.IsNone || impact.Self.IsNone) return null;
        if (step != _step)
        {
            _step = step;
            PerRoot.Clear();
        }
        if (!PerRoot.TryGetValue(impact.Root.Value, out var gems))
        {
            try
            {
                gems = GemReads.Equipped(world, impact.Root);
            }
            catch (KilnUnavailableException)
            {
                gems = [];
            }
            PerRoot[impact.Root.Value] = gems;
        }
        if (gems.Count == 0) return null;
        var e = impact.Self;
        for (var hop = 0; hop <= ParentHops && !e.IsNone; hop++)
        {
            foreach (var g in gems)
                if (g.Impact == e) return g;
            if (hop < ParentHops) e = world.ParentImpact(e);
        }
        return null;
    }

    /// <summary>Posts the gem event for a native event when its impact is a gem's. Tick thread (the listener).</summary>
    internal static void Deliver(ImpactNativeEvent native, ulong step)
    {
        EquippedGem? gem;
        try
        {
            gem = Find(GemReads.World, native.Impact, step);
        }
        catch (Exception e) when (e is InvalidOperationException or ArgumentException or KilnUnavailableException)
        {
            // never lose the rest of the listener's batch over one read
            if (!_faultLogged) ModLog.Warn($"gems: a native event's gem could not be read: {e.Message}");
            _faultLogged = true;
            return;
        }
        if (gem is not null) Events.Post(new GemTriggeredEvent(gem, native));
    }

    private static bool _faultLogged;
}
