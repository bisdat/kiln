# TestKit (Kiln.TestKit)

## TestKit.BootRun `0.2.3` — One OnBoot run of the kit (`HarnessBootPhase.Run`).
- `BootRun(string ModId, BootModState State, string? Reason, IReadOnlyList<TestKit.BootRunDefinition> Staged, IReadOnlyList<string> Definitions, IReadOnlyList<BootIssue> Issues)`
- `Definitions { get; init; } -> IReadOnlyList<string>` — What lowered into the generation, as `kind/key` (a data file's block of the same name is left out).
- `Issues { get; init; } -> IReadOnlyList<BootIssue>` — Its issues: CallbackFailed, BootCapOff, ShadowedByFile, ...
- `ModId { get; init; } -> string` — The mod.
- `Ok { get; } -> bool` — True when the mod's boot content is in (Committed or Staged).
- `Reason { get; init; } -> string?` — Why it failed (its OnBoot threw, a definition does not lower, mods.boot is off); null when it did not.
- `Staged { get; init; } -> IReadOnlyList<TestKit.BootRunDefinition>` — What its OnBoot staged, in order: kind, key and the canonical JSON.
- `State { get; init; } -> BootModState` — Its outcome: `Committed` (or `Staged` without commit), else `ExcludedCallback`.

## TestKit.BootRunDefinition `0.2.3` — One definition an OnBoot staged.
- `BootRunDefinition(string Kind, string Key, string Json)`
- `Json { get; init; } -> string` — The canonical JSON Kiln lowers.
- `Key { get; init; } -> string` — Its name.
- `Kind { get; init; } -> string` — The plural kind, as in blocks/*.json (`statuses`, `items`, ...).

## TestKit.CapabilityRow `0.2.1` — One capability row as a test sees it.
- `CapabilityRow()`
- `CapabilityRow(string Name, bool Available, string ReasonText)`
- `Available { get; init; } -> bool` — Whether it is available.
- `Name { get; init; } -> string` — The row's name: `kiln.<service>` for a Kiln service, a game feature's name, or a mod's own.
- `ReasonText { get; init; } -> string` — Why not (or, when available, where it comes from).

## TestKit.Coverage `0.2.1` — What the kit makes real, what it fakes and what is unavailable offline: the data the docs' table is made from.
- `static All { get; } -> IReadOnlyList<TestKit.CoverageRow>` — Every row: the explicit ones in order, then one per Kiln service no row names, by name.

## TestKit.CoverageKind `0.2.1` — How the kit stands behind an area of Kiln's API.
enum: Real, RecordingFake, Unavailable

## TestKit.CoverageRow `0.2.1` — One row of `Coverage.All`.
- `CoverageRow(string Area, TestKit.CoverageKind Kind, IReadOnlyList<string> Capabilities, string Note)`
- `Area { get; init; } -> string` — What a mod author would look for ("Events", "kiln.combat").
- `Capabilities { get; init; } -> IReadOnlyList<string>` — The capability names of the area (empty when it has none: Kiln.Api's own parts).
- `Kind { get; init; } -> TestKit.CoverageKind` — Real, a recording fake, or unavailable offline.
- `Note { get; init; } -> string` — One line: what holds and what does not.

## TestKit.FakeBlobRegistration `0.2.3` — A content blob a mod registered with the kit's kiln.assets (`Assets.AddBlob`, `OverrideBlob`).
- `FakeBlobRegistration()`
- `FakeBlobRegistration(string Hash, ulong Owner, bool Override)`
- `Hash { get; init; } -> string` — Its content hash as `ContentHash.ToString()` writes it (`size-h0-h1-h2`, hex).
- `Override { get; init; } -> bool` — True for an override of a hash, false for an added blob.
- `Owner { get; init; } -> ulong` — The native owner it was registered under (the mod's).

## TestKit.FakeChatLine `0.2.2` — A line in a fake player's chat (`FakePlayer.Chat`), or a server line a mod sent (`HarnessChat.Sent`).
- `ModId { get; } -> string?` — The mod that sent a server line; null for a player's typed line and for Kiln's own lines.
- `Speaker { get; } -> string?` — Who speaks it: a player's name for a typed line or `Chat.Say`; null for a notice (`Chat.Tell`, `Chat.Broadcast`...
- `Step { get; } -> ulong` — The harness step during which it was sent.
- `Text { get; } -> string` — The line as the game's chat shows it: a notice's text alone (Kiln's server lines start with `Chat.ServerPrefix`), a...
- `To { get; } -> TestKit.FakePlayer?` — The one player the line was for; null when it went to every player.

## TestKit.FakeChatMessage `0.2.2` — A line a fake player typed (`FakePlayer.Say`), as the chat's receive loop handed it over.
- `Consumed { get; } -> bool` — A handler or a command took it (`ChatMessage.Cancel`, a command line): nobody was shown it.
- `Player { get; } -> TestKit.FakePlayer?` — The sender; null when the line came from a connection with no player in game.
- `Step { get; } -> ulong` — The step whose start delivered it (the step after it was typed).
- `Text { get; } -> string` — The text as typed.

## TestKit.FakeControlCall `0.2.3` — One call a mod made to the fake kiln.creature_control.
- `FakeControlCall(ulong Step, string Function, ulong Owner, EntityId Entity, ulong Lease, int Result, string Detail)`
- `Detail { get; init; } -> string` — The lease state or reason it answered, in words.
- `Entity { get; init; } -> EntityId` — The body's entity (0 when none was named).
- `Function { get; init; } -> string` — The table function (`bind_body`, `acquire_control`, `release_control`, `release_body`, `next_request`, `quarantine`...
- `Lease { get; init; } -> ulong` — The lease (0 when none).
- `Owner { get; init; } -> ulong` — The caller's native owner.
- `Result { get; init; } -> int` — The KilnResult it answered.
- `Step { get; init; } -> ulong` — The harness step.

## TestKit.FakeCraftSourceRegistration `0.2.15` — A mod's live source registration in the kit.
- `FakeCraftSourceRegistration(ulong Owner, EntityId Player, EntityId Container, CraftSourceOrder Order, CraftSourceFilter Filter, IReadOnlyList<ItemId> Items)`
- `Container { get; init; } -> EntityId` — The storage container.
- `Filter { get; init; } -> CraftSourceFilter` — The item-set interpretation.
- `Items { get; init; } -> IReadOnlyList<ItemId>` — A copy of the filter items.
- `Order { get; init; } -> CraftSourceOrder` — Its position relative to station outputs.
- `Owner { get; init; } -> ulong` — The registering native owner.
- `Player { get; init; } -> EntityId` — The only player it supplies.

## TestKit.FakeEcall `0.2.4` — One ECALL of a game program: the native it calls and the call-site id.
- `FakeEcall()`
- `FakeEcall(string Native, uint Site)`
- `Native { get; init; } -> string` — The native's node name (`"applyDamage"`) or "0x" and its hash.
- `Site { get; init; } -> uint` — The ECALL's call-site id.

## TestKit.FakeGameNativeCall `0.2.4` — The call a game native answers in the fake (valid only while its handler runs).
- `ArgCount { get; } -> int` — The values the ECALL pops.
- `Bytes(int index) -> Span<byte>` — Argument `index`'s variable (empty for a literal or one not connected).
- `Get<T>(int index) -> T` — Argument `index`'s value.
- `Program { get; } -> uint` — The calling program.
- `Set<T>(int index, T value) -> bool` — Writes argument `index`'s variable (false for a literal or a smaller variable).
- `Site { get; } -> uint` — The call site.

## TestKit.FakeGameNativeHandler `0.2.4` — What a game native does when it runs in `HarnessImpactNatives.Call` (`HarnessImpactNatives.SetGameNative`): it may read and change its arguments' variables, and...
- `Invoke(TestKit.FakeGameNativeCall call) -> byte`

## `TestKit.FakeGameWriter<T>` `0.2.2` — What a program of the game does to a component every step (`FakeWorld.GameWrites`).
- `Invoke(EntityId entity, ref T value) -> void`

## TestKit.FakeHealth `0.2.2` — An entity's health in the fake kiln.combat (`HarnessCombat.Health`).
- `Current { get; } -> int` — Health points now.
- `Dead { get; } -> bool` — Dead: its health fell below 1 (and it was not healed or respawned since).
- `Max { get; } -> int` — The maximum.

## TestKit.FakeHit `0.2.2` — One hit, from a mod (`Combat.Damage`, `Kill`) or from the game (`HarnessCombat.Hit`), as it went (`HarnessCombat.Hits`).
- `Amount { get; } -> int` — The damage (negative: a healing hit), as the handlers left it.
- `Attacker { get; } -> EntityId` — The attacker (none when unknown).
- `FromGame { get; } -> bool` — A hit of the game (the test's `HarnessCombat.Hit`); false for a mod's.
- `Health { get; } -> (int Before, int After)` — The target's health before and after, once applied (-1 before).
- `Id { get; } -> ulong` — kiln.combat's id of the hit.
- `KillingBlow { get; } -> bool` — It took the target from above 0 to 0.
- `ModId { get; } -> string?` — The mod that asked; null for the game's hits (and a call outside any mod).
- `State { get; } -> TestKit.FakeHitState` — Where it is.
- `Step { get; } -> ulong` — The step during which it was dealt or asked for.
- `Target { get; } -> EntityId` — The target.

## TestKit.FakeHitState `0.2.2` — Where a hit is (`FakeHit.State`), as kiln.combat reports it.
enum: Queued, Held, Applied, Dropped

## TestKit.FakeImpactArg `0.2.4` — One value an ECALL pops (`FakeImpactCall`): a variable of the instance of an impact type, a raw literal, or nothing connected.
- `static Bool(bool value) -> TestKit.FakeImpactArg` — A bool variable (one byte).
- `static Entity(EntityId value) -> TestKit.FakeImpactArg` — An ecs.EntityId variable.
- `static Float(float value) -> TestKit.FakeImpactArg` — A float32 variable.
- `static Int(int value) -> TestKit.FakeImpactArg` — An sint32 variable.
- `static Literal(uint value) -> TestKit.FakeImpactArg` — A raw value the program pushes (a literal, not a variable).
- `static NotConnected { get; } -> TestKit.FakeImpactArg` — An argument the program leaves not connected (0).
- `static Time(TimeSpan value) -> TestKit.FakeImpactArg` — A Time variable (int64 nanoseconds).
- `static UInt(uint value) -> TestKit.FakeImpactArg` — A uint32 variable.
- `static Variable(string type, ReadOnlySpan<byte> bytes) -> TestKit.FakeImpactArg` — A variable of impact type `type` holding `bytes`.
- `static Variable<T>(string type, T value) -> TestKit.FakeImpactArg` — A variable of impact type `type` (`"sint32"`, `"impact.DamageSetup"`, ...) holding `value`'s bytes.
- `static Vector(Vector3 value) -> TestKit.FakeImpactArg` — A float3 variable.

## TestKit.FakeImpactCall `0.2.4` — One ECALL for `HarnessImpactNatives.Call`.
- `FakeImpactCall(string native, params TestKit.FakeImpactArg[] args)`
- `Args { get; } -> IReadOnlyList<TestKit.FakeImpactArg>` — The values it pops.
- `ImpactId { get; init; } -> uint` — The instance's id.
- `Native { get; } -> string` — The native.
- `Program { get; init; } -> uint` — The calling program's id (FNV-1a-32 of its GUID).
- `Self { get; init; } -> EntityId` — The instance's own entity.
- `Site { get; init; } -> uint` — The ECALL's call-site id.

## TestKit.FakeImpactCallRecord `0.2.4` — One ECALL the test made (`HarnessImpactNatives.Call`).
- `FakeImpactCallRecord(ulong Step, string Native, uint Hash, uint Program, uint Site, byte Error, byte Value, int Handlers)`
- `Error { get; init; } -> byte` — The ECALL's error (0 ok).
- `Handlers { get; init; } -> int` — The handlers that ran (a sync native's, the attachments').
- `Hash { get; init; } -> uint` — The native's hash.
- `Native { get; init; } -> string` — The native's name (a Kiln native's full name, a game native's node name) or its hash.
- `Program { get; init; } -> uint` — The calling program's id.
- `Site { get; init; } -> uint` — The call site.
- `Step { get; init; } -> ulong` — The harness step.
- `Value { get; init; } -> byte` — What it pushed.

## TestKit.FakeImpactResult `0.2.4` — What an ECALL answered (`HarnessImpactNatives.Call`).
- `Arg<T>(int index) -> T` — Argument `index`'s value after the call.
- `Bytes(int index) -> ReadOnlySpan<byte>` — Argument `index`'s variable after the call (empty for a literal or one not connected).
- `Error { get; } -> byte` — The ECALL's error: 0 ok; 0x11 the handler threw; 0x18 a required argument not connected; 0x1F a native the registry...
- `Handlers { get; } -> int` — The handlers that ran (a Kiln native's; the attachments that entered).
- `OriginalCalled { get; } -> bool` — The game's native ran.
- `Value { get; } -> byte` — What the ECALL pushed (when `FakeImpactResult.Error` is 0).

## TestKit.FakeInventory `0.2.2` — The inventory of a fake player (`FakePlayer.Inventory`) or of a container (`HarnessItems.AddContainer`), as the fake kiln.items keeps it.
- `Clear() -> void` — Empties every slot, as if the game had (gear's item entities go too).
- `Count(ItemId item) -> int` — How many of `item` the inventory holds.
- `Count(string name) -> int` — How many of the item named `name` (as `HarnessItems.AddItem` named it) it holds.
- `Owner { get; } -> EntityId` — The entity whose inventory it is.
- `Put(ItemId item, int count = 1) -> int` — Puts `count` x `item` in, as if the game had (stacks first, then empty slots; no transaction, no event).
- `Slots { get; } -> IReadOnlyList<TestKit.FakeSlot>` — Every slot, empty ones included, in the order `Items.Read` lists them.

## TestKit.FakeInventorySlot `0.2.12` — A slot as the game would draw it: the item, the stack, the item's entity (the server's id of it: what `SlotClick.ItemEntity` carries), the inventory that holds it...
- `FakeInventorySlot()`
- `FakeInventorySlot(uint Item, uint Count = 1, uint ItemEntity = 0, uint Holder = 0, uint InventoryPage = 0, byte SlotIndex = 0)`
- `Count { get; init; } -> uint` — The stack size (default 1).
- `Deconstruct(out uint Item, out uint Count, out uint ItemEntity, out uint Holder, out uint InventoryPage, out byte SlotIndex) -> void`
- `Equals(TestKit.FakeInventorySlot other) -> bool`
- `Holder { get; init; } -> uint` — The entity of the inventory that holds the item (0: none).
- `InventoryPage { get; init; } -> uint` — The inventory page the slot is on (an entity id; 0: none).
- `Item { get; init; } -> uint` — The item's id.
- `ItemEntity { get; init; } -> uint` — The server's entity id of the item: what `SlotClick.ItemEntity` carries (0: none).
- `Level { get; init; } -> int` `0.2.14` — The item's level as drawn.
- `static operator !=(TestKit.FakeInventorySlot left, TestKit.FakeInventorySlot right) -> bool`
- `static operator ==(TestKit.FakeInventorySlot left, TestKit.FakeInventorySlot right) -> bool`
- `Rarity { get; init; } -> ItemRarity?` `0.2.14` — The item's rarity as drawn (null: none).
- `SlotIndex { get; init; } -> byte` — The slot's index on its page.

## TestKit.FakeItemsCall `0.2.2` — One give or take a mod asked for (`HarnessItems.Calls`), as it ran.
- `Done { get; } -> int` — How many were given or taken (a dry run: would be).
- `DryRun { get; } -> bool` — A dry run: nothing changed.
- `Entity { get; } -> EntityId` — Whose inventory.
- `IsTake { get; } -> bool` — A take; false for a give.
- `Item { get; } -> ItemId` — The item.
- `ModId { get; } -> string?` — The mod that asked; null for a call outside any mod.
- `Requested { get; } -> int` — How many were asked for.
- `Status { get; } -> ItemsStatus` — The result's status.
- `Step { get; } -> ulong` — The step whose start ran the transaction (the step after the request).

## TestKit.FakeOutline `0.2.13` — An outline request as the kit's kiln.render holds it (`HarnessOutlines`).
- `FakeOutline(EntityId Entity, Color Color, bool Pulse, int Priority, TimeSpan? Duration, OutlineState State, OutlineReason Reason, int Group, bool ColorShared, string Owner)`
- `Color { get; init; } -> Color` — The colour asked for.
- `ColorShared { get; init; } -> bool` — A third colour: it shows with the nearest other colour's group.
- `Duration { get; init; } -> TimeSpan?` — How long it lasts (null: until removed).
- `Entity { get; init; } -> EntityId` — The outlined entity.
- `Group { get; init; } -> int` — The game's highlight group its colour uses (4 or 5) while shown, 0 otherwise.
- `Owner { get; init; } -> string` — Who asked: the mod's id, or `kiln`.
- `Priority { get; init; } -> int` — The request's priority.
- `Pulse { get; init; } -> bool` — The colour breathes.
- `Reason { get; init; } -> OutlineReason` — Why it waits or ended.
- `State { get; init; } -> OutlineState` — Its state now.
- `ThroughWalls { get; init; } -> bool` `0.2.14` — Asked with `OutlineOptions.ThroughWalls` (Kiln's silhouette through walls; its drawing is not modelled).

## TestKit.FakeOutlineSetting `0.2.13` — The player's kiln.json `"ui": {"outlines": ...}` in the kit (`HarnessOutlines.Setting`).
enum: GameFirst, ModsFirst, Off

## TestKit.FakePathAnswer `0.2.3` — What the fake's path search answers: `FakePathAnswer.Route` (found when its last point is at the request's target, partial otherwise), or one of the game's results...
- `static IterationLimit { get; } -> TestKit.FakePathAnswer` — The search hit its iteration limit.
- `static NoPath { get; } -> TestKit.FakePathAnswer` — The game found no path.
- `static NoStartNode { get; } -> TestKit.FakePathAnswer` — The start has no node.
- `static NoTargetNode { get; } -> TestKit.FakePathAnswer` — The target has no node.
- `static NotLoaded { get; } -> TestKit.FakePathAnswer` — The game gave no answer (the area is not loaded).
- `static Route(params WorldPosition[] points) -> TestKit.FakePathAnswer` — A walking route through these points.
- `static RouteWithMove(byte move, params WorldPosition[] points) -> TestKit.FakePathAnswer` — A route with a step that is not walking (`move`: the game's move kind, 2 = jump up): the service reports it as...
- `static TimedOut { get; } -> TestKit.FakePathAnswer` — The request ran out of time.

## TestKit.FakePathRequest `0.2.3` — A path request a mod made (`Queries.RequestPath`, `FindPath`, `CanReach`).
- `FakePathRequest(EntityId Anchor, WorldPosition From, WorldPosition To, double HorizontalSpace, double VerticalSpace, uint MaxIterations, bool ClosestFallback)`
- `Anchor { get; init; } -> EntityId` — The entity the game's path query was made for.
- `ClosestFallback { get; init; } -> bool` — Whether the closest node may be used when the target has none.
- `From { get; init; } -> WorldPosition` — Where the route starts.
- `HorizontalSpace { get; init; } -> double` — The free space the route needs sideways, in metres.
- `MaxIterations { get; init; } -> uint` — The search's iteration limit.
- `To { get; init; } -> WorldPosition` — Where the route should end.
- `VerticalSpace { get; init; } -> double` — The free space the route needs upwards, in metres.

## TestKit.FakePlayer `0.2.1` — A player the test joined (`HarnessPlayers.Join`).
- `Chat { get; } -> IReadOnlyList<TestKit.FakeChatLine>` `0.2.2` — What this player's chat showed, in order: server lines mods sent them (`Chat.Tell`, `Chat.Broadcast`, command...
- `Entity { get; } -> EntityId` — The player's entity: slot + 1, as on this game build.
- `InGame { get; } -> bool` — In game: from the step after the join until the step after the leave.
- `Inventory { get; } -> TestKit.FakeInventory` `0.2.2` — This player's inventory in the fake kiln.items (`HarnessItems`): made at its first use while the player is in game...
- `IsAdmin { get; } -> bool` — The game's admin bit.
- `Leave() -> bool` — The player disconnects.
- `Name { get; } -> string` — The platform name.
- `Position { get; set; } -> WorldPosition?` — Where the player stands.
- `Rights { get; } -> PlayerPermissions` — The user group's rights.
- `Say(string text) -> void` `0.2.2` — The player types `text` in chat (a command line starts with `Commands.Prefix`).
- `Slot { get; } -> int` — The game's player slot (0..15), the lowest one free at the join.
- `SteamId { get; } -> ulong` — SteamID64 (the one given to `HarnessPlayers.Join`, or a made-up one).
- `UserGroupId { get; } -> uint` — The game's raw user-group id (0: unknown).

## TestKit.FakeProgramLists `0.2.4` — A fake program's descriptor lists: the game's component names (`"Fuel"`), singletons with their namespace.
- `AnyOf { get; init; } -> IReadOnlyList<string>` — any_of.
- `EventsIn { get; init; } -> IReadOnlyList<string>` — events_in.
- `EventsOut { get; init; } -> IReadOnlyList<string>` — events_out.
- `Flags { get; init; } -> uint` — The descriptor's flags (0x1: a client program, which has no update in a server world).
- `Interval { get; init; } -> uint` — The descriptor's interval.
- `OtherRead { get; init; } -> IReadOnlyList<string>` — other_read.
- `OtherTemplate { get; init; } -> IReadOnlyList<string>` — other_tpl.
- `OtherWrite { get; init; } -> IReadOnlyList<string>` — other_write.
- `Read { get; init; } -> IReadOnlyList<string>` — read.
- `ReadOptional { get; init; } -> IReadOnlyList<string>` — read_opt.
- `SingletonsRead { get; init; } -> IReadOnlyList<string>` — sys_read (singletons; the fake has none: their slots are empty).
- `SingletonsWrite { get; init; } -> IReadOnlyList<string>` — sys_write.
- `Template { get; init; } -> IReadOnlyList<string>` — tpl.
- `TemplateOptional { get; init; } -> IReadOnlyList<string>` — tpl_opt.
- `With { get; init; } -> IReadOnlyList<string>` — with.
- `Without { get; init; } -> IReadOnlyList<string>` — without.
- `Write { get; init; } -> IReadOnlyList<string>` — write.
- `WriteOptional { get; init; } -> IReadOnlyList<string>` — write_opt.

## TestKit.FakeProgramMark `0.2.4` — A write a wrap's stage marked for the save and replication (the game's MarkWritten).
- `FakeProgramMark()`
- `FakeProgramMark(string Program, EntityId Entity, string Component, ulong Step)`
- `Component { get; init; } -> string` — The component's game name.
- `Entity { get; init; } -> EntityId` — The entity.
- `Program { get; init; } -> string` — The wrapped program.
- `Step { get; init; } -> ulong` — The world's step.

## TestKit.FakeProgramSlice `0.2.4` — What a fake program's update sees: one slice of its program (one archetype's matching entities).
- `FakeProgramSlice(string Program, IReadOnlyList<EntityId> Entities, ulong Step)`
- `Entities { get; init; } -> IReadOnlyList<EntityId>` — The slice's entities, in row order.
- `Program { get; init; } -> string` — The program.
- `Step { get; init; } -> ulong` — The world's step.

## TestKit.FakeSlot `0.2.2` — One slot of a fake inventory (`FakeInventory.Slots`).
- `Count { get; } -> int` — How many.
- `IsEmpty { get; } -> bool` — The slot is empty.
- `Item { get; } -> ItemId` — The item; none when the slot is empty.
- `ItemEntity { get; } -> EntityId` — Gear: its item entity in the fake world; none for a stack.
- `Level { get; } -> int` — Gear: its level; 0 for a stack.
- `Rarity { get; } -> ItemRarity?` — Gear: its rarity; null for a stack and an empty slot.
- `Ref { get; } -> ItemInstanceRef` `0.2.12` — Gear: the item as one particular item, with the stamp the kit's kiln.items gave its item entity (what...
- `Slot { get; } -> SlotId` — The slot (its 8-slot inventory's entity and index), as `Items.Read` reports it.

## TestKit.FakeSlotClickResult `0.2.12` — What a `HarnessUiInventory.Click` did: how many handlers got it, and whether the game would not have seen the press (a handler asked to take it).
- `FakeSlotClickResult()`
- `FakeSlotClickResult(int Delivered, bool Taken)`
- `Delivered { get; init; } -> int` — How many handlers got the click.
- `Taken { get; init; } -> bool` — True when a handler asked to take the press, so the game would not have seen it.

## TestKit.FakeSpawnCall `0.2.2` — One call a mod made to kiln.spawn that changes the world (`HarnessSpawn.Calls`), with its answer.
- `Code { get; } -> int` — The native answer: 0 ok, 1 invalid argument, 2 not found (an unknown template, no such entity), 3 unavailable.
- `Entity { get; } -> EntityId` — The new or destroyed entity; none for a refused spawn and a cleanup.
- `Kind { get; } -> TestKit.FakeSpawnKind` — What was asked.
- `ModId { get; } -> string?` — The mod that asked; null for a call outside any mod.
- `Position { get; } -> WorldPosition?` — Where a spawn was asked for; null for a destroy or a cleanup.
- `Step { get; } -> ulong` — The harness step during which the call ran.
- `Subject { get; } -> string` — The template's name or GUID text as the mod gave it, `item 0x... xN`, `entity N`, or the cleanup's filter.
- `Tag { get; } -> string` — The spawn's tag (an item drop's is "item" by default), the destroyed entity's, or the cleanup's filter.

## TestKit.FakeSpawned `0.2.2` — An entity Kiln.Spawn put into the fake world (`HarnessSpawn.All`).
- `Amount { get; } -> int` — An item drop's amount; 0 for a template spawn.
- `Entity { get; } -> EntityId` — The entity.
- `Flags { get; } -> SpawnFlags` — The spawn's flags.
- `ItemId { get; } -> uint` — An item drop's item; 0 for a template spawn.
- `Kind { get; } -> TemplateKind` — The template's kind.
- `Level { get; } -> int` — The level asked for (0: the template's own).
- `Origin { get; } -> string` — The mod that spawned it ("" outside any mod).
- `Position { get; } -> WorldPosition?` — Where it stands now (its `CurrentTransform` in the fake world); null when it is gone.
- `Serial { get; } -> ulong` — Kiln's tracked-record serial; 0 for a spawn with `SpawnFlags.NoTrack`.
- `SpawnPosition { get; } -> WorldPosition` — Where it was spawned.
- `Step { get; } -> ulong` — The harness step during which it was spawned.
- `Tag { get; } -> string` — The spawn's tag ("" when none; "item" for an item drop without one).
- `Template { get; } -> string` — The template's name (`HarnessSpawn.PickupTemplate` for an item drop).
- `TemplateId { get; } -> Guid` — The template's GUID.
- `YawDegrees { get; } -> float` — The rotation about the up axis it was spawned with.

## TestKit.FakeSpawnKind `0.2.2` — What a spawn call was (`FakeSpawnCall.Kind`).
enum: Entity, Item, Destroy, Cleanup

## TestKit.FakeTerrainWrite `0.2.3` — One write an overlay made (a batch, or a single block when a base was in the way).
- `FakeTerrainWrite()`
- `FakeTerrainWrite(string Mod, string Label, CellBox Box, long CellsChanged, bool Transient, ulong Step)`
- `Box { get; init; } -> CellBox` — The cells the write covered.
- `CellsChanged { get; init; } -> long` — How many cells the write changed.
- `Label { get; init; } -> string` — "terrain \<plan> v\<version>".
- `Mod { get; init; } -> string` — The mod that applied the patch ("kiln" from code that is not a mod's).
- `Step { get; init; } -> ulong` — The harness step it was made in.
- `Transient { get; init; } -> bool` — The write is transient: `HarnessTerrain.Restart` puts the cells it changed back.

## TestKit.FakeTimeCall `0.2.2` — A change to the clock a mod made (`HarnessTime.Calls`).
- `ModId { get; } -> string?` — The mod that asked; null for a call outside any mod.
- `Step { get; } -> ulong` — The step during which it ran.
- `Value { get; } -> double` — The time of day set (0..24), or the speed held (1 when released).
- `What { get; } -> string` — `set` (forward), `set game` (the game's setTimeOfDay), `speed` or `speed released`.

## TestKit.FakeUiBar `0.2.3` — A drawn value: the fraction, the numbers and the label (null: the game's own).
- `FakeUiBar()`
- `FakeUiBar(float Fraction, uint Current, uint Max, string? Label = null)`
- `Current { get; init; } -> uint` — The current number.
- `Deconstruct(out float Fraction, out uint Current, out uint Max, out string? Label) -> void`
- `Equals(TestKit.FakeUiBar other) -> bool`
- `Fraction { get; init; } -> float` — How full the bar is, as a fraction.
- `Label { get; init; } -> string?` — The label text; null leaves the game's own.
- `Max { get; init; } -> uint` — The maximum number.
- `static operator !=(TestKit.FakeUiBar left, TestKit.FakeUiBar right) -> bool`
- `static operator ==(TestKit.FakeUiBar left, TestKit.FakeUiBar right) -> bool`

## TestKit.FakeUiDecision `0.2.3` — What one provider did with a value: `Result` is its native result (0 ok), `Edit` the fields it committed (KilnUiEditFlags), `Invalid` when the fake refused the edit...
- `FakeUiDecision()`
- `FakeUiDecision(uint Kind, int Result, uint Edit, bool Invalid)`
- `Edit { get; init; } -> uint` — The fields the provider committed, as the native edit flags (0 when it changed nothing or its edit was refused).
- `Invalid { get; init; } -> bool` — True when the fake refused the provider's edit as the game would, counting it as a fault.
- `Kind { get; init; } -> uint` — The kind of value the provider was asked about (the numbering of `UiValueKind`: detail, slot, held, health, stamina...
- `Result { get; init; } -> int` — The provider's native result; 0 is ok.

## TestKit.FakeUiItem `0.2.3` — An item as the game's UI would show it to a provider (the fields of `UiItemView` a test sets).
- `FakeUiItem()`
- `FakeUiItem(uint Item, uint Fuel = 0, uint FuelMax = 0, uint Count = 1, uint InventoryPage = 0, byte SlotIndex = 0, uint ClientEntity = 0, uint Flags = 0, uint UnitsPerCharge = 1)`
- `ClientEntity { get; init; } -> uint` — The stack's client-world entity; valid with flag 1.
- `Count { get; init; } -> uint` — The stack size (default 1).
- `Deconstruct(out uint Item, out uint Fuel, out uint FuelMax, out uint Count, out uint InventoryPage, out byte SlotIndex, out uint ClientEntity, out uint Flags, out uint UnitsPerCharge) -> void`
- `Equals(TestKit.FakeUiItem other) -> bool`
- `Flags { get; init; } -> uint` — Which identity fields are valid: 1 client entity, 4 inventory slot, 8 detail copy, 16 synthetic.
- `Fuel { get; init; } -> uint` — The item's fuel, in raw fuel units.
- `FuelMax { get; init; } -> uint` — The item's largest fuel, in raw fuel units.
- `InventoryPage { get; init; } -> uint` — The inventory page of the slot (an entity id); valid with flag 4.
- `Item { get; init; } -> uint` — The item's id.
- `static operator !=(TestKit.FakeUiItem left, TestKit.FakeUiItem right) -> bool`
- `static operator ==(TestKit.FakeUiItem left, TestKit.FakeUiItem right) -> bool`
- `SlotIndex { get; init; } -> byte` — The slot's index on its page; valid with flag 4.
- `UnitsPerCharge { get; init; } -> uint` — The detail view's divisor for fuel (at least 1; default 1).

## TestKit.FakeVoxelAnswer `0.2.3` — What the fake answers a voxel ray: `FakeVoxelAnswer.Clear`, `FakeVoxelAnswer.Hit`, or `FakeVoxelAnswer.Refused` with one of the service's reasons (`NotLoaded`...
- `static Clear { get; } -> TestKit.FakeVoxelAnswer` — Loaded, and nothing on the segment.
- `Equals(TestKit.FakeVoxelAnswer other) -> bool`
- `static Hit(WorldPosition at, uint material = 1, ushort voxel = 65281) -> TestKit.FakeVoxelAnswer` — The first voxel surface is at `at`.
- `static operator !=(TestKit.FakeVoxelAnswer left, TestKit.FakeVoxelAnswer right) -> bool`
- `static operator ==(TestKit.FakeVoxelAnswer left, TestKit.FakeVoxelAnswer right) -> bool`
- `static Refused(string reason) -> TestKit.FakeVoxelAnswer` — Nothing is known, for `reason`: `NotLoaded`, `OutOfGrid`.

## TestKit.FakeVoxelRay `0.2.3` — A voxel ray a mod asked for (`Queries.RaycastVoxels`, `GroundBelow`, `VoxelLineOfSight`, a brain's sight).
- `FakeVoxelRay()`
- `FakeVoxelRay(WorldPosition From, WorldPosition To, byte Realm)`
- `From { get; init; } -> WorldPosition` — Where the ray starts.
- `Realm { get; init; } -> byte` — The realm the ray was cast in (0: the overworld).
- `To { get; init; } -> WorldPosition` — Where the ray ends.

## TestKit.FakeWorld `0.2.2` — The fake world of a harness (`KilnHarness.Entities`): entities with components laid out by Kiln.Game's types, under the real Kiln.World.
- `All { get; } -> IReadOnlyList<EntityId>` — Every entity made here (and every fake player's entity that was given a component), by id.
- `Create() -> EntityId` — Makes an entity with no component and no position.
- `Create(WorldPosition position) -> EntityId` — Makes an entity that stands at `position`: its `CurrentTransform` (identity rotation, scale 1), which...
- `Destroy(EntityId entity) -> bool` — Takes an entity and its components out of the world.
- `Exists(EntityId entity) -> bool` — Whether the entity is in the world: made here, or a fake player's.
- `GameWrites<T>(string program, TestKit.FakeGameWriter<T>? each = null) -> IDisposable` — A program of the game that writes `T`: from now on `World.WriteInfo` and every tracked write of the type name it...
- `Get<T>(EntityId entity) -> T` — The entity's component.
- `GetBytes(EntityId entity, string component) -> byte[]?` — A copy of the component's bytes; null when the entity has none.
- `Remove<T>(EntityId entity) -> bool` — Takes component `T` off an entity.
- `Set<T>(EntityId entity, T value) -> TestKit.FakeWorld` — Puts component `T` on an entity, or replaces its value, as if the game had: no write tracking, no event, at once.
- `SetBytes(EntityId entity, string component, ReadOnlySpan<byte> bytes) -> TestKit.FakeWorld` — Puts a component on an entity by its game name, as bytes: for a component no generated type names.
- `SetPosition(EntityId entity, WorldPosition position) -> void` — Moves an entity (or gives it its first position): the position in its `CurrentTransform`.
- `Track(string component, bool save, bool replication) -> void` — Says what the game's write tracking does with a component type, which the kit cannot know: a tracked write of it...
- `TryGet<T>(EntityId entity, out T value) -> bool` — The entity's component; false when it has none (or one of another size).
- `Writes { get; } -> IReadOnlyList<TestKit.FakeWorldWrite>` — Every tracked write a mod asked for that reached the world, in the order they ran, with what became of each (a write...

## TestKit.FakeWorldWrite `0.2.2` — One tracked write a mod asked for, as the fake world ran it (`FakeWorld.Writes`).
- `FakeWorldWrite(ulong Step, string? ModId, EntityId Entity, string Component, int Offset, WorldWriteStatus Status, byte[] Before, byte[] After)`
- `After { get; init; } -> byte[]` — The same bytes afterwards.
- `Before { get; init; } -> byte[]` — The bytes it named, as they were; empty when nothing was read.
- `Component { get; init; } -> string` — The component's short name.
- `Entity { get; init; } -> EntityId` — The entity.
- `ModId { get; init; } -> string?` — The mod that asked; null for a call outside any mod.
- `Offset { get; init; } -> int` — The first byte the write named.
- `Status { get; init; } -> WorldWriteStatus` — What became of it.
- `Step { get; init; } -> ulong` — The harness step at whose start the write ran: the step after the call.

## TestKit.HarnessAmmoList `0.2.15` — The fake kiln.ui.ammo of a harness (`KilnHarness.AmmoList`): Ui.AmmoList's own code runs on it.
- `Hold(AmmoListKind kind, ItemId weapon, params AmmoListRow[] gameRows) -> void` — The player holds `weapon` (a bow for `AmmoListKind.Arrow`, a staff for `AmmoListKind.Spell`): the game builds its...
- `KnownItems(AmmoListKind kind, params ItemId[] items) -> void` — The client's items of this kind (the arrows, the spells): a snapshot naming any other item is refused, as the game...
- `ModeOf(AmmoListKind kind) -> AmmoListMode?` — How the mods' rows go in (null: the game's list as it is).
- `NewSession() -> void` — A new client session (the player left and joined again): the snapshots and queued picks are cleared, as native does...
- `Pick(AmmoListKind kind, ItemId item) -> bool` — The player picks the row of `item`.
- `Player { get; set; } -> EntityId` — The local player the lists are built for (the context's `Player`).
- `Provided(AmmoListKind kind) -> IReadOnlyList<AmmoListRow>` — The mods' rows Kiln.Ui published for the list (composed, in order); empty while the game's list shows as it is.
- `Publishes { get; } -> int` — Snapshots published so far (a clear included).
- `Select(AmmoListKind kind, ItemId item) -> void` — The player moves the list's selection onto the row of `item`.
- `Selected(AmmoListKind kind) -> ItemId?` — The item the selection rests on: it stays on its item when the rows change, and goes to the first row when its item...
- `Session { get; } -> uint` — The client session the lists belong to (1 at the start).
- `SetGameRows(AmmoListKind kind, params AmmoListRow[] gameRows) -> void` — The backpack changed: the game's own rows of the held weapon's list are now `gameRows`.
- `Shown(AmmoListKind kind) -> IReadOnlyList<AmmoListRow>` — The list as the HUD shows it now: the game's rows, then (Append) or instead (Replace) the mods' rows.
- `Unhold(AmmoListKind kind) -> void` — The weapon is put away: the list is not built (no context; its snapshot stays until Kiln.Ui publishes another).

## TestKit.HarnessAssets `0.2.3` — The kit's kiln.assets of a harness (`KilnHarness.Assets`): Kiln.Assets' own code runs on it.
- `AddGameBlob(byte[] bytes) -> string` — Adds `bytes` to the game's content index (in order; a hash already there is not listed twice by the catalog).
- `CatalogGeneration { get; } -> ulong` — The catalog's generation: it grows with every change of the registrations.
- `IndexReady { get; set; } -> bool` — Whether the game's content index can be read.
- `Registrations { get; } -> IReadOnlyList<TestKit.FakeBlobRegistration>` — The live registrations, in the order that decides which one is served (the later one).

## TestKit.HarnessBootPhase `0.2.3` — The boot phase of a harness (`KilnHarness.BootPhase`): a unit test of a mod's `OnBoot`.
- `Run(string folder, bool commit = true) -> TestKit.BootRun` — Runs the OnBoot of the mod in `folder` (a built mod folder with a kiln.mod.json that has `"boot"`) and, with...
- `Runs { get; } -> IReadOnlyList<TestKit.BootRun>` — Every run of this harness, in order.
- `SetRecord(string key, string valueJson, Guid resourceId = default(Guid), uint typeHash = 0, int part = 0) -> void` — A record `boot.GameData.Read()` answers: its value as camelCase JSON.

## TestKit.HarnessCapabilities `0.2.1` — The capability rows mods see offline (`KilnHarness.Capabilities`): every Kiln service the kit has no fake for is unavailable with reason `offline`, so a mod's own...
- `All { get; } -> IReadOnlyList<TestKit.CapabilityRow>` — Every row, sorted by name.
- `ClearOverride(string name) -> void` — Drops the test's override of `name`.
- `Is(string name) -> bool` — Whether `name` is available (what `Game.HasFeature` answers).
- `Override(string name, bool available, string? reasonText = null) -> void` — Switches a row on or off for this harness.
- `Why(string name) -> string` — One sentence saying whether and why not, as a mod could log it.

## TestKit.HarnessChat `0.2.2` — The fake kiln.chat of a harness (`KilnHarness.Chat`): Kiln.Chat's own code (`Chat`, `Chat.Received`, `ChatMessageEvent`, commands typed in chat) runs on it.
- `Sent { get; } -> IReadOnlyList<TestKit.FakeChatLine>` — Every server line mods and Kiln sent, in order: one per `Broadcast`, `Tell` or `Say` line that reached a player...
- `Typed { get; } -> IReadOnlyList<TestKit.FakeChatMessage>` — Every line the players typed, as delivered, in order (consumed ones included).

## TestKit.HarnessCombat `0.2.2` — The fake kiln.combat of a harness (`KilnHarness.Combat`): Kiln.Combat's own code (`Combat.GetHealth`, `SetHealth`, `Heal`, `Damage`, `Kill`, god mode...
- `Health(EntityId entity) -> TestKit.FakeHealth?` — The entity's health; null when it has none.
- `Hit(EntityId target, int amount, EntityId attacker = default(EntityId), EntityId source = default(EntityId)) -> ulong` — The game deals a hit: `amount` damage (negative: a healing hit) on `target`.
- `Hits { get; } -> IReadOnlyList<TestKit.FakeHit>` — Every hit, mods' and the game's, in the order they were dealt.
- `Respawn(EntityId entity, int health) -> void` — A dead player (or creature) is alive again with `health`: `PlayerRespawnEvent` at the next step.
- `SetHealth(EntityId entity, int current, int max) -> void` — Gives an entity health, as the game's Health component holds it (no event).

## TestKit.HarnessComponentChanges `0.2.4` — The fake kiln.component_changes of a harness (`KilnHarness.ComponentChanges`): Kiln.World's own consumer of `ComponentChanges.Watch` runs on it.
- `PresentPairs { get; } -> int` — The present (entity, component) pairs the watches cover.
- `QueuedFrames { get; } -> int` — The frames of changes that wait for the consumer (at most two).
- `QueueOverflows { get; } -> ulong` — How often the queue was full and the watches were suspended.
- `SetAttribute(EntityId entity, ObservedAttribute attribute, int current, int maximum) -> void` — The typed current and maximum value of an attribute as the game's attribute code would read them (the kit does not...
- `Watches { get; } -> int` — How many watches exist.

## TestKit.HarnessCrafting `0.2.15` — Player-source registrations and their filtered counts in the kit.
- `Dyes(EntityId player) -> IReadOnlyDictionary<ItemId, long>` — The same registered counts restricted to ColorPalette items (dyes), with current filters, player-session generation...
- `Sources { get; } -> IReadOnlyList<TestKit.FakeCraftSourceRegistration>` — Live registrations, in registration order.
- `Stock(EntityId player) -> IReadOnlyDictionary<ItemId, long>` — Registered source counts only (not the bag), with the same per-player allow/deny union and current inventory...

## TestKit.HarnessCreatureControl `0.2.3` — The fake kiln.creature_control of a harness (`KilnHarness.CreatureControl`): Kiln.CreatureControl's own code (`CreatureControl.Acquire`, `WaitActive`, `Inspect`...
- `Calls { get; } -> IReadOnlyList<TestKit.FakeControlCall>` — Every call mods made, in order.
- `IsControlled(EntityId body) -> bool` — True while the broker holds `body` (its AI-off layer and the SteeringAgent removal, in the game).
- `SetBusy(EntityId body, bool busy = true) -> void` — Makes `body`'s current action keep playing (true) or end (false): a lease on it stays Pending while it plays, and...

## TestKit.HarnessDecisions `0.2.4` — The fake kiln.decisions of a harness (`KilnHarness.Decisions`): Kiln's own dispatcher of decisions runs on it.
- `Active { get; } -> int` — How many decisions are open (held, not ended).
- `Outcomes { get; } -> IReadOnlyList<DecisionResult>` — How every decision ended so far, in order: what the service recorded, whether or not a mod listened for...
- `RestartWorld() -> void` — The server world stops and the next one starts, as when a hosted world is switched: every open decision ends...

## TestKit.HarnessImpactNatives `0.2.4` — The fake kiln.abilities natives of a harness (`KilnHarness.ImpactNatives`): Kiln.Abilities' own code (`Abilities.Natives.RegisterSync`, `Attach`, `DeclareTyped`, an...
- `AddProgram(uint program, params TestKit.FakeEcall[] calls) -> void` — Declares a game program in the world's ImpactRegistry with the natives it calls and where (the ECALL's call-site...
- `Call(TestKit.FakeImpactCall call) -> TestKit.FakeImpactResult` — Makes one ECALL of `FakeImpactCall.Native`, as a program's step would, and waits for it.
- `Calls { get; } -> IReadOnlyList<TestKit.FakeImpactCallRecord>` — Every call the test made, in order.
- `OtherRead { get; } -> ISet<string>` — The types of impact_simulation's other_read list: what NativeContext.TryRead may read of any entity (with...
- `OtherWrite { get; } -> ISet<string>` — The types of impact_simulation's other_write list: what NativeContext.TryWrite may write (and TryRead read).
- `Serial { get; set; } -> bool` — The simulated pin-and-world-thread condition for ECS access (default true).
- `SetGameNative(string native, TestKit.FakeGameNativeHandler? handler = null) -> void` — Makes `native` one of the game's natives (its node name, `"applyDamage"`, or "0x" and its hash) and says what it...

## TestKit.HarnessItems `0.2.2` — The fake kiln.items of a harness (`KilnHarness.Items`): Kiln.Items' own code (`Items` Give, Take, TakeSlot, Read, FindInventories, the catalog, `ItemsGivenEvent` and...
- `AddContainer(EntityId entity, int inventories = 1) -> TestKit.FakeInventory` — Gives an entity of the fake world an inventory: a chest, a station.
- `AddItem(string name, int maxStack, ItemCategory category = ItemCategory.Materials, bool gear = false, ItemRarity rarity = ItemRarity.Common, int levelMin = 0, int levelMax = 0, Guid id = default(Guid)) -> ItemId` — Says the game has an item, so a mod can give, take, look it up and read it.
- `Calls { get; } -> IReadOnlyList<TestKit.FakeItemsCall>` — Every give and take a mod asked for, in the order they ran, with what became of each.
- `ContainerName(EntityId entity) -> string?` `0.2.13` — The name a container carries in the fake: what `Items.SetContainerName` stored (a mod's rename runs at the start of...
- `IdOf(string name) -> ItemId` — The id of an item the test added.
- `Of(EntityId entity) -> TestKit.FakeInventory?` — The inventory of an entity (a container, or an in-game fake player's); null when it has none.
- `PlayerInventories { get; set; } -> int` — How many 8-slot inventories a fake player's inventory gets (default 5, 40 slots): the kit does not model the game's...
- `SetEquipmentSlotType(ItemId item, EquipmentSlot slot) -> void` `0.2.15` — Sets the equipment type a fake item fits (both ring slots share one type).
- `SetGear(ItemInstanceRef item, int? enhancements = null, int? unlocked = null, int? level = null, int? durability = null) -> void` `0.2.12` — Changes a piece of gear as if the game had: a player's unlock at the workbench (`unlocked`, with `level` one...
- `SetPerkSlots(ItemId item, int slots) -> void` `0.2.12` — Says how many of a gear item's five perk slots its type fills (its last filled one + 1; default 5): gear made...
- `SetUpgradeRolls(params double[] rolls) -> void` `0.2.12` — Names the rolls of the upgrades that follow (`Items.UpgradeRarity`), in order, each 0 (inclusive) to 1 (exclusive)...

## TestKit.HarnessOutlines `0.2.13` — The fake kiln.render's outlines of a harness (`KilnHarness.Outlines`): Ui.World.Outline's own code runs on it.
- `All { get; } -> IReadOnlyList<TestKit.FakeOutline>` — Every request this harness got, oldest first, ended ones too.
- `Despawn(EntityId entity) -> int` — `entity` leaves the client world: every live request on it ends now (`OutlineReason.Despawned`).
- `EndWorld() -> void` — The client world ends (the player leaves the game): every live request ends now (`OutlineReason.WorldEnded`).
- `GameHighlight(EntityId entity, bool on = true) -> void` — The game highlights `entity` itself (the building cursor rests on it), or stops; applies at the next step.
- `HideModel(EntityId entity, bool hidden = true) -> void` — `entity` has no model instance (hidden, not streamed in, a voxel model), or has one again; applies at the next step.
- `Live { get; } -> IReadOnlyList<TestKit.FakeOutline>` — The requests that have not ended, oldest first.
- `Of(WorldOutline outline) -> TestKit.FakeOutline?` — The request behind `outline` (null: not this harness's, or it ended before the kit saw it).
- `SetState(WorldOutline outline, OutlineState state, OutlineReason reason = OutlineReason.None) -> void` — Pins the state of `outline`'s request (the steps no longer decide it): what `WorldOutline.State` and...
- `Setting { get; set; } -> TestKit.FakeOutlineSetting` — The player's kiln.json `"ui": {"outlines": ...}`; applies at the next step.
- `Showing(EntityId entity) -> TestKit.FakeOutline?` — The request whose colour `entity`'s model shows now, or null.

## TestKit.HarnessPlayers `0.2.1` — The players of a harness's server (`KilnHarness.Players`).
- `All { get; } -> IReadOnlyList<TestKit.FakePlayer>` — The players the test joined that have not left (in game, or joining until the next step).
- `Join(string name, ulong steamId = 0, PlayerPermissions rights = PlayerPermissions.None, bool isAdmin = false, WorldPosition? position = null, uint userGroupId = 0) -> TestKit.FakePlayer` — A player connects and logs in: in game, with an entity (slot + 1), at the start of the next step.
- `Slots { get; } -> int` — Player slots of the server (16, as a dedicated server of this game has).

## TestKit.HarnessPrograms `0.2.4` — The fake world's programs (`KilnHarness.Programs`).
- `Add(string name, TestKit.FakeProgramLists lists, Action<TestKit.FakeProgramSlice>? update = null) -> void` — Declares one of the game's programs in the fake server world, after the ones declared before (its index).
- `Add<T>(Action<TestKit.FakeProgramSlice>? update = null) -> void` — Declares the program of Kiln.Game's typed view `T` (`Ecs.Programs.FuelSetup`): its name and lists are the view's...
- `Marks { get; } -> IReadOnlyList<TestKit.FakeProgramMark>` — The writes wrap stages marked, in order.
- `Names { get; } -> IReadOnlyList<string>` — The names declared, in table order.
- `Updates(string program) -> int` — How many slices of `program` the game's update (the declared one) ran on.

## TestKit.HarnessQueries `0.2.3` — The fake kiln.queries of a harness (`KilnHarness.Queries`): Kiln.Queries' own code runs on it.
- `MaxRaysPerStep { get; set; } -> int` — How many rays the fake admits per step before it refuses with `BudgetExceeded` (the service's default: 64).
- `OpenPaths { get; } -> int` — How many path requests hold a slot now (not ended, or ended and not yet delivered).
- `PathRequests { get; } -> IReadOnlyList<TestKit.FakePathRequest>` — Every path request that was admitted, in order.
- `Paths { get; set; } -> Func<TestKit.FakePathRequest, TestKit.FakePathAnswer>?` — What the path search answers a request.
- `PathSteps { get; set; } -> int` — How many steps after its admission a path request answers (default 3; the game takes a few).
- `Rays { get; } -> IReadOnlyList<TestKit.FakeVoxelRay>` — Every ray that was admitted, in order.
- `Voxels { get; set; } -> Func<TestKit.FakeVoxelRay, TestKit.FakeVoxelAnswer>?` — What a voxel ray answers.
- `WithoutSpawnTime(EntityId entity) -> void` — Makes `entity` an anchor without a SpawnTime marker (as a saved entity or an item is in the game): a path request...

## TestKit.HarnessSide `0.2.10` — Which game process a `KilnHarness` stands for, for its mods' Kiln tasks (#82).
enum: DedicatedServer, HostingClient, JoinedClient

## TestKit.HarnessSpawn `0.2.2` — The fake kiln.spawn of a harness (`KilnHarness.Spawned`): Kiln.Spawn's own code (`Spawn`, `EntitySpawnedEvent`, `EntityDestroyedEvent`, `EntityRef` stamps) runs on...
- `AddTemplate(string name, TemplateKind kind = TemplateKind.Creature, string displayName = "", Guid id = default(Guid)) -> Guid` — Says the game has a template loaded, so a mod can spawn it by name, display name or GUID (and `Spawn.Lookup`...
- `All { get; } -> IReadOnlyList<TestKit.FakeSpawned>` — The entities Kiln.Spawn made that are still in the world, in the order they were spawned.
- `Calls { get; } -> IReadOnlyList<TestKit.FakeSpawnCall>` — Every spawn, item drop, destroy and cleanup a mod asked for, in order, refused ones included.
- `Of(string template) -> IReadOnlyList<TestKit.FakeSpawned>` — The live spawned entities of a template (by name, ignoring case).
- `const PickupTemplate = "Base_LootPickup_Generic_Gravity" -> string` — The name of the game's generic item pickup, the template of every item drop.

## TestKit.HarnessState `0.2.1` — Where a harness's server is in its life.
enum: Created, Starting, Running, Stopped, Disposed, Abandoned

## TestKit.HarnessStoppedException `0.2.1` — The harness's tick thread was given up on (a test blocked it and the watchdog stopped the harness): no call works any more.

## TestKit.HarnessTasks `0.2.10` — The Kiln tasks of a harness's mods (`KilnHarness.Tasks`).
- `Live { get; } -> int` — The tasks that have not ended yet (and ended ones with a hop still to run).
- `Settle(int maxSteps = 1000, TimeSpan? timeout = null) -> int` — Runs the mods' tasks to their end: waits while their work runs on the workers, and steps the server whenever all of...

## TestKit.HarnessTerrain `0.2.3` — The fake world of a harness behind Kiln.Terrain (`KilnHarness.Terrain`): `Terrain.Inspect`, `Apply`, `Forget` and `Applied` run their own code on it.
- `AddBase(CellBox box) -> void` — A player base: overlays leave blocks that touch `box` alone unless bases are allowed.
- `Editing { get; set; } -> Action<CellBox>?` — Runs inside every write of an overlay where kiln.build posts its `BuildEditingEvent`: after the overlay read the...
- `Get(CellPos at) -> Voxel` — The cell at `at`.
- `GroundY { get; set; } -> int` — Cells below this height are solid ground, cells from it up are air, where nothing was set.
- `Loaded { get; set; } -> bool` — Whether the voxel tiles are there.
- `LoseLedger(string mod, bool wholeWorldData = false) -> bool` — Loses `mod`'s terrain ledger in this world, as when the world's Kiln data or the ledger key is lost; the cells and...
- `Restart() -> void` — The server saves the world and starts again (`HarnessTerrain.SaveWorld`, then the next start).
- `Revision { get; } -> uint` — The world file's revision; `HarnessTerrain.SaveWorld` adds one.
- `SaveWorld() -> void` — The game writes the world file: the ledgers as they are now are in that save (a record made before it and lost after...
- `Set(CellPos at, Voxel cell) -> void` — Sets one cell, as a player's edit would (no ledger knows of it).
- `Witnessed(string mod) -> IReadOnlyList<string>` — The plans Kiln's witness has for `mod` in this world, sorted.
- `WorldId { get; set; } -> string` — The world's id: a ledger belongs to one world.
- `Writes { get; } -> IReadOnlyList<TestKit.FakeTerrainWrite>` — Every write overlays made, in order.

## TestKit.HarnessTime `0.2.2` — The fake kiln.time of a harness (`KilnHarness.Time`): Kiln.Time's own code (`Time.OfDay`, `State`, `IsDay`, `Set`, `SetSpeed`, `SetCycle`, `TimeSetEvent`...
- `Calls { get; } -> IReadOnlyList<TestKit.FakeTimeCall>` — Every change to the clock a mod made, in order.
- `DayLength { get; set; } -> TimeSpan` — How long the day phase lasts in real time (the server's GameSettings dayTimeDuration, 2 to 60 minutes; default 30).
- `Hours { get; } -> double` — The time of day now, 0..24.
- `IsDay { get; } -> bool` — Whether it is day (06:00 to 18:00).
- `NightLength { get; set; } -> TimeSpan` — How long the night phase lasts in real time (nightTimeDuration, 2 to 60 minutes; default 12).
- `Set(double hours) -> void` — Sets the time of day as the game's own admin menu does (setTimeOfDay: the clock restarts in its first cycle), with...
- `Speed { get; } -> double` — The speed the clock runs at (1: the game's; what a mod's `Time.SetSpeed` holds).

## TestKit.HarnessUiInventory `0.2.12` — The fake kiln.ui.inventory of a harness (`KilnHarness.UiInventory`): Ui.Inventory's own code runs on it.
- `Click(int index, SlotButton button = SlotButton.Left, SlotModifiers modifiers = SlotModifiers.None) -> TestKit.FakeSlotClickResult` — The player presses `button` with `modifiers` on drawn slot `index`.
- `ClickSubscriptions { get; } -> int` — OnClick subscriptions held now.
- `CloseScreen() -> void` — Every menu closed: the play view (no slots drawn).
- `Draw(HudScreen screen, params TestKit.FakeInventorySlot[] slots) -> void` — The game draws `slots` on `screen` (the latest UI frame from now on; the hovered slot is cleared).
- `Drawn { get; } -> IReadOnlyList<TestKit.FakeInventorySlot>` — The slots of the latest `HarnessUiInventory.Draw` (empty before the first).
- `Hover(int? index) -> void` — The cursor rests on drawn slot `index` (null: on none): what Ui.Inventory.Hovered reads.
- `MarkOf(int index) -> SlotMark?` — The mark drawn slot `index` would show (null: none): an item mark of any mod first, then the MarkWhen rules in...
- `OpenBackpack(params TestKit.FakeInventorySlot[] slots) -> void` — The player opens the backpack with these slots drawn (the backpack screen, HUD Backpack).
- `OpenChest(EntityId chest, params TestKit.FakeInventorySlot[] slots) -> void` — The player opens the storage chest `chest` (its entity: what InventoryScreen.Container and GameScreen.Target carry)...
- `PanelsShown { get; } -> IReadOnlyList<string>` — The titles of the Ui.Inventory.AddPanel panels that show on the open screen now (their Screens and When, in the...
- `PressPad(string chord) -> TestKit.FakeSlotClickResult` `0.2.14` — The player presses a controller chord (`"Pad:LS"`, `"Pad:LB+A"`) while the game's selection rests on a drawn slot...
- `Screen { get; } -> InventoryScreen?` — The backpack or chest screen open now (what Ui.Inventory.Screen says in a game), or null.
- `Select(int? index) -> void` `0.2.14` — The game's own selection (a controller's: what Ui.Inventory.Selected reads, since 0.2.0-dev.30.5) rests on drawn...

## TestKit.HarnessUiItemDetail `0.2.14` — Explicit game-panel captures for Ui.Values.ItemDetail.
- `Capture(ItemId item, EntityId entity, int level, ItemRarity? rarity, params UiItemDetailLine[] lines) -> void` — Records a completed panel (owned copy).
- `Clear() -> void` — Forgets every recorded panel, as on a client world change.

## TestKit.HarnessUiValues `0.2.3` — The fake kiln.ui.values of a harness (`KilnHarness.UiValues`): Ui.Values' own code runs on it.
- `Decisions { get; } -> IReadOnlyList<TestKit.FakeUiDecision>` — Every provider call of `HarnessUiValues.Draw` so far, in order.
- `DetailLines(TestKit.FakeUiItem item) -> IReadOnlyList<string>` — Draws `item`'s detail view: the tooltip-lines providers run in order and the lines they added come back (at most 16...
- `Draw(uint kind, TestKit.FakeUiItem item, TestKit.FakeUiBar game) -> TestKit.FakeUiBar` — Draws a value of `kind` (`UiValueKind`'s number: 0 detail, 1 slot, 2 held, 3 health, 4 stamina, 5 mana) for `item`...
- `Registrations { get; } -> int` — Registrations held now (values and tooltip lines).

## TestKit.HarnessWorld `0.2.1` — The world of a harness's server (`KilnHarness.World`): what the game's world file and its save cycle are to Kiln.Saves.
- `Directory { get; } -> string` — The world's sidecar folder, `kiln/data/worlds/<id>`: mod data and per-world configs.
- `Id { get; } -> Guid` — The world's id (what `Saves.WorldId` and `WorldData.WorldId` say).
- `Loaded { get; } -> bool` — Whether the world has loaded (the first step after the server started).
- `Revision { get; } -> uint` — The revision of the newest written save (0: never saved).
- `Save(int maxSteps = 100) -> uint` — Saves the world now, as an admin's "save now": asks the game for a save and steps the server until the world file is...

## TestKit.HarnessWorldTables `0.2.4` — Scripted process boot data, not a game or renderer.
- `Dependency(string mod, string dependency) -> void` — Declares that `mod` depends on `dependency`, as if its manifest listed it.
- `GameMaterial(string name, uint item, bool roof = false, bool terrain = false) -> void` — Scripts a game (vanilla) material that a pool entry may name as its `gameMaterial`.
- `Material(string mod, string key, uint item, bool roof = false, bool live = true, bool terrain = false) -> void` — Configures a live or retained building/terrain material and its block item.
- `PendingCount { get; } -> int` — How many accepted pool and brush definitions are pending: `HarnessWorldTables.PendingToolDefinitionsJson` copies them.
- `PendingToolDefinitionsJson(string mod) -> string` `0.2.14` — A copy of the accepted pending policy-pool and computed brush descriptors, in world-table JSON form.
- `Readiness { get; set; } -> CapacityReadiness` — The readiness the fake's boot tables report.
- `ReservePoolId(uint id) -> void` — Models an occupied foreign pool ID so collision refusal can be tested without searching for an FNV collision.
- `ServePool(string mod, string key, BuildingMaterialPoolBlock block) -> void` — Configures one actual served boot row.
- `StorageFails { get; set; } -> bool` — When true, the fake refuses new pool and brush definitions with a storage failure, as if it could not persist them.
- `TerrainBrushCells(string mod, string key) -> byte[]?` `0.2.14` — A copy of a pending sphere descriptor's raw fill bytes (x fastest, then y, then z), sampled at half-metre voxel...
- `TerrainItem(string name, uint item, byte cell) -> void` `0.2.14` — Scripts a placeable terrain item's item-to-cell mapping.

## TestKit.KilnHarness `0.2.1` — A Kiln server with no game, for mod tests under `dotnet test`: a temp server folder with its `kiln/` tree, mods loaded the way the game loads them (Kiln.Host's own...
- `KilnHarness(TestKit.KilnHarnessOptions? options = null)` — Creates the temp server folder, points Kiln at it and starts the tick thread.
- `AmmoList { get; } -> TestKit.HarnessAmmoList` `0.2.15` — The fake kiln.ui.ammo of this harness: the HUD's ammunition and spell lists Ui.AmmoList adds rows to (the game's...
- `As(TestKit.ModHandle mod, Action action) -> void` — Runs `action` on the tick thread as code of `mod`: its `HostAccess.Current` and synchronization context are set as...
- `As<T>(TestKit.ModHandle mod, Func<T> func) -> T` — Runs `action` on the tick thread as code of `mod`: its `HostAccess.Current` and synchronization context are set as...
- `Assets { get; } -> TestKit.HarnessAssets` `0.2.3` — The kit's kiln.assets of this harness: the game's content index the blob catalog reads, and the content blobs mods...
- `Boot() -> void` — Loads the waiting mods and starts the server (the first step) when that has not happened yet.
- `BootPhase { get; } -> TestKit.HarnessBootPhase` `0.2.3` — The boot phase of this harness (Mod.OnBoot): run a boot mod's OnBoot as a start does, read what it staged and why it...
- `Capabilities { get; } -> TestKit.HarnessCapabilities` — Capability rows as mods see them offline, and the test's overrides.
- `Chat { get; } -> TestKit.HarnessChat` `0.2.2` — The server's text chat as the kit's fake of kiln.chat keeps it: what the players typed and every server line mods sent.
- `Combat { get; } -> TestKit.HarnessCombat` `0.2.2` — Health, hits and deaths in this server's fake world as the kit's fake of kiln.combat keeps them: the test gives...
- `ComponentChanges { get; } -> TestKit.HarnessComponentChanges` `0.2.4` — The kit's fake kiln.component_changes of this harness: what is watched, and the typed values its attribute events carry.
- `ConfigDirectory { get; } -> string` — `kiln/config`: mod configs (`Config.Directory`).
- `ControlDirectory { get; } -> string` — `kiln/control`: the console folder is `control/console` (Kiln.Chat polls it).
- `Crafting { get; } -> TestKit.HarnessCrafting` `0.2.15` — The linked crafting-source recording fake.
- `CreatureControl { get; } -> TestKit.HarnessCreatureControl` `0.2.3` — The kit's fake kiln.creature_control of this harness: what a test scripts of the bodies a mod leases, and what the...
- `CurrentStep { get; } -> ulong` — The last step that ran (0 before the first).
- `DataDirectory { get; } -> string` — `kiln/data`: each mod's `ModContext.DataDirectory` is `data/<id>`.
- `Decisions { get; } -> TestKit.HarnessDecisions` `0.2.4` — The kit's fake kiln.decisions of this harness: how the decisions mods were asked about ended.
- `Dispose() -> void` — Stops the server (`ServerStoppingEvent`, every mod's `OnUnload` in reverse load order), logs any mod whose load...
- `DumpLogs() -> string` — The captured lines as text, one per line (for an assertion message).
- `EnterWorld() -> void` `0.2.10` — The player hosts or joins again, for the tasks (#82): new sessions of the side's worlds (the server world first on a...
- `Entities { get; } -> TestKit.FakeWorld` `0.2.2` — The fake world's entities and components: what Kiln.World (`World`), Kiln.Game's typed reads and tracked writes run...
- `FindLogs(string text, string? modId = null) -> IReadOnlyList<TestKit.LogLine>` — The lines whose text contains `text` (ordinal), optionally only those of one mod.
- `GameLog(string text, LogLevel level = LogLevel.Info) -> void` — A line of the game's own log, delivered at the next step: to mods' `ModContext.GameLog` handlers, and to Kiln's...
- `HasLog(string text, string? modId = null) -> bool` — Whether a line contains `text`, optionally only one of mod `modId`.
- `ImpactNatives { get; } -> TestKit.HarnessImpactNatives` `0.2.4` — The kit's fake kiln.abilities natives of this harness: the game's programs and natives a test declares, and the...
- `Items { get; } -> TestKit.HarnessItems` `0.2.2` — The items and inventories of this server as the kit's fake of kiln.items keeps them: the items the game has (the...
- `Kill() -> void` — Ends the server the way `kill -9` does, as far as the world goes: the game makes no shutdown save, a save that is...
- `KilnDirectory { get; } -> string` — `<server>/kiln`: what `KilnPaths.Root` points at while the harness lives.
- `Language { get; set; } -> string` — The game's language as mods see it (`Game.Language`, and so `ModLocale.Get`): `"en-us"`, as a dedicated server...
- `LeaveWorld() -> void` `0.2.10` — The player leaves the world, for the tasks (#82): its worlds' STOPPING, the client world first.
- `static ListTests(Assembly assembly) -> IReadOnlyList<string>` — The `[KilnTest]` names of `assembly` in the order Kiln.Testing's runner runs them: by `Order`, then the type's full...
- `Load<TMod>(TestKit.ModManifest? manifest = null) -> TestKit.ModHandle` — Loads a second, isolated copy of the assembly that holds `TMod`, as the game does: its files go into...
- `LoadFolder(string path) -> TestKit.ModHandle` — Loads a built mod folder (its `kiln.mod.json`, assemblies, `locales/` and data), copied into `kiln/mods/<id>` as...
- `LogFile { get; } -> string` `0.2.11` — The path of `kiln/logs/kiln.log`, the file the harness writes every captured line to.
- `Logs { get; } -> IReadOnlyList<TestKit.LogLine>` — Every line Kiln wrote while this harness lived, in order.
- `LogsDirectory { get; } -> string` — `kiln/logs`: `kiln.log` (every captured line), `mods.txt`, `tests/`.
- `LogsOf(TestKit.ModHandle mod) -> IReadOnlyList<TestKit.LogLine>` — The lines mod `modId` wrote (its `Logger`, and Kiln's lines in its name such as an event handler that threw).
- `LogsOf(string modId) -> IReadOnlyList<TestKit.LogLine>` — The lines mod `modId` wrote (its `Logger`, and Kiln's lines in its name such as an event handler that threw).
- `MenuFrame() -> int` `0.2.12` — One UI frame of the game's main menu, for the tasks (#83): on the game's main thread, while no world runs (after...
- `Mod(string id) -> TestKit.ModHandle` — The handle of a mod this harness was given (by id).
- `ModsDirectory { get; } -> string` — `kiln/mods`: where the harness puts each mod's folder.
- `OnMainThread(Action work) -> void` `0.2.10` — Runs `work` on the game client's main thread, where the client world steps (#82): a task started there belongs to...
- `Outlines { get; } -> TestKit.HarnessOutlines` `0.2.13` — The fake kiln.render's outlines of this harness (Ui.World.Outline): what mods outlined, and the game's side of it...
- `Players { get; } -> TestKit.HarnessPlayers` — The players on this server: `HarnessPlayers.Join` one, `FakePlayer.Leave`.
- `Programs { get; } -> TestKit.HarnessPrograms` `0.2.4` — The game's ECS programs in the harness's fake server world (kiln.world v4): the ones a test declares, which...
- `Queries { get; } -> TestKit.HarnessQueries` `0.2.3` — The fake kiln.queries of this harness: what a voxel ray answers, and the rays mods asked for.
- `ReadLogFile() -> string` `0.2.11` — The text of `KilnHarness.LogFile` as written so far ("" when it was not created).
- `Reload(TestKit.ModHandle mod, bool checkCollected = true) -> TestKit.ModHandle` — Reloads `mod` from its folder with its dependents (they unload first and load again after it), as the control...
- `ReportWorldRunning() -> void` — The game says its world runs (the line Kiln's lifecycle waits for): with `KilnHarnessOptions.WaitForWorldRunning`...
- `Run(Action action) -> void` — Runs `action` on the tick thread, as Kiln (no mod).
- `Run<T>(Func<T> func) -> T` — Runs `func` on the tick thread, as Kiln (no mod), and returns its result.
- `RunCommand(CommandSource source, string line) -> CommandResult` — Runs a command line as `source` (see `KilnHarness.RunCommand`).
- `RunCommand(TestKit.FakePlayer player, string line) -> CommandResult` — Runs a command line as `player` typed it in chat (`CommandSource.ForPlayer`), through the public `Commands.Execute`...
- `RunCommand(string line) -> CommandResult` — Runs a command line as the server console (`CommandSource.Console`), through the public `Commands.Execute` on the...
- `RunTest(TestKit.ModHandle mod, string name, TestKit.KilnTestOptions? options = null) -> TestKit.KilnTestResult` — Runs one `[KilnTest]` of `mod` through Kiln.Testing's own runner (as the control command `test <mod> =name` does)...
- `RunTests(TestKit.ModHandle mod, TestKit.KilnTestOptions? options = null) -> IReadOnlyList<TestKit.KilnTestResult>` — Runs every `[KilnTest]` of `mod`, one `KilnHarness.RunTest` each, in the runner's order.
- `ServerDirectory { get; } -> string` — The temp server folder (the game's install folder in a real server).
- `Spawned { get; } -> TestKit.HarnessSpawn` `0.2.2` — What Kiln.Spawn (`Spawn`) did in this server's fake world: the templates the game has, the entities mods spawned...
- `State { get; } -> TestKit.HarnessState` — Where the server is.
- `StateDirectory { get; } -> string` — `kiln/state`: `mods.json` for the launcher.
- `Step(int steps = 1) -> void` — Runs `steps` server steps, each in the order of Kiln.Host's Pre: the first one loads the mods waiting in `kiln/mods`...
- `StepUntil(Func<bool> condition, int maxSteps = 1000, TimeSpan pace = default(TimeSpan)) -> int` — Steps until `condition` (checked on the tick thread before each step) is true; returns the steps it took (0 when it...
- `Stop(string reason = "SIGINT") -> void` — Stops the server at once, as a stop signal does: `ServerStoppingEvent` with `reason`, then every mod's `OnUnload` in...
- `Tasks { get; } -> TestKit.HarnessTasks` `0.2.10` — Kiln tasks (`ModContext.Tasks`) of the mods under this harness: they run on the kit's task pool, whose workers are...
- `Terrain { get; } -> TestKit.HarnessTerrain` `0.2.3` — The fake world behind Kiln.Terrain in this harness: its cells, bases and what overlays wrote.
- `Time { get; } -> TestKit.HarnessTime` `0.2.2` — The server's day/night clock as the kit's fake of kiln.time runs it on the harness's virtual time.
- `UiInventory { get; } -> TestKit.HarnessUiInventory` `0.2.12` — The fake kiln.ui.inventory of this harness: draw a screen's slots, hover and click one, and ask what mark a drawn...
- `UiItemDetail { get; } -> TestKit.HarnessUiItemDetail` `0.2.14` — Recordings of the game's item-detail text and values (no game renderer is simulated).
- `UiValues { get; } -> TestKit.HarnessUiValues` `0.2.3` — The fake kiln.ui.values of this harness: draw a value or an item detail view through the registered Ui.Values providers.
- `Unload(TestKit.ModHandle mod, bool checkCollected = true) -> void` — Unloads `mod`, and first every mod that depends on it (reverse order), as the control command `unload` does.
- `World { get; } -> TestKit.HarnessWorld` — The world this server runs: its id, its saves, its sidecar folder.
- `WorldTables { get; } -> TestKit.HarnessWorldTables` `0.2.4` — Scripts boot pool snapshots and registration refusals.
- `WritePermissions(string json) -> void` — Writes `kiln/config/permissions.json` (JSONC, the format Kiln writes: groups, userGroups, players by SteamID64) and...
- `WriteServerConfig(string json) -> void` — Writes the server's `enshrouded_server.json` in `KilnHarness.ServerDirectory` (Kiln.Permissions reads its...

## TestKit.KilnHarnessOptions `0.2.1` — Settings of one `KilnHarness`.
- `CallTimeout { get; init; } -> TimeSpan` — How long an ordinary harness call (including Step, Unload and Reload) may block its tick thread before the watchdog...
- `CrashReports { get; init; } -> bool` — Crash reports (a mod disabled by the fault policy) are written to `kiln/crash-reports` in the temp folder.
- `FaultLimit { get; init; } -> int` — Faults within `KilnHarnessOptions.FaultWindowSeconds` that disable a mod (kiln.json `mods.fault_limit`).
- `FaultWindowSeconds { get; init; } -> double` — The fault window in seconds of the harness's virtual time (steps x `KilnHarnessOptions.WallDeltaMs`); 0: ever.
- `KeepFolder { get; init; } -> bool` — Keep the temp server folder after `KilnHarness.Dispose` (also: environment `KILN_TESTKIT_KEEP=1`).
- `LogSink { get; init; } -> Action<TestKit.LogLine>?` — Gets every captured log line as it is written (any thread), e.g. to show it in a test's output.
- `ServerDirectory { get; init; } -> string?` — Run in an isolated test folder instead of a new temp one, e.g. the folder of an earlier harness that kept it...
- `Side { get; init; } -> TestKit.HarnessSide` `0.2.10` — Which game process the harness stands for, for the mods' Kiln tasks: where their hops can go (the server world, the...
- `StopTimeout { get; init; } -> TimeSpan` — How long Dispose may spend stopping the server before the watchdog stops the blocked callback and abandons the harness.
- `TempDirectory { get; init; } -> string?` — Where the temp server folder is made; default the system's temp folder.
- `TestTimeout { get; init; } -> TimeSpan` — The timeout of one mod's `[KilnTest]` run (kiln.json `mods.test_timeout_s`).
- `ThrowOnModFault { get; init; } -> bool` — A harness call during which a mod faulted throws `ModFaultException` (default).
- `WaitForOtherHarness { get; init; } -> TimeSpan` — How long a new harness waits for the one still alive in this process (harnesses run one at a time: Kiln's state is...
- `WaitForWorldRunning { get; init; } -> bool` — False (default): like a server without the game-log mirror, `ServerStartedEvent` fires at the first step (source...
- `WallDeltaMs { get; init; } -> double` — Wall-clock milliseconds one step stands for (`TickInfo.WallDeltaMs`); 40.8 is a dedicated server's 24.5 steps a second.
- `WorldId { get; init; } -> Guid?` — The id of the world the server runs (`KilnHarness.World`); default the world the server folder ran last, or a new one.

## TestKit.KilnTestOptions `0.2.1` — How `KilnHarness.RunTest` judges and guards one `[KilnTest]`.
- `AllowUnavailable { get; init; } -> IReadOnlyCollection<string>` — Unavailable names (`kiln.spawn`, a game feature) a test may ask for and still count: for tests whose subject is the...
- `static Default { get; } -> TestKit.KilnTestOptions` — The defaults.
- `SetupTimeout { get; init; } -> TimeSpan` — How long `KilnTestSession`'s setup (and the server start) may block the tick thread (default 60 s).
- `WatchdogGrace { get; init; } -> TimeSpan` — How much longer than the test's timeout one harness step may take before the watchdog stops the tick thread (default...

## TestKit.KilnTestResult `0.2.1` — One `[KilnTest]` as the harness ran it.
- `KilnTestResult(string Name, Testing.TestOutcome Outcome, TimeSpan Duration, string? Message, string? Stack, IReadOnlyList<string> Unavailable)`
- `Duration { get; init; } -> TimeSpan` — How long it ran.
- `Log { get; init; } -> IReadOnlyList<TestKit.LogLine>` — Every log line written while it ran.
- `Message { get; init; } -> string?` — Why it failed, was skipped or timed out; for an offline skip also what the body did.
- `Name { get; init; } -> string` — The test's name (`attr.Name ?? Class.Method`).
- `Notes { get; init; } -> IReadOnlyList<string>` — What the kit did around the test (a fresh harness after a stopped test, faults of other code).
- `Outcome { get; init; } -> Testing.TestOutcome` — The kit's verdict: Kiln.Testing's, except that a test that asked for something the kit does not fake is...
- `Runner { get; init; } -> Testing.TestResult?` — What Kiln.Testing's runner reported for the test; null when the watchdog stopped it, or the runner never reported.
- `Stack { get; init; } -> string?` — Where it failed (the assertion's frames), or for a stopped test the tick thread's stack where it blocked.
- `Unavailable { get; init; } -> IReadOnlyList<string>` — The unavailable names (services, features) asked while it ran, by the test's mod or a mod it depends on, without the...

## TestKit.KilnTestSession `0.2.1` — Runs a mod's `[KilnTest]` methods one by one on one harness and one load, as a server runs them, for a test framework's adapter (`Kiln.TestKit.Xunit.KilnTestSuite` is...
- `KilnTestSession(Func<TestKit.KilnHarness, TestKit.ModHandle> setup, Func<TestKit.KilnHarnessOptions>? harnessOptions = null, TestKit.KilnTestOptions? options = null)` — A session that runs a mod's `[KilnTest]` methods on harnesses it makes, one after another.
- `Dispose() -> void` — Disposes the current harness.
- `DisposeError { get; } -> string?` — What the last dispose of a harness reported, if it did not stop cleanly.
- `Harness { get; } -> TestKit.KilnHarness?` — The harness the next test runs on, if one is made (null before the first `KilnTestSession.Run`).
- `HarnessCount { get; } -> int` — How many harnesses the session made (more than one after a stopped test).
- `Run(string name) -> TestKit.KilnTestResult` — Runs the test named `name`.

## TestKit.LogLine `0.2.1` — One line Kiln wrote to its log while a harness ran: a mod's `Logger`, Kiln's own code, or the host (the loader and the lifecycle).
- `LogLine(long Sequence, ulong Step, LogLevel Level, string Source, string Text)`
- `Level { get; init; } -> LogLevel` — The level.
- `ModId { get; } -> string?` — The mod that wrote it, or null for Kiln and the host.
- `Sequence { get; init; } -> long` — Order of writing, from 1.
- `Source { get; init; } -> string` — `mod:<id>`, `kiln` (Kiln.Api and the services) or `host` (Kiln.Host).
- `Step { get; init; } -> ulong` — The harness's step when it was written (0 before the first).
- `Text { get; init; } -> string` — The text.

## TestKit.ModFaultException `0.2.1` — A mod faulted (an exception Kiln's fault policy counted) during a harness call.
- `Faults { get; } -> IReadOnlyList<string>` — Each fault as Kiln logged it (mod, where, the exception with its stack).

## TestKit.ModHandle `0.2.1` — A test's handle on one mod of a `KilnHarness`: what a test keeps in a local.
- `Context { get; } -> ModContext` — The running instance's context (loaded or disabled).
- `Error { get; } -> string?` — Why the mod is not loaded or was disabled; null otherwise.
- `Folder { get; } -> string` — The mod's folder, `kiln/mods/<folder>` in the harness's temp server folder.
- `Id { get; } -> string` — The mod id.
- `Info { get; } -> ModInfo` — The manifest's metadata, once the loader has read it.
- `Instance { get; } -> Mod` — The running instance (loaded or disabled).
- `IsLoaded { get; } -> bool` — `ModHandle.State` is Loaded.
- `State { get; } -> ModState` — What the mod list says: `ModState.Pending` while it waits for the server's first step, Loaded, Disabled (fault...

## TestKit.ModLeakException `0.2.1` — After an unload, a mod's load context was still alive: something outside the mod holds its code or objects, and in the game every reload of it would leak.
- `ModIds { get; } -> IReadOnlyList<string>` — The mods whose load context is still alive.

## TestKit.ModLoadException `0.2.1` — A mod did not load: the loader refused it (manifest, resolver, a missing dependency or feature) or its `OnLoad` threw.
- `ModId { get; } -> string` — The mod that did not load (several, comma-separated, when more failed at the first step).
- `Reason { get; } -> string` — The loader's reason, followed by what the capability rows say about any feature it named.
- `State { get; } -> ModState` — Failed or Skipped, as the mod list says.

## TestKit.ModManifest `0.2.1` — A `kiln.mod.json` built in code, for `KilnHarness.Load`.
- `ModManifest(string id)` — A manifest built in code for a mod loaded by the harness.
- `Advanced { get; set; } -> bool` — The `advanced` tier flag.
- `ApiVersion { get; set; } -> string` — The Kiln API the mod was built for; default this Kiln's (`Mods.RuntimeApiVersion`).
- `DependsOn(string id, string versionRange = "*", bool optional = false) -> TestKit.ModManifest` — Adds a dependency (`"kiln"` is Kiln itself).
- `Exports(params string[] files) -> TestKit.ModManifest` — Adds exported assemblies (`exports`, file names such as `MyApi.dll`).
- `Id { get; } -> string` — The mod id.
- `LocalesDirectory { get; set; } -> string?` — A folder whose files become the mod's `locales/` (not written to the manifest).
- `Name { get; set; } -> string?` — Display name; default the id.
- `Requires(params string[] features) -> TestKit.ModManifest` — Adds required features (`features`): the resolver refuses the mod while one is unavailable.
- `Set(string key, Json.Nodes.JsonNode? value) -> TestKit.ModManifest` — Any other manifest key (`tests`, `loadAfter`, `incompatible`...), as JSON.
- `Side { get; set; } -> ModSide` — Server (default), client or both.
- `SteamMessages { get; set; } -> bool` `0.2.13` — The `steamMessages` opt-in: the mod opens its own Steam peer message leases (Kiln.Network's `SteamMessages`).
- `ToJson(string entryAssembly, string? entryType) -> string` — The manifest as the mod's `kiln.mod.json` would hold it.
- `Version { get; set; } -> string` — The mod's version (semantic versioning); default 1.0.0.
- `Writes(params string[] components) -> TestKit.ModManifest` `0.2.2` — The component types the mod writes through the game's write tracking (`"writes"`: short names such as `Fuel`): what...

## TestKit.Xunit.KilnSuiteLifetime `0.2.1` — The class fixture of every `KilnTestSuite`: when xunit has run the suite's tests it disposes this, and the suite's harness goes with it.
- `Dispose() -> void` — Disposes the suite's harness.

## TestKit.Xunit.KilnTestOrderer `0.2.1` — Orders a `KilnTestSuite`'s tests as Kiln.Testing's runner does, whatever subset xunit was asked to run; other tests of the class come after, in xunit's default order.
- `OrderTestCases<TTestCase>(IReadOnlyCollection<TTestCase> testCases) -> IReadOnlyCollection<TTestCase>` — The suite's `[KilnTest]` cases in the in-game runner's order, then the class's other cases.

## `TestKit.Xunit.KilnTestSuite<TMod>` `0.2.1` — Every `[KilnTest]` method of `TMod`'s assembly as an xunit test, run under a `KilnHarness` through Kiln.Testing's own runner: the same test body runs offline under...
- `virtual AllowUnavailable { get; } -> IReadOnlyCollection<string>` — Unavailable names the tests may ask for and still count (tests of the unavailable path).
- `virtual HarnessOptions() -> TestKit.KilnHarnessOptions` — The options of each harness the suite makes.
- `KilnTest(string name) -> void` — One `[KilnTest]`, by name.
- `static KilnTests { get; } -> IEnumerable<Xunit.TheoryDataRow<string>>` — The `[KilnTest]` names of `TMod`'s assembly, for xunit's discovery.
- `virtual LoadMod(TestKit.KilnHarness kiln) -> TestKit.ModHandle` — Loads the mod whose tests run (default `kiln.Load<TMod>()`, its own manifest file); override for a `ModManifest`...
- `virtual Setup(TestKit.KilnHarness kiln) -> void` — Before the mod loads, on each new harness: other mods it needs (a test mod's mod under test), fake players, config...
- `virtual WatchdogGrace { get; } -> TimeSpan` — How much longer than its timeout a test may block the tick thread before the watchdog stops it.
