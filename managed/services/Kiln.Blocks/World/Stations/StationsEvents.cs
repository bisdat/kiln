// SPDX-License-Identifier: MIT
// The station events: what changed (kiln.stations' watch: state, placed, removed, the bases), a finished cycle
// (Kiln.Gameplay's ItemCraftedEvent, once per cycle since 0.2.0-dev.29), a player's own station buttons (Kiln.Players'
// PlayerClientActionEvent of playerCraftingAction, typed), and the change event of Kiln's own actions.
using System.Buffers.Binary;

namespace Kiln;

/// <summary>A station's state, stop flag, selected or running recipe changed (seen in Pre, by kiln.stations' watch: the
/// game, a player or a mod). Posted on the tick thread while a handler listens. Since 0.2.0-dev.29.</summary>
[ServerOnly]
public sealed class StationStateChangedEvent : KilnEvent, IEntityEvent
{
    static StationStateChangedEvent() => StationsCore.EnsureProducers();

    internal StationStateChangedEvent(EntityId station, uint workshop, StationState before, StationState after, bool stoppedBefore,
        bool stoppedAfter, uint selectedBefore, uint selectedAfter, uint runningBefore, uint runningAfter, int baseIndex, string? byMod)
    {
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
    /// <summary>The mod whose Kiln.Stations call changed the station in the step before; null for the game, a player, or
    /// native code.</summary>
    public string? ByMod { get; }

    /// <summary>The stop flag went on.</summary>
    public bool Stopped => StoppedAfter && !StoppedBefore;
    /// <summary>The stop flag went off.</summary>
    public bool Resumed => StoppedBefore && !StoppedAfter;

    public bool Involves(EntityId entity) => entity == Station;

    public override string ToString() =>
        $"station {Station}: {Before} -> {After}{(Stopped ? " (stopped)" : Resumed ? " (resumed)" : "")} selected 0x{SelectedAfter:x8}{(ByMod is null ? "" : $" by {ByMod}")}";
}

/// <summary>A production station appeared in the world (placed, or found by the watch's first scans). Since 0.2.0-dev.29.</summary>
[ServerOnly]
public sealed class StationPlacedEvent : KilnEvent, IEntityEvent
{
    static StationPlacedEvent() => StationsCore.EnsureProducers();

    internal StationPlacedEvent(EntityId station, uint workshop, WorldPosition position, int baseIndex)
    {
        Station = station;
        Workshop = workshop;
        Position = position;
        BaseIndex = baseIndex;
    }

    public EntityId Station { get; }
    public uint Workshop { get; }
    public WorldPosition Position { get; }
    public int BaseIndex { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station {Station} placed (workshop 0x{Workshop:x8}, base #{BaseIndex})";
}

/// <summary>A production station is gone (picked up, destroyed). Since 0.2.0-dev.29.</summary>
[ServerOnly]
public sealed class StationRemovedEvent : KilnEvent, IEntityEvent
{
    static StationRemovedEvent() => StationsCore.EnsureProducers();

    internal StationRemovedEvent(EntityId station, uint workshop, WorldPosition position, int baseIndex)
    {
        Station = station;
        Workshop = workshop;
        Position = position;
        BaseIndex = baseIndex;
    }

    public EntityId Station { get; }
    public uint Workshop { get; }
    public WorldPosition Position { get; }
    public int BaseIndex { get; }
    public bool Involves(EntityId entity) => entity == Station;
    public override string ToString() => $"station {Station} removed (workshop 0x{Workshop:x8}, base #{BaseIndex})";
}

/// <summary>The game rebuilt its bases (a flame altar placed, removed or upgraded): base indices and ids may have
/// changed. Read <see cref="Bases.All"/> again. Since 0.2.0-dev.29.</summary>
[ServerOnly]
public sealed class BasesChangedEvent : KilnEvent
{
    static BasesChangedEvent() => StationsCore.EnsureProducers();

    internal BasesChangedEvent(int count) => Count = count;

    /// <summary>The number of bases now.</summary>
    public int Count { get; }
    public override string ToString() => $"bases changed: {Count}";
}

/// <summary>A production station finished a cycle: its outputs are in its inventory (Kiln.Gameplay's
/// <c>ItemCraftedEvent</c> of an automated station, once per cycle). Since 0.2.0-dev.29.</summary>
[ServerOnly]
public sealed class StationCraftedEvent : KilnEvent, IEntityEvent
{
    static StationCraftedEvent() => StationsCore.EnsureProducers();

    internal StationCraftedEvent(EntityId station, uint recipe, uint workshop, IReadOnlyList<StationStack> outputs)
    {
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
/// A player pressed a station button (fill, select, stop, use-all, water) or crafted at a bench: the game's
/// <c>playerCraftingAction</c> as Kiln.Players' <see cref="PlayerClientActionEvent"/> holds it, typed. Posted on the tick
/// thread in Pre, before the game runs it (while <see cref="CanCancel"/>): cancel it and the game never runs it (the
/// client is not told). One verdict with the generic event of the same input. Since 0.2.0-dev.29.
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
/// Since 0.2.0-dev.29.</summary>
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
            if (watch) _seq = Backend.LastSeq();  // start at the newest: events from before the watch are not this handler's
        }
        if (_watching) PollEvents();
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
        switch (e.Kind)
        {
            case StationsNative.EvState:
                string? by = null;
                if (e.By != 0)
                    lock (Gate)
                        by = Owners.GetValueOrDefault(e.By);
                Events.Post(new StationStateChangedEvent(station, e.Workshop, (StationState)e.StateBefore, (StationState)e.StateAfter,
                    e.StoppedBefore != 0, e.StoppedAfter != 0, e.SelectedBefore, e.SelectedAfter, e.RunningBefore, e.RunningAfter, e.BaseIndex, by));
                break;
            case StationsNative.EvPlaced:
                Events.Post(new StationPlacedEvent(station, e.Workshop, pos, e.BaseIndex));
                break;
            case StationsNative.EvRemoved:
                Events.Post(new StationRemovedEvent(station, e.Workshop, pos, e.BaseIndex));
                break;
            case StationsNative.EvBases:
                Events.Post(new BasesChangedEvent(e.BaseIndex));
                break;
        }
    }

    // An automated station's cycle: operator == station, a success (Kiln.Gameplay reports it once, at the cycle's end).
    internal static void OnCrafted(ItemCraftedEvent e)
    {
        var ev = CraftedOf(e.Operator, e.Station, e.Succeeded, e.RecipeId, e.WorkshopId, e.Outputs.Select(o => (o.ItemId, o.Count)));
        if (ev is not null) Events.Post(ev);
    }

    internal static StationCraftedEvent? CraftedOf(EntityId op, EntityId station, bool succeeded, uint recipe, uint workshop,
        IEnumerable<(uint Item, int Count)> outputs) =>
        !succeeded || station.IsNone || op != station || op.IsPlayer
            ? null
            : new StationCraftedEvent(station, recipe, workshop, outputs.Select(o => new StationStack(new ItemId(o.Item), o.Count)).ToArray());

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
    }
}
