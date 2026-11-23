# ModValues (Kiln.ModValues)

## ModValueCauses `0.2.17` — Why a `ModValueChangedEvent` was posted (its `ModValueChangedEvent.Cause`).
- `const Changed = "changed" -> string` — Add, Spend or Set.
- `const EpochReset = "epoch-reset" -> string` — A higher carry epoch reset carried progress.
- `const Imported = "imported" -> string` — A client's carried copy raised the value (maximum merge).
- `const Initialized = "initialized" -> string` — A character got the value for the first time (Before is null).
- `const Migrated = "migrated" -> string` — A higher definition version converted the saved value (scale, bounds, levels, kind, partition).
- `const Reset = "reset" -> string` — Reset to the initial value.

## ModValueChangedEvent `0.2.17` — A value of a character changed on the server (tick thread, after the change).
- `ModValueChangedEvent(CharacterKey character, ModValueKey key, ModValueSnapshot? before, ModValueSnapshot? after, string cause, ulong requestId)`
- `After { get; } -> ModValueSnapshot?`
- `Before { get; } -> ModValueSnapshot?` — Null when the value did not exist before (`ModValueCauses.Initialized`).
- `Cause { get; } -> string` — One of `ModValueCauses`.
- `Character { get; } -> CharacterKey`
- `Key { get; } -> ModValueKey`
- `RequestId { get; } -> ulong` — The native caller's request id; 0 for managed calls.

## ModValueDefinition `0.2.17` — A definition as Kiln knows it (described or active): what a UI needs to show a value.
- `ModValueDefinition(ModValueKey Key, ModValueKind Kind, UiText Label, UiText NumberFormat, long Min, long Max, long Initial, int Scale, IReadOnlyList<long> Thresholds, ModValuePersistence Persistence, uint Version, uint CarryEpoch, ModValueReadPolicy ReadPolicy, IReadOnlyList<string> ReadMods, ModValueSyncPolicy Sync, bool Active)`
- `Active { get; init; } -> bool`
- `CarryEpoch { get; init; } -> uint`
- `Initial { get; init; } -> long`
- `Key { get; init; } -> ModValueKey`
- `Kind { get; init; } -> ModValueKind`
- `Label { get; init; } -> UiText`
- `Max { get; init; } -> long`
- `Min { get; init; } -> long`
- `NumberFormat { get; init; } -> UiText`
- `Persistence { get; init; } -> ModValuePersistence`
- `ReadMods { get; init; } -> IReadOnlyList<string>`
- `ReadPolicy { get; init; } -> ModValueReadPolicy`
- `Scale { get; init; } -> int`
- `Sync { get; init; } -> ModValueSyncPolicy`
- `Thresholds { get; init; } -> IReadOnlyList<long>`
- `Version { get; init; } -> uint`

## ModValueDefinitionChangedEvent `0.2.17` — A definition was described, activated or retired (its mod unloaded).
- `ModValueDefinitionChangedEvent(ModValueKey key, ModValueDefinition? definition, string change)`
- `Change { get; } -> string` — "described", "activated" or "retired".
- `Definition { get; } -> ModValueDefinition?` — Null when retired.
- `Key { get; } -> ModValueKey`

## ModValueDefinitionResult `0.2.17` — Result of `ModValues.Describe` and `ModValues.Define`.
- `ModValueDefinitionResult(bool Ok, string? Reason, ModValueKey? Key)`
- `Key { get; init; } -> ModValueKey?`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`

## ModValueImportedEvent `0.2.17` — A client's carried offer was applied (tick thread): how many values went up, stayed, were rejected.
- `ModValueImportedEvent(CharacterKey character, int applied, int unchanged, int rejected)`
- `Applied { get; } -> int`
- `Character { get; } -> CharacterKey`
- `Rejected { get; } -> int`
- `Unchanged { get; } -> int`

## ModValueImportingEvent `0.2.17` — A client offered its carried copy of values (progress from other worlds).
- `ModValueImportingEvent(CharacterKey character, IReadOnlyList<ModValueKey> offered)`
- `Character { get; } -> CharacterKey`
- `Offered { get; } -> IReadOnlyList<ModValueKey>` — The keys in the offer (also ones that will be rejected).
- `Reason { get; set; } -> string` — Logged and counted when canceled.

## ModValueKey `0.2.17` — A value's name: the defining mod's id and the value's id, written "mod:id".
- `ModValueKey()`
- `ModValueKey(string Mod, string Id)`
- `Id { get; init; } -> string` — The value's id: `[a-z][a-z0-9._-]{0,63}`.
- `Mod { get; init; } -> string` — The defining mod's id (lowercase), or a native owner's namespace ("native." and 32 hex digits).
- `static TryParse(string? text, out ModValueKey key) -> bool` — Reads "mod:id" (exactly one colon, no spaces).

## ModValueKind `0.2.17` — What a mod value is.
enum: Counter, Currency, Experience

## ModValueOptions `0.2.17` — How a mod value behaves: the same fields as a `mod-values/*.json` entry.
- `ModValueOptions(ModValueKind Kind, UiText Label, long Min = 0, long Max = 9223372036854775807, long? Initial = null, int Scale = 0, IReadOnlyList<long>? Thresholds = null, UiText? NumberFormat = null, ModValuePersistence Persistence = ModValuePersistence.World, bool TrustClientImports = false, uint Version = 1, uint CarryEpoch = 1, ModValueReadPolicy ReadPolicy = ModValueReadPolicy.OwnerOnly, IReadOnlyList<string>? ReadMods = null, ModValueSyncPolicy Sync = ModValueSyncPolicy.OwnerMod, bool AllowPartitionMove = false, bool ResetOnKindChange = false)`
- `AllowPartitionMove { get; init; } -> bool` — With a new version: move a value between World and Carried instead of holding it.
- `CarryEpoch { get; init; } -> uint` — Carried only: raise it to reset carried progress everywhere.
- `Initial { get; init; } -> long?` — Where a character starts; null: `Min`.
- `Kind { get; init; } -> ModValueKind` — Counter, currency or experience.
- `Label { get; init; } -> UiText` — The name players see, in the game's languages.
- `Max { get; init; } -> long` — Highest raw value.
- `Min { get; init; } -> long` — Lowest raw value (currency and experience: 0 or more).
- `NumberFormat { get; init; } -> UiText?` — How the number shows: N0-N6, F0-F6, "0" or "0." with 1 to 6 zeros; null: N0, or N\<scale>.
- `Persistence { get; init; } -> ModValuePersistence` — World (default) or Carried.
- `ReadMods { get; init; } -> IReadOnlyList<string>?` — With `ModValueReadPolicy.AllowList`: the mods that may read it (at most 32).
- `ReadPolicy { get; init; } -> ModValueReadPolicy` — Which mods may read the value.
- `ResetOnKindChange { get; init; } -> bool` — With a new version: a kind change resets to the new initial value instead of holding it.
- `Scale { get; init; } -> int` — Fixed decimal places, 0 to 6: the raw value 1234 with scale 2 shows as 12.34.
- `Sync { get; init; } -> ModValueSyncPolicy` — What the owning player's client receives.
- `Thresholds { get; init; } -> IReadOnlyList<long>?` — Experience only (required there): raw values where levels start, from 0, strictly increasing, at most 256.
- `TrustClientImports { get; init; } -> bool` — Required for Carried: the value may come in from a client's copy (by maximum).
- `Version { get; init; } -> uint` — Raise it when you change kind, bounds, scale, thresholds, initial or persistence.

## ModValuePersistence `0.2.17` — Where a value lives.
enum: World, Carried

## ModValueReadPolicy `0.2.17` — Which mods may read a value (cooperative protection between mods, not a sandbox).
enum: OwnerOnly, Public, AllowList

## ModValueReadResult `0.2.17` — Result of `ModValues.Read` and `ModValues.Synced`.
- `ModValueReadResult(bool Ok, string? Reason, ModValueSnapshot? Value)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `Value { get; init; } -> ModValueSnapshot?`

## ModValueReasons `0.2.17` — The reasons a mod value call is refused (`IKilnResult.Reason`).
- `const AccessDenied = "AccessDenied" -> string`
- `const BoundExceeded = "BoundExceeded" -> string`
- `const Budget = "Budget" -> string` — A limit: 128 definitions per mod, revision numbers exhausted.
- `const CharacterUnavailable = "CharacterUnavailable" -> string` — No live per-character identity: an account-only key, a character not in game (writes), no world.
- `const DefinitionConflict = "DefinitionConflict" -> string` — The same id is already defined (or described) with other content.
- `const DefinitionRetired = "DefinitionRetired" -> string` — The definition was active and is gone (its mod unloaded): the data is kept.
- `const EpochRequired = "EpochRequired" -> string` — Carried progress resets only through a higher carry epoch.
- `const ImportRefused = "ImportRefused" -> string`
- `const InsufficientFunds = "InsufficientFunds" -> string`
- `const MigrationRequired = "MigrationRequired" -> string` — The saved value needs a higher definition version, or its kind/partition changed without the policy that allows it...
- `const NonMonotonic = "NonMonotonic" -> string` — Carried progress only grows.
- `const NotReady = "NotReady" -> string` — The character's values are not initialized yet (it was never in game since the definition came).
- `const NotSynced = "NotSynced" -> string` — The client has no synced copy of this value (not received, sync none, another world).
- `const StaleRevision = "StaleRevision" -> string`
- `const Undefined = "Undefined" -> string`

## ModValueResult `0.2.17` — Result of a change.
- `ModValueResult(bool Ok, string? Reason, ModValueSnapshot? Before, ModValueSnapshot? After)`
- `After { get; init; } -> ModValueSnapshot?`
- `Before { get; init; } -> ModValueSnapshot?`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`

## ModValues `0.2.17` — A mod's own per-character values: counters, currencies, and experience with levels.
- `static Add(CharacterKey character, ModValueKey key, long amount, ModValueVersion? expected = null) -> ModValueResult` — Adds `amount` (an award; a counter also takes a negative amount).
- `static Define(string id, ModValueOptions options) -> ModValueDefinitionResult` — Activates the calling mod's value `id` on the server (describes it too).
- `static Definition(ModValueKey key) -> ModValueDefinition?` — A known definition (described here, active on a server, or received from the server): label, format, kind, levels.
- `static Describe(string id, ModValueOptions options) -> ModValueDefinitionResult` — Makes the calling mod's value `id` known on this side, without activating it: its label, format and kind for UI...
- `static Format(ModValueSnapshot value) -> string` — The value as players see it: the exact decimal (raw / 10^scale, never through a double) in the definition's number...
- `static Key(string localOrQualified) -> ModValueKey` — "id" names the calling mod's value; "other-mod:id" names another mod's (to read it, where its read policy allows).
- `static Read(CharacterKey character, ModValueKey key) -> ModValueReadResult` — A character's value, also of a character that is offline.
- `static Reset(CharacterKey character, ModValueKey key, ModValueVersion? expected = null) -> ModValueResult` — Back to the initial value.
- `static Set(CharacterKey character, ModValueKey key, long value, ModValueVersion? expected = null) -> ModValueResult` — Sets the raw value (within the bounds; a carried value only upwards).
- `static Spend(CharacterKey character, ModValueKey key, long amount, ModValueVersion? expected = null) -> ModValueResult` — Spends a positive `amount` of a currency.
- `static Synced(ModValueKey key) -> ModValueReadResult` — Client: this player's copy of a value, as the server last sent it (any thread).

## ModValueSnapshot `0.2.17` — One value of one character, as of one moment (a copy: changing nothing).
- `ModValueSnapshot(Guid World, CharacterKey Character, ModValueKey Key, long Value, int Level, int Scale, ModValueKind Kind, ModValuePersistence Persistence, uint DefinitionVersion, uint CarryEpoch, ulong ValueRevision, ulong PartitionRevision)`
- `CarryEpoch { get; init; } -> uint`
- `Character { get; init; } -> CharacterKey`
- `DefinitionVersion { get; init; } -> uint`
- `Key { get; init; } -> ModValueKey`
- `Kind { get; init; } -> ModValueKind`
- `Level { get; init; } -> int`
- `PartitionRevision { get; init; } -> ulong`
- `Persistence { get; init; } -> ModValuePersistence`
- `Scale { get; init; } -> int`
- `Value { get; init; } -> long`
- `ValueRevision { get; init; } -> ulong`
- `Version { get; } -> ModValueVersion` — The version to pass back for compare-and-set.
- `World { get; init; } -> Guid`

## ModValuesSnapshotPublishedEvent `0.2.17` — A whole update of the client's synced copy is in place (after its `ModValueSyncedEvent`s).
- `ModValuesSnapshotPublishedEvent(string nonce, ulong sequence, bool full)`
- `Full { get; } -> bool` — A full replacement (a new stream, a resync) rather than a change.
- `Nonce { get; } -> string`
- `Sequence { get; } -> ulong`

## ModValueSyncedEvent `0.2.17` — The client's synced copy of a value changed (main thread, after the whole update is in place).
- `ModValueSyncedEvent(ModValueKey key, ModValueSnapshot? before, ModValueSnapshot? after, string nonce, ulong sequence)`
- `After { get; } -> ModValueSnapshot?`
- `Before { get; } -> ModValueSnapshot?`
- `Key { get; } -> ModValueKey`
- `Nonce { get; } -> string` — The stream's id (new for each server, world and character).
- `Sequence { get; } -> ulong`

## ModValueSyncPolicy `0.2.17` — What the owning player's Kiln client receives.
enum: None, OwnerMod, Readers

## ModValueVersion `0.2.17` — A value's revision in one world: for compare-and-set.
- `ModValueVersion()`
- `ModValueVersion(Guid World, ulong ValueRevision)`
- `ValueRevision { get; init; } -> ulong`
- `World { get; init; } -> Guid`
