# Blocks (Kiln.Blocks): items

## AmmoBlock — What an arrow adds to the bow that shoots it.
- `Damage { get; init; } -> DamageBlock?` — The kinds it deals: the shares count, as for a weapon.
- `Delivery { get; init; } -> IDeliveryBlock?` — A flight of its own.
- `Power { get; init; } -> int?` — The power it adds to the bow's.

## AnimatedModel — A held model that moves: the skinned model and the graph, each a GUID or the name of the `model` or `graph` step of `assets/cook.json` that makes it (the cook derives...
- `Graph { get; init; } -> string?` — The graph that plays its clip: a GUID, or the name of its `graph` step.
- `LeftGraph { get; init; } -> string?` — Daggers only: the left hand's graph.
- `LeftModel { get; init; } -> string?` — Daggers only: the left hand's model.
- `Model { get; init; } -> string?` — The skinned model: a GUID, or the name of its `model` step.

## ArmorBlock — Something worn: its slot, its stats and its look.
- `Description { get; init; } -> string?` — The piece's description in its tooltip.
- `Look { get; init; } -> PartRef<WornLook>?` — What it looks like on the body: a model of the mod's own, skinned or on one joint, for both bodies...
- `Loot { get; init; } -> LootBlock?` — Where else it can be found.
- `Name { get; init; } -> string` — The piece's name, as players read it in its tooltip.
- `OnHitTaken { get; init; } -> HitsRef?` — What every hit the wearer takes does to whoever dealt it, while it is worn, with no unlock.
- `Perks { get; init; } -> IReadOnlyList<PerkRef>?` — Enhancement slots, from the first.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Set { get; init; } -> PartRef<ArmorSet>?` — A family of looks: this piece takes the look the set has for its slot.
- `Slot { get; init; } -> PartRef<ArmorSlot>` — Where it is worn: `ArmorSlot.Chest`, or your own slot.
- `Source { get; init; } -> string?` — Where to get it, in the tooltip.
- `Stats { get; init; } -> PartRef<ArmorStats>?` — Its level, rarity, armour and bonuses.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — What it looks like as an item.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## ArmorLook — An armor set's look for a slot: a `VisualBlock` (a game item's look) or a `WornLook`, inline, from `Blocks.Define`, or by name.
- `static implicit operator ArmorLook(BlockHandle<VisualBlock> handle) -> ArmorLook` — The visual block that `Blocks.Define` returned.
- `static implicit operator ArmorLook(BlockHandle<WornLook> handle) -> ArmorLook` — The worn look that `Blocks.Define` returned.
- `static implicit operator ArmorLook(VisualBlock block) -> ArmorLook` — A visual block written in place.
- `static implicit operator ArmorLook(WornLook block) -> ArmorLook` — A worn look written in place.
- `static implicit operator ArmorLook(string name) -> ArmorLook` — A look by block name, of either kind.
- `Name { get; } -> string?` — A block's name, of either kind: which one is found when the set is used.
- `Visual { get; } -> PartRef<VisualBlock>?` — The visual block (a game item's look), when the look was made from one.
- `Worn { get; } -> PartRef<WornLook>?` — The worn look, when the look was made from one.

## ArmorResist — Armour against one kind.
- `Blunt { get; init; } -> int` — Armour against blunt damage.
- `Cut { get; init; } -> int` — Armour against cutting damage.
- `Fire { get; init; } -> int` — Armour against fire damage.
- `Ice { get; init; } -> int` — Armour against ice damage.
- `Lightning { get; init; } -> int` — Armour against lightning damage.
- `Pierce { get; init; } -> int` — Armour against piercing damage.
- `Shroud { get; init; } -> int` — Armour against shroud damage.

## ArmorSet — A family of looks, one per slot.
- `Arms { get; init; } -> ArmorLook?` — The look for the arms slot: a `VisualBlock` or a `WornLook`.
- `Chest { get; init; } -> ArmorLook?` — The look for the chest slot: a `VisualBlock` or a `WornLook`.
- `Feet { get; init; } -> ArmorLook?` — The look for the feet slot: a `VisualBlock` or a `WornLook`.
- `Head { get; init; } -> ArmorLook?` — The look for the head slot: a `VisualBlock` or a `WornLook`.
- `Key { get; init; } -> string` — How JSON names a premade.
- `Legs { get; init; } -> ArmorLook?` — The look for the legs slot: a `VisualBlock` or a `WornLook`.
- `static readonly Mage -> ArmorSet` — The game's mage armour looks, one per slot.
- `static readonly Ranger -> ArmorSet` — The game's ranger armour looks, one per slot.
- `static readonly Warrior -> ArmorSet` — The game's warrior armour looks, one per slot.

## ArmorSlot — Where a piece is worn.
- `static readonly Arms -> ArmorSlot` — The arms slot.
- `static readonly Chest -> ArmorSlot` — The chest slot.
- `static readonly Feet -> ArmorSlot` — The feet slot.
- `static readonly Head -> ArmorSlot` — The head slot.
- `Key { get; init; } -> string` — How JSON names a premade.
- `static readonly Legs -> ArmorSlot` — The legs slot.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from.
- `static readonly Ring -> ArmorSlot` — The ring slot.
- `Stats { get; init; } -> PartRef<ArmorStats>?` — The stats pieces of this slot have unless they say otherwise.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — The look pieces of this slot have unless they say otherwise.

## ArmorStats — What a piece of armor gives.
- `Bonus { get; init; } -> IDictionary<EquipBonus, double>?` — What wearing it adds: `{ [EquipBonus.Health] = 60 }`.
- `Level { get; init; } -> int` — The tooltip's level; a made piece has exactly this.
- `Magical { get; init; } -> int?` — The tooltip's magical armour.
- `Physical { get; init; } -> int?` — The tooltip's physical armour.
- `Rarity { get; init; } -> Rarity` — The rarity: its colour, and how many enhancements it can have (see `ArmorStats.Rarity`).
- `Resist { get; init; } -> ArmorResist?` — Armour against one kind, on top of the two above.

## Barber — Hair and beards on characters, on the server world.
- `static Of(EntityId character) -> (uint Hair, uint Beard)?` — The item ids a character wears as hair and beard.
- `static Wear(EntityId character, BlockHandle look) -> bool` — Puts a hair or beard block on a character: a player, or a villager.
- `static Wear(EntityId character, uint item, bool beard) -> bool` — The same with an item id: a game hair or beard (`VC_Hair_*`, `VC_Beard_*`), or 0 for none.

## BeardBlock — A beard: a `WornLook` on the jaw.
- `Description { get; init; } -> string?` — What the item says about the beard, as players read it.
- `Look { get; init; } -> PartRef<WornLook>` — The look worn on the jaw: a `WornLook` written in place, by handle or by block name.
- `Name { get; init; } -> string` — The beard's name, as players read it on its item.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## BlockIdentity `0.2.14` — A fixed identity for an item or recipe block, in place of the one Kiln derives from the mod and the block's name: `Identity = BlockIdentity.Legacy(new Guid("..."))`.
- `Guid { get; } -> Guid` — The GUID the block is defined under.
- `Id { get; } -> uint` — The game's id of it: FNV-1a over the GUID's 16 bytes, as for every item and recipe.
- `static Legacy(Guid guid) -> BlockIdentity` — The GUID another loader gave the item or recipe.
- `static Legacy(uint id) -> BlockIdentity` — Only the game id another loader gave it, when its GUID is not known: the block gets a GUID whose FNV-1a is `id` (the...

## BodyPart — A body part or a worn piece a look hides while it is worn.
enum: Head, Hair, EyeBrow, Beard, Neck, Collar, Chest, Waist, Crotch, UpperArms, LowerArms, Hands, UpperLegs, LowerLegs, Feet, ArmorHead, ArmorUpperBody, ArmorArms, ArmorLowerBody, ArmorFeet, RingLeft, RingRight, Shield, MainHand

## CosmeticBlock — A piece for a vanity slot: it shows over what the player wears there, with no stats of its own and none taken away.
- `Description { get; init; } -> string?` — The piece's description in its tooltip.
- `Dyes { get; init; } -> bool` — Takes the game's dyes.
- `Look { get; init; } -> PartRef<WornLook>` — What it looks like on the body.
- `Loot { get; init; } -> LootBlock?` — Where else it can be found.
- `Name { get; init; } -> string` — The piece's name, as players read it in its tooltip.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Slot { get; init; } -> CosmeticSlot` — The vanity slot it goes in.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## CosmeticSlot — Which vanity slot it goes in.
enum: Head, UpperBody, Arms, LowerBody, Feet

## DropPlace — A place in the game's drop tables: where an item goes into a table the game already has.
- `Chance { get; init; } -> double` — 0..1.
- `CountMax { get; init; } -> int` — The most it drops at a time.
- `CountMin { get; init; } -> int` — The fewest it drops at a time.
- `DropsLike { get; init; } -> ItemRef?` — Every table that drops this item, next to it.
- `Table { get; init; } -> string?` — A table's GUID or a glob over GUIDs.
- `Weight { get; init; } -> double` — Weight where the table picks one of several.

## EquipBonus — A stat a piece of gear adds while it is worn.
- `EquipBonus(string Key, string Config, uint Row, string Format, bool Whole)`
- `Config { get; init; } -> string` — The id of the item config the value goes into, as `0x` and eight hex digits.
- `static readonly CritChance -> EquipBonus` — Critical hit chance, as a share: 0.05 is 5%.
- `static readonly CritDamage -> EquipBonus` — Critical hit damage, as a share: 0.05 is 5%.
- `Format { get; init; } -> string` — How the tooltip row shows it: `"Normal"` (a number) or `"Percentage"` (a share times 100).
- `static readonly FrostResist -> EquipBonus` — Frost resistance, in whole points.
- `static readonly Health -> EquipBonus` — Health, in whole points.
- `static readonly HealthRegen -> EquipBonus` — Health regeneration, in whole points.
- `Key { get; init; } -> string` — The name JSON gives this bonus (`"health"`, `"melee-damage"`).
- `static readonly MagicDamage -> EquipBonus` — Magic damage, as a share: 0.05 is 5%.
- `static readonly Mana -> EquipBonus` — Mana, in whole points.
- `static readonly ManaRegen -> EquipBonus` — Mana regeneration, in whole points.
- `static readonly MeleeDamage -> EquipBonus` — Melee damage, as a share: 0.05 is 5%.
- `static readonly RangedDamage -> EquipBonus` — Ranged damage, as a share: 0.05 is 5%.
- `Row { get; init; } -> uint` — The id of the tooltip row that shows the bonus.
- `static readonly SprintSpeed -> EquipBonus` — Sprint speed, as a share: 0.05 is 5%.
- `static readonly Stamina -> EquipBonus` — Stamina, in whole points.
- `static readonly StaminaRegen -> EquipBonus` — Stamina regeneration, in whole points.
- `Whole { get; init; } -> bool` — True: whole points, so the value must be a whole number.

## FishingBlock — How a fishing rod fights a fish.
- `Endurance { get; init; } -> double?` — How long it holds out: the tooltip's Endurance (30 and 45).
- `Strength { get; init; } -> double?` — How hard it pulls: the tooltip's Strength (the game's simple rod: 1, the improved one: 2).

## Fit — A fit: the flags the game raises for what a character wears and is.
enum: None, Female, Hat, Hood, Jaw01, Jaw02, Jaw03, Jaw04, Jaw05, Jaw06, Jaw07, Jaw08, Jaw09, Jaw10, Jaw11, Jaw12, Jaw13, Jaw14, Jaw15, Jaw16, NoArmor, Shield, Tight, ArmsTight01, LegsBeltTight, LegsFeetTight, LegsFeetTight01

## HairBlock — A hair style: a `WornLook` on the head.
- `Description { get; init; } -> string?` — What the item says about the style, as players read it.
- `Look { get; init; } -> PartRef<WornLook>` — The look worn on the head: a `WornLook` written in place, by handle or by block name.
- `Name { get; init; } -> string` — The hair style's name, as players read it on its item.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## Harvest — Shorthands for `ToolBlock.Harvests`.
- `static readonly Ground -> ToolClass` — A rake: it flattens the ground.
- `static readonly Stone -> ToolClass` — A pickaxe: stone, metal, and digging.
- `static readonly Wood -> ToolClass` — An axe.

## HitsRef — Effects on a hit: `[burst]`, an `OnHitBlock` inline or from `Blocks.Define`, or a block's name.
- `HitsRef()` — An empty list.
- `Add(EffectRef effect) -> void` — The list form: `OnHit = [burst, other]`.
- `Count { get; } -> int` — The number of effects in the list form.
- `GetEnumerator() -> IEnumerator<EffectRef>` — The effects of the list form, in order.
- `static implicit operator HitsRef(BlockHandle<OnHitBlock> handle) -> HitsRef` — The on-hit block that `Blocks.Define` returned.
- `static implicit operator HitsRef(OnHitBlock block) -> HitsRef` — An on-hit block written in place.
- `static implicit operator HitsRef(string name) -> HitsRef` — An on-hit block by name.
- `Part { get; } -> PartRef<OnHitBlock>?` — The on-hit block this names, when it is not a plain list.

## ItemBlock — Anything else that goes in a bag: a material, a potion, food, an arrow, a tool.
- `Ammo { get; init; } -> AmmoBlock?` — What an arrow adds to the bow.
- `CanEat { get; init; } -> EatRule?` — When it can be used.
- `Comfort { get; init; } -> int?` — The comfort a placed one gives.
- `Description { get; init; } -> string?` — The item's description in its tooltip.
- `Feeds { get; init; } -> StatusRef?` — The status eating it gives, in the game's food slots.
- `Flow { get; init; } -> double?` — How fast a water container fills and pours, as a multiple of the game's own: at a lake, at a well or a barrel, and...
- `Fuel { get; init; } -> double?` — Seconds it burns in a fire.
- `Heals { get; init; } -> int?` — Health using it gives at once; the tooltip's number.
- `Holds { get; init; } -> int?` — A `ItemKind.WateringCan`'s or `ItemKind.WaterPouch`'s capacity, in charges as the game counts them: the game's...
- `Identity { get; init; } -> BlockIdentity?` `0.2.14` — A fixed GUID (and so game id) in place of the one Kiln derives from the mod and the name...
- `Kind { get; init; } -> ItemKind?` — Null: a `ItemBlock.Tool` makes a tool, `ItemBlock.Ammo` an arrow, `ItemBlock.Feeds` food, `ItemBlock.Heals` or...
- `LargeHolds { get; init; } -> bool` `0.2.17` — Lets `ItemBlock.Holds` go past 1,023, up to 65,535 charges.
- `Level { get; init; } -> int?` — The tooltip's level, for what has one.
- `Loot { get; init; } -> LootBlock?` — Where else it can be found.
- `Name { get; init; } -> string` — The item's name, as players read it in its tooltip.
- `OnEat { get; init; } -> IReadOnlyList<EffectRef>?` — What happens to whoever used it, once the item has really gone.
- `OnUse { get; init; } -> IReadOnlyList<EffectRef>?` — What using it does, to whoever uses it.
- `Rarity { get; init; } -> Rarity?` — Its rarity.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Source { get; init; } -> string?` — Where to get it, in the tooltip.
- `Stack { get; init; } -> int?` — How many fit in a stack.
- `Texts { get; init; } -> IReadOnlyDictionary<string, ItemTexts>?` — Texts per language: `["de"] = new ItemTexts { Name = "..." }`.
- `Tool { get; init; } -> ToolBlock?` — What a tool does.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `VanillaLook { get; init; } -> string?` `0.2.17` — The game item players without this mod see in its place (a vanilla client, a Kiln client without this mod's...
- `Visual { get; init; } -> PartRef<VisualBlock>?` — What it looks like.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## ItemKind — What sort of item it is: one of these, each checked for the fields it uses.
- `static readonly Ammo -> ItemKind` — An arrow, which adds to the bow that shoots it.
- `static readonly Consumable -> ItemKind` — A potion or other consumable, used by whoever holds it.
- `static readonly Food -> ItemKind` — Food: eating it gives a status (`ItemBlock.Feeds`).
- `Key { get; } -> string` — How JSON names it.
- `static readonly Material -> ItemKind` — Something to craft with or keep: the default when nothing else fits.
- `Starter { get; } -> string` — The game item it starts from.
- `static readonly Tool -> ItemKind` — A tool: what it harvests says what it does.
- `static readonly WateringCan -> ItemKind` — The game's watering can: filled at a lake or a well, poured on crops and soil.
- `static readonly WaterPouch -> ItemKind` — The game's water pouch: filled at a lake or a well, a sip at a time to drink.

## ItemSource `0.2.12` — Which creations of an item a `RarityRollBlock` rolls.
enum: None, Craft, Loot, Give

## LootBlock — Where it can be found, besides being made.
- `Like { get; init; } -> ItemRef?` — It is rolled in chests and world pickups where this item is, with its chances.
- `Rolled { get; init; } -> bool` — Gear only: a roll gives it a level and rarity of its own, rather than the one fixed level and rarity.
- `StackMax { get; init; } -> int?` — The most in the stack a roll gives.
- `StackMin { get; init; } -> int?` — The fewest in the stack a roll gives.
- `Tables { get; init; } -> IReadOnlyList<DropPlace>?` — The game's own drop tables it goes into: what enemies and fixed chests hold.

## LootEntry — One stack a `LootTableBlock` can give.
- `Chance { get; init; } -> double` — 0..1: how often this entry gives anything when it comes up.
- `CountMax { get; init; } -> int?` — How many, at most.
- `CountMin { get; init; } -> int` — How many, at least.
- `Item { get; init; } -> ItemRef` — A `GameItems` constant, or an item of this mod by its block name (or `"other-mod:name"`).
- `Weight { get; init; } -> double` — With `LootPick.One`: how likely this entry is to be the one picked, among the table's.

## LootPick — What one roll of a `LootTableBlock` does.
enum: Each, One

## LootStack — What a roll gave: an item, by the name the game and `Items.Give` know it by, and how many.
- `LootStack()`
- `LootStack(string Item, int Count)`
- `Count { get; init; } -> int` — How many, 1..65535.
- `Item { get; init; } -> string` — The item's name in the game.

## LootTableBlock — A table of what drops.
- `DropsLike { get; init; } -> ItemRef?` — Not a field of a loot table: see `DropPlace.DropsLike`.
- `Entries { get; init; } -> IReadOnlyList<LootEntry>` — What can come up.
- `Pick { get; init; } -> LootPick` — What one roll does: every entry tries its own chance, or one entry is picked by weight.
- `Rolls { get; init; } -> int` — How many times the table rolls, 1..64.
- `Table { get; init; } -> string?` — Not a field of a loot table: see `DropPlace.Table`.

## LootTableHandle — What `Blocks.Define` gives back for a loot table: roll it, drop it, give it.
- `Drop(WorldPosition at, Random? random = null) -> Task<IReadOnlyList<EntityId>>` — Rolls it and drops what comes up as loose pickups around `at` (the game's generic pickup: collected like loot).
- `Give(EntityId inventory, Random? random = null, GiveOptions options = GiveOptions.None) -> Task<IReadOnlyList<ItemsResult>>` — Rolls it and gives what comes up into an inventory (a chest, a player, a villager), as `Items.Give` does.
- `Roll(Random? random = null) -> IReadOnlyList<LootStack>` — One go at the table: what comes up, one stack per item.

## MoveAnimation — The states a move plays: one name (`Animation = "cinder-guard"`) or one per state event of the motion, in order (`Animation = ["start", null, "land"]`, a null keeping...
- `MoveAnimation()` — An empty list of state names.
- `MoveAnimation(IEnumerable<string?> names)` — These state names, one per state event in order; a null keeps that one.
- `Add(string? name) -> void` — Adds a state name at the end (collection initializers use this); null keeps that state.
- `Count { get; } -> int` — The number of names.
- `GetEnumerator() -> IEnumerator<string?>` — The names in order.
- `static implicit operator MoveAnimation(string name) -> MoveAnimation` — One state name for the whole move.
- `static implicit operator MoveAnimation(string?[] names) -> MoveAnimation` — These state names, in order.
- `this[int i] { get; } -> string?` — The name at `i`, from 0; null keeps that state.

## MoveApplicationResult `0.2.3` — What `Blocks.MovesStatus` answers: a snapshot that never changes.
- `AppliedGeneration { get; init; } -> ulong` — The number of the attempt whose slots are playing: the last one that went through.
- `ContentGeneration { get; init; } -> ulong` — The content generation (`Content.Status().Generation`) the last attempt read the game's data in.
- `LiveClones { get; init; } -> IReadOnlyDictionary<string, Guid>` — The sequences the applied generation plays, by `slot/link` (`"primary/1"`, `"block/1"`).
- `Ok { get; init; } -> bool` — True only when `MoveApplicationResult.State` is `MoveApplicationState.Applied`.
- `Reason { get; init; } -> string` — Empty when applied; `"PendingContent"`, `"Removed"`, or why the last attempt was refused (the block's field and what...
- `RequestedGeneration { get; init; } -> ulong` — The number of the last attempt, counted from 1 for this weapon while its mod is loaded.
- `State { get; init; } -> MoveApplicationState` — Where the weapon's moves are.

## MoveApplicationState `0.2.3` — Where a weapon's moves with follow-ups are.
enum: PendingContent, Applied, Refused, Removed

## MoveBlock — One move: the motion an attack slot plays, when its blow lands, and where its follow-up windows lead.
- `Animation { get; init; } -> MoveAnimation?` — The state the move's motion plays: a state of this mod by name, a game state by name (`"State"` or...
- `FollowUps { get; init; } -> IReadOnlyList<MoveFollowUpBlock>?` `0.2.3` — Where the motion's follow-up windows lead, when not where the class has them: each entry points the windows for one...
- `HitAt { get; init; } -> double?` — Seconds into the move its blow lands: the first damage event moves here, and what comes before it is scaled to fit.
- `Length { get; init; } -> double?` — Seconds the whole move lasts: what comes after the blow is scaled to end here.
- `Like { get; init; } -> string?` — The game sequence this move starts from: a sub-sequence name or a sequence GUID.

## MoveChain — One move or a chain of them: `Primary = move`, `Primary = [first, second]`, in JSON an object or a list.
- `MoveChain()` — An empty chain.
- `MoveChain(IEnumerable<MoveBlock> moves)` — A chain of these moves, in order.
- `Add(MoveBlock move) -> void` — Adds a move at the end (collection initializers use this).
- `Count { get; } -> int` — The number of moves.
- `GetEnumerator() -> IEnumerator<MoveBlock>` — The moves in order.
- `static implicit operator MoveChain(MoveBlock move) -> MoveChain` — A chain of one move.
- `static implicit operator MoveChain(MoveBlock[] moves) -> MoveChain` — A chain of these moves, in order.
- `this[int i] { get; } -> MoveBlock` — The move at `i`, from 0.

## MoveFollowUpBlock `0.2.3` — One route: the follow-up windows of this move that wait for `MoveFollowUpBlock.Trigger` lead to another move of the same weapon.
- `Link { get; init; } -> int` — Which link of that slot's chain, from 1.
- `Occurrence { get; init; } -> int?` — Which of the windows for `MoveFollowUpBlock.Trigger` in that sub-sequence, from 0 in the motion's own order (by...
- `SubSequence { get; init; } -> int` — Which sub-sequence of the motion, from 0.
- `To { get; init; } -> WeaponMoveSlot` — The slot of this weapon the windows lead to.
- `Trigger { get; init; } -> MoveFollowUpTrigger` — The input or outcome the windows wait for.

## MoveFollowUpTrigger `0.2.3` — What a follow-up window of a motion waits for.
enum: Primary, Heavy, Block, Parried

## OnHitBlock — What an item does to what its wearer hits.
- `Effects { get; init; } -> IReadOnlyList<EffectRef>` — What each hit does.
- `Vfx { get; init; } -> VfxRef?` — A VFX where a hit lands, as an effect with only a `EffectBlock.Vfx` would show it.

## `PartRef<T>` — A part of a block: inline, from `Blocks.Define`, a premade's key, or a block's name.
- `static implicit operator PartRef<T>(BlockHandle<T> handle) -> PartRef<T>` — The part that `Blocks.Define` returned.
- `static implicit operator PartRef<T>(T block) -> PartRef<T>` — A part written in place.
- `static implicit operator PartRef<T>(string name) -> PartRef<T>` — A part by name: a premade's key, or a block's name.
- `Inline { get; } -> T?` — The part itself, when the reference holds one: written in place, or made from a handle.
- `Name { get; } -> string?` — The part's name: `mod:name` when made from a handle.

## PerkRef — A perk in an enhancement slot: a game perk's name or GUID (`GamePerks`), `item:<name>` (that item's perk in the same slot), or `none`, or a `PerkBlock` by handle or name.
- `PerkRef()`
- `PerkRef(string? Value)`
- `static implicit operator PerkRef(BlockHandle<PerkBlock> handle) -> PerkRef` — The perk that `Blocks.Define` returned.
- `static implicit operator PerkRef(string? value) -> PerkRef` — A perk by name: a `GamePerks` constant or GUID, `item:<name>`, `none`, or a `PerkBlock`'s block name.
- `Value { get; init; } -> string?` — The perk as written; null or empty: none given.

## Rarity — What a made item is: its colour, and how many enhancements it can have (one more than the rank).
enum: Common, Uncommon, Rare, Epic, Legendary

## RarityRollBlock `0.2.12` — A rarity rolled when an item is made: for the item types it names, the server picks Common to Legendary by weight whenever the game crafts one or loot makes one, and...
- `Items { get; init; } -> IReadOnlyList<ItemRef>` — The item types: `GameItems` constants, items of this mod by block name, `"other-mod:name"`, or a pattern with `*`...
- `Sources { get; init; } -> ItemSource` — Which creations roll (default crafts and loot).
- `Weights { get; init; } -> IReadOnlyDictionary<Rarity, double>` — How likely each rarity is, among the five; a rarity left out is 0.

## RarityUpgradeBlock `0.2.12` — A rarity upgrade: the rule a station, a command or a quest runs on an item through `Items.UpgradeRarity(player, item, "name")`.
- `AllowEmptySlot { get; init; } -> bool` — Upgrade into an enhancement slot the item's type has no perk for (default false: refused).
- `Durability { get; init; } -> DurabilityRule` — What happens to durability (default `DurabilityRule.KeepRatio`).
- `Items { get; init; } -> IReadOnlyList<ItemRef>?` — The item types it takes: `GameItems` constants, items of this mod by block name, `"other-mod:name"`, or a pattern...
- `Max { get; init; } -> Rarity` — The highest rarity it upgrades to (default Legendary).
- `Socket { get; init; } -> SocketRule` — What happens about a gem socket (default `SocketRule.Keep`).
- `Steps { get; init; } -> IDictionary<Rarity, UpgradeStep>` — The steps, by the rarity each reaches (Uncommon ..

## RecipeBlock — How something is crafted.
- `Burns { get; init; } -> IDictionary<ItemRef, int>` — Fuel: what each cycle burns.
- `Comfort { get; init; } -> int?` — The comfort level it asks of whoever makes it.
- `Identity { get; init; } -> BlockIdentity?` `0.2.14` — A fixed GUID (and so recipe id) in place of the one Kiln derives: `BlockIdentity.Legacy(guid)`, for a recipe another...
- `Makes { get; init; } -> int` — How many one craft gives.
- `MenuGroup { get; init; } -> CraftingGroupRef?` `0.2.14` — The tab of the game's crafting menu that lists it, one the game has: `CraftingGroups.Carpenter.Essentials`.
- `MenuKey { get; init; } -> string?` `0.2.17` — Its name in the menu of a station with tabs of its own (`MenuBlock.Tabs`, `StationMenus.Define`): letters, digits...
- `Needs { get; init; } -> IDictionary<ItemRef, int>` — Item and count: `GameItems` constants, or an item this mod defines.
- `NeedsProps { get; init; } -> IReadOnlyList<PropRef>?` — Props that have to stand near the station: blocks that `PropBlock.Provides` one, by handle or name...
- `Output { get; init; } -> ItemRef?` — What it makes: in a station's `StationBlock.Recipes`, and in a recipe block of its own, where it is needed.
- `Seconds { get; init; } -> double?` — Seconds at a timed station.
- `Sheltered { get; init; } -> bool?` — A player crafts it only under a roof, the game's rule for most bench recipes.
- `Station { get; init; } -> StationRef` — `Stations.Hand`, a `GameWorkshops` constant, or a `StationBlock` of this mod.
- `TrackCrafted { get; init; } -> bool` `0.2.14` — Keeps the game's "crafted once" mark of the recipe, as its own recipes have it; a `KnowledgeBlock` with...

## StationRef — Where a recipe is crafted: `"hand"`, a workshop's `"0x..."` id, or a station block by handle or name.
- `StationRef()`
- `StationRef(string Value)`
- `static implicit operator StationRef(BlockHandle<StationBlock> handle) -> StationRef` — The station that `Blocks.Define` returned.
- `static implicit operator StationRef(string value) -> StationRef` — A place by name: `"hand"` (`Stations.Hand`), a workshop's `"0x..."` id or a `GameWorkshops` constant, or a station...
- `Value { get; init; } -> string` — The place as written: `"hand"`, a workshop's `"0x..."` id, or a station block's name.

## Stations — Placed stations that work on their own (kilns, smelters, looms, grinders, seed beds: every entity with the game's `FactoryStation`), on the server, on the tick thread.
- `static All() -> IReadOnlyList<EntityId>` `0.2.6` — Every production station of the world (a scan at most every 32 steps).
- `static Available { get; } -> bool` `0.2.6` — True when kiln.stations reads here (a server world, stations.read on).
- `static Categories { get; } -> IReadOnlyList<IngredientCategory>` `0.2.6` — Every ingredient category of the game's recipes (22 in its data).
- `static Category(uint id) -> IngredientCategory?` `0.2.6` — One ingredient category; null for an unknown id.
- `static Collect(EntityId station, EntityId to, ItemId? item = null, CollectWhat what = CollectWhat.Outputs) -> Task<StationResult>` `0.2.6` — Moves a station's finished items (`StationInfo.Outputs`; `what` can add or choose `StationInfo.Leftovers`), or only...
- `static CollectToBase(EntityId station, ItemId? item = null, CollectWhat what = CollectWhat.Outputs) -> Task<StationResult>` `0.2.6` — As `Stations.Collect`, into the station's base: for each stack a crafting-stock chest that takes deposits and holds...
- `const ControlCapability = "stations.control" -> string` `0.2.6` — `Stations.Stop`, `Stations.Resume`, `Stations.Select` (sigdb feature).
- `const CraftSourcesCapability = "stations.craft_sources" -> string` `0.2.9` — The same opt-in, as the last source of a player's craft (sigdb feature; it also needs kiln.rules' payment hook, and...
- `const CraftUiCapability = "stations.craft_ui" -> string` `0.2.9` — A Kiln client's crafting menu counts the opted-in outputs the server sends (sigdb feature, client; it also needs...
- `const EventsCapability = "stations.events" -> string` `0.2.6` — The station events (sigdb feature).
- `static Fill(EntityId station, FillOptions? options = null) -> Task<StationResult>` `0.2.6` — Fills a station from its base's crafting-stock chests with the game's own "fill from chests": every input topped up...
- `const FillCapability = "stations.fill" -> string` `0.2.6` — `Stations.Fill` (sigdb feature).
- `const Hand = "hand" -> string` — The player's own crafting menu.
- `static InBase(BaseId id) -> IReadOnlyList<EntityId>` `0.2.6` — The production stations standing in a base; empty when no base has this id now (the game rebuilds its bases every...
- `static IsOutputSource(EntityId station) -> bool` `0.2.9` — Whether the station is opted in as crafting stock now (`Stations.SetOutputSource`).
- `static IsStation(EntityId entity) -> bool` `0.2.6` — Whether the entity is a production station now.
- `static Nearest(EntityId entity, uint? workshop = null, double radius = 8) -> EntityId?` `0.2.6` — The production station nearest to `entity` (a player) within `radius` metres, optionally of one workshop; null when...
- `static OutputSources(BaseId? base = null) -> IReadOnlyList<EntityId>` `0.2.9` — The stations opted in as crafting stock now, in the order a fill takes from them (entity order); with `base` only...
- `const OutputSourcesCapability = "stations.output_sources" -> string` `0.2.9` — `Stations.SetOutputSource` and the fills that take from opted-in stations (sigdb feature).
- `static Plan(EntityId station, FillOptions? options = null) -> FillPlan` `0.2.6` — What `Stations.Fill` with these options would do now (no game function runs).
- `static Read(EntityId station) -> StationInfo?` `0.2.6` — A station now, with its inventory and what it would collect; null when the entity is no station.
- `const ReadCapability = "stations.read" -> string` `0.2.6` — Reads: bases, stations, recipes, plans (sigdb feature).
- `static Recipe(uint id) -> StationRecipe?` `0.2.6` — A recipe as a station runs it; null for an unknown id.
- `static RecipeName(uint recipe) -> string?` `0.2.7` — A recipe's name for players (since 0.2.0-dev.29.15): its first output's name ("Charcoal"), else the recipe's own...
- `static RecipesOf(EntityId station) -> IReadOnlyList<uint>` `0.2.6` — The recipes a station can run (its workshop's).
- `static ResolveInputs(uint recipe, IReadOnlyList<CategoryChoice>? choices = null) -> IReadOnlyList<StationStack>` `0.2.6` — A recipe's inputs as the game resolves them for a craft: the category choices applied (none: the first option of...
- `static Resume(EntityId station) -> Task<StationResult>` `0.2.6` — Clears a station's stop (its stop button again): it starts a new cycle with its selected recipe.
- `static Scanned { get; } -> bool` `0.2.6` — True once Kiln's first scan of the world's stations is done: `Stations.All` is complete from then on (a watch scans...
- `static Select(EntityId station, uint recipe, IReadOnlyList<CategoryChoice>? choices = null, bool keepStopped = false, bool allowLocked = false) -> Task<StationResult>` `0.2.6` — Selects the recipe a station repeats, with its category choices, as a player's choice in the station's screen does...
- `static SetOutputSource(EntityId station, bool on = true) -> bool` `0.2.9` — Opts a station in (or out) as crafting stock: its finished items (`StationInfo.Outputs`) become the last source of...
- `static Start(EntityId station, uint recipe) -> bool` — Tells the station to make a recipe, as a player's choice in its menu does.
- `static Status(EntityId station) -> StationStatus?` — What the station is doing.
- `static Stop(EntityId station, StopWhen when = StopWhen.Now) -> Task<StationResult>` `0.2.6` — Stops a station as its stop button does.
- `static WorkshopName(uint workshop) -> string?` `0.2.7` — A workshop's name for players (since 0.2.0-dev.29.15): the name of the item that places its station ("Charcoal...

## ToolBlock — What a tool does: what it is for, and how well.
- `BreaksInto { get; init; } -> ItemRef?` — What it becomes when its durability runs out, as the game's torch becomes a burnt one: a game item or an item of...
- `Durability { get; init; } -> double?` — Seconds a torch burns while it is held, the tooltip's Duration (the game's torch: 600).
- `Fishing { get; init; } -> FishingBlock?` — A fishing rod's pull and stamina.
- `Harvests { get; init; } -> ToolClass` — What it is for: an axe takes wood, a pickaxe stone and metal and digs, a rake flattens the ground, a glider flies, a...
- `Power { get; init; } -> int?` — How hard it bites: an axe's wood damage, a pickaxe's or a sieve's stone damage, a rake's flattening strength (the...
- `Range { get; init; } -> double?` — Metres: how far a rake flattens around where it aims (the game's: 3), how far a grappling hook reaches (13.5).
- `StaminaCost { get; init; } -> int?` — Stamina each use costs; the tooltip follows.
- `WearPerDebit { get; init; } -> int?` `0.2.3` — What each of the game's wear debits costs this tool's durability: 1 is the game's own, 0 makes it not wear, at most...

## ToolClass — A class of tool, with its starter and the number `ToolBlock.Power` scales from.
- `ToolClass(string Key, string Starter, int WoodDamage, int StoneDamage, int MetalDamage, int MainPower, int Slots, bool Digs)`
- `static readonly Axe -> ToolClass` — The game's axe: it harvests wood.
- `Digs { get; init; } -> bool` — True: it digs the ground, as a pickaxe does.
- `static readonly FishingRod -> ToolClass` — The game's simple fishing rod.
- `static readonly Glider -> ToolClass` — The game's glider, in its own slot.
- `static readonly GrapplingHook -> ToolClass` — The game's grappling hook, in its own slot.
- `Key { get; init; } -> string` — The name JSON gives this class.
- `MainPower { get; init; } -> int` — The number `ToolBlock.Power` scales from: the class's own main damage, or a rake's flattening strength.
- `MetalDamage { get; init; } -> int` — The metal damage of a hit at `ToolClass.MainPower`.
- `static readonly Pickaxe -> ToolClass` — The game's pickaxe: stone and metal, and it digs.
- `static readonly Rake -> ToolClass` — The game's rake: it flattens the ground where it aims, in a base.
- `static readonly Sieve -> ToolClass` — The game's mining sieve: stone, metal and wood, and it digs as deep as the game's sieve.
- `Slots { get; init; } -> int` — How many enhancement slots it has.
- `Starter { get; init; } -> string` — The game's tool it starts from.
- `StoneDamage { get; init; } -> int` — The stone damage of a hit at `ToolClass.MainPower`.
- `static readonly Torch -> ToolClass` — The game's torch: held, it lights and burns down.
- `Use { get; init; } -> ToolUse` — What it does.
- `WoodDamage { get; init; } -> int` — The wood damage of a hit at `ToolClass.MainPower`; `ToolBlock.Power` scales it, and the other two damages along.

## ToolUse — What a tool class does, which decides which `ToolBlock` fields it takes.
enum: Harvest, Flatten, Glide, Hook, Fish, Light

## VisualBlock — What an item looks like.
- `Animated { get; init; } -> AnimatedModel?` — A model with moving parts for the hand: a skinned model of the mod's own and the graph that plays its clip.
- `Dropped { get; init; } -> ItemRef?` — A game item, or an item of this mod, whose model this item shows lying on the ground.
- `Icon { get; init; } -> string?` — The icon alone: `item:<name>`, or a PNG in the mod's folder (then `clients: mod`).
- `Like { get; init; } -> ItemRef?` — A game item, or an item of this mod, whose whole look this item takes: held or worn model, dropped model, collection...
- `Model { get; init; } -> string?` — The model alone: `item:<name>`, or the GUID of a model your `assets/assets.json` serves (then `clients: mod`).
- `Tint { get; init; } -> string?` — `#rrggbb`: the colour of the parts the game lets you tint.
- `Vfx { get; init; } -> VfxRef?` — A VFX on the thing itself, for as long as it is there: in the hand while it is held (right hand by default, at the...

## WandAttack — One of a wand's attacks: how it is cast and what it sends.
- `Cast { get; init; } -> CastBlock` — The mana it costs and when it leaves the wand.
- `Delivery { get; init; } -> IDeliveryBlock` — How the attack gets there, and what it does when it does.
- `Moves { get; init; } -> WandMoves?` — The motion.

## WandBlock — A one-handed caster whose attacks are spells of its own: it needs no spell item, and never runs out of charges.
- `Class { get; init; } -> PartRef<WeaponClass>?` — How it is held: a class of weapon.
- `Description { get; init; } -> string?` — The wand's description in its tooltip.
- `Heavy { get; init; } -> WandAttack?` — The heavy attack.
- `Loot { get; init; } -> LootBlock?` — Where else it can be found.
- `Name { get; init; } -> string` — The wand's name, as players read it in its tooltip.
- `OnHit { get; init; } -> HitsRef?` — What every hit its wielder lands does to the target, while it is in hand, with no unlock.
- `OnKill { get; init; } -> HitsRef?` — What a hit that kills does to what it killed, while it is in hand.
- `Perks { get; init; } -> IReadOnlyList<PerkRef>?` — Enhancement slots, from the first.
- `Primary { get; init; } -> WandAttack` — The primary attack.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Source { get; init; } -> string?` — Where to get it, in the tooltip.
- `Special { get; init; } -> WandAttack?` — The special ability attack, once a player has the game's skill for it.
- `Stats { get; init; } -> PartRef<WeaponStats>?` — Its level, rarity and block.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — What it looks like.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## WandMoves — A wand's attack motion, with its timing.
- `WandMoves(string Key, string Sequence, double ReleaseSeconds, int[] Fires, int[] Silenced)`
- `Animation { get; init; } -> MoveAnimation?` — The state the motion plays, as `MoveBlock.Animation` takes it.
- `static readonly Bolt -> WandMoves` — The quick bolt.
- `Fires { get; init; } -> int[]` — The indexes of the sequence's spawn events that send this attack.
- `static readonly Heavy -> WandMoves` — The long wind-up.
- `HitAt { get; init; } -> double?` — Seconds into the motion the attack leaves the wand: the release, as `CastBlock.ReleaseAt` (give one of them, or the...
- `Key { get; init; } -> string` — The name JSON gives this motion (`"bolt"`, `"heavy"`, `"special"`).
- `ReleaseSeconds { get; init; } -> double` — Seconds into the motion where the attack leaves the wand, as the game's sequence has it.
- `Sequence { get; init; } -> string` — The game's own attack sequence it plays, by name.
- `Silenced { get; init; } -> int[]` — The indexes of the sequence's other spawn events, which are switched off so that only the attack goes out.
- `static readonly Special -> WandMoves` — The special ability's throw.

## WeaponBlock — A weapon that is exactly what its recipe tooltip shows (the crafted-item rule is built in).
- `Class { get; init; } -> PartRef<WeaponClass>` — How it is held and swung: `WeaponClass.Sword`, or your own.
- `Description { get; init; } -> string?` — The weapon's description in its tooltip.
- `Loot { get; init; } -> LootBlock?` — Where else it can be found.
- `Moves { get; init; } -> WeaponMoves?` — What its attack slots play: its own motions, timing and chains, slot by slot.
- `Name { get; init; } -> string` — The weapon's name, as players read it in its tooltip.
- `OnHit { get; init; } -> HitsRef?` — What every hit its wielder lands does to the target, while it is in hand, with no unlock.
- `OnKill { get; init; } -> HitsRef?` — What a hit that kills does to what it killed, while it is in hand.
- `Perks { get; init; } -> IReadOnlyList<PerkRef>?` — Enhancement slots, from the first.
- `Recipe { get; init; } -> RecipeBlock?` — How it is crafted.
- `Source { get; init; } -> string?` — Where to get it, in the tooltip.
- `Stats { get; init; } -> PartRef<WeaponStats>?` — Its level, power, rarity and damage.
- `Tweak { get; init; } -> FieldValues` — Game fields this block does not cover, set on the item it makes: `Tweak = { { GameFields.Items.MaxStackSize, 5 } }`.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — What it looks like.
- `Was { get; init; } -> string?` — The name this block had before a rename.

## WeaponClass — A class of weapon: how it is held and swung, which a look and stats can start from.
- `static readonly Axe -> WeaponClass` — A one-handed axe.
- `static readonly Bow -> WeaponClass` — A bow.
- `static readonly Daggers -> WeaponClass` — A pair of daggers, held in both hands.
- `static readonly Greataxe -> WeaponClass` — A two-handed greataxe.
- `static readonly Greatsword -> WeaponClass` — A two-handed greatsword.
- `static readonly Hammer -> WeaponClass` — A two-handed hammer.
- `Key { get; init; } -> string` — How JSON names a premade.
- `Like { get; init; } -> string?` — In JSON, the premade this one starts from: `{ "like": "sword", ... }`.
- `static readonly Mace -> WeaponClass` — A one-handed mace.
- `Perks { get; init; } -> IReadOnlyList<PerkRef>?` — The perks its weapons have unless they say otherwise.
- `static readonly Shield -> WeaponClass` — A shield.
- `static readonly Staff -> WeaponClass` — A staff, which casts spell items.
- `Stats { get; init; } -> PartRef<WeaponStats>?` — The stats its weapons have unless they say otherwise.
- `static readonly Sword -> WeaponClass` — A one-handed sword.
- `Visual { get; init; } -> PartRef<VisualBlock>?` — The look its weapons have unless they say otherwise.
- `static readonly Wand -> WeaponClass` — What `WandBlock` starts from.

## WeaponMoves — What a weapon's attack slots play, slot by slot.
- `Block { get; init; } -> MoveBlock?` — The block.
- `DodgeAttack { get; init; } -> MoveBlock?` — The attack out of a dodge.
- `Execution { get; init; } -> MoveBlock?` — The finishing blow on a staggered enemy.
- `Heavy { get; init; } -> MoveChain?` — The heavy attack on the ground, one move or a chain.
- `HeavyAir { get; init; } -> MoveBlock?` — The heavy attack in the air.
- `JumpAttack { get; init; } -> MoveBlock?` — The attack in a jump, before the fall.
- `Parry { get; init; } -> MoveBlock?` — The parry.
- `PlungeAttack { get; init; } -> MoveBlock?` — The heavy attack falling onto what is below.
- `Primary { get; init; } -> MoveChain?` — The light attack on the ground, one move or a chain.
- `SneakAttack { get; init; } -> MoveBlock?` — The attack from sneaking.
- `Special { get; init; } -> MoveBlock?` — The special ability, once a player has the game's skill for it.

## WeaponMovesApplicationChangedEvent `0.2.3` — A weapon's moves with follow-ups went somewhere: applied, refused, or removed with its mod.
- `After { get; } -> MoveApplicationResult` — What it answers now.
- `Before { get; } -> MoveApplicationResult` — What `Blocks.MovesStatus` answered before.
- `BlockName { get; } -> string` — The weapon block's name in that mod.
- `ModId { get; } -> string` — The mod whose weapon it is.

## WeaponMoveSlot `0.2.3` — An attack slot of a weapon, as `WeaponMoves` names them: where a `MoveFollowUpBlock` leads.
enum: Primary, Heavy, HeavyAir, DodgeAttack, JumpAttack, PlungeAttack, Block, Parry, Special, Execution, SneakAttack

## WeaponStats — What a weapon's tooltip says: level, power, rarity, damage kinds, block strength and stamina cost.
- `Block { get; init; } -> double?` — A shield's block strength, 0..1.
- `Damage { get; init; } -> DamageBlock?` — The kinds it deals.
- `Level { get; init; } -> int` — The tooltip's level; a made weapon has exactly this.
- `Power { get; init; } -> int?` — The tooltip's damage at that level.
- `Rarity { get; init; } -> Rarity` — The rarity: its colour, and how many enhancements it can have (see `WeaponStats.Rarity`).
- `StaminaCost { get; init; } -> int?` — Stamina each use costs; the tooltip follows.

## WornAttach — Where a rigid look sits: the game's slots on the player's skeleton, or `j<index>` for any joint.
- `const ArmLeft = "ArmL" -> string` — The left arm.
- `const Chest = "Chest" -> string` — The chest.
- `const FootLeft = "FootL" -> string` — The left foot.
- `const FootRight = "FootR" -> string` — The right foot.
- `const HandLeft = "HandPalmL" -> string` — The palm of the left hand.
- `const HandRight = "HandPalmR" -> string` — The palm of the right hand.
- `const Head = "Head" -> string` — The head.
- `const KneeLeft = "KneeL" -> string` — The left knee.
- `const KneeRight = "KneeR" -> string` — The right knee.
- `const Pelvis = "Pelvis" -> string` — The pelvis.

## WornLook — What something looks like on the body: a model of your own or a game item's, what it hides and how the rest of the character fits around it.
- `Attach { get; init; } -> string?` — A rigid model: a mesh with no skin, on this slot (`WornAttach`) or `j<index>`.
- `Female { get; init; } -> string?` — A second file for a female body, instead of a morph target.
- `Fits { get; init; } -> Fit?` — What it makes the rest of the character do: `Fit.Hood` gives hair its hood cut.
- `Hides { get; init; } -> IReadOnlyList<BodyPart>?` — What is not drawn while this is worn.
- `Icon { get; init; } -> string?` — The icon: a PNG in the mod's folder, or `item:<name>`.
- `Like { get; init; } -> ItemRef?` — A game item, or an item of this mod, whose whole look this takes: its models, what it hides and how it fits.
- `Lods { get; init; } -> IReadOnlyList<string>?` — Coarser files of `WornLook.Model`, from the next level on.
- `Materials { get; init; } -> IDictionary<string, string>` — What draws it: a glTF material's name to a material block's name or a game material's GUID, and `"*"` for every...
- `Model { get; init; } -> string?` — The model: a glTF in the mod's folder.
- `PixelSizes { get; init; } -> IReadOnlyList<double>?` — When each level shows: one per level, 0 first.
- `Variants { get; init; } -> IDictionary<Fit, string>` — Whole models for what the character wears or is: `[Fit.Shield] = "models/coat-shield.glb"`...
