// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessComponentChanges? _componentChanges;

    /// <summary>The kit's fake kiln.component_changes of this harness: what is watched, and the typed values its
    /// attribute events carry. Since 0.2.0-dev.29.</summary>
    public HarnessComponentChanges ComponentChanges => _componentChanges ??= new HarnessComponentChanges(this);
}

/// <summary>
/// The fake kiln.component_changes of a harness (<see cref="KilnHarness.ComponentChanges"/>): Kiln.World's own consumer of
/// <see cref="Kiln.ComponentChanges.Watch"/> runs on it. Since 0.2.0-dev.29.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> The service's rules: the budgets and refusals (32 watches, 4096 present pairs, 128 roots a
/// pair), the suspensions, the inventory links of <see cref="ComponentWatchExpansion.PlayerInventoryAndItems"/> (the fake
/// world's InventorySetup and Inventory bytes), the origin of a change (the kit's tracked writes are
/// <see cref="ChangeOrigin.Kiln"/> with the mod's id; bytes a test sets with <see cref="FakeWorld.SetBytes"/> are
/// <see cref="ChangeOrigin.Unknown"/>, as a game program's write), the two-frame queue, the world's end
/// (<see cref="HarnessDecisions.RestartWorld"/>). A change made during step N is posted in step N+1's Pre.</para>
/// <para><b>Not modelled.</b> The typed values of an attribute unless the test gives them (<see cref="SetAttribute"/>),
/// kiln.combat's and kiln.items' own writes, program wraps' writes, the native window calls' timing.</para>
/// </remarks>
public sealed class HarnessComponentChanges
{
    private readonly KilnHarness _harness;

    internal HarnessComponentChanges(KilnHarness harness) => _harness = harness;

    private ComponentChangesModel Model => ComponentChangesFake.ModelOf(_harness);

    /// <summary>The typed current and maximum value of an attribute as the game's attribute code would read them (the
    /// kit does not decode attribute bytes): an <see cref="AttributeValueChangedEvent"/> is raised for a change of the
    /// component when both the value before and after are known. Any thread.</summary>
    public void SetAttribute(EntityId entity, ObservedAttribute attribute, int current, int maximum) =>
        _harness.Call(() => Model.Typed[(entity.Value, (uint)attribute)] = (current, maximum));

    /// <summary>How many watches exist.</summary>
    public int Watches => _harness.Call(() => Model.Counts().Watches);

    /// <summary>The present (entity, component) pairs the watches cover.</summary>
    public int PresentPairs => _harness.Call(() => Model.Counts().Present);

    /// <summary>The frames of changes that wait for the consumer (at most two).</summary>
    public int QueuedFrames => _harness.Call(() => Model.Counts().QueuedFrames);

    /// <summary>How often the queue was full and the watches were suspended.</summary>
    public ulong QueueOverflows => _harness.Call(() => Model.QueueOverflows);
}
