# Client (Kiln.Client)

## BuildingAction `0.2.13` — What one building input request does (kiln_client.h's `KILN_BUILDING_ACTION_*`).
enum: Select, Place, Remove, Undo, Dismantle

## BuildingInput `0.2.13` — The local player's ordinary building input: select a building item, place, remove, undo, dismantle, emitted through the game's own input path as if the player pressed it.
- `static Available { get; } -> bool` — True when building input works here: the capability row is available and this kiln.client has the members.
- `const Capability = "client.building_input" -> string` — The capability row: available only on a game client whose build has a verified input producer (ready for the first...
- `static Dismantle(EntityId prop, WorldPosition position, Quaternion rotation, Vector3? scale = null, uint expectedSession = 0) -> BuildingInputResult` — Dismantles the exact prop `prop` at the transform (the action is held for 1.2 s).
- `const MaxScale = 1024 -> float` — The largest scale component (each must be in (0, `BuildingInput.MaxScale`]).
- `static Place(WorldPosition position, Quaternion rotation, Vector3? scale = null, uint expectedSession = 0) -> BuildingInputResult` — The primary building action at the transform (the local player places what is selected).
- `const QueueTimeoutMilliseconds = 5000 -> int` — A queued request that the input path has not taken within this many milliseconds ends `BuildingInputState.TimedOut`.
- `static Remove(WorldPosition position, Quaternion rotation, Vector3? scale = null, uint expectedSession = 0) -> BuildingInputResult` — The secondary building action at the transform.
- `static Select(ItemId item, byte slot, ItemId material = default(ItemId), uint expectedSession = 0) -> BuildingInputResult` — Selects `item` (and `material`) in actionbar slot `slot` of the local player.
- `static Undo(uint expectedSession = 0) -> BuildingInputResult` — The game's own building undo.

## BuildingInputRefusal `0.2.13` — Why a building input submit was refused (`BuildingInputResult.Refusal`).
enum: None, Busy, Stale, NoLocalPlayer, ModUnloaded, Rejected

## BuildingInputRequest `0.2.13` — One submitted building input request (`BuildingInput`).
- `Action { get; } -> BuildingAction` — What the request does.
- `Cancel() -> bool` — Cancels the request when it is still the mod's latest.
- `ClientSession { get; } -> uint` — The client world session it was submitted for (`Client.ClientWorldSession`).
- `Id { get; } -> uint` — kiln.client's id of the request; never 0.
- `State { get; } -> BuildingInputState` — Where the request is now: a copy read from kiln.client (any thread, never waits).

## BuildingInputResult `0.2.13` — The answer of a `BuildingInput` submit.
- `BuildingInputResult(bool Ok, string? Reason, BuildingInputRefusal Refusal, BuildingInputRequest? Request)`
- `Ok { get; init; } -> bool` — True when the request was queued.
- `Reason { get; init; } -> string?` — Why not; null when queued.
- `Refusal { get; init; } -> BuildingInputRefusal` — Why not, as a value; `BuildingInputRefusal.None` when queued.
- `Request { get; init; } -> BuildingInputRequest?` — The queued request; null when refused.

## BuildingInputState `0.2.13` — Where a building input request is (kiln_client.h's `KILN_BUILDING_INPUT_*`).
enum: Unknown, Queued, Pressed, Dispatched, TimedOut, Cancelled

## Client — The game client: side, session state, the local player, and the client world's tick.
- `static Available { get; } -> bool` — True when the kiln.client service is there (any Kiln with hooks enabled).
- `static ClientWorldSession { get; } -> uint` — Client worlds since the game started (one per hosted or joined game).
- `static ClientWorldSteps { get; } -> ulong` — Client-world steps since the game started.
- `static HasLocalPlayer { get; } -> bool` — Whether `Client.LocalPlayer` can be known on this game build.
- `static InGame { get; } -> bool` — In game (hosting or joined).
- `static IsClient { get; } -> bool` — True in the game client (enshrouded.exe), false on a dedicated server.
- `static IsClientThread { get; } -> bool` — True on the thread that steps the client world (the game's main thread).
- `static IsDedicatedServer { get; } -> bool` — True on a dedicated server (enshrouded_server.exe).
- `static IsHosting { get; } -> bool` — This client hosts the world (single player or co-op host).
- `static IsRemote { get; } -> bool` — This client joined a world hosted elsewhere.
- `static LocalPlayer { get; } -> EntityId` — The local player's entity, `EntityId.None` in the menu, while loading, before the player spawned, or on a server.
- `static NextTick() -> Client.NextTickAwaitable` — `await Client.NextTick()` continues on the main thread at the next client-world step.
- `static Post(Action work) -> void` — Runs `work` on the game's main thread before the client world's next step.
- `static ServerWorldSession { get; } -> uint` — Server worlds since the game started (one per hosted game).
- `static State { get; } -> ClientState` — What runs right now.
- `static Tick -> TickHandler?` — One client-world step (about 60 a second while in game; nothing in the menu), on the game's main thread.

## Client.NextTickAwaitable
- `GetAwaiter() -> Client.NextTickAwaitable.Awaiter`

## Client.NextTickAwaitable.Awaiter
- `GetResult() -> void`
- `IsCompleted { get; } -> bool`
- `OnCompleted(Action continuation) -> void`
- `UnsafeOnCompleted(Action continuation) -> void`

## ClientState — What runs in this process.
enum: Menu, DedicatedServer, Hosting, Remote
