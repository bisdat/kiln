# Sigs (Kiln.Sigs)

## Sigs.Abilities.RegisterNative `0.2.4` — The game function `abilities/RegisterNative` as a typed hook and call: `bool (ptr registry, u32 hash, ptr fn, ptr name, u8 argc)` (sigdb descriptor; its code reads...
- `const Capability = "sig/abilities/RegisterNative" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Abilities.RegisterNative.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "abilities/RegisterNative" -> string` — The sigdb symbol.

## Sigs.Abilities.RegisterNative.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `Argc { get; set; } -> byte` — Argument 5 `argc` (u8, stack+0x28).
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Fn { get; set; } -> nint` — Argument 3 `fn` (ptr, r8).
- `Hash { get; set; } -> uint` — Argument 2 `hash` (u32, rdx).
- `Name { get; set; } -> nint` — Argument 4 `name` (ptr, r9).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Registry { get; set; } -> nint` — Argument 1 `registry` (ptr, rcx).
- `Result { get; set; } -> bool` — The return value (bool, rax): zero until the original ran or the handler set it; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Abilities.RegisterNative.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Abilities.RegisterNative.Call call) -> void`

## Sigs.AttrRef `0.2.4` — The game's struct `AttrRef` as a sigdb descriptor declares it: 16 bytes, aligned to 8.
- `Attr -> nint` — `attr` (ptr) at +0x8.
- `Error -> byte` — `error` (u8) at +0x0.
- `Reserved -> uint` — `reserved` (u32) at +0x4.
- `const SizeOf = 16 -> int` — The struct's size in bytes.

## Sigs.Core.GetComponent `0.2.4` — The game function `GetComponent` as a typed hook and call: `void (ptr output, ptr world, u32 componentHash, u32 entity)` (sigdb descriptor; its code reads [rcx, rdx...
- `const Capability = "sig/GetComponent" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Core.GetComponent.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint output, nint world, uint componentHash, uint entity) -> void` — Calls the game function directly, through its hook chain as the game's own callers do, on the tick thread (never...
- `const Symbol = "GetComponent" -> string` — The sigdb symbol.

## Sigs.Core.GetComponent.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `ComponentHash { get; set; } -> uint` — Argument 3 `componentHash` (u32, r8).
- `Entity { get; set; } -> uint` — Argument 4 `entity` (u32, r9).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Output { get; set; } -> nint` — Argument 1 `output` (ptr, rcx).
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.
- `World { get; set; } -> nint` — Argument 2 `world` (ptr, rdx).

## Sigs.Core.GetComponent.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Core.GetComponent.Call call) -> void`

## Sigs.Items.AttrChanged `0.2.4` — The game function `items/AttrChanged` as a typed hook and call: `void (ptr component, ptr registry)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/items/AttrChanged" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Items.AttrChanged.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "items/AttrChanged" -> string` — The sigdb symbol.

## Sigs.Items.AttrChanged.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Component { get; set; } -> nint` — Argument 1 `component` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Registry { get; set; } -> nint` — Argument 2 `registry` (ptr, rdx).
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Items.AttrChanged.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Items.AttrChanged.Call call) -> void`

## Sigs.Items.AttrFind `0.2.4` — The game function `items/AttrFind` as a typed hook and call: `AttrRef* (AttrRef* /*hidden return buffer*/, u32 attribute, ptr registry)` (sigdb descriptor; its code...
- `const Capability = "sig/items/AttrFind" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Items.AttrFind.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(uint attribute, nint registry) -> Sigs.AttrRef` — Calls the game function directly, through its hook chain as the game's own callers do, on the tick thread (never...
- `const Symbol = "items/AttrFind" -> string` — The sigdb symbol.

## Sigs.Items.AttrFind.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `Attribute { get; set; } -> uint` — Argument 1 `attribute` (u32, rdx).
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Registry { get; set; } -> nint` — Argument 2 `registry` (ptr, r8).
- `Result { get; } -> Sigs.AttrRef` — The caller's return buffer (AttrRef): a reference into the caller's memory, valid during the call; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Items.AttrFind.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Items.AttrFind.Call call) -> void`

## Sigs.Items.GetItemVolume `0.2.4` — The game function `items/GetItemVolume` as a typed hook and call: `OptS32* (OptS32* /*hidden return buffer*/, keen::ItemInfo* item)` (sigdb descriptor; its code reads...
- `const Capability = "sig/items/GetItemVolume" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Items.GetItemVolume.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint item) -> Sigs.OptS32` — Calls the game function directly, through its hook chain as the game's own callers do, from any thread.
- `const Symbol = "items/GetItemVolume" -> string` — The sigdb symbol.

## Sigs.Items.GetItemVolume.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Item { get; set; } -> nint` — Argument 1 `item` (keen::ItemInfo*, rdx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Result { get; } -> Sigs.OptS32` — The caller's return buffer (OptS32): a reference into the caller's memory, valid during the call; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Items.GetItemVolume.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Items.GetItemVolume.Call call) -> void`

## Sigs.OptS32 `0.2.4` — The game's struct `OptS32` as a sigdb descriptor declares it: 8 bytes, aligned to 4.
- `Has -> byte` — `has` (u8) at +0x4.
- `const SizeOf = 8 -> int` — The struct's size in bytes.
- `Value -> int` — `value` (s32) at +0x0.

## Sigs.Routines.DecideTask `0.2.16` — The game function `routines/DecideTask` as a typed hook and call: `void (ptr ctx, u8 phase)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/routines/DecideTask" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.DecideTask.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/DecideTask" -> string` — The sigdb symbol.

## Sigs.Routines.DecideTask.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Phase { get; set; } -> byte` — Argument 2 `phase` (u8, rdx).
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Routines.DecideTask.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.DecideTask.Call call) -> void`

## Sigs.Routines.FinishState `0.2.16` — The game function `routines/FinishState` as a typed hook and call: `bool (ptr ctx, u8 condition)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/routines/FinishState" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.FinishState.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/FinishState" -> string` — The sigdb symbol.

## Sigs.Routines.FinishState.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Condition { get; set; } -> byte` — Argument 2 `condition` (u8, rdx).
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Result { get; set; } -> bool` — The return value (bool, rax): zero until the original ran or the handler set it; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Routines.FinishState.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.FinishState.Call call) -> void`

## Sigs.Routines.ProcessCommands `0.2.16` — The game function `routines/ProcessCommands` as a typed hook and call: `void (ptr ctx)` (sigdb descriptor; its code reads [rcx]).
- `const Capability = "sig/routines/ProcessCommands" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.ProcessCommands.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/ProcessCommands" -> string` — The sigdb symbol.

## Sigs.Routines.ProcessCommands.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Routines.ProcessCommands.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.ProcessCommands.Call call) -> void`

## Sigs.Routines.SetDecision `0.2.16` — The game function `routines/SetDecision` as a typed hook and call: `void (ptr ctx, u8 decision)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/routines/SetDecision" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.SetDecision.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/SetDecision" -> string` — The sigdb symbol.

## Sigs.Routines.SetDecision.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `Decision { get; set; } -> byte` — Argument 2 `decision` (u8, rdx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Routines.SetDecision.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.SetDecision.Call call) -> void`

## Sigs.Routines.SetState `0.2.16` — The game function `routines/SetState` as a typed hook and call: `void (ptr ctx, u8 state)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/routines/SetState" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.SetState.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/SetState" -> string` — The sigdb symbol.

## Sigs.Routines.SetState.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.
- `State { get; set; } -> byte` — Argument 2 `state` (u8, rdx).

## Sigs.Routines.SetState.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.SetState.Call call) -> void`

## Sigs.Routines.SleepCheck `0.2.16` — The game function `routines/SleepCheck` as a typed hook and call: `bool (ptr ctx)` (sigdb descriptor; its code reads [rcx]).
- `const Capability = "sig/routines/SleepCheck" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.SleepCheck.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/SleepCheck" -> string` — The sigdb symbol.

## Sigs.Routines.SleepCheck.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Result { get; set; } -> bool` — The return value (bool, rax): zero until the original ran or the handler set it; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Routines.SleepCheck.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.SleepCheck.Call call) -> void`

## Sigs.Routines.VillagerWorker `0.2.16` — The game function `routines/VillagerWorker` as a typed hook and call: `void (ptr ctx)` (sigdb descriptor; its code reads [rcx]).
- `const Capability = "sig/routines/VillagerWorker" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Routines.VillagerWorker.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "routines/VillagerWorker" -> string` — The sigdb symbol.

## Sigs.Routines.VillagerWorker.Call `0.2.16` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Ctx { get; set; } -> nint` — Argument 1 `ctx` (ptr, rcx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Routines.VillagerWorker.Handler `0.2.16` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Routines.VillagerWorker.Call call) -> void`

## Sigs.Rules.QuatRotate `0.2.4` — The game function `rules/QuatRotate` as a typed hook and call: `ptr (ptr output, ptr rotation, ptr vector)` (sigdb descriptor; its code reads [rcx, rdx, r8]).
- `const Capability = "sig/rules/QuatRotate" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Rules.QuatRotate.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint output, nint rotation, nint vector) -> nint` — Calls the game function directly, through its hook chain as the game's own callers do, from any thread.
- `const Symbol = "rules/QuatRotate" -> string` — The sigdb symbol.

## Sigs.Rules.QuatRotate.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Output { get; set; } -> nint` — Argument 1 `output` (ptr, rcx).
- `Result { get; set; } -> nint` — The return value (ptr, rax): zero until the original ran or the handler set it; what the caller gets.
- `Rotation { get; set; } -> nint` — Argument 2 `rotation` (ptr, rdx).
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.
- `Vector { get; set; } -> nint` — Argument 3 `vector` (ptr, r8).

## Sigs.Rules.QuatRotate.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Rules.QuatRotate.Call call) -> void`

## Sigs.Time.GetGameTime `0.2.4` — The game function `time/GetGameTime` as a typed hook and call: `s64 (keen::Daytime* daytime, s64 ecsNow)` (sigdb descriptor; its code reads [rcx, rdx]).
- `const Capability = "sig/time/GetGameTime" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Time.GetGameTime.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint daytime, long ecsNow) -> long` — Calls the game function directly, through its hook chain as the game's own callers do, on the tick thread (never...
- `const Symbol = "time/GetGameTime" -> string` — The sigdb symbol.

## Sigs.Time.GetGameTime.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Daytime { get; set; } -> nint` — Argument 1 `daytime` (keen::Daytime*, rcx).
- `EcsNow { get; set; } -> long` — Argument 2 `ecsNow` (s64, rdx).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Result { get; set; } -> long` — The return value (s64, rax): zero until the original ran or the handler set it; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.

## Sigs.Time.GetGameTime.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Time.GetGameTime.Call call) -> void`

## Sigs.Time.SetTimeScale `0.2.4` — The game function `time/SetTimeScale` as a typed hook and call: `void (keen::Daytime* daytime, f32 scale, s64 ecsNow)` (sigdb descriptor; its code reads [rcx, xmm1, r8]).
- `const Capability = "sig/time/SetTimeScale" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Time.SetTimeScale.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint daytime, float scale, long ecsNow) -> void` — Calls the game function directly, through its hook chain as the game's own callers do, on the tick thread (never...
- `const Symbol = "time/SetTimeScale" -> string` — The sigdb symbol.

## Sigs.Time.SetTimeScale.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), `Call.CallOriginal` and `Call.Skip`.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Daytime { get; set; } -> nint` — Argument 1 `daytime` (keen::Daytime*, rcx).
- `EcsNow { get; set; } -> long` — Argument 3 `ecsNow` (s64, r8).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Scale { get; set; } -> float` — Argument 2 `scale` (f32, xmm1).
- `Skip() -> void` — Ends the call without the game's function: the caller gets nothing.

## Sigs.Time.SetTimeScale.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Time.SetTimeScale.Call call) -> void`

## Sigs.Weather.SampleAt `0.2.4` — The game function `weather/SampleAt` as a typed hook and call...
- `const Capability = "sig/weather/SampleAt" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.Weather.SampleAt.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `static Invoke(nint weather, nint at, long now) -> Sigs.WeatherSample` — Calls the game function directly, through its hook chain as the game's own callers do, on the tick thread (never...
- `const Symbol = "weather/SampleAt" -> string` — The sigdb symbol.

## Sigs.Weather.SampleAt.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `At { get; set; } -> nint` — Argument 2 `at` (keen::WorldTransform*, r8).
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Now { get; set; } -> long` — Argument 3 `now` (s64, r9).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Result { get; } -> Sigs.WeatherSample` — The caller's return buffer (WeatherSample): a reference into the caller's memory, valid during the call; what the...
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.
- `Weather { get; set; } -> nint` — Argument 1 `weather` (keen::WeatherSystem*, rdx).

## Sigs.Weather.SampleAt.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.Weather.SampleAt.Call call) -> void`

## Sigs.WeatherSample `0.2.4` — The game's struct `WeatherSample` as a sigdb descriptor declares it: 16 bytes, aligned to 4.
- `F0 -> float` — `f0` (f32) at +0x0.
- `F1 -> float` — `f1` (f32) at +0x4.
- `F2 -> float` — `f2` (f32) at +0x8.
- `F3 -> float` — `f3` (f32) at +0xc.
- `const SizeOf = 16 -> int` — The struct's size in bytes.

## Sigs.World.ReadOther `0.2.4` — The game function `world/ReadOther` as a typed hook and call: `ptr (ptr output, ptr storage, u16* accessor, u32 entity)` (sigdb descriptor; its code reads [rcx] and...
- `const Capability = "sig/world/ReadOther" -> string` — The descriptor's capability row: on when the symbol resolved and its code fits the descriptor.
- `static readonly Descriptor -> SigDescriptor` — The descriptor as kiln.hooks takes it.
- `static Detour(Sigs.World.ReadOther.Handler handler, int priority = 0) -> HookResult` — Hooks the function: `handler` runs on every call, on the calling thread (game worker threads too), in priority order...
- `const Symbol = "world/ReadOther" -> string` — The sigdb symbol.

## Sigs.World.ReadOther.Call `0.2.4` — One call of the function in flight, inside a `Handler`: its arguments (read and write; a narrow integer is masked to its type), its `Call.Result`, `Call.CallOriginal`...
- `Accessor { get; set; } -> ushort*` — Argument 3 `accessor` (u16*, r8).
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now; a second call does...
- `Entity { get; set; } -> uint` — Argument 4 `entity` (u32, r9).
- `OriginalCalled { get; } -> bool` — Whether `Call.CallOriginal` ran.
- `Output { get; set; } -> nint` — Argument 1 `output` (ptr, rcx).
- `Result { get; set; } -> nint` — The return value (ptr, rax): zero until the original ran or the handler set it; what the caller gets.
- `Skip() -> void` — Ends the call without the game's function: the caller gets `Call.Result` as the handler set it.
- `Storage { get; set; } -> nint` — Argument 2 `storage` (ptr, rdx).

## Sigs.World.ReadOther.Handler `0.2.4` — A handler of the function: it gets the call in flight by reference.
- `Invoke(ref Sigs.World.ReadOther.Call call) -> void`
