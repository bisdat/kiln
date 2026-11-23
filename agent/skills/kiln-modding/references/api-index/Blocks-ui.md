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

## TextRow — A line of text.
- `Color { get; init; } -> string?` — "#rrggbb", "#rrggbbaa", or white, black, gray, red, green, yellow, orange, cyan.
- `Text { get; init; } -> UiText` — The text.
- `Wrap { get; init; } -> bool` — Break long text at the window's edge.

## ToggleRow — A checkbox on a true/false config entry.
- `Bind { get; init; } -> string` — The true/false config entry it shows and writes: `config.<path>`.
- `Label { get; init; } -> UiText` — The checkbox's label.

## UiAction — What a button or a key does: exactly one of these.
- `ApplyEffect { get; init; } -> EffectRef?` — Applies an effect to the player, through a hidden ability Kiln makes for it (`ui-apply.<name>`).
- `CastAbility { get; init; } -> AbilityRef?` — Asks the server to cast an ability of this mod (or `"other-mod:name"`), as its key would: its costs and cooldown...
- `CloseWindow { get; init; } -> WindowRef?` — Closes a window block of this mod, or `"other-mod:name"`.
- `Command { get; init; } -> string?` — A command line of this game's command registry, as the console runs it ("reload", "/mymod-reset 5"): as the host's...
- `OpenWindow { get; init; } -> WindowRef?` — Opens a window block of this mod, or `"other-mod:name"`.
- `Set { get; init; } -> UiSet?` — Sets a config entry to a value.
- `Toast { get; init; } -> UiText?` — A short message in the corner of the screen.
- `Toggle { get; init; } -> string?` — Flips a true/false config entry: `"config.clock.show"`.

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

## UiSet — A config entry and the value to give it.
- `Bind { get; init; } -> string` — The config entry to set: `config.<path>`.
- `To { get; init; } -> object` — A true/false, a number or a text, which the entry takes: a number in its range, an option of an enum.

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
