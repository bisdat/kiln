// SPDX-License-Identifier: MIT
// The public shapes of kiln.stations' managed side (Kiln.Blocks: Stations, Bases): bases, stations, recipes as a station
// runs them, fill options and plans, results. StationState and StationNeeds are World/StationRuntime.cs' (shipped in
// dev.28). docs/kiln/services/stations.md.
namespace Kiln;

/// <summary>A base's id: the GUID the game gives a flame altar (its <c>BaseIdComponent</c>, kept in the save). A base
/// made of several overlapping altar zones takes the smallest of its altars' ids (the 16 bytes compared in memory
/// order), so the id changes when an overlapping altar is added or removed. Since 0.2.0-dev.29.14.</summary>
/// <param name="Value">The GUID.</param>
public readonly record struct BaseId(Guid Value)
{
    /// <summary>No base.</summary>
    public static BaseId None => default;

    /// <summary>True for <see cref="None"/>.</summary>
    public bool IsNone => Value == Guid.Empty;

    /// <summary>The GUID in its usual form.</summary>
    public override string ToString() => IsNone ? "none" : Value.ToString();
}

/// <summary>One of the game's bases as its crafting code sees it: the union of overlapping flame-altar zones, with the
/// crafting-stock containers the game lists for it (the Magic storage chests and the player-planted refilling trees:
/// ordinary chests are never in it) and its water sources (wells and barrels holding water). Since 0.2.0-dev.29.14.</summary>
/// <param name="Id">The base's id (see <see cref="BaseId"/>).</param>
/// <param name="Index">The game's record index this session (0..15): it changes when the bases are rebuilt.</param>
/// <param name="Altars">The flame altars whose zones make the base (more than one when their zones overlap), each with its
/// own id: keep per-base data by these (one of them stays when an overlapping altar comes or goes).</param>
/// <param name="Min">The corner of the base's box with the smallest coordinates.</param>
/// <param name="Max">The opposite corner.</param>
/// <param name="StorageCount">Crafting-stock containers the game lists for the base now.</param>
/// <param name="WaterCount">Water sources holding water.</param>
/// <param name="ZoneCount">Altar zone boxes merged into the base.</param>
/// <param name="StationCount">Production stations standing in the base (as of the last scan).</param>
public sealed record BaseInfo(BaseId Id, int Index, IReadOnlyList<BaseAltar> Altars, WorldPosition Min, WorldPosition Max,
    int StorageCount, int WaterCount, int ZoneCount, int StationCount)
{
    /// <summary>The name a player gave the base's (first) flame altar; null when none. Read when asked (one read of the
    /// altar), on the tick thread.</summary>
    public string? Name
    {
        get
        {
            if (Altars.Count == 0) return null;
            try { return Items.ContainerName(Altars[0].Entity); }
            catch (Exception e) when (e is KilnUnavailableException or InvalidOperationException or NotSupportedException) { return null; }
        }
    }

    /// <summary>Whether <paramref name="p"/> lies in the base's box (the game also checks the altar zones inside it:
    /// <see cref="Bases.At"/> answers exactly).</summary>
    public bool InBox(WorldPosition p) => p.X >= Min.X && p.Y >= Min.Y && p.Z >= Min.Z && p.X < Max.X && p.Y < Max.Y && p.Z < Max.Z;

    /// <summary>The base in one line, for a log.</summary>
    public override string ToString() =>
        $"base {Id} (#{Index}): {Altars.Count} altar(s), {StorageCount} storage, {WaterCount} water, {StationCount} station(s)";
}

/// <summary>One flame altar of a base: its entity (this session) and its own id (the GUID the game keeps in the save with
/// the altar; it never changes). Since 0.2.0-dev.29.14.</summary>
public readonly record struct BaseAltar(EntityId Entity, BaseId Id);

/// <summary>A base whose id changed when the game rebuilt its bases: an overlapping altar came or went
/// (<see cref="BasesChangedEvent.Changed"/>). Old and new share at least one altar.</summary>
public readonly record struct BaseIdChange(BaseId Old, BaseId New);

/// <summary>For a recipe input "any item of category X", the option chosen (the game's
/// <c>RecipeInputCategorySelection</c>).</summary>
/// <param name="Category">The ingredient category's id.</param>
/// <param name="Index">The option's index in the category (<see cref="IngredientCategory.Options"/>).</param>
public readonly record struct CategoryChoice(uint Category, int Index);

/// <summary>One option of an ingredient category: an item, and how many of it count as one (the game's scaling: a craft
/// needs <c>ceil(count x Scaling)</c> of it).</summary>
public readonly record struct IngredientOption(ItemId Item, float Scaling);

/// <summary>An ingredient category of the game's recipes ("any wood"): its options in the game's order.</summary>
public sealed record IngredientCategory(uint Id, IReadOnlyList<IngredientOption> Options);

/// <summary>An amount of one item.</summary>
public readonly record struct StationStack(ItemId Item, int Count)
{
    public override string ToString() => $"{Count} x {Item}";
}

/// <summary>A recipe input as the data has it: an item (<see cref="Item"/>), or any item of a category
/// (<see cref="Category"/>, <see cref="CategoryCount"/>).</summary>
public readonly record struct StationInput(ItemId Item, int Count, uint Category, int CategoryCount)
{
    /// <summary>True for "any item of a category".</summary>
    public bool IsCategory => Category != 0;
}

/// <summary>A recipe as a station runs it. Since 0.2.0-dev.29.14. <see cref="Name"/> is the game's debug name (Kiln.Content's
/// recipe entry), not a text for players: show the outputs' item names instead.</summary>
/// <param name="Id">The recipe id.</param>
/// <param name="Workshop">The workshop it belongs to (a station runs the recipes of its <see cref="StationInfo.Workshop"/>).</param>
/// <param name="Inputs">Its inputs as the data has them (per craft).</param>
/// <param name="Outputs">What one craft makes.</param>
/// <param name="Duration">One cycle, not scaled by the server's factory speed.</param>
/// <param name="WaterCharges">Water one craft uses (charges; 0: none).</param>
/// <param name="AlwaysUnlocked">True when the recipe needs no knowledge (no unlock).</param>
public sealed record StationRecipe(uint Id, uint Workshop, IReadOnlyList<StationInput> Inputs, IReadOnlyList<StationStack> Outputs,
    TimeSpan Duration, int WaterCharges, bool AlwaysUnlocked)
{
    /// <summary>The game's debug name of the recipe (Kiln.Content); null when the content registry does not list it.</summary>
    public string? Name { get; init; }

    /// <summary>A name for players (since 0.2.0-dev.29.15): its first output's (<see cref="Items.DisplayName"/>), else the
    /// recipe's own name text (<see cref="Stations.RecipeName"/>): in the game's text language on a hosting client, the English
    /// names Kiln has on a dedicated server. Null when neither is known (yet: the game's names are read in the background the
    /// first time).</summary>
    public string? DisplayName { get; init; }
}

/// <summary>A production station now. Since 0.2.0-dev.29.14.</summary>
/// <param name="Entity">The station.</param>
/// <param name="Workshop">Which recipes it runs (<c>Crafting.workshopId</c>).</param>
/// <param name="Position">Where it stands.</param>
/// <param name="Base">The base it stands in; <see cref="BaseId.None"/> when none.</param>
/// <param name="BaseIndex">That base's record index this session; -1 when none.</param>
/// <param name="State">The game's state.</param>
/// <param name="StoppedByPlayer">The stop button's flag (set by a player or by Kiln's stop).</param>
/// <param name="UseAllOptions">"Use all ingredient options" is on.</param>
/// <param name="Needs">What it lacks.</param>
/// <param name="RunningRecipe">The cycle in progress; 0 when none.</param>
/// <param name="SelectedRecipe">The recipe it repeats; 0 when none.</param>
/// <param name="SelectedChoices">The category choices of the selected recipe.</param>
/// <param name="CraftsLoaded">Complete crafts of the selected recipe its inputs hold now (as the game counts them).</param>
/// <param name="Duration">The cycle in progress' duration (its own recipe), else one cycle of the selected recipe; scaled by
/// the server's factory speed.</param>
/// <param name="Progress">0..1 of the cycle in progress (while paused: where it paused), 0 when none runs.</param>
/// <param name="Water">The station's water (units), null for a station without water.</param>
/// <param name="WaterMax">Its capacity (units), null for a station without water.</param>
/// <param name="Slots">Its inventory: inputs and outputs share it.</param>
/// <param name="Outputs">The slots holding an output of the selected or running recipe that is not also one of its
/// inputs (category options included): what <see cref="Stations.Collect"/> moves by default.</param>
/// <param name="Leftovers">The slots holding something that is neither an input nor an output of the selected or running
/// recipe (an old recipe's inputs or outputs): <see cref="CollectWhat.Leftovers"/> moves them.</param>
/// <param name="StopPending">A <see cref="StopWhen.AfterCurrentCraft"/> stop waits for the cycle's end (not saved: a restart
/// forgets it).</param>
/// <param name="Template">The station's template (the game's debug name, e.g. <c>T1_Charcoal_Kiln_Factory</c>); null when
/// Kiln.Spawn cannot read it.</param>
public sealed record StationInfo(EntityId Entity, uint Workshop, WorldPosition Position, BaseId Base, int BaseIndex,
    StationState State, bool StoppedByPlayer, bool UseAllOptions, StationNeeds Needs, uint RunningRecipe, uint SelectedRecipe,
    IReadOnlyList<CategoryChoice> SelectedChoices, int CraftsLoaded, TimeSpan Duration, double Progress, int? Water, int? WaterMax,
    IReadOnlyList<InventorySlot> Slots, IReadOnlyList<InventorySlot> Outputs, IReadOnlyList<InventorySlot> Leftovers, bool StopPending,
    string? Template)
{
    /// <summary>A cycle runs.</summary>
    public bool IsRunning => State == StationState.Running;

    /// <summary>A name for players (since 0.2.0-dev.29.15): the name of the item that places it ("Charcoal Kiln"), else its
    /// workshop's name text (<see cref="Stations.WorkshopName"/>): in the game's text language on a hosting client, the English
    /// names Kiln has on a dedicated server. Null when neither is known (yet).</summary>
    public string? DisplayName { get; init; }

    /// <summary>The station in one line, for a log.</summary>
    public override string ToString() =>
        $"station {Entity} workshop 0x{Workshop:x8} {State}{(StoppedByPlayer ? " (stopped)" : "")} recipe 0x{SelectedRecipe:x8} loaded {CraftsLoaded} progress {Progress:P0} base {Base}";
}

/// <summary>How <see cref="FillOptions.Crafts"/> counts.</summary>
public enum FillMode
{
    /// <summary>The station should hold this many complete crafts in all (the default).</summary>
    UpTo = 0,
    /// <summary>This many more complete crafts than it holds (the game's own button means this).</summary>
    Add = 1,
}

/// <summary>Options of <see cref="Stations.Fill"/>. Since 0.2.0-dev.29.14.</summary>
public sealed record FillOptions
{
    /// <summary>Crafts to fill for, 1..1000 (see <see cref="Mode"/>). Default 1.</summary>
    public int Crafts { get; init; } = 1;

    /// <summary><see cref="FillMode.UpTo"/> (default) or <see cref="FillMode.Add"/>.</summary>
    public FillMode Mode { get; init; }

    /// <summary>All of it or nothing; default false: as many crafts as the base's stock and the station's room cover.</summary>
    public bool Exact { get; init; }

    /// <summary>Run the game's function and roll back: the result says what would move.</summary>
    public bool DryRun { get; init; }

    /// <summary>Pour the recipe's water from the base's water sources (default true).</summary>
    public bool Water { get; init; } = true;

    /// <summary>The recipe to fill for; null: the station's selected recipe.</summary>
    public uint? Recipe { get; init; }

    /// <summary>The category choices for <see cref="Recipe"/>; null: the station's own.</summary>
    public IReadOnlyList<CategoryChoice>? Choices { get; init; }

    /// <summary>Also take from this player's inventories first, as the game's own button does. None (the default): never
    /// a player's bag, only the base's stock containers.</summary>
    public EntityId Player { get; init; }

    /// <summary>Reserves: at least this many of each item stay in the base's stock containers (up to 64 items).</summary>
    public IReadOnlyDictionary<ItemId, long>? Keep { get; init; }
}

/// <summary>What kept a fill below what was asked.</summary>
public enum FillLimit
{
    /// <summary>Nothing (or not a fill).</summary>
    None = 0,
    /// <summary>The base's stock of <see cref="StationResult.ShortItem"/>.</summary>
    Stock = 1,
    /// <summary>A reserve (<see cref="FillOptions.Keep"/>) of <see cref="StationResult.ShortItem"/>.</summary>
    Reserve = 2,
    /// <summary>The station's room.</summary>
    Room = 3,
    /// <summary>Water: the base's water sources, or the station's water capacity.</summary>
    Water = 4,
}

/// <summary>One input of a fill plan: what one craft needs, what the station holds, what the base's stock can give.</summary>
public readonly record struct FillInput(ItemId Item, int PerCraft, int Have, long Stock);

/// <summary>What a fill would do now (<see cref="Stations.Plan"/>). Since 0.2.0-dev.29.14.</summary>
/// <param name="Status">Done when <see cref="CraftsPlanned"/> crafts would go in; Partial when fewer than asked; otherwise
/// why none would.</param>
/// <param name="Station">The station.</param>
/// <param name="Recipe">The recipe.</param>
/// <param name="CraftsLoaded">Complete crafts the station holds now.</param>
/// <param name="CraftsAffordable">More crafts the base's stock covers (after the reserves).</param>
/// <param name="CraftsPlanned">More crafts a fill with these options would ask the game for.</param>
/// <param name="Inputs">The resolved inputs (category choices applied).</param>
/// <param name="Message">Why, when nothing (or less than asked) would go in.</param>
public sealed record FillPlan(StationActionStatus Status, EntityId Station, uint Recipe, int CraftsLoaded, int CraftsAffordable, int CraftsPlanned,
    IReadOnlyList<FillInput> Inputs, string Message) : IKilnResult
{
    /// <summary>What keeps the fill below what was asked.</summary>
    public FillLimit LimitedBy { get; init; }
    /// <summary>The item short (<see cref="FillLimit.Stock"/>, <see cref="FillLimit.Reserve"/>); null otherwise.</summary>
    public ItemId? ShortItem { get; init; }
    /// <summary>Water one craft needs (Fuel units, 64 a charge); 0 when the recipe needs none or the options pour none.</summary>
    public int WaterPerCraft { get; init; }
    /// <summary>Water in the station now (units).</summary>
    public int WaterHave { get; init; }
    /// <summary>Water in the base's water sources (units).</summary>
    public int WaterStock { get; init; }
    /// <summary>The station has "use all ingredient options" on: its cycles may consume other options of a category than
    /// the one a fill loads, so <see cref="CraftsLoaded"/> can drop faster than the plan says.</summary>
    public bool UseAllOptions { get; init; }

    public bool Ok => Status is StationActionStatus.Done or StationActionStatus.Partial;
    public string? Reason => Ok ? null : Message.Length > 0 ? Message : Status.ToString();
}

/// <summary>When <see cref="Stations.Stop"/> stops.</summary>
public enum StopWhen
{
    /// <summary>At once, as the game's stop button: the running cycle is aborted and the game puts its inputs back into
    /// the station (its progress is lost). The default.</summary>
    Now = 0,
    /// <summary>When the running cycle has ended (its outputs made; nothing is lost).</summary>
    AfterCurrentCraft = 1,
}

/// <summary>What a station action did.</summary>
public enum StationActionStatus
{
    /// <summary>It happened.</summary>
    Done = 0,
    /// <summary>Fill: fewer crafts than asked (the stock or the station's room).</summary>
    Partial = 1,
    /// <summary>Fill: the base's stock covers not even one craft.</summary>
    NotEnough = 2,
    /// <summary>Fill: no room in the station; Collect: no room in the target.</summary>
    NoRoom = 3,
    /// <summary>The entity is no production station.</summary>
    NotAStation = 4,
    /// <summary>The station stands in no base.</summary>
    NoBase = 5,
    /// <summary>No recipe selected, or an unknown recipe.</summary>
    NoRecipe = 6,
    /// <summary>The recipe is not one of this station's.</summary>
    WrongWorkshop = 7,
    /// <summary>Nothing to do (already stopped, running, selected, loaded; nothing to collect).</summary>
    Unchanged = 8,
    /// <summary>Select: no one on the server has unlocked the recipe.</summary>
    Locked = 9,
    /// <summary>The game refused it.</summary>
    Refused = 10,
    /// <summary>The capability went off after the call.</summary>
    Unavailable = 11,
    /// <summary>Not run: the world closed, or the program did not run it in 10 s.</summary>
    Dropped = 12,
    /// <summary>Stop after the current craft: the cycle did not end within about an hour of steps; nothing was written.</summary>
    TimedOut = 13,
    /// <summary>Not taken: 1024 actions are waiting. Try again in a step.</summary>
    QueueFull = 14,
    /// <summary>Stop after the current craft: withdrawn before it was written, by a later <see cref="Stations.Resume"/>,
    /// <see cref="Stations.Select"/> or <see cref="Stations.Stop"/> now on the station that passed its checks (that call's
    /// message says it withdrew it), or because the station's selected recipe changed (a player's or a mod's select);
    /// nothing was written. A stop meanwhile, a player's too, answers <see cref="Unchanged"/> instead.</summary>
    Canceled = 15,
}

/// <summary>What <see cref="Stations.Collect"/> moves.</summary>
[Flags]
public enum CollectWhat
{
    /// <summary>The finished items (<see cref="StationInfo.Outputs"/>). The default.</summary>
    Outputs = 1,
    /// <summary>What is neither an input nor an output of the station's recipe (<see cref="StationInfo.Leftovers"/>).</summary>
    Leftovers = 2,
    /// <summary>Both.</summary>
    All = Outputs | Leftovers,
}

/// <summary>Which station action.</summary>
public enum StationAction
{
    Fill = 1,
    Stop = 2,
    Resume = 3,
    Select = 4,
    Collect = 5,
}

/// <summary>What a station action did. <see cref="Ok"/> when it happened (Partial and Unchanged included); otherwise
/// <see cref="Reason"/> says why and nothing changed. Since 0.2.0-dev.29.14.</summary>
/// <param name="Status">The outcome.</param>
/// <param name="Action">The action.</param>
/// <param name="Station">The station.</param>
/// <param name="Recipe">Fill / Select: the recipe.</param>
/// <param name="CraftsRequested">Fill: more crafts asked for.</param>
/// <param name="CraftsBefore">Fill: complete crafts loaded before.</param>
/// <param name="CraftsAfter">Fill: complete crafts loaded after.</param>
/// <param name="CraftsAdded">Fill: the crafts the game's fill ran with.</param>
/// <param name="GameError">Fill: the game's code of its last attempt (2 no room in the station, 4 missing materials), 0.</param>
/// <param name="Changes">Fill: the ledger (a take from a chest: <c>From</c> set; a put into the station: <c>To</c> set);
/// Collect: the moves.</param>
/// <param name="StateBefore">The station's state before; null when nothing was read (a refusal before the station was
/// looked at).</param>
/// <param name="StateAfter">After; null as <paramref name="StateBefore"/>.</param>
/// <param name="DryRun">Fill: a dry run (nothing changed).</param>
/// <param name="Message">Why, or a note.</param>
public sealed record StationResult(StationActionStatus Status, StationAction Action, EntityId Station, uint Recipe, int CraftsRequested,
    int CraftsBefore, int CraftsAfter, int CraftsAdded, int GameError, IReadOnlyList<MoveChange> Changes, StationState? StateBefore,
    StationState? StateAfter, bool DryRun, string Message) : IKilnResult
{
    public bool Ok => Status is StationActionStatus.Done or StationActionStatus.Partial or StationActionStatus.Unchanged;
    public string? Reason => Ok ? null : Message.Length > 0 ? Message : Status.ToString();

    /// <summary>Fill (not Done): what kept it below what was asked.</summary>
    public FillLimit LimitedBy { get; init; }
    /// <summary>Fill limited by <see cref="FillLimit.Stock"/> or <see cref="FillLimit.Reserve"/>: the item; null otherwise.</summary>
    public ItemId? ShortItem { get; init; }

    /// <summary>The same call may succeed later as it is: <see cref="StationActionStatus.QueueFull"/>,
    /// <see cref="StationActionStatus.Dropped"/>, and the stock or room shortages of a fill (they change as players and
    /// stations move items).</summary>
    public bool CanRetry => Status is StationActionStatus.QueueFull or StationActionStatus.Dropped or StationActionStatus.NotEnough
        or StationActionStatus.NoRoom or StationActionStatus.Partial;

    /// <summary>Something changed (or would, in a dry run).</summary>
    public bool Changed => Status is StationActionStatus.Done or StationActionStatus.Partial;

    /// <summary>The outcome in one line, for a log.</summary>
    public override string ToString() =>
        $"{Action} {Status} station {Station}" + (Action == StationAction.Fill ? $" recipe 0x{Recipe:x8} crafts {CraftsBefore} -> {CraftsAfter}" : "") +
        (DryRun ? " (dry run)" : "") + (Message.Length > 0 ? $": {Message}" : "");
}
