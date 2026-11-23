# Assets (Kiln.Assets)

## AssetKey — A game content key: a resource GUID, a type and a part index.
- `AssetKey()` — A key by a reflection type name (e.g. "keen::RenderModel"); the hash is FNV-1a-32.
- `AssetKey(Guid guid, string typeName, uint part = 0)` — A key by a reflection type name (e.g. "keen::RenderModel"); the hash is FNV-1a-32.
- `AssetKey(Guid guid, uint type, uint part = 0)` — A key by a reflection type name (e.g. "keen::RenderModel"); the hash is FNV-1a-32.
- `Guid { get; } -> Guid`
- `const ItemInfoType = 3050211173 -> uint`
- `Part { get; } -> uint`
- `Type { get; } -> uint`
- `const UiTextureType = 4192758167 -> uint`

## Assets — Mod resources, overrides, icons and retextures (`kiln.assets`).
- `static AddBlob(byte[] bytes) -> ContentHash` — Add a new blob; the returned hash goes into a resource's content reference. kiln.assets records it as a managed...
- `static Available { get; } -> bool` — True when kiln.assets is loaded and its hooks are on (a verified build or policy "any"; capability `kiln.assets`).
- `static Dump(string spec) -> string` — Write resources / blobs to \<game>/kiln/data/assets/dump/.
- `static IconStateOf(Guid itemGuid) -> IconState` — The UI state of an icon: used by content code to decide whether to alias to a base item.
- `static ListAllBlobs() -> IReadOnlyList<BlobEntry>` — The blobs every mod has supplied through this class (see `Assets.ListBlobs`), oldest first.
- `static ListBlobs() -> IReadOnlyList<BlobEntry>` — The content blobs this mod added with `Assets.AddBlob` or overrode with `Assets.OverrideBlob` and has not removed...
- `static ListResources(uint type) -> AssetKey[]` — Every key of a type (the game's plus new ones).
- `static LoadManifest(string modDirectory) -> string` — Register every entry of a mod folder's assets/assets.json under this mod.
- `static OpenBlobCatalog(BlobCatalogMode mode = BlobCatalogMode.RequireComplete) -> BlobCatalogOpenResult` `0.2.3` — A snapshot of every content blob the game serves: the game's own content index and every live registration...
- `static Override(AssetKey key, byte[] bytes) -> void` — Override a game resource's bytes for the session.
- `static OverrideBlob(ContentHash hash, byte[] bytes) -> void` — Serve 'bytes' instead of the game's blob 'hash' (bytes.Length must equal hash.Size).
- `static OverrideTexture(AssetKey key, byte[] png) -> void` — Override a texture resource's pixels with a PNG, encoded to the texture's own format.
- `static ReadBlob(ContentHash hash) -> byte[]?` `0.2.3` — The bytes the game reads for a content blob now: a registration's, else the game's own from its container...
- `static ReadResidentBlob(ContentHash hash) -> byte[]?` `0.2.8` — `Assets.ReadBlob` for any thread, a worker or a task included: the bytes of a resident content blob, without ever...
- `static ReadResource(AssetKey key) -> byte[]?` — The bytes the game gets for 'key' now (an override if any, else the game's own), or null if absent.
- `static Reflect(AssetKey key) -> ReflectedResource?` — The resource as a reflected struct: read any field by path, change one copy-on-write (see `ReflectedResource`).
- `static Register(AssetKey key, byte[] bytes, bool? mustBeNew = null) -> void` — Serve 'bytes' for 'key'. mode: null = either, true = must be new, false = must override a game resource.
- `static RegisterIcon(Guid itemGuid, byte[] png) -> void` — Register a PNG as the inventory / crafting icon of the item with this GUID.
- `static RegisterIcon(Guid itemGuid, string pngPath) -> void` — Register an icon from a PNG file (absolute, or relative to \<game>/kiln).
- `static RemoveBlob(ContentHash hash) -> void`
- `static RemoveIcon(Guid itemGuid) -> void` — Remove this mod's icon for a GUID.
- `static RemoveResource(AssetKey key) -> void` — Remove this mod's resource / override for a key.
- `static Status() -> string` — A JSON status string (hooks, counts, container, reads).
- `static TypeHash(string name) -> uint` — FNV-1a-32 of a reflection type name, e.g. "keen::UiTextureResource".

## AssetsCatalogChange `0.2.3` — What changed the catalog (`AssetsCatalogChangedEvent.Reason`).
enum: None, Ready, Registration, Remove, Revoke

## AssetsCatalogChangedEvent `0.2.3` — The content blob catalog changed: a blob was registered or a registration removed, a mod's registrations went with it, or the game's content index became readable.
- `Generation { get; } -> ulong` — The catalog's generation now (`BlobCatalogInfo.Generation` of a snapshot opened now).
- `Reason { get; } -> AssetsCatalogChange` — The last change.

## BlobCatalogEntry `0.2.3` — One content blob of a catalog snapshot.
- `BlobCatalogEntry(ContentHash ContentHash, BlobOrigins Origins, BlobOrigins SelectedOrigin, ulong Owner, ulong Sequence, bool Overridden)`
- `ContentHash { get; init; } -> ContentHash` — The blob's hash.
- `Origins { get; init; } -> BlobOrigins` — Every origin contributing it: `BlobOrigins.Game` when the game's index lists it (also when a mod overrides it), plus...
- `Overridden { get; init; } -> bool` — The game lists the hash and a registration's bytes are served in place of the game's.
- `Owner { get; init; } -> ulong` — The served registration's native owner; 0 for the game's own.
- `SelectedOrigin { get; init; } -> BlobOrigins` — Whose bytes are served now: one origin; `BlobOrigins.Game` for the game's own.
- `Sequence { get; init; } -> ulong` — The served registration's number (kiln.assets counts every registration); 0 for the game's own.

## BlobCatalogInfo `0.2.3` — A catalog snapshot's header (`BlobCatalogSnapshot.Info`), or why an open was refused (`BlobCatalogOpenResult.Info`).
- `BlobCatalogInfo(ulong Generation, ulong Total, BlobCatalogStatus Status, bool GameComplete, bool RegistrationsComplete, BlobCatalogReason Reason)`
- `GameComplete { get; init; } -> bool` — The game's whole content index is in it.
- `Generation { get; init; } -> ulong` — The catalog's generation the snapshot was built at; it moves on every registration, removal, mod unload and...
- `Reason { get; init; } -> BlobCatalogReason` — Why it was refused; `BlobCatalogReason.None` with a snapshot.
- `RegistrationsComplete { get; init; } -> bool` — Every live registration is in it.
- `Status { get; init; } -> BlobCatalogStatus` — Complete, incomplete, or why there is no snapshot.
- `Total { get; init; } -> ulong` — Entries in the snapshot; 0 without one.

## BlobCatalogMode `0.2.3` — What `Assets.OpenBlobCatalog` builds.
enum: RequireComplete, RegistrationsOnly

## BlobCatalogOpenResult `0.2.3` — What `Assets.OpenBlobCatalog` answered: a snapshot, or (`BlobCatalogOpenResult.Ok` false) a budget refusal with its reason.
- `BlobCatalogOpenResult(bool Ok, string? Reason, BlobCatalogSnapshot? Snapshot, BlobCatalogInfo Info)`
- `Info { get; init; } -> BlobCatalogInfo` — The snapshot's header, or the refusal's status and reason.
- `Ok { get; init; } -> bool` — A snapshot was opened.
- `Reason { get; init; } -> string?` — Why not, in words; null when `BlobCatalogOpenResult.Ok`.
- `Snapshot { get; init; } -> BlobCatalogSnapshot?` — The snapshot (dispose it); null when not `BlobCatalogOpenResult.Ok`.

## BlobCatalogReason `0.2.3` — Why a catalog open was refused (`BlobCatalogInfo.Reason`).
enum: None, GameNotReady, UnreadableMap, EntryBudget, MemoryBudget, UnsupportedTable

## BlobCatalogSnapshot `0.2.3` — An immutable snapshot of the content blob catalog: it never changes, whatever is registered after it was opened (open a new one to see that).
- `Dispose() -> void` — Closes the snapshot.
- `Info { get; } -> BlobCatalogInfo` — The snapshot's header: generation, total, status and completeness.
- `const MaxPage = 256 -> int` — The most entries `BlobCatalogSnapshot.ReadPage` copies at once.
- `ReadPage(ulong first, int capacity = 256) -> IReadOnlyList<BlobCatalogEntry>` — Entries `first` onward, at most `capacity`, sorted by (size, hash0, hash1, hash2): bounded copies, never borrowed...
- `Total { get; } -> ulong` — Entries in the snapshot.

## BlobCatalogStatus `0.2.3` — What a catalog open answered (`BlobCatalogInfo.Status`).
enum: Complete, Incomplete, NotReady, LimitExceeded, Unavailable

## BlobEntry — A content blob a mod supplied through `Assets` (see `Assets.ListBlobs`).
- `BlobEntry()`
- `BlobEntry(ContentHash Hash, BlobKind Kind, string Mod)`
- `Hash { get; init; } -> ContentHash` — The blob's content hash.
- `Kind { get; init; } -> BlobKind` — New blob, or an override of the game's.
- `Mod { get; init; } -> string` — The id of the mod that supplied it.

## BlobKind — How a mod supplied a content blob: new bytes under a new hash, or bytes served instead of the game's blob.
enum: Added, Override

## BlobOrigins `0.2.3` — Where the bytes of a content blob come from (`BlobCatalogEntry.Origins`).
enum: None, Game, Manifest, Managed, NativeApi

## ContentHash — A content blob hash (keen::ContentHash): the identity of a streamed blob (texture pixels, mesh data).
- `Equals(ContentHash other) -> bool`
- `H0 { get; } -> uint`
- `H1 { get; } -> uint`
- `H2 { get; } -> uint`
- `static operator !=(ContentHash a, ContentHash b) -> bool`
- `static operator ==(ContentHash a, ContentHash b) -> bool`
- `Size { get; } -> uint`

## IconState — The UI state of a registered icon (see `Assets.IconStateOf`).
enum: None, Pending, Served, Failed
