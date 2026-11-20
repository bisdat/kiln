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
describe them, the side that runs the world defines them. The same options again are accepted; other options for the same
id in the same load of the mod are refused (`DefinitionConflict`). A mod's next load (a reload) defines them again.

| Field | Rule |
|---|---|
| `Kind` | Counter (may go down), Currency (0 or more, `Spend`, never carried), Experience (0 or more, scale 0, levels) |
| `Label`, `NumberFormat` | `UiText` per language. Formats: N0-N6, F0-F6, `0`, `0.0` to `0.000000` (default N0, or N<scale>) |
| `Min`, `Max`, `Initial` | 64-bit; `Initial` defaults to `Min` (a counter with Min 10 starts at 10) and must be within the bounds |
| `Scale` | 0-6 fixed decimal places: raw 1234 at scale 2 is 12.34. Formatting is exact (decimal, never a double) |
| `Thresholds` | experience only: from 0, strictly increasing, at most 256. Level = how many thresholds the value reached |
| `Persistence` | World (this world's record) or Carried (travels with the character, see below) |
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
- Every initialization, write, migration, import and reset posts `ModValueChangedEvent` (cause, before, after).
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
process.

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
authority). These keys are reserved: do not use them in your own CharacterData. A stored object that is not such a record
is held, never overwritten.

## Capability rows
| Row | Side | What |
|---|---|---|
| `stats.definitions` | both | `Describe`, `Definition` |
| `stats.own` | server (a hosted world on a client: session) | the ledger: `Define`, `Read`, `Add`, `Spend`, `Set`, `Reset`; needs kiln.saves |
| `stats.sync` | client | `Synced`; needs kiln.net |
| `stats.ui` | client | the stat source of UI blocks (package 09); needs kiln.ui |

## Native facade
`kiln.mod-values` v1 (`native/core/abi/services/kiln_mod_values.h`): the same ledger for native mods and other
engines, owners mapped to namespaces (a managed mod's id; a native owner's `native.` + 32 hex digits of SHA-256 of its
registry name). See the header for the slots and rules.

## Test kit
`kiln.ModValues` (`HarnessModValues`): the real ledger on an in-memory store; `Join`/`Leave` a character
(`HarnessModValues.Character(n)`), `World`, `Record(character, mod, partition)`. `ModValues.Define`, `Add` and the
rest work in `kiln.Run`.
