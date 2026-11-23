// SPDX-License-Identifier: MIT
// A brain's sight (docs/kiln/modding/npc-ai.md): by default distance and faction, as before. A behaviour can ask for voxel
// line of sight (Kiln.Queries): then a candidate behind terrain or a wall is not seen, and a candidate whose line could
// not be checked (not loaded, over the step's ray budget, the capability off) is not seen either. It never falls back to
// distance alone: a brain that asked for walls does not get wall vision when the answer is missing.
namespace Kiln;

/// <summary>What stands between a brain and what it sees.</summary>
public enum NpcOcclusionMode
{
    /// <summary>Nothing: everything hostile within <c>sight</c> is seen.</summary>
    DistanceAndFaction = 0,
    /// <summary>Terrain and building voxels (<c>Queries.VoxelLineOfSight</c>): not props, not other creatures.</summary>
    Voxels = 1,
}

/// <summary>How a creature with a brain sees. Set on <see cref="BehaviourBlock.Perception"/>; a creature keeps the policy
/// it was spawned with.</summary>
public sealed record NpcPerceptionPolicy
{
    /// <summary>What stands between the brain and what it sees. Default: <see cref="NpcOcclusionMode.DistanceAndFaction"/>.</summary>
    public NpcOcclusionMode Occlusion { get; init; } = NpcOcclusionMode.DistanceAndFaction;
    /// <summary>Added to the creature's position: where it looks from. Metres along the world's axes (Y up), each within 32.</summary>
    public WorldPosition FromOffset { get; init; } = new(0, 1.5, 0);
    /// <summary>Added to a candidate's position: what of it must be visible.</summary>
    public WorldPosition ToOffset { get; init; } = new(0, 1.5, 0);
}

/// <summary>What a brain's sight did since the creature was spawned. <see cref="Updates"/> counts looks (four a second).
/// In <see cref="NpcOcclusionMode.Voxels"/> each candidate of a look ends in exactly one of: <see cref="Visible"/>,
/// <see cref="Occluded"/>, <see cref="NotLoaded"/>, <see cref="OutOfGrid"/>, <see cref="BudgetExceeded"/>,
/// <see cref="OutOfRange"/> (farther than a ray reaches), <see cref="EntityGone"/>, <see cref="OtherRefused"/>; a look
/// while <c>queries.line_of_sight</c> is off counts in <see cref="CapabilityUnavailableUpdates"/> and sees nothing.
/// <see cref="LastRefusal"/> is the latest look's last refusal, or null.</summary>
/// <param name="Updates">How many looks the brain made (four a second).</param>
/// <param name="Checks">How many candidates were put to the line-of-sight question, in <see cref="NpcOcclusionMode.Voxels"/>.</param>
/// <param name="Visible">Candidates the line of sight found clear: they are seen.</param>
/// <param name="Occluded">Candidates the line of sight found blocked by terrain or a building.</param>
/// <param name="NotLoaded">Candidates whose line could not be checked because the terrain there was not loaded.</param>
/// <param name="OutOfGrid">Candidates whose line could not be checked because it left the grid the query covers.</param>
/// <param name="BudgetExceeded">Candidates whose line could not be checked because the step's ray budget was used up.</param>
/// <param name="OutOfRange">Candidates farther than a ray reaches, counted without asking the query.</param>
/// <param name="EntityGone">Candidates that had left the world when the line was checked.</param>
/// <param name="OtherRefused">Candidates whose line was refused for any other reason, or whose ray was degenerate (no length, or not a number).</param>
/// <param name="CapabilityUnavailableUpdates">Looks made while <c>queries.line_of_sight</c> was off. Such a look sees nothing.</param>
/// <param name="LastRefusal">The latest look's last refusal, such as <c>OutOfGrid</c>, or null.</param>
public sealed record NpcPerceptionStats(ulong Updates, ulong Checks, ulong Visible, ulong Occluded, ulong NotLoaded, ulong OutOfGrid, ulong BudgetExceeded,
    ulong OutOfRange, ulong EntityGone, ulong OtherRefused, ulong CapabilityUnavailableUpdates, string? LastRefusal);

internal sealed class NpcPerception(NpcPerceptionPolicy policy)
{
    public NpcPerceptionPolicy Policy { get; } = policy;

    private ulong _updates, _checks, _visible, _occluded, _notLoaded, _outOfGrid, _budget, _outOfRange, _gone, _other, _capability;
    private string? _last;

    public NpcPerceptionStats Stats => new(_updates, _checks, _visible, _occluded, _notLoaded, _outOfGrid, _budget, _outOfRange, _gone, _other, _capability, _last);

    private static void Add(ref ulong n) { if (n != ulong.MaxValue) n++; }

    internal const string Capability = "queries.line_of_sight";
    internal const double MaxOffset = 32;

    /// <summary>What is wrong with a policy, or null. Pure.</summary>
    internal static string? Problem(NpcPerceptionPolicy? p)
    {
        if (p == null) return "null: leave it out for the default";
        if (!Enum.IsDefined(p.Occlusion)) return $"occlusion {(int)p.Occlusion} is not DistanceAndFaction or Voxels";
        foreach (var (name, o) in new[] { ("fromOffset", p.FromOffset), ("toOffset", p.ToOffset) })
            foreach (var v in new[] { o.X, o.Y, o.Z })
                if (!double.IsFinite(v) || Math.Abs(v) > MaxOffset) return $"{name} {o} has a component that is not a number within {MaxOffset} m";
        return null;
    }

    /// <summary>One look. <paramref name="candidates"/>: the hostile ones within sight, nearest first (then by id), with
    /// their positions. Returns the ones that are seen. <paramref name="available"/>: null when
    /// <c>queries.line_of_sight</c> is on, else why it is off. <paramref name="sight"/>: the line-of-sight question;
    /// it may throw <see cref="KilnUnavailableException"/> when the capability goes while the look runs.</summary>
    public List<EntityId> Look(EntityId self, WorldPosition at, IReadOnlyList<(EntityId Entity, WorldPosition Position)> candidates,
        Func<string?> available, Func<EntityId, EntityId, VoxelSightOptions, VoxelSightResult> sight)
    {
        Add(ref _updates);
        _last = null;
        var seen = new List<EntityId>(candidates.Count);
        if (Policy.Occlusion == NpcOcclusionMode.DistanceAndFaction)
        {
            foreach (var (e, _) in candidates) seen.Add(e);
            return seen;
        }
        if (available() is { } off)
        {
            Add(ref _capability);
            _last = $"{Capability}: {off}";
            return seen;
        }
        var options = new VoxelSightOptions { FromOffset = Policy.FromOffset, ToOffset = Policy.ToOffset };
        var from = new WorldPosition(at.X + Policy.FromOffset.X, at.Y + Policy.FromOffset.Y, at.Z + Policy.FromOffset.Z);
        foreach (var (e, p) in candidates)
        {
            var to = new WorldPosition(p.X + Policy.ToOffset.X, p.Y + Policy.ToOffset.Y, p.Z + Policy.ToOffset.Z);
            // the ray's own number rule, so a candidate that is no ray costs no call and none of the step's budget
            switch (RayRules.Check(from, to))
            {
                case RayNumbers.TooLong:
                    Add(ref _outOfRange);
                    _last = "RayTooLong";
                    continue;
                case RayNumbers.Degenerate or RayNumbers.NotFinite:
                    Add(ref _other);
                    _last = "DegenerateRay";
                    continue;
            }
            Add(ref _checks);
            VoxelSightResult r;
            try { r = sight(self, e, options); }
            catch (KilnUnavailableException x)
            {
                // the capability went in the middle of the look: what was seen so far is not a look
                seen.Clear();
                Add(ref _capability);
                _last = $"{Capability}: {x.Message}";
                return seen;
            }
            catch (ArgumentException)
            {
                Add(ref _other);
                _last = "DegenerateRay";
                continue;
            }
            if (r.Ok)
            {
                if (r.Visible)
                {
                    Add(ref _visible);
                    seen.Add(e);
                }
                else Add(ref _occluded);
                continue;
            }
            _last = r.Reason ?? "Refused";
            switch (r.Reason)
            {
                case "NotLoaded": Add(ref _notLoaded); break;
                case "OutOfGrid": Add(ref _outOfGrid); break;
                case "BudgetExceeded": Add(ref _budget); break;
                case "EntityGone": Add(ref _gone); break;
                default: Add(ref _other); break;
            }
        }
        return seen;
    }
}
