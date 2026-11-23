# Spawn (Kiln.Spawn)

## CleanupResult — What `Spawn.CleanupTracked` did.
- `CleanupResult()`
- `CleanupResult(int Destroyed, int Missing, int Kept)`
- `Destroyed { get; init; } -> int`
- `Kept { get; init; } -> int`
- `Missing { get; init; } -> int`

## EntityDestroyedEvent — Kiln.Spawn destroyed an entity: `Spawn.Destroy` (`EntityDestroyedEvent.Reason` "destroy") or `Spawn.CleanupTracked` ("cleanup").
- `Entity { get; } -> EntityId`
- `Origin { get; } -> string` — Id of the mod that destroyed it.
- `Position { get; } -> WorldPosition?` — Where it was just before (null when unknown).
- `Reason { get; } -> string`
- `Template { get; } -> Guid?`
- `TemplateName { get; } -> string?`

## EntityInfo — An entity's template and transform at the moment of the call.
- `EntityInfo(EntityId Entity, Guid? Template, string? TemplateName, WorldPosition Position, float YawDegrees, bool IsTracked, ulong Serial, long? SpawnTime, EntityId Parent)`
- `Entity { get; init; } -> EntityId`
- `IsTracked { get; init; } -> bool`
- `Parent { get; init; } -> EntityId`
- `Position { get; init; } -> WorldPosition`
- `Serial { get; init; } -> ulong`
- `SpawnTime { get; init; } -> long?`
- `Template { get; init; } -> Guid?`
- `TemplateName { get; init; } -> string?`
- `YawDegrees { get; init; } -> float`

## EntitySpawnedEvent — Kiln.Spawn created an entity (for any mod).
- `Amount { get; } -> int`
- `Entity { get; } -> EntityId`
- `ItemId { get; } -> uint` — Item pickups: the game's item id and the amount.
- `Kind { get; } -> SpawnedKind`
- `Origin { get; } -> string` — Id of the mod that spawned it ("kiln" outside mod code).
- `Position { get; } -> WorldPosition`
- `Tag { get; } -> string?`
- `Template { get; } -> Guid?` — The template GUID when spawned by GUID; null when spawned by name (see `EntitySpawnedEvent.TemplateName`).
- `TemplateName { get; } -> string?` — The name given to the spawn call, when spawned by name.

## Spawn — Creates and destroys server-world entities from templates (creatures, item drops, props), and keeps a per-world list of what Kiln spawned so it can be removed again...
- `static Available { get; } -> bool` — True when the native service is loaded and usable on this game build (capability `kiln.spawn`; `Capabilities.Why`...
- `static CleanupTracked(string? tag = null, string? origin = null) -> CleanupResult` — Destroys tracked entities (all, or those with `tag` / spawned by `origin`) and drops records whose entity is gone.
- `static Destroy(EntityId entity) -> bool` — Destroys a non-player entity now (and drops its tracked record).
- `static DestroyIfAlive(EntityRef entity) -> bool` `0.2.2` — Destroys the entity `entity` was captured from, now (and drops its tracked record).
- `static Entity(Guid template, WorldPosition position, float yawDegrees = 0, SpawnOptions? options = null) -> Task<EntityId>` — Spawns a template by GUID (see `TemplateInfo.Id`).
- `static Entity(string template, WorldPosition position, float yawDegrees = 0, SpawnOptions? options = null) -> Task<EntityId>` — Spawns a template by name ("Enemy_Vukah_Melee"), display name ("Vukah") or GUID text at `position` (metres; placed...
- `static EntityCount { get; } -> int` — Number of entities in the server world right now.
- `static Exists(EntityId entity) -> bool` — Whether the entity exists right now.
- `static Find(Guid template, WorldPosition? near = null, double radius = 0, int max = 256) -> IReadOnlyList<EntityId>` — Entities of a template within `radius` metres of `near` (null: the whole world), nearest first.
- `static FindByName(string nameContains, WorldPosition? near = null, double radius = 0, int max = 256) -> IReadOnlyList<EntityId>` — Entities whose template name contains `nameContains` (case-insensitive).
- `static Group(string template, WorldPosition center, int count, double spread = 4, SpawnOptions? options = null) -> Task<IReadOnlyList<EntityId>>` — Spawns `count` (1..50) copies on a circle of radius `spread` around `center` (the admin "spawn N enemies near a...
- `static Inspect(EntityId entity) -> EntityInfo?` — The entity's template and transform, or null when it does not exist.
- `static Item(uint itemId, int amount, WorldPosition position, int level = 0, SpawnOptions? options = null) -> Task<EntityId>` — Drops `amount` (1..65535) of an item (the game's ItemId, e.g. from kiln.items) as a loose pickup at `position`: the...
- `static Lookup(string name) -> TemplateInfo?` — A template by name, display name or GUID text; null if unknown.
- `static Refresh() -> int` — Re-checks the tracked list against the world now; returns how many are live.
- `static SupportsComponents { get; } -> bool` — True when this kiln.spawn honours `SpawnOptions.Components` (capability `spawn.components`).
- `static Template(Guid id) -> TemplateInfo?` — A template by GUID; null if unknown.
- `static Templates { get; } -> IReadOnlyList<TemplateInfo>` — Every known template (the game's loaded templates plus Kiln's catalog), sorted by name.
- `static Tracked { get; } -> IReadOnlyList<TrackedEntity>` — What Kiln spawned in this world and has not removed yet (live or not found right now).
- `static TryEntity(Guid template, WorldPosition position, float yawDegrees = 0, SpawnOptions? options = null) -> SpawnResult` `0.2.2` — Spawns a template by GUID like `Spawn.Entity`, with the answer as a result (see `Spawn.TryEntity`).
- `static TryEntity(string template, WorldPosition position, float yawDegrees = 0, SpawnOptions? options = null) -> SpawnResult` `0.2.2` — Spawns a template like `Spawn.Entity` and reports the engine's refusal as a result instead of a `SpawnException`...
- `static TryItem(ItemId item, int amount, WorldPosition position, int level = 0, SpawnOptions? options = null) -> SpawnResult` `0.2.2` — Drops an item like `Spawn.Item`, named by its `ItemId`, with the answer as a result (see `Spawn.TryEntity`).
- `static WorldKey { get; } -> string?` — The world the tracked list belongs to (the world GUID from the save), or null.
- `static WorldRunning { get; } -> bool` — The question for the row `spawn.world`: true once the server world runs (its save is loaded).

## SpawnComponent — An extra component for `SpawnOptions.Components`: the engine copies `SpawnComponent.Data` into the new entity as component `SpawnComponent.Hash` (overriding the...
- `SpawnComponent(uint Hash, byte[] Data)`
- `Data { get; init; } -> byte[]` — The component's bytes; the length must equal the engine's component size, else the engine refuses the spawn...
- `Hash { get; init; } -> uint` — The component's qualified hash, FNV-1a-32 of `"keen::ecs::<Name>"` (`SpawnComponent.Of` computes it).
- `static HashOf(string name) -> uint` — FNV-1a-32 of `"keen::ecs::<name>"` (a name with "::" is hashed as given).
- `static Of(string name, byte[] data) -> SpawnComponent` — A component by name (`"Faction"` or `"keen::ecs::Faction"`).
- `static Of<T>(string name, T value) -> SpawnComponent` — A component from an unmanaged value (its bytes, e.g. a struct that mirrors the engine's layout).

## SpawnedKind
enum: Entity, Item

## SpawnException — The game (or Kiln) refused a spawn; `SpawnException.Code` is the native result code.
- `SpawnException(string message, int code)`
- `Code { get; } -> int`

## SpawnFlags — Spawn behaviour switches.
enum: None, NoTrack, Transient, AllowUnknown

## SpawnOptions — Optional spawn settings.
- `Components { get; init; } -> IReadOnlyList<SpawnComponent>?` — Extra components given to the engine with the spawn (at most 64; one value per component).
- `Flags { get; init; } -> SpawnFlags`
- `Level { get; init; } -> int` — > 0: the entity's level (OverrideLevel).
- `Tag { get; init; } -> string?` — Label stored with the tracked record (cleanup can select by it), e.g. "admin" or "raids".

## SpawnResult `0.2.2` — The answer to `Spawn.TryEntity` and `Spawn.TryItem`: the new entity, or why the game (or kiln.spawn) refused.
- `SpawnResult(bool Ok, EntityRef Entity, string? Reason, int Code)`
- `Code { get; init; } -> int` — The native result code: 0 when `SpawnResult.Ok`, else the code a `SpawnException` would carry (2 unknown template, 1...
- `Entity { get; init; } -> EntityRef` — The new entity with its serial, so a stale id is noticed later (`EntityRef.IsAlive`); `EntityRef.None` when refused.
- `Ok { get; init; } -> bool` — True when the entity was created.
- `Reason { get; init; } -> string?` — Null when `SpawnResult.Ok`; else why: an unknown template, an invalid position or request, or the engine's refusal...

## TemplateInfo — A spawnable template: GUID (game byte order = `Guid.ToByteArray`), name and kind.
- `TemplateInfo(Guid Id, string Name, string DisplayName, TemplateKind Kind, bool Shipped, bool LoadedByGame, bool? NeverSaved = null, bool Expires = false)`
- `DisplayName { get; init; } -> string` — In-game name from Kiln's catalog ("Vukah"), or "".
- `Expires { get; init; } -> bool` — The template has a LifeTime: the game removes its entities after a while.
- `Id { get; init; } -> Guid` — The template GUID.
- `Kind { get; init; } -> TemplateKind` — What it is, judged from the name.
- `LoadedByGame { get; init; } -> bool` — Loaded by the running server (spawnable).
- `Name { get; init; } -> string` — The engine's template name ("Enemy_Vukah_Melee").
- `NeverSaved { get; init; } -> bool?` — The template carries the game's DoNotSave tag: its entities never reach the save (null: unknown on this build).
- `Shipped { get; init; } -> bool` — Listed in Kiln's shipped catalog.

## TemplateKind — What a template is, judged from its name.
enum: Unknown, Creature, Pickup, Prop, Npc, Other

## TrackedEntity — An entity Kiln spawned in this world (kept in `kiln/data/spawn/<world>.json`).
- `TrackedEntity(ulong Serial, EntityId Entity, Guid Template, string TemplateName, WorldPosition Position, WorldPosition SpawnPosition, DateTimeOffset SpawnedAt, bool IsLive, SpawnFlags Flags, string Tag, string Origin)`
- `Entity { get; init; } -> EntityId`
- `Flags { get; init; } -> SpawnFlags`
- `IsLive { get; init; } -> bool`
- `Origin { get; init; } -> string`
- `Position { get; init; } -> WorldPosition`
- `Serial { get; init; } -> ulong`
- `SpawnedAt { get; init; } -> DateTimeOffset`
- `SpawnPosition { get; init; } -> WorldPosition`
- `Tag { get; init; } -> string`
- `Template { get; init; } -> Guid`
- `TemplateName { get; init; } -> string`
