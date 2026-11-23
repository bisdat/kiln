// SPDX-License-Identifier: MIT
namespace Kiln.TestKit;

public sealed partial class KilnHarness
{
    private HarnessUiSnapshot? _uiSnapshot;

    /// <summary>The fake kiln.ui.snapshot of this harness (since 0.2.0-dev.31): publish the live game values that
    /// BlockUi.ReadWorld and UI blocks' <c>world.*</c> bindings read.</summary>
    public HarnessUiSnapshot UiSnapshot => _uiSnapshot ??= new HarnessUiSnapshot(this);
}

/// <summary>The local player's health, stamina and mana and their maxima, as the game has them.</summary>
public readonly record struct FakeVitals(uint Health, uint HealthMax, uint Stamina = 0, uint StaminaMax = 0, uint Mana = 0, uint ManaMax = 0);

/// <summary>The local player's position, in metres (the publication carries it in 32.32 fixed point).</summary>
public readonly record struct FakeWorldPosition(double X, double Y, double Z);

/// <summary>One nonempty slot of the local player's own inventory.</summary>
public readonly record struct FakeWorldSlot(uint Item, uint Count, uint InventoryPage = 0, uint SlotIndex = 0, uint Pide = 0);

/// <summary>
/// What a test publishes through <see cref="HarnessUiSnapshot.Publish"/>: each group it gives is in the publication when
/// something demanded it. A group left null is <c>NoSnapshot</c> (no sample yet); <see cref="Unavailable"/> names another
/// reason for a group (<c>StaleSource</c>, <c>LayoutOff</c>, <c>MissingComponent</c>, <c>Expired</c>, ...).
/// </summary>
public sealed record FakeWorldSample
{
    /// <summary>The local player's entity (each inventory slot's holder).</summary>
    public uint LocalPlayer { get; init; } = 1;

    /// <summary>Health, stamina and mana (<see cref="UiWorldFields.LocalVitals"/>).</summary>
    public FakeVitals? Vitals { get; init; }

    /// <summary>The position (<see cref="UiWorldFields.LocalPosition"/>).</summary>
    public FakeWorldPosition? Position { get; init; }

    /// <summary>The own inventory's nonempty slots (<see cref="UiWorldFields.LocalInventory"/>), in any order: the
    /// publication sorts them by page and slot. More than 512 make the group unavailable with <c>Budget</c>.</summary>
    public IReadOnlyList<FakeWorldSlot>? Inventory { get; init; }

    /// <summary>The hour of the world this game hosts (<see cref="UiWorldFields.HostTime"/>). On a
    /// <see cref="HarnessSide.JoinedClient"/> harness the group is <c>DeferredSource</c> whatever this says.</summary>
    public double? HostHours { get; init; }

    /// <summary>Groups that are missing for a reason of their own, by one <see cref="UiWorldFields"/> bit each.</summary>
    public IReadOnlyDictionary<UiWorldFields, string>? Unavailable { get; init; }
}

/// <summary>
/// The fake kiln.ui.snapshot of a harness (<see cref="KilnHarness.UiSnapshot"/>): Kiln.Blocks' BlockUi.ReadWorld,
/// BlockUi.DemandWorld and the block runtime's <c>world.*</c> bindings run on it unchanged.
/// </summary>
/// <remarks>
/// <para><b>Modelled.</b> Demands (64 per mod, 1024 in all; their union is what a publication carries), whole
/// publications (header and inventory of one moment; an undersized copy writes nothing and the retry gets the same
/// publication), tokens (2 per mod, 128 in all), a world change (<see cref="EndWorld"/>: every token revoked, NoWorld
/// until the next publication), the 512-slot budget, the hour on a joined world (DeferredSource).</para>
/// <para><b>Not modelled.</b> The game's collector (05a's stage, the client world's Post, GetEntityTransform, kiln.time),
/// the 100 ms cadence, the 1000 ms age and the 2 s token life. A publication is there at once; the fake answers on every
/// harness side.</para>
/// </remarks>
public sealed class HarnessUiSnapshot
{
    private readonly KilnHarness _harness;

    internal HarnessUiSnapshot(KilnHarness harness) => _harness = harness;

    private UiSnapshotFakeState State => KitFakes.State<UiSnapshotFakeState>(_harness);

    /// <summary>Makes <paramref name="sample"/> the newest publication and returns its id (ids grow by one per
    /// publication). Any thread.</summary>
    /// <exception cref="ArgumentException">An <see cref="FakeWorldSample.Unavailable"/> key that is not one group, or a
    /// reason kiln.ui.snapshot does not give.</exception>
    /// <exception cref="ArgumentOutOfRangeException">A position that is not finite or outside ±2147483648 metres.</exception>
    public ulong Publish(FakeWorldSample sample) => UiSnapshotFake.Publish(_harness, sample);

    /// <summary>A world or session change (the menu, a loading screen, a rejoin): the publication goes and every token is
    /// revoked at once. Demands stay.</summary>
    public void EndWorld() => UiSnapshotFake.EndWorld(_harness);

    /// <summary>The groups the live demands ask for together (what the next publication carries).</summary>
    public UiWorldFields Demanded
    {
        get { lock (State.Lock) return (UiWorldFields)State.Requested(); }
    }

    /// <summary>Demands held now (all mods, Kiln.Blocks' windows and HUD lines included).</summary>
    public int Demands
    {
        get { lock (State.Lock) return State.Demands.Count; }
    }

    /// <summary>Tokens not released yet (a read releases its own).</summary>
    public int Tokens
    {
        get { lock (State.Lock) return State.Tokens.Count; }
    }

    /// <summary>The newest publication's id; 0 when there is none.</summary>
    public ulong Publication
    {
        get { lock (State.Lock) return State.Current?.Header.Publication ?? 0; }
    }
}
