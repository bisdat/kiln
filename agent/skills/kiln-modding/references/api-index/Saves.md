# Saves (Kiln.Saves)

## BackupInfo — A backup made by `Saves.Backup`: the world file and the matching Kiln sidecar generation.
- `BackupInfo(string Name, string Directory, DateTimeOffset Created, string WorldId, uint Revision, uint WorldUnix, SaveReason Reason, int Files, long Bytes, int DataBundles)`
- `Bytes { get; init; } -> long` — The total size of those files in bytes.
- `Created { get; init; } -> DateTimeOffset` — When the backup was made (UTC).
- `DataBundles { get; init; } -> int` — How many mod data bundles of the matching sidecar generation the backup holds.
- `Directory { get; init; } -> string` — The backup's folder.
- `Files { get; init; } -> int` — How many files the backup holds.
- `Name { get; init; } -> string` — The name given to the backup.
- `Reason { get; init; } -> SaveReason` — The reason of the save the backup was made from.
- `Revision { get; init; } -> uint` — The world file's revision in the backup.
- `WorldId { get; init; } -> string` — The world's GUID as text.
- `WorldUnix { get; init; } -> uint` — The save time in the world file's header (UTC seconds, 0 when unknown).

## CharAck — Client to server: an update was written to the client's sidecar.
- `CharAck(Guid Character, long Revision)`
- `Character { get; init; } -> Guid` — The game's character id.
- `Revision { get; init; } -> long` — The revision the client wrote.

## CharacterData — Per-character mod data of the calling mod: one world's record of one character (like `PlayerData`, but two characters of one account have separate data).
- `static Characters { get; } -> IReadOnlyList<CharacterKey>` — Characters the calling mod has data for in this world.
- `static Contains(CharacterKey character, string key) -> bool` — Whether the character has a value under `key`.
- `static EnsureStarted() -> void` — Starts the channel and the session tracking (Kiln.Skills does this at Kiln's first service tick).
- `static Get<T>(CharacterKey character, string key) -> T` — The object stored under `key` for the character, created with `new T()` if there is none.
- `static Get<T>(EntityId player, string key) -> T` — `CharacterData.Get` for a player in game.
- `static KeyOf(EntityId player) -> CharacterKey` — The character of a connected player (`CharacterKey.None` when the player is unknown).
- `static Keys(CharacterKey character) -> IReadOnlyList<string>` — The calling mod's keys for the character, a snapshot.
- `static LastOf(ulong playerId) -> CharacterKey` — The character last played by this Steam or account id in this world, none if unknown.
- `static MigrateFromPlayerData(CharacterKey character, string key, params ulong[] playerIds) -> bool` — Moves the calling mod's `PlayerData` entry to this character, once: only when the character has no such key yet.
- `static Remove(CharacterKey character, string key) -> bool` — Removes the character's key (from the next save on).
- `static Set<T>(CharacterKey character, string key, T value) -> void` — Stores `value` for the character under `key` (replacing what was there).
- `static Status { get; } -> CharacterSyncStatus` — Counters of the transport.
- `static TryGet<T>(CharacterKey character, string key, out T? value) -> bool` — Reads the object stored under `key` for the character without creating one.

## CharacterData.Carried — What travels with the character: the same API, but a Kiln client keeps a copy next to its character and offers it to the next server, and the server sends changes...
- `static Characters { get; } -> IReadOnlyList<CharacterKey>` — Characters the calling mod has carried data for in this world.
- `static Contains(CharacterKey character, string key) -> bool` — Whether the character carries a value under `key`.
- `static Get<T>(CharacterKey character, string key) -> T` — The object carried by the character under `key`, created with `new T()` if there is none.
- `static Get<T>(EntityId player, string key) -> T` — `Carried.Get` for a player in game.
- `static Keys(CharacterKey character) -> IReadOnlyList<string>` — The calling mod's carried keys for the character, a snapshot.
- `static MigrateFromPlayerData(CharacterKey character, string key, params ulong[] playerIds) -> bool` — See `CharacterData.MigrateFromPlayerData`.
- `static Remove(CharacterKey character, string key) -> bool` — Removes the carried key (from the next save on).
- `static Revision(CharacterKey character) -> long` — The revision of this character's carried data on this server (+1 each time a save finds a change).
- `static Set<T>(CharacterKey character, string key, T value) -> void` — Stores `value` for the character to carry under `key` (replacing what was there).
- `static TryGet<T>(CharacterKey character, string key, out T? value) -> bool` — Reads the object carried by the character under `key` without creating one.

## CharacterImportedEvent — A character's carried data was replaced by what its client offered.
- `CharacterImportedEvent(CharacterKey character, EntityId player, long revision, IReadOnlyList<string> applied, IReadOnlyList<string> kept)`
- `Applied { get; } -> IReadOnlyList<string>` — Mods loaded here whose data came in.
- `Character { get; } -> CharacterKey` — The character whose carried data was replaced.
- `Kept { get; } -> IReadOnlyList<string>` — Mods not loaded here: their data is stored and passed on, nothing reads it.
- `Player { get; } -> EntityId` — The player, none for a lab peer.
- `Revision { get; } -> long` — The revision the server now holds, taken from the client's offer.

## CharacterImportingEvent — A client offered newer carried data for a character.
- `CharacterImportingEvent(CharacterKey character, EntityId player, long revision, long serverRevision, IReadOnlyDictionary<string, IReadOnlyList<string>> mods)`
- `Character { get; } -> CharacterKey` — The character whose data the client offered.
- `Mods { get; } -> IReadOnlyDictionary<string, IReadOnlyList<string>>` — Mod id -> its keys in the offer.
- `Player { get; } -> EntityId` — The player, none for a lab peer.
- `Reason { get; set; } -> string` — Shown in the log and sent to the client.
- `Revision { get; } -> long` — The client's revision and the server's.
- `ServerRevision { get; } -> long` — The revision the server holds for this character.

## CharacterKey — Who a character is: the game's CharacterId under the account that plays it.
- `CharacterKey()`
- `CharacterKey(ulong Account, Guid Id)`
- `Account { get; init; } -> ulong` — The game's account key (`Player.AccountId`).
- `static explicit operator PlayerRef(CharacterKey k) -> PlayerRef` — The person behind a character, as a `PlayerRef` with the character id only: no Steam id (the key's...
- `Id { get; init; } -> Guid` — The game's CharacterId.
- `IsAccountOnly { get; } -> bool` — The game's character id is unknown on this build: this key stands for the account, so every character of the account...
- `IsNone { get; } -> bool` — True for `CharacterKey.None`: no account and no character id.
- `static None { get; } -> CharacterKey` — The key of no character (the default value).
- `Text { get; } -> string` — The text used in storage scopes: "\<account hex>.\<id>".
- `static TryParse(string? text, out CharacterKey key) -> bool` — Reads `CharacterKey.Text` back.

## CharacterSettings — `kiln/config/characters.json` (server): what a server takes from clients.
- `ImportProgress { get; init; } -> bool` — Take carried data from clients, and send it back.
- `MaxCharactersPerAccount { get; init; } -> int` — Characters one account may bring to this world.
- `MaxImportBytes { get; init; } -> int` — The largest offer, in bytes of JSON values.
- `MaxKeys { get; init; } -> int` — Most keys in an offer.
- `MaxValueBytes { get; init; } -> int` — The largest single value.

## CharacterSyncStatus — The transport's counters.
- `CharacterSyncStatus(bool Started, bool ChannelOpen, bool ImportsAllowed, int Sessions, long Imports, long Refused, long Kept, long Dropped, long Updates, long UpdateBytes, long SidecarWrites)`
- `ChannelOpen { get; init; } -> bool` — The `kiln:character` channel is open (Kiln.Network is available).
- `Dropped { get; init; } -> long` — Offered values left out because they were broken or over a limit.
- `Imports { get; init; } -> long` — Offers the server took.
- `ImportsAllowed { get; init; } -> bool` — This server takes carried data from clients (`importProgress` in `characters.json`).
- `Kept { get; init; } -> long` — Mods whose offered data was stored unread because the mod is not loaded here.
- `Refused { get; init; } -> long` — Offers the server refused: the setting is off, a cap was passed, or a handler canceled the import.
- `Sessions { get; init; } -> int` — Characters in game that the transport tracks now.
- `SidecarWrites { get; init; } -> long` — Times a client-side sidecar file was written (a hosted game's own player, or an update received).
- `Started { get; init; } -> bool` — The transport started (`CharacterData.EnsureStarted` ran).
- `UpdateBytes { get; init; } -> long` — Bytes of JSON values in those updates.
- `Updates { get; init; } -> long` — Updates made to send a character's changes back to its client.

## CharHello — Server to client, sent when the player is in game: which character this is and whether the server takes carried data.
- `CharHello(Guid Character, string? World, bool Imports, int MaxBytes, long Revision)`
- `Character { get; init; } -> Guid` — The game's character id.
- `Imports { get; init; } -> bool` — The server takes carried data from clients (`importProgress` in `characters.json`).
- `MaxBytes { get; init; } -> int` — The largest offer the server takes, in bytes of JSON values.
- `Revision { get; init; } -> long` — The server's revision of this character's carried data.
- `World { get; init; } -> string?` — The world's id, or null when it is not known.

## CharLabJoin — Lab peers (tools/nettest) have no player: they say which character they pretend to be.
- `CharLabJoin(Guid Character, ulong Account, bool Local = false)`
- `Account { get; init; } -> ulong` — The account key it pretends to play under.
- `Character { get; init; } -> Guid` — The character the lab peer pretends to be.
- `Local { get; init; } -> bool` — True to act as the local player of a hosted game, whose sidecar is read and written directly.

## CharLabLeave — A lab peer (tools/nettest) leaves: its character is no longer in game.
- `CharLabLeave(Guid Character)`
- `Character { get; init; } -> Guid` — The character that left.

## CharOffer — Client to server: the character's carried data as the client's sidecar holds it.
- `CharOffer(Guid Character, long Revision, Dictionary<string, Dictionary<string, Json.JsonElement>> Mods)`
- `Character { get; init; } -> Guid` — The game's character id.
- `Mods { get; init; } -> Dictionary<string, Dictionary<string, Json.JsonElement>>` — The data, by mod id and then key, as JSON values.
- `Revision { get; init; } -> long` — The client's revision of the carried data.

## CharResult — Server to client: what happened to an offer.
- `CharResult(Guid Character, bool Accepted, string Reason, long Revision, List<string> Applied, List<string> Kept, List<string> Dropped)`
- `Accepted { get; init; } -> bool` — The server took the offer.
- `Applied { get; init; } -> List<string>` — Mod ids loaded on the server whose data came in.
- `Character { get; init; } -> Guid` — The game's character id.
- `Dropped { get; init; } -> List<string>` — Entries left out of the offer: a mod id that is not valid, or `mod/key` of a value that was broken or too big.
- `Kept { get; init; } -> List<string>` — Mod ids not loaded on the server: their data is stored and passed on, nothing reads it.
- `Reason { get; init; } -> string` — Why: `taken`, or the reason the offer was not taken.
- `Revision { get; init; } -> long` — The server's revision of the carried data after the answer.

## CharUpdate — A null value removes the key.
- `CharUpdate(Guid Character, long Revision, Dictionary<string, Dictionary<string, Json.JsonElement?>> Changes, bool Final)`
- `Changes { get; init; } -> Dictionary<string, Dictionary<string, Json.JsonElement?>>` — The changed values by mod id and then key, as JSON; null removes the key.
- `Character { get; init; } -> Guid` — The game's character id.
- `Final { get; init; } -> bool` — True for the last update of a session: the character left, or the server is shutting down.
- `Revision { get; init; } -> long` — The revision the client has after applying the changes.

## DataMatch — Which sidecar generation `WorldData` loaded for the world file on disk.
enum: None, Exact, Staged, Older

## EntityData — Data the calling mod attaches to an entity (Forge's capabilities / data attachments).
- `static Available { get; } -> bool` — True when kiln.saves and kiln.spawn are both available: every entity's data works.
- `static Clear(EntityId entity) -> bool` — Removes everything the calling mod attached to the entity.
- `static Entities { get; } -> IReadOnlyList<EntityId>` — Live entities the calling mod has attachments on.
- `static Get<T>(EntityId entity, string key) -> T` — The object the calling mod attached to the entity under `key`, created with `new T()` if there is none.
- `static Has(EntityId entity, string key) -> bool` — Whether the calling mod has a value attached to the entity under `key`.
- `static IsPersistent(EntityId entity) -> bool` — Whether the entity has a known, saved world template with a position for matching after a restart.
- `static Matched { get; } -> int` `0.2.7` — Saved attachments of the calling mod matched to a loaded entity since the world's data was loaded: with...
- `static Remove(EntityId entity, string key) -> bool` — Removes the calling mod's key from the entity.
- `static Set<T>(EntityId entity, string key, T value) -> void` — Attaches `value` to the entity under `key` (replacing what was there).
- `static TryGet<T>(EntityId entity, string key, out T? value) -> bool` — Reads the object the calling mod attached to the entity under `key` without creating one.
- `static Waiting { get; } -> int` — Loaded attachments of the calling mod still waiting for their entity: saved with the world, not matched to a loaded...

## InstanceClaimResult `0.2.2` — The answer to `InstanceData.Claim`: whether the mod's values of a parked record are on the item now.
- `Item { get; } -> ItemInstanceRef` — The item the values were to go on.
- `Keys { get; } -> IReadOnlyList<string>` — The keys that moved (or, when not `InstanceClaimResult.Ok`, the keys the item already has).
- `Mod { get; } -> string` — The mod whose values moved ("" for every mod: `/instances claim`).
- `Ok { get; } -> bool` — True when the values are on the item and the record no longer holds them.
- `Reason { get; } -> string?` — Why not; null when `InstanceClaimResult.Ok`.
- `RecordId { get; } -> string` — The record's id.

## InstanceData — Values that stay with one item: `sword.Get(Kills).Count++`, and the count follows that sword through moves between chests and players, saves, restarts, `kill -9` and...
- `static All<T>(InstanceKey<T> key) -> IReadOnlyList<(ItemInstanceRef Item, T Value)>` — Every item in this session that carries a value of `key`, with the value (the live object).
- `static Claim<T>(ParkedInstance<T> parked, ItemInstanceRef item) -> InstanceClaimResult` `0.2.2` — Puts a parked record of the key's mod on `item`: every value the mod has in that record (all its keys, not only this...
- `static Discard<T>(ParkedInstance<T> parked) -> bool` `0.2.2` — Drops a parked record of the key's mod: every value the mod has in that record goes (other mods' values stay parked).
- `static Key<T>(string name, InstanceKeyOptions? options = null) -> InstanceKey<T>` — Declares a key of the calling mod.
- `static Parked<T>(InstanceKey<T> key) -> IReadOnlyList<ParkedInstance<T>>` — The saved values of `key` that are not on an item now: records waiting for their holder or their item...
- `static Synced() -> IReadOnlyList<SyncedItem>` `0.2.2` — Client: every item this game has copies for, as the server sent them: the items of this player's own inventory that...
- `static Synced(EntityId item) -> SyncedItem?` `0.2.2` — Client: the item with the server's entity id `item` as this game knows it; null when the server sent nothing for it.
- `static Synced<T>(EntityId item, InstanceKey<T> key) -> T?` `0.2.2` — Client: the server's value of `key` on the item with the server's entity id `item`, as a copy; null when the server...

## InstanceDataChangedEvent — A value was set or removed through the instance-data API.
- `Item { get; } -> ItemInstanceRef` — The item whose value changed.
- `Key { get; } -> string` — The key's name.
- `Mod { get; } -> string` — The mod id the value belongs to.
- `Removed { get; } -> bool` — True for Remove; false for Set.

## InstanceDataLostEvent — A record no longer goes with an item: the item was destroyed, it was not found in 3 sessions, the holder was over the cap, or it was set aside because it was not...
- `Detail { get; } -> string` — What happened, in words.
- `Item { get; } -> ItemId` — The item type.
- `Keys { get; } -> IReadOnlyList<string>` — The names of the mod's keys that were lost or set aside.
- `LastRef { get; } -> ItemInstanceRef` — The item as it was in this session (a destroyed item), `ItemInstanceRef.None` for a saved record.
- `Mod { get; } -> string` — The mod id the values belonged to.
- `Reason { get; } -> InstanceLossReason` — Why they no longer go with an item.

## InstanceDataParkedEvent — A record waits for its item: its holder was not found, the player left with it, or no item of its holder fits it.
- `Character { get; } -> CharacterKey` — The character for a player's record; `CharacterKey.None` otherwise.
- `Detail { get; } -> string` — Why, in words.
- `FlatSlot { get; } -> int` — The flat slot it was saved at (link x 8 + index).
- `Holder { get; } -> string` — The holder, in words (a template and position, or a character).
- `Item { get; } -> ItemId` — The item type.
- `Keys { get; } -> IReadOnlyList<string>` — The names of the mod's keys that wait.
- `Mod { get; } -> string` — The mod id the values belong to.
- `Reason { get; } -> InstanceParkReason` — Why they wait.

## InstanceDataRestoredEvent — A saved record found its item again after a load or a join: the value is on the item now.
- `Detail { get; } -> string` — Why this item, in words.
- `Item { get; } -> ItemInstanceRef` — The item the value is on now.
- `Key { get; } -> string` — The key's name.
- `Match { get; } -> InstanceMatch` — How the record was matched to this item.
- `Mod { get; } -> string` — The mod id the value belongs to.

## InstanceDataSyncedEvent `0.2.2` — Client: a copy this game holds of an item's value changed.
- `Change { get; } -> InstanceSyncChange` — What happened.
- `Item { get; } -> SyncedItem` — The item with its copies as they are after the change (for `InstanceSyncChange.Gone`: as they were last).
- `Key { get; } -> string` — The key's name.
- `Mod { get; } -> string` — The mod id the value belongs to.

## `InstanceKey<T>` — A typed key of instance data: one kind of value a mod keeps on single items (this sword's kill count, this ring's rolled bonus).
- `Mod { get; } -> string` — The id of the mod that declared it (its `kiln.mod.json` id).
- `Name { get; } -> string` — The key's name, unique within its mod.
- `Persist { get; } -> Persist` — Where its values are kept.
- `Sync { get; } -> InstanceSync` `0.2.2` — Which players get its values on their own game.

## InstanceKeyOptions — Options of an `InstanceKey`.
- `Items { get; init; } -> IReadOnlyList<ItemId>?` — The item types that may carry this key; null: any item with an item entity (gear, tools, cans).
- `NearbyRadius { get; init; } -> double` `0.2.2` — For `InstanceSync.Nearby`: how far from the item's holder a player still gets the values, in metres (1 to 256...
- `Persist { get; init; } -> Persist` — Where the values are kept (default `Persist.World`).
- `Sync { get; init; } -> InstanceSync` `0.2.2` — Which players get the values on their own game (default `InstanceSync.None`: nobody).

## InstanceLossReason — Why a record no longer goes with any item.
enum: Destroyed, Unmatched, Ambiguous, Capped, Discarded `0.2.2`

## InstanceMatch — How a saved record found its item again.
enum: Exact, Moved, Claimed `0.2.2`

## InstanceParkReason — Why a record waits.
enum: HolderAway, PlayerLeft, NoCandidate

## InstanceSync `0.2.2` — Which players get the values of an `InstanceKey` on their own game, where a mod's client side reads them (`InstanceData.Synced`).
enum: None, Holder, Nearby

## InstanceSyncChange `0.2.2` — What happened to a value this game holds a copy of (`InstanceDataSyncedEvent`).
enum: Set, Removed, Moved, Gone

## ItemInstanceDataExtensions — The calls on one item (`InstanceData`).
- `static Get<T>(this ItemInstanceRef item, InstanceKey<T> key) -> T` — The item's value of `key`, created with `new T()` if it has none: a live object, change it in place and Kiln saves...
- `static Has<T>(this ItemInstanceRef item, InstanceKey<T> key) -> bool` — Whether the item has a value of `key`.
- `static Remove<T>(this ItemInstanceRef item, InstanceKey<T> key) -> bool` — Removes the item's value of `key` (from the next save on).
- `static Set<T>(this ItemInstanceRef item, InstanceKey<T> key, T value) -> void` — Gives the item `value` for `key` (replacing what it had).
- `static TryGet<T>(this ItemInstanceRef item, InstanceKey<T> key, out T? value) -> bool` — The item's value of `key` if it has one.

## `ParkedInstance<T>` — A saved value that is not on an item now (`InstanceData.Parked`).
- `Character { get; } -> CharacterKey` — The character, for a record of a player's inventory; `CharacterKey.None` otherwise.
- `FlatSlot { get; } -> int` — The flat slot it was saved at (link x 8 + index).
- `Holder { get; } -> string` — The holder it was saved in, in words (a template and position, or a character).
- `Item { get; } -> ItemId` — The item type it was saved on.
- `Reason { get; } -> string` — Why it is not on an item, in words.
- `RecordId { get; } -> string` `0.2.2` — The saved record's id (what `/instances` lists and `/instances claim` takes).
- `SessionsMissed { get; } -> int` — Sessions in which its holder or its item was not found.
- `State { get; } -> ParkedState` — Whether it waits for its item or was set aside (`ParkedState`).
- `Value { get; } -> T?` — A copy of the value; null when it does not read as `T`.

## ParkedState — Whether a `ParkedInstance` still waits for its item or is set aside.
enum: Waiting, SetAside

## Persist — Where the values of an `InstanceKey` are kept.
enum: World, Carried, Session

## PlayerData — Persistent per-player data of the calling mod, keyed by the player's SteamID64 (Forge's player persistent data / player attachments).
- `static Contains(ulong steamId, string key) -> bool` — Whether the player has a value under `key`.
- `static Get<T>(ulong steamId, string key) -> T` — The object stored under `key` for the player, created with `new T()` if there is none.
- `static Keys(ulong steamId) -> IReadOnlyList<string>` — The calling mod's keys for the player, a snapshot.
- `static Players { get; } -> IReadOnlyList<ulong>` — Steam ids the calling mod has data for in this world.
- `static Remove(ulong steamId, string key) -> bool` — Removes the player's key (from the next save on).
- `static Set<T>(ulong steamId, string key, T value) -> void` — Stores `value` for the player under `key` (replacing what was there).
- `static TryGet<T>(ulong steamId, string key, out T? value) -> bool` — Reads the object stored under `key` for the player without creating one.

## SaveReason — Why the game saved the world.
enum: Unknown, Autosave, Shutdown, PlayerLeft, Request, Backup

## SaveResult — The outcome of `Saves.Request`.
- `SaveResult(bool Success, SaveReason Reason, uint Revision, TimeSpan Duration, string? Error)`
- `Duration { get; init; } -> TimeSpan` — From the game accepting the request to the file being complete.
- `Error { get; init; } -> string?` — Why nothing was written; null on success.
- `Ok { get; } -> bool` `0.2.2` — The save was written: `SaveResult.Success` (the contract's name for it).
- `Reason { get; init; } -> SaveReason` — The reason of the save.
- `Revision { get; init; } -> uint` — The world file's revision after the write (0 when nothing was written).
- `Success { get; init; } -> bool` — The world file was written.

## Saves — The world save: events (`WorldSavingEvent`, `WorldSavedEvent`, `WorldLoadedEvent` on the event bus), "save now", the world's identity and paths, and consistent backups.
- `static Available { get; } -> bool` — True when the native service is loaded on this build.
- `static Backup(string name) -> Task<BackupInfo>` — Saves the world now and then copies the world file and the matching Kiln sidecar generation to...
- `static BackupDirectory { get; } -> string?` — Where `Saves.Backup` writes: `kiln/backups`.
- `static Backups { get; } -> IReadOnlyList<BackupInfo>` — Backups in `kiln/backups`, oldest first.
- `static CancelRestore() -> bool` — Cancels a staged restore; false if none was staged.
- `static DataDirectory { get; } -> string?` — Kiln's sidecar folder of this world: `kiln/data/worlds/<world id>`.
- `static PendingRestore { get; } -> string?` — The staged restore, or null.
- `static Request() -> Task<SaveResult>` — Asks the game to save the world now (its own save, the one the 30 s timer makes).
- `static RestoreOnNextStart(string name) -> BackupInfo` — Stages a restore of backup `name` (its folder name or its given name, newest match) for the next server start...
- `static SaveDirectory { get; } -> string?` — The server's save directory (enshrouded_server.json `saveDirectory`).
- `static SnapshotStagingAvailable { get; } -> bool` — The runtime restages WorldData after the request's final simulation and before the game's pending snapshot.
- `static Status { get; } -> SaveStatus?` — Last save, interval, counters; null when unavailable.
- `static WorldDataReady { get; } -> bool` — True when `WorldData`, `PlayerData`, `CharacterData` and `EntityData` read this world's data.
- `static WorldFile { get; } -> string?` — The running world's file (`<save dir>/world0.ksd` on a dedicated server).
- `static WorldId { get; } -> string?` — The world's GUID (from the world file header, or the game's memory once it saved), as text like...

## SaveStatus — What `Saves.Status` reports.
- `SaveStatus(string State, string? WorldId, DateTimeOffset? LastSaved, uint Revision, TimeSpan? AutosaveInterval, SaveReason LastReason, TimeSpan LastDuration, long Accepted, long Written, long Failed, long Refused, bool RequestPending, int Slot, uint DataGeneration, DataMatch DataMatch, bool CanRequest, bool CanObserveWrites)`
- `Accepted { get; init; } -> long` — Saves the game accepted this session.
- `AutosaveInterval { get; init; } -> TimeSpan?` — The game's autosave interval (30 s on this build; null if unknown).
- `CanObserveWrites { get; init; } -> bool` — World writes are observed (WorldSavedEvent).
- `CanRequest { get; init; } -> bool` — The game's save request was found (Request and Backup work).
- `DataGeneration { get; init; } -> uint` — The sidecar generation Kiln last loaded or wrote (= a world revision).
- `DataMatch { get; init; } -> DataMatch` — How the loaded sidecar matched the world file.
- `Failed { get; init; } -> long` — Failed writes and timed-out requests.
- `LastDuration { get; init; } -> TimeSpan` — Accepted request to finished file, newest save.
- `LastReason { get; init; } -> SaveReason` — Reason of the newest accepted save.
- `LastSaved { get; init; } -> DateTimeOffset?` — When the newest world write of this session finished (null: none yet).
- `Refused { get; init; } -> long` — Requests the game refused (a save was running, or the world was not running).
- `RequestPending { get; init; } -> bool` — A Kiln request waits for the next step.
- `Revision { get; init; } -> uint` — The world file's newest written (or loaded) revision.
- `Slot { get; init; } -> int` — The game's world slot id (low byte = index + 1, high byte = storage).
- `State { get; init; } -> string` — "none", "loading", "running" or "saving".
- `WorldId { get; init; } -> string?` — The world's GUID text (null before a new world's first save).
- `Written { get; init; } -> long` — Successful world writes this session.

## SyncedItem `0.2.2` — One item as this game knows it from the server, with the values the server sent for it: the copies of what the server keeps with `InstanceData`.
- `Entity { get; } -> EntityId` — The item entity's id on the server.
- `FlatSlot { get; } -> int` — The flat slot in the holder, as Kiln.Items numbers it on the server: 0 to 255 are the backpack and action bar slots...
- `Has<T>(InstanceKey<T> key) -> bool` — Whether this game has a value of `key` for the item.
- `Holder { get; } -> EntityId` — The server's id of the entity whose inventory holds the item: a player, a chest, a station, an NPC.
- `Item { get; } -> ItemId` — The item type.
- `Json(string mod, string key) -> string?` — The value of key `key` of mod `mod` as the server's JSON; null when this game has none.
- `Keys { get; } -> IReadOnlyList<(string Mod, string Key)>` — The values this game has for the item, as (mod id, key name).
- `Own { get; } -> bool` — True when this game's own player holds the item.
- `TryGet<T>(InstanceKey<T> key, out T? value) -> bool` — The value of `key` for the item: a copy read from the server's JSON at each call.

## WorldData — Persistent per-world data of the calling mod (Forge's `SavedData`): typed objects under string keys, stored as JSON (System.Text.Json, public properties and fields)...
- `static Contains(string key) -> bool` — Whether the calling mod has a value under `key`.
- `static Get<T>(string key) -> T` — The object stored under `key`, created with `new T()` if there is none.
- `static Keys { get; } -> IReadOnlyList<string>` — The calling mod's keys.
- `static Loaded { get; } -> DataMatch` — Which stored generation was loaded for the world file on disk.
- `static Remove(string key) -> bool` — Removes a key (from the next save on).
- `static Set<T>(string key, T value) -> void` — Stores `value` under `key` (replacing what was there).
- `static TryGet<T>(string key, out T? value) -> bool` — Reads the object stored under `key` without creating one.
- `static WorldId { get; } -> string?` — The world these keys belong to (null before a new world's first save).

## WorldLoadedEvent — Fired on the tick thread once the server finished loading the world save (Forge's `LevelEvent.Load`).
- `Data { get; } -> DataMatch` — Which sidecar generation was loaded for this world file.
- `NewWorld { get; } -> bool` — No world file existed: a new world (its file and id appear with the first save).
- `Revision { get; } -> uint` — The world file's revision at load.
- `WorldFile { get; } -> string?` — The world file's path; null when it is not known.
- `WorldId { get; } -> string?` — The world's GUID as text; null before a new world's first save.

## WorldSavedEvent — Fired on the tick thread at the start of the step after the game finished writing the world file (or failed to).
- `Duration { get; } -> TimeSpan` — From the game accepting the request to the file being complete.
- `Reason { get; } -> SaveReason` — Why the game saved the world.
- `Result { get; } -> int` — The game's result byte (0 = ok).
- `Revision { get; } -> uint` — The world file's revision after the write.
- `Success { get; } -> bool` — True when the world file was written.
- `TimedOut { get; } -> bool` — A Kiln request the game did not accept within 60 s (nothing was written).
- `WorldFile { get; } -> string?` — The world file's path; null when it is not known.
- `WorldId { get; } -> string?` — The world's GUID as text; null when it is not known.

## WorldSavingEvent — Fired on the tick thread when the game has accepted a world save, before it snapshots the world (Forge's `LevelEvent.Save`).
- `BeforeSnapshot { get; } -> bool` — True when handlers run inside the game's request, before its snapshot (the normal case).
- `Reason { get; } -> SaveReason` — Why the game saved the world.
- `Revision { get; } -> uint` — The revision the world file will have (+0x1C of its header) once this save is written.
- `Step { get; } -> ulong` — The sim step the save request was seen in.
- `WorldFile { get; } -> string?` — The world file's path; null when it is not known yet.
- `WorldId { get; } -> string?` — The world's GUID as text; null before a new world's first save.
