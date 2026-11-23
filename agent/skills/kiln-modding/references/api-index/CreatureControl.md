# CreatureControl (Kiln.CreatureControl)

## ControlBodyKey `0.2.3` — A controlled body as the broker names it: the server world's epoch, the body generation the broker gave it at its first binding (never 0, never reused) and its entity.
- `ControlBodyKey()`
- `ControlBodyKey(ulong WorldEpoch, ulong BodyGeneration, EntityId Entity)`
- `BodyGeneration { get; init; } -> ulong` — The broker's generation of the body.
- `Entity { get; init; } -> EntityId` — The body's entity.
- `WorldEpoch { get; init; } -> ulong` — The server world's session number.

## ControlChangedEvent `0.2.3` — A lease Kiln.CreatureControl watches changed: one acquired through `CreatureControl.Acquire`, or named to `CreatureControl.WaitActive`.
- `After { get; } -> ControlLeaseSnapshot` — The lease now.
- `Before { get; } -> ControlLeaseSnapshot?` — What Kiln saw of the lease before (null: the first time it saw it).
- `Cause { get; } -> ControlResultCode` — Why it changed: `ControlLeaseSnapshot.Result` of `ControlChangedEvent.After`.
- `Lease { get; } -> ControlLeaseId` — The lease.

## ControlLeaseId `0.2.3` — A lease on a creature body.
- `ControlLeaseId()`
- `ControlLeaseId(ulong Value)`
- `Value { get; init; } -> ulong` — The broker's handle.

## ControlLeaseResult `0.2.3` — The answer of `CreatureControl.WaitActive` and `CreatureControl.Release`.
- `ControlLeaseResult(bool Ok, string? Reason, ControlLeaseSnapshot? State)`
- `Ok { get; init; } -> bool` — True when the lease became Active (WaitActive) or ended Released (Release).
- `Reason { get; init; } -> string?` — Why not; null when Ok.
- `State { get; init; } -> ControlLeaseSnapshot?` — The lease when the answer was made; null when the broker has no such lease.

## ControlLeaseSnapshot `0.2.3` — A lease as the broker published it.
- `ControlLeaseSnapshot(ControlBodyKey Body, ControlLeaseId Lease, ControlLeaseId WinningLease, ControlRequestId Request, ControlRole RequestedRole, ControlRole EffectiveRole, ControlLeaseState State, ControlResultCode Result, ulong TransitionSequence, ulong NoticeSequence, uint ChangedMask, string Reason)`
- `Body { get; init; } -> ControlBodyKey` — The body.
- `CanDrive { get; } -> bool` — True while this lease is the body's Active winner: the holder may write the body's input now.
- `ChangedMask { get; init; } -> uint` — What the broker changed on the body: 1 SteeringAgent removed, 2 its AI-off layer, 4 a vehicle's behaviour removed.
- `EffectiveRole { get; init; } -> ControlRole` — The role that holds the body now (None while a change is not verified).
- `IsTerminal { get; } -> bool` — True for Released and Failed.
- `Lease { get; init; } -> ControlLeaseId` — This lease.
- `NoticeSequence { get; init; } -> ulong` — The owner's notice counter when this was published.
- `Reason { get; init; } -> string` — The reason in words (starts with a stable word: "Granted", "Busy", "Quiescence", ...).
- `Request { get; init; } -> ControlRequestId` — The request id that admitted it.
- `RequestedRole { get; init; } -> ControlRole` — The role it asked for.
- `Result { get; init; } -> ControlResultCode` — Why.
- `State { get; init; } -> ControlLeaseState` — Where the lease is.
- `TransitionSequence { get; init; } -> ulong` — The body's sequence when this lease last became Active (0 before).
- `WinningLease { get; init; } -> ControlLeaseId` — The body's Active lease now (none while it changes hands).

## ControlLeaseState `0.2.3` — Where a lease is.
enum: Pending, Active, Preempted, Released, Failed

## ControlRequestId `0.2.3` — A request id for `CreatureControl.Acquire`: unique and increasing per owner (take one from `CreatureControl.NewRequestId` right before you acquire).
- `ControlRequestId()`
- `ControlRequestId(ulong Value)`
- `Value { get; init; } -> ulong` — The id; 0 is none.

## ControlRequestResult `0.2.3` — The answer of `CreatureControl.Acquire`: an admission (Pending, never permission) or a refusal with its reason.
- `ControlRequestResult(bool Ok, string? Reason, ControlLeaseId Lease, ControlLeaseSnapshot? Initial)`
- `Initial { get; init; } -> ControlLeaseSnapshot?` — The lease's first state, or the refusal as the broker stated it.
- `Lease { get; init; } -> ControlLeaseId` — The lease (none for a refusal).
- `Ok { get; init; } -> bool` — True when the request was admitted.
- `Reason { get; init; } -> string?` — Why not; null when admitted.

## ControlResultCode `0.2.3` — Why a lease is where it is (`ControlLeaseSnapshot.Reason` says more).
enum: None, Granted, Released, Preempted, Canceled, Busy, TimedOut, BodyGone, WorldEnded, CapabilityOff, OwnerUnloaded, RestoreFailed, Retired, ExpiredHandle, AiInhibited

## ControlRole `0.2.3` — What a lease is for.
enum: None, PetFollow, PetCombat, Ridden, Quarantine, Vehicle, PetStay, EngineGuard

## CreatureControl `0.2.3` — Leases on creature bodies, from the one broker that owns a controlled body's movement (service `kiln.creature_control`).
- `static Acquire(EntityRef body, ControlRole role, ControlRequestId requestId) -> ControlRequestResult` — Asks for a lease on `body` in `role`.
- `static Available { get; } -> bool` — True when kiln.creature_control is loaded (row `kiln.creature_control`).
- `static Inspect(ControlLeaseId lease) -> ControlLeaseSnapshot?` — The lease as the broker has it now; null when it has no such lease (unknown, or ended more than 10 minutes of...
- `static NewRequestId() -> ControlRequestId` — A new request id for the calling mod (unique, increasing).
- `static Release(ControlLeaseId lease) -> Task<ControlLeaseResult>` — Releases the calling mod's lease.
- `static WaitActive(ControlLeaseId lease, double timeoutSimSeconds = 0.5) -> Task<ControlLeaseResult>` — Waits until `lease` is Active (the body is yours), at most `timeoutSimSeconds` of simulation time.
