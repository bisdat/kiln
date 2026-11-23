# Friendly NPCs and quests in the game's own UI

A mod describes in JSON what the game itself then shows, and this is the default for every quest and dialogue: new knowledge ids, quests in
the game's journal with step counters and notifications, dialogue lines in the game's NPC menu with per-language text and voice-over,
quest markers on the map, and new NPCs with a place in the game's roster (one per world, unlock rule, summon tool, beds, vendor stock).
Evidence: [docs/re/quests.md](../../re/quests.md) sections 11 and 12 and
[docs/re/creatures.md](../../re/creatures.md) section 7.8. The quest logic itself (objectives, rewards) is the
engine's, [docs/kiln/modding/quests.md](../modding/quests.md).

| Layer | File |
|---|---|
| Boot build | `native/services/quests/native_pack.cpp` (finds the files, serves the result through kiln.assets) |
| Builders | `native/services/quests/questpack.cpp` (JSON to definitions), `questbuild.cpp` (the game's resources); portable, also compiled by the tests |
| Run time | `managed/services/Kiln.Quests/Native/` (`NativeQuests`: the mirror, computed conditions, dialogue options, NPCs, markers; `NativeIds`; `NativeExport`: quests from C#), `Places.cs` (where things go) |
| Villagers | `managed/services/Kiln.Creatures/Villagers.cs` + `kiln.creatures.npc_command` |
| Format | `schema/kiln-quests.schema.json`, lint rules KL210, KL312 |
| Samples | `mods/samples/quests-sample/` (the Warden's chain), `mods/samples/NpcSample/` (Tobin: voice-over, roster, a trade, a summon tool) |
| Tests | `tools/kfc/tests/test_questbuild.py` (offline, on the client's own data), `mods/tests/quest-native-test/` (lab), `native/services/quests/tests/run.sh` (host: the boot build's stops), `mods/tests/content-stop-test/` (lab: a whole build that fails) |

## Status

- **Lab and offline: verified on the dedicated server** (svn 1287369): see "Verification".
- **Seen in a real client** (joined to a lab): Tobin's topics in the game's NPC menu, his greeting's voice-over, his quest in the game's journal.
- **Not seen yet**: map markers, notifications, the tracked-quest HUD, step counters, the NPC directory, the summon tool and beds. See "Manual test".
- **A roster over 64 entries** (since 0.2.0-dev.29): lab-tested on the dedicated server without a player
  (`mods/tests/roster-cap-test/lab.sh`, nine boots on one save: 64, 65, 128 and a restart; 129, 65 with the hooks not
  installed, and 129 in a boot generation with the mod not required and required: all four stopped at the start through
  kiln.boot's stop with exit status 2, boot.json `boot.stop` and progress.json `failed`, the save byte for byte the
  same and the knowledge set before them still there after; a page dropped and computed again on a checked context (a dedicated server);
  `roster.no-pages` ignored without `roster.lab`; the negative control without the page, whose two page-2 assertions
  failed as they must). There the roster's programs ran on the world's thread, also with kiln.world's
  parallel switch on, so a run of the hooks on the game's workers (a server with players) is written for but not
  seen; a hosting game's two worlds and a game client's stopped start are not seen yet (guided checks
  `17-roster-client`, `17-roster.refused-start`). A lost page is computed again on a dedicated server only; a game
  client keeps such NPCs until the game's next evaluation. See [The roster's size](#the-rosters-size-since-020-dev29).
- **A whole build that fails stops the start** (since 0.2.0-dev.29): lab-tested on the dedicated server without a player
  (`mods/tests/content-stop-test/lab.sh`, twelve boots on one save): a normal start served the fixture's knowledge,
  quest and clone; each kind of whole failure forced by the lab switch (`read`, `compile`, `shared`, `assets`, `hook`,
  and `compile` and `hook` in a boot generation, the last inside the knowledge collector's prepare) stopped before any
  world with exit status 3, the stop line, `content-refused.json`, `boot.json` `boot.stop` and progress.json `failed`,
  the save byte for byte the same; a mod with its own error beside a good one was named and left out while the good one
  was served; `late` logged that the start could not be stopped and the server ran on; a mod with no native content
  and the switch on did not stop. One mod whose `kiln.mod.json` field has the wrong type (left out and named, the
  others served) is host-tested only. A game client's stop (its message box) is not seen yet. See
  [When the whole build fails](#when-the-whole-build-fails-since-020-dev29).

## What needs what

| You use | Vanilla clients | `clients` | `side` |
|---|---|---|---|
| A quest, a dialogue, `clone`, `roster`: the default | refused | **`mod`** | `both` |
| `"journal": "chat"`, `"native": false` | follow in chat, `/quests`, `/talk` | `none` | `server` is enough |
| New knowledge ids only (`knowledge`), read and granted by the server | join, and store ids they do not use | `none` | `server` is enough |
| A game NPC placed as a giver (`npcs.*.place`, nothing else) | see it | `none` | `server` is enough |

**`clients` must be `"mod"`, not `"kiln"`.** A client builds these resources from the mod's own
`quests/*.json` at boot, before any world or server connection exists, so a server cannot send them. `kiln lint` reports
KL210 as an error otherwise.

## Authored roster bodies

`NpcBlock.Motions` names an `NpcMotionsBlock`: Kiln-owned motion roles bound to states of the body's cooked graph.
The [authoring contract](../modding/npc-blocks.md#authored-roster-bodies-and-motions) covers the complete 27-binding
Fisher vector, shared skeleton/slot checks, explicit size, and the deferred `assets/cooked/npc-bodies.v1.json` bundle.
Native admission, body lowering and the NPC motion closure are built (below); the network clearance (Ready and the
body's closure hash in the join handshake) is not installed yet, so every authored body is still refused with that reason.
An unavailable dependency is a refusal, not a rendered fallback and not a release hold.

| Capability | Needs | Current contract |
|---|---|---|
| `blocks.npc_body` | `kiln.knowledge`, `quests.boot_pack`, `quests.npc_body`, `quests.npc`, `assets.resources`, `assets.content` | Managed row; refuses authored-body definition with the missing dependency's reason. Ordinary game-body NPCs do not need it |
| `quests.boot_pack` | Private native content rollback helper and the resource initialization hook | Per-mod compile/preflight and owner-isolated rollback through the existing boot path; no public bootpack or epoch API |
| `quests.npc_body` | Native authored-body admission and lowering, the NPC motion closure, kiln.assets' private body registration, the network clearance | Unavailable while the network clearance is not installed (the row names it); a body mod is then refused whole. A game update that changes the verified Fisher layout makes the row unavailable (`symbol-missing`) with the changed type named |

Native packs compile separately in dependency order against the surviving effective resources. A refused pack withdraws
only its `bootpack:<mod>` owner, restores its text-tag before-images without overwriting unrelated writers, and leaves
earlier successful packs intact. Failed required native dependencies are excluded before their dependants compile.
A missing template or a mod-specific build failure refuses only that pack; missing shared knowledge, journal or NPC
collection inputs still stop startup, as does a roster overflow.
An invariant failure stops startup through the existing named fault/notice path rather than continuing with a partial pack.
Aborting a boot generation always releases its plan, removes successful withdrawals from the pending list, and clears
its served-name metadata. A rollback failure remains a named restart-required fault, not a fresh build over partially
withdrawn owners.
Rollback records retain resource presence, length and a local 64-bit fingerprint, not full resource copies. The private
compile snapshots are released after each pack. Repeated Kiln knowledge-map tails are compacted to the original input
prefix plus the latest maps; a different effective blob from another writer starts a new prefix so its unknown data
is preserved. Text undo records are discarded at the first world start, when the helper can no longer abort a pack.
The assets store still retains published immutable layers: knowledge storage is one bounded blob per pack, not one
blob containing every earlier map copy. Journal/NPC builders retain their existing append-only layout.
Without the private helper, ordinary legacy packs retain their old path and whole body-dependent packs are excluded.
This is rollback behind the existing startup barrier, **not** an unpublished snapshot or an epoch: an independently
started native-plugin worker reading GameKnowledge during publication could see it before the pack's item definitions.
No such production reader has been measured; that hypothetical worker remains an open concurrency risk.

### Admission and lowering at the native boot build

An authored body is admitted once per mod, before the mod compiles, against **this machine's** cook
(`assets/cooked/npc-bodies.v1.json`, never shipped as identity). Admission refuses the whole mod, naming the NPC, when:
the cache is missing, malformed or from an older cook; an authored source (model, clips, glTF buffers and images, material
blocks and their textures) or a cooked output no longer hashes as recorded; the NPC's resolved body, animation or motions
blocks differ from what was cooked; the cook has no complete 27-row motion vector, no graph of its own or not exactly one
RenderModel row for the body; a game client finds a dedicated server's cook (skipped renderer output) or an incomplete
cook; a game client's material blocks are not built and served by kiln.vfx/kiln.assets (G2); or kiln.types no longer has
the verified Fisher layout. A dedicated server takes the model id from the cook's RenderModel row and publishes no
renderer output. Only NPC blocks (and NPCs Kiln.Blocks wrote for code at an earlier start) may carry a body; a mod's own
`quests/*.json` naming one refuses that mod.

The admitted pack compiles with the body lowered first: the Fisher template is copied into an owned buffer, its five
appearance producers removed, model, graph, cloth collider, slots, path/movement size, interaction box, steering sphere
and UI offsets set from the cooked ids and the authored Size, and then the ordinary quest edit (name, NpcId, loca,
dialogue) runs on those bytes. The vanilla Fisher key is never written; an authored body on a non-clone or another
chassis refuses the mod. The body's cooked resources and blobs are registered by kiln.assets' private body export for
the same `bootpack:<mod>` owner inside that mod's transaction (new keys only, checked absent first and absent again
after a rollback; never pinned), so a refusal or withdrawal takes them away with the rest of the pack and leaves earlier
and independent mods as they were.

### The NPC motion closure

The Fisher's template names 21 action sequences (sit, sleep, inspect, greetings, talk, reactions, refusal, idle gestures,
its object interaction, init/farewell/option lines, turns, jump and door; 27 animation events). For an authored body each
is cloned under an id of the mod (UUIDv5 of the mod, the NPC, the source sequence and the binding vector's hash; its
sub-sequence ids likewise), every `SetAnimationEvent` is bound in serialized order to the authored state of its role and
variant, and every whitelisted event's local times are retimed to the cooked clip durations (chapter 16f section 5.1:
zero stays zero, the lead-in is kept, each animation phase takes its longest bound clip, `lerpDuration` maps through the
same piecewise-linear time map, loop flags and all other fields are copied). Then every sequence reference of the
template's ActiveNpcState, NpcState, NpcActionSequences, NpcDialog and PathWalker is replaced by its clone. The build
refuses the mod, naming the sequence, for an event type outside the verified whitelist, a changed event size, a
different number of animation events than the manifest, a nonzero reference outside the 21, or a source id left anywhere
in the template afterwards (a reference path a game update added). The sequence, event and reference layouts are
checked against kiln.types together with the body's. The clones are new keys published with the template under
`bootpack:<mod>`; the game's own sequences are never changed.

### Body status

The boot build's manifest (kiln.knowledge `native_manifest`, read by Kiln.Quests/Kiln.Blocks) carries a `bodies` array,
one row per authored-body NPC: `mod`, `npc` (the block name), `key` (`was` or the name), `state` (`ready`, `failed`,
`removed`), `reason` (the refusal, empty when ready), `template` (the clone's GUID), `model`, `graph`, `closure` and
`generation`. `closure` is the body's network identity: a SHA-256 over its authored-source hash, its binding vector, its
cooked model and graph ids, its size and the verified chassis; never over machine-local cooked bytes or process handles.
A failed mod's rows carry its refusal. The native build keeps the last build's rows in
`<Kiln data>/kiln-quests/native/npc-bodies.ledger.json`; a body that was ready there and is not built at all at the next
start (its mod or NPC taken out) is reported once as `removed`. The ledger records statuses only, never game state.

Generated material-block/texture hashes describe dependencies only; they must not be mistaken for prepared material
resource bytes. No Ready status, hot body replacement, or safe missing-model fallback is promised by a cooked manifest.
Both sides need the mod. A vanilla peer must be refused or have the new NPC withheld by normal content admission;
never publish an authored body under a vanilla template key.

## The format

`quests/*.json`, read by two readers: this boot build and the quest engine. No key asks for the game's UI: only `"journal": "chat"`
and `"native": false` stay out of it. Field reference: [schemas/kiln-quests.md](../modding/schemas/kiln-quests.md).

```jsonc
{
  "format": 1,
  "knowledge": {
    "tobin_met": { "scope": "world" },               // world | player (default) | both; bool (default) | counter | flags
    "pages": { "type": "flags", "expectedFlags": 7 }
  },
  "npcs": {
    "tobin": {
      "template": "NPC_Workshop_Fisher_Trader",      // the base: its look, behaviour, workshop
      "clone": { "guid": "299d68c5-…", "name": "Kiln_NpcSample_Tobin" },   // fixed forever; true derives both
      "names": { "en": "Tobin the Wanderer", "de": "Tobin der Wanderer" },
      "description": { "en": "Tobin the Wanderer trades a little…" },  // a roster NPC's line on the game's Survivors page
      "dialogue": "tobin",
      "place": { "near": "player", "distance": [25, 45], "yaw": 90, "once": true, "unlock": true },   // or "at": [x, y, z]
      "roster": {                                    // an entry in the game's NPC roster; needs clone
        "unlock": "tobin_met",                       // a knowledge name, 0x…, or npc:<debugName>.unlock; none = always
        "roamingRange": 10,
        "workshop": "0xc4acc104",                    // optional: default the base's
        "summonItem": {                              // a GUID, or kiln.content's item definition
          "guid": "83ff4ce1-…", "name": "Kiln_NpcSample_TobinTool",
          "base": "NPC_Townsfolk_Male_Villager_08_fisher_Trader",   // its name and description are the NPC's: see "Recruiting and beds"
          "texts": { "en": { "caption": "Tobin's Whistle" } },      // what the tool is
          "refs": { "equipment.placedEntity": "299d68c5-…" }   // the clone's GUID: what the tool places
        }
      },
      "vendor": { "recipes": [ { "guid": "7797ab3a-…", "name": "…", "station": "0xc4acc104",
                                 "inputs": [ { "item": "Material_T1_Twigs", "count": 2 } ],
                                 "outputs": [ { "item": "Material_T1_Plant_Fibre", "count": 1 } ], "unlock": "always" } ] }
    }
  },
  "quests": {
    "tobin_kindling": {
      "name": { "en": "Kindling for Tobin" }, "giver": "tobin", "scope": "player",
      "source": "Fisher",                            // source: the journal's portrait (keen::QuestSourceType)
      "steps": [
        { "id": "gather", "title": { "en": "Gather 3 Twigs" },   // the short line: the journal's list, the tracked quest
          "text": { "en": "Tobin's fire is dying." },            // the journal's paragraph
          "objective": { "collect": { "item": "Material_T1_Twigs", "count": 3 } },   // the journal counts 0/3
          "site": { "near": "tobin", "distance": [30, 50] },     // a place on the ground, outside every base
          "marker": { "name": { "en": "Tobin's Kindling" } } }   // the marker comes by itself; this names it
      ],
      "rewards": [ { "xp": 100 } ]
    }
  },
  "dialogues": {
    "tobin": {
      "start": "hello",
      "greetings": [ { "if": { "quest": "tobin_kindling", "is": "active" }, "text": { "en": "Back so soon?" } } ],
      "nodes": {
        "hello": {
          "text": { "en": "Well met, Flameborn." }, "voice": "tobin_hello",  // a voiceover step of assets/cook.json, or a GUID
          "options": [
            { "text": { "en": "I can find kindling." }, "if": { "quest": "tobin_kindling", "is": "available" },
              "do": [ { "start": "tobin_kindling" } ], "reply": { "en": "Bless you." }, "voice": "tobin_thanks",
              "hint": "Attention", "goto": "end" },
            { "text": { "en": "Who are you?" }, "reply": { "en": "A wanderer." }, "once": true }
          ]
        }
      },
      "farewell": [ { "text": { "en": "Keep warm." } } ],
      "transition": [ { "text": { "en": "Anything else?" } } ]
    }
  }
}
```

### From a graph to the game's menu

The game's dialogue is one flat list of topics, and each line has exactly one knowledge query. So:

- The **start node's text** is a greeting (an init line). `greetings` adds lines that win over it while their `if` holds.
- Every **option** of every node is one topic. Its reply is `reply`, else the target node's text, else its own text; its
  voice is `voice`, else the target node's. `do` runs on the server through the engine (`Dialogues.ApplyOption`), which
  checks `if` again there.
- A **later node's** options need a gate: Kiln adds a knowledge object that the option leading there sets. It is per
  player and stays set, so those topics stay listed afterwards.
- **`if`** can be anything the engine can evaluate, on any node. The game checks **one knowledge query per line**, so:
  `{quest, is: available | active | done | started}` of a quest of this mod, `{step, is: active | done}` and `{knowledge, is}` with a name the build knows
  are that one query, and the game evaluates them at once. Everything else (`all`, `any`, `not`, `level`, `item`, `predicate`, `available` on a quest with `requires`, a gate or
  `once` together with an `if`) gets a knowledge object of its own (`cond.…`, "computed" in the manifest) that the engine keeps true or false for each
  player, once a second. The menu is still the game's. The engine checks `if` again when the line is picked.
- **A hand-in** (an option whose `do` advances a step with a `deliver` objective, with `if` the step being active or no `if`) is on offer only while the player holds the items: its one query is
  `s.<quest>.<step>.ready`, which the engine keeps. With the items gone between the menu and the pick, `Advance` refuses, the step stays, and the player gets the reason in chat.
- **An NPC without a dialogue** that is a quest's `giver` or a `deliver`'s `to` gets a topic for each by itself: the quest's name, and the step's text.
- `once` hides a topic after it was heard; `close` plays the NPC's outro after the reply; `hint` (`Hint`, `Attention`,
  `MainQuest`) is the marker over the NPC's head.
- A dialogue on a **game NPC** adds topics, and its lines stay. Its start node needs no text then, unless `"greet": true` makes it one
  more greeting. Several mods can add to one NPC; they share one override, in load order. On a **clone** the dialogue replaces the base's
  greetings and topics; the base's `farewell` and `transition` lines stay, with the base's voice, unless you give your own.
- `kiln lint` reports what the build refuses as KL312. The build logs the same things; that mod then has no native data, and its quests run with chat lines.

## Ids

Every id is FNV-1a-32 of a UUIDv5 of `<mod>:<kind>:<name>`, the same in C++ and in C# (`NativeIds`). Ids therefore do not
change between runs and saves keep them, but **renaming a mod, quest, step, dialogue, node or moving an option changes
ids**: what the save holds for the old ones is dropped at the next load. An option's id includes its index, so add
new options at the end of a node.

| Object | Knowledge name | Kind |
|---|---|---|
| your own | `<name>`, from elsewhere `<mod>:<name>` (`Knowledge.Resolve`) | as declared |
| a quest's "rewarded" Bool (its journal `entryId`) | `q.<quest>` | player |
| quest state | `q.<quest>.started`, `.active`, `.done` | player, or world for `"scope": "world"` |
| step state | `s.<quest>.<step>.visible`, `.active`, `.done` | as the quest |
| a step's counter, when its counted objectives add up to more than 1 | `s.<quest>.<step>.count` | counter, as the quest |
| a hand-in's "holds the items" | `s.<quest>.<step>.ready` | player |
| a line (its "heard" knowledge) | `line.<dialogue>.<node>.<option>` | player |
| a node's gate | `gate.<dialogue>.<node>` | player |
| a condition one query cannot say | `cond.<dialogue>.<node>.<option>`, `cond.<dialogue>.greeting<n>`, `cond.auto.<quest>.available` | player |
| the always-true requirement | `kiln.always` | world counter, never set |

## Running it

- **At boot** (kiln-quests.dll, before anything can read a resource): reads `quests/*.json`, `quests/generated/*.json`, what code defined at an earlier start
  (`kiln/data/kiln-quests/native/<mod>.json`), `assets/cooked/audio.json` and
  `assets/cook.json` of every enabled mod of the load order, compiles, and hands kiln.assets the rebuilt
  `GameKnowledgeResource`, `JournalRegistryResource` and `NpcCollection`, and the edited or new `TemplateResource`s. Definitions of
  summon tools and vendor recipes go to kiln.content. On a client, the texts go to kiln.content's loca hooks.
  ```
  knowledge: native: 2 mod(s): 48 new knowledge object(s), 3 quest(s), 2 NPC(s) in the roster, 6 template(s), 0 text(s); GameKnowledgeResource 1509084 bytes served by kiln.assets
  ```
  An error in one mod leaves out that mod's native data and logs why (`knowledge: native: <mod>: …`); the others build.
  When the whole build fails, so that nothing is served, the start stops instead ([below](#when-the-whole-build-fails-since-020-dev29)).
- **Changes need a restart**: resources are read once per process. `Quests.Reload()` reloads the engine's quests, not these.
- **At run time** (`NativeQuests`, started by the engine, server side):
  - mirrors the engine's state into the knowledge above: at every change, at a join, after a reset, and every 2 s whatever differs;
  - keeps the computed conditions and hand-ins for each player, once a second;
  - runs a native option's `do` on `DialogueLineEvent`, on the tick thread;
  - places NPCs with `place` once their world's data and entities are there. A dedicated server has both when it starts; a game client's hosted
    world starts about 10 s before the game loads it, and has its entities when its player arrives, so there it waits for the first player. A new
    hosted world starts with no NPC bound and nothing waiting from the last one;
  - moves a step's marker to the step's place when someone enters the step.
- **Removing a mod**: the next start has no objects for its ids, so the world's and players' records of them are dropped at
  load, and saved entities of its clones are gone (lab, below). Its quest sidecar stays in the world's data. Knowledge the game set on its
  own account stays: a game NPC's unlock granted for `place` is one of the game's ids.

### Placing NPCs

The game enforces two rules on any entity with an NPC template (`unique_npc_spawns`):
one entity per NpcId, and a **locked roster NPC is destroyed** (about a second after it spawns).

| `place` of | Unlock not set | Default of `"unlock"` |
|---|---|---|
| a game NPC | Kiln grants it for this world, waits for the roster to report it, then spawns | `true` |
| a clone with `roster.unlock` | Kiln waits for your knowledge, then spawns | `false` |
| a clone without `roster` | no such rule | – |

- **Where**: `"at": [x, y, z]`, or `"near": "player" | npc id | [x, y, z]` with `"distance": [min, max]`. On the voxel surface, outside every base zone (`"inBase": true` allows one, `"exact": true` takes `at` as it is). A wanted
  place in a base moves out, with a warning. Kept per world: one place from then on.
- **Checked**: 3 s after the spawn. Gone: a warning and another try, up to 5, then an error. `"once"` counts only an NPC that stayed.
- `"once": true` remembers the placement in the world's data: an NPC that later dies or is removed is not placed again. Either way, an
  entity already in the world is used and nothing spawns.
- Granting a game NPC's unlock is a real change to that world: it shows in the NPC directory and stays.
- The engine is told the entity (`Quests.BindNpc`), so `talk` and `deliver` objectives find it.

### Vendors

A vendor's stock is the recipes of its workshop. A clone shares its base's workshop, so `vendor.recipes` appear at the base NPC
too (Tobin's trade also shows at the fisher trader). A workshop of its own would need a new id in `WorkshopRegistryResource` **and**
in the crafting menu's configuration (`cfg+0xf098`: 48 workshops, client UI data); this was not built. `roster.workshop` can only name an
existing workshop.

### Recruiting and beds

Unchanged game paths, driven by the roster entry:

- **the game's Survivors list draws a roster NPC from its tool**: the row's title is the tool's item name, the page's text is the
  tool's item description, and only the page's title is the roster entry's own name (client 26430625: the row `0xc046c0` reads
  `ItemInfo.name` of `NpcResource.itemId`, the page `0xc46f1f` reads `.description`). So every tool of the game is named after its NPC
  ("Captain Melville Fontane", "… is a fisher and entrepreneur who offers fishing gear."). The boot build does the same for a tool
  defined inline: the NPC's `names` and `description`, per language. A name or description of the tool's own is replaced, with a warning
  (lint KL313); say what the tool is in its `caption` or `lore`. Without `description` the page shows the NPC's name: the base tool's text
  would be about another NPC. The game shows one item either way, so the tool in the inventory carries the NPC's name too;
- the tool's `equipment.placedEntity` is the clone's GUID. Using it places the NPC, or, when the NPC is already in the world, moves it
  (`NpcCommandEvent` Reset);
- every vanilla tool has `placementConsume` set, so expect a clone of one to be used up by placing;
- the tool's Assign sequence (aiming at a bed) sets `NpcCommon.bedId` and `NpcBed.associatedNpc`, finding the NPC by the tool's item in the roster.

## Limits

What the game's UI cannot do, and how a mod works within it instead of around it:

| It cannot | Why | Do this |
|---|---|---|
| More than one knowledge check on a line | the data format | nothing to do: anything more becomes a computed object, about a second behind |
| Nested menus: a node's options replacing the list | one flat topic list | `goto` adds the node's topics to the list from then on. `once` removes a topic, `if` keeps it for its moment, `close` ends the talk |
| Open the NPC menu from the server | the client opens it when the player talks | a marker on the NPC, a `hint` over its head, a greeting that wins while a step is active. `{"talk": …}` stays a chat conversation |
| A free-text notification | its notifications come from journal knowledge | a step's text, an NPC's reply. `{"toast": …}` is a chat line now, and lint warns |
| A give-up button | the journal has none | a topic: `"if": {"quest": "q", "is": "active"}, "do": [{"reset": "q"}]` |
| Show rewards in the journal | filling `QuestRewardResource` makes the game pay them too, and its item claim path was not traced | the engine pays at completion: the game's XP path and item popup. Say it in the text |
| Different places per player | a marker is one entity with one place per step | a site per world: everyone goes to one place. A `reach` `offset` still counts per player, but the marker shows the last player's |
| A refusal in the menu | the game has its own rule (id 1, missing requirement) only for its own check | conditions hide what cannot be picked; a pick that still fails tells the player in chat |

The numbers:

| Limit | Why | What happens |
|---|---|---|
| 128 roster NPCs in all, 35 are the game's (64 where Kiln's second unlock page cannot be installed) | the game keeps the unlock states in one u64 per world and takes an entry's index modulo 64; Kiln keeps a second page beside it ([The roster's size](#the-rosters-size-since-020-dev29)) | the start stops before any world loads, with the reason (a world without the mods' data would drop their progress) |
| 32 texts per line | the client stops the game above it (`0x460b40`) | Kiln writes one per line |
| 8 knowledge operations per line | the server pushes at most 9, with "heard" | build error |
| 6,050 world and 11,000 player objects (5,825 and 10,744 are the game's) | what the game reserves its run-time maps for (`0x99ead0`) | build warning; beyond it is untested. Those numbers bound entries set at run time, so this is likely softer than it looks. A quest costs 4 player objects + 3 to 5 per step + 1 per line; 256 are left before the warning. quests-sample (4 quests, 11 lines) takes 50, NpcSample 17 |
| new Extern (named) queries | not built: step and line requirements are single objects | – |
| template table 13,000 (12,269 used) | inline in ecs storage (`docs/re/gamedata.md`) | shared with kiln.gamedata clones; one per step with a place |
| water | the voxel surface does not say where water is | a site can be a lake bed: name a place, or check the map |

## The roster's size (since 0.2.0-dev.29)

The game keeps every roster entry's unlock state in one 64-bit value per world (`NpcRegistry`): its evaluator sets bit
`index & 63` and its query reads it the same way, so a 65th entry would share the first entry's state. Kiln keeps a
**second page of 64** beside each world's registry: the game's own evaluator runs once per page of at most 64 entries,
page 0 goes into the game's value as before, page 1 into Kiln's page, attached in the setter of that registry on the
thread that evaluated it, and the game's query answers an entry at index 64 or more from that page. The audit of every
reader of the value, on the dedicated server and the client: [docs/re/npc-roster.md](../../re/npc-roster.md).

| Your mods' roster (the game's 35 included) | What runs | Capacity |
|---|---|---|
| no roster NPC, or 64 entries at most | nothing is hooked. Two callbacks before and two after each world's step return at once until a mod reads the roster (below); from then on each world reads its registry's value after its step | 64 |
| 65 to 128 entries | at the start, before any world, the three hooks go in (`quests/NpcEvalUnlocks`, `NpcRegistrySetUnlocks`, `NpcIsUnlocked`, through kiln.hooks' checked installation, which writes no byte while a thread cannot be shown to be outside them) | 128 |
| 129 or more | **the start stops** before any world loads: "the roster holds at most 128 NPCs (two 64-bit unlock pages): 35 of the game's + 94 new" | – |
| 65 or more where the second page is not available (feature `quests.npc_pages` did not resolve, a hook could not go in) | **the start stops** the same way, with the 64 text and the reason: "… (the second unlock page is not available here: …)" | – |

**A stopped start.** A roster the build cannot hold would leave the world without all of the mods' native quests,
knowledge and NPCs, and a world loaded so drops the mods' knowledge (unlocks, quest progress) at its next save. So the
process ends before any world or save is opened, always the same way: with a boot generation (mods with `"boot":
true` or `"boot": "required"`) or without one, for a required boot mod as for any other mod, at the game's resource
start or inside a boot generation's prepare (a game client whose resources were up before Kiln). In this order:

1. `kiln/state/roster-refused.json` (`why`, `mods`, `side`, `exitCode` 19202, `unixExitStatus` 2, `restart` false,
   `advice`, `bootStop`, `pid`, `utc`; the next start moves it to `roster-refused.last.json`; fields in
   [launcher/contracts.md](../launcher/contracts.md) section 5.9),
2. the error line in `kiln.log`: "roster: the start is stopped (pid …, exit code 0x4B02): <the reason>; mods: …. No
   world was loaded and no save was opened … Do not restart the game or the server: the next start stops the same way
   … Fix the mod list …",
3. kiln.boot's stop (`stop_start`): its line "boot: the start stops (exit code 0x4B02): kiln.knowledge: …",
   `progress.json` `failed` with the reason and `boot.json` `boot.stop` (`code`, `reason`, `by`, `mods`); it then
   waits, a second at most per file, until another reader could see that text in `progress.json`, `boot.json` and
   `kiln.log` (best effort, not a flush of every file to the disk; at a bound it goes on and logs which file it did not
   see),
4. on a game client a message box with the reason, owned by the game's window, which closes by itself after 60
   seconds (the steps above come first, so a player who ends the game meanwhile still has the file and the line),
5. the process ends with exit code 0x4B02 (`KILN_KNOW_ROSTER_EXIT_CODE`, 19202; a Linux parent under Wine sees 2). If
   writing the files or the message blocks, kiln.boot's 65-second bound ends the process anyway.

The launcher's Play page and Deck Home say ‘Kiln stopped the start’, name the mods, show the advice, and offer Review
these mods to open the filtered mods list.

Seen on the lab's dedicated server, with and without a boot generation (the 129 and 65-refused boots and the two
boot-mod boots): steps 1 to 3 and 5, the save byte for byte unchanged. Not seen on a game client yet: the message box,
a player who hosts, a player who joins (guided check `17-roster.refused-start`). The stop is a start-time act only:
kiln.boot refuses it once a world is present or has run in this process (ending the process then could lose play);
kiln.knowledge then logs that the start could not be stopped and goes on without the mods' native data.

Mixed versions: this kiln.knowledge beside an older kiln.boot without `stop_start` stops by itself (steps 1, 2, 4 and
5, no `boot.json` stop record and no `progress.json` "failed"). An older kiln.knowledge (0.2.0-dev.28 and before)
cannot be made to stop at all, whatever the core: it refuses the roster at 64 and loads the world without the mods'
native data. Run Kiln as one matched bundle (core and services of one release), on the server and in every game.
**Never restart on it**: the next start stops the same way. The release's systemd unit has
`RestartPreventExitStatus=1 2 3`; a hosting panel or a restart script must treat exit status 2 (19202 on Windows) as a
configuration error ([dedicated-servers.md](../dedicated-servers.md#when-kiln-stops-the-start-no-restart-loop)).

`npcs.expanded_roster` says which: available "installed" once the page is in, available "ready" while it could go in
and nothing needs it, unavailable with the reason otherwise.

**Which world a page belongs to.** Each world binds its own registry, on its own thread, at its start and before every
step (its `NpcRegistry` singleton, under its core session); a page is attached only for a registry a live world bound,
and only that world's stop removes it (a hosting game's two worlds keep their pages apart; a stop of one leaves the
other's alone). An evaluation that was running when its world stopped attaches nothing. Kiln never reads another
world's memory to find out who owns a registry.

**"Kept".** An entry at index 64 or more is never answered from the game's value. When its page cannot answer (no page
for the cache, a page that no longer matches it or a cache the loader reset to "not computed", a registry no live
world bound, a world that stopped or a value that changed while the answer was read, a busy lock, a missing page that
is not computed again for want of a checked context, and a registry whose fields cannot be read or that cannot place
the entry, which may then be at 64 or more),
the hook gives the game's own "cache invalid" answer, which its only caller (`unique_npc_spawns`, the one-per-world
rule) treats like an unlocked NPC: it is kept, not destroyed. Each cause is counted in the world's stop line in
`kiln.log` ("kept N: no page, stale, unowned, moved, busy, no checked context, unreadable"). The Survivors directory
never reads the value: it evaluates each entry's own requirement.

**A lost page.** A page that is missing for a computed cache (a setter that did not pair with its evaluation, a busy
lock, a registry bound late) would otherwise stay missing until the knowledge changes. **On a dedicated server** Kiln
computes it again before the world's next step: the game's evaluator on the knowledge context the game itself passed
when it last evaluated that world's registry, and only while that context is the world's `G38Knowledge` singleton read
at that moment (a temporary the game passed, or a knowledge object replaced since, would be a stale pointer: then
nothing is computed and the page's ids stay "kept", counted as "no checked context", until the game's own next
evaluation attaches a page). The result is kept only when it agrees with the game's cache (page 0 and the version);
twice per cache at most ("roster: the server world's second page was missing for its cache (version …): computed again
at the step's start"). On the lab's dedicated server the context the game passed was the world's `G38Knowledge` ("the
server world's evaluator context is its G38Knowledge singleton"). **In a game client nothing is computed again**, for
its own client world and for a world it hosts alike: the client executable's evaluator has not been walked or run
outside its program, so a missing page there stays "kept" until the game's own next evaluation of that world attaches
one (when the knowledge next changes). Guided check `17-roster-client` still records each world's context line (it
fails on "is NOT"), the evidence a later client recomputation would need.

What the reading of the code could not settle about the dedicated server's call: the game calls `NpcEvalUnlocks` only inside the update
of its `npc_unlock_state_server` / `npc_unlock_state_client` program (which reads `G38Knowledge` and writes
`NpcRegistry`); Kiln calls it from the world's Pre, outside any program, on the world's thread, when no program of
that world runs. Statically (server `0xa2ef90` and its query evaluators to depth 4, 23 functions) the evaluator writes
only its result and the version it is given, which are Kiln's own locals here, and its callees write only their own
result structs and stack; no locked instruction was found. Six indirect calls on that path (imports and a function
pointer, an error path among them) were not followed, and the client executable's evaluator was not walked the same
way. Kiln catches a fault in the call (SEH) and keeps nothing of it; what a fault in the middle of a game callee could
leave behind (a callee's own state the walk did not see) is not known: none has been seen (lab: "recovery failed 0").

**Saves and other versions.** Nothing of the pages is saved: the game saves knowledge (the mods' unlock objects among
it) and the NPC entities; the value and the pages are computed again from knowledge after every start.

While a world's page is missing, its locked NPCs at index 64 or more are "kept", and a save taken then holds them: the
game's save snapshots the world after its step ([docs/re/saves.md](../../re/saves.md)), and a shutdown can take its last snapshot without
another evaluation of the roster, so a world can be saved with a locked page-2 NPC present. After the next load, the
first cleanup once a page exists again removes it, as the game removes any locked unique NPC. "Kept" never removes
anything, and it unlocks nothing. On a game client a page can stay missing until the knowledge next changes (above),
so such an NPC can stay longer there. A world saved with 128 entries and loaded:

- with Kiln and the same mods: as before the restart (lab: an entry on page 2 unlocked before a restart is unlocked
  after it).
- with this Kiln where the page cannot be installed, or with more than 128 entries: the start stops before the world
  loads (above); the save is not touched (lab: the save byte for byte the same, and the knowledge set before two
  stopped starts still there after them).
- with an older Kiln (0.2.0-dev.28 and before: 64 at most): that Kiln refuses the roster at the start but does not
  stop, so none of the mods' native quests and NPCs are built, and their knowledge objects are unknown to that world:
  the game drops them as it loads the save, so the mods' unlock states and quest progress are gone once that world is
  saved again. Do not open such a world with an older Kiln.
- without Kiln, or without the mods: the game has only its own 35 entries, as for any mod NPC at any roster size; the
  mods' knowledge is dropped the same way (the game's load keeps only ids it knows). Not run without Kiln: what the
  vanilla game does with the mods' NPC entities in such a save.

A player joining with this Kiln on a client that cannot install the second page does not get past its own start (the
message box above; guided check `17-roster.refused-start`). A client with an older Kiln builds none of the mods' native
quests and NPCs and does not stop; whether the server admits it is up to the mods' requirement of their clients
(`clients: "mod"` asks for the mod, not for its roster): not run.

**Lab switches** (`kiln/control/`, read at the start, logged at every start while present; never needed to play):
`roster.fail-install` (the hooks are not installed, as if their installation failed: a roster past 64 stops the start),
`roster.no-pages` (NEGATIVE CONTROL: 128 entries taken without the second page; page-2 states are wrong and the game may
keep the wrong NPCs; honoured only together with `roster.lab`, otherwise ignored with a warning; never on a save you
keep), `roster.lab` (a log line per attached page, and `roster.drop-page`, polled, drops a world's page once so its
recomputation can be seen). The 0.2.0-dev.29 test builds' data-folder switch
`kiln/data/kiln.knowledge/roster-64` is gone.

**Reading the roster** (any thread, from snapshots each world publishes after its step; never a game call on your
thread; the snapshot of the world's last completed step, and NotReady after a step whose registry could not be read,
never an older answer):

```csharp
RosterWorldResult w = NativeQuests.GetServerRosterWorld();      // Ok with a token, or NoWorld / NotReady
if (w.Ok)
{
    NpcUnlockResult u = NativeQuests.GetUnlock(npcId, w.World!.Value);  // Ok: Unlocked, KnowledgeVersion; else WorldEnded, NotReady, UnknownNpc
    CapacityQueryResult b = NativeQuests.GetRosterBudget(w.World!.Value); // Capacity 64 or 128, GameUsed 35, KilnUsed the mods' entries
}
RosterWorldResult c = NativeQuests.GetClientRosterWorld();      // a game client: its own client world's registry
[Subscribe] void OnRoster(RosterUnlockChangedEvent e) { /* e.World, e.NpcId, e.Before, e.After, e.KnowledgeVersion */ }
```

A token is the world's kind and the core session's counter of that kind: a read with the token of a world that stopped
answers `WorldEnded`, never the next world's state; never save or send one. Rows: `npcs.roster_read` (server world),
`npcs.client_roster_read` (a game client), `npcs.roster_budget`, `npcs.expanded_roster`. C:
`kiln.knowledge.roster_world`, `roster_budget`, `npc_unlock_at` ([knowledge.md](knowledge.md#c)).

## When the whole build fails (since 0.2.0-dev.29)

The boot build serves one shared pack for every mod: the game's knowledge, journal and NPC collection, rebuilt with all
of the mods' entries. When that whole build fails, nothing of any mod's native quests, knowledge or NPCs is served, and
a world loaded so drops all of that progress at its next save. So the start stops before any world loads, the same way
the roster's stop does (the steps above), with its own exit code and file:

| | The roster cannot be held | The whole build failed |
|---|---|---|
| Exit code | 0x4B02 (19202), Wine status 2 | 0x4B03 (19203, `KILN_KNOW_CONTENT_EXIT_CODE`), Wine status 3 |
| State file | `kiln/state/roster-refused.json` | `kiln/state/content-refused.json` (same fields; [launcher/contracts.md](../launcher/contracts.md)) |
| kiln.log | `roster: the start is stopped (pid …, exit code 0x4B02): …` | `native: the start is stopped (pid …, exit code 0x4B03): the mods' native quests, knowledge and NPCs could not be built at all (<what failed>); mods: …` |
| Message title (game client) | Kiln: the NPC roster is too long | Kiln: the mods' NPCs and quests could not be built |
| What to do | remove these mods, or update Kiln | remove or update the mods named; if it still stops, send `kiln/logs/kiln.log` to the mods' authors or to Kiln |

What counts as the whole build failing, and what does not:

| Case | Stops the start | Why |
|---|---|---|
| One mod's files throw when they are read (a `kiln.mod.json` field of the wrong type, such as `"side": 5`) | **no**: that mod is left out and named (`native: <mod>: reading its files failed (…)`), every other mod is read, built and served; a required boot mod is refused through kiln.boot by name (0x4B01) | one mod's error |
| The read throws in a way that is no one mod's (listing the profile's mods or the mods folder fails: an allocation failure, a path that does not convert) | yes, naming the mods read before it that ship such content | nothing is built. Inside a boot generation the knowledge collector's refusal names no mod, and kiln.boot stops with 0x4B01 |
| The build throws | yes, naming the mods that ship such content | nothing is built |
| The shared resources do not build (the manifest's `failed`: the game's knowledge, journal or NPC collection missing or damaged, two mods defining one clone, a template the game does not have) | yes, naming the mods the build was to serve | nothing is served |
| kiln.assets refuses the built knowledge, journal, NPC collection or a template, or kiln.assets is not there | yes, naming the mods the build was to serve | the shared pack is not served as it was built |
| The hook on the game's resource start (`assets/ResourceInit`) is not resolved or cannot be installed | yes, at Kiln's start (or in a boot generation's prepare), naming the mods that ship such content | nothing would ever build the pack (a game client whose resources were up before Kiln builds at once and does not stop) |
| One mod's own errors (its entry `ok: false`, its files skipped) | **no**: that mod loads without its native content, the log names it, the others are served | as before; a required boot mod is still refused by kiln.boot (0x4B01) |
| No enabled mod ships native quests, knowledge or NPCs (no files, or files with none) | **no** | nothing to lose |
| The roster cannot be held | as the roster (0x4B02), not as this stop | the more precise reason |

Chat quests (`"journal": "chat"`), chat conversations (`"native": false`) and ordinary placement of an existing game
NPC need no rebuilt resources. A missing native-resource hook does not stop a start containing only those inputs.

The stop is a start-time act only, as the roster's: once a world is present or has run in this process, kiln.log says
`native: the start could NOT be stopped: …` and the process goes on without the mods' native data. A required boot mod
whose content the failed build left out is also reported to kiln.boot (0x4B01) afterwards, but the stop above comes
first. Never restart on exit status 3 either: the next start stops the same way.

**Lab switch** `kiln/control/knowledge.fail-build` (read once at the start, a warning at every start while it is there;
never needed in play): its first word forces one kind of whole failure without a broken game: `read` (the read throws
after every mod was read, a failure that is no one mod's), `compile` (the build throws), `shared` (the build cannot read the game's
`GameKnowledgeResource`), `assets` (the built pack is not given to kiln.assets), `hook` (the hook on `ResourceInit` is
not installed), or `late` (the build serves as usual; 30 steps after the server world's first one the stop is asked
for, which must log that it could not be stopped and must not end the process). Each stops only when a mod ships such
content, as the real failure does. Another word is ignored with a warning.

Tested: host, `native/services/quests/tests/run.sh` (`native_pack_test.cpp`: native_pack.cpp with fake Kiln
services and a fake build: every case above by its real cause and by the switch, with and without a boot generation,
one mod with a wrong-typed `kiln.mod.json` field beside good ones (optional and required in a boot generation), and the
texts of both stops). Lab: `mods/tests/content-stop-test/lab.sh` (a dedicated server: a normal start, each
switch, one broken mod beside a good one, `late`, and no content with the switch on).

## API

```csharp
using Kiln;

// what the boot build made (any thread)
NativeModInfo? m = NativeQuests.Mod("npcsample");              // Ok, Errors, Knowledge, Quests, Lines, Npcs, Markers, Computed
uint? met = NativeQuests.KnowledgeId("npcsample", "tobin_met");
uint same = NativeIds.Knowledge("npcsample", "tobin_met");     // the rule itself, no build needed
await Knowledge.World.Grant(met!.Value);                       // or Knowledge.World.Grant("npcsample:tobin_met")
await Knowledge.GrantUnchecked(EntityId.None, 0x12345678);     // for tests of the game's own rule: an unknown id goes to the game's apply, which drops it

// the mirror (tick thread); needs no player, so a test can ask it
bool shown = NativeQuests.ShownByGame(def);                    // says native, and the boot build made it
IReadOnlyList<NativeWanted> w = NativeQuests.Wanted(fullId, holder);   // what the knowledge should hold now: ("done", id, 1), ("gather.count", id, 2), ...
bool holds = NativeQuests.Holds(m.Computed[0], holder);        // a computed condition or hand-in, as the engine sees it
NativeQuests.Resync(holder);                                   // write it again now; the engine does at every change, join and reset, and every 2 s

// villagers (tick thread)
Villager? v = Villagers.Find(templateGuid);   // or Villagers.Of(entity), Villagers.List()
v.GoTo(pos);                                  // the game's GoToPosition: walks there (path grid: a player nearby; ignored in state Start)
v.Teleport(pos, yawDegrees: 90);              // the game's Reset: moves it, and that becomes its home
v.Stay();                                     // Reset where it stands
v.Follow(player, distance: 4, catchUp: 40);   // repeated GoTo; Reset beyond catchUp; ends with Stay, GoTo, despawn, mod unload
VillagerState s = v.State;                    // keen::active_npc::State
Villagers.Watch(v.Entity);                    // VillagerStateEvent about 4x a second (all villagers: 1x a second while subscribed)
[Subscribe] void OnState(VillagerStateEvent e) { /* e.Entity, e.NpcId, e.From, e.To */ }
```

C: `kiln.creatures.npc_command(const KilnNpcCommand*)` (appended; any thread), `kiln.knowledge.native_manifest(buffer, cap, &len)`
(appended), `KILN_KNOW_REQ_UNCHECKED` in `KilnKnowledgeRequest.flags`.

## Verification

### Offline (`python3 -m unittest discover -s tools/kfc/tests -p test_questbuild.py`, 16 tests)
The same C++ as kiln-quests.dll, compiled for Linux, on resources tools/kfc reads from the installed client. The result is decoded with the
game's reflection. Checked: every vanilla and every new knowledge id found by the game's bucket rule with the right type and
scope; the game's arrays unchanged in the rebuilt resources; moved quests and roster entries decode to the same values; the
journal's requirements name the manifest's objects; a clone's NpcId and lines; the lines' voice-over GUIDs against the cook's
UUIDv5; Balthazar's vanilla lines intact and nothing else in his template moved; a marker for every step with a place, its requirements, tag and type; the journal's counter in the shape the
game's own step uses (`SimpleCount GreaterThan n-1`); one query per line, with hand-ins and computed conditions; the Warden's chain with no keys and the C# quest from its shipped file; `"journal": "chat"` and `"native": false` building nothing; topics for a giver
without a dialogue; bad input reported and nothing built; every constant of the builders against the game's reflection.

### Lab (no player; `mods/tests/quest-native-test`)
The whole sequence on one build, from a clean save: 175 PASS, then 59 after a restart, 7 after a restart without the mod, 5 with the mod back; 0 FAIL, one `[E` line, the deliberate "gave up". Log
excerpts: [services/quests.md](quests.md) and [docs/re/quests.md](../../re/quests.md) section 11. In short:

| Row | Check | Result |
|---|---|---|
| Q3 | negative: a new id without the rebuilt map | Kiln refuses it; the game's own apply drops it |
| Q3 | with the map: world bool, counter, flags accepted | PASS |
| Q3 | the world save keeps them across a restart | PASS |
| Q3 | a restart without the mod drops them | PASS |
| Q3 | the mod back: its knowledge stays lost, the ids work again, a world quest's state returns from the engine's sidecar | PASS |
| Q3 | engine state mirrored, the game's evaluator sees the step visible, then done; survives a restart | PASS, world-scope quest |
| Default | a quest with no journal key is in the game's journal | PASS |
| Mirror | complete through a line of the menu, after an action that ended off the tick thread: the knowledge ids, the game's evaluator on the line's own query going false, a second pick refused, a reset clearing it, all of it twice | PASS, world-scope quest |
| Counter | the journal's counter object at 0, 2, 3 of 3, and back to 0 after a reset | PASS |
| A moment on screen | a step already fulfilled: its counter full at once, still open after 343 ms, passed at 3437 ms | PASS |
| Hand-in | not on offer without the items or with too few, on offer with them, refused and staying on the step without, what the mirror wants after it; a reset, then again | PASS, synthetic holder |
| Conditions | all/level/not/item, available with a requirement, a gate with an `if`: one object each, true and false at the right moments | PASS, synthetic holder |
| Places | an NPC and two Vukah asked for the middle of the base: on the voxel surface, at least 20 m from every base zone; the marker there, named by the journal's step | PASS |
| Creatures | verified after 8 s; one destroyed: logged, a new one elsewhere, the marker follows; destroyed again: gives up as an error; a reset ends it | PASS |
| N | a game NPC placed once, survives restart, a second spawn is destroyed | PASS |
| N | a clone in the roster: locked is destroyed; unlocked lives; one per world; registry follows | PASS |
| N | a clone without a roster entry | PASS |
| N | the entity's template slot reads the rebuilt data (override and clone) | PASS |
| N | `NpcCommandEvent` Reset moves it and sets its home | PASS |
| N | `GoTo`, `Follow`'s walking, `VillagerStateEvent` | **not seen in the lab**: the game ignores GoToPosition while an NPC is in state Start, and without a player NPCs stay in Start, so no state ever changed. Manual test |

A player's own knowledge cannot be written without a player, so the player-scope rows assert what the mirror wants (`NativeQuests.Wanted`, `Holds`) and the writing is shown by world quests, through the same code. Player maps, the client's reads and everything drawn are not covered without a player.

### To repeat
```sh
export KILN_OUT=<build dir>
mods/samples/NpcSample/build-assets.sh
for m in tests/quest-native-test/quest-native-test.csproj:quest-native-test samples/NpcSample/NpcSample.csproj:npcsample samples/quests-sample/QuestsSample.csproj:quests-sample; do
  dotnet build mods/${m%%:*} -c Release -o $KILN_OUT/mods/${m##*:} --artifacts-path $KILN_OUT/dotnet; done
tools/deploy-server.sh ~/srv/enshrouded/labs/<lab> --policy any
bash -c 'ulimit -e 1; exec nice -n 19 ionice -c 3 tools/lab/lab.sh start <lab>'; grep -a 'quest-native-test' ~/srv/enshrouded/labs/<lab>/kiln/logs/kiln.log   # run 1
tools/lab/lab.sh stop <lab>; tools/lab/lab.sh start <lab>                                                     # run 2: the save kept it
# without the mod:
mv <lab>/kiln/mods/quest-native-test/quests{,.off}; rm <lab>/kiln/data/kiln-quests/native/quest-native-test.json   # what it defines in code
touch <lab>/kiln/mods/{npcsample,quests-sample}/.disabled                                                     # the negative test, and the drop
```
The test changes the lab's world: after the whole sequence it only says so. Copy the save back and delete `kiln/data/quest-native-test/state.txt`, `kiln/data/worlds` and `kiln/data/kiln-quests` to repeat it. Without quest-engine-test in the same lab: both use `QuestInput`.

## Manual test
```sh
export KILN_OUT=<build dir>
mods/samples/NpcSample/build-assets.sh
dotnet build mods/samples/NpcSample/NpcSample.csproj -c Release -o $KILN_OUT/mods/npcsample --artifacts-path $KILN_OUT/dotnet
tools/client/client.sh backup-saves
tools/client/client.sh deploy --real --policy any --mod $KILN_OUT/mods/npcsample
```
Launch; click Skip on "Processing Vulkan shaders". Play → **LOCAL** tab → slot 1 (a character copy), host offline. Never the STEAM tab.
`kiln.log` must show `saves: world loaded: slot 0x0002 (world1.ksd)`. Commands go through the console folder:
```sh
C=~/.local/share/Steam/steamapps/common/Enshrouded/kiln/control/console
echo 'npcsample here' > $C/here.cmd
```

| # | Do | See or hear | `kiln.log` |
|---|---|---|---|
| N1 | Boot | | `knowledge: native: 1 mod(s): 17 new knowledge object(s), 1 quest(s), 1 NPC(s) in the roster, 3 template(s), <more than 0> text(s)` |
| N2 | In game, `npcsample here` | Tobin, with the fisher trader's look, about 3 m east of you | `npcsample: Tobin is at (…)` |
| N3 | Talk to him | Title "Tobin the Wanderer"; greeting "Well met, Flameborn. The night is cold and my fire is dying."; two 523 Hz beeps (three 784 Hz with German voice) | |
| N4 | Read the topics | "I can find you some kindling." with an attention marker, "Who are you?". No "Here are your twigs." | |
| N5 | Pick "Who are you?" | "Just a wanderer. I trade a little…", no sound | `quests: native: … chose npcsample:tobin hello[2]` |
| N6 | Pick the kindling topic | "Three twigs would do. Bless you." and a 997 Hz chime, 0.5 s; the game's new-quest notification | `chose … hello[0]`, then the quest started |
| N7 | Open the journal | "Kindling for Tobin", fisher portrait, step "Gather 3 Twigs for Tobin" with a counter 0/3 (holding twigs: it shows them for about 3 s, then passes) | |
| N8 | Open the map | Marker "Tobin's Kindling" 30 to 50 m from Tobin, outside any base; the game's waypoint pin there too | `tobin_kindling step gather: at (…) (the voxel surface, …)` |
| N9 | Talk to him with no twigs at the return step: `echo 'questadmin step <character> npcsample:tobin_kindling return' > $C/x.cmd` | "Bring the Twigs to Tobin" is active and its marker is on him, but **no "Here are your twigs." topic** | |
| N10 | `npcsample twigs` or gather 3, wait 2 s, talk, pick "Here are your twigs." | Thanks line and chime; 3 twigs taken; quest complete in the journal; about 100 XP; his summon tool in your inventory, named "Tobin the Wanderer", with the line "Tobin's Whistle" under the name. Talk again: the hand-in and the kindling topic are gone | `chose … hello[1]`, quest complete; **no** `must run on the tick thread`, no `FAILED` |
| N11 | Open his crafting menu, then the fisher trader's | "Tobin's Trade": 2 Twigs → 1 Plant Fibre, at both | |
| N12 | Journal > Survivors, before and after you hold his tool | The row reads "Tobin the Wanderer" / Fisher, with his portrait. The page: title "Tobin the Wanderer", text "Tobin the Wanderer trades a little and keeps a fire going…" (German: "Tobin der Wanderer", "… handelt ein wenig…"). Nowhere "Tobin's Whistle" as his name or "Calls Tobin to where you point" as his text | no `native:` warning about `summonItem` |
| N13 | In a base, use the whistle on a spot. Run `npcsample whistle` if it was used up, then aim at a bed | He moves there, then takes the bed | `npcsample status` shows him |
| N14 | `npcsample follow`, walk off, then `npcsample stay` | He walks after you, then stays there | `npcsample status` shows the state |
| N15 | Talk to Balthazar, if he's in this world | His usual topics plus "Have you met Tobin?", with chime | `chose npcsample:balthazar_tobin topic[0]` |
| N16 | Set text and voice language to German, talk to Tobin again | German text; three 784 Hz beeps on the greeting | |
| N17 | Quit to menu, host the same slot again | Quest still complete; one Tobin; the hand-in stays gone; no notification for the done quest | he isn't placed by the service: `npcsample here` spawned him, so no `native: NPC npcsample:tobin` line. Never `was placed once in this world and is gone` while he stands there |
| N17b | `echo 'questadmin reset <character> npcsample:tobin_kindling' > $C/x.cmd`, talk to him | The journal entry is gone; "I can find you some kindling." is back; the quest can be done again the same way | `tobin_kindling: reset` |
| N18 | Remove the mod | Game starts without him; the journal entry is gone | |

A topic missing at N4, or no `chose` line at N5, points at the client's option path.
