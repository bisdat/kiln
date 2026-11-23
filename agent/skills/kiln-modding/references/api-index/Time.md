# Time (Kiln.Time)

## DayStartedEvent — The day phase began (the clock passed `DayTimeState.DayStart`, also by a set or sleeping).
- `Hours { get; } -> double`
- `State { get; } -> DayTimeState` — The clock when Kiln noticed.

## DayTimeState — A snapshot of the server's day/night clock.
- `DayTimeState()`
- `DayTimeState(double Hours, double HoursField, bool IsDay, double DayStart, double NightStart, TimeSpan DayLength, TimeSpan NightLength, float TimeScale, uint ChangeCounter, uint? SyncedCounter, TimeSpan GameTime, TimeSpan EcsTime, bool WeatherAvailable, int WeatherLayers)`
- `ChangeCounter { get; init; } -> uint` — Goes up on every re-anchor; the server re-sends the clock to clients when it moves.
- `Clock { get; } -> string` — "HH:mm" of `DayTimeState.Hours`.
- `DayLength { get; init; } -> TimeSpan` — Real time the day phase lasts (game setting `dayTimeDuration`).
- `DayStart { get; init; } -> double` — Start of the day phase in hours (6 by default).
- `EcsTime { get; init; } -> TimeSpan` — The world's ECS time.
- `GameTime { get; init; } -> TimeSpan` — The day/night cycle clock (one cycle = day + night length).
- `Hours { get; init; } -> double` — Time of day, 0..24, at the current game clock (reflects a set at once).
- `HoursField { get; init; } -> double` — The game's own time-of-day field, updated once per sim step.
- `IsDay { get; init; } -> bool` — Day phase (`DayTimeState.DayStart` to `DayTimeState.NightStart`), as of the last step.
- `NightLength { get; init; } -> TimeSpan` — Real time the night phase lasts (game setting `nightTimeDuration`).
- `NightStart { get; init; } -> double` — Start of the night phase in hours (18 by default).
- `SyncedCounter { get; init; } -> uint?` — The counter the server last sent to clients; null when not observed yet.
- `TimeScale { get; init; } -> float` — 1 normally; the game raises it while players sleep at night.
- `WeatherAvailable { get; init; } -> bool` — Whether `Time.ResetWeather` can work.
- `WeatherLayers { get; init; } -> int` — Number of weather layers (0 = unknown).

## NightStartedEvent — The night phase began (see `DayStartedEvent`).
- `Hours { get; } -> double`
- `State { get; } -> DayTimeState`

## Time — The server world's time of day (service `kiln.time`).
- `static Available { get; } -> bool` — True when the native service is loaded and usable on this game build (capability `kiln.time`).
- `static CanSetSpeed { get; } -> bool` — True when this build can hold the clock's speed (`Time.SetSpeed`, `Time.SetCycle`; capability `time.speed`).
- `static Cycle { get; } -> TimeCycle` — The day and night cycle as the held speed shows it.
- `static DayLength { get; } -> TimeSpan`
- `static readonly FastCycle -> TimeSpan` — A whole day and night takes this long in `TimeCycle.Fast`.
- `static FastSpeed(TimeSpan day, TimeSpan night) -> double` — The speed `TimeCycle.Fast` uses for a cycle of `day` + `night`: one cycle in `Time.FastCycle`, at least 2.
- `static Format(double hours) -> string` — "HH:mm" for hours 0..24.
- `static HeldSpeed { get; } -> double?` — The clock speed Kiln holds (0 = stopped, 1 = the game's normal speed), or null when the game decides (1, faster...
- `static IsDay { get; } -> bool`
- `static readonly MaxWeatherDuration -> TimeSpan` `0.2.2` — The longest duration `Time.SetWeather` takes (game time).
- `static NightLength { get; } -> TimeSpan`
- `static OfDay { get; set; } -> double` — Time of day in hours, 0..24 (e.g. 13.5 = 13:30).
- `static Ready { get; } -> bool` — True once the server world exists and its clock can be read.
- `static ReleaseWeather(int? layer = null, bool now = false) -> Task<WeatherResult>` `0.2.2` — Hands the weather back to the game: Kiln's holds end on `layer` (null: every layer).
- `static ResetWeather() -> void` — Ends all weather and lets every weather layer pick a new phenomenon on the server's next weather update (in the same...
- `static Set(TimeSpan timeOfDay, TimeSetMode mode = TimeSetMode.Forward) -> void` — Sets the time of day from a clock time (e.g. `TimeSpan.FromHours(7.5)` = 07:30).
- `static Set(double hours, TimeSetMode mode = TimeSetMode.Forward) -> void` — Sets the time of day (hours; wrapped into 0..24).
- `static Set(string text, TimeSetMode mode = TimeSetMode.Forward) -> void` — Sets the time from text: "7", "7.5", "07:30", "19:45", "7:30pm", "noon", "midnight", "dawn", "dusk".
- `static SetCycle(TimeCycle cycle) -> void` — Normal (the game's), Stopped (the sun stands still) or Fast (a whole day and night in about 90 s).
- `static SetSpeed(double? speed) -> void` — Holds the day/night clock at a speed: 0 stops it (sleeping doesn't move it either), 1 is the game's normal speed...
- `static SetWeather(WeatherPhenomenon phenomenon, TimeSpan? duration = null) -> Task<WeatherResult>` `0.2.2` — Starts one phenomenon of `Time.WeatherPhenomena` (or a layer's "nothing" from `WeatherLayer.Choices`) on its layer.
- `static SetWeather(string phenomenon, TimeSpan? duration = null, int? layer = null) -> Task<WeatherResult>` `0.2.2` — Starts a weather phenomenon by its name, on every layer that has one of that name (`"rain"`: every region's rain...
- `static State { get; } -> DayTimeState` — The current clock state.
- `static TryGetState(out DayTimeState state) -> bool` — Like `Time.State`, without throwing.
- `static TryParse(string? text, out double hours) -> bool` — Parses "7", "7.5", "07:30", "7:30:15", "7pm", "7:30 am", "noon", "midnight", "dawn" (6:00), "dusk" (18:00).
- `static Weather { get; } -> IReadOnlyList<WeatherLayer>` `0.2.2` — Every weather layer: what runs there, until when, whether Kiln holds it, and what it can run.
- `static WeatherChoiceAvailable { get; } -> bool` `0.2.2` — True when the weather can be chosen right now: capability `time.weather_choose`, and the server's weather ran at...
- `static WeatherPhenomena { get; } -> IReadOnlyList<WeatherPhenomenon>` `0.2.2` — The game's weather table (the server's `WeatherSystemResource`): every phenomenon with its name, layer, effect...

## TimeCycle — The day and night cycle (`Time.SetCycle`).
enum: Normal, Stopped, Fast

## TimeSetEvent — Code changed the time of day through `Time.Set` (a mod, a command, Kiln.Admin).
- `Before { get; } -> double` — Time of day before (hours; NaN when it could not be read).
- `By { get; } -> string` — Id of the mod whose code set it ("kiln" for Kiln itself).
- `Hours { get; } -> double` — The time set (hours, 0..24).
- `Mode { get; } -> TimeSetMode`

## TimeSetMode — How `Time.Set` moves the clock.
enum: Forward, GameReset

## TimeSpeedChangedEvent `0.2.2` — Code changed the speed Kiln holds the day/night clock at (`Time.SetSpeed`, `Time.SetCycle`).
- `After { get; } -> double?` — The held speed now (null: the game decides again).
- `Before { get; } -> double?` — The held speed before (`Time.HeldSpeed`; null: the game decided).
- `By { get; } -> string` — Id of the mod whose code set it ("kiln" for Kiln itself).

## WeatherChangedEvent `0.2.2` — What runs on a weather layer changed: the game rolled the next phenomenon, or Kiln started or released one.
- `After { get; } -> WeatherPhenomenon?` — What runs now.
- `Before { get; } -> WeatherPhenomenon?` — What ran before (null: nothing was set, after a reset).
- `ByKiln { get; } -> bool` — Kiln made this change: a mod's choice, a held roll, or the game's roll that a release with `now` or...
- `Held { get; } -> bool` — Kiln holds a choice on the layer now.
- `Layer { get; } -> int` — The layer.

## WeatherEffect `0.2.2` — What a weather phenomenon does besides clouds (the game's `WeatherPhenomenonEffect`).
enum: None, Rain, Blizzard

## WeatherLayer `0.2.2` — One weather layer of the server: what runs there now and what can.
- `WeatherLayer(int Index, Guid Guid, WeatherPhenomenon? Current, bool Held, WeatherPhenomenon? HeldPhenomenon, TimeSpan Remaining, IReadOnlyList<WeatherPhenomenon> Choices)`
- `Choices { get; init; } -> IReadOnlyList<WeatherPhenomenon>` — What the layer's roll can pick: its phenomena and its "nothing".
- `Current { get; init; } -> WeatherPhenomenon?` — What runs now (a layer's "nothing" when nothing does); null right after a reset, until the game rolls.
- `Guid { get; init; } -> Guid` — Its GUID in the game's data.
- `Held { get; init; } -> bool` — Kiln holds a choice here (`Time.SetWeather` without a duration).
- `HeldPhenomenon { get; init; } -> WeatherPhenomenon?` — The held choice.
- `Index { get; init; } -> int` — The layer (0 is the clouds over the whole world on 26430253; the others are regions).
- `Remaining { get; init; } -> TimeSpan` — Game time until the layer's next roll.

## WeatherPhenomenon `0.2.2` — One phenomenon of the game's weather table (its `WeatherSystemResource`), or a layer's "nothing".
- `WeatherPhenomenon(int Index, string Name, Guid Guid, int Layer, WeatherEffect Effect, float Probability, TimeSpan MinDuration, TimeSpan MaxDuration)`
- `Effect { get; init; } -> WeatherEffect` — Rain or blizzard besides the clouds.
- `Guid { get; init; } -> Guid` — Its GUID in the game's data (empty for "nothing").
- `Index { get; init; } -> int` — Its index in the game's table; -1 for a layer's "nothing".
- `IsNothing { get; } -> bool` — A layer's "nothing": no phenomenon (clear sky there).
- `IsRolled { get; } -> bool` — The game's roll picks it (it lies in a layer's range); only such phenomena can be chosen.
- `Layer { get; init; } -> int` — The layer whose roll picks it; -1 when the game never rolls it (its debug entries, not offered).
- `MaxDuration { get; init; } -> TimeSpan` — The longest.
- `MinDuration { get; init; } -> TimeSpan` — The shortest run the game rolls for it (game time).
- `Name { get; init; } -> string` — The game's name: `clouds_few`, `clouds_medium`, `clouds_overcast`, `clouds_fluffy`, `rain`, `rain_snow` on 26430253...
- `const NothingName = "none" -> string` — The name `Time.SetWeather` takes for a layer's "nothing".
- `Probability { get; init; } -> float` — Its weight in its layer's roll.

## WeatherResult `0.2.2` — What `Time.SetWeather` or `Time.ReleaseWeather` did: `WeatherResult.Ok` when the game ran it on every layer asked; otherwise `WeatherResult.Reason` says why (the...
- `WeatherResult(bool Ok, string? Reason, IReadOnlyList<WeatherLayer> Layers)`
- `Layers { get; init; } -> IReadOnlyList<WeatherLayer>` — The layers asked for, as they are when the result arrives.
- `Ok { get; init; } -> bool` — True when every layer's request ran.
- `Reason { get; init; } -> string?` — Why not; null when `Ok`.
