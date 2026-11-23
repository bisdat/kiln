// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>What occupies a voxel cell. Since 0.2.0-dev.30.6.</summary>
public enum BuildBlockKind
{
    /// <summary>An empty cell. A terrain cell with zero fill also reads as air.</summary>
    Air,
    /// <summary>A terrain cell (raw material ids 1-127) with a nonzero fill.</summary>
    Terrain,
    /// <summary>A building block (raw material ids 128-255). Zero paint is still a building block.</summary>
    Building,
}
/// <summary>Comparison with the shipped world, not ownership or execution side. Since 0.2.0-dev.30.6.</summary>
public enum BlockProvenance
{
    /// <summary>Not known: the baked column is not loaded, its reader is unqualified, the origin cannot be established,
    /// or the cell is air.</summary>
    Unknown = -1,
    /// <summary>Differs from the shipped world: a building with a different material, or terrain where the shipped
    /// cell had no filled terrain.</summary>
    PlayerPlaced,
    /// <summary>Same as the shipped world: a building with the same material as the shipped cell, or terrain where the
    /// shipped cell had any filled terrain. A ruin rebuilt with its original material is Natural again, and digging,
    /// repainting or refilling natural terrain never makes it <see cref="PlayerPlaced"/>.</summary>
    Natural,
}
/// <summary>The material's mining hardness class. Since 0.2.0-dev.30.6.</summary>
public enum BlockHardness
{
    /// <summary>The softest class (0).</summary>
    Soft,
    /// <summary>The second class (1).</summary>
    SlightlyHard,
    /// <summary>The middle class (2).</summary>
    ModeratelyHard,
    /// <summary>The fourth class (3).</summary>
    Hard,
    /// <summary>The hardest breakable class (4).</summary>
    VeryHard,
    /// <summary>The material cannot be broken (5). <see cref="Build.BreakBox"/> always spares it, even with
    /// <see cref="BlockBreakFlags.Instant"/>.</summary>
    Unbreakable,
}
/// <summary>A block operation's explicit refusal. Since 0.2.0-dev.30.6.</summary>
public enum BlockRefusal
{
    /// <summary>No refusal: the call went ahead. A query that ran and found nothing (a clear line) is also None.</summary>
    None = 0,
    /// <summary>The requested mode has no qualified engine implementation on this build. Nothing was changed.
    /// The drops mode (<see cref="BlockBreakFlags.Drops"/>) currently always refuses this way.</summary>
    SymbolMissing = 1,
    /// <summary>The <c>kiln.build</c> service is not loaded.</summary>
    ServiceMissing = 2,
    /// <summary>The <c>kiln.build</c> service is older than the block API and has no row for the call.</summary>
    ServiceOlder = 3,
    /// <summary>The server has authority only: a client joined to another machine's world cannot read or break that
    /// world. Ask the host or dedicated server.</summary>
    WrongSide = 4,
    /// <summary>There is no game in this process.</summary>
    Offline = 8,
    /// <summary>There is no running server world.</summary>
    NoSession = 9,
    /// <summary>The native service refused an argument, for example a position outside <see cref="Build.Grid"/>, unknown
    /// flag bits, or a damage or sphere value out of range. An out-of-grid cell is not proof of air or a clear line. The
    /// managed argument checks of <see cref="Build.BreakBox"/> throw instead.</summary>
    InvalidArgument = 16,
    /// <summary>The native call was not made on the server tick. Managed calls check the thread first and throw
    /// instead.</summary>
    WrongThread = 17,
    /// <summary>The voxel tiles are not loaded. Use <see cref="Build.Load"/> before querying or breaking the area.</summary>
    NotLoaded = 18,
    /// <summary>A protection zone denies the edit (<see cref="BuildFlags.RespectZones"/>). The whole box is refused.</summary>
    Zone = 19,
    /// <summary>The box touches a protected player base (no <see cref="BuildFlags.InBases"/>). The whole box is refused.</summary>
    Base = 20,
    /// <summary>The operation faulted. Partial changes may remain, and a restart is required before retrying.</summary>
    Faulted = 21,
    /// <summary>The edit could not be verified by readback. Partial or neighbouring changes may remain.</summary>
    Unverified = 22,
    /// <summary>A <see cref="BuildEditingEvent"/> handler canceled the edit before the native call.</summary>
    Canceled = 23,
    /// <summary>The terrain damage map is full. Retry after damage heals; this is not an engine fault.</summary>
    DamageMapFull = 24,
}
/// <summary>Additional terrain hits for a wall search. Since 0.2.0-dev.30.6.</summary>
[Flags]
public enum FindWallFlags
{
    /// <summary>Stop only at building cells; terrain is passed over.</summary>
    None = 0,
    /// <summary>Also stop at player-placed terrain; unknown provenance is passed over.</summary>
    PlayerTerrain = 1,
    /// <summary>Also stop at all terrain, including natural ground.</summary>
    Terrain = 2,
}
/// <summary>Controlled breaking options. Since 0.2.0-dev.30.6.</summary>
[Flags]
public enum BlockBreakFlags
{
    /// <summary>Damage mode, the default: each hit adds damage by hardness and a cell is removed when its damage
    /// reaches 255. Natural and unknown-provenance buildings are spared and all terrain stays.</summary>
    None = 0,
    /// <summary>Remove immediately, but never Unbreakable materials.</summary>
    Instant = 1,
    /// <summary>Use material item/rate drops; refuses the whole call when unqualified.</summary>
    Drops = 2,
    /// <summary>Include natural and unknown-provenance buildings. Never overrides terrain protection.</summary>
    WorldBuildings = 4,
    /// <summary>Include only player-placed terrain. Natural and unknown-provenance ground stays.</summary>
    PlayerTerrain = 8,
}
/// <summary>Loaded cell facts. Material is the raw voxel id (building 128..255); Fill is paint for buildings, not
/// occupancy or provenance. Null mining facts mean unknown. Health is raw config health, not damage-scaled.
/// Since 0.2.0-dev.30.6.</summary>
/// <param name="Kind">What occupies the cell: air, terrain or a building block.</param>
/// <param name="Material">The raw voxel material id. Building materials are 128-255 (not EMF's 0-127); terrain ids are
/// 1-127.</param>
/// <param name="Fill">The raw fill byte. For a building it is paint, so zero paint is still a building block; for terrain
/// a zero fill is air. It says nothing about provenance.</param>
/// <param name="Provenance">Comparison with the shipped world: <see cref="BlockProvenance.Natural"/>,
/// <see cref="BlockProvenance.PlayerPlaced"/> or <see cref="BlockProvenance.Unknown"/>. It is not an owner id, a paint
/// bit or the server or client side.</param>
/// <param name="Hardness">The material's mining hardness class; null when unknown (never guessed) or for an air
/// cell.</param>
/// <param name="Health">The material's raw health points from its config, not scaled by damage; null when unknown or for
/// an air cell.</param>
/// <param name="Damage">The cell's current damage, 0-255. For a building it is the game's native crack state; for terrain
/// it is Kiln's accumulated damage value, read when the session clock is available. Null when unknown.</param>
/// <param name="Center">The centre of the cell in world metres.</param>
public sealed record BlockInfo(BuildBlockKind Kind, byte Material, byte Fill, BlockProvenance Provenance,
    BlockHardness? Hardness, int? Health, int? Damage, WorldPosition Center);
/// <summary>A successful query may have no hit. Refusal is distinct from a clear line. Since 0.2.0-dev.30.6.</summary>
/// <param name="Info">The facts of the cell that was read or hit; null when a query that ran found nothing (a clear line),
/// or when it was refused.</param>
/// <param name="Refusal"><see cref="BlockRefusal.None"/> when the query ran, even with no hit; otherwise why it did not.
/// A refused query is not proof of air or of a clear line.</param>
public sealed record BlockQueryResult(BlockInfo? Info, BlockRefusal Refusal) : IKilnResult
{
    /// <summary>True when the query hit a cell: <see cref="Info"/> is not null.</summary>
    public bool Found => Info is not null;
    /// <summary>True when the query ran (<see cref="Refusal"/> is <see cref="BlockRefusal.None"/>), whether or not it
    /// found a cell.</summary>
    public bool Ok => Refusal == BlockRefusal.None;
    /// <summary>Why the query was refused, in words; null when <see cref="Ok"/>.</summary>
    public string? Reason => BlockReasons.Text(Refusal);
}
/// <summary>Sphere selection with a caller-controlled protection margin and minimum radius. Only the building radius
/// shrinks away from Natural/Unknown buildings; terrain keeps the original radius. Since 0.2.0-dev.30.6.</summary>
/// <param name="Center">The sphere's centre in world metres.</param>
/// <param name="Radius">The sphere's radius in metres; finite and greater than zero, or the call is refused as
/// <see cref="BlockRefusal.InvalidArgument"/>. Terrain is selected inside this full radius.</param>
/// <param name="Margin">The protection margin in metres, zero or more; default 0. The cells within
/// <c>Radius + Margin</c> of the centre are scanned for Natural or Unknown buildings, and the building radius shrinks to
/// the distance of the nearest one minus this margin. The scan must fit 4096 loaded cells.</param>
/// <param name="MinimumRadius">Metres, zero or more; default 0. When the shrunken building radius falls below this, no
/// buildings are hit. Terrain keeps the full <paramref name="Radius"/> either way.</param>
public sealed record BlockBreakSphere(WorldPosition Center, double Radius, double Margin = 0, double MinimumRadius = 0);
/// <summary>One exact, loaded cell box, optionally selected by a cell mask. Since 0.2.0-dev.30.6.</summary>
public sealed record BlockBreakOptions
{
    /// <summary>The break mode and protection choices. None (the default) is damage mode with natural buildings and all
    /// terrain protected; <see cref="BlockBreakFlags.Instant"/> removes at once.</summary>
    public BlockBreakFlags Flags { get; init; }
    /// <summary>Five nonnegative damage values, by hardness (up to 100000). Empty/all zero means 100,100,95,60,50.</summary>
    public int[] Damage { get; init; } = [];
    /// <summary>Raw voxel material ids to include. Empty means every breakable material.</summary>
    public byte[] Materials { get; init; } = [];
    /// <summary>Optional mask in x,y,z order, with exactly the box's cell count. False cells are untouched.</summary>
    public bool[]? Selection { get; init; }
    /// <summary>The existing build permissions, actor, journal and label. Unloaded and AllowAltars are refused.</summary>
    public BuildOptions Build { get; init; } = new();
    /// <summary>Terrain damage threshold as a multiple of material health. Default 1; finite, greater than zero,
    /// at most one million. Terrain has no native visual crack state.</summary>
    public double TerrainHealthScale { get; init; } = 1;
    /// <summary>Forget terrain damage after this inactivity. Default 20 seconds; greater than zero, at most one day.
    /// Building crack healing follows the game's native 20-second lifetime instead.</summary>
    public TimeSpan TerrainHealAfter { get; init; } = TimeSpan.FromSeconds(20);
    /// <summary>Optional sphere. Scan Radius+Margin for eligible protected buildings, shrink the building radius to
    /// the nearest protected center minus Margin, skipping buildings below MinimumRadius. Terrain keeps the full sphere.
    /// WorldBuildings bypasses shrink; an additional Selection mask still restricts edited cells.</summary>
    public BlockBreakSphere? ProtectedSphere { get; init; }
}
/// <summary>Counts and refusal of a controlled break. A fault may leave partial commits; never assume compensation.
/// Removed counts cells proven air by readback. Without Verified it is only a lower bound. BlocksWritten can be
/// partial. Kept is the underlying writer's successful save-region report, not a durability promise after faults.
/// Since 0.2.0-dev.30.6.</summary>
/// <param name="Refusal"><see cref="BlockRefusal.None"/> when the call went ahead; otherwise why it was refused or failed.
/// <see cref="BlockRefusal.Faulted"/> and <see cref="BlockRefusal.Unverified"/> can leave partial changes, so read the
/// other fields too.</param>
/// <param name="Considered">Selected cells that were not air.</param>
/// <param name="Damaged">Cells whose damage rose. In <see cref="BlockBreakFlags.Instant"/> mode it equals
/// <paramref name="Removed"/>.</param>
/// <param name="Removed">Selected cells read back as air. Without <paramref name="Verified"/> it is only a lower
/// bound.</param>
/// <param name="Dropped">How many drops were granted. Drops are not available yet (the drops mode refuses as
/// <see cref="BlockRefusal.SymbolMissing"/>), so this stays 0.</param>
/// <param name="ProtectedCells">Considered cells spared because their provenance is Natural: natural buildings (unless
/// <see cref="BlockBreakFlags.WorldBuildings"/> was set) and, with <see cref="BlockBreakFlags.PlayerTerrain"/>, natural
/// terrain.</param>
/// <param name="UnknownCells">Considered cells spared because their material facts are unknown or their provenance is
/// Unknown (buildings only when <see cref="BlockBreakFlags.WorldBuildings"/> is not set).</param>
/// <param name="UnbreakableCells">Considered cells spared because their material is
/// <see cref="BlockHardness.Unbreakable"/>.</param>
/// <param name="ExcludedCells">Considered cells left alone by the selection rules: outside the
/// <see cref="BlockBreakOptions.ProtectedSphere"/>, terrain without <see cref="BlockBreakFlags.PlayerTerrain"/>, or a
/// material not in <see cref="BlockBreakOptions.Materials"/>.</param>
/// <param name="MayHaveChanged">The writer or damage mutator was entered, so changes may have happened (also after a
/// fault). False when nothing was touched. There is no rollback, so never repeat the whole operation blindly.</param>
/// <param name="Verified">The readback behind <paramref name="Removed"/> completed (also set when nothing was selected).
/// It does not say every planned removal succeeded.</param>
/// <param name="BlocksWritten">8x8x8 blocks the writer committed; can be partial after a fault.</param>
/// <param name="Kept">The writer reported the touched chunks in Kiln's save regions. Not a durability promise after
/// a fault.</param>
public sealed record BlockBreakResult(BlockRefusal Refusal, ulong Considered, ulong Damaged, ulong Removed, ulong Dropped,
    ulong ProtectedCells, ulong UnknownCells, ulong UnbreakableCells, ulong ExcludedCells,
    bool MayHaveChanged, bool Verified, uint BlocksWritten, bool Kept) : IKilnResult
{
    /// <summary>True when the break was not refused (<see cref="Refusal"/> is <see cref="BlockRefusal.None"/>). Spared
    /// cells and cells that only took damage still count as ok; read the counts to see what happened.</summary>
    public bool Ok => Refusal == BlockRefusal.None;
    /// <summary>Why the break was refused or failed, in words; null when <see cref="Ok"/>.</summary>
    public string? Reason => BlockReasons.Text(Refusal);
}
internal static class BlockReasons
{
    internal static string? Text(BlockRefusal r) => r switch
    {
        BlockRefusal.None => null,
        BlockRefusal.SymbolMissing => "the requested mode has no qualified engine implementation on this build",
        BlockRefusal.ServiceMissing => "kiln.build is not loaded",
        BlockRefusal.ServiceOlder => "kiln.build is older than the block API",
        BlockRefusal.WrongSide => "server authority only; ask the host or dedicated server",
        BlockRefusal.Offline => "no game in this process",
        BlockRefusal.NoSession => "no running server world",
        BlockRefusal.NotLoaded => "the voxel tiles are not loaded",
        BlockRefusal.Zone => "a protection zone denies this edit",
        BlockRefusal.Base => "the box touches a protected player base",
        BlockRefusal.Faulted => "the operation faulted; partial changes may remain; restart required before retrying",
        BlockRefusal.Unverified => "the requested edit could not be verified; partial or neighbouring changes may remain",
        BlockRefusal.Canceled => "a BuildEditingEvent handler canceled the edit",
        BlockRefusal.DamageMapFull => "the terrain damage map is full; retry after damage heals; earlier hits may remain",
        _ => r.ToString(),
    };
}

public static partial class Build
{
    /// <summary>Reads a loaded cell on the server tick. Missing metadata/provenance stays unknown, not inferred from
    /// paint or fill. Since 0.2.0-dev.30.6.</summary>
    [TickThreadOnly, RequiresCapability("build.block_info")]
    public static unsafe BlockQueryResult BlockAt(WorldPosition position)
    {
        Services.RequireTickThread("Build.BlockAt");
        Rows.Ensure();
        var t = T;
        if (t == null) return new(null, BlockRefusal.ServiceMissing);
        if (t->Header.Size < 168 || t->BlockAt == null) return new(null, BlockRefusal.ServiceOlder);
        BuildNative.BlockInfo b = default; b.Size = (uint)sizeof(BuildNative.BlockInfo);
        var rc = t->BlockAt(new(position), &b);
        return BlockQuery(rc, b);
    }
    /// <summary>Finds the first crossed building cell within 64 metres, optionally terrain. Includes the starting
    /// cell; returns a successful result without Info for a clear line. Since 0.2.0-dev.30.6.</summary>
    [TickThreadOnly, RequiresCapability("build.find_wall")]
    public static unsafe BlockQueryResult FindWall(WorldPosition from, WorldPosition to, FindWallFlags flags = FindWallFlags.None)
    {
        Services.RequireTickThread("Build.FindWall");
        Rows.Ensure();
        var t = T;
        if (t == null) return new(null, BlockRefusal.ServiceMissing);
        if (t->Header.Size < 176 || t->FindWall == null) return new(null, BlockRefusal.ServiceOlder);
        BuildNative.BlockInfo b = default; b.Size = (uint)sizeof(BuildNative.BlockInfo);
        var rc = t->FindWall(new(from), new(to), (uint)flags, &b);
        return BlockQuery(rc, b);
    }
    private static BlockQueryResult BlockQuery(int rc, BuildNative.BlockInfo b)
    {
        if (rc == BuildNative.NotFound && b.Refusal == 0) return new(null, BlockRefusal.None);
        if (rc != 0) return new(null, b.Refusal == 0 ? BlockRefusal.Faulted : (BlockRefusal)b.Refusal);
        return new(new((BuildBlockKind)b.Kind, (byte)b.Material, (byte)b.Fill, (BlockProvenance)b.Provenance,
            b.Hardness < 0 ? null : (BlockHardness)b.Hardness, b.Health < 0 ? null : b.Health,
            b.Damage < 0 ? null : b.Damage, b.Center.ToPosition()), BlockRefusal.None);
    }
    /// <summary>Breaks selected cells in an exact box of at most 4096 loaded cells. Unknown/Unbreakable materials and
    /// natural terrain are spared. An unavailable requested mode refuses everything before mutation. Damage, cracks and
    /// drops require their own qualified capability; no unrestricted fallback. A writer fault may partially commit.
    /// Since 0.2.0-dev.30.6.</summary>
    [TickThreadOnly, RequiresCapability("build.break_building")]
    public static unsafe BlockBreakResult BreakBox(CellBox box, BlockBreakOptions? options = null)
    {
        Services.RequireTickThread("Build.BreakBox");
        Rows.Ensure();
        var t = T;
        if (t == null) return BreakRefused(BlockRefusal.ServiceMissing);
        if (t->Header.Size < 184 || t->BreakBox == null) return BreakRefused(BlockRefusal.ServiceOlder);
        CheckBox(box);
        if (box.Count > 4096) throw new ArgumentException("at most 4096 cells per break", nameof(box));
        var o = options ?? new(); var build = o.Build;
        if (o.Damage.Length != 0 && o.Damage.Length != 5) throw new ArgumentException("five damage values required", nameof(options));
        if (o.Selection != null && o.Selection.Length != box.Count) throw new ArgumentException("selection size", nameof(options));
        // Every argument check throws before BuildEditingEvent: handlers never see an edit that is then not attempted.
        if (!double.IsFinite(o.TerrainHealthScale) || o.TerrainHealthScale <= 0 || o.TerrainHealthScale > 1e6 ||
            o.TerrainHealAfter <= TimeSpan.Zero || o.TerrainHealAfter > TimeSpan.FromDays(1))
            throw new ArgumentOutOfRangeException(nameof(options), "invalid terrain health threshold or expiry");
        if (!Editing(BuildEditKind.Write, box, build)) return BreakRefused(BlockRefusal.Canceled);
        BuildNative.Break q = default; q.Size = (uint)sizeof(BuildNative.Break); q.Flags = (uint)o.Flags;
        q.Origin[0] = box.X; q.Origin[1] = box.Y; q.Origin[2] = box.Z;
        q.Dims[0] = (uint)box.SizeX; q.Dims[1] = (uint)box.SizeY; q.Dims[2] = (uint)box.SizeZ;
        for (var i = 0; i < o.Damage.Length; i++) q.Damage[i] = o.Damage[i];
        foreach (var m in o.Materials) q.Materials[m / 8] |= (byte)(1 << (m % 8));
        q.Actor = build.Actor.Value; q.BuildFlags = (uint)build.Flags;
        q.TerrainHealthScale = o.TerrainHealthScale; q.TerrainHealSeconds = o.TerrainHealAfter.TotalSeconds;
        if (o.ProtectedSphere is { } sphere)
        {
            q.Flags |= 16;
            q.SphereCenter = new(sphere.Center); q.SphereRadius = sphere.Radius;
            q.SphereMargin = sphere.Margin; q.SphereMinimum = sphere.MinimumRadius;
        }
        var mask = o.Selection?.Select(b => b ? (byte)1 : (byte)0).ToArray();
        var origin = System.Text.Encoding.UTF8.GetBytes(Caller);
        Voxel[]? before = null;
        JournalEntry? entry = null;
        if (build.Journal && TryRead(box, out before)) entry = Journal.Begin(BuildEditKind.Write, build);
        BuildNative.BreakResult r = default; r.Size = (uint)sizeof(BuildNative.BreakResult);
        int rc;
        fixed (byte* m = mask)
        fixed (byte* mod = origin)
        {
            q.Selection = m; q.OriginMod = new NativeStr { Ptr = mod, Len = (nuint)origin.Length };
            rc = t->BreakBox(&q, &r);
        }
        var refusal = r.Refusal == 0 && rc != 0 ? BlockRefusal.Faulted : (BlockRefusal)r.Refusal;
        var changed = (r.Flags & 1) != 0;
        if (entry != null && before != null && changed && TryRead(box, out var after)) entry.AddCells(box, before, after);
        // Publish only observed removals, never the writer's planned cells_changed after a partial fault.
        var editRefusal = refusal switch { BlockRefusal.Zone => BuildRefusal.Zone, BlockRefusal.Base => BuildRefusal.Base,
            BlockRefusal.NotLoaded => BuildRefusal.NotLoaded, _ => BuildRefusal.None };
        // Count readback is weaker than BuildResult.Verified's exact-write contract.
        var editVerified = refusal == BlockRefusal.None && (r.Write.Flags & BuildNative.RVerified) != 0;
        var edit = new BuildResult((long)r.Removed, (int)r.Write.BlocksWritten, editVerified,
            (r.Write.Flags & BuildNative.RKept) != 0, box, r.Write.Milliseconds, editRefusal, r.Write.Zone);
        Finish(BuildEditKind.Write, box, build, edit, entry);
        return new(refusal, r.Considered, r.Damaged, r.Removed, r.Dropped, r.ProtectedCells, r.UnknownCells,
            r.UnbreakableCells, r.ExcludedCells, changed, (r.Flags & 2) != 0, r.Write.BlocksWritten,
            (r.Write.Flags & BuildNative.RKept) != 0);
    }
    private static BlockBreakResult BreakRefused(BlockRefusal reason) => new(reason, 0, 0, 0, 0, 0, 0, 0, 0, false, false, 0, false);
}
