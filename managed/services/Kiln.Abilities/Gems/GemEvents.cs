// SPDX-License-Identifier: MIT
// Gem wave G1: the gem events that need no forge hook. GemTriggeredEvent re-posts kiln.abilities' native events of a
// gem's impact (AbilitiesDispatch.Deliver), GemSocketChangedEvent comes from polling players' items (GemWatch).
// docs/kiln/services/gems.md.
namespace Kiln;

/// <summary>A socketed gem did something: its impact program ran applyDamage, applyBuff or spawnEntity (or a native
/// passed to <see cref="Abilities.Natives.Observe"/>). The same call as the <see cref="ImpactNativeEvent"/> in
/// <see cref="Native"/>, posted right after it (one Pre after the call), for the gem impacts of players' equipment:
/// the gem impact itself, or an impact it spawned (its <c>ImpactHierarchy</c> parent chain, up to 3 steps up).
/// Observe only; to change what a gem does use <see cref="Abilities.Natives.Wrap"/> or a data pack. Built only while
/// someone subscribes. Server (and a hosting client).</summary>
[ServerOnly]
public sealed class GemTriggeredEvent : KilnEvent, IEntityEvent
{
    static GemTriggeredEvent()
    {
        GemsCaps.Ensure();
        AbilitiesDispatch.Ensure();
    }

    internal GemTriggeredEvent(EquippedGem gem, ImpactNativeEvent native)
    {
        Gem = gem;
        Native = native;
        Target = native switch
        {
            ImpactDamageEvent d => d.Target,
            ImpactBuffEvent b => b.Target,
            ImpactSpawnEvent s => s.Spawned,
            _ => EntityId.None,
        };
    }

    /// <summary>The player whose gem it is.</summary>
    public EntityId Player => Gem.Player;

    /// <summary>The gem: its impact slot, carrier, gem item and level, and the gem impact's entity.</summary>
    public EquippedGem Gem { get; }

    /// <summary>What the gem did: an <see cref="ImpactDamageEvent"/>, <see cref="ImpactBuffEvent"/>,
    /// <see cref="ImpactSpawnEvent"/> or another observed native.</summary>
    public ImpactNativeEvent Native { get; }

    /// <summary>Whom it did it to: the damage's or buff's target, the spawned entity; none for another native.</summary>
    public EntityId Target { get; }

    /// <summary>The step the native ran in.</summary>
    public ulong Step => Native.Step;

    /// <summary>The gem's definition (<see cref="Gems.Get"/>; null when the catalog does not know it).</summary>
    public GemInfo? Info => Gems.Get(Gem.Socket.Gem);

    /// <summary>Whether the event is about <paramref name="entity"/> (the player or the target).</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Player || entity == Target);

    /// <summary>For a log line.</summary>
    public override string ToString() =>
        $"gem {Gem.Socket.Gem} level {Gem.Socket.Level} of {Player} ({Gem.Slot}): {Native.NativeName}" +
        (Target.IsNone ? "" : $" -> {Target}") + $" (step {Step})";
}

/// <summary>What happened to a socket (<see cref="GemSocketChangedEvent"/>).</summary>
public enum GemSocketChange
{
    /// <summary>The socket got a gem (empty before, or another gem: a swap).</summary>
    Socketed = 1,

    /// <summary>The socket is empty and the gem is back in the owner's inventory.</summary>
    Unsocketed = 2,

    /// <summary>The same gem at another level (the forge's Upgrade, or a mod).</summary>
    LevelChanged = 3,

    /// <summary>The gem no longer exists: it left the socket without coming back to the owner's inventory (the forge's
    /// Recycle), or the carrier was destroyed with it (<see cref="GemSocketChangedEvent.After"/> null for both).</summary>
    Removed = 4,
}

/// <summary>A gem socket of one of a player's items changed: socketed, unsocketed, upgraded or removed, however it
/// happened (the Gem Forge, a mod, a command). Kiln reads players' items in turns (about 2 players a step, each
/// player every <c>players / 2</c> steps), so the event comes a few steps late, says nothing about forges or costs, and a
/// change undone between two reads is not seen. A carrier that leaves the player with its gem (into a chest, to another
/// player) is not a change; an item first seen with a gem (picked up, crafted) is not either. Observe only; built only
/// while someone subscribes. Server (and a hosting client).</summary>
[ServerOnly]
public sealed class GemSocketChangedEvent : KilnEvent, IEntityEvent
{
    static GemSocketChangedEvent()
    {
        GemsCaps.Ensure();
        GemWatch.Hook();
    }

    internal GemSocketChangedEvent(EntityId owner, ItemInstanceRef carrier, SocketedGem? before, SocketedGem? after,
        GemSocketChange change, ulong step)
    {
        Owner = owner;
        Carrier = carrier;
        Before = before;
        After = after;
        Change = change;
        Step = step;
    }

    /// <summary>The player whose item it is.</summary>
    public EntityId Owner { get; }

    /// <summary>The item with the socket.</summary>
    public ItemInstanceRef Carrier { get; }

    /// <summary>The socket's gem before (null: empty).</summary>
    public SocketedGem? Before { get; }

    /// <summary>The socket's gem now (null: empty, or the carrier is gone).</summary>
    public SocketedGem? After { get; }

    /// <summary>What happened.</summary>
    public GemSocketChange Change { get; }

    /// <summary>The step Kiln saw it in.</summary>
    public ulong Step { get; }

    /// <summary>Whether the event is about <paramref name="entity"/> (the owner or the carrier's item entity).</summary>
    public bool Involves(EntityId entity) => !entity.IsNone && (entity == Owner || entity == Carrier.Entity.Id);

    /// <summary>For a log line.</summary>
    public override string ToString() =>
        $"{Owner}'s {Carrier}: {Change} ({Show(Before)} -> {Show(After)}, step {Step})";

    private static string Show(SocketedGem? g) => g is { } s ? $"{s.Gem} level {s.Level}" : "empty";
}
