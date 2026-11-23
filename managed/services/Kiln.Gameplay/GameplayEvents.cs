// SPDX-License-Identifier: MIT
// Gameplay events on Kiln's bus (Forge: PlayerInteractEvent, BlockEvent, ItemCraftedEvent, EntityItemPickupEvent,
// EntityTeleportEvent, LivingSpawnEvent.CheckSpawn). All are posted on the tick thread, and only while a handler is
// subscribed to them. See docs/kiln/services/gameplay.md for when each fires and what canceling does in the game.
namespace Kiln;

/// <summary>Actions a protection zone can deny (and the action an event is about).</summary>
[Flags]
public enum GameplayActions : uint
{
    /// <summary>No action.</summary>
    None = 0,
    /// <summary>Place building blocks and props.</summary>
    Build = 0x01,
    /// <summary>Dismantle blocks, pick props up.</summary>
    Break = 0x02,
    /// <summary>Dig, add or flatten terrain.</summary>
    Terraform = 0x04,
    /// <summary>Accept interaction offers (open chests and doors, use stations, beds, portals, talk to NPCs).</summary>
    Interact = 0x08,
    /// <summary>Loot, harvest, pick items up.</summary>
    Loot = 0x10,
    /// <summary>Craft at stations inside.</summary>
    Craft = 0x20,
    /// <summary>Natural spawns (random spawners) inside; applies to everyone.</summary>
    Spawn = 0x40,
    /// <summary>Every action.</summary>
    All = 0x7F,
}

/// <summary>A stack of items (item id as the game's <c>ItemId</c>, see Kiln.Items).</summary>
/// <param name="ItemId">The item's id (the game's <c>ItemId</c>); 0 in a recipe's inputs means an input
/// category.</param>
/// <param name="Count">How many.</param>
public readonly record struct GameplayItemStack(uint ItemId, int Count);

/// <summary>Base of the cancelable gameplay events: a protection zone may have canceled the event before any handler
/// ran (<see cref="DeniedByZone"/> != 0, <see cref="CancelableEvent.Canceled"/> already true). A handler that
/// subscribed with <c>receiveCanceled</c> may un-cancel it, which overrides the zone for this one event.</summary>
public abstract class GameplayCancelableEvent : CancelableEvent
{
    /// <summary>Server-world step.</summary>
    public ulong Step { get; internal init; }

    /// <summary>Id of the zone that denied this (0 = none).</summary>
    public uint DeniedByZone { get; internal init; }

    /// <summary>Name of that zone ("" when none or unknown).</summary>
    public string ZoneName => DeniedByZone == 0 ? "" : Gameplay.ZoneName(DeniedByZone);

    /// <summary>Sent by Kiln (<see cref="Gameplay.Inject"/>), not by the game.</summary>
    public bool Injected { get; internal init; }
}

/// <summary>Base of the gameplay notifications.</summary>
public abstract class GameplayEvent : KilnEvent
{
    /// <summary>The server-world step the event was posted in.</summary>
    public ulong Step { get; internal init; }
}

// ------------------------------------------------------------------------------------------------- crafting ----

/// <summary>A craft request the game handles in this step (a player's craft at a station or by hand, or a mod's
/// request). Pre phase: before the game's <c>handle_crafting_events</c> reads it. Cancel: the request is dropped;
/// nothing is consumed or made, and the client is told nothing (its UI shows the materials still there).</summary>
public sealed class CraftEvent : GameplayCancelableEvent
{
    /// <summary>Whose inventory pays and receives: a player, a chest, a villager.</summary>
    public EntityId Operator { get; internal init; }
    /// <summary>The entity with <c>Crafting</c>: a workbench, a villager, or the player itself (hand crafting).</summary>
    public EntityId Station { get; internal init; }
    /// <summary>The recipe being crafted.</summary>
    public uint RecipeId { get; internal init; }
    /// <summary>How many times the recipe is crafted.</summary>
    public int Amount { get; internal init; }
    /// <summary>Where the station is; null when it has no position.</summary>
    public WorldPosition? StationPosition { get; internal init; }
    /// <summary>True when the operator is a player.</summary>
    public bool ByPlayer => Operator.IsPlayer;
}

/// <summary>The game's result codes of one craft (the craft core).</summary>
public enum CraftResult
{
    /// <summary>The craft went through.</summary>
    Crafted = 0,
    /// <summary>The output did not fit in the operator's inventory.</summary>
    NoRoomForOutput = 2,
    /// <summary>The operator lacked ingredients.</summary>
    MissingMaterials = 4,
    /// <summary>A crafting prop the recipe needs was not near the station.</summary>
    MissingCraftingProps = 5,
    /// <summary>Any other code of the game; <see cref="ItemCraftedEvent.RawResult"/> has it.</summary>
    Other = -1,
}

/// <summary>The game ran its craft core once (one unit of a craft request, or an automated station's cycle). Post
/// phase of the same step. Forge's ItemCraftedEvent, fired for failures too: check <see cref="Succeeded"/>.
/// An automated station (a kiln, a smelter: <see cref="Operator"/> == <see cref="Station"/>) is reported once per
/// finished cycle, when its outputs arrive, and only on success (since 0.2.0-dev.29; before, a cycle's start, when the
/// inputs are taken, was reported as well).</summary>
public sealed class ItemCraftedEvent : GameplayEvent
{
    static ItemCraftedEvent() => Gameplay.RegisterHookEvent(typeof(ItemCraftedEvent),2,"gameplay.craft");
    /// <summary>Whose inventory paid and received.</summary>
    public EntityId Operator { get; internal init; }
    /// <summary>The entity that did the crafting: a workbench, a villager, or the player (hand crafting).</summary>
    public EntityId Station { get; internal init; }
    /// <summary>The recipe that was crafted.</summary>
    public uint RecipeId { get; internal init; }
    /// <summary>The id of the workshop the recipe belongs to.</summary>
    public uint WorkshopId { get; internal init; }
    /// <summary>The game's result code, as a named value.</summary>
    public CraftResult Result { get; internal init; }
    /// <summary>The game's raw result code; 0 is success.</summary>
    public int RawResult { get; internal init; }
    /// <summary>True when the craft went through (<see cref="RawResult"/> is 0).</summary>
    public bool Succeeded => RawResult == 0;
    /// <summary>The recipe's outputs for one craft (what arrived when <see cref="Succeeded"/>).</summary>
    public IReadOnlyList<GameplayItemStack> Outputs { get; internal init; } = [];
    /// <summary>Where the station is; null when it has no position.</summary>
    public WorldPosition? StationPosition { get; internal init; }
    /// <summary>True when the operator is a player.</summary>
    public bool ByPlayer => Operator.IsPlayer;
}

// ---------------------------------------------------------------------------------------------- interaction ----

/// <summary>Someone accepts an interaction offer: E on a chest, door, workstation, bed, portal, NPC (villagers send
/// it too). Pre phase, before the game's <c>interaction_attempt</c>. Cancel: the attempt becomes the game's own
/// DeclinedInteraction, which ends the guest's interaction sequence as when the game refuses (the client sees the
/// interaction fail; nothing opens).</summary>
public sealed class InteractEvent : GameplayCancelableEvent
{
    /// <summary>Who accepts the offer: a player or a villager.</summary>
    public EntityId Guest { get; internal init; }
    /// <summary>The entity whose offer is accepted: a chest, a door, a station, a bed, a portal, an NPC.</summary>
    public EntityId Host { get; internal init; }
    /// <summary>The id of the offer that is accepted.</summary>
    public uint OfferId { get; internal init; }
    /// <summary>Where the host is; null when it has no position.</summary>
    public WorldPosition? HostPosition { get; internal init; }
    /// <summary>True when the guest is a player.</summary>
    public bool ByPlayer => Guest.IsPlayer;
}

/// <summary>The game's answer to an interaction attempt of the previous step (Pre phase): accepted, or declined
/// (including the ones Kiln declined).</summary>
public sealed class InteractionResultEvent : GameplayEvent
{
    /// <summary>Who tried to interact.</summary>
    public EntityId Guest { get; internal init; }
    /// <summary>The entity they tried to interact with.</summary>
    public EntityId Host { get; internal init; }
    /// <summary>The id of the offer.</summary>
    public uint OfferId { get; internal init; }
    /// <summary>True when the game accepted the attempt; false when it was declined.</summary>
    public bool Accepted { get; internal init; }
}

/// <summary>A loot interaction: take an item lying in the world, loot a container all at once, harvest a plant,
/// gather produce (Forge's EntityItemPickupEvent). Pre phase, before the game's loot programs. Cancel: becomes a
/// DeclinedInteraction; nothing moves. Items collected by walking over them (no interaction) are not seen here,
/// only in <see cref="ItemPickupEvent"/>.</summary>
public sealed class LootEvent : GameplayCancelableEvent
{
    /// <summary>Who receives: a player, or a chest / villager for mod-sent loot.</summary>
    public EntityId Guest { get; internal init; }
    /// <summary>What is looted: the item pickup, container, plant, animal.</summary>
    public EntityId Host { get; internal init; }
    /// <summary>The id of the loot offer.</summary>
    public uint OfferId { get; internal init; }
    /// <summary>The game's <c>lootAll</c> flag of the request.</summary>
    public bool LootAll { get; internal init; }
    /// <summary>The game's <c>bypassInteraction</c> flag of the request.</summary>
    public bool BypassInteraction { get; internal init; }
    /// <summary>Where the looted entity is; null when it has no position.</summary>
    public WorldPosition? HostPosition { get; internal init; }
    /// <summary>True when the guest is a player.</summary>
    public bool ByPlayer => Guest.IsPlayer;
}

/// <summary>Items arrived in an inventory in the previous step (the game's PickupEvent: the "+N" popup). Fires for
/// looting, harvesting, crafting output, building refunds (Forge's ItemPickupEvent, after the fact).</summary>
public sealed class ItemPickupEvent : GameplayEvent
{
    /// <summary>The inventory owner the items arrived with.</summary>
    public EntityId Receiver { get; internal init; }
    /// <summary>Where the items came from (none for crafting output).</summary>
    public EntityId Source { get; internal init; }
    /// <summary>The item that arrived (the game's <c>ItemId</c>).</summary>
    public uint ItemId { get; internal init; }
    /// <summary>How many arrived.</summary>
    public int Count { get; internal init; }
    /// <summary>The 8-slot inventory entity the items went to, and the slot index.</summary>
    public EntityId SlotInventory { get; internal init; }
    /// <summary>The index of the slot they went to, in <see cref="SlotInventory"/>.</summary>
    public int SlotIndex { get; internal init; }
    /// <summary>The game's flag for a pickup that shows no popup.</summary>
    public bool HiddenInUi { get; internal init; }
    /// <summary>Where the pickup happened.</summary>
    public WorldPosition Position { get; internal init; }
}

// ------------------------------------------------------------------------------------------------- building ----

/// <summary>What a <see cref="BuildEvent"/> is about.</summary>
public enum BuildKind
{
    /// <summary>A building block was placed.</summary>
    PlaceBlock = 1,
    /// <summary>A prop was placed.</summary>
    PlaceProp = 2,
    /// <summary>A block was dismantled.</summary>
    Dismantle = 3,
    /// <summary>A prop was picked up.</summary>
    PickUpProp = 4,
    /// <summary>Terrain was dug, added to, flattened or changed (see
    /// <see cref="BuildEvent.TerraformType"/>).</summary>
    Terraform = 5,
}

/// <summary>How a building operation ended.</summary>
public enum BuildOutcome
{
    /// <summary>The game did it.</summary>
    Done,
    /// <summary>The game refused it itself (no room, collision, missing materials, ...).</summary>
    RefusedByGame,
    /// <summary>A protection zone refused it (the game rolled the inventory back and told the client it failed).</summary>
    DeniedByZone,
}

/// <summary>A building or terraforming operation of this step (Forge's BlockEvent.EntityPlaceEvent / BreakEvent).
/// Post phase: the operation already ran (or was refused) inside the step on a game worker thread, where no mod code
/// can run. To refuse edits use <see cref="Gameplay.Protect"/> zones, which the game's building code consults
/// synchronously; this event then reports <see cref="BuildOutcome.DeniedByZone"/>.</summary>
/// <remarks>Managed mods referencing Kiln.Gameplay request the five building detours at host startup, even for
/// crafting-only use. Native plugins request them at startup if they will later add build/break denies. With no worlds this
/// happens immediately. Interest first seen while any world runs waits for checked installation at safe Pre ticks; the shared
/// budget is two targets per Pre (five building targets need three Pres when alone). Earlier operations are not replayed. Check capability <c>gameplay.build_hooks</c> for the
/// waiting/refusal reason. Unsubscribing leaves installed hooks in place, passing through without listeners or zones.
/// Terraforming uses its separate hook.</remarks>
public sealed class BuildEvent : GameplayEvent
{
    static unsafe BuildEvent() => EventProducers.Register(typeof(BuildEvent),
        GameplayNative.RequestBuild(GameplayNative.T) != null ? "gameplay.build_hooks" : "gameplay.build",
        Gameplay.PrepareBuildEvents, allowPending: true);
    /// <summary>What the operation was.</summary>
    public BuildKind Kind { get; internal init; }
    /// <summary>The player who did it.</summary>
    public EntityId Player { get; internal init; }
    /// <summary>Block / prop item (place), material item (dismantle); 0 for terraforming.</summary>
    public uint ItemId { get; internal init; }
    /// <summary>Where it happened.</summary>
    public WorldPosition Position { get; internal init; }
    /// <summary>The prop entity (<see cref="BuildKind.PickUpProp"/>).</summary>
    public EntityId Target { get; internal init; }
    /// <summary>Terraform: 0 remove (dig), 1 add, 2-3 flatten, 4-5 overgrowth, 6-7 decay.</summary>
    public int TerraformType { get; internal init; }
    /// <summary>How it ended.</summary>
    public BuildOutcome Outcome { get; internal init; }
    /// <summary>The id of the zone that refused it (0 = none).</summary>
    public uint DeniedByZone { get; internal init; }
    /// <summary>The name of that zone (empty when none or unknown).</summary>
    public string ZoneName => DeniedByZone == 0 ? "" : Gameplay.ZoneName(DeniedByZone);
    /// <summary>True for placing a block or a prop.</summary>
    public bool IsPlacement => Kind is BuildKind.PlaceBlock or BuildKind.PlaceProp;
    /// <summary>True for dismantling a block, picking up a prop, or digging terrain (terraform type 0).</summary>
    public bool IsRemoval => Kind is BuildKind.Dismantle or BuildKind.PickUpProp || (Kind == BuildKind.Terraform && TerraformType == 0);
}

// ------------------------------------------------------------------------------------------------ teleports ----

/// <summary>A watched entity (every player; others with <see cref="Gameplay.WatchTeleports"/>) is about to be
/// teleported by the game: fast travel, respawn, portal, admin teleport, kiln.world. Pre phase, the step before the
/// game's <c>teleport</c> program moves it. Cancel: the entity stays where it is. Setting <see cref="Target"/> sends
/// it there instead.</summary>
public sealed class TeleportEvent : GameplayCancelableEvent
{
    /// <summary>The entity that is about to be teleported.</summary>
    public EntityId Entity { get; internal init; }
    /// <summary>Where it is now.</summary>
    public WorldPosition From { get; internal init; }
    /// <summary>The destination. Handlers may change it (metres).</summary>
    public WorldPosition Target { get; set; }
    /// <summary>The spawn point / flame altar it travels to (none for a plain position).</summary>
    public EntityId TargetEntity { get; internal init; }
    /// <summary>The game searches walkable ground near the destination.</summary>
    public bool SearchesGround { get; internal init; }
    /// <summary>True when the entity is a player.</summary>
    public bool IsPlayer => Entity.IsPlayer;
}

/// <summary>A watched entity was teleported this step (its TeleportCount changed). Post phase.</summary>
public sealed class TeleportedEvent : GameplayEvent
{
    /// <summary>The entity that was teleported.</summary>
    public EntityId Entity { get; internal init; }
    /// <summary>Where it was before.</summary>
    public WorldPosition From { get; internal init; }
    /// <summary>Where it is now.</summary>
    public WorldPosition To { get; internal init; }
    /// <summary>The entity's teleport count after this teleport.</summary>
    public int Count { get; internal init; }
}

// ------------------------------------------------------------------------------------------------- spawning ----

/// <summary>A random spawner (the world's natural enemy and wildlife spawns, which run near players) created a
/// creature this step (Forge's LivingSpawnEvent.CheckSpawn). Post phase: the entity exists but no client has seen it.
/// Cancel: it is destroyed before the step returns; the spawner treats it as gone. Spawns refused by a
/// <see cref="GameplayActions.Spawn"/> zone arrive already canceled with <see cref="Entity"/> none.</summary>
public sealed class NaturalSpawnEvent : GameplayCancelableEvent
{
    static NaturalSpawnEvent() => Gameplay.RegisterHookEvent(typeof(NaturalSpawnEvent),8,"gameplay.spawn");
    /// <summary>The creature the spawner made; none for a spawn that a zone refused.</summary>
    public EntityId Entity { get; internal init; }
    /// <summary>The GUID of the creature's template.</summary>
    public Guid Template { get; internal init; }
    /// <summary>Where it spawned.</summary>
    public WorldPosition Position { get; internal init; }
    /// <summary>A <see cref="Gameplay.ReplaceNaturalSpawns"/> rule swapped the spawner's template: <see cref="Template"/>
    /// is the replacement.</summary>
    public bool Replaced { get; internal init; }
}

// ------------------------------------------------------------------------------------------- entity lifecycle ----

/// <summary>Kinds of entities for <see cref="EntityJoinLevelEvent"/> / <see cref="EntityLeaveLevelEvent"/> (from the
/// template name, like <see cref="TemplateKind"/>).</summary>
[Flags]
public enum EntityCategories : uint
{
    /// <summary>No category.</summary>
    None = 0,
    /// <summary>Players.</summary>
    Player = 0x01,
    /// <summary>Enemies and animals.</summary>
    Creature = 0x02,
    /// <summary>Villagers.</summary>
    Npc = 0x04,
    /// <summary>Props: chests, stations, decoration, building props.</summary>
    Prop = 0x08,
    /// <summary>Loose item pickups.</summary>
    Pickup = 0x10,
    /// <summary>The rest: VFX, attacks, projectiles, loot containers, spawners, weakspots, child entities. Many and
    /// short-lived: only reported when a mod asks for them with <see cref="Gameplay.WatchEntities"/>.</summary>
    Other = 0x20,
    /// <summary>What is reported when no mod narrowed or widened it.</summary>
    Default = Player | Creature | Npc | Prop | Pickup,
    /// <summary>Every category.</summary>
    All = 0x3F,
}

/// <summary>Fields shared by <see cref="EntityJoinLevelEvent"/> and <see cref="EntityLeaveLevelEvent"/>.</summary>
public interface IEntityLifecycleEvent
{
    /// <summary>The entity that joined or left the world.</summary>
    EntityId Entity { get; }
    /// <summary>Template GUID (null for entities without one).</summary>
    Guid? Template { get; }
    /// <summary>The entity this one belongs to (a creature for its weakspot), or None.</summary>
    EntityId Parent { get; }
    /// <summary>The entity's category.</summary>
    EntityCategories Category { get; }
    /// <summary>Where it was (metres); null when it has no transform.</summary>
    WorldPosition? Position { get; }
    /// <summary>Created / destroyed outside the game's sim step: by Kiln (Kiln.Spawn, a service, a mod's Pre/Post code),
    /// not by a game program. Entities the game makes for a Kiln request inside the step (a gear item's entity from
    /// Kiln.Items) count as the game's.</summary>
    bool ByKiln { get; }
    /// <summary>Kiln.Spawn's template record (name, display name, kind), or null.</summary>
    TemplateInfo? TemplateInfo { get; }
    /// <summary>The engine's template name ("Enemy_Vukah_Melee"), or "".</summary>
    string TemplateName { get; }
}

internal struct TemplateLookup
{
    private TemplateInfo? _info;
    private bool _looked;

    public TemplateInfo? Get(Guid? template)
    {
        if (_looked) return _info;
        _looked = true;
        return _info = template is { } g ? Spawn.Template(g) : null;
    }
}

/// <summary>An entity was created in the server world (Forge: EntityJoinLevelEvent), by any path: spawners, NPC
/// logic, loot, child entities, a player joining, the save loading (once, at world start), or Kiln.Spawn. Posted in
/// Post of the step that created it (or the Post after a creation on the tick thread). Only the categories mods ask for
/// are reported (<see cref="Gameplay.WatchEntities"/>; <see cref="EntityCategories.Default"/> otherwise).</summary>
/// <remarks>Cancel (not players): the entity is destroyed again right away, like a canceled
/// <see cref="NaturalSpawnEvent"/>; game logic that created it treats it as gone. Be selective: removing child entities
/// (weakspots, links) of a creature you keep can break it.</remarks>
public sealed class EntityJoinLevelEvent : GameplayCancelableEvent, IEntityLifecycleEvent
{
    static EntityJoinLevelEvent() => Gameplay.RegisterHookEvent(typeof(EntityJoinLevelEvent),16,"gameplay.entity_events");
    private TemplateLookup _t;
    /// <inheritdoc/>
    public EntityId Entity { get; internal init; }
    /// <inheritdoc/>
    public Guid? Template { get; internal init; }
    /// <inheritdoc/>
    public EntityId Parent { get; internal init; }
    /// <inheritdoc/>
    public EntityCategories Category { get; internal init; }
    /// <inheritdoc/>
    public WorldPosition? Position { get; internal init; }
    /// <inheritdoc/>
    public bool ByKiln { get; internal init; }
    /// <inheritdoc/>
    public TemplateInfo? TemplateInfo => _t.Get(Template);
    /// <inheritdoc/>
    public string TemplateName => TemplateInfo?.Name ?? "";
    /// <summary>True when the entity is a player.</summary>
    public bool IsPlayer => Category == EntityCategories.Player;
}

/// <summary>An entity was destroyed (Forge: EntityLeaveLevelEvent): LifeTime ran out, the game culled or despawned
/// it, its loot was taken, a queued destroy, <c>Spawn.Destroy</c>. Posted in Post; observe only. Not reported for the
/// bulk removal when the world shuts down.</summary>
public sealed class EntityLeaveLevelEvent : GameplayEvent, IEntityLifecycleEvent
{
    static EntityLeaveLevelEvent() => Gameplay.RegisterHookEvent(typeof(EntityLeaveLevelEvent),16,"gameplay.entity_events");
    private TemplateLookup _t;
    /// <inheritdoc/>
    public EntityId Entity { get; internal init; }
    /// <inheritdoc/>
    public Guid? Template { get; internal init; }
    /// <inheritdoc/>
    public EntityId Parent { get; internal init; }
    /// <inheritdoc/>
    public EntityCategories Category { get; internal init; }
    /// <inheritdoc/>
    public WorldPosition? Position { get; internal init; }
    /// <inheritdoc/>
    public bool ByKiln { get; internal init; }
    /// <inheritdoc/>
    public TemplateInfo? TemplateInfo => _t.Get(Template);
    /// <inheritdoc/>
    public string TemplateName => TemplateInfo?.Name ?? "";
    /// <summary>True when the entity was a player.</summary>
    public bool IsPlayer => Category == EntityCategories.Player;
}

// ------------------------------------------------------------------------------------------------- discard ------

/// <summary>How a player discarded a stack (<see cref="ItemTossEvent.How"/>).</summary>
public enum DiscardKind
{
    /// <summary>The inventory's destroy action (dragging a stack out / the destroy button).</summary>
    Destroy = 1,
    /// <summary>The item menu's Trash.</summary>
    Trash = 2,
}

/// <summary>A player is about to discard an item stack from an inventory (Forge: ItemTossEvent). Enshrouded has no
/// world drop: discarding destroys the stack. Pre phase, read from the player's input before the game's
/// <c>inventory_actions</c> handles it. Cancel: the action is marked as handled, the game never runs it and the item
/// stays in the slot.</summary>
public sealed class ItemTossEvent : GameplayCancelableEvent
{
    /// <summary>The player who discards the stack.</summary>
    public EntityId Player { get; internal init; }
    /// <summary>How the player discards it.</summary>
    public DiscardKind How { get; internal init; }
    /// <summary>The inventory entity of the slot (a player's bag, a chest's inventory link).</summary>
    public EntityId Inventory { get; internal init; }
    /// <summary>The index of the slot in <see cref="Inventory"/>.</summary>
    public int SlotIndex { get; internal init; }
    /// <summary>The game's item id in that slot (Kiln.Items <c>ItemId</c>); 0 if the slot could not be read.</summary>
    public uint ItemId { get; internal init; }
    /// <summary>How many items the stack holds.</summary>
    public int Count { get; internal init; }
    /// <summary>The stack's own entity (gear with level, perks, durability), or None.</summary>
    public EntityId ItemEntity { get; internal init; }
    /// <summary>Where the player is; null when it has no position.</summary>
    public WorldPosition? PlayerPosition { get; internal init; }
}
