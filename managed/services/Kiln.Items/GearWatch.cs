// SPDX-License-Identifier: MIT
// The gear watch: the game's own changes of an item's enhancements and level (a player's unlock at the workbench, the
// game's free +1 / -1) are no write of Kiln's, so they are found by reading. While a gear event has subscribers, the
// service tick reads one player's items per step (kiln.items' inventory read, then its gear read of each item entity) and
// compares them with the last read. Kiln's own writes are told to the watch when they are made and when their result
// arrives, so they are never reported twice. kiln.component_changes does not see gear writes (they are kiln.items'
// tracked writes, not a watched component): the poll is the one path.
namespace Kiln;

internal static unsafe class GearWatch
{
    /// <summary>How often the list of players in game is read again (steps).</summary>
    internal const int PlayersEvery = 25;

    private sealed class Seen(ItemInstanceRef item, EntityId holder, GearState state)
    {
        public readonly ItemInstanceRef Item = item;
        public EntityId Holder = holder;
        public GearState State = state;
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<EntityId, Seen> Last = new();     // by item entity
    private static readonly Dictionary<ItemInstanceRef, int> InFlight = new();  // Kiln's own writes not answered yet (by ref: a stamp is never reused)
    private static IReadOnlyList<EntityId> _players = [];
    private static ulong _playersAt;
    private static int _cursor;
    private static long _session = -1;
    private static IDisposable? _tick;

    /// <summary>Kiln.Items' start: the watch's service tick (it reads nothing while no gear event has subscribers).</summary>
    internal static void Start()
    {
        lock (Gate) _tick ??= ServiceTickAccess.Add("kiln.items gear watch", Step);
    }

    /// <summary>Items the watch knows now (tests).</summary>
    internal static int Count
    {
        get
        {
            lock (Gate) return Last.Count;
        }
    }

    /// <summary>A write of Kiln's for the item was asked for: the watch leaves the item alone until its result.</summary>
    internal static void Begin(ItemInstanceRef item)
    {
        lock (Gate) InFlight[item] = InFlight.GetValueOrDefault(item) + 1;
    }

    /// <summary>A write of Kiln's for the item was answered: <paramref name="after"/> (null: nothing changed) is what the
    /// watch knows of it from now, so the change is not reported again as the game's.</summary>
    internal static void End(ItemInstanceRef item, GearState? after = null)
    {
        lock (Gate)
        {
            if (InFlight.TryGetValue(item, out var n))
            {
                if (n <= 1) InFlight.Remove(item);
                else InFlight[item] = n - 1;
            }
            if (after != null && Last.TryGetValue(item.Entity.Id, out var seen) && seen.Item == item) seen.State = after;
        }
    }

    /// <summary>Forgets everything (tests; a world change).</summary>
    internal static void Reset()
    {
        lock (Gate)
        {
            Last.Clear();
            InFlight.Clear();
            _players = [];
            _playersAt = 0;
            _cursor = 0;
            _session = -1;
        }
    }

    private static void Step(ulong step)
    {
        if (!GearEvents.Wanted)
        {
            lock (Gate) if (Last.Count > 0) Last.Clear();  // a new subscriber starts from a fresh read, not an old one
            return;
        }
        var t = Items.V8;
        if (t == null || !Items.GearReadAvailable) return;
        try
        {
            if (Lifecycle.ServerSession != _session)
            {
                Reset();
                _session = Lifecycle.ServerSession;  // another server world: its entity ids are new
            }
            if (_players.Count == 0 || step >= _playersAt + PlayersEvery || step < _playersAt)
            {
                _players = Items.FindInventories(InventoryKinds.Players);
                _playersAt = step;
            }
            if (_players.Count == 0) return;
            var holder = _players[_cursor++ % _players.Count];
            Scan(t, holder);
        }
        catch (Exception e) when (e is KilnUnavailableException or NotSupportedException or InvalidOperationException)
        {
            // the world is not running (or kiln.items cannot read now): nothing to compare
        }
    }

    // One player's items, against the last read.
    private static void Scan(ItemsNative.TableV8* t, EntityId holder)
    {
        var inv = Items.Read(holder);
        var here = new HashSet<EntityId>();
        var changes = new List<(ItemInstanceRef Item, GearState Before, GearState After)>();
        if (inv != null)
            foreach (var slot in inv.Slots)
            {
                if (slot.IsEmpty || slot.ItemEntity.IsNone || slot.Ref.IsNone) continue;
                var item = slot.Ref;
                var id = item.Entity.Id;
                here.Add(id);
                lock (Gate) if (InFlight.ContainsKey(item)) continue;
                ItemsNative.GearState s = default;
                if (t->GearRead(id.Value, Items.StampOf(item, nameof(item)), &s) != ItemsNative.Ok) continue;
                var now = Items.StateOf(item, in s);
                lock (Gate)
                {
                    if (InFlight.ContainsKey(item)) continue;
                    if (Last.TryGetValue(id, out var seen) && seen.Item == item)
                    {
                        if (Moved(seen.State, now)) changes.Add((item, seen.State, now));
                        seen.State = now;
                        seen.Holder = holder;
                    }
                    else Last[id] = new Seen(item, holder, now);  // first sight: the baseline, no event
                }
            }
        lock (Gate)
            foreach (var gone in Last.Where(e => e.Value.Holder == holder && !here.Contains(e.Key) && !InFlight.ContainsKey(e.Value.Item)).Select(e => e.Key).ToList())
                Last.Remove(gone);  // moved away or destroyed: read afresh where it shows up again
        foreach (var (item, before, after) in changes) GearEvents.Changed(item, before, after, GearChangeSource.Game, "game");
    }

    /// <summary>The game's change the watch reports: slots, unlocked or level (wear is not a gear change).</summary>
    internal static bool Moved(GearState before, GearState after) =>
        before.Enhancements != after.Enhancements || before.Unlocked != after.Unlocked || before.Level != after.Level;
}
