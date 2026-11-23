# Blocks (Kiln.Blocks): affixes

## ActiveAffix `0.2.2` — One stat change Kiln holds on a player (or a lab's stand-in) for the affixes of what is worn or in hand.
- `ActiveAffix(string Affix, string Stat, double Value, EntityId Item)`
- `Affix { get; init; } -> string` — The affix: `"mod:name"`.
- `Item { get; init; } -> EntityId` — The item it comes from; `EntityId.None` for an affix that does not stack.
- `Stat { get; init; } -> string` — The stat's key.
- `Value { get; init; } -> double` — The value applied: one item's, or for an affix that does not stack the highest of the worn items'.

## AffixBlock `0.2.2` — One kind of rolled modifier on an item: "of the Bear, +3 to +8 Strength".
- `Group { get; init; } -> string?` — Affixes with the same group never roll onto one item together ("prefix", "element").
- `Name { get; init; } -> string` — What players read: "of the Bear", "Fleet".
- `Stacks { get; init; } -> bool` — What two worn items with this affix do together: their values add up (true, the default), or only the highest one...
- `Stat { get; init; } -> Stat` — The stat it changes while its item is worn or in hand: a `Stat` constant, or `new Stat("my-key", guid, Whole: true)`...
- `Tiers { get; init; } -> IReadOnlyList<AffixTier>` — Its tiers, the weakest first: tier 1 is `Tiers[0]`.

## AffixEntry `0.2.2` — One affix a pool can give.
- `Affix { get; init; } -> string` — An affix of this mod by its name, or `"other-mod:name"`.
- `Weight { get; init; } -> double` — How likely it is to come up, among the pool's.

## Affixes `0.2.2` — Affixes: rolled modifiers on one item ("+5 Strength", "+5% Move Speed"), kept with that item and applied to whoever wears or holds it.
- `static Active(EntityId wearer) -> IReadOnlyList<ActiveAffix>` — What Kiln holds on `wearer` right now for the affixes of what is worn and in hand.
- `static Add(ItemInstanceRef item, string affix, int tier = 1, double? value = null, Random? random = null) -> AffixResult` — Adds one affix to `item`.
- `static Clear(ItemInstanceRef item) -> bool` — Takes every affix off `item`.
- `const DataKey = "affixes" -> string` — The item data key of the rolls.
- `const DataMod = "kiln-affixes" -> string` — The owner the rolls are kept under as item data, and the mod id of their `InstanceDataSyncedEvent`.
- `static Define(string name, AffixBlock block) -> AffixHandle` — Defines an affix of the calling mod: the same as `Blocks.Define` (one registration, one handle, the id `"mod:name"`).
- `static DefinePool(string name, AffixPoolBlock block) -> AffixPoolHandle` — Defines a pool of the calling mod, which items roll which affixes: the same as `Blocks.Define`.
- `static Find(string affix) -> AffixHandle?` — A defined affix by `"mod:name"` (or a name of the calling mod); null when no loaded mod defines it.
- `static FindPool(string pool) -> AffixPoolHandle?` — A defined pool by `"mod:name"` (or a name of the calling mod); null when no loaded mod defines it.
- `static Load(string json) -> AffixFile` — Defines the affixes and pools of a data file, as the calling mod's blocks: the same shapes as the C# records, in...
- `const MaxPerItem = 8 -> int` — The most affixes one item carries.
- `static Of(ItemInstanceRef item) -> IReadOnlyList<RolledAffix>` — The affixes `item` carries (empty: none, or the ref is stale).
- `static Remove(ItemInstanceRef item, string affix) -> bool` — Takes one affix off `item`.
- `static Roll(ItemInstanceRef item, AffixPoolHandle pool, Random? random = null, bool replace = false) -> AffixResult` — Rolls `pool` onto `item`: between the pool's `Min` and `Max` different affixes, each with a tier its item level...
- `static Set(ItemInstanceRef item, IReadOnlyList<RolledAffix> affixes) -> AffixResult` — Gives `item` exactly these affixes (an empty list takes them all off): for tools and commands that edit an item.
- `static StatOf(EntityId entity, Stat stat) -> Task<double?>` — The value the game's attribute for `stat` holds on `entity` now, read by a small program through the game's own...
- `static Synced(EntityId item) -> IReadOnlyList<RolledAffix>?` — Client: the affixes of the item with the server's entity id `item`, as the server sent them to this game; null when...
- `static SyncedItems() -> IReadOnlyList<SyncedAffixes>` — Client: every item with affixes this game knows from the server: the player's own, and those of holders nearby.

## AffixesAppliedEvent `0.2.2` — What Kiln applies to a player for worn and held affixes changed: an item was put on or taken off, its affixes changed, or the player left (then...
- `Before { get; } -> IReadOnlyList<ActiveAffix>` — What was applied before.
- `Now { get; } -> IReadOnlyList<ActiveAffix>` — What is applied now.
- `Wearer { get; } -> EntityId` — The player.

## AffixesChangedEvent `0.2.2` — An item's affixes changed through `Affixes` (a roll, an add, a set, a removal).
- `Before { get; } -> IReadOnlyList<RolledAffix>` — Its affixes before the call.
- `Item { get; } -> ItemInstanceRef` — The item.
- `Now { get; } -> IReadOnlyList<RolledAffix>` — Its affixes now.

## AffixFile `0.2.2` — What `Affixes.Load` read from a data file.
- `AffixFile(IReadOnlyList<AffixHandle> Affixes, IReadOnlyList<AffixPoolHandle> Pools)`
- `Affixes { get; init; } -> IReadOnlyList<AffixHandle>` — The affixes the file defined.
- `Pools { get; init; } -> IReadOnlyList<AffixPoolHandle>` — The pools the file defined.

## AffixHandle `0.2.2` — What `Blocks.Define`, `Affixes.Define` and an `affixes` section of `blocks/*.json` give: one object, whichever route defined it (`Blocks.Find` and `Affixes.Find`...
- `Id { get; } -> string` — `"mod:name"`: what `RolledAffix.Affix` holds, the same whichever route defined the affix.

## AffixPoolBlock `0.2.2` — Which items roll which affixes, and how many.
- `Affixes { get; init; } -> IReadOnlyList<AffixEntry>` — What can come up.
- `Items { get; init; } -> IReadOnlyList<ItemRef>?` — The item types that roll from this pool: `GameItems` constants, or items of this mod by their block name.
- `Max { get; init; } -> int` — How many at most; at least `AffixPoolBlock.Min`, at most 8.
- `Min { get; init; } -> int` — How many different affixes a roll gives at least (0 to 8).

## AffixPoolHandle `0.2.2` — What `Blocks.Define`, `Affixes.DefinePool` and an `affixPools` section of `blocks/*.json` give: roll it onto an item.
- `Id { get; } -> string` — `"mod:name"`, the same whichever route defined the pool.
- `Roll(ItemInstanceRef item, Random? random = null, bool replace = false) -> AffixResult` — Rolls the pool onto `item`: `Affixes.Roll`.

## AffixResult `0.2.2` — The answer of a call that changes an item's affixes.
- `Affixes { get; } -> IReadOnlyList<RolledAffix>` — The item's affixes after the call (when not `AffixResult.Ok`: as they were).
- `Item { get; } -> ItemInstanceRef` — The item.
- `Ok { get; } -> bool` — True when the change was made.
- `Reason { get; } -> string?` — Why not; null when `AffixResult.Ok`.

## AffixTier `0.2.2` — One tier of an `AffixBlock`: the range a roll of this tier takes its value from.
- `Max { get; init; } -> double` — The highest value; at least `AffixTier.Min`, and of the same sign.
- `Min { get; init; } -> double` — The lowest value: whole points for a whole-number stat (`Stat.Strength`: 3), a share for the others...
- `MinItemLevel { get; init; } -> int` — The item level from which this tier can roll (0: any).
- `Step { get; init; } -> double?` — The grid a rolled value lies on.
- `Weight { get; init; } -> double` — How likely a roll of the affix takes this tier, among the tiers the item's level allows.

## RolledAffix `0.2.2` — One affix as an item carries it: which affix, which tier, and the rolled value.
- `RolledAffix(string Affix, string Name, string Stat, int Tier, double Value)`
- `Affix { get; init; } -> string` — The affix: `"mod:name"`.
- `Name { get; init; } -> string` — Its display name, as it was when rolled.
- `Stat { get; init; } -> string` — The stat's key (`"strength"`, `"move-speed"`, or the key of a mod's own `Stat`), as it was when rolled: the roll...
- `Text { get; } -> string` — The line a player reads: "+5 Strength (of the Bear)", "+5% Move Speed (Fleet)".
- `Tier { get; init; } -> int` — The tier, from 1.
- `Value { get; init; } -> double` — The value: whole points, or a share (0.05 is 5 %).
- `ValueText { get; } -> string` — The value alone: "+5", "-3", "+5%".

## SyncedAffixes `0.2.2` — An item with affixes as a player's game knows it from the server (`Affixes.SyncedItems`).
- `SyncedAffixes(EntityId Item, ItemId ItemType, EntityId Holder, int FlatSlot, bool Own, IReadOnlyList<RolledAffix> Affixes)`
- `Affixes { get; init; } -> IReadOnlyList<RolledAffix>` — Its affixes.
- `FlatSlot { get; init; } -> int` — The slot in the holder, as Kiln.Items numbers it.
- `Holder { get; init; } -> EntityId` — The server's id of who holds it: a player, a chest.
- `Item { get; init; } -> EntityId` — The item entity's id on the server.
- `ItemType { get; init; } -> ItemId` — The item type.
- `Own { get; init; } -> bool` — True when this game's player holds it.
