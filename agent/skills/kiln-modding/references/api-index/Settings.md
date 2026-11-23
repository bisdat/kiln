# Settings (Kiln.Settings)

## SettingChange — The result of `Settings.Set`: the value before and after (after = what the game holds now).
- `SettingChange(string Name, object Before, object After, bool Changed)`
- `After { get; init; } -> object`
- `Before { get; init; } -> object`
- `Changed { get; init; } -> bool`
- `Name { get; init; } -> string`

## SettingChangedEvent — A game setting changed.
- `After { get; } -> object`
- `Before { get; } -> object` — Values as `Settings.Get` returns them (bool, double, long, string).
- `By { get; } -> string` — Id of the mod that changed it, "kiln" for Kiln itself, "game" for a change outside Kiln.
- `FromGame { get; } -> bool`
- `Name { get; } -> string` — The setting (enshrouded_server.json key).

## SettingFlags — What Kiln knows about when a change takes effect.
enum: None, Live, PlayersRead, NextEvent, DocumentedRange, Unknown, Changed, Verified

## SettingInfo — One game setting with its current value.
- `SettingInfo(int Index, string Name, string Group, SettingKind Kind, SettingFlags Flags, string Effect, object Value, object Initial, object? Default, double Min, double Max, IReadOnlyList<string> Options, int Offset)`
- `Changed { get; } -> bool`
- `Default { get; init; } -> object?`
- `Effect { get; init; } -> string`
- `Flags { get; init; } -> SettingFlags`
- `Group { get; init; } -> string`
- `Index { get; init; } -> int`
- `Initial { get; init; } -> object`
- `Kind { get; init; } -> SettingKind`
- `Live { get; } -> bool`
- `Max { get; init; } -> double`
- `Min { get; init; } -> double`
- `Name { get; init; } -> string`
- `Offset { get; init; } -> int`
- `Options { get; init; } -> IReadOnlyList<string>`
- `PlayersRead { get; } -> bool`
- `Value { get; init; } -> object`

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
- `ConfigPath { get; init; } -> string`
- `Count { get; init; } -> int`
- `DayDuration { get; init; } -> TimeSpan?`
- `DaytimeChanges { get; init; } -> uint`
- `NightDuration { get; init; } -> TimeSpan?`
- `Preset { get; init; } -> string?`
- `Ready { get; init; } -> bool`
- `ResendsRequested { get; init; } -> long`
- `ResendsSent { get; init; } -> long`
- `ResendsToPlayers { get; init; } -> bool`
- `Revision { get; init; } -> long`
- `Sets { get; init; } -> long`
- `TimeOfDay { get; init; } -> TimeSpan?`
- `VerifiedSingleton { get; init; } -> bool`
