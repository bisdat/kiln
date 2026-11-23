# Ui (Kiln.Ui): part 2 of 2

## UiColorSlot — A colour of the current look (`UiDraw.PushColor`).
enum: Text, TextDisabled, WindowBg, ChildBg, Border, FrameBg, FrameBgHovered, FrameBgActive, Button, ButtonHovered, ButtonActive, Header, HeaderHovered, HeaderActive, Separator, ScrollbarBg, ScrollbarGrab, NavCursor, CheckMark, SliderGrab, TitleBg `0.2.8`, TitleBgActive `0.2.8`, PopupBg `0.2.8`, Tab `0.2.8`, TabHovered `0.2.8`, TabSelected `0.2.8`, TableHeaderBg `0.2.8`, TextSelectedBg `0.2.8`

## UiCursor — Where the next widget goes (`UiDraw.Cursor`).
- `UiCursor()`
- `UiCursor(float X, float Y, float ScreenX, float ScreenY, float AvailWidth, float AvailHeight, float WindowX, float WindowY, float WindowWidth, float WindowHeight, float FontSize, float LineHeight, float ScrollY, float ScrollMaxY)`
- `AvailHeight { get; init; } -> float` — The height left in the current window or child region, from the cursor.
- `AvailWidth { get; init; } -> float` — The width left in the current window or child region, from the cursor.
- `FontSize { get; init; } -> float` — The current font size in pixels, the UI scale applied.
- `LineHeight { get; init; } -> float` — The height of one text line plus the item spacing, in pixels.
- `ScreenX { get; init; } -> float` — The same position's x in screen pixels.
- `ScreenY { get; init; } -> float` — The same position's y in screen pixels.
- `ScrollMaxY { get; init; } -> float` — The largest vertical scroll position.
- `ScrollY { get; init; } -> float` — The vertical scroll position.
- `WindowHeight { get; init; } -> float` — The current window's (or child region's) height in pixels.
- `WindowWidth { get; init; } -> float` — The current window's (or child region's) width in pixels.
- `WindowX { get; init; } -> float` — The current window's (or child region's) left edge in screen pixels.
- `WindowY { get; init; } -> float` — The current window's (or child region's) top edge in screen pixels.
- `X { get; init; } -> float` — Where the next widget goes, x, window-local (scrolling included).
- `Y { get; init; } -> float` — Where the next widget goes, y, window-local (scrolling included).

## UiDraw — Immediate-mode drawing inside a Kiln window or a frame handler (a small wrapper over Dear ImGui).
- `BeginChild(string id, float width = 0, float height = 0) -> bool` — A scrolling region (call `UiDraw.EndChild` always).
- `BeginChild(string id, float width, float height, UiChildFlags flags, UiWindowFlags windowFlags = UiWindowFlags.None) -> bool` — A child region (call `UiDraw.EndChild` always).
- `BeginTab(string label) -> bool` — A tab of the tab bar; true while it is the selected one (then draw its contents and call `UiDraw.EndTab`).
- `BeginTabBar(string id) -> bool` — Starts a tab bar; true when it is shown (then add tabs with `UiDraw.BeginTab` and end it with `UiDraw.EndTabBar`).
- `BeginTable(string id, params string[] columns) -> bool` — A table; returns false when hidden (then skip the rows).
- `Button(string label, float width = 0, float height = 0) -> bool` — A button; true on the frame it is clicked.
- `ButtonHeld(UiMouseButton button = UiMouseButton.Left) -> bool` `0.2.13` — `button` is held down now (pressed in an earlier frame or this one, not yet released), wherever the pointer is.
- `Capturing { get; } -> bool` — Kiln captures mouse and keyboard right now.
- `Checkbox(string label, ref bool value) -> bool` — A check box; true on the frame `value` was toggled.
- `CloseScreen() -> void` — Closes the `UiScreen` being drawn (inside its draw callback).
- `CollapsingHeader(string label, bool defaultOpen = false) -> bool` — A header that folds what follows it; true while it is open.
- `Combo(string label, ref int index, IReadOnlyList<string> items) -> bool` — A drop-down list; true when `index` changed.
- `CurrentScreen { get; } -> UiScreen?` — The `UiScreen` being drawn, if any.
- `Cursor { get; } -> UiCursor` — Where the next widget goes and the space left.
- `DefaultFocus() -> void` — The widget drawn last gets the gamepad / keyboard focus when its window appears.
- `DeltaSeconds { get; } -> float` — Seconds since the previous overlay frame.
- `DragSource(string kind, string payload, string? preview = null) -> bool` `0.2.13` — Makes the widget drawn last a drag source (Dear ImGui's drag and drop): press it and move, and while the button is...
- `DrawRect(UiLayer layer, float x0, float y0, float x1, float y1, Color color, float rounding = 0, float thickness = 0) -> void` — A rectangle on `layer` (screen pixels).
- `DrawRect(float x0, float y0, float x1, float y1, Color color, float rounding = 0, bool filled = true) -> void` — A rectangle between two corners in screen pixels, over everything: filled, or only its outline.
- `DrawText(UiLayer layer, float x, float y, string text, Color color, float size = 0, float wrap = 0) -> void` — Text on `layer` at (x, y) screen pixels; size 0 = the current font size; wrap > 0 wraps at that width.
- `DrawText(float x, float y, string text, Color? color = null, float size = 0) -> void` — Text at (`x`, `y`) screen pixels, over everything.
- `DropTarget(string kind) -> string?` `0.2.13` — Makes the widget drawn last a drop target for payloads of `kind` (Dear ImGui's drag and drop): the payload's text on...
- `EndChild() -> void` — Ends a child region begun with `BeginChild`; call it whether or not the region was shown.
- `EndTab() -> void` — Ends a tab that `UiDraw.BeginTab` returned true for.
- `EndTabBar() -> void` — Ends a tab bar that `UiDraw.BeginTabBar` returned true for.
- `EndTable() -> void` — Ends a table that `UiDraw.BeginTable` returned true for.
- `FocusKeyboardHere(int offset = 0) -> void` — The keyboard goes to the next widget (a text field: type into it at once).
- `Font(UiFont font) -> UiDraw.FontChoice` — Text drawn inside the scope uses `font` at the current size: `using (ui.Font(UiFont.GameBold)) ui.Text("MODS");`.
- `FontSize(float size) -> UiDraw.FontScope` — Text drawn inside the scope uses `size` pixels at UI scale 1 (the scale applies on top)...
- `FrameNumber { get; } -> ulong` — Overlay frames since start.
- `HasLayout { get; } -> bool` — This kiln.ui has the layout calls in this file (version 3).
- `Height { get; } -> float` — Screen height in pixels (`UiDraw.Width` is the width).
- `HeldMods { get; } -> KeyMods` — The modifier keys held now.
- `Id(string id) -> UiDraw.IdScope` — Scopes widget ids (needed when the same label appears twice).
- `Image(UiImage image, float width = 0, float height = 0) -> void` — Draws an image (`Ui.LoadImage`, or an item's icon from `Ui.ItemImage`); 0 = its own size (scaled by `UiDraw.Scale`).
- `ImageButton(string id, UiImage image, float width = 0, float height = 0) -> bool` — An image as a button; true when clicked.
- `ImageButtonTrimmed(string id, UiImage image, float width, float height) -> bool` `0.2.8` — An image as a button, like `UiDraw.ImageButton`, showing only its art, fitted inside the image's box (see...
- `ImagePart(UiImage image, float width, float height, float u0, float v0, float u1, float v1) -> void` `0.2.8` — Draws part of an image (since 0.2.0-dev.29.16): the rectangle (`u0`, `v0`) to (`u1`, `v1`) in texture coordinates...
- `ImageTrimmed(UiImage image, float width, float height) -> void` `0.2.8` — Draws only an image's art (since 0.2.0-dev.29.16), otherwise like `UiDraw.Image`: the transparent margin around it...
- `InputMultiline(string label, ref string value, int maxBytes = 4096, float width = -1, float height = 0) -> bool` — A multi-line text field (at most `maxBytes` UTF-8 bytes).
- `InputText(string label, ref string value, int maxBytes = 256) -> bool` — A one-line text field editing `value` (at most `maxBytes` UTF-8 bytes).
- `ItemClicked(UiMouseButton button = UiMouseButton.Left) -> bool` — True on the frame `button` went down over the widget drawn last: `if (ui.Button("Item"))` answers a left click...
- `ItemHovered { get; } -> bool` — The mouse is over the widget drawn last.
- `ItemIcon(ItemId item, float size = 0) -> void` `0.2.7` — Draws the game's own icon of `item` (since 0.2.0-dev.29.15), `size` pixels square (0: 64 times `UiDraw.Scale`), as...
- `ItemIconTrimmed(ItemId item, float size) -> void` `0.2.8` — Draws the game's own icon of `item` like `UiDraw.ItemIcon`, but only its art (since 0.2.0-dev.29.16): the game's...
- `KeyPressed(UiKey key, bool repeat = false, bool claim = false) -> bool` — `key` went down this frame (only while Kiln has the input).
- `LastItem { get; } -> UiItem` — The rectangle and state of the widget drawn last.
- `Look(UiStyle style) -> UiDraw.LookScope` — Switches the look (the player's, the game's, Kiln's) for the windows begun inside the scope...
- `MeasureText(string text, float size = 0) -> (float Width, float Height)` — The size in pixels of `text` at `size` (0 = the current font size).
- `MeasureText(string text, float size, float wrap) -> (float Width, float Height)` — The size of `text` in the current font (size 0 = current size), wrapped at `wrap` when > 0.
- `MenuOpen { get; } -> bool` — The Kiln menu is open (input belongs to Kiln).
- `ModFont(UiFontFile font) -> UiDraw.FontChoice` `0.2.8` — Text drawn inside the scope uses a mod's font at the current size.
- `NextColumn() -> bool` — Moves to the next cell of the row; false when that column is not visible.
- `NextItemWidth(float width) -> void` — The width of the next widget (pixels; negative = the rest of the line minus that much).
- `NextRow() -> void` — Starts a new table row (after `UiDraw.BeginTable`).
- `Palette { get; } -> UiPalette` `0.2.8` — The colours of the look in use here (the player's outside any `UiDraw.Look`): draw your own shapes and text to match it.
- `Pointer { get; } -> Vector2?` `0.2.13` — The pointer (the mouse cursor) now, in the overlay's pixels: the space `UiDraw.DrawRect`, `UiDraw.DrawText`...
- `Progress(float fraction, string? overlay = null, float width = -1, float height = 0) -> void` — A progress bar for `fraction` (0 to 1, clamped) with optional text over it.
- `PushColor(UiColorSlot slot, Color color) -> UiDraw.PopScope` — A colour of the current look until the scope ends: `using (ui.PushColor(UiColorSlot.Text, gold)) ...`.
- `PushVar(UiStyleVar var, float x, float y = 0) -> UiDraw.PopScope` — A spacing or rounding of the current look until the scope ends (y is ignored for one-value vars).
- `Row(params string[] cells) -> void` — Starts a row and fills its cells with `cells` as text.
- `SameLine(float offset = 0, float spacing = -1) -> void` — Puts the next widget on the same line.
- `Scale { get; } -> float` — UI scale (DPI); multiply your own pixel sizes by it.
- `ScrollHere(float ratio = 0.5) -> void` — Scrolls the current window so the cursor sits at `ratio` (0 top, 0.5 middle, 1 bottom).
- `Selectable(string label, bool selected = false) -> bool` — A selectable row that fills the line's width; true on the frame it is clicked.
- `Selectable(string label, bool selected, float width, float height = 0, UiSelectableFlags flags = UiSelectableFlags.None) -> bool` — A selectable row of a size (0 = the rest of the line / one text line).
- `Separator() -> void` — A horizontal line.
- `Separator(string text) -> void` — A horizontal line with `text` in it.
- `SetCursor(float x, float y) -> void` — Moves the next widget to (x, y), window-local like `UiCursor.X`.
- `Slider(string label, ref float value, float min, float max) -> bool` — A slider for a float between `min` and `max`; true on the frame `value` changed.
- `Slider(string label, ref int value, int min, int max) -> bool` — A slider for an integer between `min` and `max`; true on the frame `value` changed.
- `SmallButton(string label) -> bool` — A small button (no frame padding, fits a line of text); true on the frame it is clicked.
- `Space(float width, float height) -> void` — Empty space.
- `Spacing() -> void` — A small vertical gap.
- `Text(Color color, string text) -> void` — A line of text in `color`.
- `Text(string text) -> void` — A line of text.
- `TextDisabled(string text) -> void` — A line of text in the look's disabled (greyed) colour.
- `TextWrapped(string text) -> void` — Text that wraps at the right edge of the window.
- `Theme(UiTheme theme) -> UiDraw.LookScope` `0.2.8` — Switches to a mod's theme for the windows begun (and the text drawn) inside the scope...
- `Tooltip(string text) -> void` — Shows `text` as a tooltip while the previous widget is hovered.
- `TreeNode(string label) -> bool` — A tree node; call `UiDraw.TreePop` when it returns true.
- `TreePop() -> void` — Ends a `UiDraw.TreeNode` that returned true.
- `Width { get; } -> float` — Screen size in pixels.
- `WindowRect { get; } -> (float X, float Y, float Width, float Height)` — Position and size of the current window in pixels.

## UiDraw.FontChoice — The scope `UiDraw.Font` returns; dispose it to go back to the earlier font.
- `FontChoice()`
- `FontChoice(bool pushed)`
- `Dispose() -> void` — Ends the font scope, if it was pushed.
- `Pushed { get; } -> bool` — True when the font was pushed; false when it is not loaded or kiln.ui is older than v4.

## UiDraw.FontScope — The scope `UiDraw.FontSize` returns; dispose it to go back to the earlier size.
- `FontScope()`
- `FontScope(bool pushed)`
- `Dispose() -> void` — Ends the font size scope.

## UiDraw.IdScope — The scope `UiDraw.Id` returns; dispose it to leave the id scope.
- `Dispose() -> void` — Ends the id scope.

## UiDraw.LookScope — Ends a `UiDraw.Look` or `UiDraw.Theme`.
- `Dispose() -> void` — Ends the look, if it is still the one on top.

## UiDraw.PopScope — Ends a `UiDraw.PushColor` (kind 1) or `UiDraw.PushVar` (kind 2).
- `PopScope()`
- `PopScope(int kind)`
- `Dispose() -> void` — Takes the colour or spacing off again.

## UiFont — A font for `UiDraw.Font` (kiln.ui v4).
enum: Default, Game, GameBold

## UiFontFile `0.2.8` — A mod's font (`Ui.LoadFont`) for its own windows: `using (ui.ModFont(font)) ...` or a theme's `UiThemeSpec.Font`.
- `Bytes { get; } -> int` — The size of its file in bytes.
- `Dispose() -> void` — Frees the font.

## UiImage — A PNG image for Kiln windows (`UiDraw.Image`); Dispose frees it (also when the owning mod unloads).
- `Dispose() -> void` — Frees the image.
- `Height { get; } -> int` — The image's height in pixels.
- `Item { get; } -> ItemId?` `0.2.7` — The item whose game icon this image draws (`Ui.ItemImage`), or null for a mod's own picture.
- `Width { get; } -> int` — The image's width in pixels.

## UiItem — The widget drawn last (`UiDraw.LastItem`), in screen pixels.
- `UiItem()`
- `UiItem(float MinX, float MinY, float MaxX, float MaxY, bool Hovered, bool Active, bool Focused, bool Clicked, bool Visible, bool Edited, bool Activated, bool Deactivated)`
- `Activated { get; init; } -> bool` — It became active this frame.
- `Active { get; init; } -> bool` — It is held: the mouse is down on it, or a text field is being edited.
- `Clicked { get; init; } -> bool` — The left button went down over it this frame.
- `Deactivated { get; init; } -> bool` — It stopped being active this frame.
- `Edited { get; init; } -> bool` — Its value changed this frame.
- `Focused { get; init; } -> bool` — It has the gamepad or keyboard navigation focus.
- `Height { get; } -> float` — The widget's height in pixels.
- `Hovered { get; init; } -> bool` — The mouse is over it.
- `MaxX { get; init; } -> float` — The widget's right edge.
- `MaxY { get; init; } -> float` — The widget's bottom edge.
- `MinX { get; init; } -> float` — The widget's left edge.
- `MinY { get; init; } -> float` — The widget's top edge.
- `Visible { get; init; } -> bool` — It is not clipped away.
- `Width { get; } -> float` — The widget's width in pixels.

## UiItemDetailLine `0.2.14` — A constructed game text line, in display order.
- `UiItemDetailLine(string Text, float? Value)`
- `Text { get; init; } -> string` — The line's text, in the language at capture time.
- `Value { get; init; } -> float?` — The computed stat number of a stat's first text row; null otherwise.

## UiItemDetailResult `0.2.14` — Ok when a game panel was captured.
- `UiItemDetailResult(bool Ok, string? Reason, UiItemDetailSnapshot? Snapshot)`
- `Ok { get; init; } -> bool` — True when a panel was captured.
- `Reason { get; init; } -> string?` — Why not; null when `UiItemDetailResult.Ok`.
- `Snapshot { get; init; } -> UiItemDetailSnapshot?` — The captured panel; null when not `UiItemDetailResult.Ok`.

## UiItemDetailSnapshot `0.2.14` — An owned copy of the last completed panel for this item/variant.
- `UiItemDetailSnapshot(ItemId Item, EntityId Entity, int Level, ItemRarity? Rarity, ulong Session, ulong Frame, ulong CaptureMs, IReadOnlyList<UiItemDetailLine> Lines)`
- `CaptureMs { get; init; } -> ulong` — When it was captured, on the `Environment.TickCount64` clock.
- `Entity { get; init; } -> EntityId` — The client entity of the item the panel was captured for.
- `Frame { get; init; } -> ulong` — The game UI frame of the capture.
- `Item { get; init; } -> ItemId` — The item's id.
- `Level { get; init; } -> int` — The item's level.
- `Lines { get; init; } -> IReadOnlyList<UiItemDetailLine>` — The panel's text lines in display order.
- `Rarity { get; init; } -> ItemRarity?` — The item's rarity, when known.
- `Session { get; init; } -> ulong` — The client session of the capture.

## UiItemView `0.2.3` — What the game drew the value for, copied for the callback.
- `UiItemView()`
- `UiItemView(uint Flags, EntityId LocalPlayer, EntityId ClientEntity, EntityId ServerEntity, ItemId Item, EntityId InventoryPage, byte SlotIndex, uint Fuel, uint FuelMax, uint UnitsPerCharge, uint Durability, uint DurabilityMax, uint Count, uint Level, ItemRarity Rarity, GameScreen Screen)`
- `ClientEntity { get; init; } -> EntityId` — The item's entity in the client's world (flag 1); none for a plain stack and a synthetic detail.
- `Count { get; init; } -> uint` — How many items are in the stack.
- `Durability { get; init; } -> uint` — The raw durability.
- `DurabilityMax { get; init; } -> uint` — The raw maximum durability.
- `Flags { get; init; } -> uint` — What the game drew the value for, as bit flags: 1 a client entity, 2 a proven server entity, 4 a real inventory...
- `Fuel { get; init; } -> uint` — The item's fuel in raw fuel units (for water: the water held).
- `FuelMax { get; init; } -> uint` — The most fuel the item holds, in raw fuel units.
- `InventoryPage { get; init; } -> EntityId` — The inventory page entity of the slot (flag 4: a real slot).
- `Item { get; init; } -> ItemId` — The item's id.
- `Level { get; init; } -> uint` — The item's level.
- `LocalPlayer { get; init; } -> EntityId` — The local player's entity.
- `Rarity { get; init; } -> ItemRarity` — The item's rarity.
- `Screen { get; init; } -> GameScreen` — The game screen the value was drawn on.
- `ServerEntity { get; init; } -> EntityId` — Always `EntityId.None` in this version.
- `SlotIndex { get; init; } -> byte` — The slot's index on its page (flag 4: a real slot).
- `UnitsPerCharge { get; init; } -> uint` — Raw fuel units per charge: the detail view's divisor, at least 1.

## UiKey — Keys for `UiDraw.KeyPressed` (keyboard and gamepad).
enum: Tab, Left, Right, Up, Down, PageUp, PageDown, Home, End, Enter, Escape, Backspace, F, GamepadLB, GamepadRB, GamepadX, GamepadY, GamepadStart, GamepadBack

## UiLabelEdit `0.2.3` — A label edit: `UiLabelMode.Set` needs a `Text`, the other modes take none.
- `UiLabelEdit(UiLabelMode Mode, UiText? Text = null)`
- `Mode { get; init; } -> UiLabelMode` — What to do with the label.
- `Text { get; init; } -> UiText?` — The text for `UiLabelMode.Set`; null for the other modes.

## UiLabelMode `0.2.3` — What a provider does with the label of the detail bar.
enum: Keep, Set, Clear, Game

## UiLayer — Where `UiDraw.DrawRect` draws.
enum: Window, Background, Foreground

## UiLook `0.2.8` — The player's choice of look for Kiln's UI (kiln.json `ui.look`).
enum: Game, Translucent, Kiln, Theme

## UiMouseButton — A mouse button for `UiDraw.ItemClicked`.
enum: Left, Right, Middle

## UiPalette `0.2.8` — The colours of a look by role (`UiDraw.Palette`): draw your own shapes and text in the look in use.
- `UiPalette()`
- `UiPalette(Color Text, Color TextMuted, Color WindowBackground, Color PanelBackground, Color FrameBackground, Color Border, Color Accent, Color Button, Color Header, Color Attention, Color Warning, Color Ok)`
- `Accent { get; init; } -> Color` — Check marks, slider grabs, the chosen tab's line, the focus outline, a held button (`UiThemeRole.Accent`).
- `Attention { get; init; } -> Color` — A status colour for your own drawing (`UiThemeRole.Attention`).
- `Border { get; init; } -> Color` — Outlines, separators, table lines (`UiThemeRole.Border`).
- `Button { get; init; } -> Color` — Buttons (`UiThemeRole.Button`).
- `FrameBackground { get; init; } -> Color` — Text fields, check boxes, slider tracks, idle tabs (`UiThemeRole.FrameBackground`).
- `Header { get; init; } -> Color` — Chosen and hovered rows, collapsing headers (`UiThemeRole.Header`).
- `Ok { get; init; } -> Color` — A status colour: fine (`UiThemeRole.Ok`).
- `PanelBackground { get; init; } -> Color` — Child regions and tiles, the chosen tab, table headers (`UiThemeRole.PanelBackground`).
- `Text { get; init; } -> Color` — Text (`UiThemeRole.Text`).
- `TextMuted { get; init; } -> Color` — Disabled and secondary text (`UiThemeRole.TextMuted`).
- `this[UiThemeRole role] { get; } -> Color` — The colour of `role`.
- `Warning { get; init; } -> Color` — A status colour: a problem (`UiThemeRole.Warning`).
- `WindowBackground { get; init; } -> Color` — Windows, title bars and pop-ups (`UiThemeRole.WindowBackground`).

## UiPlayerLook `0.2.8` — The player's look setting (`Themes.Player`; read only: the player chooses).
- `UiPlayerLook()`
- `UiPlayerLook(UiLook Look, string? ThemeId, float Background, float Opacity, float Scale)`
- `Background { get; init; } -> float` — The translucent look's window background opacity (0.1 to 1).
- `Look { get; init; } -> UiLook` — The look.
- `Opacity { get; init; } -> float` — Kiln's whole overlay, text included (0.3 to 1).
- `Scale { get; init; } -> float` — The overlay's size on top of the display's scale (0.75 to 1.5).
- `ThemeId { get; init; } -> string?` — With `UiLook.Theme`: the theme's `UiTheme.Id`, else null.

## UiProviderDecision `0.2.3` — A provider's answer.
- `UiProviderDecision(bool Ok, string? Reason, UiBarEdit Edit)`
- `Edit { get; init; } -> UiBarEdit` — What to change; `UiBarEdit.None` changes nothing.
- `Ok { get; init; } -> bool` — True to commit the edit (or pass the value on with `UiBarEdit.None`); false to refuse.
- `static readonly Pass -> UiProviderDecision` — Change nothing.
- `Reason { get; init; } -> string?` — Why it refused; null when `UiProviderDecision.Ok`.
- `static Refuse(string reason) -> UiProviderDecision` — Refuse, saying why (for the log and the event).
- `static Replace(UiBarEdit edit) -> UiProviderDecision` — Commit `edit`.

## UiRect `0.2.3` — A rectangle (game UI units or pixels, as the property says).
- `UiRect()`
- `UiRect(float X, float Y, float Width, float Height)`
- `Height { get; init; } -> float` — The height.
- `Width { get; init; } -> float` — The width.
- `X { get; init; } -> float` — The left edge.
- `Y { get; init; } -> float` — The top edge.

## UiScreen — A full screen of a mod: a menu with lists, buttons, sliders, text fields and images.
- `Close() -> void` — Closes the screen, if it is open.
- `Closed -> Action?` — Runs (as the owning mod, on the thread that closed it) when the screen closes.
- `Dispose() -> void` — Removes the screen (also done when the owning mod unloads).
- `IsOpen { get; } -> bool` — The screen is open now.
- `Open() -> void` — Opens the screen (does nothing when it is open, removed, or the overlay does not draw).
- `Opened -> Action?` — Runs when the screen opens.
- `Options { get; } -> UiScreenOptions` — The options the screen was created with.
- `Title { get; set; } -> UiText` — The title shown in the title bar.
- `Toggle() -> void` — Opens the screen if it is closed, closes it if it is open.

## UiScreenKey `0.2.3` — The part of the game screen a presentation identity names: no frame number, language or thread.
- `UiScreenKey()`
- `UiScreenKey(GameUiState State, HudScreen Hud, byte Crafting, uint Station)`
- `Crafting { get; init; } -> byte` — The game's crafting state byte of the screen; 0 when no crafting menu is open.
- `Hud { get; init; } -> HudScreen` — The open game screen (see `GameScreen.Hud`).
- `State { get; init; } -> GameUiState` — The game's top-level UI state.
- `Station { get; init; } -> uint` — The `keen::WorkshopId` of the crafting menu's station; 0 for the player's own crafting.

## UiScreenOptions — Options of a `UiScreen`.
- `CloseButton { get; init; } -> bool` — A close button (X) in the title bar.
- `CloseOnEscape { get; init; } -> bool` — Escape closes the screen (the top one first).
- `DimBackground { get; init; } -> bool` — Darken the game behind the screen.
- `FullScreen { get; init; } -> bool` — The whole display: no title bar, not movable or resizable, no frame (Width and Height are ignored).
- `Height { get; init; } -> float` — Size in pixels at UI scale 1, centred on the screen.
- `Resizable { get; init; } -> bool` — Whether the player can resize the screen.
- `Style { get; init; } -> UiStyle` — The screen's look; `UiStyle.Player` by default (the game's look unless the player chose another; before...
- `Theme { get; init; } -> UiTheme?` `0.2.8` — Your own theme for the screen (`Themes.Define`), over `UiScreenOptions.Style`.
- `TitleBar { get; init; } -> bool` — Whether the screen has a title bar.
- `Width { get; init; } -> float` — Size in pixels at UI scale 1, centred on the screen.

## UiSelectableFlags — Flags of the sized `UiDraw.Selectable`.
enum: None, SpanAllColumns, AllowDoubleClick, Disabled, AllowOverlap, Highlight

## UiStatus — Diagnostics of the overlay (`Ui.Status`).
- `UiStatus()`
- `UiStatus(bool Hooked, bool Device, bool Swapchain, bool Window, uint Width, uint Height, uint Format, long Presents, long FramesDrawn, double FrameMs, float Scale, string Error)`
- `Device { get; init; } -> bool` — The game's Vulkan device has been seen.
- `Error { get; init; } -> string` — The last overlay error; empty when there is none.
- `Format { get; init; } -> uint` — The Vulkan format (`VkFormat`) of the swapchain.
- `FrameMs { get; init; } -> double` — CPU time of the last overlay frame in milliseconds (the UI, the callbacks, recording and submitting).
- `FramesDrawn { get; init; } -> long` — Presents the overlay drew into.
- `Height { get; init; } -> uint` — The swapchain's height in pixels.
- `Hooked { get; init; } -> bool` — The Vulkan hooks are installed.
- `Presents { get; init; } -> long` — Game presents seen.
- `Scale { get; init; } -> float` — The UI scale (DPI).
- `Swapchain { get; init; } -> bool` — A swapchain of the game window exists.
- `Width { get; init; } -> uint` — The swapchain's width in pixels.
- `Window { get; init; } -> bool` — The game window's message handling is hooked.

## UiStyle — The look of a Kiln window or screen.
enum: Kiln, Game, Player `0.2.8`

## UiStyleVar — A spacing or rounding of the current look (`UiDraw.PushVar`); pixels (multiply by `UiDraw.Scale`), the `Align` ones 0..1.
enum: WindowPadding, FramePadding, ItemSpacing, ItemInnerSpacing, FrameRounding, FrameBorderSize, ChildRounding, ChildBorderSize, SelectableTextAlign, ButtonTextAlign, ScrollbarSize, WindowBorderSize

## UiText — A text in several languages: Kiln shows the one for the game's text language (exact key like "pt-br", then the primary language "pt", then "en", then "*").
- `UiText(IReadOnlyDictionary<string, string> texts)` — A text per language: the keys are language keys ("en", "pt-br") or `*` for any language.
- `UiText(string text)` — A text that is the same in every language.
- `For(string languageKey) -> string` — The text for a game language key ("de-de", "en-us"; see `GameLanguage`).
- `static implicit operator UiText(string text) -> UiText` — A plain string as a text for every language.
- `Languages { get; } -> IReadOnlyCollection<string>` — The languages given ("en", "de", "*", ...).
- `static Of(params (string Language, string Text)[] texts) -> UiText` — `UiText.Of(("en", "Mods"), ("de", "Mods"), ("fr", "Mods"))`.

## UiTheme `0.2.8` — A mod's theme (`Themes.Define`): give it to your windows (`UiWindowOptions.Theme`, `UiScreenOptions.Theme`, `CraftingPanelOptions.Theme`) or use it inside your own...
- `Base { get; } -> UiStyle` — What it starts from (`UiStyle.Game` or `UiStyle.Kiln`).
- `DisplayName { get; } -> UiText` — Its name in Kiln's Look list.
- `Dispose() -> void` — Removes the theme (its windows fall back to the player's look; a player who picked it sees the game's look until it...
- `Id { get; } -> string` — `<mod id>:<name>`: what kiln.json's `ui.theme` names when the player picks it.
- `IsDisposed { get; } -> bool` — Removed (disposed, or its mod unloaded).
- `Name { get; } -> string` — The name given to `Themes.Define`.
- `OfferedToPlayer { get; } -> bool` — Listed in Kiln's Look setting (`UiThemeSpec.OfferToPlayer`), on a game client whose overlay draws.

## UiThemeRole `0.2.8` — A colour role of a look (`UiPalette`, `UiThemeSpec.Roles`).
enum: Text, TextMuted, WindowBackground, PanelBackground, FrameBackground, Border, Accent, Button, Header, Attention, Warning, Ok

## UiThemeSpec `0.2.8` — A theme's definition (`Themes.Define`).
- `Background { get; init; } -> float?` — The windows' background opacity, 0..1 (null: the base's).
- `Base { get; init; } -> UiStyle` — What it starts from: `UiStyle.Game` (the game's look, default) or `UiStyle.Kiln`.
- `Colors { get; init; } -> Dictionary<UiColorSlot, Color>` — Single colours (with alpha), set after the roles.
- `Font { get; init; } -> UiFontFile?` — Its windows' text (`Ui.LoadFont`; null: the base's font, the game's for the game look).
- `Name { get; init; } -> UiText?` — Its name in Kiln's Look list (per language); null: the name given to `Themes.Define`.
- `OfferToPlayer { get; init; } -> bool` — Lists it in Kiln's Look setting, where the player may pick it for all of Kiln's UI.
- `Roles { get; init; } -> Dictionary<UiThemeRole, Color>` — Colours by role over the base's (the alpha is ignored: `UiThemeSpec.Background` is the windows' opacity).
- `Rounding { get; init; } -> float?` — Every corner (windows, frames, panels, pop-ups, grabs, tabs), pixels at scale 1 (null: the base's).
- `Vars { get; init; } -> Dictionary<UiStyleVar, (float X, float Y)>` — Spacings and sizes in pixels at UI scale 1 (Kiln applies the scale); the `Align` ones 0..1.

## UiTooltipDecision `0.2.3` — A tooltip-lines provider's answer: Ok with the lines to add (at most 16 per detail view, all providers together; a provider whose lines do not fit adds none), or not...
- `UiTooltipDecision(bool Ok, string? Reason, IReadOnlyList<UiText> Lines)`
- `Lines { get; init; } -> IReadOnlyList<UiText>` — The lines to add.
- `static readonly None -> UiTooltipDecision` — No lines.
- `Ok { get; init; } -> bool` — True to add `Lines`; false to add none.
- `Reason { get; init; } -> string?` — Why it refused; null when `UiTooltipDecision.Ok`.
- `static Refuse(string reason) -> UiTooltipDecision` — Refuse, saying why (for the log).
- `static With(params UiText[] lines) -> UiTooltipDecision` — These lines.

## UiTooltipLines `0.2.12` — A tooltip-lines provider's answer: Ok with the lines to add (at most 16 per detail view, all providers of Ui.Tooltip and Ui.Values.TooltipLines together; a provider...
- `UiTooltipLines(bool Ok, string? Reason, IReadOnlyList<TooltipLine> Lines)`
- `Lines { get; init; } -> IReadOnlyList<TooltipLine>` — The lines to add.
- `static readonly None -> UiTooltipLines` — No lines.
- `Ok { get; init; } -> bool` — True to add `Lines`; false to add none.
- `Reason { get; init; } -> string?` — Why it refused; null when `UiTooltipLines.Ok`.
- `static Refuse(string reason) -> UiTooltipLines` — Refuse, saying why (for the log).
- `static With(params TooltipLine[] lines) -> UiTooltipLines` — These lines.

## UiTooltipLinesProvider `0.2.12` — A provider of lines for the game's item detail view.
- `Invoke(in UiValueContext context, in UiItemView item) -> UiTooltipLines`

## UiTooltipOpenResult `0.2.3` — The details window was opened: Ok with the snapshot's session and source frame, or not Ok ("no tooltip": no item detail with lines in this session; "viewport too small").
- `UiTooltipOpenResult(bool Ok, string? Reason, ulong Session, ulong SourceFrame)`
- `Ok { get; init; } -> bool` — True when the window was opened.
- `Reason { get; init; } -> string?` — Why not; null when `UiTooltipOpenResult.Ok`.
- `Session { get; init; } -> ulong` — The session of the snapshot it shows.
- `SourceFrame { get; init; } -> ulong` — The game UI frame the snapshot was collected at.

## UiTooltipPanelChangedEvent `0.2.3` — Kiln's details window opened or closed (also closed by a world change or a provider change).
- `UiTooltipPanelChangedEvent(bool open, ulong session, ulong sourceFrame, ItemId item, string? reason)`
- `Item { get; } -> ItemId` — The item the window is about.
- `Open { get; } -> bool` — True when the window opened, false when it closed.
- `Reason { get; } -> string?` — Why it closed or could not open; null when opened or closed by a call.
- `Session { get; } -> ulong` — The session of the snapshot it shows.
- `SourceFrame { get; } -> ulong` — The game UI frame the snapshot was collected at.

## UiTooltipProvider `0.2.3` — A tooltip-lines provider.
- `Invoke(in UiValueContext context, in UiItemView item) -> UiTooltipDecision`

## UiTooltipScale `0.2.12` — A tooltip-values provider's answer: Ok with a factor the detail's stat numbers are multiplied by (finite, 0 to 1000; providers multiply in order), or not Ok (the...
- `UiTooltipScale(bool Ok, string? Reason, float Factor)`
- `static By(float factor) -> UiTooltipScale` — Multiply the numbers by `factor`.
- `Factor { get; init; } -> float` — What the stat numbers are multiplied by: finite, 0 to 1000.
- `static readonly None -> UiTooltipScale` — No change (factor 1).
- `Ok { get; init; } -> bool` — True to apply `Factor`; false to leave the numbers.
- `Reason { get; init; } -> string?` — Why it refused; null when `UiTooltipScale.Ok`.
- `static Refuse(string reason) -> UiTooltipScale` — Refuse, saying why (for the log).

## UiTooltipValuesProvider `0.2.12` — A provider of a factor for the stat numbers of the game's item detail view (the main damage, armour or block number and the other values its stats block draws), on...
- `Invoke(in UiValueContext context, in UiItemView item) -> UiTooltipScale`

## UiValueChangedEvent `0.2.3` — A value shown by the game's UI changed (also a game value changing while every provider passes).
- `UiValueChangedEvent(UiValueIdentity identity, UiBarValue? before, UiBarValue? after, ulong frame, UiValueChangeKind changeKind)`
- `After { get; } -> UiBarValue?` — The value shown now; null for `UiValueChangeKind.Removed`.
- `Before { get; } -> UiBarValue?` — The value shown before; null for `UiValueChangeKind.Added`.
- `ChangeKind { get; } -> UiValueChangeKind` — How the value changed.
- `Frame { get; } -> ulong` — The completed game UI frame it was published at.
- `Identity { get; } -> UiValueIdentity` — What the value is a value of.

## UiValueChangeKind `0.2.3` — How a published value changed.
enum: Added, Updated, Removed, Evicted

## UiValueContext `0.2.3` — Where a provider call happens.
- `UiValueContext()`
- `UiValueContext(UiValueKind Kind, ulong Session, ulong Frame, ulong ClientStep)`
- `ClientStep { get; init; } -> ulong` — The client world's step.
- `Frame { get; init; } -> ulong` — The game UI frame (see above for what it is by kind).
- `Kind { get; init; } -> UiValueKind` — Which value of the game's UI is asked for.
- `Session { get; init; } -> ulong` — The session of the call (see `UiValueIdentity.Session`).

## UiValueIdentity `0.2.3` — A presentation identity (what `UiValueChangedEvent` keys on).
- `UiValueIdentity()`
- `UiValueIdentity(UiValueKind Kind, ulong Session, EntityId LocalPlayer, UiScreenKey Screen, EntityId InventoryPage, byte SlotIndex, EntityId ClientEntity, ItemId Item, bool SyntheticGroup)`
- `ClientEntity { get; init; } -> EntityId` — The item's entity in the client's world; none when there is none.
- `InventoryPage { get; init; } -> EntityId` — The inventory page entity of the slot; none for a HUD value.
- `Item { get; init; } -> ItemId` — The item's id; 0 for a HUD value.
- `Kind { get; init; } -> UiValueKind` — Which value of the game's UI it is.
- `LocalPlayer { get; init; } -> EntityId` — The local player's entity.
- `Screen { get; init; } -> UiScreenKey` — The part of the game screen it was shown on.
- `Session { get; init; } -> ulong` — The session the identity was published in: it changes when every value is forgotten (`UiValuesResetEvent`).
- `SlotIndex { get; init; } -> byte` — The slot's index on its page.
- `SyntheticGroup { get; init; } -> bool` — True for a synthetic detail (built for an item id alone); its page, slot and entity are empty.

## UiValueKind `0.2.3` — Which value of the game's UI a provider replaces.
enum: Detail, Slot, Held, Health, Stamina, Mana

## UiValueProviderChange `0.2.3` — How a value provider's registration changed.
enum: Added, Removed, Faulted

## UiValueProviderChangedEvent `0.2.3` — A value provider was registered, removed, or removed for its faults.
- `UiValueProviderChangedEvent(UiValueKind kind, ItemId item, string mod, UiValueProviderChange change)`
- `Change { get; } -> UiValueProviderChange` — What happened to the registration.
- `Item { get; } -> ItemId` — The provider's item filter (`ItemId` 0: every item).
- `Kind { get; } -> UiValueKind` — The kind of value the provider replaces.
- `Mod { get; } -> string` — The id of the mod that registered it ("kiln" for Kiln's own).

## UiValueProviderRefusedEvent `0.2.3` — A value provider refused (Ok false, or its label did not fit the text budget).
- `UiValueProviderRefusedEvent(UiValueKind kind, string mod, ulong frame, string reason)`
- `Frame { get; } -> ulong` — The game UI frame in which it refused.
- `Kind { get; } -> UiValueKind` — The kind of value the provider replaces.
- `Mod { get; } -> string` — The id of the mod that registered the provider ("kiln" for Kiln's own).
- `Reason { get; } -> string` — The provider's reason, cut at 256 characters.

## UiValueRegistration `0.2.3` — A registration of `Values`.
- `Dispose() -> void` — Removes the registration.
- `Handle { get; } -> ulong` — The native handle (0 when refused).
- `Ok { get; } -> bool` — True when the registration was accepted.
- `Reason { get; } -> string?` — Why it was refused; null when `UiValueRegistration.Ok`.

## UiValuesResetEvent `0.2.3` — Every published value was forgotten at once (instead of a Removed per identity).
- `UiValuesResetEvent(UiValuesResetReason reason, ulong oldSession, ulong newSession)`
- `NewSession { get; } -> ulong` — The session that began.
- `OldSession { get; } -> ulong` — The session that ended (`UiValueIdentity.Session` of the forgotten values).
- `Reason { get; } -> UiValuesResetReason` — Why they were forgotten.

## UiValuesResetReason `0.2.3` — Why the published values were forgotten at once.
enum: World, Capability, Registry

## UiWheel — A radial wheel in the middle of the screen: 1 to 64 entries, 8 per page (the mouse wheel turns the page).
- `static Available { get; } -> bool` — The wheel exists in this process (a game client whose kiln.ui has it).
- `const Cancelled = -1 -> int` — The result of a wheel that was cancelled or closed.
- `Close() -> void` — Ends the wheel without a pick: `UiWheel.Result` completes with `UiWheel.Cancelled` and the result callback does not run.
- `Dispose() -> void` — Closes the wheel (the same as `UiWheel.Close`).
- `Entries { get; } -> IReadOnlyList<string>` — The entries, as given.
- `const HoldAlt = 18 -> uint` — Generic virtual-key codes for `UiWheelOptions.HoldKey`.
- `const HoldCtrl = 17 -> uint` — Generic virtual-key codes for `UiWheelOptions.HoldKey`.
- `HoldKey { get; } -> uint` — The key held while the wheel is open; 0 for none (`UiWheelOptions.HoldKey`).
- `const HoldShift = 16 -> uint` — Generic virtual-key codes for `UiWheelOptions.HoldKey`.
- `static IsAnyOpen { get; } -> bool` — Some wheel (any mod's) is open now.
- `IsOpen { get; } -> bool` — The wheel is still showing.
- `static IsValidHoldKey(uint vk) -> bool` — A hold key is 0 or a keyboard key's generic virtual-key code (no mouse buttons, no VK_LSHIFT-style codes).
- `const MaxEntries = 64 -> int` — The most entries a wheel takes.
- `static PageCount(int entries) -> int` — How many pages `entries` entries make (8 per page).
- `const PageEntries = 8 -> int` — How many entries a page of the wheel shows.
- `Result { get; } -> Task<int>` — Completes with the index of the chosen entry, or `UiWheel.Cancelled` when the player cancelled or the wheel was...
- `Title { get; } -> string?` — The title shown in the middle of the wheel; null for none (`UiWheelOptions.Title`).

## UiWheelOptions — Options of a `UiWheel`.
- `HoldKey { get; init; } -> uint` — 0, or a keyboard key's generic virtual-key code that the player holds while the wheel is open (`UiWheel.HoldShift`...
- `Title { get; init; } -> string?` — Shown in the middle of the wheel (cut at 64 bytes); null = none.

## UiWindow — A Kiln window (Forge: a Screen).
- `Dispose() -> void` — Removes the window for good (also done when the owning mod unloads).
- `Hide() -> void` — Closes the window (`UiWindow.Open` = false).
- `Open { get; set; } -> bool` — Shown right now.
- `Options { get; } -> UiWindowOptions` — The options the window was created with.
- `Show() -> void` — Opens the window (`UiWindow.Open` = true).
- `Title { get; } -> string` — The window's title.
- `Toggle() -> void` — Opens the window if it is closed, closes it if it is open.

## UiWindowFlags — Window flags for child regions.
enum: None, NoScrollbar, NoScrollWithMouse, NoBackground, AlwaysVerticalScrollbar

## UiWindowOptions — Options of a Kiln window.
- `AutoResize { get; init; } -> bool` — The window grows and shrinks with its contents.
- `BackgroundAlpha { get; init; } -> float?` — Background opacity (0..1), null = the theme's.
- `Height { get; init; } -> float` — Initial size in pixels at scale 1 (0 = fit the contents).
- `Interactive { get; init; } -> bool` — While open, Kiln takes mouse and keyboard from the game (a screen).
- `Position { get; init; } -> (float X, float Y)?` — Initial position as a fraction of the screen (0..1), the window's centre placed there; null = ImGui's default.
- `StartOpen { get; init; } -> bool` — Whether the window is open when it is created.
- `Style { get; init; } -> UiStyle` `0.2.8` — The window's look: `UiStyle.Player` (the default: the look the player chose for Kiln's UI), `UiStyle.Game` or...
- `Theme { get; init; } -> UiTheme?` `0.2.8` — Your own theme for the window (`Themes.Define`), over `UiWindowOptions.Style`.
- `TitleBar { get; init; } -> bool` — Whether the window has a title bar.
- `Width { get; init; } -> float` — Initial size in pixels at scale 1 (0 = fit the contents).

## VisibleSlot `0.2.3` — One slot draw of a frame.
- `VisibleSlot(uint Occurrence, EntityId InventoryPage, byte SlotIndex, ItemId Item, uint Count, EntityId ClientEntity, EntityId ServerEntity, uint ViewFlags, UiRect? OuterUi, UiRect? InnerUi, IReadOnlyList<byte> NativeState, uint Availability, UiRect? OuterPixels, UiRect? InnerPixels)`
- `Availability { get; init; } -> uint` — Bit flags of what this record holds: 1 the outer rectangle, 2 the inner, 4 the native state, 8 the viewport, 16 a...
- `ClientEntity { get; init; } -> EntityId` — The item's entity in the client's world; none for an empty slot or a plain stack.
- `Count { get; init; } -> uint` — How many items are in the stack.
- `Holder { get; init; } -> EntityId` — The client entity whose inventory holds the page (the player, a chest).
- `HolderFlags { get; init; } -> uint` — Bit flags of the holder: 1 known, 2 the local player, 4 `VisibleSlot.PageOrdinal` set, 8 proven to be the server's...
- `HolderSerial { get; init; } -> ulong` — Kiln's lifetime serial of the holder in this process; a destroyed and recreated holder gets a new one, never reused.
- `InnerPixels { get; init; } -> UiRect?` — The inner rectangle in pixels; null until the pixel mapping is verified.
- `InnerUi { get; init; } -> UiRect?` — The slot's inner rectangle in the game's UI units; null when the game gave none usable.
- `InventoryPage { get; init; } -> EntityId` — The inventory page entity the slot is on.
- `Item { get; init; } -> ItemId` — The item's id; 0 for an empty slot.
- `Level { get; init; } -> int` `0.2.14` — The item's level as the game drew the slot (0 for an empty slot, an item without levels, or a Kiln without the value).
- `NativeState { get; init; } -> IReadOnlyList<byte>` — The slot record's 16 state bytes.
- `Occurrence { get; init; } -> uint` — The draw order of this slot in the frame; not an identity.
- `OuterPixels { get; init; } -> UiRect?` — The outer rectangle in pixels; null until the pixel mapping is verified.
- `OuterUi { get; init; } -> UiRect?` — The slot's outer rectangle in the game's UI units; null when the game gave none usable.
- `PageOrdinal { get; init; } -> uint` — The page's position among the holder's pages, in the game's model order (not proven to be the saved inventory's page...
- `Rarity { get; init; } -> ItemRarity?` `0.2.14` — The item's rarity as the game drew the slot (the value its tooltip shows), captured on the game's main/UI thread and...
- `ServerEntity { get; init; } -> EntityId` — The item's entity in the server's world for a slot of a proven holder; none otherwise (see above).
- `SlotIndex { get; init; } -> byte` — The slot's index on its page.
- `ViewFlags { get; init; } -> uint` — The game's flags of the item view the slot was drawn from.

## VisibleSlotsResult `0.2.3` — A slots read: Ok false with a reason when there is no frame yet, the frame is older than 1000 ms, or it had more than 4096 slot draws (no partial list).
- `VisibleSlotsResult(bool Ok, string? Reason, VisibleSlotsSnapshot? Snapshot)`
- `Ok { get; init; } -> bool` — True when a frame was read.
- `Reason { get; init; } -> string?` — Why not; null when `VisibleSlotsResult.Ok`.
- `Snapshot { get; init; } -> VisibleSlotsSnapshot?` — The frame's slots; null when not `VisibleSlotsResult.Ok`.

## VisibleSlotsSnapshot `0.2.3` — One completed frame's slots.
- `VisibleSlotsSnapshot(ulong Session, ulong Frame, ulong CaptureMs, EntityId LocalPlayer, GameScreen Screen, UiRect ViewportPixels, float PixelsPerUnit, uint TransformFlags, IReadOnlyList<VisibleSlot> Slots)`
- `CaptureMs { get; init; } -> ulong` — When the frame was captured, on the `Environment.TickCount64` clock.
- `Frame { get; init; } -> ulong` — The game UI frame the slots were drawn in.
- `LocalPlayer { get; init; } -> EntityId` — The local player's entity.
- `PixelsPerUnit { get; init; } -> float` — Pixels per game UI unit.
- `Screen { get; init; } -> GameScreen` — The game screen of the frame.
- `Session { get; init; } -> ulong` — The client session the frame belongs to.
- `Slots { get; init; } -> IReadOnlyList<VisibleSlot>` — The slots drawn in the frame, in draw order.
- `TransformFlags { get; init; } -> uint` — Bit flags of the unit-to-pixel transform: 8 the viewport numbers are sane, 16 the pixel mapping is verified.
- `ViewportPixels { get; init; } -> UiRect` — The viewport in pixels.

## WorldDraw — Drawing at world positions (metres) with the frame's camera.
- `Box(WorldPosition center, Vector3 size, Color color, float thickness = 2) -> void` — A box of `size` metres centred on `center`.
- `Box(WorldPosition min, WorldPosition max, Color color, float thickness = 2) -> void` — The edges of an axis-aligned box between two corners.
- `Circle(WorldPosition center, double radius, Color color, float thickness = 2, int segments = 48) -> void` — A level ring of `radius` metres around `center` (on the ground at its height).
- `DistanceTo(WorldPosition p) -> double` — Metres from the camera.
- `Icon(WorldPosition p, UiImage image, float size = 32, Color? tint = null) -> bool` — An image centred on `p`, `size` pixels wide (times the UI scale; the height follows the image).
- `Label(WorldPosition p, string text, Color? color = null, float size = 0, float offsetY = 14, TextStyle style = TextStyle.Outline) -> bool` — Text centred above `p`, raised by `offsetY` pixels (times the UI scale).
- `Line(WorldPosition a, WorldPosition b, Color color, float thickness = 2) -> void` — A line between two world points, cut where it passes behind the camera.
- `Marker(WorldPosition p, Color color, float size = 10, MarkerShape shape = MarkerShape.Diamond, bool outline = true) -> bool` — A marker of `size` pixels (times the UI scale) at `p`.
- `Ready { get; } -> bool` — The camera is known this frame.
- `ToScreen(WorldPosition p, out Vector2 screen) -> bool` — Pixels of `p`; false when it is behind the camera or there is no camera.
- `ToScreenClamped(WorldPosition p, float margin, out Vector2 screen, out float angle) -> bool` — Like `WorldDraw.ToScreen`, but a point off screen or behind the camera is placed on the screen's edge (inset by...

## WorldOutline `0.2.13` — An outline on an entity's model in this player's game (`World.Outline`).
- `Active { get; } -> bool` — `WorldOutline.State` is not `OutlineState.Ended`: it shows or may show later.
- `ColorShared { get; } -> bool` — More than two colours are in use at once, and this one shows with the nearest other colour (the game has two...
- `Dispose() -> void` — Takes the outline away (kiln.render gives the entity's highlight back at the next client step).
- `Entity { get; } -> EntityId` — The outlined entity (in the client's world: the server's id of a replicated entity).
- `Options { get; } -> OutlineOptions` — The options it was asked with.
- `Reason { get; } -> OutlineReason` — Why it waits or ended (`OutlineReason.None` while shown).
- `State { get; } -> OutlineState` — Where it stands now (asked of kiln.render at each read); `OutlineState.Ended` after `WorldOutline.Dispose`.
