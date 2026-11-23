# Ui (Kiln.Ui): part 1 of 2

## AmmoListContext `0.2.15` — What a list is built for now: handed to a provider of `AmmoList.Provide`, which is asked again whenever any of it changes (`AmmoListContext.Revision`).
- `GameLists(ItemId item) -> bool` — Whether `item` is among `AmmoListContext.GameRows`, the game's rows as last built.
- `GameRows { get; } -> IReadOnlyList<AmmoListRow>` — The game's own rows, in its order, with their counts, as the game last built them (up to 64; a picture, not proof...
- `Held { get; } -> ItemId` — The weapon the list is built for (the bow, the staff); default when the game did not say.
- `Kind { get; } -> AmmoListKind` — The list (one kind).
- `Player { get; } -> EntityId` — The local player.
- `Revision { get; } -> ulong` — Changes whenever the held weapon, the game's own rows or counts, or the session change.
- `Session { get; } -> uint` — The client world session the list belongs to (`Client.ClientWorldSession`).
- `Truncated { get; } -> bool` — Kiln reported that rows were cut at 64 (the game's own list was longer, so `AmmoListContext.GameRows` are its first...

## AmmoListKind `0.2.15` — Which of the HUD's lists (`AmmoList`).
enum: Arrow, Spell

## AmmoListMode `0.2.15` — How a provider's rows go into the game's list.
enum: Append, Replace

## AmmoListOptions `0.2.15` — How `AmmoList.Provide` composes a provider's rows.
- `Mode { get; init; } -> AmmoListMode` — After the game's rows (default) or instead of them.

## AmmoListPick `0.2.15` — The player picked a mod's row the game cannot equip itself (the item is not in the backpack): the game did nothing, so the mod's server half can do the swap.
- `AmmoListPick(AmmoListKind Kind, ItemId Item, int Count)`
- `Count { get; init; } -> int` — The count the row showed (a picture, not inventory authority).
- `Item { get; init; } -> ItemId` — The item of the row the mod provided.
- `Kind { get; init; } -> AmmoListKind` — The list the row was picked in: `AmmoListKind.Arrow` or `AmmoListKind.Spell`.

## AmmoListProvider `0.2.15` — A provider of `AmmoList.Provide`: dispose it to take its rows out (the game's own list shows again once no provider is left).
- `Active { get; } -> bool` — False once disposed or its mod unloaded.
- `Dispose() -> void` — Takes the rows out at the next UI frame.
- `Kinds { get; } -> AmmoListKind` — The lists it provides rows for.
- `Mode { get; } -> AmmoListMode` — Its mode.
- `Refresh() -> void` — Asks for the rows again at the next UI frame: for rows that depend on the mod's own data (what its server sent)...

## AmmoListRow `0.2.15` — A row of an ammunition or spell list: the item and the count shown next to it (a picture: the count is not inventory).
- `AmmoListRow()`
- `AmmoListRow(ItemId Item, int Count)`
- `Count { get; init; } -> int` — The number shown next to it, a picture, not inventory; a negative count shows as 0.
- `Item { get; init; } -> ItemId` — The item the row shows.

## Color — An RGBA colour.
- `Color()`
- `Color(byte R, byte G, byte B, byte A = 255)`
- `A { get; init; } -> byte` — Alpha (opacity), 0 to 255.
- `B { get; init; } -> byte` — Blue, 0 to 255.
- `static readonly Black -> Color` — Black.
- `static readonly Cyan -> Color` — A light cyan.
- `Deconstruct(out byte R, out byte G, out byte B, out byte A) -> void`
- `Equals(Color other) -> bool`
- `static FromRgba(uint rgba) -> Color` — The colour from a value packed as `0xRRGGBBAA` (`Color.Rgba`).
- `G { get; init; } -> byte` — Green, 0 to 255.
- `static readonly Gray -> Color` — A light grey.
- `static readonly Green -> Color` — A soft green.
- `static operator !=(Color left, Color right) -> bool`
- `static operator ==(Color left, Color right) -> bool`
- `static readonly Orange -> Color` — An orange.
- `R { get; init; } -> byte` — Red, 0 to 255.
- `static readonly Red -> Color` — A soft red.
- `Rgba { get; } -> uint` — The colour packed as `0xRRGGBBAA`.
- `static readonly White -> Color` — White.
- `WithAlpha(byte a) -> Color` — The same colour with alpha `a` (0 to 255).
- `static readonly Yellow -> Color` — A soft yellow.

## CraftingMenuAddition — A tab or subcategory Kiln added to the game's crafting menu.
- `Dispose() -> void` — Takes it out of the menu (the menu shows the change when it next rebuilds its lists: reopen it).
- `IsTab { get; } -> bool` — True for a tab, false for a subcategory.
- `Workshop { get; } -> uint` — The `keen::WorkshopId` of the station whose menu it was added to.

## CraftingPanelAnchor — Where a crafting panel sits.
enum: Right, Left, Bottom

## CraftingPanelOptions — Options of a crafting panel.
- `Anchor { get; init; } -> CraftingPanelAnchor` — Where the panel sits.
- `AtFactories { get; init; } -> bool` `0.2.7` — Also show at a production station's screen (a kiln, smelter, loom, seed bed: `CraftingScreen.IsFactory`), with its...
- `Interactive { get; init; } -> bool` — Buttons in the panel take the mouse while it is over the panel.
- `Style { get; init; } -> UiStyle` — The panel's look; `UiStyle.Player` by default (the game's look unless the player chose another; before...
- `Theme { get; init; } -> UiTheme?` `0.2.8` — Your own theme for the panel (`Themes.Define`), over `CraftingPanelOptions.Style`.
- `When { get; init; } -> Func<CraftingScreen, bool>?` — Show only when this returns true (e.g. one workshop); null = every crafting menu.
- `Width { get; init; } -> float` — Width in pixels at scale 1 (height fits the contents).

## CraftingScreen — A crafting menu that is open, or (since 0.2.0-dev.29.15) a production station's screen (`CraftingScreen.IsFactory`).
- `CraftingScreen()`
- `CraftingScreen(bool AtStation, uint Workshop, uint Recipe, bool Journal)`
- `AtStation { get; init; } -> bool` — A crafting station's menu (workbench, forge, an NPC's, a kiln's...); false for the player's own.
- `IsFactory { get; init; } -> bool` `0.2.7` — A production station's screen (keen::HudScreenType FactoryCrafting: a kiln, smelter, loom, seed bed...): the game...
- `Journal { get; init; } -> bool` — The journal's recipe book.
- `Recipe { get; init; } -> uint` — The recipe id selected in the menu, 0 = none (always 0 on a factory screen).
- `Station { get; init; } -> EntityId` `0.2.7` — On a factory screen, the station's entity: the entity the game's screen is about (play data +0x2c50).
- `Workshop { get; init; } -> uint` — keen::WorkshopId of the menu (the station's kind, e.g. the Workbench 0x4da88f73; 0 = the player's own crafting).

## GameLanguage — The game's text language (keen::LanguageId, from the game's reflection; docs/re/content.md 8.1).
- `static Current { get; } -> string` — "de-de", "en-us", ...; "en-us" when unknown (dedicated server, the game's source strings, an older build).
- `static KeyOf(uint languageId) -> string` — The key of a keen::LanguageId value.

## GameScreen — The game screen of the last UI frame.
- `GameScreen()`
- `GameScreen(GameUiState State, HudScreen Hud, ScreenKind Kind, uint StationWorkshop, CraftingScreen? Crafting, uint LanguageId, ulong Frame)`
- `Crafting { get; init; } -> CraftingScreen?` — The crafting menu that is open (or a production station's screen); null when none is.
- `Frame { get; init; } -> ulong` — Game UI frames since start, at the frame this was read.
- `Hud { get; init; } -> HudScreen` — The screen open in game: meaningful while `State` is `GameUiState.Play`, otherwise `HudScreen.Invalid`.
- `InGame { get; } -> bool` — True while a world is played: `GameScreen.State` is `GameUiState.Play`, whatever screen is open.
- `IsMainMenu { get; } -> bool` — True on the main menu (`ScreenKind.MainMenu`).
- `IsPaused { get; } -> bool` — True in the pause menu (`ScreenKind.PauseMenu`).
- `Kind { get; init; } -> ScreenKind` — A summary of `State` and `Hud`.
- `Language { get; } -> string` — The game's text language key ("de-de", ...).
- `LanguageId { get; init; } -> uint` — The game's `keen::LanguageId` of its text language; 0 when unknown.
- `static readonly None -> GameScreen` — No screen known: no UI state, no HUD screen, no crafting menu.
- `State { get; init; } -> GameUiState` — The game's top-level UI state.
- `StationWorkshop { get; init; } -> uint` — The game's `keen::WorkshopId` of the crafting station the player uses; 0 when none.
- `Target { get; init; } -> EntityId` `0.2.12` — The entity the open screen is about, on the screens the game draws as a chest-style transfer screen: a storage...

## GameUiState — keen::UiState: the game's top-level UI state.
enum: None, Preload, PreGame, Load, LoreIntro, LoreOutro, Credits, Play

## GemForgeCandidate `0.2.5` — One returned GEMS row.
- `GemForgeCandidate(uint Index, UiItemView SourceRow, UiItemView DisplayedGem, UiItemView? WeaponCarrier)`
- `DisplayedGem { get; init; } -> UiItemView` — The gem the row displays (for a socketed gem, the socket's gem).
- `Index { get; init; } -> uint` — The row's index in the list the game returned.
- `SourceRow { get; init; } -> UiItemView` — The row as the selection key sees it.
- `WeaponCarrier { get; init; } -> UiItemView?` — The weapon the gem is socketed in; null for a gem that is not socketed.

## GemForgeCandidatesResult `0.2.5` — Unavailable when the GEMS grid did not draw, changed during capture, expired, or could not be copied.
- `GemForgeCandidatesResult(bool Ok, string? Reason, GemForgeCandidatesSnapshot? Snapshot)`
- `Ok { get; init; } -> bool` — True when a list was captured.
- `Reason { get; init; } -> string?` — Why not; null when `GemForgeCandidatesResult.Ok`.
- `Snapshot { get; init; } -> GemForgeCandidatesSnapshot?` — The captured list; null when not `GemForgeCandidatesResult.Ok`.

## GemForgeCandidatesSnapshot `0.2.5` — The game's returned GEMS list for the local player's inventories, including offscreen rows.
- `GemForgeCandidatesSnapshot(ulong Session, ulong Frame, ulong CaptureMs, ulong Generation, EntityId LocalPlayer, byte Mode, SlotId? SelectedRow, UiItemView? SelectedWeapon, IReadOnlyList<GemForgeCandidate> Candidates)`
- `Candidates { get; init; } -> IReadOnlyList<GemForgeCandidate>` — The rows of the GEMS list, including offscreen ones.
- `CaptureMs { get; init; } -> ulong` — When it was captured, on the `Environment.TickCount64` clock.
- `Frame { get; init; } -> ulong` — The game UI frame of the capture.
- `Generation { get; init; } -> ulong` — Changes when the list or its context changes.
- `LocalPlayer { get; init; } -> EntityId` — The local player's entity.
- `Mode { get; init; } -> byte` — The game's mode byte (0 to 4).
- `SelectedRow { get; init; } -> SlotId?` — The slot of the selected row; null when none is selected.
- `SelectedWeapon { get; init; } -> UiItemView?` — The selected weapon; null when none is selected.
- `Session { get; init; } -> ulong` — The client session of the capture.

## Hud — Text on the game's screen and short pop-up messages (Forge: GUI overlays, toasts).
- `static Remove(HudLine line) -> void` — Removes `line` from the screen.
- `static Remove(string id) -> void` — Removes the line with `id`, if there is one.
- `static Show(string id, Func<string> text, HudAnchor anchor = HudAnchor.TopLeft, Color? color = null) -> HudLine` — A line whose text is computed every frame (render thread).
- `static Show(string id, string text = "", HudAnchor anchor = HudAnchor.TopLeft, Color? color = null) -> HudLine` — Shows (or returns the existing) line `id` at `anchor`.
- `static Toast(string text, ToastKind kind = ToastKind.Info, double seconds = 4) -> void` — A message in the corner of the screen for `seconds` (any thread).

## HudAnchor — Where HUD text sits.
enum: TopLeft, TopCenter, TopRight, MiddleLeft, Center, MiddleRight, BottomLeft, BottomCenter, BottomRight

## HudLine — A HUD line created by `Hud.Show`.
- `Anchor { get; set; } -> HudAnchor` — Where on the screen the line sits.
- `Color { get; set; } -> Color` — The text colour.
- `Dispose() -> void` — Removes the line from the screen (`Hud.Remove`).
- `Id { get; } -> string` — The line's id, as given to `Hud.Show`.
- `Provider { get; set; } -> Func<string>?` — When set, called every frame for the text instead of `HudLine.Text` (render thread).
- `Text { get; set; } -> string` — Text shown (any thread).

## HudScreen — keen::HudScreenType (reflected enum of client 26404966): the screen open in game.
enum: Invalid, InGame, SystemMenu, SettingsMenu, InGameMenu, Map, Journal, Character, PlayerCrafting, Backpack, SkillTree, SkillTreeRespec, FlameAltar, AltarUpgrade, FlameUpgrade, AltarDeactivate, AltarReskill, AltarItemPickup, LoreReading, Signpost, LogicEditor, ReadOnlyLootChest, StorageChest, Collection, FeedingStation, StationBasedCrafting, NpcDialog, NpcCrafting, FactoryCrafting, BlueprintMultiTool, BlueprintMultiToolMaterialCatalog, OvergrowthMultiTool, DecayMultiTool, ScrewdriverMultiToolBuildingMode, ScrewdriverMultiToolGraphManager, ScrewdriverMultiToolEntityConfig, TerraformingSideMenu, NpcSummonerMenu, UserFeedback, TwitchDrops, RedeemCommerceProducts, CommerceProductChest, ItemTinting, TextChat, Social, ShroudForge, CharacterCustomization, Rename, TakeScreenshot, InfoPopup, NewCommerceUnlock, NoUi, TechGallery, YcPlayground

## HudValue `0.2.3` — The HUD pairs a provider can replace.
enum: Health, Stamina, Mana

## InventoryPanelOptions `0.2.12` — Options of a panel beside the game's inventory screens (`Inventory.AddPanel`).
- `Interactive { get; init; } -> bool` — Buttons in the panel take the mouse while it is over the panel (the game does not see those clicks).
- `Screens { get; init; } -> InventoryScreens` — The screens it shows on (both by default).
- `Side { get; init; } -> InventoryPanelSide` — Where the panel sits beside the slots.
- `Style { get; init; } -> UiStyle` — The panel's look; `UiStyle.Player` by default (the game's look unless the player chose another).
- `Theme { get; init; } -> UiTheme?` — Your own theme for the panel (`Themes.Define`), over `InventoryPanelOptions.Style`.
- `When { get; init; } -> Func<InventoryScreen, bool>?` — Show only when this returns true (e.g. chests only once their entity is known); null = always.
- `Width { get; init; } -> float` — Width in pixels at scale 1 (height fits the contents).

## InventoryPanelSide `0.2.12` — Where a panel sits beside the game's slots.
enum: Right, Left, Below

## InventoryScreen `0.2.12` — One of the game's inventory screens, open now (`Inventory.Screen`).
- `InventoryScreen()`
- `InventoryScreen(InventoryScreenKind Kind, HudScreen Hud, EntityId Container)`
- `Container { get; init; } -> EntityId` — On a chest screen: the chest's entity (the game's play.target; for a chest the server sent, the server's id of it).
- `Hud { get; init; } -> HudScreen` — The game's screen (Backpack, InGameMenu, StorageChest, CommerceProductChest).
- `Kind { get; init; } -> InventoryScreenKind` — Backpack or chest.
- `static Of(GameScreen screen) -> InventoryScreen?` — The inventory screen of a game screen, or null (any other screen: the play view, a station, the map...).

## InventoryScreenChangedEvent `0.2.12` — Posted on Kiln's event bus when one of the game's inventory screens opens, closes or changes (another chest, the chest's entity named a frame late).
- `InventoryScreenChangedEvent(InventoryScreen? previous, InventoryScreen? current)`
- `Current { get; } -> InventoryScreen?` — The screen after the change; null when none is open.
- `Previous { get; } -> InventoryScreen?` — The screen before the change; null when none was open.

## InventoryScreenKind `0.2.12` — Which of the game's inventory screens is open.
enum: Backpack, Chest

## InventoryScreens `0.2.12` — The inventory screens a panel shows on.
enum: Backpack, Chest, All

## KeyBinding — One registered key binding (Forge: a KeyMapping).
- `Category { get; } -> string` — Groups bindings in the Kiln menu (default: the mod's name).
- `Consume { get; set; } -> bool` — Keep the key from the game when it matches (default true).
- `Default { get; } -> KeyChord` — The key the binding was registered with.
- `Description { get; } -> string` — What the key does, as the Keys page shows it.
- `Dispose() -> void` — Removes the binding.
- `Enabled { get; set; } -> bool` — False: the binding does not fire.
- `Id { get; } -> string` — The id the binding was registered under ("mymod.toggle").
- `IsDown { get; } -> bool` — Held right now (Forge: isDown()).
- `Key { get; } -> KeyChord` — The key in use (the player's choice from keybinds.json, else `KeyBinding.Default`).
- `OnGameScreens { get; set; } -> bool` — A key that types something (a letter, digit, punctuation or Space, without Ctrl or Alt) fires only during play or...
- `OnPress { get; set; } -> Action?` — Runs on key press (the game's main thread).
- `OnRelease { get; set; } -> Action?` — Runs on key release.
- `TakePresses() -> int` — Presses since the last call (Forge: consumeClick()); counts even without `KeyBinding.OnPress`.
- `WhileCapturing { get; set; } -> bool` — Also fire while the Kiln menu is open (default false).

## KeyBindings — Key bindings: register, remap, persist (`<configDir>/keybinds.json`: kiln/config or the profile's configDir).
- `static All { get; } -> IReadOnlyList<KeyBinding>` — Every registered binding.
- `static FilePath { get; } -> string?` — The path of keybinds.json (the profile's config folder when one is active, else kiln/config); null when it cannot be...
- `static Find(string id) -> KeyBinding?` — The binding with this id, or null.
- `static Rebind(string id, KeyChord key) -> bool` — Sets the key of `id` and saves keybinds.json, like `KeyBindings.Rebind` with a chord already parsed.
- `static Rebind(string id, string key) -> bool` — Sets the key of `id` and saves keybinds.json.
- `static Register(string id, string defaultKey, Action? onPress, string? description = null, string? category = null) -> KeyBinding` — Registers `id` ("mymod.toggle") with `defaultKey` ("F8", "Ctrl+K").
- `static Reset(string id) -> void` — Puts `id` back on its default key and saves keybinds.json.

## KeyChord — A key with modifiers, written like "F8", "Ctrl+K", "Shift+Alt+F2", "Insert", "Numpad5", "Backquote", "Key223", or a mouse input (since 0.2.0-dev.27): "Mouse4"...
- `KeyChord()`
- `KeyChord(uint VirtualKey, KeyMods Mods)`
- `IsNone { get; } -> bool` — True for `KeyChord.None`: no key.
- `IsPad { get; } -> bool` `0.2.14` — A game controller's button (with the pad's shoulder buttons or triggers held): "Pad:LB+A".
- `Mods { get; init; } -> KeyMods` — The modifiers held with it.
- `static readonly None -> KeyChord` — No key (the default value); written "None".
- `static Parse(string text) -> KeyChord` — Parses "Ctrl+Shift+F8" like `KeyChord.TryParse`.
- `static TryParse(string? text, out KeyChord chord) -> bool` — Parses "Ctrl+Shift+F8"; false for an unknown key name.
- `VirtualKey { get; init; } -> uint` — The Windows virtual-key code of the key (the mouse buttons and the wheel have codes too, described above); 0 for none.

## KeyInput `0.2.11` — Key and mouse presses as they come, for a mod that reads them itself (its own "press a key" field, a key it handles by state, a shortcut layer of its own).
- `static Listen(Func<KeyPress, bool> onKey, KeyListenFlags flags = KeyListenFlags.Mouse) -> KeyListener` — Calls `onKey` for every key press and release Kiln's overlay gets, and with `KeyListenFlags.Mouse` (the default) for...
- `static PressCode(uint virtualKey, uint scanCode) -> uint` — The code a binding stores for a press of `virtualKey` with `scanCode`, as the Kiln menu's Keys page saves it: a...

## KeyListener `0.2.11` — A listener made by `KeyInput.Listen`.
- `Dispose() -> void` — Stops the listener: once it returns, no new press reaches it.
- `Mouse { get; } -> bool` — It gets the mouse inputs of `KeyListenFlags.Mouse` as well.
- `Ok { get; } -> bool` — True when the listener gets presses.
- `Pad { get; } -> bool` `0.2.14` — It gets a game controller's buttons (`KeyListenFlags.Pad`) as well.
- `Reason { get; } -> string?` — Why it gets none (no Kiln overlay here: a dedicated server, or kiln-ui.dll did not load); null when `KeyListener.Ok`.

## KeyListenFlags `0.2.11` — What a key listener gets besides keyboard keys.
enum: None, Mouse, Pad `0.2.14`

## KeyMods — Modifier keys of a binding.
enum: None, Ctrl, Shift, Alt, PadLB `0.2.14`, PadRB `0.2.14`, PadLT `0.2.14`, PadRT `0.2.14`

## KeyPress `0.2.11` — One press or release of a key or a mouse input, as Kiln's overlay got it from the game's window (before the game sees it).
- `KeyPress()`
- `KeyPress(uint VirtualKey, uint ScanCode, KeyMods Mods, bool Down, bool Repeat, bool TextInput, bool Captured)`
- `Captured { get; init; } -> bool` — Kiln's overlay has the input now (the Kiln menu or a Kiln screen is open).
- `Chord { get; } -> KeyChord` — The press as the Kiln menu's Keys page saves it: `KeyPress.Code` with `KeyPress.Mods`.
- `Code { get; } -> uint` — The code a binding stores for this press (`KeyInput.PressCode`): a punctuation key is the key at that place of a US...
- `Down { get; init; } -> bool` — A press (true) or a release.
- `IsMouse { get; } -> bool` — A mouse input: the middle or a side button, or a notch of the wheel.
- `IsPad { get; } -> bool` `0.2.14` — A game controller's button (`KeyListenFlags.Pad`).
- `Mods { get; init; } -> KeyMods` — Ctrl, Shift and Alt held at the time.
- `Repeat { get; init; } -> bool` — The auto-repeat of a held key.
- `ScanCode { get; init; } -> uint` — The key's scan code (0x100 set for an extended key); 0 for the mouse and for a key sent without one.
- `TextInput { get; init; } -> bool` — A Kiln text field has the keyboard (the player is typing in Kiln's overlay).
- `VirtualKey { get; init; } -> uint` — The Windows virtual-key code; for the mouse (`KeyListenFlags.Mouse`) 0x04 middle, 0x05 Mouse4, 0x06 Mouse5, and...

## MarkerShape — Marker shapes for `WorldDraw.Marker`.
enum: Diamond, Circle, Square, Triangle, Cross

## MenuEntry — An entry in one of the game's menus, drawn by the game's own entry renderer (its look and sounds, mouse, keyboard and gamepad navigation), right above the menu's...
- `Attention { get; set; } -> bool` — The game's attention marker next to the label (pause menu only).
- `Dispose() -> void` — Takes the entry out of the menu (also done when the owning mod unloads).
- `Location { get; } -> MenuLocation` — The menu the entry is in.
- `OnClick { get; set; } -> Action` — Runs on the game's UI thread after the frame in which the player chose the entry, as the owning mod.
- `Order { get; } -> int` — Where the entry sits: lower first (see `Menus.Add`).
- `Text { get; set; } -> UiText` — The label (any thread; per language).
- `Visible { get; set; } -> bool` — False hides the entry from the menu without removing it.

## MenuLocation — Where an entry goes.
enum: MainMenu, PauseMenu

## ModStatus — One mod's status lines, as `StatusLines.All` reads them.
- `ModStatus(string ModId, string Name, string Version, IReadOnlyList<string> Lines)`
- `Lines { get; init; } -> IReadOnlyList<string>` — The mod's status lines, in the order it added them.
- `ModId { get; init; } -> string` — The mod's id.
- `Name { get; init; } -> string` — The mod's name.
- `Version { get; init; } -> string` — The mod's version.

## OutlineOptions `0.2.13` — How `World.Outline` outlines an entity.
- `Color { get; init; } -> Color` — The outline's colour.
- `Duration { get; init; } -> TimeSpan?` — How long the outline lasts; null (the default) = until you dispose it, your mod unloads, the entity despawns or the...
- `Priority { get; init; } -> int` — Among mods' outlines on the same entity the highest priority shows; on a tie the newest.
- `Pulse { get; init; } -> bool` — The colour breathes (a 1.2 s period, 40 % to 100 % brightness).
- `ThroughWalls { get; init; } -> bool` `0.2.14` — Also seen through walls (default false; client only, like the outline).

## OutlineReason `0.2.13` — Why an outline waits or ended (`WorldOutline.Reason`).
enum: None, Pending, Outshone, NoModel, SwitchedOff, Removed, Despawned, WorldEnded, Expired, ModUnloaded, Failed

## OutlineState `0.2.13` — Where an outline stands (`WorldOutline.State`; `WorldOutline.Reason` says why).
enum: Waiting, Shown, GameHighlight, Ended

## Render — Render events, the game's camera and frame diagnostics (client).
- `static Available { get; } -> bool` — Render events and drawing work in this process (a game client with Kiln's overlay).
- `static Camera { get; } -> RenderCamera` — The latest camera.
- `static CameraAvailable { get; } -> bool` — The game's camera is found on this build (world drawing works).
- `static FieldOfViewIsHorizontal { get; set; } -> bool` — Calibration: read the camera's field of view as horizontal instead of vertical.
- `static HandlerBudgetMs { get; set; } -> double` — Milliseconds a `RenderFrameEvent` handler may take per frame (0.1 to 100, default 2).
- `static Stats { get; } -> RenderStats` — Frame diagnostics.

## RenderCamera — The game's camera: where it is, where it looks, its lens.
- `RenderCamera()` — A camera for tests and tools.
- `RenderCamera(WorldPosition position, Quaternion rotation, float fovY, float near = 0.1, float distance = 0)` — A camera for tests and tools.
- `AgeMs { get; } -> uint` — Milliseconds since the game last wrote the camera.
- `Aspect { get; } -> float` — The game's own aspect value (projection uses the screen's width / height).
- `Distance { get; } -> float` — Distance of the third-person camera from what it looks at (the character), metres.
- `Far { get; } -> float` — The far plane's distance, in metres.
- `Forward { get; } -> Vector3` — The view direction (unit vector, world space).
- `FovY { get; } -> float` — Vertical field of view in radians.
- `Frame { get; } -> ulong` — Camera writes since the game started (one per client frame).
- `Heading { get; } -> double` — Compass heading of the view in degrees: 0 north (+z), 90 east (+x), 180 south, 270 west.
- `Near { get; } -> float` — The near plane's distance, in metres.
- `Pitch { get; } -> double` — Degrees above the horizon (negative: looking down).
- `Pivot { get; } -> WorldPosition` — What the camera looks at: `RenderCamera.Position` + `RenderCamera.Forward` x `RenderCamera.Distance` (about the...
- `Position { get; } -> WorldPosition` — Where the camera is, in world metres.
- `Project(WorldPosition p, float width, float height, out Vector2 screen, out float depth) -> bool` — Pixels of a world point on a `width` x `height` screen.
- `Right { get; } -> Vector3` — The camera's right direction (unit vector, world space).
- `Rotation { get; } -> Quaternion` — Turns camera space into world space.
- `ToCameraSpace(WorldPosition p) -> (double X, double Y, double Z)` — A world point in camera space (x right, y up, z forward; metres).
- `Up { get; } -> Vector3` — The camera's up direction (unit vector, world space).
- `Valid { get; } -> bool` — A camera was written in the last half second (in game).

## RenderDraw — Immediate-mode drawing in pixels (origin top left, y down) for a `RenderFrameEvent` handler.
- `Arrow(Vector2 tip, float angle, float size, Color color) -> void` — A filled arrow head at `tip` pointing along `angle` (radians, 0 = right, pi/2 = down), `size` pixels long.
- `Circle(float x, float y, float radius, Color color, float thickness = 1, int segments = 0) -> void` — A circle outline; `segments` 0 = automatic.
- `FillCircle(float x, float y, float radius, Color color, int segments = 0) -> void` — A filled circle; `segments` 0 = automatic.
- `FillPolygon(ReadOnlySpan<Vector2> points, Color color) -> void` — A filled convex polygon (3..4096 points, clockwise on screen).
- `FillRect(float x0, float y0, float x1, float y1, Color color, float rounding = 0) -> void` — A filled rectangle from (x0, y0) to (x1, y1).
- `FillTriangle(Vector2 a, Vector2 b, Vector2 c, Color color) -> void` — A filled triangle through three points.
- `Image(UiImage image, float x0, float y0, float x1, float y1, Color? tint = null) -> void` — An image (`Ui.LoadImage`, a PNG in your mod, or an item's icon from `Ui.ItemImage`) stretched over the rectangle.
- `ImageCentered(UiImage image, float x, float y, float width = 0, float height = 0, Color? tint = null) -> void` — An image centred on (x, y), `width` x `height` pixels (0 = its own size).
- `ImagePart(UiImage image, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color? tint = null) -> void` `0.2.8` — Part of an image over the rectangle (since 0.2.0-dev.29.16): (`u0`, `v0`) to (`u1`, `v1`) in texture coordinates...
- `ImageTrimmed(UiImage image, float x0, float y0, float x1, float y1, Color? tint = null) -> void` `0.2.8` — An image over the rectangle like `RenderDraw.Image`, but only its art (since 0.2.0-dev.29.16): the transparent...
- `Line(Vector2 a, Vector2 b, Color color, float thickness = 1) -> void` — A line from `a` to `b` in pixels.
- `Line(float x0, float y0, float x1, float y1, Color color, float thickness = 1) -> void` — A line from (x0, y0) to (x1, y1) in pixels.
- `MeasureText(string text, float size = 0) -> (float Width, float Height)` — The size in pixels of `text` at `size` (0 = the overlay's font size); (0, 0) outside the handler.
- `Polyline(ReadOnlySpan<Vector2> points, Color color, float thickness = 1, bool closed = false) -> void` — Connected lines through `points` (2..4096).
- `Rect(float x0, float y0, float x1, float y1, Color color, float thickness = 1, float rounding = 0) -> void` — A rectangle outline from (x0, y0) to (x1, y1).
- `Text(float x, float y, string text, Color? color = null, float size = 0, TextAlign align = TextAlign.Left, TextStyle style = TextStyle.Shadow, bool middle = false) -> void` — Text at (x, y): the top left corner, the top centre (`TextAlign.Center`) or the top right.
- `Triangle(Vector2 a, Vector2 b, Vector2 c, Color color, float thickness = 1) -> void` — A triangle outline through three points.

## RenderFrameEvent — Posted on the Kiln event bus once per presented frame of the game client, while anything subscribes to it (Forge: `RenderGuiEvent` / `RenderLevelStageEvent`).
- `Camera { get; } -> RenderCamera` — The camera the game renders with this frame (`RenderCamera.Valid` expires 500 ms after the last write, or is false...
- `Capturing { get; } -> bool` — A Kiln screen or window has the mouse and keyboard.
- `DeltaSeconds { get; } -> float` — Seconds since the previous frame (at most 0.25).
- `Draw { get; } -> RenderDraw` — Drawing in pixels, under Kiln's windows and over the game's picture.
- `Fps { get; } -> float` — Frames per second, smoothed over about half a second.
- `Frame { get; } -> ulong` — Overlay frames since the game started.
- `Height { get; } -> float` — Screen height in pixels (`RenderFrameEvent.Width` is the width).
- `MenuOpen { get; } -> bool` — The Kiln menu is open.
- `Scale { get; } -> float` — UI scale (DPI); multiply your own pixel sizes by it.
- `Screen { get; } -> GameScreen` — The game's screen (main menu, in game, map, ...).
- `Width { get; } -> float` — Screen size in pixels.
- `World { get; } -> WorldDraw` — Drawing at world positions with `RenderFrameEvent.Camera`.

## RenderStats — `Render.Stats`: handlers, frames posted, the time all handlers took (last frame and the slowest), handlers turned off, and the camera's state.
- `RenderStats()`
- `RenderStats(int Handlers, long Frames, double LastFrameMs, double MaxFrameMs, int Disabled, bool Camera, bool CameraValid, long CameraFrames)`
- `Camera { get; init; } -> bool` — The game's camera is found on this build (`Render.CameraAvailable`).
- `CameraFrames { get; init; } -> long` — Camera writes seen since the game started.
- `CameraValid { get; init; } -> bool` — A camera was written lately (`RenderCamera.Valid`).
- `Disabled { get; init; } -> int` — How many handlers were turned off (over budget, stalls or exceptions).
- `Frames { get; init; } -> long` — Frames posted to them.
- `Handlers { get; init; } -> int` — How many `RenderFrameEvent` handlers are subscribed.
- `LastFrameMs { get; init; } -> double` — The time all handlers took in the last frame, in milliseconds.
- `MaxFrameMs { get; init; } -> double` — The longest time all handlers took in one frame, in milliseconds.

## ScreenChangedEvent — Posted on Kiln's event bus when the game screen changes (Forge: ScreenEvent.Opening / Closing).
- `ScreenChangedEvent(GameScreen previous, GameScreen current)`
- `Current { get; } -> GameScreen` — The screen after the change.
- `KindChanged { get; } -> bool` — The kind opened (`ScreenChangedEvent.Current`.Kind) when it differs from the previous one.
- `Previous { get; } -> GameScreen` — The screen before the change.

## ScreenKind — What kind of screen the player looks at (a summary of `GameUiState` and `HudScreen`).
enum: None, Title, MainMenu, Loading, Cinematic, Credits, InGame, PauseMenu, Settings, Inventory, Map, Journal, Character, Crafting, SkillTree, Storage, Dialog, Chat, Other

## SlotButton `0.2.12` — A mouse button on a slot.
enum: Left, Right, Pad `0.2.14`

## SlotClick `0.2.12` — A click on a slot the game drew.
- `SlotClick(SlotButton Button, SlotModifiers Modifiers, VisibleSlot Slot, GameScreen Screen, bool Taken)`
- `Button { get; init; } -> SlotButton` — The button that was pressed.
- `Holder { get; } -> EntityId` — The entity whose inventory holds the slot (the player, a chest), when the game's models said.
- `Item { get; } -> ItemId` — The item in the slot (`VisibleSlot.Item`); 0 for an empty slot.
- `ItemEntity { get; } -> EntityId` — The item entity (weapons, tools, armour, cans).
- `Level { get; } -> int` `0.2.14` — The item's level as the game drew the slot (`VisibleSlot.Level`).
- `Modifiers { get; init; } -> SlotModifiers` — The modifier keys held with it.
- `Pad { get; init; } -> KeyChord?` `0.2.14` — The controller chord pressed, for a `SlotClickOptions.Pad` click (`SlotButton.Pad`); null for a mouse click.
- `Pointer { get; init; } -> Vector2?` `0.2.13` — Where the button went down, in the overlay's pixels: the space `UiDraw` draws in and `UiDraw.Pointer` reports, with...
- `Rarity { get; } -> ItemRarity?` `0.2.14` — The item's rarity as the game drew the slot (`VisibleSlot.Rarity`).
- `Screen { get; init; } -> GameScreen` — The game screen the click was on.
- `Slot { get; init; } -> VisibleSlot` — The slot that was clicked, as the game drew it.
- `Taken { get; init; } -> bool` — True when Kiln kept the press from the game (`SlotClickOptions.Take`).

## SlotClickOptions `0.2.12` — Which clicks a handler gets (`Inventory.OnClick`).
- `Button { get; init; } -> SlotButton` — The mouse button.
- `Modifiers { get; init; } -> SlotModifiers` — Held exactly (None: a plain click).
- `Pad { get; init; } -> KeyChord?` `0.2.14` — A game controller's button or combination instead of a mouse click (`KeyChord.Parse("Pad:LS")`, `"Pad:LB+A"`...
- `Take { get; init; } -> bool` — The game never sees the press (no pick-up, no move).

## SlotMark `0.2.12` — A mark on a slot: drawn over the game's slot, in this player's game only.
- `SlotMark()`
- `SlotMark(Color Color, SlotMarkStyle Style = SlotMarkStyle.Border, float Thickness = 3)`
- `Color { get; init; } -> Color` — The mark's colour (use alpha for `SlotMarkStyle.Fill`).
- `Deconstruct(out Color Color, out SlotMarkStyle Style, out float Thickness) -> void`
- `Equals(SlotMark other) -> bool`
- `static operator !=(SlotMark left, SlotMark right) -> bool`
- `static operator ==(SlotMark left, SlotMark right) -> bool`
- `Style { get; init; } -> SlotMarkStyle` — How the mark is drawn.
- `Thickness { get; init; } -> float` — The line width of a border, and the gap of a corner square, in pixels at UI scale 1 (at least 1 pixel is drawn).

## SlotMarkStyle `0.2.12` — How a mark is drawn on a slot.
enum: Border, Fill, Corner

## SlotModifiers `0.2.12` — Modifier keys held with a click (exact: Ctrl alone is not Ctrl+Shift).
enum: None, Ctrl, Shift, Alt

## StatusLine — A status line of a mod (see `StatusLines`).
- `Dispose() -> void` — Removes the line.
- `Id { get; } -> string` — The line's name within its mod: giving the same one again replaces the line.
- `ModId { get; } -> string` — The mod that added it ("kiln" for Kiln's own).
- `Text { get; } -> IReadOnlyList<string>` — The lines this entry reports right now (a source runs now, on the calling thread).

## StatusLines — Persistent status lines of mods.
- `static Add(Func<string?> source, string? id = null) -> StatusLine` — Adds a line whose text `source` computes whenever someone looks.
- `static All() -> IReadOnlyList<ModStatus>` — Every mod that has lines, in load order, with the lines as of now.
- `static Lines(string modId) -> IReadOnlyList<string>` — The mod's lines now (in the order it added them); empty when it has none.
- `static Remove(string id) -> bool` — Removes the calling mod's line `id`; false when it has none.
- `static Set(string id, string? text) -> StatusLine?` — Sets the text of line `id` of the calling mod, creating it (it stays until changed).
- `static Summary() -> string` — The status as text, one block per mod: `mod-id 1.2.0` and its lines indented.
- `static WriteToLog(Logger? log = null) -> void` — Writes `StatusLines.Summary` into the log (one line per status line, tagged "status").

## TextAlign — Text alignment for `RenderDraw.Text`.
enum: Left, Center, Right

## TextStyle — Text look for `RenderDraw.Text`.
enum: Plain, Shadow, Outline

## ToastKind — The kind of a `Hud.Toast` message, which sets its look.
enum: Info, Success, Warning, Error

## TooltipColor `0.2.12` — A tooltip line's colour: the game's own palette (the rarity colours of the detail's header), a positive and a negative colour, and a muted one.
enum: Default, Positive, Negative, Common, Uncommon, Rare, Epic, Legendary, Unique, Muted

## TooltipLine `0.2.12` — One line in the game's item detail view: its text, colour, and whether it uses the game's smaller detail text size.
- `TooltipLine()`
- `TooltipLine(UiText Text, TooltipColor Color = TooltipColor.Default, bool Small = false)`
- `Color { get; init; } -> TooltipColor` — The line's colour.
- `Deconstruct(out UiText Text, out TooltipColor Color, out bool Small) -> void`
- `Equals(TooltipLine other) -> bool`
- `static operator !=(TooltipLine left, TooltipLine right) -> bool`
- `static operator ==(TooltipLine left, TooltipLine right) -> bool`
- `Small { get; init; } -> bool` — True to use the game's smaller detail text size.
- `Text { get; init; } -> UiText` — The line's text.

## TooltipPlace `0.2.12` — Where a tooltip line goes in the game's item detail view.
enum: End, AfterStats

## Ui — Kiln's overlay: windows, per-frame drawing, the Kiln menu; the game's screens (`Screens`), entries in its menus (`Menus`), crafting menus (`Crafting`) and full mod...
- `static AddConfigPage(string modId, Action<UiDraw> draw) -> IDisposable` — Adds a page to the Kiln menu's config tab for `modId` (the seam for the config registry: a config editor registers...
- `static Available { get; } -> bool` — The overlay exists in this process (a game client with a supported renderer).
- `static Frame -> Action<UiDraw>?` — Every overlay frame (render thread).
- `static ItemIconsAvailable { get; } -> bool` `0.2.7` — Whether `Ui.ItemImage` draws the game's icons here (a game client whose overlay draws, with this Kiln's kiln.ui and...
- `static ItemImage(ItemId item) -> UiImage?` `0.2.7` — The game's own icon of `item` as an image for `UiDraw.Image`, `UiDraw.ImageButton` and `RenderDraw.Image` (since...
- `static LoadFont(byte[] ttf) -> UiFontFile?` `0.2.8` — A TrueType or OpenType font (TTF or OTF) for your own windows (a theme's `UiThemeSpec.Font`, or `UiDraw.ModFont`).
- `static LoadFont(string path) -> UiFontFile?` `0.2.8` — A font file (absolute, or relative to the calling mod's folder); null when it is missing (see `Ui.LoadFont`).
- `static LoadImage(byte[] png) -> UiImage?` — A PNG (8-bit RGBA/RGB/grey, up to 4096x4096) as an image for Kiln windows; null when it is not a PNG or there is no...
- `static LoadImage(string path) -> UiImage?` — A PNG file (absolute, or relative to the calling mod's folder).
- `static MenuOpen { get; set; } -> bool` — The Kiln menu (mods, network, keys).
- `static OpenScreens { get; } -> IReadOnlyList<UiScreen>` — The screens open now, the top one last.
- `static OpenWheel(IReadOnlyList<string> entries, Action<int>? onResult = null, UiWheelOptions? options = null) -> UiWheel?` — Opens a radial wheel (`UiWheel`) with `entries`; `onResult` gets the chosen index or -1 (render thread).
- `static Screen(UiText title, Action<UiDraw> draw, UiScreenOptions? options = null) -> UiScreen` — A full screen drawn by `draw` every frame while open (closed at first).
- `static Status { get; } -> UiStatus` — Overlay diagnostics.
- `static Window(string title, Action<UiDraw> draw, UiWindowOptions? options = null) -> UiWindow` — Creates a window drawn by `draw` every frame while open.

## Ui.AmmoList `0.2.15` — The HUD's ammunition (arrows) and spell lists: rows a mod adds or puts in place of the game's, and picks of rows the game cannot equip itself (`AmmoList.Picked`).
- `static Available { get; } -> bool` — Whether `AmmoList.Provide` works on this game build (capability `ui.ammo.list`)...
- `const ListCapability = "ui.ammo.list" -> string` — The capability row of `AmmoList.Provide`: the game build's list builder is measured.
- `const PickCapability = "ui.ammo.pick" -> string` — The capability row of `AmmoList.Picked`: the build's pick of a list row is measured, so Kiln can keep the game from...
- `static Picked -> Action<AmmoListPick>?` — One of this mod's rows that the game cannot equip itself (the item is not in the backpack) was selected: the game...
- `static PicksAvailable { get; } -> bool` — Whether `AmmoList.Picked` works on this game build (capability `ui.ammo.pick`).
- `static Provide(AmmoListKind kinds, Func<AmmoListContext, IReadOnlyList<AmmoListRow>> rows, AmmoListOptions? options = null) -> AmmoListProvider` — Adds rows to the lists in `kinds` (or puts them in place of the game's rows, `AmmoListOptions.Mode`) until the...

## Ui.Crafting — The game's crafting menus: `Ui.Crafting.Current`, `AddPanel`, `AddTab`, `AddSubcategory`.
- `static AddPanel(UiText title, Action<UiDraw, CraftingScreen> draw, CraftingPanelOptions? options = null) -> IDisposable` — A panel drawn next to every open crafting menu (or those `CraftingPanelOptions.When` picks), with the menu's...
- `static AddSubcategory(uint workshop, int tab, UiText label, IEnumerable<uint> recipes) -> CraftingMenuAddition?` — A new subcategory with its own label at the end of tab `tab` (index, -1 = the last) of `workshop`'s crafting menu.
- `static AddTab(uint workshop, UiText label, IEnumerable<uint> recipes, int iconFromTab = -1) -> CraftingMenuAddition?` — A new tab in the crafting menu of `workshop` (keen::WorkshopId; 0 = the player's own crafting), with one subcategory...
- `static Current { get; } -> CraftingScreen?` — The crafting menu open now (station or own, workshop, selected recipe), or null.
- `static Describe(uint workshop = 0) -> string` — The menu configuration and Kiln's additions as JSON (0 = every workshop).
- `static MenuAvailable { get; } -> bool` — The menu extension works on this build (kiln.craftmenu: the client's kiln.content).

## Ui.Inventory `0.2.12` — The game's own inventory slots (backpack, chests, stations): marks and clicks; panels beside the backpack and chest screens, and which of them is open with the...
- `static AddChestButton(string caption, Action<InventoryScreen> handler) -> IDisposable` `0.2.16` — Adds a native game entry to the storage chest's action row, not a Kiln overlay panel.
- `static AddPanel(UiText title, Action<UiDraw, InventoryScreen> draw, InventoryPanelOptions? options = null) -> IDisposable` — A Kiln panel docked beside the game's backpack and chest screens while they are open (Kiln.Ui's widgets, the...
- `const ChestButtonCapability = "ui.chest.buttons" -> string` `0.2.16` — A game-drawn button in the storage chest's own action row.
- `static ClearMarks() -> void` — Removes every mark of your mod.
- `const ClickCapability = "ui.slots.click" -> string` — The capability row of `Inventory.OnClick`.
- `static Hovered { get; } -> VisibleSlot?` — The slot under the cursor in the game's latest UI frame (null: none, or not known).
- `static Mark(EntityId item, SlotMark mark) -> void` — Marks the item's slot wherever the game draws it (backpack, chest, hotbar) until `Inventory.Unmark` or your mod unloads.
- `const MarkCapability = "ui.slots.inspect" -> string` — The capability row of marks and `Inventory.Hovered` (the slot inspection).
- `static MarkWhen(Func<VisibleSlot, SlotMark?> rule) -> IDisposable` — A rule asked for each slot the game drew, every overlay frame (render thread: keep it a lookup). null = no mark.
- `static OnClick(SlotClickOptions options, Action<SlotClick> handler) -> IDisposable` — Calls `handler` (on the client's main thread, after the next UI frame) for each click matching `options` on a slot...
- `const PanelCapability = "ui.inventory.panels" -> string` — The capability row of `Inventory.AddPanel` (where the slots are drawn, the chest's entity).
- `static Screen { get; } -> InventoryScreen?` — The game's backpack or chest screen open now, or null (any thread).
- `static ScreenChanged -> Action<InventoryScreen?, InventoryScreen?>?` — (previous, current) on the game's UI thread right after the UI frame in which a backpack or chest screen opened...
- `static Selected { get; } -> VisibleSlot?` `0.2.14` — The slot the game's own selection is on in its latest UI frame, on a screen with a pointer (the backpack and...
- `const TintCapability = "ui.slots.tint" -> string` `0.2.16` — Native background colour rules for the game's own inventory slots.
- `static TintWhen(Func<UiItemView, Color?> rule, ItemId item = default(ItemId)) -> IDisposable` `0.2.16` — Changes the game's own slot background, not an overlay mark.
- `static Unmark(EntityId item) -> void` — Removes your mod's mark from the item's slots (see `Inventory.Mark`).

## Ui.Menus — Entries in the game's own menus (Forge: adding buttons in ScreenEvent.Init): `Ui.Menus.Add`.
- `static Add(MenuLocation where, UiText text, Action onClick, int order = 0) -> MenuEntry?` — Adds an entry to the main menu or the pause menu.
- `static AddToBoth(UiText text, Action onClick, int order = 0) -> (MenuEntry? Main, MenuEntry? Pause)` — Adds the same entry to the main menu and the pause menu.
- `static Available { get; } -> bool` — The game-menu entries work on this build (features ui.main_menu / ui.system_menu).
- `static KilnEntryVisible { set; }` — Kiln's own "Mods" entry (both menus; opens the Kiln menu).

## Ui.Screens — The game's screens (client): `Ui.Screens.Current`, `Ui.Screens.Changed`.
- `static Available { get; } -> bool` — The game-UI integration runs on this build (feature ui.screens).
- `static Changed -> Action<GameScreen, GameScreen>?` — (previous, current) on the game's UI thread right after the UI frame in which the screen changed, in the main menu too.
- `static Current { get; } -> GameScreen` — The screen kiln.ui reads from the game's UI (any thread).
- `static UiTick -> Action<GameScreen>?` — Every game UI frame on the game's UI thread (menus included).

## Ui.Themes `0.2.8` — Themes for mods' own windows, and the ones mods offer the player (docs/kiln/ui.md "Looks and themes").
- `static Define(string name, UiThemeSpec spec) -> UiTheme` — Defines (or, for a name this mod defined before, changes in place) a theme: the base look's palette with the spec's...
- `static Offered { get; } -> IReadOnlyList<UiTheme>` — The themes mods offer to the player now (Kiln's Look setting lists them).
- `static Player { get; } -> UiPlayerLook` — The player's look setting (kiln.json `ui`).

## Ui.Tooltip `0.2.12` — Lines and stat numbers inside the game's own item detail view, on this client.
- `static AddLines(ItemId item, UiTooltipLinesProvider provider, TooltipPlace place = TooltipPlace.End) -> UiValueRegistration` — Adds lines for `item` (default: every item) to the game's item detail view, at `place`, and to Kiln's "Item details"...
- `static AddLines(UiTooltipLinesProvider provider, TooltipPlace place = TooltipPlace.End) -> UiValueRegistration` — Adds lines for every item.
- `static AddValues(ItemId item, UiTooltipValuesProvider provider) -> UiValueRegistration` — Multiplies the stat numbers the game's item detail view draws for `item` (default: every item) by the provider's...
- `static AddValues(UiTooltipValuesProvider provider) -> UiValueRegistration` — Multiplies the stat numbers of every item.
- `static InGame { get; } -> bool` — True while lines are drawn into the game's item detail view (false: they show in Kiln's "Item details" window only...
- `const LinesCapability = "ui.tooltip.lines" -> string` — The row of drawing lines into the game's item detail view.
- `const ValuesCapability = "ui.tooltip.values" -> string` — The row of `Tooltip.AddValues`.

## Ui.Values `0.2.3` — Values in the game's own UI: replace what the game draws for an item's detail number and bar, a slot's bar, the held item's indicator and the local player's HUD pairs.
- `static CloseTooltipDetails() -> void` — Closes the details window.
- `const DetailCapability = "ui.values.detail" -> string` — The capability rows.
- `static GemForgeCandidates() -> GemForgeCandidatesResult` `0.2.5` — Copies the latest completed GEMS grid snapshot.
- `const GemForgeCandidatesCapability = "ui.gemforge.inspect" -> string` `0.2.5` — The read-only Gem Forge snapshot capability.
- `static HeldBar(UiBarProvider provider) -> UiValueRegistration` — Replaces the held item's indicator pair (raw fuel units; Max above 0).
- `const HeldCapability = "ui.values.held" -> string` — The capability rows.
- `static Hud(HudValue vital, UiBarProvider provider) -> UiValueRegistration` — Replaces one of the local player's HUD pairs (Current/Max in the vital's units; Max above 0).
- `const HudCapability = "ui.values.hud" -> string` — The capability rows.
- `static ItemBar(ItemId item, UiBarProvider provider) -> UiValueRegistration` — Replaces the number and bar of `item`'s detail view (`ItemId` 0: every item).
- `static ItemDetail(EntityId entity, ItemId item = default(ItemId), int? level = null, ItemRarity? rarity = null) -> UiItemDetailResult` `0.2.14` — Starts capture on first use and reads a completed panel for this CLIENT presentation entity (for example...
- `static ItemDetail(ItemId item, int? level = null, ItemRarity? rarity = null) -> UiItemDetailResult` `0.2.14` — Reads the most recently inspected matching variant in this client session, optionally constrained by level and rarity.
- `const ItemDetailCapability = "ui.item_detail" -> string` `0.2.14` — Passive capture of the game's item-detail text and computed numbers, not on-demand computation or drawing.
- `static OpenTooltipDetails() -> UiTooltipOpenResult` — Opens the details window on the last inspected item's lines (a frozen snapshot, refreshed only by its Refresh button).
- `static SlotBar(ItemId item, UiBarProvider provider) -> UiValueRegistration` — Replaces the bar on `item`'s inventory slots (0: every item).
- `const SlotCapability = "ui.values.slot" -> string` — The capability rows.
- `static Slots() -> VisibleSlotsResult` — The slots of the latest completed UI frame (a copy).
- `const SlotsCapability = "ui.slots.inspect" -> string` — The capability row of `Values.Slots` and `Values.SubscribeSlots`.
- `static SubscribeGemForgeCandidates(Action<GemForgeCandidatesResult> onFrame) -> UiValueRegistration` `0.2.5` — Calls onFrame after each UI frame on the client's main thread, including unavailable frames.
- `static SubscribeSlots(Action<VisibleSlotsResult> onFrame) -> UiValueRegistration` — Calls `onFrame` once per completed UI frame, on the game client's main thread right after the frame.
- `const TooltipCapability = "ui.values.tooltip_lines" -> string` — The capability row of the tooltip lines and the details window.
- `static TooltipLines(ItemId item, UiTooltipProvider provider) -> UiValueRegistration` — Adds lines for `item` (0: every item) to Kiln's "Item details" window.

## Ui.World `0.2.13` — The game's world as this player's client draws it: outlines on entities with the game's own highlight (`World.Outline`).
- `static ClearOutlines() -> void` — Takes every outline of your mod away (their handles end with `OutlineReason.Removed`).
- `static DefaultOutlineColor { get; } -> Color` — The colour of an outline whose options name none: gold, (255, 196, 64).
- `static Outline(EntityId entity, OutlineOptions? options = null) -> WorldOutline` — Outlines `entity`'s model with the game's own highlight, in this player's game, until you dispose the handle, your...
- `const OutlineCapability = "ui.world_outline" -> string` — The capability row of `World.Outline`: a sigdb feature of the game client (kiln.render's palette hooks); kiln.render...
- `static OutlinesAvailable { get; } -> bool` — Outlines work in this process: the row `World.OutlineCapability` is on and kiln.render has its outline functions (v2).
- `static ThroughWallsAvailable { get; } -> bool` `0.2.14` — Kiln draws through-walls silhouettes in this process: the row `World.ThroughWallsCapability` is on.
- `const ThroughWallsCapability = "ui.world_outline.through_walls" -> string` `0.2.14` — The capability row of `OutlineOptions.ThroughWalls`: `World.OutlineCapability` on, kiln.render v3 (the entity's...
