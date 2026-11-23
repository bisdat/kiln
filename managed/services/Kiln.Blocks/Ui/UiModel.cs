// SPDX-License-Identifier: MIT
// UI as blocks: windows, HUD lines and key bindings, in C# and (the same shape) in the "windows", "hudLines" and "keyBindings" sections of
// blocks/*.json. They are what Kiln.Ui draws and binds on a player's game; on a dedicated server they are checked and then do nothing.
// docs/kiln/modding/ui-blocks.md.
using System.ComponentModel;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Kiln;

/// <summary>A block a player's game draws or binds with Kiln.Ui: <see cref="WindowBlock"/>, <see cref="HudLineBlock"/>, <see cref="KeyBindingBlock"/>.</summary>
public interface IUiBlock : IBlock { }

/// <summary>What a window is.</summary>
public enum WindowKind
{
    /// <summary>An overlay window: the player can drag it, and the X closes it. With <see cref="WindowBlock.Interactive"/> false it is only looked at.</summary>
    Window,
    /// <summary>A full screen of the mod in the game's look: the game behind it dims, it takes the mouse and keyboard, and Escape closes it.</summary>
    Screen,
}

/// <summary>Which of the game's menus a window's entry goes into.</summary>
public enum UiMenuIn
{
    /// <summary>The game's pause menu.</summary>
    Pause,
    /// <summary>The game's main menu.</summary>
    Main,
    /// <summary>The pause menu and the main menu.</summary>
    Both,
}

/// <summary>An entry in the game's own menus that opens a window.</summary>
public sealed record UiMenu
{
    /// <summary>The label. A plain text, or one per language: <c>UiText.Of(("en", "Settings"), ("de", "Einstellungen"))</c>.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Text { get; init; }
    /// <summary>Which of the game's menus holds the entry. Default <see cref="UiMenuIn.Pause"/>.</summary>
    public UiMenuIn In { get; init; } = UiMenuIn.Pause;
    /// <summary>Lower first. Kiln's own Mods entry has 1000.</summary>
    public int Order { get; init; }
}

/// <summary>
/// A window or a screen of rows: text, values, buttons, toggles, sliders, choices and separators. A value is a <c>game.*</c> value Kiln reads or a
/// <c>config.*</c> entry of this mod's config; a button runs an <see cref="UiAction"/>. Open it from a <see cref="KeyBindingBlock"/>, a button, a
/// <see cref="Menu"/> entry or <see cref="OpenAtStart"/>.
/// <code>
/// Blocks.Define("settings", new WindowBlock
/// {
///     Title = "My mod", Width = 360,
///     Rows =
///     [
///         new ToggleRow { Label = "Show the clock", Bind = "config.clock.show" },
///         new SliderRow { Label = "Scale", Bind = "config.clock.scale" },
///         new ButtonRow { Label = "Close", Does = new UiAction { CloseWindow = "settings" } },
///     ],
/// });
/// </code>
/// It is drawn by each player's game from the mod's own file, so the mod needs <c>clients: mod</c>. On a dedicated server it is checked and kept, and nothing is drawn.
/// What its widgets show refreshes every <see cref="Refresh"/> seconds, not every frame, and a closed window costs nothing.
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record WindowBlock : IUiBlock
{
    /// <summary>The window's title: a text, or one per language. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Title { get; init; }
    /// <summary>A window (the default) or a full screen.</summary>
    public WindowKind Kind { get; init; } = WindowKind.Window;
    /// <summary>Pixels at UI scale 1. 0: a window fits its rows, a screen is 720 wide.</summary>
    public float Width { get; init; }
    /// <summary>Pixels at UI scale 1. 0: a window fits its rows, a screen is 520 high.</summary>
    public float Height { get; init; }
    /// <summary>A window: where its centre is, [x, y] as 0..1 of the screen. Null: where the overlay puts it.</summary>
    public float[]? Position { get; init; }
    /// <summary>A window: it grows and shrinks with its rows.</summary>
    public bool AutoResize { get; init; }
    /// <summary>The title bar shows. Default true.</summary>
    public bool TitleBar { get; init; } = true;
    /// <summary>A window: how opaque the background is, 0..1. Null: the theme's.</summary>
    public float? Alpha { get; init; }
    /// <summary>A window: false gives a panel the player only looks at (no mouse and keyboard taken from the game).</summary>
    public bool Interactive { get; init; } = true;
    /// <summary>A screen: darken the game behind it. Null: yes.</summary>
    public bool? Dim { get; init; }
    /// <summary>Open as soon as the mod's game has it. Default: closed until something opens it.</summary>
    public bool OpenAtStart { get; init; }
    /// <summary>Seconds between updates of the values and conditions it shows: 0.1 to 3600. A toggle or a button answers at once.</summary>
    public double Refresh { get; init; } = 0.25;
    /// <summary>An entry in the game's pause menu and/or main menu that opens it.</summary>
    public UiMenu? Menu { get; init; }
    /// <summary>The rows, from top to bottom.</summary>
    public required IReadOnlyList<UiRow> Rows { get; init; }
    /// <summary>The window's own typed state, by key (<c>[a-z][a-z0-9_-]{0,63}</c>): what its rows bind as
    /// <c>state.&lt;key&gt;</c> (since 0.2.0-dev.31). Kept while the player stays in one world, reset in the next, never saved.
    /// At most 64 entries and 256 KiB. Null: none.</summary>
    public IReadOnlyDictionary<string, UiStateDefinition>? State { get; init; }

    /// <summary>A note in a data file, ignored.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public JsonElement Comment { init { } }
}

/// <summary>One line of a window. <c>"type"</c> in JSON says which: text, value, toggle, slider, choice, separator, button, and
/// (since 0.2.0-dev.31) input, table, tabs, tree, image.</summary>
[JsonPolymorphic(TypeDiscriminatorPropertyName = "type", UnknownDerivedTypeHandling = JsonUnknownDerivedTypeHandling.FailSerialization)]
[JsonDerivedType(typeof(TextRow), "text")]
[JsonDerivedType(typeof(ValueRow), "value")]
[JsonDerivedType(typeof(ToggleRow), "toggle")]
[JsonDerivedType(typeof(SliderRow), "slider")]
[JsonDerivedType(typeof(ChoiceRow), "choice")]
[JsonDerivedType(typeof(SeparatorRow), "separator")]
[JsonDerivedType(typeof(ButtonRow), "button")]
[JsonDerivedType(typeof(InputRow), "input")]
[JsonDerivedType(typeof(TableRow), "table")]
[JsonDerivedType(typeof(TabsRow), "tabs")]
[JsonDerivedType(typeof(TreeRow), "tree")]
[JsonDerivedType(typeof(ImageRow), "image")]
public abstract record UiRow
{
    /// <summary>Shown only while this is true: <c>"game.inGame"</c>, <c>"!game.inGame"</c>, <c>"config.clock.show"</c>, <c>"game.screen == InGame"</c>, <c>"game.fps &lt; 30"</c>, joined with <c>&amp;&amp;</c>.</summary>
    public string? When { get; init; }
    /// <summary>On the same line as the row before it.</summary>
    public bool SameLine { get; init; }
    /// <summary>Shown while the pointer rests on the row's widget.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public UiText? Tooltip { get; init; }
}

/// <summary>A line of text. <c>{game.fps:0}</c> and <c>{config.clock.scale:0.00}</c> in it show a value; <c>{{</c> and <c>}}</c> are braces.</summary>
public sealed record TextRow : UiRow
{
    /// <summary>The text. <c>{game.fps:0}</c> shows a value. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Text { get; init; }
    /// <summary>"#rrggbb", "#rrggbbaa", or white, black, gray, red, green, yellow, orange, cyan.</summary>
    public string? Color { get; init; }
    /// <summary>Break long text at the window's edge.</summary>
    public bool Wrap { get; init; }
}

/// <summary>A label and a value that cannot be edited here: <c>Label: value</c>.</summary>
public sealed record ValueRow : UiRow
{
    /// <summary>The label before the value. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary><c>game.&lt;name&gt;</c> or <c>config.&lt;path&gt;</c> (<c>config.&lt;config id&gt;/&lt;path&gt;</c> when two configs have the path).</summary>
    public required string Bind { get; init; }
    /// <summary>How a number or a true/false shows: <c>0.00</c>, <c>yes|no</c>.</summary>
    public string? Format { get; init; }
}

/// <summary>A checkbox on a true/false config entry. A click writes the config.</summary>
public sealed record ToggleRow : UiRow
{
    /// <summary>The checkbox's label. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary>The true/false config entry it shows and writes: <c>config.&lt;path&gt;</c>. Required.</summary>
    public required string Bind { get; init; }
}

/// <summary>A slider on a whole-number or decimal config entry. A drag writes the config once it rests for a moment.</summary>
public sealed record SliderRow : UiRow
{
    /// <summary>The slider's label. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary>The config entry it shows and writes: <c>config.&lt;path&gt;</c>. Required.</summary>
    public required string Bind { get; init; }
    /// <summary>Default: the entry's own range, which it must have when this and <see cref="Max"/> are left out.</summary>
    public double? Min { get; init; }
    /// <summary>The upper end of the slider. See <see cref="Min"/>.</summary>
    public double? Max { get; init; }
}

/// <summary>A drop-down on an enum config entry, with its options.</summary>
public sealed record ChoiceRow : UiRow
{
    /// <summary>The drop-down's label. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary>The enum config entry it shows and writes: <c>config.&lt;path&gt;</c>. Required.</summary>
    public required string Bind { get; init; }
}

/// <summary>A line, with a heading when it has text.</summary>
public sealed record SeparatorRow : UiRow
{
    /// <summary>The heading. Without text it is a plain line.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public UiText? Text { get; init; }
}

/// <summary>A button that runs an action.</summary>
public sealed record ButtonRow : UiRow
{
    /// <summary>The button's label. Required.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary>What a click does. Required.</summary>
    public required UiAction Does { get; init; }
}

/// <summary>When a text field writes what was typed: when it loses the keyboard (and on Enter), or on Enter only.</summary>
public enum UiInputCommit
{
    /// <summary>On Enter, and when the field loses the keyboard (Tab, a click elsewhere). Escape puts the old text back.</summary>
    Blur = 0,
    /// <summary>On Enter only; leaving the field without Enter puts the old text back.</summary>
    Enter = 1,
}

/// <summary>A text field (since 0.2.0-dev.31) on a text config entry or a string state entry. What is typed is a draft
/// until it is committed (<see cref="Commit"/>); Escape puts the committed text back. Only in an interactive window or a
/// screen: those take the keyboard, so nothing typed reaches the game or its chat.</summary>
public sealed record InputRow : UiRow
{
    /// <summary>The row's id, unique in the window: <c>[a-z][a-z0-9_-]{0,63}</c>.</summary>
    public required string Id { get; init; }
    /// <summary>The label shown beside the field.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Label { get; init; }
    /// <summary><c>config.&lt;path&gt;</c> of a text entry, or <c>state.&lt;key&gt;</c> of a string state entry.</summary>
    public required string Bind { get; init; }
    /// <summary>At most this many UTF-8 bytes (2..4096); a longer text is cut at a character before anything is written.</summary>
    public int MaxBytes { get; init; } = 256;
    /// <summary>When the typed text is written: <see cref="UiInputCommit.Blur"/> (the default) or <see cref="UiInputCommit.Enter"/>.</summary>
    public UiInputCommit Commit { get; init; } = UiInputCommit.Blur;
    /// <summary>Several lines (Enter is a new line; the text is committed when the field loses the keyboard).</summary>
    public bool Multiline { get; init; }
}

/// <summary>A column of a <see cref="TableRow"/>: its id, heading and cell, a template that reads the row's fields as
/// <c>{row.&lt;field&gt;}</c> (and any other value).</summary>
/// <param name="Id">The column's id, unique in the table.</param>
/// <param name="Label">The column's heading.</param>
/// <param name="Cell">What each row's cell in this column shows: a template such as <c>{row.count}</c>.</param>
public sealed record UiColumn(
    string Id,
    [property: JsonConverter(typeof(UiBlockTextConverter))] UiText Label,
    [property: JsonConverter(typeof(UiBlockTextConverter))] UiText Cell);

/// <summary>A table (since 0.2.0-dev.31) of the rows of a data source: <c>world.local.inventory</c> or a mod's own
/// <c>mod.&lt;id&gt;</c> rows. Rows keep the source's order and are told apart by their key. With <see cref="Selected"/>
/// bound to a string state entry, a click selects a row.</summary>
public sealed record TableRow : UiRow
{
    /// <summary>The row's id, unique in the window.</summary>
    public required string Id { get; init; }
    /// <summary>The rows: <c>world.local.inventory</c> or <c>mod.&lt;id&gt;</c> of a rows source.</summary>
    public required string Items { get; init; }
    /// <summary>1 to 16 columns.</summary>
    public required IReadOnlyList<UiColumn> Columns { get; init; }
    /// <summary>At most this many rows (1..512); a source with more shows a notice instead of a cut list.</summary>
    public int MaxRows { get; init; } = 128;
    /// <summary><c>state.&lt;key&gt;</c> of a string entry that holds the selected row's key ("" = none). Null: rows cannot be selected.</summary>
    public string? Selected { get; init; }
}

/// <summary>A tab of a <see cref="TabsRow"/>.</summary>
/// <param name="Id">The tab's id, unique in its row; the selected tab's id is what <see cref="TabsRow.Selected"/> holds.</param>
/// <param name="Label">The tab's name on the tab bar.</param>
/// <param name="Rows">The rows shown while the tab is selected.</param>
/// <param name="When">Shown only while this is true (see <see cref="UiRow.When"/>); null: always shown. A hidden selected tab moves to the first visible one.</param>
public sealed record UiTab(
    string Id,
    [property: JsonConverter(typeof(UiBlockTextConverter))] UiText Label,
    IReadOnlyList<UiRow> Rows,
    string? When = null);

/// <summary>Tabs (since 0.2.0-dev.31): each tab shows its own rows. The first visible tab is selected at first. With
/// <see cref="Selected"/> bound to a string state entry the selected tab's id is in it; otherwise the window keeps it.</summary>
public sealed record TabsRow : UiRow
{
    /// <summary>The row's id, unique in the window.</summary>
    public required string Id { get; init; }
    /// <summary>1 to 16 tabs with ids unique in the row.</summary>
    public required IReadOnlyList<UiTab> Tabs { get; init; }
    /// <summary><c>state.&lt;key&gt;</c> of a string entry that holds the selected tab's id.</summary>
    public string? Selected { get; init; }
}

/// <summary>A node of a tree written in the block (<see cref="TreeRow.Roots"/>).</summary>
/// <param name="Key">The node's key, unique in the whole tree: what the tree's selection and expanded branches hold.</param>
/// <param name="Label">The node's label.</param>
/// <param name="Children">The nodes under it, in this order; null or empty for a leaf.</param>
/// <param name="Values">Its field values (the tree's <see cref="TreeRow.Fields"/>), shown as <c>{node.&lt;field&gt;}</c>; null when it has none.</param>
public sealed record UiTreeNodeDefinition(
    string Key,
    [property: JsonConverter(typeof(UiBlockTextConverter))] UiText Label,
    IReadOnlyList<UiTreeNodeDefinition>? Children = null,
    IReadOnlyDictionary<string, UiScalar>? Values = null);

/// <summary>A tree (since 0.2.0-dev.31): nodes from a mod's own <c>mod.&lt;id&gt;</c> tree source (<see cref="Nodes"/>) or
/// written here (<see cref="Roots"/>, with <see cref="Fields"/> for their values). Branches open and close; under an open
/// branch and under each leaf, <see cref="NodeRows"/> (text and value rows) can show the node's fields as
/// <c>{node.&lt;field&gt;}</c>.</summary>
public sealed record TreeRow : UiRow
{
    /// <summary>The row's id, unique in the window.</summary>
    public required string Id { get; init; }
    /// <summary><c>mod.&lt;id&gt;</c> of a tree source. Exactly one of this and <see cref="Roots"/>.</summary>
    public string? Nodes { get; init; }
    /// <summary>The tree written in the block.</summary>
    public IReadOnlyList<UiTreeNodeDefinition>? Roots { get; init; }
    /// <summary>The fields of <see cref="Roots"/>' values (with a source, the source's schema says them).</summary>
    public IReadOnlyList<UiField>? Fields { get; init; }
    /// <summary>Up to 8 text or value rows drawn under an open branch's label and under each leaf.</summary>
    public IReadOnlyList<UiRow>? NodeRows { get; init; }
    /// <summary><c>state.&lt;key&gt;</c> of a string entry that holds the selected node's key. Null: nodes cannot be selected.</summary>
    public string? Selected { get; init; }
    /// <summary><c>state.&lt;key&gt;</c> of a stringSet entry that holds the open branches' keys. Null: the window keeps them.</summary>
    public string? Expanded { get; init; }
}

/// <summary>A PNG picture of the mod's folder (since 0.2.0-dev.31), loaded once when the block is defined: at most 8 MiB,
/// 2048 x 2048 pixels, and 8 pictures / 32 MiB of pixels per mod.</summary>
public sealed record ImageRow : UiRow
{
    /// <summary>The row's id, unique in the window.</summary>
    public required string Id { get; init; }
    /// <summary>A path in the mod's folder ("ui/map.png"): relative, no "..", no URL.</summary>
    public required string Asset { get; init; }
    /// <summary>Pixels at UI scale 1; 0: the picture's own width.</summary>
    public float Width { get; init; }
    /// <summary>Pixels at UI scale 1; 0: the picture's own height.</summary>
    public float Height { get; init; }
    /// <summary>What a click on the picture does.</summary>
    public UiAction? Does { get; init; }
}

/// <summary>
/// What a button or a key does: exactly one of these. They run on the player's game, never on a dedicated server.
/// </summary>
public sealed record UiAction
{
    /// <summary>A command line of this game's command registry, as the console runs it ("reload", "/mymod-reset 5"): as the host's own character when this game hosts the world, else
    /// as the console. It does not reach a server this game joined, unless <see cref="CommandTarget"/> is <see cref="UiCommandTarget.Server"/>.</summary>
    public string? Command { get; init; }
    /// <summary>Where <see cref="Command"/> runs: <see cref="UiCommandTarget.Local"/> (null, the default) as above, or <see cref="UiCommandTarget.Server"/>: on the server this
    /// game joined (or hosts), as this game's player there, with that player's rights (<see cref="BlockUi.ExecuteOnServer"/>; the outcome is a toast). Only with a command.</summary>
    public UiCommandTarget? CommandTarget { get; init; }
    /// <summary>Flips a true/false config entry: <c>"config.clock.show"</c>.</summary>
    public string? Toggle { get; init; }
    /// <summary>Sets a config entry to a value.</summary>
    public UiSet? Set { get; init; }
    /// <summary>Asks the server to cast an ability of this mod (or <c>"other-mod:name"</c>), as its key would: its costs and cooldown apply, and a refusal shows as a toast. The server needs the mod.</summary>
    public AbilityRef? CastAbility { get; init; }
    /// <summary>Applies an effect to the player, through a hidden ability Kiln makes for it (<c>ui-apply.&lt;name&gt;</c>). The server needs the mod.</summary>
    public EffectRef? ApplyEffect { get; init; }
    /// <summary>Opens a window block of this mod, or <c>"other-mod:name"</c>.</summary>
    public WindowRef? OpenWindow { get; init; }
    /// <summary>Closes a window block of this mod, or <c>"other-mod:name"</c>.</summary>
    public WindowRef? CloseWindow { get; init; }
    /// <summary>Opens the window when it is closed and closes it when it is open (since 0.2.0-dev.31): one key for both. A
    /// key fires while a mod's own window has the input (not while one of its text fields has the keyboard), so it also
    /// closes the window it opened; <see cref="KeyBindingBlock.WhileCapturing"/> only adds firing over the Kiln menu.</summary>
    public WindowRef? ToggleWindow { get; init; }
    /// <summary>A short message in the corner of the screen.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public UiText? Toast { get; init; }
}

/// <summary>A config entry and the value to give it.</summary>
public sealed record UiSet
{
    /// <summary>The config entry to set: <c>config.&lt;path&gt;</c>. Required.</summary>
    public required string Bind { get; init; }
    /// <summary>A true/false, a number or a text, which the entry takes: a number in its range, an option of an enum.</summary>
    public required object To { get; init; }
}

/// <summary>A window: what <see cref="Blocks.Define{T}"/> returned, its name in this mod, or <c>"other-mod:name"</c>.</summary>
/// <param name="Value">The window's name in this mod, or <c>"other-mod:name"</c>.</param>
[JsonConverter(typeof(WindowRefConverter))]
public readonly record struct WindowRef(string Value)
{
    /// <summary>A window by its name in this mod, or <c>"other-mod:name"</c>.</summary>
    public static implicit operator WindowRef(string value) => new(value);
    /// <summary>A window by its handle, as <see cref="Blocks.Define{T}"/> returned it.</summary>
    public static implicit operator WindowRef(BlockHandle<WindowBlock> handle) => new(handle.ToString());
    /// <summary>The window's name.</summary>
    public override string ToString() => Value;
}

internal sealed class WindowRefConverter : JsonConverter<WindowRef>
{
    public override WindowRef Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) => new(r.GetString() ?? "");
    public override void Write(Utf8JsonWriter w, WindowRef v, JsonSerializerOptions o) => w.WriteStringValue(v.Value);
}

/// <summary>
/// A line of text on the game's screen: <c>"FPS {game.fps:0}"</c>. It shows while its condition holds and updates at its own pace; hidden, it costs nothing.
/// <code>
/// Blocks.Define("fps", new HudLineBlock { Text = "{game.fps:0} fps", Anchor = HudAnchor.TopRight, When = "game.inGame", Refresh = 0.5 });
/// </code>
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record HudLineBlock : IUiBlock
{
    /// <summary>The text, with <c>{game.fps:0}</c> and <c>{config.clock.scale:0.00}</c> for values.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public required UiText Text { get; init; }
    /// <summary>Where on the screen the line sits. Default <see cref="HudAnchor.TopLeft"/>.</summary>
    public HudAnchor Anchor { get; init; } = HudAnchor.TopLeft;
    /// <summary>"#rrggbb", "#rrggbbaa", or white, black, gray, red, green, yellow, orange, cyan. Default white.</summary>
    public string? Color { get; init; }
    /// <summary>Shown only while this is true; see <see cref="UiRow.When"/>.</summary>
    public string? When { get; init; }
    /// <summary>Seconds between updates: 0.1 to 3600.</summary>
    public double Refresh { get; init; } = 1;

    /// <summary>A note in a data file, ignored.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public JsonElement Comment { init { } }
}

/// <summary>
/// A key that runs an action. The player can rebind it in the Kiln menu's Keys tab; the choice is kept in keybinds.json under <c>&lt;mod&gt;:&lt;name&gt;</c>.
/// <code>
/// Blocks.Define("settings-key", new KeyBindingBlock { Key = "F8", Description = "Open the settings", Does = new UiAction { OpenWindow = "settings" } });
/// </code>
/// </summary>
[BlockClients(ClientsLevel.Mod)]
public sealed record KeyBindingBlock : IUiBlock
{
    /// <summary>The default key: "F8", "Ctrl+K", "Shift+Alt+F2", "Numpad5", or "None" to bind it only once the player chooses.</summary>
    public required string Key { get; init; }
    /// <summary>What the Keys tab says. Default: the block's name.</summary>
    [JsonConverter(typeof(UiBlockTextConverter))]
    public UiText? Description { get; init; }
    /// <summary>The group in the Keys tab. Default: the mod's name.</summary>
    public string? Category { get; init; }
    /// <summary>What the key does. Required.</summary>
    public required UiAction Does { get; init; }
    /// <summary>A key that types something (a letter, a digit) fires in play only; true fires on every game screen too, chat included.</summary>
    public bool OnGameScreens { get; init; }
    /// <summary>Also fire while the Kiln menu is open.</summary>
    public bool WhileCapturing { get; init; }
    /// <summary>The name this block had before a rename: the player's rebinding follows it.</summary>
    public string? Was { get; init; }

    /// <summary>A note in a data file, ignored.</summary>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public JsonElement Comment { init { } }
}

/// <summary>A <see cref="UiText"/> in JSON: a string, or an object of language to string (<c>{ "en": "Save", "de": "Speichern" }</c>, <c>"*"</c> for every other language).</summary>
internal sealed class UiBlockTextConverter : JsonConverter<UiText>
{
    public override UiText? Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
    {
        if (r.TokenType == JsonTokenType.String) return new UiText(r.GetString() ?? "");
        if (r.TokenType != JsonTokenType.StartObject) throw new JsonException("a text is a string, or an object of language to text: { \"en\": \"...\", \"de\": \"...\" }");
        var texts = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
        {
            var language = r.GetString()!;
            r.Read();
            if (r.TokenType != JsonTokenType.String) throw new JsonException($"the text for \"{language}\" is a string");
            texts[language] = r.GetString()!;
        }
        if (texts.Count == 0) throw new JsonException("a text in languages needs at least one: { \"en\": \"...\" }");
        return new UiText(texts);
    }

    public override void Write(Utf8JsonWriter w, UiText v, JsonSerializerOptions o)
    {
        if (v.Languages.Count == 1 && v.Languages.First() == "*")
        {
            w.WriteStringValue(v.For("*"));
            return;
        }
        w.WriteStartObject();
        foreach (var language in v.Languages.Order(StringComparer.Ordinal)) w.WriteString(language, v.For(language));
        w.WriteEndObject();
    }
}
