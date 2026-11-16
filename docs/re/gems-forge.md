# The Shroud Forge's gem actions (server 26430253, exe `f0d824be…`; client 26430625, exe `5d9ef2f1…`) **[dis] [refl] [rt]**

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

## Lab
`mods/tests/gems-forge-test` (lab `gems-g3`, a copy of the gems-g1 world, server 26430253, no client): a spawned
`1_Player` and a spawned `Shroud_Gem_Forge_01_Grassland`. Results: see the G3 report
(`/mnt/Dongus/kiln-wave/gems/out/g3-forge/REPORT.md`).

## Not known
- What a joined client's forge screen shows after a canceled action (the client predicts nothing for this host-only
  program; the inventory replicates as it is).
- Whether a socket on a non-weapon carrier (gem wave G4) behaves the same in the handler (no category read was found).
- The client's own UI command path (`0xda5b00` / `0xd61e90`) is not used.
