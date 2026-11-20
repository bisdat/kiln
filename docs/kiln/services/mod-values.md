# Mods' own character values (`Kiln.ModValues`)

`managed/services/Kiln.ModValues/` (0.2.0-dev.31; dossier chapter 05b). A mod gives each character values of its own: a
**counter**, a **currency**, or **experience** with levels. The server owns them, saves them with the character
(CharacterData, in the world's sidecar) and sends the owning player's Kiln client a read-only copy. A vanilla client sees
nothing of them and gets no frames. Tests: `managed/tests/Kiln.ModValues.Tests`; lab: `mods/tests/mod-values-test`
(`mvnet.py`); sample: `mods/samples/ModValues` ("Fishing"); guided check `05b-stats-ui`.

These are Kiln values, not the game's attributes: `Progression.AddExperience` still grants the game's own experience,
and a mod value changes no game formula by itself (apply its effect through the gameplay APIs). The game's inventory header
and character sheet do not show them: a mod shows them in its own window or HUD line, or on an item through
`Ui.Tooltip.AddLines` (the sample does all three).

## Defining values
In C# (`ModValueOptions`) or in `mod-values/*.json` ([schema](../modding/schemas/kiln-mod-values.md), lint KL350-KL355):

```csharp
// both sides (for UI): the names, kinds and formats are known before any server answers
ModValues.Describe("xp", new ModValueOptions(ModValueKind.Experience, UiText.Of(("en", "Fishing XP"), ("de", "Angel-EP")),
    Thresholds: [0, 100, 400, 900], ReadPolicy: ModValueReadPolicy.Public, Sync: ModValueSyncPolicy.Readers));
// server (OnLoad or later, tick thread): active; characters in game get it at its initial value now
ModValues.Define("tokens", new ModValueOptions(ModValueKind.Currency, "Tokens", Max: 1_000_000, Initial: 10));
```

```json
{"format": 1, "values": {
  "tokens": {"kind": "currency", "label": "Tokens", "numberFormat": "N0", "min": "0", "max": "1000000", "initial": "10"}}}
```

Kiln reads every loaded mod's `mod-values/*.json` (strict JSON, file-name order, 64-bit numbers as strings): both sides
describe them, the side that runs the world defines them; a game client that starts hosting a world (single player, a
co-op host) defines them then, and a mod reloaded later is read again. Call `Define` in `OnLoad`: on a dedicated
server it is active at once; on a game client that runs no world yet (a both-side mod in the main menu) the value is
described and kept, and it becomes active when that client hosts a world. The same options again are accepted; other options for the same
id in the same load of the mod are refused (`DefinitionConflict`). A mod's next load (a reload) defines them again.

| Field | Rule |
|---|---|
| `Kind` | Counter (may go down), Currency (0 or more, `Spend`, never carried), Experience (0 or more, scale 0, levels) |
| `Label`, `NumberFormat` | `UiText` per language. Formats: N0-N6, F0-F6, `0`, `0.0` to `0.000000` (default N0, or N<scale>) |
| `Min`, `Max`, `Initial` | 64-bit; `Initial` defaults to `Min` (a counter with Min 10 starts at 10) and must be within the bounds |
| `Scale` | 0-6 fixed decimal places: raw 1234 at scale 2 is 12.34. Formatting is exact (decimal, never a double) |
| `Thresholds` | experience only: from 0, strictly increasing, at most 256. Level = how many thresholds the value reached |
| `Persistence` | World (this world's record) or Carried (travels with the character, see below; give a carried value a real `Max`: lint KL356 warns without one) |
| `Version` | raise it when kind, bounds, initial, scale, thresholds or persistence change (see Migration) |
| `ReadPolicy`, `ReadMods` | which mods read it: OwnerOnly, Public, AllowList (at most 32). Cooperative, not a sandbox |
| `Sync` | None (never sent), OwnerMod (sent; on the client only the owning mod reads it), Readers (the read policy decides) |

Limits: ids `[a-z][a-z0-9._-]{0,63}`, 128 definitions per mod, label 1024 bytes, number formats 256 bytes, a definition
4096 bytes (compact UTF-8 JSON).

## Reading and changing (server)
`Read`, `Add`, `Spend`, `Set` and `Reset` take a `CharacterKey` (`CharacterData.KeyOf(player)`) and a key
(`ModValues.Key("id")` for your own value, `ModValues.Key("other-mod:id")` to read another mod's). Server side, tick
thread, row `stats.own`.

- Only the defining mod writes its values (`AccessDenied` otherwise, whatever the read policy). A key is a name, never a
  permission.
- Writes need the character in game (`CharacterUnavailable` otherwise); reads also work for offline characters.
- An account-only key (a build without the game's character id) is `CharacterUnavailable`: values are never shared by
  the characters of one account.
- Results follow the one error rule: `Ok = false` and a reason from `ModValueReasons` (`InsufficientFunds`,
  `BoundExceeded`, `StaleRevision`, `NotReady`, `Undefined`, `DefinitionRetired`, `MigrationRequired`,
  `NonMonotonic`, `EpochRequired`, `Budget`) and nothing changed; a bad argument (a negative `Spend`, `Spend` on a
  counter, lowering a currency with `Add`) throws `ArgumentException`; an off row throws `KilnUnavailableException`.
- Adding 0 is accepted and changes nothing (no revision, no event). Arithmetic is checked: an overflow is
  `BoundExceeded` with no change.
- Every value has a revision per world (`ModValueSnapshot.Version`); pass it back as `expected` for compare-and-set.
  Revision 0 is a real revision (a fresh value): the world id in it keeps another world's revision 0 from matching.
- Every initialization, write, migration, import and reset posts `ModValueChangedEvent` (cause, before, after). Like
  every event on Kiln's bus it reaches every subscriber, whatever the value's read policy: the read policy guards
  `Read`, `Synced` and the native facade (whose subscriptions are filtered per subscriber), not the managed event
  bus. Filter your handlers by `Key.Mod`, and keep a value that must stay with its mod at `Sync = None` and out of
  other mods' event handlers by not relying on the policy for secrecy.
- `Spend` is not a transaction with anything else: when what you sell then fails, give the amount back (the sample's
  `fishing-buy-bait`).
- Players change values only through your server code: a command (`[Command]`, its permission node
  `<mod>.command.<name>` applies), or a request on your own channel whose handler takes the character from
  `ctx.Player`. There is no client write and no generic grant.

## The player's copy (client)
`ModValues.Synced(key)` (client, any thread, row `stats.sync`) reads the copy the server sent this player over
channel `kiln:mod-values` (optional; six JSON messages, chunked atomic full and delta transfers, acks and resyncs). The
caller is the mod whose code runs on the thread (its handlers, its window and HUD callbacks): the owning mod reads its
OwnerMod values; others read Readers values their read policy allows. `NotSynced` before the copy arrived, for a value
with `Sync = None`, and after leaving the world (the copy is cleared then). `ModValueSyncedEvent` per changed value,
then `ModValuesSnapshotPublishedEvent`. `ModValues.Format(snapshot)` gives the text players see, in the game's language;
`ModValues.Definition(key)` the label and format. In a hosted game the host's own copy runs through the same code in
process, and is cleared when the host leaves its world. Window draws and HUD line callbacks run as the mod that made them,
so a mod's HUD line reads its own OwnerOnly values.

## Carried values
`Persistence = Carried` is for progress that travels: experience, or a counter with Min 0 or more, with
`TrustClientImports = true`. A currency never travels. Carried values only grow (`NonMonotonic`); `Reset` is refused
(`EpochRequired`): raise `CarryEpoch` to reset carried progress everywhere. The player's Kiln client keeps a copy next
to its character (CharacterSync's sidecar) and offers it once per stream; the server takes, per value, the exact maximum
of its own and the offered one (after rescaling to the current scale and clamping), never a sum. A server with
`importProgress: false` in `kiln/config/characters.json` takes none, and `CharacterImportingEvent` (reserved keys
only) or `ModValueImportingEvent` can refuse an offer. The copy is client-side and editable: never carry anything a
server must be able to trust.

## Migration, removal
A definition's version and fingerprint are saved with each value. The same version and content: the value is used as it
is. Another content at the same or a lower version: the value is held (`MigrationRequired`; nothing changes, nothing is
sent) until a higher version comes. A higher version converts: scale (raw times or divided by powers of ten, toward zero),
clamped to the new bounds, levels from the new thresholds; a kind change holds the value unless `ResetOnKindChange`; a
persistence change holds it unless `AllowPartitionMove` (moved, never copied). Labels, formats, read and sync policies
change without a version. A mod that unloads parks its values (`DefinitionRetired`); they come back when it defines them
again. Nothing deletes a saved value.

## Storage
Per character and owning mod: CharacterData `mod-values.world.v1` (World values), CharacterData
`mod-values.carried-authority.v1` (accepted Carried values), CharacterData.Carried `mod-values.carried.v1` (a projection
for the transport, rebuilt from the authority at start, before each save and after a carried import; never read as
authority). These keys are reserved: mods' own CharacterData calls refuse them (`ArgumentException`) and `Keys` does not
list them. A stored object that is not such a record is held, never overwritten.

The copy is rewritten only for characters that have a carried record in this world, and never below what the player's
own copy holds for the same value (kind, scale and epoch): a world that does not hold the mod, a world the player's offer
was refused by, or a first visit before the offer is merged passes the player's copy on unchanged.

A new world has no id until its first save. Until then its values use a provisional world id for the session (the
records are in memory, as all of a new world's mod data is, and go to disk with that save); the first save gives the
real id, the values stay, the stream starts over with a new nonce, and a compare-and-set made with a version from before
the save is refused once as `StaleRevision`.

## Capability rows
| Row | Side | What |
|---|---|---|
| `stats.definitions` | both | `Describe`, `Definition` |
| `stats.own` | server (a hosted world on a client: session) | the ledger: `Define`, `Read`, `Add`, `Spend`, `Set`, `Reset`; needs kiln.saves |
| `stats.sync` | client | `Synced`; needs kiln.net |
| `stats.ui` | client | the stat source of UI blocks (package 09); needs kiln.ui |

## Native facade
`kiln.mod-values` v1 (`native/core/abi/services/kiln_mod_values.h`, in the core): the same ledger, registry and synced
copy for native mods and other engines. Kiln.ModValues hands the core its backend at its start (the first service tick)
through the host-only table `kiln.mod-values.backend` v1; before that, or without Kiln.ModValues, every slot but
`owner_namespace` answers `KILN_ERR_UNAVAILABLE` (call from a tick and try again on a later one).

- `api->service("kiln.mod-values", 1)`; `owner_namespace(owner, buf, cap, &required)` gives your namespace: a managed
  mod's manifest id, else `native.` and the first 32 hex digits of the SHA-256 of the owner's registry name (a plugin:
  `plugin:<file>.dll`; a native mod: `native:<id>`). `required` counts the NUL; a short buffer gets nothing.
- Keys are `"<namespace>:<id>"`. `describe` any thread; `define`, `read`, `change` on the server's tick thread
  (`KILN_ERR_WRONG_THREAD` elsewhere); `read_synced` on a client, any thread.
- `KilnModValueDef`: `size`, `version >= 1`, `carry_epoch >= 1`; `label_json` and `number_format_json` are UiText JSON
  (`"\"Coins\""` or `{"en":"Coins"}`); `KMV_HAS_INITIAL` says `initial` is used (0 is a real initial value);
  `KMV_TRUST_IMPORTS` for Carried. Strings and arrays are copied.
- Results: `KILN_OK` with `ok = 0` is a refusal with a `KMV_*` reason (the managed `ModValueReasons`, plus
  `KMV_DEFINITION_CONFLICT` = 15); `flags`: a read gives AFTER, an accepted change BEFORE|AFTER, a refusal 0. Bad
  shapes are `KILN_ERR_INVALID_ARGUMENT`, a revoked owner `KILN_ERR_REVOKED`.
- Characters are `{account key, CharacterId in RFC 4122 byte order}` (not `Guid.ToByteArray`'s order); world GUIDs too.
- `change`: ADD (signed for a World counter, else 0 or more), SPEND (currency, positive), SET (absolute raw), RESET
  (refused for Carried); compare-and-set with `KMV_EXPECT_REVISION` and `{world, value_revision}` (revision 0 is valid).
- `subscribe(owner, KMV_CANONICAL_EVENTS | KMV_SYNCED_EVENTS, filter, fn, user, &handle)`: filter `""` (your namespace),
  `"ns:id"` or `"ns:*"`; another namespace's events only while its read policy lets you read it (synced: also sync
  Readers). Canonical callbacks on the server tick, synced ones on the client's main thread; records are borrowed until
  the callback returns; a faulting callback is removed. Revoking the owner removes its subscriptions and retires its
  definitions (the data stays parked).

## Test kit
`kiln.ModValues` (`HarnessModValues`): the real ledger on an in-memory store; `Join`/`Leave` a character
(`HarnessModValues.Character(n)`), `World`, `Record(character, mod, partition)`. `ModValues.Define`, `Add` and the
rest work in `kiln.Run`.
