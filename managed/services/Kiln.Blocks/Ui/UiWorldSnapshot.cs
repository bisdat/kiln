// SPDX-License-Identifier: MIT
// Live game values for UI blocks (dossier 09 §4.4, §5): one immutable snapshot of this game's local player (vitals,
// position, own inventory) and, when this game hosts the world, the server's hour. Collected by kiln.ui.snapshot on the
// right world's own thread (05a's pre-provider stage and the client world's Post; the hosted server's Post), never by a
// renderer: a joined client's UI never calls the server-bound World, Items or Time APIs. The block runtime reads it
// through UiWorldSource; mods through BlockUi.ReadWorld and BlockUi.DemandWorld.
namespace Kiln;

/// <summary>The groups of live game values a window or a HUD line can ask for.</summary>
[Flags]
public enum UiWorldFields : uint
{
    /// <summary>Nothing.</summary>
    None = 0,
    /// <summary>The local player's health, stamina and mana and their maxima, as the game has them (before any mod's HUD change).</summary>
    LocalVitals = 1,
    /// <summary>The local player's position in this game's world.</summary>
    LocalPosition = 2,
    /// <summary>The local player's own inventory (nonempty slots).</summary>
    LocalInventory = 4,
    /// <summary>The hour of day of the world this game hosts. A joined world has none here (deferred).</summary>
    HostTime = 8,
}

/// <summary>When a part of a <see cref="UiWorldSnapshot"/> was taken: the world's session and step, and the capture time in milliseconds (Environment.TickCount64).</summary>
/// <param name="Session">The world's session: a world or session change (menu, loading screen, rejoin) gives a new one.</param>
/// <param name="Step">The world's step the values were taken in.</param>
/// <param name="CaptureMs">When they were taken: Environment.TickCount64, in milliseconds.</param>
public sealed record UiSnapshotStamp(ulong Session, ulong Step, ulong CaptureMs);

/// <summary>One nonempty slot of the local player's inventory. <see cref="Key"/> is <c>l:</c> + 8 hex digits of the page + <c>:</c> + the slot index.</summary>
/// <param name="Key">The slot's key, <c>l:</c> + 8 hex digits of the page + <c>:</c> + the slot index: the row key a table tells slots apart by.</param>
/// <param name="Holder">The entity that owns the inventory: the local player.</param>
/// <param name="InventoryPage">The inventory page (one of the player's inventories) the slot is in.</param>
/// <param name="SlotIndex">The slot's index in its page.</param>
/// <param name="Item">The game's item id of what is in the slot.</param>
/// <param name="Count">How many are in the slot.</param>
/// <param name="Pide">The stack's own item entity in this game's world; 0 for a plain stack without one.</param>
public sealed record UiInventoryValue(string Key, uint Holder, uint InventoryPage, byte SlotIndex, uint Item, uint Count, uint Pide);

/// <summary>
/// One publication of live game values. Read a field only when its group is in <see cref="AvailableMask"/>
/// (<see cref="UiWorldFields"/>); <see cref="UnavailableReasons"/> says why the others are missing, one entry per group in
/// ascending bit order. Position is in 32.32 fixed point (metres = value / 4294967296).
/// </summary>
/// <param name="Publication">The publication's id: each publication gets the next one (monotonic for the process).</param>
/// <param name="AvailableMask">The <see cref="UiWorldFields"/> groups this publication holds; the fields of other groups are not valid.</param>
/// <param name="Local">When the local player's values were taken, in the game's own world.</param>
/// <param name="Server">When <paramref name="ServerHours"/> was taken, in the world this game hosts; all zeros when it hosts none.</param>
/// <param name="LocalPlayer">The local player's entity.</param>
/// <param name="Health">The local player's health as the game computed it, before any mod's HUD change (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="HealthMax">The local player's maximum health (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="Stamina">The local player's stamina (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="StaminaMax">The local player's maximum stamina (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="Mana">The local player's mana (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="ManaMax">The local player's maximum mana (<see cref="UiWorldFields.LocalVitals"/>).</param>
/// <param name="XFixed">The local player's x position, 32.32 fixed point (<see cref="UiWorldFields.LocalPosition"/>).</param>
/// <param name="YFixed">The local player's y position, 32.32 fixed point (<see cref="UiWorldFields.LocalPosition"/>).</param>
/// <param name="ZFixed">The local player's z position, 32.32 fixed point (<see cref="UiWorldFields.LocalPosition"/>).</param>
/// <param name="ServerHours">The hour of day of the world this game hosts (<see cref="UiWorldFields.HostTime"/>).</param>
/// <param name="Inventory">The local player's nonempty own slots, sorted by page and slot (<see cref="UiWorldFields.LocalInventory"/>).</param>
/// <param name="UnavailableReasons">Why each group is missing (<c>NoWorld</c>, <c>StaleSource</c>, <c>LayoutOff</c>, ...), one entry per group in ascending bit order; null for a group that is there.</param>
public sealed record UiWorldSnapshot(ulong Publication, uint AvailableMask, UiSnapshotStamp Local, UiSnapshotStamp Server, uint LocalPlayer,
    uint Health, uint HealthMax, uint Stamina, uint StaminaMax, uint Mana, uint ManaMax,
    long XFixed, long YFixed, long ZFixed, decimal ServerHours,
    IReadOnlyList<UiInventoryValue> Inventory, IReadOnlyList<string?> UnavailableReasons)
{
    /// <summary>Whether every group in <paramref name="fields"/> is in this publication.</summary>
    public bool Has(UiWorldFields fields) => fields != UiWorldFields.None && ((UiWorldFields)AvailableMask & fields) == fields;
}

/// <summary>What <see cref="BlockUi.ReadWorld"/> returned: a snapshot, or why there is none now (no world, no sample yet, expired).</summary>
/// <param name="Ok">True when <paramref name="Snapshot"/> is set.</param>
/// <param name="Reason">Why there is no snapshot now; null when Ok.</param>
/// <param name="Snapshot">The latest publication; null when not Ok.</param>
public sealed record UiWorldReadResult(bool Ok, string? Reason, UiWorldSnapshot? Snapshot) : IKilnResult;

/// <summary>A request that the world collector gather some groups (<see cref="BlockUi.DemandWorld"/>). Dispose it when the values are no longer shown.</summary>
public sealed class UiDemandRegistration : IKilnResult, IDisposable
{
    private Action? _release;

    internal UiDemandRegistration(bool ok, string? reason, ulong handle, Action? release)
    {
        Ok = ok;
        Reason = reason;
        Handle = handle;
        _release = release;
    }

    /// <inheritdoc/>
    public bool Ok { get; }

    /// <inheritdoc/>
    public string? Reason { get; }

    /// <summary>The collector's handle; 0 when refused.</summary>
    public ulong Handle { get; }

    /// <summary>Stops asking. Safe to call more than once.</summary>
    public void Dispose() => Interlocked.Exchange(ref _release, null)?.Invoke();
}

/// <summary>Where the block runtime gets live game values: kiln.ui.snapshot in a game (UiWorldSnapshotNative); tests replace them.</summary>
internal static class UiWorldSource
{
    /// <summary>The latest whole publication (AnyThread; never a world read).</summary>
    internal static Func<UiWorldReadResult> Read = static () => UiWorldSnapshotNative.RuntimeRead();

    /// <summary>Adds a demand for some groups for <c>mod</c>; dispose to remove it (AnyThread).</summary>
    internal static Func<string, UiWorldFields, UiDemandRegistration> Demand = static (mod, fields) => UiWorldSnapshotNative.RuntimeDemand(mod, fields);

    /// <summary>This process hosts a live server world (world.server.available).</summary>
    internal static Func<bool> Hosting = static () => UiWorldSnapshotNative.Hosting();
}
