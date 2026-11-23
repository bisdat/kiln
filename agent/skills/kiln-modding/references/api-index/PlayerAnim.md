# PlayerAnim (Kiln.PlayerAnim)

## PlayerAnimDefineResult `0.2.1` — The answer to `PlayerAnimStates.Define`.
- `PlayerAnimDefineResult(string Mod, string Name, Guid Guid, uint Id, PlayerAnimStateStatus Status, string? Reason)`
- `Guid { get; init; } -> Guid` — The state's GUID (`PlayerAnimStates.GuidOf`).
- `Id { get; init; } -> uint` — The state's id in the graph (`PlayerAnimStates.IdOf`).
- `Mod { get; init; } -> string` — The defining mod's id.
- `Name { get; init; } -> string` — The state's name.
- `Ok { get; } -> bool` — True only for `PlayerAnimStateStatus.Live`.
- `Reason { get; init; } -> string?` — Null when live; "kept for the next start" when pending; the service's reason when refused.
- `Status { get; init; } -> PlayerAnimStateStatus` — Live, Pending or Refused.

## PlayerAnimGameState `0.2.1` — A state of the game's own player graph.
- `PlayerAnimGameState(string Machine, string Name, PlayerAnimStateKind Kind, IReadOnlyList<uint> Ids)`
- `Ids { get; init; } -> IReadOnlyList<uint>` — What enters it: one id for most states, four for a jump-attack state (one per weapon family), none for a state no id...
- `Kind { get; init; } -> PlayerAnimStateKind` — How a sequence enters it.
- `Machine { get; init; } -> string` — The debug name of the machine it is in.
- `Name { get; init; } -> string` — Its debug name.

## PlayerAnimGraphState `0.2.1` — What the player's graph is.
enum: Off, Waiting, Untouched, Merged

## PlayerAnimState `0.2.1` — A state a mod adds to the player's animation graph: the C# form of one entry of `assets/player-states.json`.
- `PlayerAnimState(string Name, string Clip, string Like)`
- `BlendIn { get; init; } -> float?` — Blend-in time in seconds, 0 to 5; null: the neighbour's.
- `Clip { get; init; } -> string` — A clip of this mod's cook by name, or a GUID written out (a clip the game or another mod serves).
- `Like { get; init; } -> string` — A state of the player's graph by its debug name (`State`, or `Machine/State` when several machines have the name).
- `Loop { get; init; } -> bool?` — Whether the clip loops; null: the clip's own flag.
- `Name { get; init; } -> string` — The state's name: 1 to 64 characters, no `/`, case-sensitive, unique in the mod.
- `Speed { get; init; } -> float?` — Play speed, above 0; null: 1.

## PlayerAnimStateInfo `0.2.1` — A state a mod defines, in its `assets/player-states.json` or from code.
- `PlayerAnimStateInfo(string Mod, string Name, Guid Guid, uint Id, PlayerAnimStateKind Kind, PlayerAnimStateStatus Status, string Like, string Machine, string Reason)`
- `Guid { get; init; } -> Guid` — `PlayerAnimStates.GuidOf`(Mod, Name): what a sequence event names.
- `Id { get; init; } -> uint` — `PlayerAnimStates.IdOf`(Guid): what the graph compares.
- `Kind { get; init; } -> PlayerAnimStateKind` — How a sequence enters it (follows from the neighbour's machine).
- `Like { get; init; } -> string` — The game state it is like, as written.
- `Machine { get; init; } -> string` — The debug name of the machine it is in; empty when it was not placed.
- `Mod { get; init; } -> string` — The defining mod's id.
- `Name { get; init; } -> string` — The state's name.
- `Reason { get; init; } -> string` — Why it is not live; empty when it is.
- `Status { get; init; } -> PlayerAnimStateStatus` — Live, Refused, Pending (defined from code, in the graph from the next start) or Unavailable.

## PlayerAnimStateKind `0.2.1` — How a sequence enters a state.
enum: State, Overlay

## PlayerAnimStates `0.2.1` — Mods' states in the player's animation graph (`kiln.playeranim`).
- `static All { get; } -> IReadOnlyList<PlayerAnimStateInfo>` — Every state mods define, by mod id, then name.
- `static Available { get; } -> bool` — True when the native service `kiln.playeranim` is here.
- `static Define(PlayerAnimState state) -> PlayerAnimDefineResult` — Requests a player-state definition from the calling mod's code.
- `static Find(string mod, string name) -> PlayerAnimStateInfo?` — Mod `mod`'s state `name`, from its `assets/player-states.json` or from code, with its status.
- `static FindGameStates(string name) -> IReadOnlyList<PlayerAnimGameState>` — The game's own states with this name, `State` or `Machine/State`.
- `static Graph { get; } -> PlayerAnimGraphState` — What the player's graph is on this side: `PlayerAnimGraphState.Off` without the service.
- `static GraphHash { get; } -> string` — Which graph this side serves: 16 hex digits (the first 8 bytes of SHA-256 over the served graph), all zero when it...
- `static GuidOf(string mod, string name) -> Guid` — The GUID of mod `mod`'s state `name`, by the rule alone (no lookup): UUIDv5 of `<mod>:state:<name>` in Kiln's id...
- `static IdOf(Guid state) -> uint` — A state's id in the graph: FNV-1a-32 of its GUID's 16 bytes in the game's order (`Guid.ToByteArray`), as `BlockIds.Fnv`.

## PlayerAnimStateStatus `0.2.1` — Where a mod's state stands.
enum: Live, Refused, Pending, Unavailable
