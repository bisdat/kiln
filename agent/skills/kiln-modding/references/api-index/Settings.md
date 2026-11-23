# Settings (Kiln.Settings)

## SettingChange — The result of `Settings.Set`: the value before and after (after = what the game holds now).
- `SettingChange(string Name, object Before, object After, bool Changed)`
- `After { get; init; } -> object` — The value the game holds now, as `Settings.Get` returns it.
- `Before { get; init; } -> object` — The value before the change, as `Settings.Get` returns it.
- `Changed { get; init; } -> bool` — False when the setting already had that value, so nothing changed.
- `Name { get; init; } -> string` — The setting (`enshrouded_server.json` key).

## SettingChangedEvent — A game setting changed.
- `After { get; } -> object` — The value after the change, as `Settings.Get` returns it.
- `Before { get; } -> object` — Values as `Settings.Get` returns them (bool, double, long, string).
- `By { get; } -> string` — Id of the mod that changed it, "kiln" for Kiln itself, "game" for a change outside Kiln.
- `FromGame { get; } -> bool` — True for a change made outside Kiln (`SettingChangedEvent.By` is "game").
- `Name { get; } -> string` — The setting (enshrouded_server.json key).

## SettingFlags — What Kiln knows about when a change takes effect.
enum: None, Live, PlayersRead, NextEvent, DocumentedRange, Unknown, Changed, Verified

## SettingInfo — One game setting with its current value.
- `SettingInfo(int Index, string Name, string Group, SettingKind Kind, SettingFlags Flags, string Effect, object Value, object Initial, object? Default, double Min, double Max, IReadOnlyList<string> Options, int Offset)`
- `Changed { get; } -> bool` — True when the value differs from the world's own value (`SettingFlags.Changed`).
- `Default { get; init; } -> object?` — The game's "Default" preset value (from the server readme); null when it is not known.
- `Effect { get; init; } -> string` — When a change takes effect, in words.
- `Flags { get; init; } -> SettingFlags` — What Kiln knows about when a change takes effect.
- `Group { get; init; } -> string` — The group it belongs to (Player, Survival, World, Time, Rates, Enemies or Other).
- `Index { get; init; } -> int` — The setting's index in `Settings.All`; stable for the life of the process.
- `Initial { get; init; } -> object` — The world's own value: what the settings held when Kiln first read them.
- `Kind { get; init; } -> SettingKind` — How the value is stored, and so the .NET type of `Value`.
- `Live { get; } -> bool` — True when the server uses a new value without a restart (`SettingFlags.Live`).
- `Max { get; init; } -> double` — The highest value `Settings.Set` accepts without `anyValue`.
- `Min { get; init; } -> double` — The lowest value `Settings.Set` accepts without `anyValue`.
- `Name { get; init; } -> string` — The `enshrouded_server.json` key, like `"enemyDamageFactor"`.
- `Offset { get; init; } -> int` — The setting's byte offset in the game's settings block (for diagnostics).
- `Options { get; init; } -> IReadOnlyList<string>` — The option names of an enum setting, in the game's order; empty for the other kinds.
- `PlayersRead { get; } -> bool` — True when players' games read it too (`SettingFlags.PlayersRead`).
- `Value { get; init; } -> object` — The current value, as `Settings.Get` returns it.

## SettingKind — How a setting's value is stored.
enum: Bool, Float, Int, Enum, Duration

## Settings — The server's game settings (the `gameSettings` block of `enshrouded_server.json`: player, enemy and boss factors, durability, starving, day/night length, tombstone...
- `static All { get; } -> IReadOnlyList<SettingInfo>` — Every setting with its current value, in the game's order.
- `static Available { get; } -> bool` — True when the native service is loaded and usable on this game build (capability `kiln.settings`).
- `static Get(string name) -> object` — The current value: bool, double, long (whole numbers, nanosecond durations) or string (option name).
- `static Has(string name) -> bool` — Whether the game has a setting of that name (enshrouded_server.json key, any case).
- `static Info(string name) -> SettingInfo` — Everything about one setting. unknown name
- `static Ready { get; } -> bool` — True once the live settings were found (from the end of the server's first world step; capability `settings.ready`).
- `static Resend() -> void` — Sends the settings to every player now (after several `Set(..., resend: false)`).
- `static Reset(string name, bool resend = true) -> SettingChange` — Back to the world's own value (what the server started with).
- `static ResetAll(bool resend = true) -> int` — Every setting back to the world's own value.
- `static Revision { get; } -> long` — +1 for every change kiln.settings saw (its own, or the game's admin menu).
- `static Save() -> string` — Writes the live values into enshrouded_server.json with `"gameSettingsPreset": "Custom"` (the game ignores...
- `static Set(string name, object value, bool resend = true, bool anyValue = false) -> SettingChange` — Changes a setting now and sends the settings to every player (unless `resend` is false).
- `static Status { get; } -> SettingsStatus` — State of kiln.settings and of the day/night clock.
- `static TrySet(string name, object value, out SettingChange? change, out string error, bool resend = true, bool anyValue = false) -> bool` — `Settings.Set` without the argument exception: false and the reason when refused.
- `static Values { get; } -> IReadOnlyDictionary<string, object>` — name -> current value, JSON-friendly (what an admin "settings_get" returns).

## SettingsStatus — State of kiln.settings and of the game's day/night clock.
- `SettingsStatus(bool Ready, bool ResendsToPlayers, bool VerifiedSingleton, int Count, long Revision, long Sets, long ResendsRequested, long ResendsSent, string? Preset, TimeSpan? DayDuration, TimeSpan? NightDuration, TimeSpan? TimeOfDay, uint DaytimeChanges, string ConfigPath)`
- `ConfigPath { get; init; } -> string` — The `enshrouded_server.json` that `Settings.Save` writes.
- `Count { get; init; } -> int` — How many settings there are; 0 until `Ready`.
- `DayDuration { get; init; } -> TimeSpan?` — The length of the day the game's day/night clock uses now; null when the clock is not readable.
- `DaytimeChanges { get; init; } -> uint` — Goes up by one each time the clock took new durations (or a time skip).
- `NightDuration { get; init; } -> TimeSpan?` — The length of the night the clock uses now; null when the clock is not readable.
- `Preset { get; init; } -> string?` — The `gameSettingsPreset` the server started with (Default, Relaxed, Hard, Survival or Custom); null when unknown.
- `Ready { get; init; } -> bool` — True once the live settings were found (capability `settings.ready`).
- `ResendsRequested { get; init; } -> long` — How many times kiln.settings asked the game to send the settings to the players.
- `ResendsSent { get; init; } -> long` — How many times the game sent the settings to the players.
- `ResendsToPlayers { get; init; } -> bool` — True when changes are sent to the connected players (the game's send flag is known on this build).
- `Revision { get; init; } -> long` — Goes up by one for every change kiln.settings sees, its own or the game's admin menu (`Settings.Revision`).
- `Sets { get; init; } -> long` — How many values were changed through kiln.settings.
- `TimeOfDay { get; init; } -> TimeSpan?` — The clock's time of day, 0 to 24 hours; null when the clock is not readable.
- `VerifiedSingleton { get; init; } -> bool` — True when the settings block is the world's registered `ecs::GameSettings` singleton.
