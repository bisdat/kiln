# Game (Kiln.Game)

## Ecs — The game's ECS types, generated from its reflection table: `Ecs.Health`, `Ecs.NetworkHealth`, `Ecs.Enemy01_State`.
- `static All<T>() -> IReadOnlyList<EntityId>` — Every entity carrying component `T`; empty when `T` is not supported on the running game.
- `static Available { get; } -> bool` — True when kiln.world is available on this build, so the typed reads below can work.
- `static Get<T>(EntityId entity) -> T?` — The component, or null when the entity does not have it or `T` is not supported on the running game.
- `static Has<T>(EntityId entity) -> bool` — True if the entity has component `T`; false too when `T` is not supported on the running game.
- `static Supports<T>() -> bool` — The first-use check's answer for `T`: true when its generated layout equals the running game's (the game's type...
- `static TryGet<T>(EntityId entity, out T value) -> bool` — Reads component `T` of `entity`.
- `static WhyNot<T>() -> string?` — Null when `T` is supported (`Ecs.Supports`), else the difference: the type, the field or value, the generated and...

## Ecs.GameGuid — A game guid (reflection kind 29, `keen::Guid`): 16 bytes.
- `GameGuid()` — A guid from its 16 bytes, in memory order.
- `GameGuid(ReadOnlySpan<byte> bytes)` — A guid from its 16 bytes, in memory order.
- `CopyTo(Span<byte> destination) -> void` — Copies the 16 bytes, in memory order, into `destination`.
- `Equals(Ecs.GameGuid other) -> bool`
- `IsEmpty { get; } -> bool` — True when all 16 bytes are zero.
- `static operator !=(Ecs.GameGuid left, Ecs.GameGuid right) -> bool` — True when the guids differ.
- `static operator ==(Ecs.GameGuid left, Ecs.GameGuid right) -> bool` — True when the guids are equal.

## Ecs.ObjectReference — A game object reference (reflection kind 28): the 16-byte guid of the object it names.
- `ObjectReference()` — A reference from its 16 bytes, in memory order.
- `ObjectReference(ReadOnlySpan<byte> bytes)` — A reference from its 16 bytes, in memory order.
- `CopyTo(Span<byte> destination) -> void` — Copies the 16 bytes, in memory order, into `destination`.
- `Equals(Ecs.ObjectReference other) -> bool`
- `IsEmpty { get; } -> bool` — True when all 16 bytes are zero (no object).
- `static operator !=(Ecs.ObjectReference left, Ecs.ObjectReference right) -> bool` — True when they name different objects.
- `static operator ==(Ecs.ObjectReference left, Ecs.ObjectReference right) -> bool` — True when both name the same object.

## GameLayoutStamps — The layouts of the generated game types in this `Kiln.Game.dll`, and the check that tells whether a mod built against other layouts can run against these.
- `static Check(string stampJson, out string? reason) -> bool` — Compares a mod's stamp (the text of its `kiln.gametypes.json`) with this assembly.
- `static Table { get; } -> string` — The table of this assembly, as embedded: one line per generated type with its layout hash and its fields (or enum...

## GameTypes.Components — Typed, name-stable reads of ECS components, using the generated layouts (see Generated.cs) over kiln.world.
- `static All<T>() -> IReadOnlyList<EntityId>` — Every entity carrying component `T`.
- `static Available { get; } -> bool` — True when kiln.world is available (component reads work on this build).
- `static Get<T>(EntityId entity) -> T?` — The component, or null when the entity does not have it.
- `static Has<T>(EntityId entity) -> bool` — True if the entity has component `T`.
- `static NameOf<T>() -> string?` — The ECS component name for a generated type (e.g. "NetworkHealth"), or null if it is not generated.
- `static TryGet<T>(EntityId entity, out T value) -> bool` — Reads component `T` of `entity`.

## GameTypes.ComponentTypes — Type-hash constants for every generated ECS component.
- `const CurrentTransform = 41600356 -> uint`
- `const Health = 3692040337 -> uint`
- `const LastTransform = 1265286393 -> uint`
- `const Mana = 1299327400 -> uint`
- `const NetworkHealth = 560903673 -> uint`
- `const NpcCommon = 120438199 -> uint`
- `const Stamina = 2453735012 -> uint`
- `const TeleportCount = 3221107951 -> uint`
- `const Velocity = 4214432000 -> uint`

## GameTypes.CurrentTransform — ECS component `keen::ecs::CurrentTransform` (56 bytes).
- `const Component = "CurrentTransform" -> string` — The ECS component name used by kiln.world.
- `const Hash = 41600356 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `transform -> byte*`

## GameTypes.Health — ECS component `keen::ecs::Health` (40 bytes).
- `const Component = "Health" -> string` — The ECS component name used by kiln.world.
- `dataStorage -> byte*`
- `const Hash = 3692040337 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).

## GameTypes.LastTransform — ECS component `keen::ecs::LastTransform` (72 bytes).
- `const Component = "LastTransform" -> string` — The ECS component name used by kiln.world.
- `const Hash = 1265286393 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `transform -> byte*`
- `velocity -> byte*`

## GameTypes.Mana — ECS component `keen::ecs::Mana` (40 bytes).
- `const Component = "Mana" -> string` — The ECS component name used by kiln.world.
- `dataStorage -> byte*`
- `const Hash = 1299327400 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).

## GameTypes.NetworkHealth — ECS component `keen::ecs::NetworkHealth` (8 bytes).
- `const Component = "NetworkHealth" -> string` — The ECS component name used by kiln.world.
- `const Hash = 560903673 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `health -> uint`
- `healthMax -> uint`

## GameTypes.NpcCommon — ECS component `keen::ecs::NpcCommon` (1688 bytes).
- `avoidWaterData -> byte*`
- `bedId -> byte*`
- `closestBuildingCursorId -> byte*`
- `closestPlayerDistance -> float`
- `closestPlayerId -> byte*`
- `closestPlayerPosition -> byte*`
- `const Component = "NpcCommon" -> string` — The ECS component name used by kiln.world.
- `currentSequenceId -> byte*`
- `currentStateDuration -> byte*`
- `currentTargetId -> byte*`
- `currentTargetPosition -> byte*`
- `goToTargetTries -> byte`
- `const Hash = 120438199 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `history -> byte*`
- `historyIndex -> byte`
- `interactionStates -> byte`
- `interactionTarget -> byte*`
- `lastStateChange -> byte*`
- `movementMode -> byte`
- `previousTargetId -> byte*`
- `queryId -> byte*`
- `resumeUpdateTimeOffset -> byte*`
- `shouldBeMoving -> byte`
- `sleepTime -> float`
- `squaredDistanceToTarget -> float`
- `stateTimer -> byte*`
- `stuckTime -> byte*`
- `stuckTimeOut -> byte*`
- `targetSearchStates -> byte`
- `taskFailures -> byte`
- `unstuckTries -> byte`
- `useReducedRoamingSpace -> byte`
- `wakeUpTime -> float`
- `yDistanceToTarget -> float`

## GameTypes.Stamina — ECS component `keen::ecs::Stamina` (40 bytes).
- `const Component = "Stamina" -> string` — The ECS component name used by kiln.world.
- `dataStorage -> byte*`
- `const Hash = 2453735012 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).

## GameTypes.TeleportCount — ECS component `keen::ecs::TeleportCount` (16 bytes).
- `const Component = "TeleportCount" -> string` — The ECS component name used by kiln.world.
- `counter -> byte`
- `const Hash = 3221107951 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `lastTeleportTime -> byte*`

## GameTypes.Velocity — ECS component `keen::ecs::Velocity` (12 bytes).
- `const Component = "Velocity" -> string` — The ECS component name used by kiln.world.
- `const Hash = 4214432000 -> uint` — The engine type hash (FNV-1a-32 of the qualified name).
- `velocity -> byte*`
