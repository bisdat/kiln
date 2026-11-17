# The Shroud Forge's gem actions (server 26430253, exe `f0d824be…`; client 26430625, exe `5d9ef2f1…`) **[dis] [refl] [rt]**

Also: two inventories in one action, payers and the return, and the client's GEMS grid for offered chest gems (GitHub
issue 61, sections below).

Evidence for kiln.gemcraft (gem wave G3: `Gems.Socket`, `Unsocket`, `Upgrade`, `Recycle`, `GemCraftingEvent`,
`GemCraftedEvent`). Starting points: the gem dossier (`/mnt/Dongus/kiln-wave/dev29/out/gem-system/DOSSIER.md` §1, §5) and
the Gem Forge report §6 (the hosted copy's Slot branch). Method: capstone, recursive descent from the handler, both exes;
reflection from `sigdb/types`; program lists from `sigdb/programs`. Signatures: `sigdb/symbols/server.d/gems-forge.json`
(the client composes it unchanged: `client.d/gems-forge.json` is an empty overlay that records the client's addresses).
Lab: `mods/tests/gems-forge-test` (below).

## The handler
`equipment_actions` (server update `0x1472c0`, client `0x388280`; flags `0x210`) calls, per player, the equip handler and
then the weapon-gem handler **`0x144bb0`** (client **`0x385880`**) with one argument: the program's unpacked iterator
(`rsp+0x20` in the update, which then calls iter_next with the size **0xf0**). The iterator follows the program's
declaration (`sigdb/programs/server/f0d824be….json`, 30 qwords):

| qword | offset | what | the handler uses it for |
|---|---|---|---|
| 0 | +0x00 | ProgramContext | the job (`[ctx]`), the current entity (= the player, `world/CurrentEntityId` `0x65b870`) |
| 1 | +0x08 | PlayerInput (read) | the request: `weaponGemAction` at `+0x260` |
| 2-7 | +0x10 | Equipment write record | Upgrade: the gem impact is despawned so it respawns at the new level (`0x1428b0`, `0x5fbe20`) |
| 8-13 | +0x40 | ServerConsumedPlayerInput write record | `+0x98` consumedWeaponGemAction; record `+0x28` = tracker (0: not marked) |
| 14-17 | +0x70..+0x88 | BalancingRegistry, ItemRegistry, RecipeRegistry, AttributeRegistry | costs; item lookups; the material multiplier; Level attributes |
| 18 | +0x90 | KnowledgeTriggerQueue | Slot: a knowledge trigger (`0x9a3cd0(queue, player, 7)`) |
| 19 | +0x98 | ShroudForge template accessor | `0x64f9f0(storage, accessor, forge)` (client `0x9b3b60`) |
| 20-27 | +0xa0..+0xd8 | Inventory, InventoryEmptyState, InventorySetup, ItemState, Level, OwnerRelationship, SlotSelection, WeaponGemSlot | the transaction's setup block (as `equipment_actions`' equip handler builds it) and the gem / socket reads |
| 28-29 | +0xe0, +0xe8 | GemCraftingEvent, InventoryFull senders | the success event; InventoryFull |

The request, `ChangeWeaponGemAction` (28 bytes) **[refl]**: `+0` weaponPide, `+4` weaponGem (InventorySlotId, 8 bytes),
`+0xc` forgeEntityId, `+0x10` materialItemId, `+0x14` versionData, `+0x18` action (`WeaponGemActionType`: Invalid 0, Slot 1,
Unslot 2, Upgrade 3, Recycle 4). `ShroudForge` (68 bytes): `+0x10` upgradeMaterial (an item GUID), `+0x20`
allowAllMaterialTiersForUpgraderInsert, `+0x24` maxGemLevel, `+0x28` craftingCostModifier.

Order **[dis]**: return at once when `input.version == consumed.version`; forge 0 → store the consumed version, return
(this is the cancel path kiln.gemcraft uses); the forge's template must read and have `maxGemLevel != 0`; the gem: the
request's slot when it is set (getItemStack `0x93fce0`; an entity without an Inventory resolves index 0 through its
WeaponGemSlot, so `{weaponPide, 0}` names the socket), else the weapon's WeaponGemSlot; its Level (`0x724bf568`) and cap
(`0x638a979a`) through AttrFind; then the action. Every branch stores the consumed version, whatever happened, **except**
when the gem's Level cannot be read: no Level component (server `0x144e93`, client `0x385b62`) or an attribute lookup
error (`0x144f59`/`0x144f62`, `0x385c28`/`0x385c31`) jumps past the store (`0x145019` / `0x385d13`), so the game offers
that version again every step until the client sends another (kiln.gemcraft's hold book runs such a version once and
never holds or reports it again).

The setup block the handler gives InventoryTxInit (server `0x144c1c..0x144d03`, client `0x3858ec..0x3859d3`, `rsp+0x70`)
**[dis]**: `+0` flags 4 (InventoryFull to the operator), `+8` the iterator's ctx, `+0x10` ItemRegistry, `+0x18`
BalancingRegistry, `+0x20` 0 (no AttributeRegistry), `+0x28` InventorySetup, `+0x30` Inventory, `+0x38` SlotSelection,
`+0x40` ItemState, `+0x48` OwnerRelationship, `+0x50` InventorySetup, `+0x58` InventoryEmptyState, **`+0x60`
WeaponGemSlot** (what makes `{weaponPide, 0}` a slot of the transaction), `+0xA8` the InventoryFull sender, `+0xC8` the
player (operator), everything else 0.

| action | branch (S / C) | cost or yield | what moves |
|---|---|---|---|
| Slot 1 | `0x1453c2` / `0x3860b9` | `0x938fa0` / `0xe91e60` (maxGemLevel, multiplier, modifier, Balancing), paid in `materialItemId` (removeItems on the same tx, must leave 0 missing) | `0x1b4950` / `0x414db0`: the exact source slot → `{weaponPide, 0}`, one item, the gem's entity kept |
| Unslot 2 | `0x1452a9` / `0x385fa0` | `0x938f30` / `0xe91df0` = `xor eax,eax; ret`: **free** on this build | `0x1b4bb0` / `0x415010` (sigdb `gems/UnslotMove`): `Result12* (out, tx, u64 src, u32 owner, u16 amount, u32 1, u8 1, u8 1)` called with `({weaponPide, 0}, the player, 1, 1, 1, 1)`: takeItems `0x1b9460` from `src`, then `0x1b4d10` puts the stack into `owner`'s inventory; the handler commits only when `out.error == 0 && out.value == 0` (value: what did not fit) |
| Upgrade 3 | `0x145118` / `0x385e0f` | refused when level ≥ cap or ≥ maxGemLevel; `0x939020` / `0xe91ee0` (level, maxGemLevel, multiplier, modifier, Balancing) | the payment is **committed first**, then Level +1 and its formulas (`0xab970`), then the gem impact is reset |
| Recycle 4 | `0x144f9a` / `0x385c69` | essence = the forge's upgradeMaterial; `0x939060` / `0xe91f20` (level, maxGemLevel, Balancing) | destroyItemStack (`0x1b27a0`) of the request's slot, addItems of the yield |

The multiplier: `0x940b80` / `0xe99940` (material, `0xff88b470`, RecipeRegistry) when the forge allows all material tiers,
else the constant 1.0 (`0xbd6b90` / `0x13d0c90`). The costs read the BalancingRegistry's table at `+0x20` (`+0x200`, `+0x204`,
`+0x20c`). Success sends `GemCraftingEvent {+8 forge, +0xc action}` (hash `0xe4bc5e18`, 16 bytes), which
`gem_crafting_sequence` plays as the forge's action sequence.

**No ownership check** in the handler: it reads neither the carrier's category nor its slot type, nor checks that the
source is a gem item, nor that the items are the player's (the Gem Forge report §6.3). Its "gem" is any stack with an item
entity: Recycle would destroy a sword in the request's slot for essence, Upgrade would raise a sword's Level. kiln.gemcraft
checks those itself before it calls the handler for code (a WeaponGem for Slot, Upgrade and Recycle; an unreadable
category is refused).

## What Kiln does (kiln-items.dll, `native/services/items/gems.cpp`)
- **Code actions**: the program `kiln_items_gems` (kiln.world; an entity program over the player characters: read
  PlayerInput) declares `equipment_actions`' access without the consumed input, plus Fuel and PickupEvent for kiln.items'
  own give/take. For a request it maps its iterator onto the handler's layout by name, points qword 1 at a request buffer
  of its own and qwords 8-13 at a consumed record of its own (tracker 0), and calls the handler. The layout is checked in
  the first Pre of each server world against `equipment_actions`' declaration in the game's program directory
  (`game_program_list`), and the handler iterator's eight accessors against the world's component map. A free or replaced
  cost gives the player the game's price right before the call (the game takes it back) and takes the replacement; a
  refusal undoes both. Recycle's yield is replaced after the call.
- **Players' actions**: a kiln.hooks link on the handler. While a hold lease runs (a `GemCraftingEvent` handler listens), a
  new version is not passed on: it stays unconsumed and the game offers it again next step. Pre reports it; the answer
  runs it (allow) or calls the handler once on a copy of the iterator whose request copy has forge 0 (cancel: the game
  only stores the consumed version; the forge-0 test is the handler's first decision, before any action branch, on both
  exes: server `0x144d06..0x144d2d`, client `0x3859d6..0x3859fd`; before it only the setup block on the stack and the
  current-entity read). 8 steps without an answer: it runs. A verdict stands after holding stops (the hook asks the
  book whenever it holds an entry); an action released before Pre saw it is not reported; a version that ran is
  remembered per player, so one the game leaves unconsumed runs again without a second event. While watched, the state
  before and after (socket, gem, Level, the material and essence counts of the player) gives `GemCraftedEvent`.
- **Free and replaced costs**: the take-back of Kiln's gift is measured (`gift_left`: the material count before and after
  the handler), not assumed, because Upgrade commits its payment before the Level write and a commit can roll back by
  itself. A replaced cost must be a plain stack (no HasPIDE): the take is by item id from anywhere.
- **The forgeless return** (`KILN_GEM_RETURN`, for `Gems.ReturnSocketedGems`): inside the player's update of
  `kiln_items_gems`, a transaction on Kiln's buffer with the handler's own setup block (above, from the mapped iterator),
  `gems/UnslotMove(out, tx, {carrier, 0}, player, 1, 1, 1, 1)`, commit on the handler's condition, else rollback; then the
  socket and the backpack are read again (Unslot's judge). No forge, no cost, no `GemCraftingEvent`.

## Two inventories, payers and the return (GitHub issue 61, 0.2.0-dev.29.15) **[dis]**
How the handler resolves items in a second inventory: through the transaction's own accessors, never through the player.
`getItemStack` of the request's `weaponGem` (an `InventorySlotId` = `{EntityId entityId, u8 slotIndex}` **[refl]**) reads
the named entity's `Inventory` (a chest's page is an entity like the backpack's), and `{weaponPide, 0}` reads the carrier's
`WeaponGemSlot`, wherever the carrier is. The Slot branch's move (`0x1b4950` / `0x414db0`) takes from the exact source slot
and puts into the socket on the one transaction; a request naming a chest's slot runs the same code. Only three calls name
the player (the current entity, `0x65b870` / `0x9bf9e0`):

| call | server | client (hosted copy) | arguments at the call |
|---|---|---|---|
| Slot's payment | `0x14544b` | `0x386142` | `PayMaterial` (`0x1b0d60` / `0x411330`, removeItems with no preferred slot): rcx out, rdx tx, r8d owner = the player, r9d item, `[rsp+0x20]` count |
| Unslot's payment | `0x145332` | `0x386029` | the same (the game skips it while the cost is 0) |
| Upgrade's payment | `0x1451bf` | `0x385eb6` | the same; the handler commits right after it |
| Unslot's move | `0x145392` | `0x386089` | `gems/UnslotMove`: rcx out, rdx tx, r8 src `{weaponPide, 0}`, r9d owner = the player |
| Recycle's give | `0x1450cb` | `0x385dc2` | `items/InventoryAddItems`: rcx out, rdx tx, r8d owner = the player, r9 slot none, `[rsp+0x20]` item, `[rsp+0x28]` count (the server and the client order the stack stores differently: two patterns) |

Each payment call is followed by `cmp byte [out], 0; jne rollback; cmp dword [out+8], 0; jne rollback`: anything
missing rolls the whole transaction back. Kiln (sigdb feature `gems.payers`) puts a mid hook on each call instruction
(after `mov r8d/r9d, [rax]`); while a code action with a route runs on that thread it takes from the first payers itself
(removeItems on the same tx) and points r8 at the last payer with the rest as the count, and r9 / r8 of the move and the
give at `return_to`. removeItems with a container as the owner is what the game's own crafting fill does (it takes from
the base's stock containers on a transaction whose operator is the player, docs/re/stations.md 3.1).

**Does the forge pay from base storage?** No. The three payments call removeItems with the player as the owner (it walks
the owner's InventorySetup links, docs/re/items.md 4), not `InventoryPay` / `BaseStockForPlayer` as crafting does. The
client's price test (below) counts only the player's Generic views.

## The client's GEMS grid, selection and request (client 26430625) **[dis]**
Evidence for `ui.gemforge.sources` (0.2.0-dev.29.15), continuing the snapshot's (`docs/kiln/ui-gemforge.md`; the #42
research report `/mnt/Dongus/kiln-wave/dev29/out/gemforge/REPORT.md`):
- **The grid** `ui/GemForgeGrid` (`0xcaefc0`, `void(state, header, rect, actions, float lower, u8 isWeapons)`) reads only
  the header's data (`+0`) and count (`+8`) (`0xcaf08a`, `0xcaf15a`, the loop `0xcaf200..0xcaf62f` steps 0x48). A loose gem
  row (ItemInfo category 15) is its own displayed view; the cell is `ui/ItemSlot` (`0xcaf595`). A click stores the row's
  first 8 bytes (its `InventorySlotId`) at state+0x10 (`0xcaf5b2`) and, but in mode 3, the row's weapon view at
  state+0x18 (empty for a loose gem).
- **The selected view** is resolved each frame in `ui/GemForgeMenu` from state+0x10 in the local model's main views only
  (`+0x2d8` / `+0x2e0`, the loop `0xcacf36..0xcacf74`); found, the 0x48 bytes go to rbp-0x20 and ecx = the item
  (`0xcacf83`); not found, the frame keeps an empty view (item 0 at rbp-0x18). Both meet at **`0xcacfa4`** (`mov rax,
  [UiApp]`), where the item is looked up (`0xe94470`, rcx = item) to tell a weapon (its socket view) from a gem. The
  selected view is the fourth argument of the action panel `0xd23f90` (`lea r9, [rbp-0x20]`, `0xcad0eb`).
- **The price test** in `0xd23f90` (r13 = state): the material is resolved from the forge's material selection (state
  +0xcc / +0xd0, `0xe99d40`), its count summed over the model's Generic index views (`+8` / `+0x10` into `+0x2d8`) into
  esi, the cost into r15d (`0xe91e60` Slot, `0xe91ee0` Upgrade). Insert: **`0xd243dc`** `cmp esi, r15d; setae [rsp+0x51]`;
  Upgrade: **`0xd246cc`** `cmp r12d, [r14+8]; jae; cmp esi, r15d; jb`. `[rsp+0x51]` is the confirm entry's enabled
  byte; the menu calls the dispatcher `0xd50cf0` only for a triggered entry (`0xcae827..0xcae84d`).
- **The request**: `0xd50cf0` builds UI command 0x5a (Upgrade, `0xda5b00`) or 0x5b (Insert, `0xd61e90`) with the
  selected view's key at +4, the weapon pide (state+0x2c) at +0xc, the material at +0x10 and the forge (play+0x2c50) at
  +0x14; Recycle is 0x5d (`0xd71650`). The command processor `0x85c7f0` (switch at `0x85c834`: 0x5a → `0x85dda9` action
  3, 0x5b → `0x85de13` action 1, 0x5c → `0x85de7d` Unslot, 0x5d → `0x85dec1` action 4) copies them into
  `ClientPlayerInput.weaponGemAction` (+0x258) with a new version (`0x9bd990`). Entity ids of replicated entities equal
  the server's (docs/re/network.md), so a row keyed by a chest's real `{page, slot}` arrives as that slot on the server.
- Kiln's hooks: a raw link on the grid (after the snapshot's) that draws a copy with the offered rows appended; a mid hook
  at `0xcacfa4` that writes the offered view to rbp-0x20 and rcx = its item when state+0x10 names an offered key and the
  model found nothing; mid hooks at `0xd243dc` / `0xd246cc` that set esi to what the offer's payer group holds (plus the
  player's own count for "player first"). Patterns in `sigdb/symbols/client.d/gemforge.json`; at install each site's
  bytes are compared with the instructions above. Static only: never run in a real client before release.

## Lab
`mods/tests/gems-forge-test` (lab `gems-g3`, a copy of the gems-g1 world, server 26430253, no client): a spawned
`1_Player` and a spawned `Shroud_Gem_Forge_01_Grassland`. Results: see the G3 report
(`/mnt/Dongus/kiln-wave/gems/out/g3-forge/REPORT.md`).

## Not known
- What a joined client's forge screen shows after a canceled action (the client predicts nothing for this host-only
  program; the inventory replicates as it is).
- Whether a socket on a non-weapon carrier (gem wave G4) behaves the same in the handler (no category read was found).
- The client's own UI command path (`0xda5b00` / `0xd61e90`) is not called by Kiln; `ui.gemforge.sources` relies on it
  sending the selected row's key as it is (above).
- Whether the detail panel draws everything of an offered chest gem when the gem's item entity is not replicated to the
  client (a chest far from the player); the cell needs only the item id, level and rarity.
