// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>A negotiated Kiln peer gets fifteen seconds to offer its copy. After the server-only decision, late offers
/// cannot replace live storage. Tick counts are service steps (25 Hz), independent of wall-clock adjustments.</summary>
internal sealed class StorageOfferWindow(ulong joinedAt)
{
    private readonly ulong _joinedAt = joinedAt;
    internal const ulong TimeoutSteps = 15 * 25;
    internal bool Decided { get; private set; }
    internal bool TimedOut { get; private set; }
    internal bool Offer()
    {
        if (Decided) return false;
        Decided = true;
        return true;
    }
    internal bool Poll(ulong now, bool vanilla)
    {
        if (Decided || (!vanilla && now - _joinedAt < TimeoutSteps)) return false;
        Decided = true;
        TimedOut = !vanilla;
        return true;
    }
}
