// SPDX-License-Identifier: MIT
// The station events: what changed (kiln.stations' watch: state, placed, removed, the bases), a finished cycle
// (Kiln.Gameplay's ItemCraftedEvent, once per cycle since 0.2.0-dev.29.14), a player's own station buttons (Kiln.Players'
// PlayerClientActionEvent of playerCraftingAction, typed), and the change event of Kiln's own actions.
using System.Buffers.Binary;

namespace Kiln;

/// <summary>A station's state, stop flag, selected or running recipe changed (seen in Pre, by kiln.stations' watch: the
/// game, a player or a mod). Posted on the tick thread while a handler listens. Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class StationStateChangedEvent : KilnEvent, IEntityEvent
{
    static StationStateChangedEvent() => StationsCore.EnsureProducers();

    internal StationStateChangedEvent(EntityId station, uint workshop, StationState before, StationState after, bool stoppedBefore,
        bool stoppedAfter, uint selectedBefore, uint selectedAfter, uint runningBefore, uint runningAfter, int baseIndex, string? byMod,
        BaseId @base = default, EntityId? player = null)
    {
        Base = @base;
        Player = player;
        Station = station;
        Workshop = workshop;
        Before = before;
        After = after;
        StoppedBefore = stoppedBefore;
        StoppedAfter = stoppedAfter;
        SelectedBefore = selectedBefore;
        SelectedAfter = selectedAfter;
        RunningBefore = runningBefore;
        RunningAfter = runningAfter;
        BaseIndex = baseIndex;
        ByMod = byMod;
    }

    public EntityId Station { get; }
    public uint Workshop { get; }
    public StationState Before { get; }
    public StationState After { get; }
    public bool StoppedBefore { get; }
    public bool StoppedAfter { get; }
    public uint SelectedBefore { get; }
    public uint SelectedAfter { get; }
    public uint RunningBefore { get; }
    public uint RunningAfter { get; }
    /// <summary>The base's record index this session; -1 when the station stands in none.</summary>
    public int BaseIndex { get; }
    /// <summary>The base the station stands in; <see cref="BaseId.None"/> when none.</summary>
    public BaseId Base { get; }
    /// <summary>The mod whose Kiln.Stations stop, resume or select made exactly this change (the stop flag or the selection)
    /// in the step before; null for the game, a player, native code, and for what the game did afterwards (the state a
    /// stop or a fill leads to). For "did my own call do this", <see cref="StationChangedEvent"/> is the record of the call.</summary>
    public string? ByMod { get; }
    /// <summary>The player whose station button made this change, while Kiln holds the players' station buttons (a handler
    /// of <see cref="StationPlayerActionEvent"/> listens); null otherwise.</summary>
    public EntityId? Player { get; }

    /// <summary>The stop flag went on.</summary>
    public bool Stopped => StoppedAfter && !StoppedBefore;
    /// <summary>The stop flag went off.</summary>
    public bool Resumed => StoppedBefore && !StoppedAfter;

    public bool Involves(EntityId entity) => entity == Station;

    public override string ToString() =>
        $"station {Station}: {Before} -> {After}{(Stopped ? " (stopped)" : Resumed ? " (resumed)" : "")} selected 0x{SelectedAfter:x8}{(ByMod is null ? "" : $" by {ByMod}")}{(Player is { } p ? $" by player {p}" : "")}";
}

/// <summary>A production station appeared in the world (placed, or found by the watch's first scans). Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class StationPlacedEvent : KilnEvent, IEntityEvent
{
    static StationPlacedEvent() => StationsCore.EnsureProducers();

    internal StationPlacedEvent(EntityId station, uint workshop, WorldPosition position, int baseIndex, BaseId @base = default)
    {
        Base = @base;
        Station = station;
        Workshop = workshop;
        Position = position;
        BaseIndex = baseIndex;
    }

    public EntityId Station { get; }
    public uint Workshop { get; }
    public WorldPosition Position { get; }
    public int BaseIndex { get; }
    /// <summary>The base it stands in; <see cref="BaseId.None"/> when none.</summary>
    public BaseId Base { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station {Station} placed (workshop 0x{Workshop:x8}, base #{BaseIndex})";
}

/// <summary>A production station is gone (picked up, destroyed). Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class StationRemovedEvent : KilnEvent, IEntityEvent
{
    static StationRemovedEvent() => StationsCore.EnsureProducers();

    internal StationRemovedEvent(EntityId station, uint workshop, WorldPosition position, int baseIndex, BaseId @base = default)
    {
        Base = @base;
        Station = station;
        Workshop = workshop;
        Position = position;
        BaseIndex = baseIndex;
    }

    public EntityId Station { get; }
    public uint Workshop { get; }
    public WorldPosition Position { get; }
    public int BaseIndex { get; }
    /// <summary>The base it stood in; <see cref="BaseId.None"/> when none.</summary>
    public BaseId Base { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station {Station} removed (workshop 0x{Workshop:x8}, base #{BaseIndex})";
}

/// <summary>The game rebuilt its bases (a flame altar placed, removed or upgraded): base indices may have changed, and a
/// base's id changes when an overlapping altar comes or goes. <see cref="Changed"/> pairs old and new ids that share an
/// altar, so a mod can carry its per-base data over; <see cref="Kept"/> lists the ids that are there before and after. A
/// base that split keeps its id for one part (<see cref="Kept"/>) and shows the other as changed from it; a rename has
/// no <see cref="Kept"/> entry. A base whose altars Kiln did not find (id <see cref="BaseId.None"/>) is in none of the
/// lists. The first rebuild a watch sees after a world starts is its baseline and posts nothing. Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class BasesChangedEvent : KilnEvent
{
    static BasesChangedEvent() => StationsCore.EnsureProducers();

    internal BasesChangedEvent(int count, IReadOnlyList<BaseId> added, IReadOnlyList<BaseId> removed, IReadOnlyList<BaseIdChange> changed,
        IReadOnlyList<BaseId> kept)
    {
        Count = count;
        Kept = kept;
        Added = added;
        Removed = removed;
        Changed = changed;
    }

    /// <summary>The number of bases now.</summary>
    public int Count { get; }
    /// <summary>Bases that share no altar with a base before.</summary>
    public IReadOnlyList<BaseId> Added { get; }
    /// <summary>Bases before that share no altar with a base now.</summary>
    public IReadOnlyList<BaseId> Removed { get; }
    /// <summary>Bases whose id changed: old and new share at least one altar (two bases that merged both map to the new
    /// one; a base that split maps to each part).</summary>
    public IReadOnlyList<BaseIdChange> Changed { get; }
    /// <summary>Bases whose id is the same before and after (their altars may have changed).</summary>
    public IReadOnlyList<BaseId> Kept { get; }
    public override string ToString() => $"bases changed: {Count} (+{Added.Count} -{Removed.Count} ~{Changed.Count} ={Kept.Count})";
}

/// <summary>A production station finished a cycle: its outputs are in its inventory (Kiln.Gameplay's
/// <c>ItemCraftedEvent</c> of an automated station, once per cycle). Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class StationCraftedEvent : KilnEvent, IEntityEvent
{
    static StationCraftedEvent() => StationsCore.EnsureProducers();

    internal StationCraftedEvent(EntityId station, uint recipe, uint workshop, IReadOnlyList<StationStack> outputs, BaseId @base = default)
    {
        Base = @base;
        Station = station;
        Recipe = recipe;
        Workshop = workshop;
        Outputs = outputs;
    }

    public EntityId Station { get; }
    public uint Recipe { get; }
    public uint Workshop { get; }
    /// <summary>What the cycle made.</summary>
    public IReadOnlyList<StationStack> Outputs { get; }
    /// <summary>The base the station stands in; <see cref="BaseId.None"/> when none.</summary>
    public BaseId Base { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station {Station} crafted recipe 0x{Recipe:x8}: {string.Join(", ", Outputs)}";
}

/// <summary>What a player pressed at a station (the game's <c>CraftingActionType</c>).</summary>
public enum StationPlayerAction : byte
{
    /// <summary>A craft at a bench (not a production station's action).</summary>
    Craft = 0,
    /// <summary>"Fill from chests".</summary>
    Fill = 1,
    /// <summary>Selected a recipe (also the button that starts a stopped station).</summary>
    Select = 2,
    /// <summary>"Use all ingredient options" switched.</summary>
    UseAllOptions = 3,
    /// <summary>The stop button.</summary>
    Stop = 4,
    /// <summary>Water poured into the station.</summary>
    Water = 5,
}

/// <summary>
/// A player pressed a station button (fill, select, stop, use-all, water) or crafted at a bench (<see cref="Action"/>
/// <see cref="StationPlayerAction.Craft"/>: then <see cref="Station"/> is a workbench, not a production station): the game's
/// <c>playerCraftingAction</c> as Kiln.Players' <see cref="PlayerClientActionEvent"/> holds it, typed. Posted on the tick
/// thread in Pre, before the game runs it (while <see cref="CanCancel"/>): cancel it and the game never runs it (the
/// client is not told). One verdict with the generic event of the same input. Since 0.2.0-dev.29.14.
/// </summary>
[ServerOnly]
public sealed class StationPlayerActionEvent : CancelableEvent, IEntityEvent
{
    static StationPlayerActionEvent() => StationsCore.EnsureProducers();

    internal StationPlayerActionEvent(EntityId player, EntityId station, StationPlayerAction action, uint recipe, int amount,
        IReadOnlyList<CategoryChoice> choices, bool useAllOptions, bool canCancel)
    {
        Player = player;
        Station = station;
        Action = action;
        Recipe = recipe;
        Amount = amount;
        Choices = choices;
        UseAllOptions = useAllOptions;
        CanCancel = canCancel;
    }

    public EntityId Player { get; }
    public EntityId Station { get; }
    public StationPlayerAction Action { get; }
    /// <summary>Select, Fill, Craft: the recipe.</summary>
    public uint Recipe { get; }
    /// <summary>Fill, Craft: how many crafts.</summary>
    public int Amount { get; }
    public IReadOnlyList<CategoryChoice> Choices { get; }
    /// <summary>UseAllOptions: the new value.</summary>
    public bool UseAllOptions { get; }
    /// <summary>True: the game has not run it and a cancel stops it.</summary>
    public bool CanCancel { get; }
    public bool Involves(EntityId entity) => entity == Player || entity == Station;
    public override string ToString() => $"player {Player} at station {Station}: {Action} recipe 0x{Recipe:x8} x{Amount}";
}

/// <summary>A Kiln.Stations call changed a station (the contract's change event of every write): a fill that moved
/// something, a stop, a resume, a selection, a collect. Not posted for a dry run or an action that changed nothing.
/// Since 0.2.0-dev.29.14.</summary>
[ServerOnly]
public sealed class StationChangedEvent : KilnEvent, IEntityEvent
{
    internal StationChangedEvent(StationResult result, string? byMod)
    {
        Result = result;
        ByMod = byMod;
    }

    /// <summary>What the call did.</summary>
    public StationResult Result { get; }
    public EntityId Station => Result.Station;
    public StationAction Action => Result.Action;
    /// <summary>The mod that called; null for Kiln itself.</summary>
    public string? ByMod { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station changed: {Result}{(ByMod is null ? "" : $" by {ByMod}")}";
}

internal static partial class StationsCore
{
    private static readonly ChangeNotices<StationChangedEvent, uint> Notices = new("kiln.stations");
    private static readonly Dictionary<ulong, string> Owners = new();  // native owner -> mod id, of the mods that sent actions
    private static int _producers;
    private static IDisposable? _tick;
    private static bool _watching;
    private static ulong _seq;
    private static EventSubscription? _crafted, _actions;
    private static List<(BaseId Id, HashSet<Guid> Altars)>? _bases;          // the bases at the last BASES event (or the watch's start)
    private static readonly Dictionary<uint, (EntityId Player, StationPlayerAction Action, ulong Step)> Pressed = new();  // station -> a player's last stop / select

    internal static void Changed(StationResult result, ModContext? mod)
    {
        var id = mod?.Info.Id;
        Notices.Post(result.Station.Value, new StationChangedEvent(result, id), result.Station);
    }

    internal static void EnsureProducers()
    {
        if (Interlocked.Exchange(ref _producers, 1) != 0) return;
        foreach (var t in new[] { typeof(StationStateChangedEvent), typeof(StationPlacedEvent), typeof(StationRemovedEvent), typeof(BasesChangedEvent),
                     typeof(StationCraftedEvent), typeof(StationPlayerActionEvent) })
            EventProducers.Register(t, Stations.EventsCapability, WhyEventsOff);
        StartPump();
    }

    internal static string? WhyEventsOff() => Backend.Status() switch
    {
        null => "kiln.stations is not here (an older kiln-items.dll, or no server world in this process)",
        { Events: false } s => s.Detail.Length > 0 ? s.Detail : "kiln.stations reports its events off",
        _ => null,
    };

    internal static void StartPump()
    {
        lock (Gate) _tick ??= ServiceTick.Add("kiln.stations", Pump);
    }

    private static void ResetPump()
    {
        _watching = false;
        _seq = 0;
        _crafted?.Dispose();
        _crafted = null;
        _actions?.Dispose();
        _actions = null;
        _bases = null;
        lock (Gate) Pressed.Clear();
        lock (Gate) Owners.Clear();
    }

    internal static void Pump(in TickInfo tick) => PumpOnce();

    internal static void PumpOnce()
    {
        var watch = Events.HasSubscribers<StationStateChangedEvent>() || Events.HasSubscribers<StationPlacedEvent>() ||
                    Events.HasSubscribers<StationRemovedEvent>() || Events.HasSubscribers<BasesChangedEvent>();
        if (watch != _watching)
        {
            if (Backend.Watch(watch) == StationsNative.Ok || !watch) _watching = watch;
            if (watch)
            {
                _seq = Backend.LastSeq();  // start at the newest: events from before the watch are not this handler's
                _bases = SnapshotBases();
            }
        }
        if (_watching)
        {
            _bases ??= SnapshotBases();  // a watch started before the world: its baseline once the bases can be read
            PollEvents();
        }
        Follow(ref _crafted, Events.HasSubscribers<StationCraftedEvent>(), () => Events.On<ItemCraftedEvent>(OnCrafted, EventPriority.Monitor));
        Follow(ref _actions, Events.HasSubscribers<StationPlayerActionEvent>(), () => Events.On<PlayerClientActionEvent>(OnClientAction));
    }

    private static void Follow(ref EventSubscription? sub, bool want, Func<EventSubscription> make)
    {
        if (want && sub is null)
        {
            try { sub = make(); }
            catch (KilnUnavailableException) { }  // the producer is off here: nothing to follow
        }
        else if (!want && sub is not null)
        {
            sub.Dispose();
            sub = null;
        }
    }

    private static void PollEvents()
    {
        Span<StationsNative.Event> buf = stackalloc StationsNative.Event[32];
        for (var round = 0; round < 64; round++)
        {
            var rc = Backend.Events(_seq, buf, out var n);
            if (rc != StationsNative.Ok && rc != StationsNative.BufferTooSmall) return;
            for (var i = 0; i < (int)n; i++)
            {
                _seq = buf[i].Seq;
                Post(buf[i]);
            }
            if (rc != StationsNative.BufferTooSmall || n == 0) return;
        }
    }

    internal static void Post(in StationsNative.Event e)
    {
        var station = new EntityId(e.Station);
        var pos = new WorldPosition(e.Position.X, e.Position.Y, e.Position.Z);
        var baseId = new BaseId(StationsNative.GuidOf(e.Base));
        switch (e.Kind)
        {
            case StationsNative.EvState:
                string? by = null;
                EntityId? player = null;
                lock (Gate)
                {
                    if (e.By != 0) by = Owners.GetValueOrDefault(e.By);
                    else if (Pressed.TryGetValue(e.Station, out var p))
                    {
                        if (ServiceTick.Step > p.Step + 4) Pressed.Remove(e.Station);  // that press changed nothing
                        else if (PressMatches(p.Action, e))
                        {
                            player = p.Player;
                            Pressed.Remove(e.Station);
                        }
                    }
                }
                Events.Post(new StationStateChangedEvent(station, e.Workshop, (StationState)e.StateBefore, (StationState)e.StateAfter,
                    e.StoppedBefore != 0, e.StoppedAfter != 0, e.SelectedBefore, e.SelectedAfter, e.RunningBefore, e.RunningAfter, e.BaseIndex, by,
                    baseId, player));
                break;
            case StationsNative.EvPlaced:
                Events.Post(new StationPlacedEvent(station, e.Workshop, pos, e.BaseIndex, baseId));
                break;
            case StationsNative.EvRemoved:
                Events.Post(new StationRemovedEvent(station, e.Workshop, pos, e.BaseIndex, baseId));
                break;
            case StationsNative.EvBases:
                var now = SnapshotBases();
                if (_bases is null || now is null)
                {
                    _bases = now;  // the baseline (a watch from before the world, or bases that cannot be read): nothing to diff
                    break;
                }
                var (added, removed, changed, kept) = DiffBases(_bases, now);
                _bases = now;
                Events.Post(new BasesChangedEvent(e.BaseIndex, added, removed, changed, kept));
                break;
        }
    }

    private static List<(BaseId Id, HashSet<Guid> Altars)>? SnapshotBases()
    {
        try
        {
            var b = Backend;
            if (b.BaseCount(out var n) != StationsNative.Ok) return null;
            var list = new List<(BaseId, HashSet<Guid>)>();
            for (uint i = 0; i < n; i++)
            {
                if (b.BaseAt(i, out var nb) != StationsNative.Ok) continue;
                var altars = new HashSet<Guid>();
                for (var k = 0; k < Math.Min(nb.AltarCount, (uint)StationsNative.MaxAltars); k++) altars.Add(StationsNative.AltarIdOf(nb, k));
                list.Add((new BaseId(StationsNative.GuidOf(nb.Id)), altars));
            }
            return list;
        }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException)
        {
            return null;
        }
    }

    /// <summary>Old and new bases by their altars: a pair sharing an altar is the same base (changed when the ids differ,
    /// kept when they are equal). Bases without a known altar (id none) are left out.</summary>
    internal static (IReadOnlyList<BaseId> Added, IReadOnlyList<BaseId> Removed, IReadOnlyList<BaseIdChange> Changed, IReadOnlyList<BaseId> Kept)
        DiffBases(IReadOnlyList<(BaseId Id, HashSet<Guid> Altars)> before, IReadOnlyList<(BaseId Id, HashSet<Guid> Altars)> after)
    {
        var added = new List<BaseId>();
        var removed = new List<BaseId>();
        var changed = new List<BaseIdChange>();
        var kept = new List<BaseId>();
        before = before.Where(b => !b.Id.IsNone && b.Altars.Count > 0).ToArray();
        after = after.Where(b => !b.Id.IsNone && b.Altars.Count > 0).ToArray();
        foreach (var a in after)
        {
            var olds = before.Where(o => o.Altars.Overlaps(a.Altars)).ToArray();
            if (olds.Length == 0) added.Add(a.Id);
            foreach (var o in olds)
                if (o.Id != a.Id) changed.Add(new BaseIdChange(o.Id, a.Id));
                else kept.Add(a.Id);
        }
        foreach (var o in before)
            if (!after.Any(a => a.Altars.Overlaps(o.Altars))) removed.Add(o.Id);
        return (added, removed, changed, kept);
    }

    /// <summary>Whether a state change is what a player's press leads to: a stop sets the stop flag, a select changes the
    /// selection or clears the stop.</summary>
    internal static bool PressMatches(StationPlayerAction action, in StationsNative.Event e) => action switch
    {
        StationPlayerAction.Stop => e.StoppedAfter != 0 && e.StoppedBefore == 0,
        StationPlayerAction.Select => e.SelectedAfter != e.SelectedBefore || (e.StoppedBefore != 0 && e.StoppedAfter == 0),
        _ => false,
    };

    // An automated station's cycle: operator == station, a success (Kiln.Gameplay reports it once, at the cycle's end).
    internal static void OnCrafted(ItemCraftedEvent e)
    {
        var ev = CraftedOf(e.Operator, e.Station, e.Succeeded, e.RecipeId, e.WorkshopId, e.Outputs.Select(o => (o.ItemId, o.Count)), BaseOf(e.Station));
        if (ev is not null) Events.Post(ev);
    }

    private static BaseId BaseOf(EntityId station)
    {
        try { return Backend.BaseOf(station.Value, out var b) == StationsNative.Ok ? new BaseId(StationsNative.GuidOf(b.Id)) : default; }
        catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException) { return default; }
    }

    internal static StationCraftedEvent? CraftedOf(EntityId op, EntityId station, bool succeeded, uint recipe, uint workshop,
        IEnumerable<(uint Item, int Count)> outputs, BaseId @base = default) =>
        !succeeded || station.IsNone || op != station || op.IsPlayer
            ? null
            : new StationCraftedEvent(station, recipe, workshop, outputs.Select(o => new StationStack(new ItemId(o.Item), o.Count)).ToArray(), @base);

    // PlayerCraftingAction (68 bytes, the game's reflection): craftingStationId +0, recipeId +4, itemId +8, amount u16 +0x0c,
    // waterAmount +0x10, waterSourceId +0x14, inputCategorySelections +0x18 (4 x 8), useAll +0x38, version +0x3c, type +0x40.
    internal const string CraftingActionName = "playerCraftingAction";

    internal static StationPlayerActionEvent? Decode(EntityId player, ReadOnlySpan<byte> p, bool canCancel)
    {
        if (p.Length < 0x41) return null;
        var type = p[0x40];
        if (type > (byte)StationPlayerAction.Water) return null;
        var choices = new List<CategoryChoice>();
        for (var i = 0; i < 4; i++)
        {
            var cat = BinaryPrimitives.ReadUInt32LittleEndian(p[(0x18 + 8 * i)..]);
            if (cat != 0) choices.Add(new CategoryChoice(cat, p[0x18 + 8 * i + 4]));
        }
        return new StationPlayerActionEvent(player, new EntityId(BinaryPrimitives.ReadUInt32LittleEndian(p)), (StationPlayerAction)type,
            BinaryPrimitives.ReadUInt32LittleEndian(p[4..]), BinaryPrimitives.ReadUInt16LittleEndian(p[0x0c..]), choices, p[0x38] != 0, canCancel);
    }

    internal static void OnClientAction(PlayerClientActionEvent e)
    {
        if (!string.Equals(e.Name, CraftingActionName, StringComparison.Ordinal)) return;
        var ev = Decode(e.Player.Entity, e.Payload, e.CanCancel);
        if (ev is null) return;
        Events.Post(ev);
        if (ev.Canceled && e.CanCancel) e.Cancel();
        else Press(ev.Station, ev.Player, ev.Action);
    }

    /// <summary>A player's station button that ran: a stop or a select names the player on the state event it leads to.</summary>
    internal static void Press(EntityId station, EntityId player, StationPlayerAction action)
    {
        if (action is StationPlayerAction.Select or StationPlayerAction.Stop)
            lock (Gate) Pressed[station.Value] = (player, action, ServiceTick.Step);
    }
}
