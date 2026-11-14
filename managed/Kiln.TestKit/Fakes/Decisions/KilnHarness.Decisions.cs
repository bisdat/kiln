// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessDecisions? _decisions;

    /// <summary>The kit's fake kiln.decisions of this harness: how the decisions mods were asked about ended. Since
    /// 0.2.0-dev.28.</summary>
    public HarnessDecisions Decisions => _decisions ??= new HarnessDecisions(this);
}

/// <summary>
/// The fake kiln.decisions of a harness (<see cref="KilnHarness.Decisions"/>): Kiln's own dispatcher of decisions runs
/// on it. Since 0.2.0-dev.28.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> The service's broker: every decision has an id, is delivered to the handlers once in the Pre
/// after it was held, is answered once and ends in one <see cref="DecisionCompletedEvent"/>; a held action that nobody
/// answers is released after two more steps (<see cref="DecisionTerminal.TimeoutReleased"/>); a world that ends
/// (<see cref="RestartWorld"/>) ends what is open with <see cref="DecisionTerminal.WorldEnded"/>.</para>
/// <para><b>Not modelled.</b> The game: nothing dies, is picked up or equipped here. The events that hold such an
/// action are faked with the service that owns them.</para>
/// </remarks>
public sealed class HarnessDecisions
{
    private readonly KilnHarness _harness;

    internal HarnessDecisions(KilnHarness harness) => _harness = harness;

    internal DecisionsModel Model => DecisionsFake.ModelOf(_harness);

    /// <summary>How every decision ended so far, in order: what the service recorded, whether or not a mod listened for
    /// <see cref="DecisionCompletedEvent"/>.</summary>
    public IReadOnlyList<DecisionResult> Outcomes => Model.Record;

    /// <summary>How many decisions are open (held, not ended).</summary>
    public int Active => Model.ActiveCount;

    /// <summary>The server world stops and the next one starts, as when a hosted world is switched: every open decision
    /// ends <see cref="DecisionTerminal.WorldEnded"/> (mods hear of it at the next step), and decisions of the next
    /// world carry the next <see cref="DecisionId.WorldEpoch"/>.</summary>
    public void RestartWorld() => _harness.Call(() => Model.RestartWorld());
}
