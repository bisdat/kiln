# Hooks (Kiln.Hooks)

## BytePattern — Byte patterns in the common "48 8B ??
- `static Parse(string pattern) -> short[]`

## Constants — sigdb layout constants for advanced mods.
- `static Get(string name) -> long` — The sigdb constant `name` (core, or "\<modid>/name").
- `static TryGet(string name, out long value) -> bool`

## DetourHandler — A detour handler: returns the value the hooked function should yield.
- `Invoke(HookCall call) -> nuint`

## HookCall — A detour call in progress.
- `ArgCount { get; } -> int` — Number of valid argument slots (4 on x64).
- `CallOriginal() -> nuint` — Runs the rest of the detour chain and then the real function, with the current argument values, and returns what it...
- `ReadArgAs<T>(int i) -> T` — Reinterprets argument `i` as an unmanaged value (e.g. a pointer dereferenced by the caller).
- `this[int i] { get; set; } -> nuint` — Argument `i` (0..3): rcx, rdx, r8, r9.

## HookHandle — A handle to an installed hook or patch; removing it is idempotent.
- `Dispose() -> void`
- `Reason { get; } -> string?` `0.2.4` — The late installation refusal, or null.
- `Remove() -> void` — Removes this hook/patch.
- `State { get; } -> HookRegistrationState` `0.2.4` — Pending until the next safe tick; Installed, Refused, or Removed afterwards.

## HookModule `0.2.11` — A loaded module opened for hooks with `Hooks.Module`: another mod's DLL or a loader's DLL (Creative Mode's `creative.dll`, say).
- `At(nuint offset) -> nuint` — The absolute address of the code at `offset` from the module's base (an RVA, as a disassembler shows it for the DLL).
- `Base { get; } -> nuint` — Where the module is loaded.
- `CodeEnd { get; } -> nuint` — The end of its last executable section (absolute, exclusive).
- `CodeStart { get; } -> nuint` — The start of its first executable section (absolute).
- `Contains(nuint address) -> bool` — Whether `address` lies in the module's code span.
- `Describe(nuint address) -> string` — "creative.dll+0x15F5F7" for an address in the module.
- `Find(string pattern) -> nuint` — The single address in the module's code matching `pattern` (sigdb syntax: hex bytes, `??` wildcards, holes act as...
- `FindAll(string pattern, int max = 64) -> nuint[]` — Up to `max` addresses in the module's code matching `pattern`, in address order.
- `IsGame { get; } -> bool` — True for the game's exe (Kiln hooked its code already; nothing was pinned).
- `Name { get; } -> string` — The name it was opened by (its file name, as given to `Hooks.Module`).
- `Size { get; } -> nuint` — Its image size in bytes.

## HookRefusedEvent `0.2.4` — A typed hook was refused (`HookResult.Ok` false, or later on a pending request's handle): a game update changed the function, its symbol no longer resolves, or...
- `ModId { get; } -> string` — The mod that asked for the hook.
- `Reason { get; } -> string` — Why it was refused (the same text as `HookResult.Reason`).
- `Symbol { get; } -> string` — The sigdb symbol it was asked for.

## HookRegistrationState `0.2.4` — The lifecycle of a hook request.
enum: Pending, Installed, Refused, Removed

## HookResult `0.2.4` — The outcome of installing a typed hook.
- `HookResult(bool Ok, string? Reason, HookHandle? Handle)`
- `Handle { get; init; } -> HookHandle?` — Removes the hook (it also goes when the mod unloads); null when refused.
- `Ok { get; init; } -> bool` — True when installed or accepted pending; inspect Pending and Handle.State for completion.
- `Pending { get; } -> bool` — The request was accepted and will install at the next safe tick.
- `Reason { get; init; } -> string?` — Why it was refused; null when `Ok`.

## Hooks — Forge's lowest tier for Kiln: install inline detours (with a callable original and a priority chain, like a Mixin @Inject head/tail with cancellation) and...
- `static Available { get; } -> bool` — True when kiln.hooks is loaded on this build.
- `static Detour(nuint address, DetourHandler handler, int priority = 0) -> HookHandle` — Detours the function at `address` (absolute).
- `static Detour(string symbol, DetourHandler handler, int priority = 0) -> HookHandle` — Detours the function at `symbol` (a sigdb name - core, or "\<modid>/name" from a shipped fragment).
- `static FindInModule(string moduleName, string pattern) -> nuint` `0.2.11` — The single address matching `pattern` in the code of the loaded module `moduleName`, which this opens for hooks...
- `static Mid(nuint address, MidHandler handler, int priority = 0) -> HookHandle` — Installs a mid-function hook at `address` (absolute).
- `static Mid(string symbol, MidHandler handler, int priority = 0) -> HookHandle` — Installs a mid-function hook at `symbol`.
- `static Module(string name) -> HookModule` `0.2.11` — Opens the loaded module `name` (its file name: `"creative.dll"`) for hooks: another mod's DLL or a loader's DLL.
- `static Stats() -> HooksStats` — Hook/patch counts for diagnostics.
- `static TryModule(string name, out HookModule? module) -> bool` `0.2.11` — `Hooks.Module` that returns false when no module of that name is loaded (yet).

## HooksStats — Snapshot of what kiln.hooks has installed.
- `HooksStats()`
- `HooksStats(int DetourTargets, int MidTargets, int Handlers, int Patches, ulong DetourCalls, ulong MidCalls)`
- `DetourCalls { get; init; } -> ulong`
- `DetourTargets { get; init; } -> int`
- `Handlers { get; init; } -> int`
- `MidCalls { get; init; } -> ulong`
- `MidTargets { get; init; } -> int`
- `Patches { get; init; } -> int`

## Keep — Zeroed native memory that outlives a mod's reload: the same block for the same key for the life of the process (the first call with a key decides the size).
- `static Block(string key, int size) -> nint` — The block for `key`; `size` bytes, zeroed, on first use.
- `static Span<T>(string key, int count) -> Span<T>` — The block as a span of `T` (its first-use size, in whole T).

## Memory — Reads and writes the game process's memory, and applies byte patches (access transformers) that are restored automatically when the mod unloads.
- `static Patch(nuint address, params byte[] bytes) -> HookHandle` — Overwrites bytes at `address`, remembering the originals; restored when the handle is removed or the mod unloads.
- `static Read<T>(nuint address) -> T` — Reads `T` from `address`.
- `static ReadBytes(nuint address, int count) -> byte[]` — Copies `count` bytes from `address`.
- `static TryRead<T>(nuint address, out T value) -> bool`
- `static Write<T>(nuint address, T value) -> void` — Writes `T` to `address` (not restored on unload; use `Memory.Patch` for reversible code edits).

## MidHandler — A mid-hook handler: reads and writes the registers in place.
- `Invoke(ref Registers regs) -> void`

## OriginalCode — Finds and reads the running executable's code as it is in the exe file, before any hook.
- `static Available { get; } -> bool` — True when the exe file could be read and parsed.
- `static FindUnique(string pattern) -> nuint` — The single address whose original bytes match `pattern` ("48 8B ??
- `static Read(nuint address, int count) -> byte[]` — The bytes at `address` as they are in the exe file.
- `static TryFindUnique(string pattern, out nuint address, out string why) -> bool` — Like `OriginalCode.FindUnique`, without throwing; `why` says "no match", "N+ matches", or why the pattern or...

## PeCode — The .text section of a PE32+ file as it is mapped at `loadedBase`: its bytes, with the image's DIR64 base relocations applied for the difference between the preferred...
- `FindUnique(ReadOnlySpan<short> pattern, out int count) -> long` — The RVA of the single match of `pattern` (see `BytePattern`), or -1.
- `static Parse(ReadOnlySpan<byte> file, ulong loadedBase) -> PeCode` — Parses an x64 PE32+ image.
- `static Parse(ReadOnlySpan<byte> file, ulong loadedBase, ReadOnlySpan<byte> mappedHeaders) -> PeCode` — Like `PeCode.Parse`, requiring all mapped PE headers to match the file before using its bytes.
- `Text { get; } -> byte[]`
- `TextRva { get; } -> uint`
- `TryRead(ulong rva, Span<byte> dst) -> bool` — The bytes at `rva` (relative to the image base); false outside .text.

## Registers — Full CPU register context at a mid hook.
- `R10 -> nuint` — General registers.
- `R11 -> nuint` — General registers.
- `R12 -> nuint` — General registers.
- `R13 -> nuint` — General registers.
- `R14 -> nuint` — General registers.
- `R15 -> nuint` — General registers.
- `R8 -> nuint` — General registers.
- `R9 -> nuint` — General registers.
- `Rax -> nuint` — General registers.
- `Rbp -> nuint` — General registers.
- `Rbx -> nuint` — General registers.
- `Rcx -> nuint` — General registers.
- `Rdi -> nuint` — General registers.
- `Rdx -> nuint` — General registers.
- `Rflags -> nuint` — General registers.
- `Rip -> nuint` — Where execution goes on (the next hook, or the overwritten instructions).
- `Rsi -> nuint` — General registers.
- `Rsp -> nuint` — General registers.
- `TrampolineRsp -> nuint` — safetyhook's rule: starts at `Rsp - 8` (it points at `Registers.Rip`); the game resumes with `rsp = TrampolineRsp; ret`.
- `Xmm(int n) -> Span<byte>` — xmm register `n` (0..15) as a 16-byte span.
- `XmmD(int n) -> double` — The low 8 bytes of xmm`n` as a double (a float argument/return).
- `XmmF(int n) -> float`

## SigArg `0.2.4` — One declared parameter of a descriptor.
- `SigArg()`
- `SigArg(string Name, SigKind Kind, uint Size)`
- `Kind { get; init; } -> SigKind`
- `Name { get; init; } -> string`
- `Size { get; init; } -> uint`

## SigCall `0.2.4` — A typed call in flight, as the generated `Call` wraps it.
- `CallOriginal() -> void` — Runs the rest of the hook chain and the game's function once, with the arguments as they are now, and stores its result.
- `F32(int p) -> float` — A float position (the low 4 bytes of its xmm register or stack slot).
- `F64(int p) -> double` — A double position.
- `Int(int p) -> ulong` — The 8-byte integer slot of position `p` (upper bytes of a narrow type are garbage).
- `OriginalCalled { get; } -> bool` — Whether `SigCall.CallOriginal` ran.
- `RetAddress { get; } -> void*` — The address of rax (a struct of 1/2/4/8 bytes returned by value lives in it).
- `RetF32 { get; } -> float` — The float return (the low 4 bytes of xmm0).
- `RetF64 { get; } -> double` — The double return.
- `RetInt { get; } -> ulong` — The integer / pointer return (rax).
- `ReturnBuffer { get; } -> void*` — The caller's return buffer (sret: position 0), valid during the call.
- `SetF32(int p, float value) -> void` — Writes a float position.
- `SetF64(int p, double value) -> void` — Writes a double position.
- `SetInt(int p, ulong value) -> void` — Writes the 8-byte integer slot of position `p`.
- `SetRetF32(float value) -> void` — Sets the float return.
- `SetRetF64(double value) -> void` — Sets the double return.
- `SetRetInt(ulong value) -> void` — Sets rax.
- `Skip() -> void` — Ends the call without the game's function: the result is what the handler set (zero otherwise; an sret buffer as the...
- `SlotAddress(int p) -> void*` — The address of position `p`'s integer slot (a by-value struct of 1/2/4/8 bytes lives in it).

## SigDescriptor `0.2.4` — A typed descriptor as the generated class carries it: the declared parameters (never the hidden return buffer), the return, `sret` and `invoke`.
- `SigDescriptor(string symbol, SigKind ret, uint retSize, bool sret, SigInvoke invoke, params SigArg[] args)` — A descriptor of `symbol`.
- `Args { get; } -> IReadOnlyList<SigArg>` — The declared parameters, in order.
- `Invoke { get; } -> SigInvoke` — Whether a direct call is offered.
- `Positions { get; } -> int` — Call positions: the declared parameters plus the hidden buffer.
- `Ret { get; } -> SigKind` — The return kind.
- `RetSize { get; } -> uint` — The return's size in bytes (a struct's; 0 for void).
- `Sret { get; } -> bool` — The function returns a struct through a hidden first pointer.
- `StackCount { get; } -> int` — Positions on the stack (beyond the fourth).
- `Symbol { get; } -> string` — The sigdb symbol.

## SigFrame `0.2.4` — A call frame for a typed direct call (the generated `Invoke`): the four integer and four float registers and up to 16 stack positions, then rax and xmm0 for the result.
- `RetF32 { get; } -> float` — The low 4 bytes of xmm0 after the call.
- `RetF64 { get; } -> double` — The low 8 bytes of xmm0 after the call.
- `RetInt { get; } -> ulong` — rax after the call.
- `SetF32(int p, float value) -> void` — Sets float position `p`.
- `SetF64(int p, double value) -> void` — Sets double position `p`.
- `SetInt(int p, ulong value) -> void` — Sets integer position `p`.

## SigHandler `0.2.4` — A handler as the generated `Detour` installs it.
- `Invoke(SigCall call) -> void`

## SigInvoke `0.2.4` — Whether a descriptor offers a direct call (sigdb `"invoke"`).
enum: None, Tick, Any

## SigKind `0.2.4` — The type of an argument or return of a typed descriptor (KILN_SIG_* in kiln_hooks.h).
enum: Void, S8, U8, S16, U16, S32, U32, S64, U64, F32, F64, Bool, Ptr, ByVal, Struct

## SigSummary — Result of resolving a mod's shipped symbol fragment.
- `SigSummary()`
- `SigSummary(int Symbols, int SymbolsOk, int Constants, int ConstantsOk, int Features, int FeaturesOk)`
- `Constants { get; init; } -> int`
- `ConstantsOk { get; init; } -> int`
- `Features { get; init; } -> int`
- `FeaturesOk { get; init; } -> int`
- `Symbols { get; init; } -> int`
- `SymbolsOk { get; init; } -> int`

## Symbols — sigdb symbol lookup and image scanning for advanced mods: core symbols, the mod's own shipped fragment, and ad-hoc pattern scans in the running executable.
- `static Find(string pattern) -> nuint` — The single address matching `pattern` (sigdb syntax; holes act as wildcards).
- `static FindAll(string pattern, int max = 64) -> nuint[]` — Up to `max` addresses matching `pattern`.
- `static Get(string name) -> nuint` — Absolute address of the sigdb symbol `name` (a core name, or "\<modid>/name" from the mod's shipped fragment).
- `static Hole(string pattern, nuint match, string hole) -> long` — Value of a named hole in `pattern` matched at `match` (a rel32 hole gives the absolute target).
- `static ImageBase { get; } -> nuint` — The image base of the running executable.
- `static ImageSize { get; } -> nuint` — The image size in bytes.
- `static LoadShipped() -> SigSummary` — Loads the calling mod's shipped symbol fragment (`<modDir>/sigdb/server.json`) against the running exe, so its...
- `static TryGet(string name, out nuint address) -> bool`

## TypedHooks `0.2.4` — What the generated classes call: install a typed hook, call a described function.
- `static Detour(SigDescriptor sig, SigHandler handler, int priority = 0) -> HookResult` — Installs a typed hook on `sig`'s symbol.
- `static Invoke(SigDescriptor sig, ref SigFrame frame) -> void` — Calls `sig`'s function with `frame` (through its hook chain, as the game's own callers do) and leaves the result in it.
