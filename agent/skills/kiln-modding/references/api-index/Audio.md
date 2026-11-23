# Audio (Kiln.Audio)

## AudioStatus — kiln.audio's state (client side), from `Sound.Status`.
- `AudioStatus(bool Available, bool SoundSystem, bool Voiceover, bool Narration, bool VoiceoverManager, bool Frames, int MainThread, uint VoiceLanguageId, string VoiceLanguage, float VoiceoverVolume, uint LoreMode, uint LoreQueued, uint Live, ulong Played, ulong Failed, ulong GameStarts, ulong GameStartsOffMainThread, uint GameThreads, IReadOnlyList<uint> Scenes)`
- `Available { get; init; } -> bool` — The play feature (`audio.play`) is available.
- `Failed { get; init; } -> ulong` — The number of Kiln sound starts that failed since boot.
- `Frames { get; init; } -> bool` — A main-thread drain is running, so queued calls start (a UI frame or a client-world step).
- `GameStarts { get; init; } -> ulong` — The number of start calls the game made itself (counted only when the start probe is on; diagnostics).
- `GameStartsOffMainThread { get; init; } -> ulong` — Of `GameStarts`, the calls that were not on the main thread (diagnostics).
- `GameThreads { get; init; } -> uint` — The number of distinct threads the game's own start calls came from (diagnostics).
- `Live { get; init; } -> uint` — The number of sounds that are pending or playing.
- `LoreMode { get; init; } -> uint` — The game's lore auto-play mode byte (believed: 0 play now, 1 queue, 2 and up never; this mapping is not confirmed in...
- `LoreQueued { get; init; } -> uint` — The number of entries in the game's lore queue.
- `MainThread { get; init; } -> int` — The id of the thread that runs the drains (the game's main thread).
- `Narration { get; init; } -> bool` — Narration (`Voiceover.Narrate`, the `audio.narration` feature) is available.
- `Played { get; init; } -> ulong` — The number of Kiln sound starts that worked since boot.
- `Scenes { get; init; } -> IReadOnlyList<uint>` — The game's scene ids, indexed by `SoundScene` (seven entries); 0 means a scene that was not captured.
- `SoundSystem { get; init; } -> bool` — The game's sound system and its scenes are captured, so sounds can play.
- `VoiceLanguage { get; init; } -> string` — The player's voice language as a short key: en, de, fr, es or zh.
- `VoiceLanguageId { get; init; } -> uint` — The game's language id of the player's voice-over setting; 0 when not seen yet.
- `Voiceover { get; init; } -> bool` — Voice-over lines (`Voiceover.Play`, the `audio.voiceover` feature) are available.
- `VoiceoverManager { get; init; } -> bool` — The game's voice-over manager is running (in a game world, not the menu).
- `VoiceoverVolume { get; init; } -> float` — The voice-over volume the game's manager applies.

## CueInfo — A cue as the mod's cook wrote it (null fields: not in the index).
- `CueInfo(string Mod, string Name, SoundId Id, string? Bus, SoundScene DefaultScene, bool Loop, double? MinDistance, double? MaxDistance, float Volume, int DurationMs, bool Override)`
- `Bus { get; init; } -> string?` — The cue container's bus as the cook wrote it (`Sfx`, `Music`, `Ui` or `Voice`); null when none is written.
- `DefaultScene { get; init; } -> SoundScene` — The scene `Sound.Play2D` uses when you give none: the cooked `playScene`, else derived from `Bus` (Ui gives Ui...
- `DurationMs { get; init; } -> int` — The cooked length of the cue's take in milliseconds; 0 when unknown.
- `Id { get; init; } -> SoundId` — The cue's GUID.
- `Loop { get; init; } -> bool` — True when the cue loops: it plays until you stop it.
- `MaxDistance { get; init; } -> double?` — For a positional (3D) cue, the far end of its attenuation range in metres.
- `MinDistance { get; init; } -> double?` — For a positional (3D) cue, the near end of its attenuation range in metres.
- `Mod { get; init; } -> string` — The id of the mod the cue belongs to.
- `Name { get; init; } -> string` — The cue's name in the mod's cook recipe.
- `Override { get; init; } -> bool` — True when the cook wrote the cue as an override of an existing game cue (mode `override`) instead of a new one.
- `Volume { get; init; } -> float` — The cue's own volume from the cook, 0..1 (1 when none is written).

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
- `IsNone { get; } -> bool` — True when this is `SoundHandle.None`.
- `IsRemote { get; } -> bool` — A server-side handle (the sound was relayed to Kiln clients).
- `static readonly None -> SoundHandle` — No sound (the default value): what a call returns when nothing was started.
- `Value { get; init; } -> uint` — Kiln's id for the sound.

## SoundId — A cue: a SoundContainerResource GUID (takes, volume, loop, 3D distances), vanilla or a mod's.
- `SoundId()`
- `SoundId(Guid Cue)`
- `Cue { get; init; } -> Guid` — The cue's resource GUID.
- `IsNone { get; } -> bool` — True when this is `SoundId.None` (an empty GUID).
- `static Named(ModContext mod, string name) -> SoundId` — The calling mod's cue `name` (its cook recipe's `cue` step): the GUID from `assets/cooked/audio.json`, else...
- `static Named(string modId, string name) -> SoundId` — Another loaded mod's cue (e.g. a sound pack's), by mod id and cue name.
- `static readonly None -> SoundId` — No cue (the default value).
- `static Of(Guid cue) -> SoundId` — Any cue by GUID (vanilla or modded).
- `static Of(string cue) -> SoundId` — Any cue by GUID text.

## SoundInfo — Details of a sound (client side).
- `SoundInfo(SoundHandle Handle, SoundState State, SoundEndReason Reason, SoundScene Scene, uint Instance, int Thread, EntityId Entity, bool Following, bool Unmatched, string? VoiceLanguage, TimeSpan? StartDelay, TimeSpan? Played, WorldPosition Position)`
- `Entity { get; init; } -> EntityId` — The entity the sound was started on (`Sound.PlayOn`); `EntityId.None` for a sound without one.
- `Following { get; init; } -> bool` — True while the sound's position follows `SoundInfo.Entity`.
- `Handle { get; init; } -> SoundHandle` — The handle these details belong to.
- `Instance { get; init; } -> uint` — The game's own instance id behind the handle; 0 when the game has not started it yet.
- `Played { get; init; } -> TimeSpan?` — How long the sound has played so far, or in total once it has ended.
- `Position { get; init; } -> WorldPosition` — The last position set for the sound (`Sound.PlayAt`, `Sound.PlayOn`, `Sound.SetPosition`).
- `Reason { get; init; } -> SoundEndReason` — Why the sound is `SoundState.Stopped` or `SoundState.Failed`.
- `Scene { get; init; } -> SoundScene` — The sound scene the sound plays in.
- `StartDelay { get; init; } -> TimeSpan?` — The time from the request until the game started the sound.
- `State { get; init; } -> SoundState` — Where the sound stands: pending, playing, stopped or failed.
- `Thread { get; init; } -> int` — The id of the thread that made the game's start call (the game's main thread).
- `Unmatched { get; init; } -> bool` — True when the sound was asked to follow an entity but plays at its given position without following: the entity was...
- `VoiceLanguage { get; init; } -> string?` — For a voice-over line or narration: the voice language that was played (en, de, fr, es or zh).

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
- `IsNone { get; } -> bool` — True when this is `VoiceoverId.None` (an empty GUID).
- `static Named(ModContext mod, string name) -> VoiceoverId` — The calling mod's voice-over `name` (its recipe's `voiceover` step).
- `static Named(string modId, string name) -> VoiceoverId` — Another loaded mod's voice-over line.
- `static readonly None -> VoiceoverId` — No line (the default value).
- `static Of(Guid line) -> VoiceoverId` — Any voice-over line by GUID (the game's or a mod's).
- `static Of(string line) -> VoiceoverId` — Any voice-over line by GUID text.
- `Value { get; init; } -> Guid` — The line's resource GUID.

## VoiceoverInfo — A voice-over line as the mod's cook wrote it.
- `VoiceoverInfo(string Mod, string Name, VoiceoverId Id, bool Narration, string? Fallback, IReadOnlyDictionary<string, VoiceoverLanguage?> Languages, IReadOnlyDictionary<string, string> Subtitles)`
- `Fallback { get; init; } -> string?` — The language whose cue the cook used to fill empty slots, as written for the line; null when it set none, so empty...
- `Id { get; init; } -> VoiceoverId` — The line's GUID.
- `Languages { get; init; } -> IReadOnlyDictionary<string, VoiceoverLanguage?>` — The cooked takes by language key (en, de, fr, es, zh).
- `MaxDurationMs { get; } -> int` — The longest take over the languages (ms).
- `Mod { get; init; } -> string` — The id of the mod the line belongs to.
- `Name { get; init; } -> string` — The line's name in the mod's cook recipe.
- `Narration { get; init; } -> bool` — True when the cook marked the line as narration (for the lore queue, `Voiceover.Narrate`).
- `Subtitles { get; init; } -> IReadOnlyDictionary<string, string>` — The cooked subtitle text by text-language key.

## VoiceoverLanguage — One voice language of a cooked voice-over line.
- `VoiceoverLanguage(string Language, Guid Cue, int DurationMs, string? FallbackOf)`
- `Cue { get; init; } -> Guid` — The GUID of the cue that plays for this language.
- `DurationMs { get; init; } -> int` — The cooked length of the take in milliseconds; 0 when unknown.
- `FallbackOf { get; init; } -> string?` — When the cook filled this slot from another language, that language's key; null for a take recorded for this language.
- `Language { get; init; } -> string` — The language key: en, de, fr, es or zh.
