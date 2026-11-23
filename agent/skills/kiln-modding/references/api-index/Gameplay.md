# Gameplay (Kiln.Gameplay)

## BuildEvent — A building or terraforming operation of this step (Forge's BlockEvent.EntityPlaceEvent / BreakEvent).
- `DeniedByZone { get; } -> uint` — The id of the zone that refused it (0 = none).
- `IsPlacement { get; } -> bool` — True for placing a block or a prop.
- `IsRemoval { get; } -> bool` — True for dismantling a block, picking up a prop, or digging terrain (terraform type 0).
- `ItemId { get; } -> uint` — Block / prop item (place), material item (dismantle); 0 for terraforming.
- `Kind { get; } -> BuildKind` — What the operation was.
- `Outcome { get; } -> BuildOutcome` — How it ended.
- `Player { get; } -> EntityId` — The player who did it.
- `Position { get; } -> WorldPosition` — Where it happened.
- `Target { get; } -> EntityId` — The prop entity (`BuildKind.PickUpProp`).
- `TerraformType { get; } -> int` — Terraform: 0 remove (dig), 1 add, 2-3 flatten, 4-5 overgrowth, 6-7 decay.
- `ZoneName { get; } -> string` — The name of that zone (empty when none or unknown).

## BuildKind — What a `BuildEvent` is about.
enum: PlaceBlock, PlaceProp, Dismantle, PickUpProp, Terraform

## BuildOutcome — How a building operation ended.
enum: Done, RefusedByGame, DeniedByZone

## CraftEvent — A craft request the game handles in this step (a player's craft at a station or by hand, or a mod's request).
- `Amount { get; } -> int` — How many times the recipe is crafted.
- `ByPlayer { get; } -> bool` — True when the operator is a player.
- `Operator { get; } -> EntityId` — Whose inventory pays and receives: a player, a chest, a villager.
- `RecipeId { get; } -> uint` — The recipe being crafted.
- `Station { get; } -> EntityId` — The entity with `Crafting`: a workbench, a villager, or the player itself (hand crafting).
- `StationPosition { get; } -> WorldPosition?` — Where the station is; null when it has no position.

## CraftResult — The game's result codes of one craft (the craft core).
enum: Other, Crafted, NoRoomForOutput, MissingMaterials, MissingCraftingProps

## DiscardKind — How a player discarded a stack (`ItemTossEvent.How`).
enum: Destroy, Trash

## EntityCategories — Kinds of entities for `EntityJoinLevelEvent` / `EntityLeaveLevelEvent` (from the template name, like `TemplateKind`).
enum: None, Player, Creature, Npc, Prop, Pickup, Other, All, Default

## EntityJoinLevelEvent — An entity was created in the server world (Forge: EntityJoinLevelEvent), by any path: spawners, NPC logic, loot, child entities, a player joining, the save loading...
- `ByKiln { get; } -> bool`
- `Category { get; } -> EntityCategories`
- `Entity { get; } -> EntityId`
- `IsPlayer { get; } -> bool` — True when the entity is a player.
- `Parent { get; } -> EntityId`
- `Position { get; } -> WorldPosition?`
- `Template { get; } -> Guid?`
- `TemplateInfo { get; } -> TemplateInfo?`
- `TemplateName { get; } -> string`

## EntityLeaveLevelEvent — An entity was destroyed (Forge: EntityLeaveLevelEvent): LifeTime ran out, the game culled or despawned it, its loot was taken, a queued destroy, `Spawn.Destroy`.
- `ByKiln { get; } -> bool`
- `Category { get; } -> EntityCategories`
- `Entity { get; } -> EntityId`
- `IsPlayer { get; } -> bool` — True when the entity was a player.
- `Parent { get; } -> EntityId`
- `Position { get; } -> WorldPosition?`
- `Template { get; } -> Guid?`
- `TemplateInfo { get; } -> TemplateInfo?`
- `TemplateName { get; } -> string`

## Gameplay — kiln.gameplay: player/world interaction events and protection zones.
- `static Available { get; } -> bool` — True when the native service is loaded on this game build (capability `kiln.gameplay`).
- `static Enable() -> bool` — Makes sure the native service delivers events to the bus.
- `static EntityEventsAvailable { get; } -> bool` — True when this build can provide entity lifecycle events (capability `gameplay.entity_events`).
- `static IsDenied(EntityId player, WorldPosition position, GameplayActions action, out uint zoneId) -> bool` — Would `action` by `player` at `position` be denied by a zone?
- `static Protect(ProtectionZone zone) -> ZoneHandle` — Adds a protection zone, owned by the calling mod (removed when it unloads).
- `static Recipe(uint id) -> GameplayRecipe?` — A recipe by id, or null.
- `static ReplaceNaturalSpawns(Guid from, Guid to, double chance = 1) -> IDisposable` — Natural spawns of `from` spawn `to` instead, with probability `chance` (0..1): decided inside the random spawner's...
- `static Status { get; } -> GameplayStatus?` — The service's state, or null when the native service is not loaded.
- `static WatchEntities(EntityCategories categories) -> IDisposable` — Which entities `EntityJoinLevelEvent` / `EntityLeaveLevelEvent` report: the union of every mod's requests...
- `static WatchTeleports(EntityId entity, bool watch = true) -> void` — Adds (or removes) a non-player entity to the teleport watch list, so `TeleportEvent` and `TeleportedEvent` fire for it.
- `static WorkshopRecipes(uint workshop) -> IReadOnlyList<uint>` — The recipe ids a workshop (a station kind; 0 = hand crafting) offers.
- `static Zones { get; } -> IReadOnlyList<ZoneHandle>` — Zones registered through this assembly.

## Gameplay.Inject — Sends the game's own request events from Kiln's ECS program (next step), as a client or villager would: the game validates them as usual, and they pass through the...
- `static Craft(EntityId operatorEntity, EntityId station, uint recipeId, int amount = 1) -> void` — A CraftRecipeEvent: `operatorEntity` pays and receives (a player, or a chest with an inventory), `station` has...
- `static Interact(EntityId guest, EntityId host, uint offerId) -> void` — An AcceptOfferedInteractionEvent (`offerId` = the host's current offer).
- `static Loot(EntityId guest, EntityId host, uint offerId = 0, bool lootAll = true, bool bypassInteraction = true) -> void` — A LootInteractionEvent.

## GameplayActions — Actions a protection zone can deny (and the action an event is about).
enum: None, Build, Break, Terraform, Interact, Loot, Craft, Spawn, All

## GameplayCancelableEvent — Base of the cancelable gameplay events: a protection zone may have canceled the event before any handler ran (`GameplayCancelableEvent.DeniedByZone` != 0...
- `DeniedByZone { get; } -> uint` — Id of the zone that denied this (0 = none).
- `Injected { get; } -> bool` — Sent by Kiln (`Inject`), not by the game.
- `Step { get; } -> ulong` — Server-world step.
- `ZoneName { get; } -> string` — Name of that zone ("" when none or unknown).

## GameplayEvent — Base of the gameplay notifications.
- `Step { get; } -> ulong` — The server-world step the event was posted in.

## GameplayItemStack — A stack of items (item id as the game's `ItemId`, see Kiln.Items).
- `GameplayItemStack()`
- `GameplayItemStack(uint ItemId, int Count)`
- `Count { get; init; } -> int` — How many.
- `ItemId { get; init; } -> uint` — The item's id (the game's `ItemId`); 0 in a recipe's inputs means an input category.

## GameplayRecipe — A recipe of the game's recipe registry.
- `GameplayRecipe(uint Id, uint Workshop, IReadOnlyList<GameplayItemStack> Inputs, IReadOnlyList<GameplayItemStack> Outputs, TimeSpan Duration, ulong RequiredProps)`
- `Duration { get; init; } -> TimeSpan` — How long a craft takes.
- `HasCategoryInputs { get; } -> bool` — True when an input is a category (item 0: "any wood") rather than one item.
- `Id { get; init; } -> uint` — The recipe's id.
- `Inputs { get; init; } -> IReadOnlyList<GameplayItemStack>` — What a craft takes.
- `Outputs { get; init; } -> IReadOnlyList<GameplayItemStack>` — What a craft makes.
- `RequiredProps { get; init; } -> ulong` — A bit mask of the crafting props that must be near the station (a bit per prop in the game's table of crafting props).
- `Workshop { get; init; } -> uint` — The id of the workshop (station) that crafts it.

## GameplayStatus — Native status of kiln.gameplay.
- `GameplayStatus(bool Intercept, bool Inject, bool CraftHook, bool BuildHooks, bool TerraformHook, bool SpawnHook, bool Teleports, bool WorldRunning, int Zones, int Subscribers, ulong Steps, ulong EventsSeen, ulong Dispatched, ulong Canceled, ulong Denied, ulong HookRecords)`
- `BuildHooks { get; init; } -> bool` — True when the building core is hooked.
- `Canceled { get; init; } -> ulong` — How many Pre events were neutralized.
- `CraftHook { get; init; } -> bool` — True when the craft core is hooked.
- `Denied { get; init; } -> ulong` — How many hook operations zones refused.
- `Discard { get; init; } -> bool` — Discard actions (`ItemTossEvent`) are read on this build.
- `DiscardsCanceled { get; init; } -> ulong` — Discard actions canceled so far.
- `DiscardsSeen { get; init; } -> ulong` — Discard actions read so far.
- `Dispatched { get; init; } -> ulong` — How many events were handed to subscribers.
- `EntitiesFiltered { get; init; } -> ulong` — Creations/destructions the hooks saw outside the categories.
- `EntitiesJoined { get; init; } -> ulong` — Entity creations reported so far.
- `EntitiesLeft { get; init; } -> ulong` — Entity destructions reported so far.
- `EntityCategories { get; init; } -> EntityCategories` — Categories reported now.
- `EntityHooks { get; init; } -> bool` — createEntity / destroyEntity are hooked (entity lifecycle events work).
- `EventsSeen { get; init; } -> ulong` — How many pending records were read (all types).
- `HookRecords { get; init; } -> ulong` — How many hook records were reported in Post.
- `Inject { get; init; } -> bool` — True when `Inject` works: Kiln's program is registered and the game's emitter was found.
- `Intercept { get; init; } -> bool` — True when pending events are read in Pre.
- `SpawnHook { get; init; } -> bool` — True when the random spawners' spawn is hooked.
- `Steps { get; init; } -> ulong` — How many Pre passes read pending events.
- `Subscribers { get; init; } -> int` — How many event subscriptions there are.
- `Teleports { get; init; } -> bool` — True when Teleport components are watched.
- `TerraformHook { get; init; } -> bool` — True when the terraforming impact is hooked.
- `WorldRunning { get; init; } -> bool` — True when a server world is stepping.
- `Zones { get; init; } -> int` — How many protection zones are registered.

## IEntityLifecycleEvent — Fields shared by `EntityJoinLevelEvent` and `EntityLeaveLevelEvent`.
- `ByKiln { get; } -> bool` — Created / destroyed outside the game's sim step: by Kiln (Kiln.Spawn, a service, a mod's Pre/Post code), not by a...
- `Category { get; } -> EntityCategories` — The entity's category.
- `Entity { get; } -> EntityId` — The entity that joined or left the world.
- `Parent { get; } -> EntityId` — The entity this one belongs to (a creature for its weakspot), or None.
- `Position { get; } -> WorldPosition?` — Where it was (metres); null when it has no transform.
- `Template { get; } -> Guid?` — Template GUID (null for entities without one).
- `TemplateInfo { get; } -> TemplateInfo?` — Kiln.Spawn's template record (name, display name, kind), or null.
- `TemplateName { get; } -> string` — The engine's template name ("Enemy_Vukah_Melee"), or "".

## InteractEvent — Someone accepts an interaction offer: E on a chest, door, workstation, bed, portal, NPC (villagers send it too).
- `ByPlayer { get; } -> bool` — True when the guest is a player.
- `Guest { get; } -> EntityId` — Who accepts the offer: a player or a villager.
- `Host { get; } -> EntityId` — The entity whose offer is accepted: a chest, a door, a station, a bed, a portal, an NPC.
- `HostPosition { get; } -> WorldPosition?` — Where the host is; null when it has no position.
- `OfferId { get; } -> uint` — The id of the offer that is accepted.

## InteractionResultEvent — The game's answer to an interaction attempt of the previous step (Pre phase): accepted, or declined (including the ones Kiln declined).
- `Accepted { get; } -> bool` — True when the game accepted the attempt; false when it was declined.
- `Guest { get; } -> EntityId` — Who tried to interact.
- `Host { get; } -> EntityId` — The entity they tried to interact with.
- `OfferId { get; } -> uint` — The id of the offer.

## ItemCraftedEvent — The game ran its craft core once (one unit of a craft request, or an automated station's cycle).
- `ByPlayer { get; } -> bool` — True when the operator is a player.
- `Operator { get; } -> EntityId` — Whose inventory paid and received.
- `Outputs { get; } -> IReadOnlyList<GameplayItemStack>` — The recipe's outputs for one craft (what arrived when `ItemCraftedEvent.Succeeded`).
- `RawResult { get; } -> int` — The game's raw result code; 0 is success.
- `RecipeId { get; } -> uint` — The recipe that was crafted.
- `Result { get; } -> CraftResult` — The game's result code, as a named value.
- `Station { get; } -> EntityId` — The entity that did the crafting: a workbench, a villager, or the player (hand crafting).
- `StationPosition { get; } -> WorldPosition?` — Where the station is; null when it has no position.
- `Succeeded { get; } -> bool` — True when the craft went through (`ItemCraftedEvent.RawResult` is 0).
- `WorkshopId { get; } -> uint` — The id of the workshop the recipe belongs to.

## ItemPickingUpEvent `0.2.4` — The game is about to give a receiver an item it walked over (an automatic pickup in range, not an interaction: interaction loot is `LootEvent`).
- `ItemPickingUpEvent(DecisionId id, EventEntityKey receiver, EventEntityKey ground, ItemId item, int amount, WorldPosition? position, ulong exposureEpoch)` — Makes the event (Kiln posts it; a mod has no reason to).
- `Amount { get; } -> int` — The drop's raw amount (read only).
- `ExposureEpoch { get; } -> ulong` — Kiln's number of this sampled exposure (unique in the process): a new one after the receiver left and came back...
- `GroundEntity { get; } -> EntityId` — The item on the ground (its drop entity).
- `GroundKey { get; } -> EventEntityKey` — Which life of the item on the ground.
- `Id { get; } -> DecisionId` — The decision; the same id is on its `DecisionCompletedEvent`.
- `Involves(EntityId entity) -> bool` — Whether the event is about `entity` (the receiver or the item on the ground).
- `Item { get; } -> ItemId` — The item.
- `Position { get; } -> WorldPosition?` — Where the drop was when the step began; null when it had no position then.
- `Receiver { get; } -> EntityId` — The entity that would get the item (a player, or a character standing in for one).
- `ReceiverKey { get; } -> EventEntityKey` — Which life of the receiver.

## ItemPickupEvent — Items arrived in an inventory in the previous step (the game's PickupEvent: the "+N" popup).
- `Count { get; } -> int` — How many arrived.
- `HiddenInUi { get; } -> bool` — The game's flag for a pickup that shows no popup.
- `ItemId { get; } -> uint` — The item that arrived (the game's `ItemId`).
- `Position { get; } -> WorldPosition` — Where the pickup happened.
- `Receiver { get; } -> EntityId` — The inventory owner the items arrived with.
- `SlotIndex { get; } -> int` — The index of the slot they went to, in `ItemPickupEvent.SlotInventory`.
- `SlotInventory { get; } -> EntityId` — The 8-slot inventory entity the items went to, and the slot index.
- `Source { get; } -> EntityId` — Where the items came from (none for crafting output).

## ItemTossEvent — A player is about to discard an item stack from an inventory (Forge: ItemTossEvent).
- `Count { get; } -> int` — How many items the stack holds.
- `How { get; } -> DiscardKind` — How the player discards it.
- `Inventory { get; } -> EntityId` — The inventory entity of the slot (a player's bag, a chest's inventory link).
- `ItemEntity { get; } -> EntityId` — The stack's own entity (gear with level, perks, durability), or None.
- `ItemId { get; } -> uint` — The game's item id in that slot (Kiln.Items `ItemId`); 0 if the slot could not be read.
- `Player { get; } -> EntityId` — The player who discards the stack.
- `PlayerPosition { get; } -> WorldPosition?` — Where the player is; null when it has no position.
- `SlotIndex { get; } -> int` — The index of the slot in `ItemTossEvent.Inventory`.

## LootEvent — A loot interaction: take an item lying in the world, loot a container all at once, harvest a plant, gather produce (Forge's EntityItemPickupEvent).
- `BypassInteraction { get; } -> bool` — The game's `bypassInteraction` flag of the request.
- `ByPlayer { get; } -> bool` — True when the guest is a player.
- `Guest { get; } -> EntityId` — Who receives: a player, or a chest / villager for mod-sent loot.
- `Host { get; } -> EntityId` — What is looted: the item pickup, container, plant, animal.
- `HostPosition { get; } -> WorldPosition?` — Where the looted entity is; null when it has no position.
- `LootAll { get; } -> bool` — The game's `lootAll` flag of the request.
- `OfferId { get; } -> uint` — The id of the loot offer.

## NaturalSpawnEvent — A random spawner (the world's natural enemy and wildlife spawns, which run near players) created a creature this step (Forge's LivingSpawnEvent.CheckSpawn).
- `Entity { get; } -> EntityId` — The creature the spawner made; none for a spawn that a zone refused.
- `Position { get; } -> WorldPosition` — Where it spawned.
- `Replaced { get; } -> bool` — A `Gameplay.ReplaceNaturalSpawns` rule swapped the spawner's template: `NaturalSpawnEvent.Template` is the replacement.
- `Template { get; } -> Guid` — The GUID of the creature's template.

## OfferAccepted — Someone accepted an offer of yours.
- `OfferAccepted()`
- `OfferAccepted(EntityId Player, EntityId Entity)`
- `Entity { get; init; } -> EntityId` — The entity the offer is on.
- `Player { get; init; } -> EntityId` — The player who accepted the offer.

## OfferOptions — Optional settings of an offer.
- `BuildMode { get; init; } -> bool` — Usable in build mode too (allowedAsBuildingSpark).
- `LowPriority { get; init; } -> bool` — Other offers nearby win the prompt (the game's reduceInteractionWeight).
- `RangeMeters { get; init; } -> byte` — How close the player must be, in metres (1..255); 0 = 32.
- `Sequence { get; init; } -> uint` — The interaction sequence (`OfferSequences`); 0 = Touch.

## Offers — Interaction offers on any entity: the prompt a player accepts with E, and your handler when they do.
- `static Add(EntityId entity, string? text, uint verb, Action<OfferAccepted> handler, OfferOptions? options = null) -> void` — Puts an offer on `entity`: players near it see a prompt; `handler` runs (tick thread, as your mod) each time one...
- `static Available { get; } -> bool` — True when offers work on this server (kiln.gameplay's component_change and kiln.world).
- `static CustomVerbId(string text) -> uint` — The prompt-word id `Offers.Add` uses for a custom text (EMF's: FNV-1a-32 of "emf.verb:" + text, UTF-8).
- `static IsActive(EntityId entity) -> bool` — The calling mod has an offer on `entity` (pending or active; EMF's offer_active).
- `static List() -> IReadOnlyList<(EntityId Entity, string Owner, uint Verb, long Accepts)>` — The offers of every mod: entity, owner mod id ("kiln" for none), verb id, accepts (for admin tools).
- `static Remove(EntityId entity) -> bool` — Takes your offer off `entity`: Kiln removes only the components it added (within a step or two).
- `static SpawnWithOffer(string template, WorldPosition position, string? text, uint verb, Action<OfferAccepted> handler, OfferOptions? options = null, SpawnOptions? spawn = null) -> Task<EntityId>` — Spawns `template` (as `Spawn.Entity`) and puts an offer on it.
- `static Status(EntityId entity) -> OfferStatus` — Where the calling mod's offer on `entity` stands.
- `static TryAdd(EntityId entity, string? text, uint verb, Action<OfferAccepted> handler, out string reason, OfferOptions? options = null) -> bool` — `Offers.Add` that returns false and the reason instead of throwing.

## OfferSequences — What the player plays when accepting (interaction sequences).
- `const ButtonPress = 2942390232 -> uint` — The button press sequence.
- `const Touch = 1045531071 -> uint` — A short touch, no screen (the default).

## OfferStatus — Where an offer stands.
enum: None, Pending, Active, Removing

## OfferVerbs — Prompt words (loca tag ids of the game) for `Offers.Add`.
- `const Activate = 2166119838 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Collect = 2425743549 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Enter = 2019553657 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Inspect = 638032107 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Open = 204358933 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Pet = 1834705559 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Press = 1865818160 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Read = 1662597084 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Release = 2928998250 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Rest = 1660103147 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Sit = 3753371512 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Sleep = 3303076299 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Take = 870679286 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Talk = 3649444939 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Touch = 413737516 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...
- `const Use = 641956883 -> uint` — The loca tag id of the game's prompt word with this field's name (Use, Open, Talk, Read, Sit, Sleep, Pet, Touch...

## ProtectionZone — A protected area: the listed actions are refused to every player who is not allowed.
- `AllowAdmins { get; init; } -> bool` — Players with the game's admin bit are allowed too.
- `AllowedSteamIds { get; init; } -> IReadOnlyCollection<ulong>` — SteamID64s of the players allowed in the zone (the owners).
- `Center { get; init; } -> WorldPosition` — The centre of a sphere or a cylinder.
- `Contains(WorldPosition p) -> bool` — True when `p` is inside the zone's shape.
- `Deny { get; init; } -> GameplayActions` — The actions refused to players who are not allowed in the zone (default: build, break, terraform, interact, loot).
- `Max { get; init; } -> WorldPosition` — The highest corner of a box (the highest height of a cylinder).
- `MaxY { get; init; } -> double` — The top of a cylinder (sets Max.Y); see `ProtectionZone.MinY`.
- `Min { get; init; } -> WorldPosition` — The lowest corner of a box (the lowest height of a cylinder).
- `MinY { get; init; } -> double` — Cylinder height range (sets Min.Y / Max.Y).
- `Name { get; init; } -> string` — The zone's name, as `GameplayCancelableEvent.ZoneName` reports it.
- `Radius { get; init; } -> double` — The radius of a sphere or a cylinder, in metres (default 16).
- `Shape { get; init; } -> ZoneShape` — The zone's shape (default `ZoneShape.Sphere`).

## TeleportedEvent — A watched entity was teleported this step (its TeleportCount changed).
- `Count { get; } -> int` — The entity's teleport count after this teleport.
- `Entity { get; } -> EntityId` — The entity that was teleported.
- `From { get; } -> WorldPosition` — Where it was before.
- `To { get; } -> WorldPosition` — Where it is now.

## TeleportEvent — A watched entity (every player; others with `Gameplay.WatchTeleports`) is about to be teleported by the game: fast travel, respawn, portal, admin teleport, kiln.world.
- `Entity { get; } -> EntityId` — The entity that is about to be teleported.
- `From { get; } -> WorldPosition` — Where it is now.
- `IsPlayer { get; } -> bool` — True when the entity is a player.
- `SearchesGround { get; } -> bool` — The game searches walkable ground near the destination.
- `Target { get; set; } -> WorldPosition` — The destination.
- `TargetEntity { get; } -> EntityId` — The spawn point / flame altar it travels to (none for a plain position).

## ZoneHandle — A registered zone.
- `Active { get; } -> bool` — True until the zone is removed.
- `Dispose() -> void` — Removes the zone.
- `Id { get; } -> uint` — The zone's id, as `GameplayCancelableEvent.DeniedByZone` reports it.
- `Update(ProtectionZone zone) -> void` — Replaces the zone's shape, actions and owners.
- `Zone { get; } -> ProtectionZone` — The zone as it is registered now (`ZoneHandle.Update` replaces it).

## ZoneShape — Shape of a `ProtectionZone`.
enum: Sphere, Cylinder, Box
