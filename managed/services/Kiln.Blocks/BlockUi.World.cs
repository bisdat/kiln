// SPDX-License-Identifier: MIT
// BlockUi's live game values (dossier 09 §4.4, §5.2): ReadWorld, DemandWorld and UiWorldSnapshotChangedEvent over the
// native kiln.ui.snapshot (Ui/UiWorldSnapshotNative.cs). Collected by kiln-core on the right world's own thread, never by
// a renderer: a joined client's UI never calls the server-bound World, Items or Time APIs for these.
namespace Kiln;

public static partial class BlockUi
{
    /// <summary>
    /// The newest publication of live game values: the local player's health, stamina and mana as the game has them
    /// (before any mod's HUD change), the local player's position in this game's world, its own inventory, and the hour
    /// of the world when this game hosts it. One immutable value: its inventory is always of the same moment as its
    /// header, never mixed with another collection. Only what something demanded is collected
    /// (<see cref="DemandWorld"/>, and every open block window and HUD line for what it shows): call DemandWorld first
    /// and keep the registration while you read.
    /// </summary>
    /// <remarks>
    /// <para>Collected at most every 100 ms on the client world's step. Read a group only when
    /// <see cref="UiWorldSnapshot.Has"/> says it is there; <see cref="UiWorldSnapshot.UnavailableReasons"/> says why the
    /// others are missing (one entry per <see cref="UiWorldFields"/> bit in ascending order, null when present):
    /// <c>NoWorld</c> (menu, loading screen), <c>NoSnapshot</c> (not demanded, or no sample yet), <c>StaleSource</c>
    /// (the game's UI update and the world step did not meet in one step), <c>Budget</c> (more than 512 inventory slots),
    /// <c>DeferredSource</c> (the hour on a world this game joined: only a hosting game has it), <c>MissingComponent</c>,
    /// <c>LayoutOff</c> (this game build cannot read it), <c>Expired</c> (older than 1000 ms).</para>
    /// <para>Ok false with one of those names as <see cref="IKilnResult.Reason"/> when there is nothing to show: no world,
    /// no publication yet, the newest one older than 1000 ms, or the world changed during the read.</para>
    /// </remarks>
    /// <exception cref="KilnUnavailableException"><c>ui.blocks.world</c> is off (a dedicated server, an older kiln-core).</exception>
    [ClientOnly, AnyThread, RequiresCapability("ui.blocks.world")]
    public static UiWorldReadResult ReadWorld()
    {
        UiWorldCaps.Require();
        var r = UiWorldSnapshotNative.Read(UiWorldSnapshotNative.OwnOwner) ?? new UiWorldReadResult(false, "Unavailable", null);
        if (r is { Ok: false, Reason: "Unavailable" } && !UiWorldSnapshotNative.Present)
            throw ServiceCaps.Unavailable(UiWorldCaps.World, "kiln.ui.snapshot is not here");
        return r;
    }

    /// <summary>
    /// Asks kiln-core to collect <paramref name="fields"/> (any combination of <see cref="UiWorldFields"/>) until the
    /// registration is disposed or the calling mod unloads. Demands of all mods and windows are joined; nothing is read
    /// from the game while nobody asks. <see cref="UiWorldFields.HostTime"/> on a joined world is accepted and stays
    /// unavailable (<c>DeferredSource</c>): no network subscription is made for it.
    /// </summary>
    /// <returns>Ok, or Ok false with <c>Budget</c> (a mod holds at most 64 demands, all of them 1024).</returns>
    /// <exception cref="ArgumentException"><paramref name="fields"/> is <see cref="UiWorldFields.None"/> or has an unknown bit.</exception>
    /// <exception cref="KilnUnavailableException"><c>ui.blocks.world</c> is off.</exception>
    /// <exception cref="InvalidOperationException">The calling mod is unloading.</exception>
    [ClientOnly, AnyThread, RequiresCapability("ui.blocks.world")]
    public static UiDemandRegistration DemandWorld(UiWorldFields fields)
    {
        if (!UiWorldSnapshotNative.ValidFields(fields))
            throw new ArgumentException($"no known group of live game values in {fields}", nameof(fields));
        UiWorldCaps.Require();
        return UiWorldSnapshotNative.Demand(UiWorldSnapshotNative.OwnerOf(ModContext.CurrentMod), fields, strict: true);
    }
}

/// <summary>
/// A new publication of live game values (<see cref="BlockUi.ReadWorld"/>) is there. Raised on the game's main thread in
/// the client world's next step (on a host too, never on the server's tick), at most once per publication and only while
/// something subscribes: subscribing starts the watch. It carries only what was demanded (<see cref="BlockUi.DemandWorld"/>).
/// Only Kiln raises it.
/// </summary>
[ClientOnly]
public sealed class UiWorldSnapshotChangedEvent : KilnEvent
{
    static UiWorldSnapshotChangedEvent() => UiWorldFeed.Ensure();

    internal override bool TickDispatchAllowed => false;

    internal UiWorldSnapshotChangedEvent(ulong oldPublication, UiWorldSnapshot newSnapshot)
    {
        OldPublication = oldPublication;
        NewSnapshot = newSnapshot;
    }

    /// <summary>The publication this subscriber's feed saw before (0 for the first one).</summary>
    public ulong OldPublication { get; }

    /// <summary>The new publication.</summary>
    public UiWorldSnapshot NewSnapshot { get; }
}
