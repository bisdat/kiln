# Kiln mod values (mod-values/*.json), format 1

<!-- Generated from schema/kiln-mod-values.schema.json by kiln_manage::lint::docs (cargo test -p kiln-manage --test lint with KILN_LINT_WRITE=1). Do not edit. -->

A mod's own character values: counters, currencies and experience values with levels (dossier chapter 05b, section 4.1). Kiln.ModValues reads every mod-values/*.json of a mod when the mod loads: both sides describe the definitions (their names, labels and number formats are known before any handshake, and before the mod's UI blocks), and the server defines them, which makes the values live for each character. Strict JSON: no comments, no trailing commas, unknown keys and a key given twice are refused. Every 64-bit number (min, max, initial, thresholds) is a base-10 string, so no JSON reader rounds it. The same fields, in C#, are the options of Kiln.ModValues' Describe and Define. Beyond the schema, kiln lint checks the 64-bit range, min <= max, initial within [min, max], thresholds that strictly increase, the size limits of labels, number formats and a definition, and ids across all of a mod's mod-values files (at most 128).

- Files: `**/mod-values/*.json`
- JSON: comments **not allowed**, trailing commas **not allowed**
- Schema: [`schema/kiln-mod-values.schema.json`](../../../../schema/kiln-mod-values.schema.json); rules: [lint.md](../lint.md)

## Example

```json
{
  "format": 1,
  "values": {
    "tokens": {
      "initial": "10",
      "kind": "currency",
      "label": "Tokens",
      "max": "1000000",
      "min": "0",
      "numberFormat": "N0",
      "persistence": "world",
      "readPolicy": "ownerOnly",
      "scale": 0,
      "sync": "ownerMod"
    },
    "xp": {
      "allowPartitionMove": false,
      "carryEpoch": 1,
      "initial": "0",
      "kind": "experience",
      "label": {
        "de": "Angel-EP",
        "en": "Fishing XP"
      },
      "max": "9223372036854775807",
      "min": "0",
      "numberFormat": {
        "*": "N0"
      },
      "persistence": "world",
      "readMods": [],
      "readPolicy": "public",
      "resetOnKindChange": false,
      "scale": 0,
      "sync": "readers",
      "thresholds": [
        "0",
        "100",
        "400",
        "900"
      ],
      "trustClientImports": false,
      "version": 1
    }
  }
}
```

## Beyond the schema

What kiln lint checks that the schema cannot say:

- [KL350](../lint.md#kl350-mod-value-number): min, max, initial and every threshold fit a signed 64-bit integer (-9223372036854775808..9223372036854775807).
- [KL351](../lint.md#kl351-mod-value-bounds): min <= max, and initial within [min, max], defaults included (min "0", max "9223372036854775807", initial = min). Kiln refuses a definition otherwise; it never clamps.
- [KL352](../lint.md#kl352-mod-value-thresholds): thresholds strictly increase (compared as whole numbers, not as text or floating point).
- [KL353](../lint.md#kl353-mod-value-size): a definition's label is at most 1024 bytes, its numberFormat at most 256 bytes and the whole definition at most 4096 bytes, each as compact JSON in UTF-8.
- [KL354](../lint.md#kl354-mod-value-duplicate): an id is defined once per mod, across all its mod-values/*.json files (inside one file a key given twice is [KL004](../lint.md#kl004-duplicate-key), an error here).
- [KL355](../lint.md#kl355-mod-value-count): at most 128 values per mod, across all its mod-values/*.json files (one file over 128 is [KL108](../lint.md#kl108-size)).

## Top level

| Key | Type | Required | Notes |
|---|---|---|---|
| `$schema` | string |  |  |
| `format` | `1` | yes |  |
| `values` | object | yes | The mod's value definitions by id. An id is the value's name inside the mod; other mods and native code name it "<mod>:<id>". Ids are unique across all of a mod's mod-values files, at most 128 per mod. |

## definition

One value. Defaults: min "0", max "9223372036854775807", initial = min, scale 0, persistence world, readPolicy ownerOnly, sync ownerMod, version 1, carryEpoch 1. kiln lint also checks min <= max and that initial is within [min, max] (Kiln never clamps it), and that thresholds strictly increase.

| Key | Type | Required | Notes |
|---|---|---|---|
| `allowPartitionMove` | boolean |  | Allow a change of persistence (world <-> carried) between versions to move the stored values. Default `false`. |
| `carryEpoch` | [uint32](#uint32) |  | Raise it to stop accepting carried values from before. Default `1`. |
| `initial` | [int64](#int64) |  | A character's first value. Default: min. Must be within [min, max]: Kiln refuses it otherwise, it never clamps. |
| `kind` | `"counter"` \| `"currency"` \| `"experience"` | yes | counter: any whole number in [min, max]. currency: spent and earned, never below 0. experience: points with levels ("thresholds"), never below 0, scale 0. |
| `label` | [text](#text) | yes | The name players see. |
| `max` | [int64](#int64) |  | The highest value. Default "9223372036854775807". |
| `min` | [int64](#int64) |  | The lowest value. Default "0"; at least "0" for a currency, an experience value and a carried value. |
| `numberFormat` | [numberFormat](#numberformat) |  |  |
| `persistence` | `"world"` \| `"carried"` |  | world: kept in the world's save. carried: travels with the character to other worlds, taken from the player's client when they join; needs trustClientImports true, and only for a counter or an experience value with min at least 0 (a currency is never carried). Default `"world"`. |
| `readMods` | array of `common.schema.json modId` |  | The mod ids allowed to read the value, with readPolicy allowList only (empty otherwise). max items 32 |
| `readPolicy` | `"ownerOnly"` \| `"public"` \| `"allowList"` |  | Which mods may read the value: the defining mod only, every loaded mod, or the mods of readMods. No other mod can ever write it. Default `"ownerOnly"`. |
| `resetOnKindChange` | boolean |  | When kind changes between versions, reset stored values to initial instead of refusing the definition. Default `false`. |
| `scale` | integer |  | Decimal places: the stored whole number is shown divided by 10^scale (scale 2: "1250" shows as 12.50). 0 for an experience value. Default `0`. min 0 max 6 |
| `sync` | `"none"` \| `"ownerMod"` \| `"readers"` |  | What the character's own client gets: none (the value stays on the server), ownerMod (the defining mod's client code may read it), readers (also the mods readPolicy allows). Default `"ownerMod"`. |
| `thresholds` | array of [int64](#int64) |  | kind experience only, and required there: the points at which each level starts, "0" first, strictly increasing. Level = 1 + the index of the greatest threshold reached. min items 1 max items 256 |
| `trustClientImports` | boolean |  | Accept a carried value a client brings. Required (true) for persistence carried. Default `false`. |
| `version` | [uint32](#uint32) |  | The definition's version: raise it when the meaning of the stored number changes. Default `1`. |

## id

a value id: a lower-case letter, then up to 63 of a-z, 0-9, '.', '_' and '-'

Type: string

## int64

A whole number as a base-10 string: an optional '-', no '+', no leading zeros, no spaces, within -9223372036854775808..9223372036854775807 (kiln lint checks the range). With "scale" s the value shown is this number / 10^s.

Type: string

## notNegative

Not negative: no leading '-' (the form is int64's). Pattern `^(?!-)`.

## numberFormat

How the value is shown: N0..N6 (with group separators), F0..F6, "0", or "0." followed by 1 to 6 zeros; or one per language ({"*": "N0"}). The game's selected culture formats it, en-US when that fails. Default N0 for scale 0, N<scale> otherwise. At most 256 bytes per definition as compact JSON in UTF-8 (kiln lint checks it).

Type: [numberFormatValue](#numberformatvalue) or object

## numberFormatValue

a number format: N0..N6, F0..F6, "0", or "0." followed by 1 to 6 zeros

Type: string

## text

A string, or a text per language ({"en": "...", "de": "..."}, "*" for any other). At most 1024 bytes per definition as compact JSON in UTF-8 (kiln lint checks it).

Type: string or object

## uint32

min 1 max 4294967295

Type: integer

