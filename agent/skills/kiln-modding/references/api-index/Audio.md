# Audio (Kiln.Audio)

## AudioStatus — kiln.audio's state (client side).
- `AudioStatus(bool Available, bool SoundSystem, bool Voiceover, bool Narration, bool VoiceoverManager, bool Frames, int MainThread, uint VoiceLanguageId, string VoiceLanguage, float VoiceoverVolume, uint LoreMode, uint LoreQueued, uint Live, ulong Played, ulong Failed, ulong GameStarts, ulong GameStartsOffMainThread, uint GameThreads, IReadOnlyList<uint> Scenes)`
- `Available { get; init; } -> bool`
- `Failed { get; init; } -> ulong`
- `Frames { get; init; } -> bool`
- `GameStarts { get; init; } -> ulong`
- `GameStartsOffMainThread { get; init; } -> ulong`
- `GameThreads { get; init; } -> uint`
- `Live { get; init; } -> uint`
- `LoreMode { get; init; } -> uint`
- `LoreQueued { get; init; } -> uint`
- `MainThread { get; init; } -> int`
- `Narration { get; init; } -> bool`
- `Played { get; init; } -> ulong`
- `Scenes { get; init; } -> IReadOnlyList<uint>`
- `SoundSystem { get; init; } -> bool`
- `VoiceLanguage { get; init; } -> string`
- `VoiceLanguageId { get; init; } -> uint`
- `Voiceover { get; init; } -> bool`
- `VoiceoverManager { get; init; } -> bool`
- `VoiceoverVolume { get; init; } -> float`

## CueInfo — A cue as the mod's cook wrote it (null fields: not in the index).
- `CueInfo(string Mod, string Name, SoundId Id, string? Bus, SoundScene DefaultScene, bool Loop, double? MinDistance, double? MaxDistance, float Volume, int DurationMs, bool Override)`
- `Bus { get; init; } -> string?`
- `DefaultScene { get; init; } -> SoundScene`
- `DurationMs { get; init; } -> int`
- `Id { get; init; } -> SoundId`
- `Loop { get; init; } -> bool`
- `MaxDistance { get; init; } -> double?`
- `MinDistance { get; init; } -> double?`
- `Mod { get; init; } -> string`
- `Name { get; init; } -> string`
- `Override { get; init; } -> bool`
- `Volume { get; init; } -> float`

## Sound — Play sounds and music: 2D (UI), at a world position, attached to an entity, or in Kiln's music slot.
- `static Available { get; } -> bool` — Client: kiln.audio runs and the game's sound system is captured (capabilities `kiln.audio`, `audio.play`).
- `static Describe(SoundId cue) -> CueInfo?` — What a mod's cook says about a cue (null: not a cooked cue of a loaded mod, e.g. vanilla).
- `static Info(SoundHandle h) -> SoundInfo?` — Details of a handle (client), null when unknown.
- `static IsPlaying(SoundHandle h) -> bool` — True while the sound is queued or playing (false once a one-shot ended, after `Sound.Stop`, for unknown handles, and...
- `static Play2D(SoundId cue, float volume = 1, SoundScene? scene = null) -> SoundHandle` — A sound without a position (UI style).
- `static PlayAt(SoundId cue, WorldPosition at, float volume = 1, float radius = 0) -> SoundHandle` — A positional sound at `at` (Game scene).
- `static PlayFor(EntityId player, SoundId cue, float volume = 1) -> SoundHandle` — Server: a 2D sound for one player (their Kiln client).
- `static PlayMusic(SoundId cue, float volume = 1, TimeSpan fadeIn = default(TimeSpan)) -> SoundHandle` — Music (the Music scene, the player's music slider) in Kiln's music slot: starting one stops the previous Kiln track...
- `static PlayOn(SoundId cue, EntityId entity, float volume = 1) -> SoundHandle` — A positional sound that follows `entity` (client: a client-world entity; on a host the ids equal the server's).
- `static Replace(SoundId cue, SoundId replacement) -> bool` — Makes the vanilla (or any) cue `cue` play `replacement`'s takes and settings: the replacement's...
- `static Restore(SoundId cue) -> void` — Undoes `Sound.Replace` for `cue` (the calling mod's override).
- `static SetPosition(SoundHandle h, WorldPosition at) -> void` — Moves a playing sound (ends following an entity).
- `static SetVolume(SoundHandle h, float volume, TimeSpan fade = default(TimeSpan)) -> void` — Changes a playing sound's volume (0..1), optionally fading.
- `static State(SoundHandle h) -> SoundState` — The handle's state (client).
- `static Status { get; } -> AudioStatus?` — kiln.audio's state (client), null on a server or unsupported build.
- `static StatusJson() -> string` — Diagnostics as JSON (status, recent handles, the game's own SoundStart threads).
- `static Stop(SoundHandle h, TimeSpan fade = default(TimeSpan)) -> void` — Stops a sound (any thread on a client; tick thread for server handles).
- `static StopAll(TimeSpan fade = default(TimeSpan)) -> int` — Stops every sound the calling mod started (client).

## SoundEndReason — Why a sound is `SoundState.Stopped` or `SoundState.Failed`.
enum: None, Finished, Stopped, EntityGone, StartFailed, NoSoundSystem, TimedOut, Replaced, Silent, Refused, WorldEnded, NotFound, Remote

## SoundHandle — A playing (or finished) sound.
- `SoundHandle()`
- `SoundHandle(uint Value)`
- `IsNone { get; } -> bool`
- `IsRemote { get; } -> bool` — A server-side handle (the sound was relayed to Kiln clients).
- `static readonly None -> SoundHandle`
- `Value { get; init; } -> uint`

## SoundId — A cue: a SoundContainerResource GUID (takes, volume, loop, 3D distances), vanilla or a mod's.
- `SoundId()`
- `SoundId(Guid Cue)`
- `Cue { get; init; } -> Guid`
- `IsNone { get; } -> bool`
- `static Named(ModContext mod, string name) -> SoundId` — The calling mod's cue `name` (its cook recipe's `cue` step): the GUID from `assets/cooked/audio.json`, else...
- `static Named(string modId, string name) -> SoundId` — Another loaded mod's cue (e.g. a sound pack's), by mod id and cue name.
- `static readonly None -> SoundId`
- `static Of(Guid cue) -> SoundId` — Any cue by GUID (vanilla or modded).
- `static Of(string cue) -> SoundId` — Any cue by GUID text.

## SoundInfo — Details of a sound (client side).
- `SoundInfo(SoundHandle Handle, SoundState State, SoundEndReason Reason, SoundScene Scene, uint Instance, int Thread, EntityId Entity, bool Following, bool Unmatched, string? VoiceLanguage, TimeSpan? StartDelay, TimeSpan? Played, WorldPosition Position)`
- `Entity { get; init; } -> EntityId`
- `Following { get; init; } -> bool`
- `Handle { get; init; } -> SoundHandle`
- `Instance { get; init; } -> uint`
- `Played { get; init; } -> TimeSpan?`
- `Position { get; init; } -> WorldPosition`
- `Reason { get; init; } -> SoundEndReason`
- `Scene { get; init; } -> SoundScene`
- `StartDelay { get; init; } -> TimeSpan?`
- `State { get; init; } -> SoundState`
- `Thread { get; init; } -> int`
- `Unmatched { get; init; } -> bool`
- `VoiceLanguage { get; init; } -> string?`

## SoundScene — The game's sound scenes (keen::SoundSceneType).
enum: Music, Ui, Game, Ambience, LoreVoiceover

## SoundState — Where a sound stands.
enum: None, Pending, Playing, Stopped, Failed

## Voiceover — Voice-over: `Voiceover.Play` speaks a line on the game's dialogue channel (what NPC dialogue uses: one line at a time, the player's voice-over language and volume, at...
- `static Available { get; } -> bool` — Client: the voice-over feature resolved on this build (capability `audio.voiceover`).
- `static Describe(VoiceoverId line) -> VoiceoverInfo?` — What a mod's cook says about a line (languages, subtitles), null when not a cooked line of a loaded mod.
- `static IsPlaying(SoundHandle h) -> bool` — True while the line is queued or playing (`Sound.IsPlaying`).
- `static Narrate(VoiceoverId line, UiText? subtitle = null, uint nameLoca = 0, uint descriptionLoca = 0) -> SoundHandle` — Narrates `line` through the lore queue (as a lore reading or journal entry): it plays at once or after the current...
- `static NarrateFor(EntityId player, VoiceoverId line) -> SoundHandle` — Server: narrates the line for one player's Kiln client.
- `static NarrationAvailable { get; } -> bool` — Client: narration (the lore queue) resolved on this build.
- `static Play(VoiceoverId line, EntityId speaker = default(EntityId), UiText? subtitle = null) -> SoundHandle` — Speaks `line` as `speaker` (a client-world entity; the line stays at it).
- `static PlayAt(VoiceoverId line, WorldPosition at, UiText? subtitle = null) -> SoundHandle` — Speaks `line` at a position (no speaker entity).
- `static PlayFor(EntityId player, VoiceoverId line, EntityId speaker = default(EntityId)) -> SoundHandle` — Server: speaks the line for one player's Kiln client (speaker optional).
- `static Stop(TimeSpan fade = default(TimeSpan)) -> void` — Stops the line Kiln is playing on the dialogue channel (not the game's own dialogue).

## VoiceoverId — A voice-over line: a VoiceoverResource GUID (one cue per voice language: en, de, fr, es, zh).
- `VoiceoverId()`
- `VoiceoverId(Guid Value)`
- `IsNone { get; } -> bool`
- `static Named(ModContext mod, string name) -> VoiceoverId` — The calling mod's voice-over `name` (its recipe's `voiceover` step).
- `static Named(string modId, string name) -> VoiceoverId` — Another loaded mod's voice-over line.
- `static readonly None -> VoiceoverId`
- `static Of(Guid line) -> VoiceoverId`
- `static Of(string line) -> VoiceoverId`
- `Value { get; init; } -> Guid`

## VoiceoverInfo — A voice-over line as the mod's cook wrote it.
- `VoiceoverInfo(string Mod, string Name, VoiceoverId Id, bool Narration, string? Fallback, IReadOnlyDictionary<string, VoiceoverLanguage?> Languages, IReadOnlyDictionary<string, string> Subtitles)`
- `Fallback { get; init; } -> string?`
- `Id { get; init; } -> VoiceoverId`
- `Languages { get; init; } -> IReadOnlyDictionary<string, VoiceoverLanguage?>`
- `MaxDurationMs { get; } -> int` — The longest take over the languages (ms).
- `Mod { get; init; } -> string`
- `Name { get; init; } -> string`
- `Narration { get; init; } -> bool`
- `Subtitles { get; init; } -> IReadOnlyDictionary<string, string>`

## VoiceoverLanguage — One voice language of a cooked voice-over line.
- `VoiceoverLanguage(string Language, Guid Cue, int DurationMs, string? FallbackOf)`
- `Cue { get; init; } -> Guid`
- `DurationMs { get; init; } -> int`
- `FallbackOf { get; init; } -> string?`
- `Language { get; init; } -> string`
