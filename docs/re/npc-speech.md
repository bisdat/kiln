# NPC speech bubbles (greetings, idle lines) in the game client

Request: Marshall (Discord Ryrin), #mod-dev 2026-11-25. Game build: client 26430625 (sha 5d9ef2f1), server 26430253
(sha f0d824be), svn 1287369. Static analysis only (capstone over `/mnt/Dongus/kiln-wave/sfi/out/binaries` copies, Ghidra
headless decompile of single functions, `tools/kfc` over the client's data). Tags: **[data]** read from game data,
**[dis]** read in the code, **[hyp]** inferred, not seen.

## 1. Where the bubble text comes from

- **[data]** The bubble lines are the NPC's own **`NpcDialog` lines**, nothing else: `NpcDialog.dialogResource`
  (`keen::NpcDialogResource`) `initLines` (greetings), `transitionLines` ("Anything else?"), `farewellLines`. Each line
  has `bodyTexts: BlobArray<LocaTagId>` (variants) and a parallel `voiceovers` array.
  Ramone Jones (`Townsfolk_Male_villager_02_RamoneJones`, `fe2ce3af-...`): init lines `723637513` (his wake-up line,
  `isWakeup`), `2262475715` "Hello, Flameborn!", `3935509066` "Our hero!", `934215426` "Good day to you!"; transition
  `2803405929` "Anything else?"; farewells `653977772` "See you soon!", `899690196` "Praise be!", `1310400781` "Farewell!".
  All his voiceovers are null.
- **[data]** Who has them: every villager (`Townsfolk_*`), every craftsperson and assistant (`NPC_Workshop_*`), the
  trader and the Bard: all 55 templates with `NpcState` also have `NpcDialog`. "No talk page" on a base villager means his
  `dialogOptions` are all gated by knowledge requirements, not that he has no lines.
- **[data]** `NpcState.defaultGreetingSequence` / `attentionGreetingSequence` and `ActiveNpcState.socializingSetup.sequences`
  are **animations** (Ramone's `a0df25b9` "NPC_Townfolk_Greet": one SetAnimationEvent). They hold no text. The mumble a
  villager makes is the `VoiceoverNotifierEvent`s of the talk sequence (`51f5b029` "NPC_Townfolk_Talk": 34 notifiers,
  chosen per voice by entity tags), which the server plays as the dialogue's `defaultSequence`.
- So "the game's existing lines" are loca ids; a Kiln text needs a loca id the game can look up. kiln.content's loca
  hooks already answer ids Kiln registers (`kc_nc_set_texts`, the same path as new items' names, per language), so a mod
  never needs a game text entry or a resource patch.

## 2. Who picks a line and who draws it (client) **[dis]**

Everything about the text is **client-side**. The server never sends a line's text.

1. `ui_dialog` (client program #684, `0x460b40`; template NpcDialog, read RenderTransform/UiOffsets, write
   DynamicNpcDialog and G38UiPlayData) keeps one **record per NPC** (0x14b80 B) in a hash map at
   `G38UiPlayData+0x463de0` (find `0xd28db0(map, &entityId)`; key hash `x^=x>>16; x*=0x45d9f3b` twice). It sorts the
   NPC's lines into lists of 0x294-byte entries `{u32 lineId; u32 bodyTexts[32] @+8; u8 nTexts @+0x88; Guid vo[32]
   @+0x8c; u8 nVo @+0x28c}`: init lines with a marker/requirement ("priority") at `+0x90/+0x98` (ptr/count), plain
   greetings at `+0x5338/+0x5340`, transitions `+0xa5e0/+0xa5e8`, farewell ids `+0xf888/+0xf890`, options
   `+0xf8d0/+0xf8d8`. The record is rebuilt only when it is new or the knowledge counter (`DynamicNpcDialog
   .syncedKnowledgeQueryCounter`) changes.
2. The HUD's **NPC layer** (`doNpcUIWindow`, "[GameUi] NPC Layer", `0xc48d70`) calls the **per-NPC bubble** function
   `0xcc7760(entry, layer, slots, pos)` for each nearby NPC (`entry+0` entity id, `entry+8` the record). It runs a small
   state machine per NPC: idle → (the NPC is the player's interaction target, within **5 m**, `0x13d45e4`) pick lines
   (`0xd99e10(slot, record, storeGlobal)`: a random entry of the priority list, else of the plain greetings, and a random
   body text of it; also a transition line) → show the greeting → while the dialogue screen (HUD 26) is open show the plain
   greeting → after it, the transition line. The line ids go out as a local event `{entity, lineId, voiceover}`
   (`0xb0f2c0`), which is how `play_dialog_voiceover` plays a line's voice-over **[hyp: the event is
   SelectDialogOptionEvent; its layout matches]**.
3. The bubble itself is drawn by `0xd0dda0(BubbleParams*)`: `+0 u32 text (LocaTagId)`, `+4 u8 show` (fades to
   shown/hidden), `+5 u8 reset` (hide at once), `+6 u8 kind`, `+8 f32`, pointers at `+0x10/+0x18`. A text id 0 keeps
   the bubble hidden (`0xcc7760` clears `show` when the text is 0). The game's settings `hideNpcSpeechBubble` and the
   UI's own hide flags are checked by the game before or inside this; Kiln does not bypass them.
4. The dialogue window's own state (`0xd95ca0`) takes the plain greeting and a random farewell id from the same record
   when the dialogue opens and closes.
5. **Voice:** craftspeople's lines carry real voice-overs (the record's `vo[]`); villagers' are null and only mumble
   through the talk sequence, which the **server** plays when a player picks/opens a line (`npc_behavior`, creatures.md
   7.3 step 4).

Not followed: whether `0xb0f2c0`'s event reaches the server as `SelectDialogOptionAction` (so a Suppress may still
show the talk gesture when a player presses E); where the farewell text of `0xd95ca0` is drawn; the NPC layer's own
range (which NPCs get an entry).

## 3. Greeting range and cooldowns (server) **[refl] [data]**

`npc_behavior` (server #294) writes the player's `NpcDialogPartner {isInGreetingRange, lastGreetingTime}` and the NPC's
`DynamicNpcState {nextGreetingTime, greetedPlayers (PlayerMask), playersInSight}` and plays `defaultGreetingSequence`
(the wave). `NpcState.minDistanceToPlayer` 2 m, `idleSetup` cooldown 15-30 s (Ramone). This is animation only; it is
replicated as the actor's sequence. The text bubble does **not** depend on it: the client shows the bubble from its own
target/range test.

## 4. What Kiln hooks (kiln.npcspeech)

- The host cannot make a vanilla client show a different text: the text is the client's own template data and its own
  random pick. **Kiln clients only**; a vanilla client keeps the game's lines (nothing sent to it, nothing breaks).
- The decision is the host's; it travels as data (per NPC: mode + texts) on a Kiln channel; each Kiln client applies it
  where the game draws: the bubble's text id and visibility, and the record's voice-over GUIDs.
- Hooks (client, Kiln owns them; mods never call game functions):
  - `npcspeech/NpcBubble` `0xcc7760`: entry detour. Notes the NPC (entity, record) for the draw below; for an NPC
    under Kiln speech it mutes the record's voice-overs (priority + plain greetings) and puts them back when the
    override ends.
  - `npcspeech/BubbleDraw` `0xd0dda0`: entry detour. Inside an NPC's bubble call only: Suppress → `show` 0;
    Replace → the shown text becomes one of the mod's lines (a new pick each time the game picks a new line); Say →
    the mod's text, `show` 1, for its duration (also when the NPC is not the target: every NPC the layer draws).
  - `npcspeech/PickLines` `0xd99e10`: anchor only (never hooked/called): the record's list offsets are captured from
    its code, so a moved layout turns the voice part off instead of writing at a stale offset.
- Graceful degrade: a symbol or capture that does not resolve lowers `npcspeech.bubble` (no bubble change) or
  `npcspeech.voice` (texts change, voices stay the game's), with one kiln.log line. No text hook
  (`content.loca`/kiln.content missing): `npcspeech.bubble` lowered too, as Kiln's texts could not be shown.

## 5. Reproduce
- Lines and texts: `python3 tools/kfc/kfc.py decode fe2ce3af-e771-4179-9450-f2dee8d21209 keen::ecs::TemplateResource` (Ramone's
  NpcDialog), the En_Us table of the `LocaTagCollectionResource` (`kfc.mapmarkers.loca_table`) for the ids; sequences with
  `decode <guid> keen::actor::ActorSequenceResource`.
- Code: Ghidra headless decompile of single functions (no auto-analysis) of the client copy: `0x460b40` ui_dialog,
  `0xcc7760` NpcBubble, `0xd99e10` PickLines, `0xd0dda0` BubbleDraw, `0xd95ca0` the dialogue window's lines, `0xd28db0`
  the record map's find; callers by rel32 scan. sigdb: `python3 tools/sigport/sigdb_check.py sigdb/symbols/client.json
  <enshrouded.exe>` resolves `npcspeech/*` and both features on 26430625.
- Usage and status: [docs/kiln/services/npc-speech.md](../kiln/services/npc-speech.md).
