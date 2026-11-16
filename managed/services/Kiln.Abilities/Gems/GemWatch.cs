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
    private static ulong? _refreshed;  // the step of the last look for players; null: never (or forgotten)
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
        _refreshed = null;
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
            // players are looked for at the first step and then every PlayerRefreshSteps steps, also when none was
            // found (World.Find is one pass over the entity map: review), and at once when the steps started again (a
            // new world of a hosting game: the old world's snapshots name its item entities, so they go: review)
            if (_refreshed is not { } at || step < at || step - at >= PlayerRefreshSteps)
            {
                if (_refreshed is { } last && step < last) Last.Clear();
                _refreshed = step;
                _players = [];  // until a look succeeds: with no server world World.Find throws (a client's menu: review)
                _players = world.Players().Take(1024).ToList();
                var alive = _players.Select(p => p.Value).ToHashSet();
                foreach (var gone in Last.Keys.Where(k => !alive.Contains(k)).ToList()) Last.Remove(gone);
            }
            if (_players.Count == 0) return found;
            // the gem ids from the item catalog alone, not the gem catalog (no kiln.gamedata reads: review)
            var gems = GemReads.GemIds(world);
            for (var n = 0; n < PlayersPerStep && n < _players.Count; n++)
            {
                if (_next >= _players.Count) _next = 0;
                var player = _players[_next++];
                var items = world.ReadHolder(player);
                if (items is null)
                {
                    // gone (logged out) or not set up: its snapshot goes too, so a player who comes back under this id,
                    // or another one who gets it, is first seen again instead of compared with the old items (review)
                    Last.Remove(player.Value);
                    continue;
                }
                var now = GemSnapshot.Of(items, g => gems.Contains(g.Value));
                if (Last.TryGetValue(player.Value, out var before))
                    foreach (var d in Diff(before, now, world.Read))
                    {
                        found.Add((player, d));
                        if (post) Events.Post(new GemSocketChangedEvent(player, d.Carrier, d.Before, d.After, d.Change, step));
                    }
                Last[player.Value] = now;
            }
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
            // a read the world cannot give now (no server world: a game client's menu or joined session; the service
            // off): tried again at the next turn, said once (review: InvalidOperationException went out to the tick)
            if (!_faultLogged) ModLog.Warn($"gems: socket changes are not watched now: {e.Message}");
            _faultLogged = true;
        }
        return found;
    }

    /// <summary>The socket changes between two reads of one player's items. A carrier the second read does not have is
    /// a change only when it is gone (<paramref name="read"/> finds nothing): then its gem is Removed with it. An emptied
    /// socket is Unsocketed while gems of its kind came back to the inventory: the loose ones of that kind now less
    /// before, plus the sockets (of carriers in both reads) that got that kind meanwhile (a gem moved from one weapon to
    /// another left the loose count as it was), less the gems swaps put back; each return counts once (review). Pure
    /// apart from <paramref name="read"/>.</summary>
    internal static List<GemSocketDiff> Diff(GemSnapshot before, GemSnapshot after, Func<ItemInstanceRef, GemItemRead?> read)
    {
        var list = new List<GemSocketDiff>();
        var carriers = before.Sockets.OrderBy(k => k.Key.Entity.Id.Value).ToList();
        var filled = new Dictionary<uint, int>();
        foreach (var (carrier, (gem, _)) in carriers)
            if (after.Sockets.TryGetValue(carrier, out var now) && !now.Gem.IsNone && now.Gem != gem)
                filled[now.Gem.Value] = filled.GetValueOrDefault(now.Gem.Value) + 1;
        var returns = new Dictionary<uint, int>();
        int Returns(uint kind)
        {
            if (!returns.TryGetValue(kind, out var n))
                returns[kind] = n = after.LooseGems.GetValueOrDefault(kind) - before.LooseGems.GetValueOrDefault(kind) +
                                    filled.GetValueOrDefault(kind);
            return n;
        }
        foreach (var (carrier, (gem, _)) in carriers)  // a swap's old gem came back first
            if (!gem.IsNone && after.Sockets.TryGetValue(carrier, out var now) && !now.Gem.IsNone && now.Gem != gem)
                returns[gem.Value] = Returns(gem.Value) - 1;
        foreach (var (carrier, (gem, level)) in carriers)
        {
            var was = gem.IsNone ? (SocketedGem?)null : new SocketedGem(carrier, gem, level);
            if (after.Sockets.TryGetValue(carrier, out var now))
            {
                var nowGem = now.Gem.IsNone ? (SocketedGem?)null : new SocketedGem(carrier, now.Gem, now.Level);
                if (was is null && nowGem is null) continue;
                if (was is null) list.Add(new(carrier, null, nowGem, GemSocketChange.Socketed));
                else if (nowGem is null)
                {
                    var back = Returns(gem.Value) > 0;
                    if (back) returns[gem.Value]--;
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
/// read once per root and step, and each impact's answer, found once per impact and step.</summary>
internal static class GemTriggers
{
    /// <summary>How far up the impact hierarchy a spawned impact is followed to its gem.</summary>
    internal const int ParentHops = 3;

    private static readonly Dictionary<uint, IReadOnlyList<EquippedGem>> PerRoot = new();
    private static readonly Dictionary<uint, EquippedGem?> PerSelf = new();
    private static ulong _step = ulong.MaxValue;

    /// <summary>Forgets the step's lookups (tests).</summary>
    internal static void Reset()
    {
        PerRoot.Clear();
        PerSelf.Clear();
        _step = ulong.MaxValue;
    }

    /// <summary>The equipped gem whose impact is <paramref name="impact"/>'s self (or an ancestor of it); null when none.
    /// A root whose equipment cannot be read has no gem for the rest of the step. Tick thread.</summary>
    internal static EquippedGem? Find(IGemWorld world, ImpactRef impact, ulong step)
    {
        if (impact.Root.IsNone || impact.Self.IsNone) return null;
        if (step != _step)
        {
            _step = step;
            PerRoot.Clear();
            PerSelf.Clear();
        }
        if (!PerRoot.TryGetValue(impact.Root.Value, out var gems))
        {
            try
            {
                gems = GemReads.Equipped(world, impact.Root);
            }
            catch (KilnUnavailableException)
            {
                gems = [];  // the row says why
            }
            catch (Exception e)
            {
                gems = [];  // one read per root and step, also when it failed (review)
                Fault(e);
            }
            PerRoot[impact.Root.Value] = gems;
        }
        if (gems.Count == 0) return null;
        // one walk per impact and step: a gem wearer's other impacts (swings, perks, buffs) call natives often, and each
        // hop is a kiln.world read (review)
        if (PerSelf.TryGetValue(impact.Self.Value, out var known)) return known;
        EquippedGem? found = null;
        var cur = impact.Self;
        for (var hop = 0; hop <= ParentHops && !cur.IsNone && found is null; hop++)
        {
            foreach (var g in gems)
                if (g.Impact == cur)
                {
                    found = g;
                    break;
                }
            if (found is null && hop < ParentHops) cur = world.ParentImpact(cur);
        }
        PerSelf[impact.Self.Value] = found;
        return found;
    }

    /// <summary>Posts the gem event for a native event when its impact is a gem's. Tick thread (the listener): nothing
    /// here may throw into it, or the listener loses the rest of its batch (review: any exception, not three kinds).</summary>
    internal static void Deliver(ImpactNativeEvent native, ulong step)
    {
        GemTriggeredEvent ev;
        try
        {
            if (Find(GemReads.World, native.Impact, step) is not { } gem) return;
            ev = new GemTriggeredEvent(gem, native);
        }
        catch (Exception e)
        {
            Fault(e);
            return;
        }
        Events.Post(ev);
    }

    private static void Fault(Exception e)
    {
        if (!_faultLogged) ModLog.Warn($"gems: a native event's gem could not be read ({e.GetType().Name}): {e.Message}");
        _faultLogged = true;
    }

    private static bool _faultLogged;
}
