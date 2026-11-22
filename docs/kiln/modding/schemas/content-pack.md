# Kiln data pack (content/*.json), format 1

<!-- Generated from schema/content-pack.schema.json by kiln_manage::lint::docs (cargo test -p kiln-manage --test lint with KILN_LINT_WRITE=1). Do not edit. -->

kiln.content's and kiln.gamedata's data packs: rules over the game's registries (items, recipes, loot, drops, templates, arsenals, sequences, buffs, perks, skills, itemdata), typed gear values (gear, gearRules), new items / recipes and gamedata clones ("add" lists), and sections of other services (creatures). Read by native/services/content (nlohmann::json with comments allowed; trailing commas are NOT allowed). Field paths come from the game's reflection at run time, so this schema checks the document's shape; `kiln lint` checks names and paths against the game-data catalog. Docs: docs/kiln/services/content.md, gamedata.md, creatures.md.

- Files: `**/content/*.json`
- JSON: comments allowed, trailing commas **not allowed**
- Schema: [`schema/content-pack.schema.json`](../../../../schema/content-pack.schema.json); rules: [lint.md](../lint.md)

## Top level

| Key | Type | Required | Notes |
|---|---|---|---|
| `abilities` |  |  | Reserved for Kiln.Abilities (a known section name; no service reads it yet). |
| `arsenals` | [arsenalsSection](#arsenalssection) |  |  |
| `buffs` | [buffsSection](#buffssection) |  |  |
| `comment` |  |  | Free text, ignored. |
| `creatures` | [creaturesSection](#creaturessection) |  |  |
| `drops` | [dropsSection](#dropssection) |  |  |
| `format` | `1` |  | 1 (optional). Another value refuses the whole file. |
| `gear` | [gearSection](#gearsection) |  |  |
| `gearRules` | [gearRules](#gearrules) |  |  |
| `itemdata` | [itemdataSection](#itemdatasection) |  |  |
| `items` | [itemsSection](#itemssection) |  |  |
| `loot` | [lootSection](#lootsection) |  |  |
| `options` | [options](#options) |  |  |
| `perks` | [perksSection](#perkssection) |  |  |
| `recipes` | [recipesSection](#recipessection) |  |  |
| `registry` | `registry-section.schema.json` |  | Edits of Kiln registries (experimental, KILNX031): every mod's add, then alter, then final, by Kiln ids and Kiln fields (docs/kiln/modding/registries/README.md). Read by the registry stage at start, not applied with the pack's other sections; fields that apply now or at world load follow a pack's hot reload. |
| `sequences` | [sequencesSection](#sequencessection) |  |  |
| `skills` | [skillsSection](#skillssection) |  |  |
| `templates` | [templatesSection](#templatessection) |  |  |

## arsenalsSection

keen::enemy::EnemyArsenalResource (guid, attacks[], behaviors[]); selectors: an arsenal GUID or template:<template name>.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` |  |  | arsenals cannot be cloned (templates, sequences, perks, buffs can) |

## buffsSection

keen::BuffType (defaultLifeTime, slot, applyType, impact, impactValues, uiValues); selectors: 0x<buff type id>, buff_<id>, a clone's name or GUID.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [cloneDefinition](#clonedefinition) |  |  |

## cloneDefinition

A clone with a fixed GUID (never change a released one), a name and a base of the same section; fields are layers of the "gamedata definitions" owner.

| Key | Type | Required | Notes |
|---|---|---|---|
| `addComponents` | array of string |  | Templates: components of the game's types the clone has and its base hasn't, zeroed; its fields fill them in. |
| `base` | string | yes | min length 1 |
| `description` | string |  |  |
| `displayName` | string |  |  |
| `fields` | object |  |  |
| `guid` | [fixedGuid](#fixedguid) | yes |  |
| `name` | [cloneName](#clonename) | yes |  |
| `placeholder` |  |  | "placeholder" is written by Kiln for missing gamedata Kiln warns (not refused). |
| `removeComponents` | array of string |  | Templates: components of the base the clone does without, such as a creature's gear (SubEntities) on another body. A component the base hasn't, CurrentTransform, and an actor's own are refused. |
| `texts` | object |  |  |

## cloneName

a clone's name is 1..128 of letters, digits, _ - .

Type: string

## creaturesSection

kiln.creatures' variants and natural-spawn rules (Creatures.ApplyJson). Docs: docs/kiln/services/creatures.md#data-packs-the-creatures-section.

| Key | Type | Required | Notes |
|---|---|---|---|
| `spawnRules` | array of object |  |  |
| `variants` | array of object |  |  |

## damageKind

Type: `"All"` \| `"Physical"` \| `"Cut"` \| `"Pierce"` \| `"Blunt"` \| `"Wood"` \| `"Stone"` \| `"Metal"` \| `"Poison"` \| `"Explosion"` \| `"SkillShot"` \| `"Magical"` \| `"MagicalBase"` \| `"MagicalAdder"` \| `"MagicalMultiplier"` \| `"Fire"` \| `"Fog"` \| `"Ice"` \| `"Lightning"`

## definitionStack

An item name, or {item, count} / {category: "0x...", count} (inputs only).

Type: string or object

## dropsSection

Default-inventory tables (enemy loot, fixed chests): { "<table GUID \| glob>": { "count": op, "chance": op, "items": { "<item>": { "countMin": ... } } } }.

Type: object

## enemyFeature

an enemy feature: [+\|-]UseTaunt, Move, Patrol, Roaming, ReturnHome, FleeHome, Combat, Run, Sleep, Awareness, Investigate, ... (EnemyFeatures)

Type: string

## faction

Type: `"Environment"` \| `"HitAll"` \| `"Fogger"` \| `"Vukah"` \| `"Scavenger"` \| `"Wildlife"` \| `"WildBeast"` \| `"Player"` \| `"AutomatedPlayer"` \| `"HitOthers"` \| `"Skeleton"` \| `"PlayerAlly"` \| `"FarmAnimal"` \| `"Cyclops"` \| `"Drak"` \| `"Ancients"` \| `"None"`

## fields

Field values: paths of the registry's reflection (Content.Dump() lists them).

Type: object

## fixedGuid

A definition's fixed GUID: pick it once (uuid4) and never change it; the id is FNV-1a of its 16 bytes and saves store the id.

## gdRule

Paths of the game's reflection -> values. @where: conditions on original values (any path) and name (a glob).

| Key | Type | Required | Notes |
|---|---|---|---|
| `@refs` | boolean |  |  |
| `@where` | [where](#where) |  |  |

## gearNumber

A number in the tooltip's own units, or an op relative to the game's value of the item: {"mul": 1.2}, {"add": 5}, {"set": 30}, "min", "max", "raiseOnly", "lowerOnly".

Type: number or object

## gearRarityValues

One value per rarity: a list of up to five, common to legendary (null keeps the game's), or {"common": ..., "legendary": ...}.

Type: array of null or [gearNumber](#gearnumber) or object

## gearRule

The gear values of every item the selector matches. Other keys are item field paths, as in the items section (the typed keys win where both name one field).

| Key | Type | Required | Notes |
|---|---|---|---|
| `@refs` | boolean |  | Opt in to changing the perk slots of game items (perks), as in the items section. |
| `@where` | [where](#where) |  | Conditions on the game's original values of the item, as in the items section. |
| `armor` | object |  | The armour the tooltip shows per kind at the item's lowest level, solved into armorSetup.distribution. 0..100000. |
| `block` | [gearNumber](#gearnumber) |  | blockSetup.quality, 0..1. |
| `damage` | object |  | Shares of the damage per kind (0.5 = half; the game's weapons add up to 1), and for wood, stone, metal and explosion the whole numbers the game keeps for what a tool harvests and breaks. Kinds not named keep their value. |
| `levels` | integer or array of integer |  | The levels a copy is made at: a level (both ends) or [lowest, highest], 1..55. |
| `maxStack` | [gearNumber](#gearnumber) |  | maxStackSize, 1..65535. |
| `perks` | [perks](#kitbash) |  |  |
| `power` | [gearNumber](#gearnumber) |  | The damage the tooltip shows at the item's lowest level (levels, else the game's), solved into damageSetup.dmgMod with the item's speed. 0..100000. |
| `rarity` | string or integer |  | The type's rarity: what the recipe tooltip shows and a copy is made with when the game does not roll it. The game has no rarity above legendary. |
| `recipe` | object |  | Every recipe that makes the item: its ingredients and crafting time (other recipe fields: the recipes section). |
| `rolled` | boolean |  | generateRarity: the game rolls the rarity of each new copy (true, as for loot weapons), or every copy is the type's rarity (false). |
| `socket` | boolean |  | gemSlotOverride: new copies get a gem socket (gear other than weapons needs Kiln's sockets: docs/kiln/services/gems-sockets.md). |
| `speed` | [gearNumber](#gearnumber) |  | damageSetup.speedMod: the damage factor of the weapon's speed class (it multiplies the damage; the attack speed is the animation). More than 0, at most 100. Alone it changes the tooltip's damage: give power too to keep it. |
| `staminaCost` | [gearNumber](#gearnumber) |  | The item's StaminaCost value (impactValues.StaminaCost), as weapon blocks write it; for items that have one. |

## gearRules

The game's balancing values for gear (keen::BalancingTable): boot values, read before the game starts, so the mod needs "boot": true; they act where the mod runs (the server's values act; a player's game shows them when it has the mod too). Applied whole or not at all. Docs: docs/kiln/services/content.md#gear-rules-boot-values.

| Key | Type | Required | Notes |
|---|---|---|---|
| `comment` |  |  | Free text, ignored. |
| `durability` | object |  | Max durability: (start + growth x (level - 1)) x the rarity's factor x the item's own factor. |
| `lootRarityRates` |  |  | not supported yet: Kiln has not mapped the game resource that holds the loot rarity rates |
| `rarityDurability` | [gearRarityValues](#gearrarityvalues) |  | rarityDurabilityMod: the factor of each rarity on a copy's max durability (the game's: 0.9, 0.95, 1, 1.05, 1.1). 0.01..100. |
| `socketChance` | [gearRarityValues](#gearrarityvalues) |  | gemSlotProbability: the chance of each rarity that a new weapon gets a gem socket (the game's: 0, 0.05, 0.25, 0.5, 1). 0..1. |
| `unlockCost` | object |  | What unlocking an enhancement at the workbench costs (the game's perk unlock cost). |

## gearSection

Typed values of item types in the numbers the game's tooltip shows ("<selector>": {gear rule}): kiln.content solves them into item fields per matched item, as layers of the pack's mod, after the items section. Docs: docs/kiln/services/content.md#gear-values.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` |  |  | new items go in the items section's "add" list; gear rules change items |

## guid

A GUID in .NET text form (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx).

Type: string

## hexId

Pattern `^0[xX][0-9a-fA-F]{1,8}$`.

Type: string

## itemDefinition

A new item: a clone of `base` with its own fixed GUID, debug name, texts and values (docs/kiln/services/content.md#new-items-and-recipes-forges-deferredregister).

| Key | Type | Required | Notes |
|---|---|---|---|
| `base` | string | yes | The item it is cloned from (debug name, 0x id or GUID). min length 1 Names a **items** entry (checked against the catalog). |
| `description` | string |  | Empty: none, rather than the base item's. |
| `displayName` | string |  | The name players see (every language). |
| `drops` | array of object |  | Drop-table stacks: {table: <table GUID \| glob over GUIDs \| like:<item>>, countMin, countMax, chance, weight}. |
| `fields` | [fields](#fields) |  |  |
| `guid` | [fixedGuid](#fixedguid) | yes |  |
| `icon` | string |  | item:<name> / an item GUID (that item's icon), or a PNG file relative to the mod folder (registered with kiln.assets; Kiln clients only). |
| `loot` | boolean or object |  | Random loot: true = like the base, or {like, stackSizeMin, stackSizeMax, stackSizeMaxScaled, fields}. |
| `lore` | string |  |  |
| `name` | [itemName](#itemname) | yes |  |
| `placeholder` | object |  | Kiln's own (missing content): mods do not write it. Kiln warns (not refused). |
| `rolledLoot` | boolean |  | Opt-in for a rolled loot item: keeps the base's generateRarity and level range. Default false: a fixed rarity and one level, as the recipe tooltip shows. |
| `source` | string |  | Where to get it, in the tooltip (every language). Not given: nothing, not the base item's. |
| `texts` | [texts](#texts) |  |  |

## itemName

a debug name is 1..200 of A-Z a-z 0-9 _ - .

Type: string

## itemRef

item:<name> (that item's value), item:<name>.<field path>, a GUID, or none.

Type: string

## itemRule

Field changes of every item the selector matches.

| Key | Type | Required | Notes |
|---|---|---|---|
| `@refs` | boolean |  | Opt in to changing references of game items (kitbash keys, refs). |
| `@where` | [where](#where) |  |  |
| `icon` |  |  | icons are definition-only (an "add" entry or assets/assets.json) |

## itemSelector

A debug name, "0x..." / decimal id, a GUID, or a case-insensitive glob (Material_*, *). min length 1 Names a **items** entry (checked against the catalog).

Type: string

## itemdataSection

Items through kiln.gamedata's paths (references, configs, lists): the paths Content.ItemData takes. Mods normally use "items"; a game item's references need "@refs": true in the rule.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` |  |  | new items go in items.add |

## itemsSection

Rules over the item registry ("<selector>": {fields}) and new items ("add": [...]).

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [itemDefinition](#itemdefinition) |  |  |

## kitbash

Kitbash references (docs/kiln/services/content.md#kitbash-new-items-made-of-loaded-resources-v4).

| Key | Type | Required | Notes |
|---|---|---|---|
| `animation` | [itemRef](#itemref) |  | equipment.equipmentEntityModelSetup.animation |
| `collectionModel` | [itemRef](#itemref) |  | collectionInfo.overrideModel |
| `iconModel` | [itemRef](#itemref) |  | iconModel |
| `impact` | [itemRef](#itemref) |  | equipment.impactEntity |
| `model` | [itemRef](#itemref) |  | item:<name>: that item's whole visual setup; a model GUID: just the model. |
| `perks` | string or array of null or `"none"` or string or [guid](#guid) or string |  | perkReferences[i] / perkIds[i]: a list of up to 5 (perk GUID, item:<name>, item:<name>.perkReferences[k], none, null = keep), or "item:<name>" for all five. |
| `program` | [itemRef](#itemref) |  | equipment.program |
| `refs` | object |  | Any reference field by path: item:<name>, item:<name>.<path>, a GUID (also one kiln.assets serves), or none. |
| `visual` | [itemRef](#itemref) |  | equipment.visualEntity |
| `weaponType` | [itemRef](#itemref) |  | weaponCategoryReference |

## lang

A language key: en, de, pt-BR, zh-CN, or * (every language). Kiln normalizes _ to - and ignores case; lookups fall back to the primary language, then en, then *. Pattern `^(\*\|[A-Za-z0-9][A-Za-z0-9_-]{1,11})$`.

Type: string

## lootSection

Random loot of containers and world pickups, per item: { "<item selector>": { "stackSizeMin": 5, ... } }.

Type: object

## op

An operation relative to the game's original value.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | number |  |  |
| `lowerOnly` | boolean |  |  |
| `max` | number |  |  |
| `min` | number |  |  |
| `mul` | number |  |  |
| `raiseOnly` | boolean |  |  |
| `reset` | boolean |  | kiln.gamedata: takes this owner's layer off. |
| `set` |  |  | Sets this value (any value form). |

## options

| Key | Type | Required | Notes |
|---|---|---|---|
| `refs` | boolean |  | This document may change references of game items (kitbash opt-in for every rule). |
| `stackGuard` | boolean |  | Keep the stack guard on (see content.md: Stack guard). |

## perksSection

keen::Perk (impactValues, damageModifier, perkArmorSetup, uiValues); selectors: a debug name, a GUID, 0x id, a glob.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [cloneDefinition](#clonedefinition) |  |  |

## recipeDefinition

A new recipe: base (a recipe to copy) and/or station (hand, 0x<workshop id>, or a recipe whose station to use).

| Key | Type | Required | Notes |
|---|---|---|---|
| `base` | string |  | min length 1 Names a **recipes** entry (checked against the catalog). |
| `displayName` | string |  | A recipe name (most recipes show their result's). |
| `duration` | number |  | Crafting time in seconds (default: the template's). min 0 max 1000000 |
| `fields` | [fields](#fields) |  |  |
| `guid` | [fixedGuid](#fixedguid) | yes |  |
| `inputs` | array of [definitionStack](#definitionstack) | yes | min items 1 max items 32 |
| `name` | [itemName](#itemname) | yes |  |
| `outputs` | array of [definitionStack](#definitionstack) | yes | min items 1 max items 32 |
| `placeholder` |  |  | "placeholder" is written by Kiln for missing content Kiln warns (not refused). |
| `station` | string |  | min length 1 |
| `unlock` | `"always"` \| `"base"` |  | always (default): no knowledge requirement; base: the base recipe's requirement. |
| `water` | integer |  | Water charges a craft (RecipeInfo.waterCharges; 1 charge = 64 water units), as the recipe screen's Water row shows it. A station with a tank (Seedbed, Oven, Cooking Station, Laboratory, Coffee Brewer, Beehive) takes it from its own tank at the start of each craft; a crafting menu (hand, Workbench, Kitchen, NPCs) from the crafter and the base's wells and barrels. Default: the base recipe's water, none for a recipe with only a station. kiln lint KL408 warns at a station without a tank or above its tank. min 0 max 1023 |

## recipeRule

| Key | Type | Required | Notes |
|---|---|---|---|
| `@where` | [where](#where) |  |  |
| `inputs` | [stackList](#stacklist) |  |  |
| `outputs` | [stackList](#stacklist) |  | outputs cannot be categories |

## recipesSection

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [recipeDefinition](#recipedefinition) |  |  |

## ruleStack

a stack needs "item" (or "category" for inputs)

| Key | Type | Required | Notes |
|---|---|---|---|
| `category` | string \| integer |  | Names a **itemCategories** entry (checked against the catalog). |
| `count` | integer |  | Default `1`. min 1 max 65535 |
| `item` | string |  | min length 1 Names a **items** entry (checked against the catalog). |

## sequencesSection

keen::actor::ActorSubSequenceResource (events, length, loop, mask); selectors: a sub-sequence name, a sequence GUID, <guid>#k, a glob.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [cloneDefinition](#clonedefinition) |  |  |

## skillsSection

keen::SkillTreeNode (costs, maxLevel, configValues, uiPosition); selectors: 0x<node id>, skill_<id>, item:<ability item> (the node that unlocks it).

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` |  |  | skill nodes cannot be cloned here (talent trees: Kiln.Skills) |

## stackList

The whole new list (1..32 stacks), or {"mul": x, "min": 1} to scale the original amounts.

Type: array of [ruleStack](#rulestack) or object

## templatesSection

keen::ecs::TemplateResource: paths start with a component type name (Enemy01BehaviorState.combatCommitmentSetup.view.range).

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | array of [cloneDefinition](#clonedefinition) |  |  |

## texts

Texts per language: { "<language>": { "name", "caption", "description", "lore", "acquisitionHint", "primaryAction", "secondaryAction", "tertiaryAction" } }. Texts not given stay the base item's.

Type: object

## value

A field value: a number, true / false, an enum name, "0x...", a GUID or name for a reference, a flag-name list (["Magic"], ["+Run", "-FleeHome"]), seconds for times; an op ({"mul": 2}, {"set": v}); in gamedata sections also an object that sets a struct's fields.

| Key | Type | Required | Notes |
|---|---|---|---|
| `add` | number |  |  |
| `lowerOnly` | boolean |  |  |
| `max` | number |  |  |
| `min` | number |  |  |
| `mul` | number |  |  |
| `raiseOnly` | boolean |  |  |
| `reset` | boolean |  |  |

## where

Conditions on the game's ORIGINAL values: field path -> a number / bool / enum name for equality, ">1", ">=", "<", "<=", "!=", "!Name"; a list = any of. "name": a glob over the entry name; recipes also "output" / "input" (an item name or glob in the list).

| Key | Type | Required | Notes |
|---|---|---|---|
| `input` | string |  | Names a **items** entry (checked against the catalog). |
| `name` | string |  |  |
| `output` | string |  | Names a **items** entry (checked against the catalog). |

