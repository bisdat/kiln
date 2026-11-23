// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>How a path request ended (or <see cref="Pending"/>). The numbers are kiln.queries' own.</summary>
public enum QueryPathStatus
{
    /// <summary>Not over yet: the request is waiting or being searched.</summary>
    Pending = 0,
    /// <summary>A walking route whose last point is at the target (within 1.5 m across and 3 m up or down).</summary>
    Found = 1,
    /// <summary>A walking route that ends somewhere else (the closest the search came, with or without <see cref="QueryPathRequest.ClosestFallback"/>).</summary>
    Partial = 2,
    /// <summary>The game's search found no route to the target.</summary>
    NoPath = 3,
    /// <summary>The game's search found no path node at the start point.</summary>
    NoStartNode = 4,
    /// <summary>The game's search found no path node at the target (see <see cref="QueryPathRequest.ClosestFallback"/>).</summary>
    NoTargetNode = 5,
    /// <summary>The search used up <see cref="QueryPathRequest.MaxIterations"/> before it came to an answer.</summary>
    IterationLimit = 6,
    /// <summary>The game's search ran out of memory.</summary>
    OutOfMemory = 7,
    /// <summary>The game gave no answer it stands behind: the area is not loaded, or a result code Kiln does not know.</summary>
    NotLoadedOrUnknown = 8,
    /// <summary>The request was cancelled: with <see cref="Queries.CancelPath"/>, or by cancelling the token of <see cref="Queries.FindPath"/> or <see cref="Queries.CanReach"/>.</summary>
    Cancelled = 9,
    /// <summary>The anchor went, or its id came to name another entity.</summary>
    OwnerGone = 10,
    /// <summary>No answer came before both 10 seconds and 150 server steps had passed.</summary>
    TimedOut = 11,
    /// <summary>More than 4096 points: none are given.</summary>
    Truncated = 12,
    /// <summary>The request was refused, or could not be answered (the service was unavailable); the reason says which.</summary>
    Refused = 13,
    /// <summary>The route needs a move that is not walking (a jump, a door, swimming): its points are given for diagnosis only.</summary>
    UnsupportedTraversal = 14,
    /// <summary>The game's route could not be read as one: a found route with no points, or a point or length that is not a finite number.</summary>
    MalformedResult = 15,
    /// <summary>32 requests are open in all, or 8 of this mod's, counting results not yet delivered.</summary>
    QueueFull = 16,
    /// <summary>The anchor has no SpawnTime marker: Kiln could not tell it from a later entity with the same id.</summary>
    OwnerIdentityUnavailable = 17,
    /// <summary>An endpoint is outside the realm's voxel grid: refused at once, the game is not asked.</summary>
    OutOfGrid = 18,
    /// <summary>The server world closed (or none was running) before the request ended.</summary>
    WorldClosed = 19,
    /// <summary>The mod that made the request was retired (unloaded or reloaded); its open requests end without a result.</summary>
    OwnerRetired = 20,
}

/// <summary>Where a path request is.</summary>
public enum QueryPathState
{
    /// <summary>The request was admitted.</summary>
    Created = 0,
    /// <summary>It waits in the queue for the game's search.</summary>
    Queued = 1,
    /// <summary>The game took the query and is searching.</summary>
    Searching = 2,
    /// <summary>The search ended with an answer.</summary>
    Completed = 3,
    /// <summary>The request was cancelled.</summary>
    Cancelled = 4,
    /// <summary>The request timed out.</summary>
    TimedOut = 5,
}

/// <summary>How a point of a route is reached from the one before it (the game's PathMoveType).</summary>
public enum QueryPathMove : byte
{
    /// <summary>A step on the ground. The only move a <see cref="QueryPathStatus.Found"/> or <see cref="QueryPathStatus.Partial"/> route has.</summary>
    Walk = 0,
    /// <summary>Flying.</summary>
    Fly = 1,
    /// <summary>A jump up to a higher point.</summary>
    JumpUp = 2,
    /// <summary>A jump down to a lower point.</summary>
    JumpDown = 3,
    /// <summary>A jump across a gap.</summary>
    JumpAcross = 4,
    /// <summary>Passing a door.</summary>
    OpenDoor = 5,
    /// <summary>Swimming on the surface of the water.</summary>
    Swim = 6,
    /// <summary>Diving under the water.</summary>
    Dive = 7,
    /// <summary>Walking on the ground under water.</summary>
    WalkUnderWater = 8,
    /// <summary>Flying through water.</summary>
    FlyThroughWater = 9,
}

/// <summary>One point of a route.</summary>
/// <param name="Position">Where the point is in the world.</param>
/// <param name="RawMove">The game's move byte as it came; <see cref="Move"/> when Kiln knows it.</param>
public sealed record QueryPathPoint(WorldPosition Position, byte RawMove)
{
    /// <summary>How this point is reached from the one before it; null when <see cref="RawMove"/> is not one of the known <see cref="QueryPathMove"/> values.</summary>
    [System.Text.Json.Serialization.JsonIgnore]
    public QueryPathMove? Move => RawMove <= 9 ? (QueryPathMove)RawMove : null;
}

/// <summary>A question to the game's path search: a walking route from <paramref name="From"/> to <paramref name="To"/>,
/// at most 200 m apart.</summary>
/// <param name="Anchor">An entity the game's query is made for. It is <b>not moved</b> and need not be what will walk the
/// route. It must have a SpawnTime marker, which entities spawned into the world by the game or by <c>Spawn</c> have;
/// saved entities, items and players may not. For those, spawn a helper (<c>Base_LootPickup_Generic_Gravity</c>) at
/// <paramref name="From"/>, use it as the anchor and destroy it afterwards.</param>
/// <param name="From">Where the route starts.</param>
/// <param name="To">Where the route should end. At most <see cref="Queries.MaxPathDistance"/> metres from <paramref name="From"/>.</param>
public sealed record QueryPathRequest(EntityId Anchor, WorldPosition From, WorldPosition To)
{
    /// <summary>Free space the route needs sideways, in metres, as the game's query takes it (0 to 16; rounded up to half metres).</summary>
    public double HorizontalSpace { get; init; } = 0.5;
    /// <summary>Free space the route needs upwards, in metres.</summary>
    public double VerticalSpace { get; init; } = 2;
    /// <summary>The search's iteration limit, 1 to 65535.</summary>
    public uint MaxIterations { get; init; } = 2000;
    /// <summary>When the target has no node, allow the closest one. Endpoint tolerance still determines whether the
    /// result is <see cref="QueryPathStatus.Found"/> or <see cref="QueryPathStatus.Partial"/>.</summary>
    public bool ClosestFallback { get; init; }
}

/// <summary>A route as it was at the step the game answered: a copy, nothing live. <see cref="Ok"/> for
/// <see cref="QueryPathStatus.Found"/> and <see cref="QueryPathStatus.Partial"/>; read <see cref="ReachesTarget"/> to tell them apart.</summary>
/// <param name="Ok">True when the game gave a walking route: <see cref="QueryPathStatus.Found"/> or <see cref="QueryPathStatus.Partial"/>.</param>
/// <param name="Reason">Why there is no route (the game's word, or the refusal); null when <see cref="Ok"/>.</param>
/// <param name="Request">The request's id (<see cref="QueryPathTicket.Request"/>); 0 when it was refused at once.</param>
/// <param name="Generation">The server world the request belongs to (worlds since the process started); 0 when it was refused at once.</param>
/// <param name="Status">How the request ended.</param>
/// <param name="GameResult">The game's own result code, when it gave one.</param>
/// <param name="ReachesTarget">True when the route's last point is at the target (<see cref="QueryPathStatus.Found"/>); false for a <see cref="QueryPathStatus.Partial"/> route.</param>
/// <param name="Points">The route's points from start to end, a copy. Empty when the game gave none or too many.</param>
/// <param name="Length">Metres along <paramref name="Points"/>, for a found or partial route.</param>
/// <param name="Steps">Server steps the request was worked on.</param>
public sealed record QueryPathResult(bool Ok, string? Reason, ulong Request, uint Generation, QueryPathStatus Status, byte? GameResult, bool ReachesTarget,
    IReadOnlyList<QueryPathPoint> Points, double Length, uint Steps) : IKilnResult;

/// <summary>An admitted request (<see cref="Ok"/>: <see cref="Completion"/> ends some steps later) or a refusal
/// (<see cref="Completion"/> is complete already, with the same reason).</summary>
/// <param name="Ok">True when the request was admitted.</param>
/// <param name="Reason">Why the request was refused; null when <see cref="Ok"/>.</param>
/// <param name="Request">The request's id, for <see cref="Queries.CancelPath"/> and to match <see cref="PathQueryChangedEvent.Request"/>; 0 when refused.</param>
/// <param name="Completion">The task that ends with the result: some server steps later when admitted, already complete when refused.</param>
public sealed record QueryPathTicket(bool Ok, string? Reason, ulong Request, Task<QueryPathResult> Completion) : IKilnResult;

/// <summary>Whether the target can be walked to. <see cref="Ok"/>: the search gave an answer, and <see cref="Reachable"/> is it.
/// Not <see cref="Ok"/>: nothing is known (<see cref="Reason"/>); <see cref="Reachable"/> is false then, and that does not mean "no".</summary>
/// <param name="Ok">True when the search gave an answer.</param>
/// <param name="Reason">Why nothing is known; null when <see cref="Ok"/>.</param>
/// <param name="Reachable">True when a walking route arrives at the target. False for no route or a route that ends elsewhere, and when not <see cref="Ok"/>.</param>
/// <param name="Status">How the search that answered ended.</param>
/// <param name="GameResult">The game's own result code, when it gave one.</param>
public sealed record QueryReachabilityResult(bool Ok, string? Reason, bool Reachable, QueryPathStatus Status, byte? GameResult) : IKilnResult;

/// <summary>The answer of <see cref="Queries.CancelPath"/>.</summary>
/// <param name="Ok">True when a request of the calling mod that had not ended was ended as <see cref="QueryPathStatus.Cancelled"/>.</param>
/// <param name="Reason">Why not; <c>UnknownRequest</c> when the request is unknown, another mod's or already ended. Null when <see cref="Ok"/>.</param>
/// <param name="Request">The request id that was asked about.</param>
public sealed record QueryCancelResult(bool Ok, string? Reason, ulong Request) : IKilnResult;

/// <summary>A path request of this mod moved on: queued, searching, or ended (<see cref="TerminalStatus"/>). Posted on the
/// tick thread; normally before the request's task completes. World close completes tasks immediately and defers the
/// event to a later tick; retired mod code receives no further events.</summary>
public sealed class PathQueryChangedEvent(ulong request, uint generation, EntityId anchor, QueryPathState before, QueryPathState after, QueryPathStatus? terminalStatus, string? reason) : KilnEvent
{
    /// <summary>The request's id (<see cref="QueryPathTicket.Request"/>).</summary>
    public ulong Request { get; } = request;
    /// <summary>The server world the request belongs to (worlds since the process started).</summary>
    public uint Generation { get; } = generation;
    /// <summary>The entity the request was made for (<see cref="QueryPathRequest.Anchor"/>).</summary>
    public EntityId Anchor { get; } = anchor;
    /// <summary>The state the request left.</summary>
    public QueryPathState Before { get; } = before;
    /// <summary>The state the request entered.</summary>
    public QueryPathState After { get; } = after;
    /// <summary>How the request ended; null while it is not over.</summary>
    public QueryPathStatus? TerminalStatus { get; } = terminalStatus;
    /// <summary>Why the request ended without a route; null while it is not over, and when it ended with a route.</summary>
    public string? Reason { get; } = reason;
}

internal static class PathMaps
{
    internal static async Task<QueryReachabilityResult> Reachability(Task<QueryPathResult> path) => Queries.Reachability(await path.ConfigureAwait(false));
}
