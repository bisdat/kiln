// SPDX-License-Identifier: MIT
// The change events of the buff budget and of result-bearing buff definitions (chapter 17 I1, I5; since 0.2.0-dev.29):
// CapacityChangedEvent with Quota "gamedata.buff_budget", DefinitionRegistrationChangedEvent for a buff clone defined with
// GameDataRegistry.Define(clone, options). Both are posted at the next step, and only when something listens.
namespace Kiln;

internal static unsafe class BuffBudgetWatch
{
    private static readonly object Gate = new();
    private static CapacityInfo? _last;  // the budget as last seen: a GetBudget, a define, or the watch
    private static readonly Dictionary<Guid, (DefinitionRegistrationState State, uint Slots)> Known = new();

    /// <summary>A budget a mod read: it becomes the one the next change is measured from.</summary>
    public static CapacityQueryResult Seen(CapacityQueryResult r)
    {
        Compare(r.Budget);
        return r;
    }

    /// <summary>After a define: its registration's change, then the budget's.</summary>
    public static void Defined(string modId, DefinitionRegistrationResult r)
    {
        (DefinitionRegistrationState State, uint Slots)? before;
        lock (Gate)
        {
            before = Known.TryGetValue(r.RegistrationId, out var b) ? b : null;
            Known[r.RegistrationId] = (r.State, r.SlotsConsumed);
        }
        if (Events.HasSubscribers<DefinitionRegistrationChangedEvent>() &&
            DefinitionRegistration.Changed(modId, r, before?.State, before?.Slots ?? 0) is { } ev)
            Events.Enqueue(ev);
        Poll();
    }

    /// <summary>The service tick: every 25 steps, while something listens for <see cref="CapacityChangedEvent"/>.</summary>
    public static void Tick(ulong step)
    {
        if (step % 25 == 5 && Events.HasSubscribers<CapacityChangedEvent>()) Poll();
    }

    private static void Poll()
    {
        var t = GameDataNative.Buffs(Content.GD);
        if (t == null) return;
        var n = KilnCapacityInfo.New();
        if (t->BuffBudget(&n) == ContentNative.Ok) Compare(CapacityMapping.ToResult(n).Budget);
    }

    private static void Compare(CapacityInfo now)
    {
        CapacityInfo? prev;
        lock (Gate)
        {
            prev = _last;
            _last = now;
        }
        if (prev != null && Events.HasSubscribers<CapacityChangedEvent>() && CapacityMapping.Changed(ContentCaps.BuffBudget, prev, now) is { } ev)
            Events.Enqueue(ev);
    }
}
