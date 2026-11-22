// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>What occupies a voxel cell. Since 0.2.0-dev.30.6.</summary>
public enum BuildBlockKind { Air, Terrain, Building }
/// <summary>Comparison with the shipped world, not ownership or execution side. Since 0.2.0-dev.30.6.</summary>
public enum BlockProvenance { Unknown = -1, PlayerPlaced, Natural }
/// <summary>The material's mining hardness class. Since 0.2.0-dev.30.6.</summary>
public enum BlockHardness { Soft, SlightlyHard, ModeratelyHard, Hard, VeryHard, Unbreakable }
/// <summary>A block operation's explicit refusal. Since 0.2.0-dev.30.6.</summary>
public enum BlockRefusal
{
    None = 0, SymbolMissing = 1, ServiceMissing = 2, ServiceOlder = 3, WrongSide = 4, Offline = 8, NoSession = 9,
    InvalidArgument = 16, WrongThread = 17, NotLoaded = 18, Zone = 19, Base = 20, Faulted = 21, Unverified = 22,
    Canceled = 23,
}
/// <summary>Additional terrain hits for a wall search. Since 0.2.0-dev.30.6.</summary>
[Flags]
public enum FindWallFlags
{
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
public sealed record BlockInfo(BuildBlockKind Kind, byte Material, byte Fill, BlockProvenance Provenance,
    BlockHardness? Hardness, int? Health, int? Damage, WorldPosition Center);
/// <summary>A successful query may have no hit. Refusal is distinct from a clear line. Since 0.2.0-dev.30.6.</summary>
public sealed record BlockQueryResult(BlockInfo? Info, BlockRefusal Refusal) : IKilnResult
{
    public bool Found => Info is not null;
    public bool Ok => Refusal == BlockRefusal.None;
    public string? Reason => BlockReasons.Text(Refusal);
}
/// <summary>Sphere selection with a caller-controlled protection margin and minimum radius. Only the building radius
/// shrinks away from Natural/Unknown buildings; terrain keeps the original radius. Since 0.2.0-dev.30.6.</summary>
public sealed record BlockBreakSphere(WorldPosition Center, double Radius, double Margin = 0, double MinimumRadius = 0);
/// <summary>One exact, loaded cell box, optionally selected by a cell mask. Since 0.2.0-dev.30.6.</summary>
public sealed record BlockBreakOptions
{
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
public sealed record BlockBreakResult(BlockRefusal Refusal, ulong Considered, ulong Damaged, ulong Removed, ulong Dropped,
    ulong ProtectedCells, ulong UnknownCells, ulong UnbreakableCells, ulong ExcludedCells,
    bool MayHaveChanged, bool Verified, uint BlocksWritten, bool Kept) : IKilnResult
{
    public bool Ok => Refusal == BlockRefusal.None;
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
        if (!Editing(BuildEditKind.Write, box, build)) return BreakRefused(BlockRefusal.Canceled);
        BuildNative.Break q = default; q.Size = (uint)sizeof(BuildNative.Break); q.Flags = (uint)o.Flags;
        q.Origin[0] = box.X; q.Origin[1] = box.Y; q.Origin[2] = box.Z;
        q.Dims[0] = (uint)box.SizeX; q.Dims[1] = (uint)box.SizeY; q.Dims[2] = (uint)box.SizeZ;
        for (var i = 0; i < o.Damage.Length; i++) q.Damage[i] = o.Damage[i];
        foreach (var m in o.Materials) q.Materials[m / 8] |= (byte)(1 << (m % 8));
        q.Actor = build.Actor.Value; q.BuildFlags = (uint)build.Flags;
        if (!double.IsFinite(o.TerrainHealthScale) || o.TerrainHealthScale <= 0 || o.TerrainHealthScale > 1e6 ||
            o.TerrainHealAfter <= TimeSpan.Zero || o.TerrainHealAfter > TimeSpan.FromDays(1))
            throw new ArgumentOutOfRangeException(nameof(options), "invalid terrain health threshold or expiry");
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
