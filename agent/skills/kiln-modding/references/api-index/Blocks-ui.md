# Blocks (Kiln.Blocks): ui

## ButtonRow — A button that runs an action.
- `Does { get; init; } -> UiAction` — What a click does.
- `Label { get; init; } -> UiText` — The button's label.

## ChoiceRow — A drop-down on an enum config entry, with its options.
- `Bind { get; init; } -> string` — The enum config entry it shows and writes: `config.<path>`.
- `Label { get; init; } -> UiText` — The drop-down's label.

## HudLineBlock — A line of text on the game's screen: `"FPS {game.fps:0}"`.
- `Anchor { get; init; } -> HudAnchor` — Where on the screen the line sits.
- `Color { get; init; } -> string?` — "#rrggbb", "#rrggbbaa", or white, black, gray, red, green, yellow, orange, cyan.
- `Comment { init; }` — A note in a data file, ignored.
- `Refresh { get; init; } -> double` — Seconds between updates: 0.1 to 3600.
- `Text { get; init; } -> UiText` — The text, with `{game.fps:0}` and `{config.clock.scale:0.00}` for values.
- `When { get; init; } -> string?` — Shown only while this is true; see `UiRow.When`.

## ImageRow `0.2.16` — A PNG picture of the mod's folder (since 0.2.0-dev.31), loaded once when the block is defined: at most 8 MiB, 2048 x 2048 pixels, and 8 pictures / 32 MiB of pixels...
- `Asset { get; init; } -> string` — A path in the mod's folder ("ui/map.png"): relative, no "..", no URL.
- `Does { get; init; } -> UiAction?` — What a click on the picture does.
- `Height { get; init; } -> float` — Pixels at UI scale 1; 0: the picture's own height.
- `Id { get; init; } -> string` — The row's id, unique in the window.
- `Width { get; init; } -> float` — Pixels at UI scale 1; 0: the picture's own width.

## InputRow `0.2.16` — A text field (since 0.2.0-dev.31) on a text config entry or a string state entry.
- `Bind { get; init; } -> string` — `config.<path>` of a text entry, or `state.<key>` of a string state entry.
- `Commit { get; init; } -> UiInputCommit`
- `Id { get; init; } -> string` — The row's id, unique in the window: `[a-z][a-z0-9_-]{0,63}`.
- `Label { get; init; } -> UiText`
- `MaxBytes { get; init; } -> int` — At most this many UTF-8 bytes (2..4096); a longer text is cut at a character before anything is written.
- `Multiline { get; init; } -> bool` — Several lines (Enter is a new line; the text is committed when the field loses the keyboard).

## IUiBlock — A block a player's game draws or binds with Kiln.Ui: `WindowBlock`, `HudLineBlock`, `KeyBindingBlock`.

## KeyBindingBlock — A key that runs an action.
- `Category { get; init; } -> string?` — The group in the Keys tab.
- `Comment { init; }` — A note in a data file, ignored.
- `Description { get; init; } -> UiText?` — What the Keys tab says.
- `Does { get; init; } -> UiAction` — What the key does.
- `Key { get; init; } -> string` — The default key: "F8", "Ctrl+K", "Shift+Alt+F2", "Numpad5", or "None" to bind it only once the player chooses.
- `OnGameScreens { get; init; } -> bool` — A key that types something (a letter, a digit) fires in play only; true fires on every game screen too, chat included.
- `Was { get; init; } -> string?` — The name this block had before a rename: the player's rebinding follows it.
- `WhileCapturing { get; init; } -> bool` — Also fire while the Kiln menu is open.

## SeparatorRow — A line, with a heading when it has text.
- `Text { get; init; } -> UiText?` — The heading.

## SliderRow — A slider on a whole-number or decimal config entry.
- `Bind { get; init; } -> string` — The config entry it shows and writes: `config.<path>`.
- `Label { get; init; } -> UiText` — The slider's label.
- `Max { get; init; } -> double?` — The upper end of the slider.
- `Min { get; init; } -> double?` — Default: the entry's own range, which it must have when this and `SliderRow.Max` are left out.

## TableRow `0.2.16` — A table (since 0.2.0-dev.31) of the rows of a data source: `world.local.inventory` or a mod's own `mod.<id>` rows.
- `Columns { get; init; } -> IReadOnlyList<UiColumn>` — 1 to 16 columns.
- `Id { get; init; } -> string` — The row's id, unique in the window.
- `Items { get; init; } -> string` — The rows: `world.local.inventory` or `mod.<id>` of a rows source.
- `MaxRows { get; init; } -> int` — At most this many rows (1..512); a source with more shows a notice instead of a cut list.
- `Selected { get; init; } -> string?` — `state.<key>` of a string entry that holds the selected row's key ("" = none).

## TabsRow `0.2.16` — Tabs (since 0.2.0-dev.31): each tab shows its own rows.
- `Id { get; init; } -> string` — The row's id, unique in the window.
- `Selected { get; init; } -> string?` — `state.<key>` of a string entry that holds the selected tab's id.
- `Tabs { get; init; } -> IReadOnlyList<UiTab>` — 1 to 16 tabs with ids unique in the row.

## TextRow — A line of text.
- `Color { get; init; } -> string?` — "#rrggbb", "#rrggbbaa", or white, black, gray, red, green, yellow, orange, cyan.
- `Text { get; init; } -> UiText` — The text.
- `Wrap { get; init; } -> bool` — Break long text at the window's edge.

## ToggleRow — A checkbox on a true/false config entry.
- `Bind { get; init; } -> string` — The true/false config entry it shows and writes: `config.<path>`.
- `Label { get; init; } -> UiText` — The checkbox's label.

## TreeRow `0.2.16` — A tree (since 0.2.0-dev.31): nodes from a mod's own `mod.<id>` tree source (`TreeRow.Nodes`) or written here (`TreeRow.Roots`, with `TreeRow.Fields` for their values).
- `Expanded { get; init; } -> string?` — `state.<key>` of a stringSet entry that holds the open branches' keys.
- `Fields { get; init; } -> IReadOnlyList<UiField>?` — The fields of `TreeRow.Roots`' values (with a source, the source's schema says them).
- `Id { get; init; } -> string` — The row's id, unique in the window.
- `NodeRows { get; init; } -> IReadOnlyList<UiRow>?` — Up to 8 text or value rows drawn under an open branch's label and under each leaf.
- `Nodes { get; init; } -> string?` — `mod.<id>` of a tree source.
- `Roots { get; init; } -> IReadOnlyList<UiTreeNodeDefinition>?` — The tree written in the block.
- `Selected { get; init; } -> string?` — `state.<key>` of a string entry that holds the selected node's key.

## UiAction — What a button or a key does: exactly one of these.
- `ApplyEffect { get; init; } -> EffectRef?` — Applies an effect to the player, through a hidden ability Kiln makes for it (`ui-apply.<name>`).
- `CastAbility { get; init; } -> AbilityRef?` — Asks the server to cast an ability of this mod (or `"other-mod:name"`), as its key would: its costs and cooldown...
- `CloseWindow { get; init; } -> WindowRef?` — Closes a window block of this mod, or `"other-mod:name"`.
- `Command { get; init; } -> string?` — A command line of this game's command registry, as the console runs it ("reload", "/mymod-reset 5"): as the host's...
- `CommandTarget { get; init; } -> UiCommandTarget?` `0.2.16` — Where `UiAction.Command` runs: `UiCommandTarget.Local` (null, the default) as above, or `UiCommandTarget.Server`: on...
- `OpenWindow { get; init; } -> WindowRef?` — Opens a window block of this mod, or `"other-mod:name"`.
- `Set { get; init; } -> UiSet?` — Sets a config entry to a value.
- `Toast { get; init; } -> UiText?` — A short message in the corner of the screen.
- `Toggle { get; init; } -> string?` — Flips a true/false config entry: `"config.clock.show"`.
- `ToggleWindow { get; init; } -> WindowRef?` `0.2.16` — Opens the window when it is closed and closes it when it is open (since 0.2.0-dev.31): one key for both.

## UiBindingValueChangedEvent `0.2.16` — A data source's publication was committed.
- `UiBindingValueChangedEvent(string mod, string id, ulong oldVersion, ulong newVersion)`
- `Id { get; } -> string`
- `Mod { get; } -> string`
- `NewVersion { get; } -> ulong`
- `OldVersion { get; } -> ulong`

## UiBoolState `0.2.16` — A true/false state entry.
- `UiBoolState(bool Initial = false)`
- `Initial { get; init; } -> bool`

## UiColumn `0.2.16` — A column of a `TableRow`: its id, heading and cell, a template that reads the row's fields as `{row.<field>}` (and any other value).
- `UiColumn(string Id, UiText Label, UiText Cell)`
- `Cell { get; init; } -> UiText`
- `Id { get; init; } -> string`
- `Label { get; init; } -> UiText`

## UiControlState `0.2.16` — What an unbound tab bar or tree of a window holds: the selected tab or node, the expanded branches.
- `UiControlState(string? Selected, IReadOnlyList<string> Expanded)`
- `Expanded { get; init; } -> IReadOnlyList<string>`
- `Selected { get; init; } -> string?`

## UiControlStateChangedEvent `0.2.16` — An unbound tab bar or tree of a window changed what it holds (the row's id in `UiControlStateChangedEvent.RowId`).
- `UiControlStateChangedEvent(WindowRef window, string rowId, UiControlState? before, UiControlState after, UiStateVersion version, UiStateChangeCause cause)`
- `After { get; } -> UiControlState`
- `Before { get; } -> UiControlState?`
- `Cause { get; } -> UiStateChangeCause`
- `RowId { get; } -> string`
- `Version { get; } -> UiStateVersion`
- `Window { get; } -> WindowRef`

## UiDataPublishResult `0.2.16` — What `Bindings.Publish` returned: the source's version after the publication was committed, or why not (`UiStateReasons.Superseded` when a newer one replaced it first).
- `UiDataPublishResult(bool Ok, string? Reason, ulong Version)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `Version { get; init; } -> ulong`

## UiDataRegistration `0.2.16` — A data source of a mod (`Bindings.Define`).
- `Dispose() -> void` — Takes the source away; windows bound to it show it as unavailable.
- `Id { get; } -> string` — The source's id: windows bind it as `mod.<id>`.
- `Ok { get; } -> bool`
- `Reason { get; } -> string?`

## UiDataRow `0.2.16` — One row of a rows source: a unique key (`[A-Za-z0-9._:-]{1,128}`) and its field values.
- `UiDataRow(string Key, IReadOnlyDictionary<string, UiScalar> Values)`
- `Key { get; init; } -> string`
- `Values { get; init; } -> IReadOnlyDictionary<string, UiScalar>`

## UiDataSchema `0.2.16` — A data source's shape: a scalar of `UiDataSchema.ScalarType`, or rows / tree nodes with up to 16 `UiDataSchema.Fields` (each required in every row or node).
- `UiDataSchema(UiDataShape Shape, UiScalarType? ScalarType = null, IReadOnlyList<UiField>? Fields = null)`
- `Fields { get; init; } -> IReadOnlyList<UiField>?`
- `ScalarType { get; init; } -> UiScalarType?`
- `Shape { get; init; } -> UiDataShape`

## UiDataShape `0.2.16` — What shape a data source has.
enum: Scalar, Rows, Tree

## UiDataSnapshot `0.2.16` — A publication of a data source (`Bindings.Publish`): copied at once, so the caller may change its own collections once Publish has returned.
- `UiDataSnapshot(UiDataSnapshot original)`

## UiDataSnapshot.Rows `0.2.16`
- `Rows(IReadOnlyList<UiDataRow> Values)`
- `Values { get; init; } -> IReadOnlyList<UiDataRow>`

## UiDataSnapshot.Scalar `0.2.16`
- `Scalar(UiScalar Value)`
- `Value { get; init; } -> UiScalar`

## UiDataSnapshot.Tree `0.2.16`
- `Tree(IReadOnlyList<UiTreeDataNode> Values)`
- `Values { get; init; } -> IReadOnlyList<UiTreeDataNode>`

## UiDecimalState `0.2.16` — A decimal state entry in [`UiDecimalState.Min`, `UiDecimalState.Max`] with at most `UiDecimalState.Scale` digits after the point (0..6); a value with more is refused...
- `UiDecimalState(decimal Initial, decimal Min = -79228162514264337593543950335, decimal Max = 79228162514264337593543950335, int Scale = 6)`
- `Initial { get; init; } -> decimal`
- `Max { get; init; } -> decimal`
- `Min { get; init; } -> decimal`
- `Scale { get; init; } -> int`

## UiDemandRegistration `0.2.16` — A request that the world collector gather some groups (`BlockUi.DemandWorld`).
- `Dispose() -> void` — Stops asking.
- `Handle { get; } -> ulong` — The collector's handle; 0 when refused.
- `Ok { get; } -> bool`
- `Reason { get; } -> string?`

## UiField `0.2.16` — A data field: its id (as `row.<id>` / `node.<id>` in templates) and type.
- `UiField(string Id, UiScalarType Type)`
- `Id { get; init; } -> string`
- `Type { get; init; } -> UiScalarType`

## UiInputCommit `0.2.16` — When a text field writes what was typed: when it loses the keyboard (and on Enter), or on Enter only.
enum: Blur, Enter

## UiInt64State `0.2.16` — A whole-number state entry in [`UiInt64State.Min`, `UiInt64State.Max`].
- `UiInt64State(long Initial, long Min = -9223372036854775808, long Max = 9223372036854775807)`
- `Initial { get; init; } -> long`
- `Max { get; init; } -> long`
- `Min { get; init; } -> long`

## UiInventoryValue `0.2.16` — One nonempty slot of the local player's inventory.
- `UiInventoryValue(string Key, uint Holder, uint InventoryPage, byte SlotIndex, uint Item, uint Count, uint Pide)`
- `Count { get; init; } -> uint`
- `Holder { get; init; } -> uint`
- `InventoryPage { get; init; } -> uint`
- `Item { get; init; } -> uint`
- `Key { get; init; } -> string`
- `Pide { get; init; } -> uint`
- `SlotIndex { get; init; } -> byte`

## UiMenu — An entry in the game's own menus that opens a window.
- `In { get; init; } -> UiMenuIn` — Which of the game's menus holds the entry.
- `Order { get; init; } -> int` — Lower first.
- `Text { get; init; } -> UiText` — The label.

## UiMenuIn — Which of the game's menus a window's entry goes into.
enum: Pause, Main, Both

## UiRow — One line of a window.
- `UiRow(UiRow original)`
- `SameLine { get; init; } -> bool` — On the same line as the row before it.
- `Tooltip { get; init; } -> UiText?` — Shown while the pointer rests on the row's widget.
- `When { get; init; } -> string?` — Shown only while this is true: `"game.inGame"`, `"!game.inGame"`, `"config.clock.show"`, `"game.screen == InGame"`...

## UiScalar `0.2.16` — One typed value of a data field or a scalar source.
- `UiScalar(UiScalar original)`
- `abstract Type { get; } -> UiScalarType` — The type of the value.

## UiScalar.Bool `0.2.16`
- `Bool(bool Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> bool`

## UiScalar.Decimal `0.2.16`
- `Decimal(decimal Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> decimal`

## UiScalar.Float64 `0.2.16` — A finite double (NaN and infinities are refused).
- `Float64(double Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> double`

## UiScalar.Int64 `0.2.16`
- `Int64(long Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> long`

## UiScalar.String `0.2.16`
- `String(string Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> string`

## UiScalar.UInt64 `0.2.16`
- `UInt64(ulong Value)`
- `override Type { get; } -> UiScalarType`
- `Value { get; init; } -> ulong`

## UiScalarType `0.2.16` — The types a data field or scalar has.
enum: Bool, Int64, UInt64, Decimal, Float64, String

## UiSet — A config entry and the value to give it.
- `Bind { get; init; } -> string` — The config entry to set: `config.<path>`.
- `To { get; init; } -> object` — A true/false, a number or a text, which the entry takes: a number in its range, an option of an enum.

## UiSnapshotStamp `0.2.16` — When a part of a `UiWorldSnapshot` was taken: the world's session and step, and the capture time in milliseconds (Environment.TickCount64).
- `UiSnapshotStamp(ulong Session, ulong Step, ulong CaptureMs)`
- `CaptureMs { get; init; } -> ulong`
- `Session { get; init; } -> ulong`
- `Step { get; init; } -> ulong`

## UiStateChangeCause `0.2.16` — Why a window's state changed.
enum: Widget, Action, Api, SelectionInvalidated, Pruned

## UiStateDefinition `0.2.16` — One entry of a window's state (`WindowBlock.State`): a typed value the window's rows bind as `state.<key>`.
- `UiStateDefinition(UiStateDefinition original)`

## UiStateReadResult `0.2.16` — What `BlockUi.State` returned.
- `UiStateReadResult(bool Ok, string? Reason, UiStateSnapshot? Snapshot)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `Snapshot { get; init; } -> UiStateSnapshot?`

## UiStateReasons `0.2.16` — Why a state or data call was refused (`IKilnResult.Reason` starts with one of these).
- `const AccessDenied = "AccessDenied" -> string` — The window or data source belongs to another mod.
- `const Budget = "Budget" -> string` — The write would pass a size limit of the window or the mod.
- `const StaleSession = "StaleSession" -> string` — The world changed (the state was reset) after the write was asked for.
- `const StaleSource = "StaleSource" -> string` — The data a widget's edit was made on changed before the edit could be written.
- `const StaleVersion = "StaleVersion" -> string` — The expected version is not the current one.
- `const Superseded = "Superseded" -> string` — A newer publication of the same data source replaced this one before it was committed.
- `const UnknownKey = "UnknownKey" -> string` — The window declares no such state entry, or no such data source was defined.
- `const UnknownSelection = "UnknownSelection" -> string` — A selection names a row, node or tab that the window does not show now.
- `const UnknownWindow = "UnknownWindow" -> string` — No such window here (not defined, its mod not loaded, or this is not a game with an overlay).
- `const Unloaded = "Unloaded" -> string` — The window or the mod went away before the write ran.

## UiStateResult `0.2.16` — What `BlockUi.SetState` returned: the snapshot after the write (a write of the same value is accepted and changes nothing), or why it was refused (`UiStateReasons`).
- `UiStateResult(bool Ok, string? Reason, UiStateSnapshot? Snapshot)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `Snapshot { get; init; } -> UiStateSnapshot?`

## UiStateSnapshot `0.2.16` — A window's whole state at one version: the declared entries and the tab bars' and trees' own state.
- `UiStateSnapshot(WindowRef Window, UiStateVersion Version, IReadOnlyDictionary<string, UiStateValue> Values, IReadOnlyDictionary<string, UiControlState> Controls)`
- `Controls { get; init; } -> IReadOnlyDictionary<string, UiControlState>`
- `Values { get; init; } -> IReadOnlyDictionary<string, UiStateValue>`
- `Version { get; init; } -> UiStateVersion`
- `Window { get; init; } -> WindowRef`

## UiStateValue `0.2.16` — A state value, of one of the types a `UiStateDefinition` declares.
- `UiStateValue(UiStateValue original)`

## UiStateValue.Bool `0.2.16` — A true/false.
- `Bool(bool Value)`
- `Value { get; init; } -> bool`

## UiStateValue.Decimal `0.2.16` — A decimal.
- `Decimal(decimal Value)`
- `Value { get; init; } -> decimal`

## UiStateValue.Int64 `0.2.16` — A whole number.
- `Int64(long Value)`
- `Value { get; init; } -> long`

## UiStateValue.String `0.2.16` — A text.
- `String(string Value)`
- `Value { get; init; } -> string`

## UiStateValue.StringSet `0.2.16` — A set of texts; stored unique and in ordinal order.
- `StringSet(IReadOnlyList<string> Value)`
- `Value { get; init; } -> IReadOnlyList<string>`

## UiStateVersion `0.2.16` — Which state a write expects to change: the world session's token and the window's revision.
- `UiStateVersion()`
- `UiStateVersion(ulong Session, ulong Revision)`
- `Revision { get; init; } -> ulong`
- `Session { get; init; } -> ulong`

## UiStatImportRegistration `0.2.16` — A declared dependency on another mod's stat for UI blocks (`Blocks.ImportStat`): it names the stat's type for the blocks that show it; whether this game may read it...
- `Dispose() -> void` — Forgets the import (blocks compiled with it keep it).
- `Key { get; } -> ModValueKey` — The imported stat.
- `Ok { get; } -> bool`
- `Reason { get; } -> string?`

## UiStringSetState `0.2.16` — A set of texts (unique, kept in ordinal order): what a tree's expanded branches bind to.
- `UiStringSetState(IReadOnlyList<string> Initial, int MaxItems = 512, int MaxItemBytes = 128)`
- `Initial { get; init; } -> IReadOnlyList<string>`
- `MaxItemBytes { get; init; } -> int`
- `MaxItems { get; init; } -> int`

## UiStringState `0.2.16` — A text state entry of at most `UiStringState.MaxBytes` UTF-8 bytes (up to 4096).
- `UiStringState(string Initial = "", int MaxBytes = 256)`
- `Initial { get; init; } -> string`
- `MaxBytes { get; init; } -> int`

## UiTab `0.2.16` — A tab of a `TabsRow`.
- `UiTab(string Id, UiText Label, IReadOnlyList<UiRow> Rows, string? When = null)`
- `Id { get; init; } -> string`
- `Label { get; init; } -> UiText`
- `Rows { get; init; } -> IReadOnlyList<UiRow>`
- `When { get; init; } -> string?`

## UiTreeDataNode `0.2.16` — One node of a tree source: a key unique in the whole tree, its parent's key (null at a root), its label and field values.
- `UiTreeDataNode(string Key, string? ParentKey, UiText Label, IReadOnlyDictionary<string, UiScalar> Values)`
- `Key { get; init; } -> string`
- `Label { get; init; } -> UiText`
- `ParentKey { get; init; } -> string?`
- `Values { get; init; } -> IReadOnlyDictionary<string, UiScalar>`

## UiTreeNodeDefinition `0.2.16` — A node of a tree written in the block (`TreeRow.Roots`).
- `UiTreeNodeDefinition(string Key, UiText Label, IReadOnlyList<UiTreeNodeDefinition>? Children = null, IReadOnlyDictionary<string, UiScalar>? Values = null)`
- `Children { get; init; } -> IReadOnlyList<UiTreeNodeDefinition>?`
- `Key { get; init; } -> string`
- `Label { get; init; } -> UiText`
- `Values { get; init; } -> IReadOnlyDictionary<string, UiScalar>?`

## UiWindowStateChangedEvent `0.2.16` — A window's declared state entry changed (one event per changed entry, all at the new version).
- `UiWindowStateChangedEvent(WindowRef window, string key, UiStateValue? before, UiStateValue? after, UiStateVersion version, UiStateChangeCause cause)`
- `After { get; } -> UiStateValue?`
- `Before { get; } -> UiStateValue?`
- `Cause { get; } -> UiStateChangeCause`
- `Key { get; } -> string`
- `Version { get; } -> UiStateVersion`
- `Window { get; } -> WindowRef`

## UiWindowStateResetEvent `0.2.16` — A window's state went back to its initial values with a new session token: a new world.
- `UiWindowStateResetEvent(WindowRef window, UiStateVersion oldVersion, UiStateVersion newVersion)`
- `NewVersion { get; } -> UiStateVersion`
- `OldVersion { get; } -> UiStateVersion`
- `Window { get; } -> WindowRef`

## UiWorldFields `0.2.16` — The groups of live game values a window or a HUD line can ask for.
enum: None, LocalVitals, LocalPosition, LocalInventory, HostTime

## UiWorldReadResult `0.2.16` — What `BlockUi.ReadWorld` returned: a snapshot, or why there is none now (no world, no sample yet, expired).
- `UiWorldReadResult(bool Ok, string? Reason, UiWorldSnapshot? Snapshot)`
- `Ok { get; init; } -> bool`
- `Reason { get; init; } -> string?`
- `Snapshot { get; init; } -> UiWorldSnapshot?`

## UiWorldSnapshot `0.2.16` — One publication of live game values.
- `UiWorldSnapshot(ulong Publication, uint AvailableMask, UiSnapshotStamp Local, UiSnapshotStamp Server, uint LocalPlayer, uint Health, uint HealthMax, uint Stamina, uint StaminaMax, uint Mana, uint ManaMax, long XFixed, long YFixed, long ZFixed, decimal ServerHours, IReadOnlyList<UiInventoryValue> Inventory, IReadOnlyList<string?> UnavailableReasons)`
- `AvailableMask { get; init; } -> uint`
- `Has(UiWorldFields fields) -> bool` — Whether every group in `fields` is in this publication.
- `Health { get; init; } -> uint`
- `HealthMax { get; init; } -> uint`
- `Inventory { get; init; } -> IReadOnlyList<UiInventoryValue>`
- `Local { get; init; } -> UiSnapshotStamp`
- `LocalPlayer { get; init; } -> uint`
- `Mana { get; init; } -> uint`
- `ManaMax { get; init; } -> uint`
- `Publication { get; init; } -> ulong`
- `Server { get; init; } -> UiSnapshotStamp`
- `ServerHours { get; init; } -> decimal`
- `Stamina { get; init; } -> uint`
- `StaminaMax { get; init; } -> uint`
- `UnavailableReasons { get; init; } -> IReadOnlyList<string?>`
- `XFixed { get; init; } -> long`
- `YFixed { get; init; } -> long`
- `ZFixed { get; init; } -> long`

## ValueRow — A label and a value that cannot be edited here: `Label: value`.
- `Bind { get; init; } -> string` — `game.<name>` or `config.<path>` (`config.<config id>/<path>` when two configs have the path).
- `Format { get; init; } -> string?` — How a number or a true/false shows: `0.00`, `yes|no`.
- `Label { get; init; } -> UiText` — The label before the value.

## WindowBlock — A window or a screen of rows: text, values, buttons, toggles, sliders, choices and separators.
- `Alpha { get; init; } -> float?` — A window: how opaque the background is, 0..1.
- `AutoResize { get; init; } -> bool` — A window: it grows and shrinks with its rows.
- `Comment { init; }` — A note in a data file, ignored.
- `Dim { get; init; } -> bool?` — A screen: darken the game behind it.
- `Height { get; init; } -> float` — Pixels at UI scale 1.
- `Interactive { get; init; } -> bool` — A window: false gives a panel the player only looks at (no mouse and keyboard taken from the game).
- `Kind { get; init; } -> WindowKind` — A window (the default) or a full screen.
- `Menu { get; init; } -> UiMenu?` — An entry in the game's pause menu and/or main menu that opens it.
- `OpenAtStart { get; init; } -> bool` — Open as soon as the mod's game has it.
- `Position { get; init; } -> float[]?` — A window: where its centre is, [x, y] as 0..1 of the screen.
- `Refresh { get; init; } -> double` — Seconds between updates of the values and conditions it shows: 0.1 to 3600.
- `Rows { get; init; } -> IReadOnlyList<UiRow>` — The rows, from top to bottom.
- `State { get; init; } -> IReadOnlyDictionary<string, UiStateDefinition>?` `0.2.16` — The window's own typed state, by key (`[a-z][a-z0-9_-]{0,63}`): what its rows bind as `state.<key>` (since...
- `Title { get; init; } -> UiText` — The window's title: a text, or one per language.
- `TitleBar { get; init; } -> bool` — The title bar shows.
- `Width { get; init; } -> float` — Pixels at UI scale 1.

## WindowKind — What a window is.
enum: Window, Screen

## WindowRef — A window: what `Blocks.Define` returned, its name in this mod, or `"other-mod:name"`.
- `WindowRef()`
- `WindowRef(string Value)`
- `static implicit operator WindowRef(BlockHandle<WindowBlock> handle) -> WindowRef` — A window by its handle, as `Blocks.Define` returned it.
- `static implicit operator WindowRef(string value) -> WindowRef` — A window by its name in this mod, or `"other-mod:name"`.
- `Value { get; init; } -> string` — The window's name in this mod, or `"other-mod:name"`.
