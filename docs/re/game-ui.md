# The game's own UI (client 26404966): screens, menus, crafting menus, fonts

Client Steam build 26404966 (`enshrouded.exe` sha256 `ce6fdba6…`). RVAs are this build's. Sources: Ghidra 12.1.2
headless decompiles of an analysed copy of the exe, python/capstone byte scans (string and call cross references),
the game's reflection (`tools/kfc types`), the game's UI data decoded offline (`tools/kfc decode` of
`keen::FbUiBundle` 46e14df6-… and `keen::UiFontResource` 9538a125-…), ShroudForge 1.7.6/1.7.7 (MIT, its native main-menu
entry), and a real client ("Runtime" below). Symbols: `sigdb/symbols/client.d/ui.json`. Service:
`native/services/ui/game.cpp`; usage: `docs/kiln/ui.md` "The game's own UI".

Tags: **[dis]** read from the disassembly / decompile (static, proven for this build), **[refl]** the game's
reflection, **[data]** the game's UI data, **[sf]** ShroudForge's shipped implementation, **[rt]** seen at run time,
**[hyp]** hypothesis (not proven).

## 1. Architecture: immediate-mode UI built every frame
- The UI is **code-driven immediate mode** ("GameUi"), not data-driven layouts: each frame, functions like
  `doPlayStateUi()` / `doCraftingMenu` / `doNpcInteractionWindow` (their names are window-id strings in the exe) open
  frames by name or hash, lay out children, draw text and images and ask for input. Persistent per-widget state lives in
  a hash-keyed store (`0x11a0610(frame, u64 hash, size, ctor, 1)` returns the state block; `0x11bacb0(state)` = "just
  created", i.e. the widget appeared this frame). **[dis]**
- The data side is configuration only: `keen::FbUiBundle` (81976-byte struct, one resource) holds styles, colours,
  textures, loca tags, sounds and lists for every menu (`menu.crafting.recipes` is the crafting menu's tab layout,
  §5). **[refl] [data]**
- App start creates the `GameUiSystem` (`0xa43f90`, "[app] Could not create GameUiSystem"; stored at gameApp+0x5b0)
  with an update callback, **GameUiFrame `0xbd0eb0`** (`lea rax, [rip+…]` at `0x85325d`, desc+0xc0 → system+0xf60).
  The system's update `0xa4fa60` calls it as `fn(system+0x10, p2)`. **[dis]**
- GameUiFrame builds a frame context on its stack (`0xa3c910(ctx, sys10, p2)`) and publishes it in the global
  **UiApp `0x283f4a8`** for the frame (`DAT_14283f4a8 = &ctx`), then clears it at the end (`0xa50d60(0)`). Every UI
  function reads `*(UiApp)`: `+0xc8` is the FbUiBundle config, `+0xd0` the persistent **GameUiData**. UiApp is only
  valid while GameUiFrame runs (content-data saw it null in a client-world Pre). **[dis]**
- Thread: one UI thread builds every frame in every state, worlds or not (the main menu has no world step). It is the
  game's main thread (managed thread 2, which also steps the client world). **[rt]**

## 2. State: which screen is open
| What | Where | Evidence |
|---|---|---|
| GameUiData | `*(sys10 + 0x48)` (GameUiFrame's first argument; `0xa4bf60` reads it into the ctx) = `*(ctx + 0xd0)` | [dis] |
| GameUi state | `*(GameUiData + 0)` | [dis] |
| **keen::UiState** (u8) | state `+0xc`: 0 None, 1 Preload (title), 2 PreGame (main menu), 3 Load, 4 LoreIntro, 5 LoreOutro, 6 Credits, 7 Play | [refl] enum; [dis] GameUiFrame's `switch (*(state+0xc))` at `0xbd137b`: 1 → `0xc6d090` (title screen window), 2 → `0xc69f70` (PreGame: "[GameUi] Entered PreGame", main menu `0xc2a320`), 3 → `0xc1a930` (loading), 4/5 → lore screens, 6 → `0xb81a40` (credits), 7 → `0xc602f0` (PlayStateUi) |
| Play data | state `+0x38` | [dis] PlayStateUi `0xc602f0` |
| **keen::HudScreenType** (u8) | play `+0x1cd8`; `+0x1cd9` the screen to return to | [refl] enum (54 values: 1 InGame, 2 SystemMenu, 3 SettingsMenu, 4 InGameMenu, 5 Map, 6 Journal, 7 Character, 8 PlayerCrafting, 9 Backpack, 10 SkillTree, 22 StorageChest, 25 StationBasedCrafting, 26 NpcDialog, 27 NpcCrafting, 28 FactoryCrafting, 43 TextChat, 51 NoUi, …); [dis] PlayStateUi returns at once for 0x33 (NoUi) and 0; the system menu returns unless it is 2 |
| Station workshop (u32) | play `+0x2e48`: the **keen::WorkshopId** of the crafting station in use (**[rt]**: the Workbench gave 0x4da88f73 here and as the builder's argument; first read as an entity id); `+0x2c50` set for NPC crafting | [dis] PlayStateUi keys the station's UI state by it; the HUD dispatcher passes it to the crafting builder for 25/27 |
| Text language | `*(GameUiData + 0x33ce8)` = LocaPool, `+0xd8` keen::LanguageId (docs/re/content.md 8.1) | [dis] the UI text getter `0xaff910` |

- Screen changes are **queued commands**, not direct writes: `toggleHudScreen 0xd74770(ui, type)` appends
  `{u8 1, u8 type}` to a command buffer at `*(GameUiData+0x33ce0)` ("[GameUi] toggleHudScreen to %k"); 85 callers.
  The game applies it later; the HUD screen byte above is what the UI draws. **[dis]**
- The HUD screen dispatcher `0xbe86f0(type, mode)` draws the full-screen menus: 2 → the system menu `0xcd55b0`,
  3 → settings `0xcd7970`, 25/27 → the crafting builder for the station's workshop; for the tabbed in-game menu it
  maps the screen to a tab (5 Map → tab 1, 6 Journal → 2, 7 Character → 5, 8 PlayerCrafting → 3, 9 Backpack → 4,
  10 SkillTree → 6, 44 Social → 0; tab in GameUiData `+0x31e88`) and draws that tab (crafting = `0xb80050`). **[dis]**
- Kiln reads these after each GameUiFrame (hook) and reports `KilnUiScreen`: ui_state, hud_screen, the station's
  workshop, the crafting menu (§5), language, UI thread, frame. **[dis] [rt]**
- Seen at run time: title Preload → PreGame (main menu) → None → Load → Play (HUD 0, then 1 InGame); Escape = 2;
  the tabbed menu (inventory, its crafting and map tabs) = **4** (the crafting tab is told apart by the crafting
  builder running); M = 5 (Map); a chest = 22; the Workbench = 25; Return to Main Menu = Load → None → PreGame. **[rt]**

## 3. Menus and their entries
### The main menu (PreGame landing page)
- PreGame `0xc69f70` → main menu window `0xc2a320` ("pregameMainMenuWindow", "mainMenuEntries"). Its entries are drawn
  by **MainMenuEntry `0xc2cbb0(state, desc)`** (only caller: this builder, 10 calls), `desc` = 32 bytes
  `{const char* text; u64 len; u64 fnv64(text); u8 options[8]}`; returns true when the entry is activated. **[dis] [sf]**
- Entries in order (config tags at FbUiBundle offsets; the strings are loca): Play `+0x5f90` (→ PreGame page 8),
  a link `+0x6030` ("%s <img external_link>"), **Settings** `+0x4c70` (→ page 5; drawn with "%s <img attention_dot>"
  when there is news), `+0x6000`, `+0x5ff0` (conditional), `+0x6010` (→ page 6), Quit `+0x6020` (→ `0xd71220`, the
  game's quit). A second layout branch (`+0x5fa0`…) has its own Settings entry. **[dis]**
- PreGame pages: `0xd49980(pregame, page)` ("[GameUi] PreGame: Go to %k", page at `+0x24c`); the main menu's own
  sub-pages `0xd497e0` (Landing 0 / CharacterSelection 1 / CharacterCreation 2, at `+0x4c`). **[dis]**
  Corrected on 26430625 (see "Main-menu pages" below): the `+0x6010` entry's page 6 is **Credits**;
  MainMenuEntry's first argument is `pregame+8`, not the PreGame block; the sub-page byte is `pregame+0x54`.

### The system (pause) menu
- `0xcd55b0` ("SystemMenu", "systemMenuContent", "menuLayouter"; "[GameUi] System Menu opened"), drawn while the HUD
  screen is 2. Entries go through **SystemMenuEntry `0xcb4710(text*, u8 1, u8 attention)`** (17 call sites in 4
  menus: `0xbba4b0`, `0xc65ae0`, the system menu, `0xcefa30`); `text` = 24 bytes `{ptr, len, fnv64}`; returns true on
  activation (and plays the click sound `cfg+0x124e0`). **[dis]**
- Entries: Resume `+0x6e60` (→ toggleHudScreen 1), **Settings** `+0x4c70` (→ sub-state 1), `+0x73b0` (4),
  `+0x7600` (2), `+0x9fa0` (3, conditional), a toggle `+0x8470`/`+0x8480`, `+0x5f20` (5, conditional), `+0xaf90`
  (→ screen 40 RedeemCommerceProducts), `+0xaf70` (→ 39 TwitchDrops), spacing, `+0x6e70` (→ `0xd6d480`, leave to the
  main menu), `+0x6e90` (→ `0xd71220`, quit), then the build number ("%s %u"). **[dis]**

### How an entry works (focus, gamepad, keyboard)
- An entry renderer opens a child frame, registers it with the layouter's navigation (`0xa41ed0(ui, frame)`), asks
  for hover / focus (`0xa4d1f0`), builds the confirm action hint (`0xb240b0(…, 0, 1)`), reads the activation
  (`0xa470d0`), draws the label (`0xd0d2d0(text, state, p2)`) and the attention marker. Gamepad and keyboard
  navigation is therefore the layouter's: an entry drawn through the same renderer inside the same layout is focusable
  and activatable like the game's own. **[dis]**; **[sf]** ShroudForge's "ShroudForge" entry behaves so in 1.7.6.
- The text's third field is FNV-1a 64 over the bytes (sign-extended, like the game's own producer; the empty text
  carries the offset basis 0xcbf29ce484222325 in `0xaff910`). It is the widget identity: a unique label gives a unique
  entry. **[dis] [sf]**
- Text lifetime: the game passes loca strings and frame-allocator strings; the UI may keep the pointer until the frame
  renders **[hyp]**, so Kiln interns labels for the process.

### Layout and scrolling (client 26430625, static only, nothing of it run)
Asked for a scrolling main and pause menu (2026-11-13: many rows before Settings push Quit off a 1080p screen). RVAs
of 5d9ef2f1. Helper: `kx.py` (copied from the NoClip research) over the exe as a file.
- **No cursor.** An entry is a frame of the game's retained layout tree, not a row at a cursor: SystemMenuEntry
  `0xcb47f0` opens a child frame of the current parent (`0xa3cbd0(&frame, 0)`; the frame handle at `+8`), sizes it
  (`0x11cf360(frame, {1.0, 0})`: full width, height from content), registers it for navigation (`0xa41fb0(UiApp,
  frame)`), asks for hover/focus (`0xa4d2d0`, `0xa471b0`), draws the label (`0xd0d3b0`) and closes the frame
  (`0xa40080`). Positions are computed by the layouter from the tree, so neither the row height nor the list's top is
  a number the entry code reads. **[dis]**
- **The pause menu's tree** (builder around `0xcd60f9`): a frame "systemMenuContent" (`0xa3cf70`, hash
  0xca8fd50ed1226434), inside it "menuLayouter" (`0xa3cb10`, hash 0x84a61178ec189a93; a value 25 or 40 picked by the byte at
  `rbp+0xb60`, use not traced; `0xa47740` with {0xe6000000, 1.0}), inside that a column frame (`0xa3c790`) whose width is the UI scale
  (`0xd46090(UiApp, 1.4, 0.8)`) times 300 units, then the entries: Resume (`call 0xcb47f0` at `0xcd63ab`), Settings
  (`0xcd6546`, the site Kiln draws before), and the rest down to Quit. **[dis]**
- **The game's own scroll view exists:** `keen::GameUiScrollingLayout`. Begin **`0xa3e020(layout*, u8 dl, state*,
  r9, params*)`**: `layout` is the caller's storage (at least 0x470 bytes: an outer frame at `+8`, a content frame at
  `+0x38`, the copied parameters from `+0x58`), `params` a `GameUiScrollingLayoutParameterData` (0x184 bytes copied;
  fields named in the reflection strings: enableScrollbar, forceScrollbar, doScissorWhenScrolling,
  useAutoScrolling, scrollingHotKeyMask, showScrollHint, scrollbarAlignment, scrollBar), `state` a per-call-site block
  from the UI's hash-keyed state store (`0x11a06e0(store, hash, size, ctor, 1)`; the credits use hash
  0xeb56c10a5766ad62, size 4). It names the outer frame "GameUiScrollingLayout" and, vertically, moves the content
  frame by the scroll offset (`0x11cf380(content, {0, offset})`). End **`0xa40160(layout*)`** (85 callers). The
  credits (`0xb81ecc` … `0xb82a3e`) and the recipe info panel ("infoScroller", `0xbb5f66`) use it; the parameter
  block is built by `0xa3eeb0` (defaults) and `0xa3df20` plus the scrollbar look copied from the UI bundle
  (`UiApp+0xc8` at `+0xfa84…+0xfb04`). Frame state `keen::UiFrameScrollState` {scrollOffset, scrollAreaSize}.
  **[dis] [refl]**
- **Settled for the experiment (2026-11-13, static):**
  - Layout nodes: `+0x6c` alignment (u16, `0x11ced70`), `+0x90` size (two floats, `0x11cf360`; 0 = fixed or from
    content, 1.0 = the parent's), `+0xb8` stack kind (1 `0x11ceff0`, 2 `0x11cf380`: the column's kind, entries one
    below the other) with its spacing at `+0xc0`, `+0x114` the rectangle x, y, w, h (`0x11b67d0` returns it). A frame
    builder holds its node at `+8`. **[dis]**
  - The column: `0xa3c790(&col, 0)` (a stack of kind 2, no spacing) at `0xcd620a`, fixed width (`0x11ceea0`, 300 ×
    scale), size {0, 1.0} (as tall as its parent), alignment 0x201, then a flexible spacer (`0xb2a8e0(col, {1, 1})`),
    the navigation scope (`0xa3f790(rbp+0x7b0)`), Resume … Quit, a fixed and a flexible spacer, the build number,
    the scope's end (`0xa419d0`), and the column's end `0xa40080(&col)` at `0xcd6bbe`. Between `0xcd620f` and
    `0xcd6bbe` no jump leaves and none comes in from outside, and there is no `ret`: every call that opens the column
    closes it. So the column's height is bounded (its parent's), and the flexible spacers centre the entries; with too
    many entries they overflow the column. **[dis]**
  - `GameUiScrollingLayoutParameterData` fields (reflection records at `0x1b17930…0x1b17d20`: name, type, offset):
    borderColor 0x0, scrollHintTexture 0x8, padding 0x18, scrollbarAlignment 0x28, showScrollHint 0x2a,
    enableScrollbar 0x2b, useNavigation 0x2c, stealCapture 0x2d, doScissorWhenScrolling 0x2e, centerChildren 0x2f,
    enableDrag 0x30, forceScrollbar 0x31, useGlobalFocus 0x32, useFillerSeperatorLineDistance 0x33, consumeInput 0x34,
    useAutoScrolling 0x35, isDefaultRightStickTarget 0x36, fillerSeperatorLineDistance 0x38, spacing 0x3c,
    scrollingHotKeyMask 0x40, mouseDragInputMask 0x41, scrollBar 0x44 (GameUiScrollbarParameterData); 0x104 bytes
    (what begin copies). Defaults from `0xa3eeb0`: enableScrollbar, stealCapture, doScissorWhenScrolling, enableDrag,
    consumeInput and useAutoScrolling on; useNavigation, centerChildren, forceScrollbar off; scrollingHotKeyMask 1.
    The credits change enableScrollbar and consumeInput (a byte of theirs) and turn centerChildren on. **[refl] [dis]**
  - Begin's `r8` is a 4-byte state block on a node from the state store (`0x11a06e0(node, hash, 4, 0xa3b900 (a no-op
    constructor), 1)`; when `0x11bad80(block)` says it is new the credits zero it); `r9` is 0 in the callers read;
    `dl` is 1 in 41 of the callers, including the credits and the recipe info, both vertical lists. With `dl` 1 the
    content frame is a stack of kind 1, so callers open their own vertical stack inside it (the credits do). After
    begin the credits call `0xd4e3e0(layout)` and pass its float to `0xa518a0(layout, f)`. **[dis]**
- **First try in the client (2026-11-13, 3440x1440, owner):** the menu drew, no crash, the wheel scrolls; but the view
  was a third of the column (log: column 1020, view 311, entries 1522), its scrollbar a plain white bar, and the view
  did not follow the highlight (the arrow keys reach Quit out of sight). Causes, by reading:
  - **Height.** `_spacer_` (`0xb2a8e0` flexible {1, 1}, `0xb2a810` fixed) creates its node with `0x11bf2e0(builder's
    node, 0, 0)`: a child of the column's node itself, not of the current frame (`0xa3c790` and the entries use
    `0x11c4c60` on the UI context's current parent). So the builder's two flexible spacers and one fixed spacer stay
    children of the column next to Kiln's view; three flexible children of relative height 1 shared the free height:
    (1020 - 87) / 3 = 311. Node fields: `+0x78/+0x7c` fixed size, `+0x90/+0x94` relative size (`0x11cf360`,
    `0x11ceea0` fixed width, `0x11cee20` fixed height), `+0x6c` / `+0x6e` alignments. Kiln now gives the view the
    relative height 1000. **[dis]** (the weight rule itself, that relative heights share the free space in proportion,
    is inferred from 311 = 933 / 3) **[rt]**
  - **Scrollbar.** Every list caller fills the scrollbar block (GameUiScrollbarParameterData, params `+0x44`, 0xc0
    bytes) after `ScrollbarParamsInit 0xa3df20`: `+0x44` ← 32 bytes of the UI config at `+0xfa84`, `+0x24` ← 32 at
    `+0xfac4`, `+0x4` ← 8 at `+0xfb04`, `+0x7c` = 1, `+0x80/+0x84/+0x88` = 24, `+0x8c` = 20, `+0x90` = 1 (the credits
    `0xb81d89…0xb81e35` and the recipe info `0xbb5dcd…0xbb5e90` identically). Kiln copied only `0xa3eeb0`'s defaults,
    whose bar has no textures. **[dis]**
  - **Focus.** ScrollLayoutEnd `0xa40160`: without useNavigation (`+0x2c`) it looks the focused frame up in the
    builder's own scope, or with useGlobalFocus (`+0x32`) in the UI's global one (`[ctx+0x10]+0x44`, `0x11b3670`),
    checks that it lies inside the content (`0x11babf0`), takes its rectangle, and when the focused id differs from the
    one it saw last and useAutoScrolling (`+0x35`) is on, scrolls it into view (`0xa403ec…0xa403fa`). The pause menu's
    entries take the global focus (its navigation scope `0xa3f790` is opened inside Kiln's content), so the default
    (useGlobalFocus off) never found them. The game's focus-navigated lists set useGlobalFocus 1 and add 0x18 to
    scrollingHotKeyMask (`scrollerContentLayouter` `0xbefa33`, `itemGridScroller` `0xbbe35c`, …); Kiln does the same.
    A wheel scroll moves the offset without changing the focused id, so the follow does not fight it. **[dis]**
  - The begin's state block holds a float the end reads as the offset (`0xa403b5`); Kiln logs it the first times it
    moves.
- **Still a guess** (one look in the game settles it): that the view weight takes nearly the whole free height (the weight rule), that the
  scrollbar block filled like the lists' draws the game's bar, and that useGlobalFocus makes the view follow the
  entries' focus for the keys and the pad (the end code says so; nothing has run). Settled by the first try: `dl` 1
  scrolls vertically, the wheel scrolls, the inner stack sizes to its entries. Kiln's experiment (`native/services/ui/menu_scroll.cpp`) does exactly what the credits do,
  in the column, and logs the sizes it saw.
- **The height (dev.29; from a screenshot and the first try, nothing new run).** The owner's 1280x720 screenshot
  (2026-11-15, 16 rows: the game's 13, Kiln's Mods, Creative, SET tools) puts an entry frame every 40 px from 20 px,
  with 53 px and 27 px between the three groups, and Quit's at 700..740 px (cut). With the first try's column of 1020
  units at 3440x1440 both fit a UI 1080 units high at any resolution: column at y 30, 1020 tall, a row 60, the gaps
  80 + 40; the 13 game rows alone are 900. **[rt]** (pixels) **[hyp]** (the 1080-unit UI; the parts of the 87 units
  below Quit). Kiln does not rely on these numbers: it reads the column's rectangle and the top of an empty,
  zero-sized stack it opens after the last entry (the same `UiVStack` / `UiNodeSize` / `UiFrameEnd` as its own stack
  in the view; the probe's top is the rows' bottom), and wraps when the rows are taller than the column
  (`menu_scroll.h`). That the probe is laid out like any child (a rectangle at the rows' bottom, no height) is a
  reading of the layout rules above, for the client pass to confirm (kiln.log's `ui: the pause menu's entries: ...`).
- **Cost and risk:** the game's layout asserts are fatal (`0xcb4c38`: "SmallDynamicArray is out of elements", then
  `0x8a0d90`): a wrong nesting ends the game on the first scrolled frame, which no run-time check can rule out before
  it. Kiln's crash guard limits that to one start per Kiln version and game build. A game update moves every one of
  these functions (each is a sigdb symbol of feature `ui.menu_scroll`).

### What Kiln does (native injection, shipped)
- One inline hook on each renderer. When the call comes from the **Settings** entry's call site (return address =
  `ui/MainMenuSettingsA` / `B`, both layout branches; `ui/SystemMenuSettings`), Kiln first calls the original renderer
  for each of its entries (main menu: a copy of the Settings descriptor with Kiln's text, so the same options/style),
  then the game's Settings entry. Activations are queued and delivered after the UI frame. ShroudForge 1.7.6 inserted
  its entry at the same point by patching both call sites (MIT, credited in game.cpp). **[dis] [rt]**
- With ShroudForge's native runtime in the process (1.7.6 to 1.7.9, also hosted by Kiln) the two Settings calls no
  longer reach the renderer from the game: each `call` enters SF's `draw_with_mods` through a near thunk, and that
  function calls the renderer (its own row when its Modloader is there, then the Settings descriptor). The renderer's
  return address is then in `kfc-runtime.dll`; the game's (`ui/MainMenuSettingsA` / `B`) is on the stack above SF's
  frame (in Kiln's build of 1.7.9: `sub rsp, 0x480` after five pushes, so 0x4a8 bytes above the renderer's return
  address **[dis]** of that DLL). Kiln's hook takes such a call for the Settings entry too (`menu_site.h`: a caller
  outside the game's image with a Settings return address within 4 KiB above it), once per UI frame. A real client
  with SF mods had no Kiln entries in the main menu before this **[rt]**; with the fix the same client shows "Mods" and the
  SET tools entry there again **[rt]** (2026-11-11, the pinned 1.7.9 build).
- Localisation: Kiln resolves the label for the game's LanguageId (§2) from the mod's per-language texts.

## 4. Fonts, colours, textures
- Fonts: `keen::UiFontResource` (one resource, 9538a125-9b82-4857-b04e-9b448b3d0a33): `fonts` = BlobArray of
  `keen::TtfFontResource` (32 B: a ResourceContentReference to a **TTF blob**; 28 fonts, 60 KB … 11 MB: Latin and CJK),
  `languageGroups` (24 B: language mask, glyph sets (fontIndex, scale, rune ranges, variant mask Bold/Italic),
  metrics, isDefault). 6 language groups; the default one (28 Latin/Cyrillic/Greek languages) uses fonts 0 / 4 / 24 / 8
  = **Harmonique Display** Regular / Italic / Bold / Bold Italic (OpenType with CFF outlines, 85 KB each) plus CJK and
  Thai fallbacks (Noto). The default group's regular font is index 0 (84896 bytes, content blob in enshrouded_053.dat).
  **[refl] [data]**
- Kiln's game-look windows load that TTF at run time through kiln.assets (`resource_list` + `resource_read` +
  `blob_read`) into ImGui; no game file is copied or shipped. **[rt]** ("the game's UI font loaded (font 0 of 28, 84896 bytes)")
- Colours (FbUiBundle `menu.*`, `keen::UiColor` packed ABGR): tileBackgroundColor #363246 (a 169), tabHighlightColor
  #caac54, tileHighlightOutlineColor #f5c558, textTabRowBgHighlightColor #896828, tabLabelIdleColor #c0cdd5 (a 171),
  basicWidgetColor #131313 (a 90), globalColors.attention #ffe06b, warning #f26e6e. **[data]** Kiln's "game" ImGui
  style uses them.
- Menu button art: `FbUiMenuButton` (standalone / in-group nine-slice textures, background, destructive, glow,
  ornament; label style `UiCharacterStyle`). Not used by Kiln (native entries draw with the game's own; ImGui windows
  use colours only). **[refl] [data]**
- UI textures: `keen::UiTextureResource` through the UI texture cache `0xa4e5f0` (docs/re/content.md §7); kiln.assets
  serves new ones. **[dis]**

## 5. Crafting menus
- Builder **CraftingMenu `0xb80050(u64 packed, i32 select)`**: `packed` byte 0 = 1 for a station's menu (HUD screens
  25 / 27 pass `{1, …, workshop}`), byte 1 = 1 for the journal's recipe book ("[GameUi] opened Crafting Menu from
  Journal"), the high dword = the station's workshop id **[rt]** (0 = the player's own crafting: "opened Personal Crafting Menu";
  "opened NPC Crafting Menu" when play+0x2c50 is set). Called every frame while a crafting menu shows. **[dis]**
- Its persistent state (hash 0xc07230f951cf77e0, 0x588 B) holds the selection; at the end of every build it stores the
  selected recipe id in GameUiData `+0x31ed8` (own crafting) or `*(GameUiData+0x31ed0)` (a station's per-entity
  state). Kiln reads it right after the builder. **[dis]**
- List rebuild `0xda5760` (on open and when the lists change) calls the category lookup **`0xd5ade0(lists, …,
  workshop id, …)`** four times; content-data hooks it (docs/re/content.md §7). It is also called from `0xbb4600` (a
  journal recipe overview). **[dis]**
- Configuration: `FbUiBundle.menu.crafting.recipes` = `keen::FbUiCraftingRecipeCollection`: `workshops` (48 workshop
  ids, id 0 = the player's own), `trees` (per workshop a BlobArray of **tabs** = `keen::FbUiCraftingRecipeGroup`
  0x38 B {groupId GUID, icon UiTexture, label LocaTagReference, sets}), each tab's **subcategories** =
  `keen::FbUiCraftingRecipeSet` 0x28 B {label, recipeSetId, entries: BlobArray<RecipeId>}; the first subcategory of a
  tab has a zero label (no header). `maxRecipeGroupCount` 15 / `maxRecipeSetCount` 28 are exactly the largest counts
  in the data (workshop 0: 6 tabs). **[refl] [data]** Corrected 2026-11 (dev.28 RE) **[static]**: current builder
  reads max groups at bundle+0xf0a8 and group walker reserves sets from +0xf0ac; a separate 512-output bound also
  applies (current lookup section below). Kiln's additions must respect each bound.
- **Factory screens (client 26430625, static analysis, 2026-11-17; nothing of it run)** **[dis]**. The HUD screen dispatcher is
  `0xbe87d0(type, mode)` on this build (PlayStateUi calls it with mode 3 at `0xc611bf`). Its switch on `type - 2`
  (`0xbe947b`, tables `0xbea0d8` / `0xbea0b4`) sends **25** (StationBasedCrafting, `0xbe956f`) and **27** (NpcCrafting,
  `0xbe94a4`) to CraftingMenu (`0xb80130` here); both first need play `+0x2c50` non-zero (else `0xbe9556`, which closes the
  screen) and read the station's workshop from play `+0x2e48`. **28 (FactoryCrafting) falls to the default case**: no
  crafting builder runs at a kiln, smelter or loom, so Kiln's `g_craft` never names it. PlayStateUi's own switch on the HUD
  screen (`0xc61135`, the one `ui/GemForgeSwitch` patterns; cases 22..45) sends 22 StorageChest to `0xb9d0f0(0)`, 41 to
  `0xb9d0f0(1)` and **28 to `0xb9d0f0(2)`**: the chest-style transfer screen with a mode. `0xb9d0f0` reads play `+0x2c50`
  and looks it up in the UI's inventory views (play `+0x3060`, `+0x3068` count, stride 0x8f70, keyed by the first dword):
  the other inventory's entity; play `+0x18` is the player's own. So play `+0x2c50` is **the entity the screen is about**
  (a chest, a factory station, an NPC: the NPC crafting code at `0x2b88e2` writes it and clears `+0x2e48`); Kiln names it
  `ui/play.target` (captured by `@ui/HudCrafting.fields` from both dispatcher cases, 2 captures agree on 0x2c50). Play
  `+0x2e48` (Kiln's `ui/play.interaction`, the same field: **not an entity**, the workshop **[rt]** for a bench) is written at
  `0x2b990a` from `+0xc` of a component the player-UI update looks up on the interaction target: not `Crafting` (`+0x10`) nor
  `CraftingInteraction` (`+0x1c`), so whether it holds a factory's workshop is open (client step 4 of docs/kiln/ui.md "Item
  icons and names"). Kiln 0.2.0-dev.29.15 reports a factory screen as a crafting screen (`IsFactory`, `Station` from
  `+0x2c50` through kiln.ui's `screen_target`, `Workshop` from `+0x2e48`, `Recipe` 0); the factory's selected recipe is not
  read (its transfer screen's state was not traced).
- Extension point (shipped): `kiln.craftmenu` v1, implemented inside content-data's existing `0xd5ade0` hook (no
  second detour): Kiln tabs and subcategories are copies of a workshop's tab array in near memory with Kiln's entries
  appended; the tree header is pointed at the copy; labels are new LocaTagReference GUIDs whose tag ids content's loca
  hooks answer. Info panels next to the menu are Kiln overlay windows (they know station, workshop and selected
  recipe). **[dis] [rt]**

## 5a. Names and icons for mod UIs (client 26430625 / server 26430253; 0.2.0-dev.29.15) **[refl] [data]**
What Kiln reads to name and draw items, workshops and recipes in mod windows (Kiln.Items' `GameNames`, Kiln.Ui's
`ItemIcons`), from the game's resident data, so without a world (main menu) and on a joined client:
- **Item icons**: the `keen::UiTextureResource` under the item's own GUID (docs/re/resources.md section 4, "Item icons as
  pictures"). `ItemInfo.iconImage` has no reader (docs/re/content.md 8.3).
- **Texts**: `keen::LocaTagCollectionResource` (one resource, 40 B: `englishData` ResourceContentReference @0, `languages`
  BlobArray @0x20 of `LocaTagCollectionResourceLanguage` 40 B {`language` keen::LanguageId (sint32) @0, `data`
  ResourceContentReference @8}). A language's content blob is a `LocaTagCollectionResourceData` {`tags`: BlobArray of
  `LocaTagResource` 24 B {`id` LocaTagId @0, `text` BlobString @4, `arguments` @0xc, `genericArguments` @0x14}}. keen::LanguageId:
  1 En_Us, 3 De_De, ... 34 Uk_Ua (the same order as Kiln.Ui's `GameLanguage` keys). A dedicated server's data has none of it.
- **An item's name**: `ItemInfo.name` @0x48, a NameLocaTag (16-byte GUID); its tag id is FNV-1a-32 of the 16 bytes.
- **Workshops**: `keen::WorkshopRegistryResource` {`npcs` @0, `workshops` @8: BlobArray of `WorkshopResource` 56 B
  {`workshopGuid` @4, `workshopId` @0x14, `name` LocaTagId @0x18, `item` ItemId @0x1c (the item that places it), `itemRef`
  @0x20}}.
- **Recipes**: `keen::RecipeRegistryResource` `recipes` @0: BlobArray of `RecipeInfo` 168 B {`recipeId` @0x10, `output` @0x30
  (BlobArray of `RecipeItemStackResource` 24 B {`item` @0x10, `count` @0x14}), `recipeName` NameLocaTag @0x68}; most recipes'
  `recipeName` is empty (their result names them).
- **The game's items by id**: every `keen::ItemInfo` resource of the game's data (4,268 on 26430625); id = FNV-1a-32 of its
  GUID (kiln.items' registry check and kiln.content agree). Kiln reads every offset from the game's reflection (kiln.types).

## 5b. Main-menu pages (client 26430625, static analysis only)
Nothing in this section has been seen running. Source: `kiln-wave/dev22/re/REPORT-main-menu.md` (Ghidra headless,
capstone, `tools/kfc`), re-checked in the disassembly where Kiln relies on it. RVAs are 26430625's (functions sit
+0xe0 from the 26404966 RVAs above). Symbols: `ui.json` feature `ui.menu_page`. Kiln's use: `native/services/ui/menu_page.cpp`.

**The PreGame block.** 0x250 bytes in the UI's hash-keyed state store (key `0xca244aa5e0cf3615`). The store frees
blocks a frame does not touch, so a pointer to it is only good inside the frame. `+0x24c` i32 current page;
`+0x54` u8 main-menu sub-page; `+0xd0` / `+0xec` current / target camera view (`keen::FbUiCharacterView`).
There is no previous page, no target page and no transition timer. **[dis]**

**Pages** (names from 0xd41440; the enum is not reflected). The PreGame frame 0xc6a050 calls the page's draw
function from a jump table each UI frame:

| id | page | draw | camera | leaves by |
|---|---|---|---|---|
| 3 | MainMenu | 0xc2a400(pregame) | mainMenu shot | entries call GoTo(8 / 5 / 6) |
| 5 | Settings | 0xbe87d0(3, 0) | postMainMenu (close-up) | returns false → GoTo(3) |
| 6 | Credits | 0xb81b20() `bool(void)` | postMainMenu | returns false → GoTo(3) |
| 8 | PlaySelection | 0xc5d720(pregame) | postMainMenu | back: sound + page = 3 |

Others: 0 HdrCalibration, 1 EULA, 2 PS5 preset, 4 UserFeedback, 7 News, 9 SessionStarter, 10 PlayJoin,
11 BaseSharingBrowser; ids above 11 draw nothing.

- **PreGameGoTo 0xd49a60** `(pregame, page)`: compares (page 10, 3 → 8, page 5), writes `+0x24c`, logs
  "[GameUi] PreGame: Go to %k". No queue, no sound. **[dis]**
- **Main menu entries** call `MainMenuEntry(pregame+8, desc)` then, when it returns true, `GoTo(pregame, page)` at once,
  inside the main menu's draw: Settings 0xc2b460 / 0xc2babe (→ 5), Credits 0xc2bcb3 (→ 6). **[dis]**
- **Credits slot.** `call 0xb81b20` at 0xc6aac0 returns to 0xc6aac5 (`ui/PreGameCreditsRet`), which jumps to
  0xc6aa9b: `test al,al; jne skip; GoTo(rsi = pregame, 3)`. Credits has one other caller (0xbd1578, the end-game
  credits state). **[dis]**
- **Camera.** The view switch after the page switch picks the target from the page (5, 6, 8, 10 → postMainMenu) and the
  camera eases there (about 97 % in 0.5 s). Page functions read no transition state: a new page draws on the next frame
  while the camera moves. **[dis]**
- **Invite.** With a pending invite (data+0x12f20) and a page above 2 the frame writes page 3 directly (0xc6a894) and
  logs "[GameUi] going from %k to MainMenu to accept invite"; no page function runs that frame. **[dis]**
- **Back sound.** PlaySelection's back: `UiPlaySound 0xa4f310(*UiApp, &copy of cfg+0x124f0)`
  (`sounds.menu.entryCancel`). **[dis]**
- **Canvas** (UI system update 0xa4fb40; GameUiFrame gets `system+0x10`): `system+0x30/+0x34` canvas in UI units
  (1920x1080 times the scale at `+0xf48`, grown along the longer side: 1920s * max(1, a), 1080s * max(1, 1/a) with
  a = (w / h) / (1920 / 1080)), `+0x38..+0x44` viewport x, y, w, h (floats, pixels, copied from `+0xf38`), `+0x48`
  pixels per UI unit = min(w / 1920s, h / 1080s). So canvas * ppu is the viewport's size exactly. `+0xf38` has two
  writers: the system's init (0xa44357: {0, 0, 1920, 1080}) and 0xa4df13 ({0, 0, width, height} of the u32 size the
  UI is drawn at): x and y are always 0 in 26430625, the viewport is the whole picture. **[dis]** (re-read 2026-11-21 for
  GitHub issue 107: the canvas is units, not pixels.)
- **Padding.** PlaySelection uses `FbUiPreGame.playMenusPadding` = 80 / 50 / 80 / 50 units **[data]**, raised to the
  safe-area insets **[dis]**. Kiln uses the 80 / 50 values (the bundle field is not read at run time).
- **Error popup.** "errorMessageWindow" (layer 31) draws while the byte at `sys10 + 0x120 + 0x550` is set
  (0xa4c050 returns `sys10 + 0x120`; 0xbae4b0 tests `+0x550`). **[dis]**
- **Title look.** `namedStyles.menu.header1`: bold, upper case, tracking 0.05, white, 46 units. The bold face is the
  default language group's glyph set with `keen::UiFontVariantMask` bit 0 (Bold): font 24 of 28. **[data] [refl]**

**What Kiln does** (feature `ui.menu_page`; needs the MainMenuEntry hook): inside its MainMenuEntry hook, when its own
MODS entry returns true, it checks `*(pregame+0x24c) == 3` and calls `GoTo(pregame, 6)` there, as the game does for its
entries. A hook on Credits answers only calls returning to `ui/PreGameCreditsRet` while Kiln's page is open: true
(stay), or false once to leave. Nothing is written into the PreGame block.

### Client checklist (needs the real game)
1. Click MODS: the camera glides to the Settings close-up; the main menu is gone on the next frame; no credits text,
   music or sound; the Kiln page shows. Log: `ui: Kiln page in the main menu opened by the MODS entry`.
2. Escape, then gamepad B, then the Back hint: each goes back once, with the back sound; the camera glides back; no
   quit dialog. Log: `closed (left; the game goes back to its main menu)`.
3. After coming back: a main-menu entry has the focus and works with keyboard and pad, as after Settings.
4. The game's own Credits after using MODS: the real credits show and leave normally.
5. A Steam invite while the page is open: the log shows "going from Credits to MainMenu to accept invite" and
   `closed (the game left the page by itself)`; the Kiln page disappears.
6. 1920x1080, 2560x1440, 3440x1440, 1920x1200: the title's left edge and the hint bar line up with PlaySelection's
   header and tiles (80 / 50 units in).
7. Settings, frame by frame: is the game's page fully drawn on its first frame while the camera still moves? (Kiln
   draws from the first frame the page shows.)
8. The mouse cursor is visible on the page and the game's cursor does not fight Kiln's.
9. Back to the main menu from a world, then MODS again: it still works (the PreGame block is new).
10. With ShroudForge's main-menu entry installed: both entries work.
11. Insert in the main menu opens the page (`opened by the menu key`); Insert on the page leaves; Insert in a world
    opens the stand-alone menu.
12. A game error popup (for example, pull the network while joining): the page steps aside and the popup takes the
    mouse, keys and pad; Escape closes the popup, not the page.
13. The page stays readable over the real close-up shot (it has no background of its own).
14. Kill Kiln.Ui's drawing (a build without the managed host): the page leaves by itself after about a second and MODS
    then opens the stand-alone menu (`no host drew it for a while`).

## 5c. The main menu's footer (client 26430625, static analysis only)
Asked for 2026-11-16: Kiln's own line on the main menu ("Kiln &lt;version&gt; - &lt;N&gt; Mods Loaded" with Kiln's mark),
taking the place of XHL-Core's. Nothing here has been seen running. RVAs of 5d9ef2f1. Symbols: `ui.json` feature
`ui.menu_footer`. Kiln's use: `native/services/ui/menu_footer.cpp`, `code_clone.h`. Work files (helpers, the XHL-Core
signature map): `/mnt/Dongus/kiln-wave/dev29/out/menufooter/`.

**The game's footer.** `MainMenuFooter 0xc189e0` (`void(void)`, 0x3f5 bytes, unwind info version 1, no handler) has one
caller: the main menu page's draw (`call` at `0xc2cb5e`, in a chunk of `0xc2a400`, right before the page's frame closes).
It opens a window named "LegalLine" (`0xa3f530(&win, {"LegalLine", 9}, 1, layer 0x22, 0, 0)` at `0xc18a2b`), a stack
(`0xa3cbd0`, `0xa49b10`) with padding = max(safe-area insets, 10 units) and spacing {0, 5}, a flexible spacer {0, 1} (the
lines sit at the bottom), then two lines:
- the build number (only when `*(*(UiApp)+0xd0)` holds a state): loca tag `cfg+0x6be0` formatted with "%s %u" and the
  build number, `NativeTextLine 0xb1da10(rbp+0x10, &text, cfg+0x10f2c, 0)` at **`0xc18c14`** (`ui/MainMenuFooterBuildLine`),
  then the object's node aligned 0x301 (`UiNodeAlign 0x11ced70`), byte `+0x3d` = 1, the colours at `+0xb0` and `+0xb8`
  to alpha 0x7f, flags u16 `+0x90` |= 0x48, `+0x92` = 0, byte `+0xd4` |= 1, the size float `+0x7c` = `cfg+0x10b98`, and
  `NativeTextDraw 0xb25160(object)` at `0xc18caf`;
- the legal line (loca tag `cfg+0x6b80`), the same way (`0xc18d05` … `0xc18da9`);
- the stack's end `0xa40080`, the window's end `0xa3fa90`. **[dis]**

No base relocation touches the function; its 32 relative fields are 31 to the outside (20 calls) and one jump inside
(`je 0xc18cbb`, no state: no build number). **[dis]** (`tests/code_clone_test.cpp` reads it from the installed exe.)

**XHL-Core 2.1.0** (`dinput8.dll`, sha256 `9eba3c3d…`, MIT; Ghidra decompile in `kiln-wave/xhl/re/core/all.c`) draws its
main-menu entry here. A background thread (`FUN_180045510`) scans the game's exe **file** (mapped from disk; the live
image only when the file cannot be mapped) until it finds its sites, then rewrites four `E8 rel32` calls in the live code
to near stubs (`FUN_180030160`, mutex `Local\XHL.CodePatch.<pid>`, every thread suspended, each site's live byte must be
`E8`, its old target becomes the hook's "original"). It never scans again once they are in. Its LegalLine scanner
(`FUN_18005b370`) finds the "LegalLine" `lea`, the window call (`0xc18a2b`), the "%s %u" `lea` and the
`NativeTextLine` / `NativeTextDraw` calls after it and the `movss xmm0, [rcx+0x10b98]` size read:
- `0xc18c14` (the build number's `NativeTextLine`) → `FUN_1800596a0`: when its status text is not empty, it opens a
  frame "XhlStartupStatus" (FNV-1a id), builds its own text with the game's `NativeTextLine` (the build line's style
  and mode), aligns it 0x101, sets `+0x3d`, flags `|= 0x4a`, the size `cfg+0x10b98 × 1.1`, colour `0xff80ff60` (green)
  when all is well or `0xff3030ff` (red) for a problem, draws it with `NativeTextDraw`, makes it clickable while its
  input mode is on (a click opens its diagnostics), then calls the game's `NativeTextLine`. So its line sits right above
  the build number. The text: "All mods loaded" (35 languages, `PTR_s_All_mods_loaded_1800be0f8[lang × 27]`) once its
  startup check finished with no issue, else the issue ("XHL: Game updated … Click to view", "Mod conflict detected.
  Click to view", …), else nothing.
- `0xc18a2b` (the LegalLine window) → `FUN_180059bf0`: the window's input argument forced on while its status line is
  interactive.
- two calls in the main menu's entry code (found through "pregameMainMenuWindow", "mainMenuEntries",
  "PreGameStateStorageWindow", "doButton()", "Settings", "MainMenu") → `FUN_180059560`, `FUN_180059640`: keyboard and
  gamepad reach of the status line (its activation opens XHL's menu).
**[xhl]** (decompile; not run)

**What Kiln does** (feature `ui.menu_footer`): at start kiln.ui copies `MainMenuFooter` from the exe file into memory
near the image (`code_clone.h`: same layout, outside references rewritten, the copy checked against the original before
use, the original's unwind information registered for it) with its call at `0xc18c14` sent through Kiln's thunk, and
hooks the function's entry to run the copy (XHL-Core scans the exe file, not the live code, and at install checks only
that its own call sites are still calls). The thunk builds and draws Kiln's line
exactly as the game builds its build-number line (the same style, mode, fields; text with 7 no-break spaces in front of
it for the mark), reads its rectangle (`UiNodeRect`, the last layout's) and then calls the game's `NativeTextLine` for
the build number. Kiln never writes to the function's body, so XHL-Core's scan and its call patches go in as before in
either order. The copy's window call (`0xc18a2b`, `+0x4b`: the last byte of `ui/MainMenuFooter`'s pattern) goes through
a second thunk. Both thunks call the game's functions, unless XHL-Core is in the footer and reports a problem: then they
call XHL-Core's patched targets (its stubs: `jmp [rip+0]` with no register move for these two sites: their entries in the
site list handed to `FUN_180030160` carry register 0xffffffff; only `FUN_180059560`'s gets a `mov r9, reg`), so XHL-Core draws its red line under Kiln's and its window
patch lets the line take clicks; its two main-menu patches work as they do without Kiln. Kiln knows XHL-Core by the
export `XhlNativeGameBuild` of the module the build-number call's stub leads to, and reads its state through the first
call of the detour `FUN_1800596a0` whose target has the getter's shape `xor ecx,ecx; xor eax,eax; lock cmpxchg
[rip+rel32],ecx; ret` (`FUN_180043af0`, the int `DAT_180109160`: 0 = no problem, its green line or nothing; else the
problem its red line names, via `FUN_180043bd0`). Kiln looks at the two live calls each frame its copy runs (XHL-Core
patches from its own thread, maybe late). Its green "All mods loaded" is left out (Kiln's line reads "Kiln ... with
XHL-Core - N Mods Loaded"); where the getter is not found, XHL-Core's line is always drawn. At the first use kiln.ui compares the live
function with the file's: a patched call is logged (module and offset); any other change (or base-relocated bytes) leaves
the footer to the game's function. The game font's metrics for the mark's gap: Harmonique Display Regular, 1000 units per
em, ascent 980, descent 220, space and no-break space 191 (fontTools over font 0 of the UiFontResource) **[data]**.

### Client checklist (needs the real game)
Without XHL-Core:
1. Main menu: Kiln's mark and "Kiln 0.2.0-dev.29.11 - N Mods Loaded" right above the build number, in the footer's
   font, size and colour, aligned like the two game lines; N is what the Mods page counts as Loaded. kiln.log:
   `ui: the main menu's footer: Kiln's line goes above the build number`, `... says "Kiln ..."`, `... Kiln's line at ...,
   its mark at ...`.
2. The mark sits in the gap before "Kiln" (not over the text, not far from it), its height about the text's; check at
   1920x1080, 2560x1440, 3440x1440 and a UI scale change.
3. Settings, Credits, PLAY and back, the Kiln page (MODS) and back, a world and Return to Main Menu: the line and its
   mark are there only on the main menu page, and come back with it.
4. Kiln settings → `mainMenuFooter` off: the line and the mark go at once, the game's footer is as before; on again:
   they come back.
With XHL-Core 2.1.0 (as a chain DLL):
5. Kiln's line reads "Kiln <version> with XHL-Core - N Mods Loaded" (in Kiln's language) in the same place; with no
   XHL-Core problem its green "All mods loaded" is not shown at any moment. kiln.log names XHL-Core: `a module patched
   the game's footer call at +0x234 to <XHL-Core's file>+0x596a0` (and at `+0x4b` to `+0x59bf0`), `XHL-Core's footer
   line is at <file>+0x596a0; ... shows while XHL-Core reports a problem (it takes clicks)`, `XHL-Core reports no
   problem: Kiln's line only`.
6. With an XHL-Core problem (see the report's check list for how to provoke one): XHL-Core's red line ("XHL: Game
   updated · Click to view" or "Mod conflict detected. Click to view") right under Kiln's line, above the build number;
   a click on it opens XHL-Core's startup diagnostics window; kiln.log `XHL-Core reports a problem: its red line shows
   under Kiln's`.
7. XHL-Core's Insert menu, its plugins and its other features work as before, in the main menu and in a world.
8. `mainMenuFooter` off: XHL-Core's own line (green or red) is back above the build number, as XHL-Core draws it.

## 6. Other strings worth knowing
"[GameUi] … opened" log lines exist for every menu (Backpack, Map, Journal, Skill Tree, Settings, Social, NPC dialog,
…): the game logs screen changes itself, at level 3. Window ids: `titleScreenWindow`, `pregameMainMenuWindow`,
`PlayStateBaseWindow`, `recipeListWindow`, `ingredientSelectionWindow`, `itemViewerWindow`, `ContextMenuWindow`,
`textInputWindow`, `tooltipWindow`. **[dis]**

## 7. Not done / open
- Native custom screens (a mod screen drawn with the game's widgets): needs the layouter / text / image calls
  (`0xa3ca30` frames, `0xb1d930`+`0xb25080` text, `0xa48c10` images, …) as an API; Kiln ships ImGui screens in the
  game's look instead.
- Buttons inside the game's crafting panel (next to "Craft"): the recipe detail layout `0xc75580` / `0xc79ec0` was not
  mapped; Kiln's panels sit beside the menu.
- Gamepad: the game loads XInput at run time (XInput1_4 / 1_3 / 9_1_0, `XInputGetState` by name) **[dis]** (strings).
  Kiln hooks that export after it loads: while Kiln has the input the game gets an idle pad and ImGui gets the pad
  (navigation; B = Escape). **[hyp]** until tried with a pad; native entries use the game's own pad navigation.
- (answered at run time: the tabbed menu stays HUD screen 4 on every tab; M opens the map as 5.)

## Runtime (GE-Proton11-11, 1920x1280)
Kiln deployed with MenuButtons, ui-test and NewItems; a LOCAL save, world slot 0x0002
(world1.ksd), Offline. ui-test **25 PASS, 0 FAIL**.
```
ui: game UI hooked: screens on, main menu on, pause menu on, crafting on, language on
ui: gamepad: xinput1_3.dll XInputGetState hooked (idle for the game while Kiln has the input)
ui: game screen None/0 -> Preload/0
[host] lifecycle: loading the mods in the game's menu (UI frame 1)
ui-test: 13 PASS, 0 FAIL at load
ui-test: PASS 600 UI ticks (UI thread 2)
ui: menu entry 'Kiln' activated            (mouse, main menu)
ui: the game's UI font loaded (font 0 of 28, 84896 bytes)
ui: menu entry 'ui-test' activated         (keyboard: Down + Enter, main menu)
saves: world loaded: slot 0x0002 (world1.ksd) …
ui: menu entry 'ui-test' activated         (pause menu)
kiln.craftmenu: 2 tab(s) and 2 subcategories in the crafting menu (1 of 1 header(s) written; limits 15 tabs, 28 per tab)
new content: the crafting menu lists 5 new recipe(s) (3 of 3 subcategory list(s) rewritten)
menubuttons: recipe 0x222fcad4 at workshop 0x00000000      (the crafting panel's button)
menubuttons: screen InGame -> Crafting (Play/StationBasedCrafting, … workshop 0x4da88f73, recipe 0x8e78753c)
ui-test summary: 25 PASS, 0 FAIL, nothing waiting; UI thread 2
```
- Main menu and pause menu: Kiln's entries drawn by the game's renderers in the game's look (pause menu: with the
  game's attention marker), above Settings; mouse clicks and keyboard focus (Down/Enter) activate them; the game's
  own entries (Settings, Return to Main Menu, Quit) work unchanged. **[rt]**
- A Kiln screen (game look, the game's own Harmonique Display font) opened from either menu or F7: list, slider, text
  field (one character at a time), image work; Escape closes the screen only (the game's pause menu stays, in game
  the pause menu does not open); keys work afterwards (no stuck keys seen). **[rt]**
- Crafting: Kiln tabs and subcategories drawn by the game's crafting menu with their labels; the info panel follows
  the selected recipe and its button takes the click while the game keeps the keyboard. **[rt]**
- The UI thread is the game's main thread (managed thread 2, the one that also steps the client world). **[rt]**
- Not covered: a gamepad (none attached), language switching (game in English), a joined remote server, HDR.

## Native value, slot and text sites (dev.28; client 26430625 / server 26430253)

C `5d9ef2f1…`, S `f0d824be…`. Dossier chapters 05a/05b/09, values-ui REPORT delta/VERIFY and sessions 2/6.
VERIFY: 18 confirmed/one corrected (detail record ownership), plus slot gating/units/finalizer corrections below.
Drawing sites are **client-only**. `[seen]` denotes coordinator sessions, not new tests performed for this note.

### Presentation boundary [static], tested ordering [seen]

Current UiApp global C `0x283f4a8` is frame-scoped: `app=*UiApp; data=*(app+0xd0); state=*data;
play=*(state+0x38)`. Last step is a **dereference**, not an inline play object. Frame code publishes/clears UiApp
at `0xbd1116/0xbd1639`; it can be null in world callbacks. Bundle `app+0xc8`, line style bundle `+0x10bbc`,
balancing registry play `+0x4667d8`. Never keep UiApp/play/game pointers across frame, update or world lifetime.

`player_ui` C `0x2b8430` is `void(ProgramContext*)`, **not void(play*)**. Capture MID `0x2b8c1e` has RSI=play,
ECX=vanilla health before store. It reloads RSI per entity; verify play's local entity `+0x18`, not merely the last
captured pointer. Held MID `0x2bbb70`: RSI=play, EDI=held pide, RBX=NetworkFuel* or null. The no-held branch also
reaches it with zero/zero; its short conditional branch requires trampoline relocation.

Play u32 current/max pairs: health `+0x578/+0x57c`, stamina `+0x584/+0x588`, mana `+0x59c/+0x5a0`, held fuel
`+0x2e40/+0x2e44`. Adjacent `+0x580/+0x58c/+0x5a4` are **not generic maxima**. HUD C `0xc68c60` is
`void(int mode)` and gates on local player/nonzero healthMax; held reader C `0xbf5d80` reads held pair. Original
update owns all ECS work; only presentation values are substituted after its matching return. Session 1 UPDATE/HUD/
detail/slot ran on thread 392; session 2 on 400. This is tested ordering, not arbitrary-thread UI safety.

Models: play pointer/count `+0x3060/+0x3068`, stride 0x8f70, owner `+0`; views pointer/count model `+0x2d8/+0x2e0`,
stride 0x48. ItemView prefix: inventory page u32 `+0`, slot index byte `+4`, ItemId `+8`, count u32 `+0x10`,
pide u32 `+0x14`, durability u32 pair `+0x18/+0x1c`, fuel u16 pair `+0x20/+0x22`, level u32 `+0x24`, rarity byte
`+0x2c`, durability-initialized `+0x34`. Offsets `+0xc/+0x28` exist, but palette/base-damage semantic names were not
independently verified. Stackable views can have pide=0; numeric equality alone is not replicated provenance.

### Temporary bars and units [static], drawing [seen]

| Site | Register contract |
|---|---|
| Detail bar MID `0xc1401a`, before `0xc12890` | RCX=Bar*, RDI=thumbnail descriptor, `[RDI+0x18]`=detail-owned record |
| `void draw_bar(Bar*,float width,u16 align)` `0xc12890` | RCX/XMM1/R8W; current/max text from 32-bit words |
| Slot bar MID `0xc05895`, before `0xd79920` | RCX=Bar*, RBX=ItemView*; fraction drawing |

Bar: fraction f32 `+0`, current/max 32-bit `+4/+8`, animation/show-text bytes `+0xc/+0xf`, color `+0x2c`,
GameText `+0x38`; detail object extends through `+0x4f`. Mutate only temporary bar output, not its source view.

**Correction:** detail `[rdi+0x18]` is an at-least-0x180-byte **copy owned by ItemDetail**, not a live model-array
pointer. First 0x48 B matches ItemView, but thumbnail also reads `+0x60/+0xbc/+0xd4/+0xdc`. Synthetic branch
`0xc1028c` rebuilds a substitute-item prefix with pide/count/fuel=0. Instance identity is optional; do not write
through it or treat it as authoritative inventory.

Slot fuel gates: resource exists, descriptor `+0x7d` set, context `+0x51` clear, item!=0, resource flags `+0x5d8`
bit 0x2000. **No fuelMax gate**: fraction helper `0xd34ac0` returns 1.0 for max=0, so slot 0/0 draws full.
Detail skips max=0 at `0xc13ebe`. Both can select durability when view `+0x34` and the world setting permit it.

Units differ: **detail water=charges**, slot/held water=raw fuel units; durability raw; HUD respective vital units.
`u32 units_to_charges(u32)` C `0xd49090` dereferences frame UiApp, calls `u32 charge_amount(registry)`
C `0xe9fa40`: resource `+0x20`, u32 `+0xfc`; missing resource→1, zero divisor leaves raw value (effective divisor 1).
Do not call outside native frame. Field-getter signature is unique only in its 22-byte scope (three image-wide hits).
[seen] Detail displayed `1000098 / 2000000` and quarter fill; hotbar quarter fill, HUD health/mana sentinels.
This proves 32-bit storage, **not unsigned high-bit formatting**. Held nonzero water and visual stamina identification
remain unverified; session 6 only exercised the no-held zero/zero branch.

### Text construction is not layout integration [static] [seen]

ItemDetail C `0xc109f0` is `void(detail*,u8 flag)`. Collection MID **`0xc1216e`** is before mainContentArea
ScopeEnd, after selected body. A branch jumps directly there; `0xc12165` is not the common merge point. Preview
mode 4 exits before this scope and has no such callback.

`void* NativeTextLine(void* out,const GameText*,const void* style,u8 mode)` C **`0xb1da10`** returns out;
allocate 0x150 B/align 16 (constructor writes through `+0x140`). Body uses style bundle `+0x10bbc`, mode 1;
vanilla numeric bars use mode 0. GameText 24 B `{utf8 pointer,u64 byteLength,u64 hash}` is borrowed, not owned.
Existing Kiln non-ASCII signed-byte FNV convention is [source]; VERIFY independently checked only ASCII.

**Required draw:** `void NativeTextDraw(node*)` C **`0xb25160`** shapes via `0xb2d820`, submits via `0xd0ee60`,
then tail-jumps to generic ScopeEnd C `0xa40080`, including shaping failure. Constructor already does state `+0x16`
finalization. ScopeEnd alone does **not draw**, and an extra ScopeEnd after draw is not needed. Session 1's
scope-only probe drew nothing; session 2's constructor+draw produced a short line inside the panel.

**Session 6 corrects the general native-footer claim:** short line contained; long line did **not wrap**, escaping
to screen edge. German Ü/ö/ß/ü drew correctly but escaped too. Sixteen numbered rows drew in order, yet their
bottom-anchored block **covered vanilla stats/description** and did not share usable scrolling. A visible scrollbar
was not proof of reserved vertical space. Splitting/wrapping alone cannot fix that overlap. General native reflow/
scroll integration is unproved; dossier's item-text design now uses a separate explicitly opened owned viewer,
not this raw footer recipe. Two UI scales, long vanilla description and chest movement were not checked. The later
client-exit SIGSEGV was not investigated or attributed to this probe.

### Slot geometry and safe read boundaries [static], geometry also [seen]

`void* ItemSlot(out32*,descriptor*,options*,context*)` C `0xc046c0` returns its hidden output buffer: float XYWH
`+0`, state/action bytes `+0x10..+0x1f`. Descriptor `+0` points to view, including empty slots. State `+0x10` comes
from focus predicate `0xa4d2d0`; **mouse-hover versus gamepad focus is not settled**. Inner MID `0xc05433` has
RAX→float XYWH, R15→descriptor. Session 1 saw initial zero bounds then 80×80 slots; reject initial zero/nonfinite
bounds. Native UI units are not automatically viewport pixels; viewport/pixels-per-unit mapping still needs checking.

World reads are not frame APIs. Generic GetComponent S `0x64ed50` / C `0x9b2ec0` takes explicit world and returns
borrowed `{ptr,u64 size}`; GetEntityTransform S `0x64fd40` / C `0x9b3eb0` is `const void*(World*,u32 entity)`.
Use completed-world Post snapshots; existing public World/Time wrappers are server-bound, and Client.Tick is Pre,
not automatically a post-job sampler. Replicated pides already equal server IDs (see [network.md](network.md));
client-local/catalog/pide-zero views have no justified server identity. Holder sync excludes merely opening a chest.

[refl] Experience 20 B, Level 16 B start with UInt32Attribute header (rootId `+0`, storageOffset u16 `+4`,
flags/count bytes `+6/+7`); NetworkFuel 4 B, NetworkPlayerAttributes 256 B. They are fixed components, **not mod-stat
maps**. [lab] Independent CharacterData XP/currency survived restart for two character fixtures. Fake-peer command
used a fixed synthetic Player permission subject and got admin refusal: transport/storage proof, **not real-player
authentication**. Rich blocks reuse existing ImGui widgets/input/textures and add no new engine RVA. Complete
catalogs, immutable-snapshot, custom-stat and owned-viewer contracts remain in dossier chapters 05a/05b/09.

## Item detail layout and value sources for native tooltip lines (equipment wave P8a; client 26430625)

C `5d9ef2f1…`, static only (Ghidra headless without auto-analysis, capstone); nothing here has been seen running.
Design and the guided client steps: the equipment wave's TOOLTIPS.md. **[dis]** unless marked.

**Tree.** `ItemDetail 0xc109f0` builds `doItemDetailView()` › `mainColumn` › `contentRow` › (`item Base Info Column` with the
header/thumbnail `0xc12df0`) and `mainContentArea` (`0xa3c740`, vertical). For mode `*(detail+0x170) == 0` and
`detail+0xbc == 0` the body is a **scroll area** (`0xa3e020` @`0xc117e7` … `0xa40160` @`0xc11fb0`) holding a **vertical content
list** (`0xa3c790` @`0xc11841` … `0xa40080` @`0xc11fa4`): header line `0xc43850`, stats block `0xbfccf0` (@`0xc11939`), the
description, type lines, the "%s %u" line. Modes 1..5 jump to `0xb48a00`, `0xcf30c0`, `0xc771d0`, `0xceac80`, `0xc00760`.
`DetailLinesAppend 0xc1216e` lies after the scroll area has closed, which is why session 6's rows did not scroll and
covered the vanilla content. Insertion points inside the list: `0xc11fa0` (end; reached by the fall-through and `je` at
`0xc11e0c`/`0xc11e15`) and `0xc1193e` (after the stats block). ItemDetail's only caller is `0xc101b0` (16 callers).

**Nodes.** Scope helpers create a child of the layouter's current parent (`0x11c4c60`); the UiNode is at scope `+8`.
Setters: `+0x6c` alignment (`0x11ced70`), `+0x6e` (`0x11ced60`), `+0x90` size mode f32×2 (`0x11cf360`; fraction of the parent,
0 = fit), `+0x80/+0x84` preferred size (`0x11cf230`), `+0x98` / `+0xa8` 16-byte edge boxes (`0x11cf100` / `0x11cf210`), `+0xb8`
layout kind (1 `0x11ceff0`, 2 = vertical `0x11cf380`) with spacing `+0xc0`. The shaper `0xb2d820` limits the width to the
node's last layout rectangle only when `+0x90`'s width is non-zero: the description sets `{1.0, 0.0}` and wraps; a bare
constructor + draw shapes one line. TextLine: `+0x38` alignment, `+0x3d` = 1 on every detail text, `+0x90` flags (`0x40`
colour at `+0xb0`, 8 bytes; `0x08` size at `+0x7c`), `+0xd4` bit 0 with a colour.

**Values.** The view (`player_ui` chunk `0x2aff40`) takes level from `ItemState.itemLevelUi` (u8), rarity from
`ItemState.itemRarityUi` and `+0x2d/+0x2e` from `PerkContainerNew`. The header model `0xb20100` (rbp+0x250) holds the rarity
label (`0xd424c0`, loca bundle `+0x1de0` + 0x10 × rarity) at `+0x78` and the rarity colour at `+0xa0` (bundle `+0xd5f4` + 8 ×
rarity; `+0xd5ec` none, `+0xd61c` unique). The stats model `0xb1e350` (rbp+0x810) uses the **client registry's** ItemInfo:
enhancement rows (≤ 5 × 0x70 B from `+0x580`), base level `+0x7cc`; numbers from `0xe779a0` (damage = (seed × creep × level
+ seed) × dmgMod × speedMod), `0xe77a00`, `0xe77b20` with the client's BalancingTable, drawn by `StatValue 0xc2ddd0(label,
float)` (callers in `0xb8bbd0` and `0xbfcd6b`). The server's `baseDamageUi` is not read here, so a per-instance power change
is not shown by the detail view without a client hook.

## Current crafting-tree lookup (dev.28; client 26430625 / server 26430253)

C `5d9ef2f1…`, S `f0d824be…`; dossier chapter 16a, blocks-a VERIFY claims 1–2 confirmed. [static] [refl] [data]
Current C builder **0xb80130**, categories **0xd5aec0** supersede older C 0xb80050/0xd5ade0 in §5 for this build.
`u64 CraftMenuCategories(Lists* out,void* allocationContext,u32 workshop,u8 filterMode)` clears lists, finds workshop
in the client FbUiBundle, walks its parallel tree and returns listed recipe total. RCX/RDX/R8D/R9B; preserve opaque
allocation context. Requires live UI app/config and original output storage, not a server tick call. Kiln's existing
content hook already owns this seam; no second detour is needed. Nonzero filter excludes one output equipment type
(`d33510(ItemInfo+0x104)==0x18`), **not a general unlock flag**.

Station CraftingInteraction.workshopId `+0x1c` selects the workshop; station templates carry **no tab layout**.
Collection at bundle `+0xf098`, 24 B: workshops BlobArray `+0`, trees `+8`, maximum groups/sets u32 `+0x10/+0x14`.
Tree 8 B; groups 0x38 B (id GUID `+0`, icon `+0x10`, loca label `+0x20`, sets `+0x30`); sets 0x28 B (label `+0`,
recipeSetId `+0x10`, recipe IDs `+0x20`). Rebase every copied self-relative BlobArray, not only the outer tree.
WalkGroups C `0xd5acd0`, WalkSets `0xd5ef00` are inspection anchors, not extra public calls.

Current bundle has 48 workshops/48 trees, declared and observed maximum 15 groups/28 sets. Separately, set walker
bounds **512 recipe outputs per group**, u16 count `+0x9820`, 0x4c-byte records. These are independent capacities;
increasing UI maxima alone does not expand output storage. First set receives the tab's special header handling.
No new joined-client station-menu presentation check was run; schema/sync/ownership designs stay in chapter 16a.

## Inventory slots: clicks and marks (dev.30; client 26430625, static, nothing of it run)

What Kiln's `ui.slots.click` (kiln.ui.inventory, `Ui.Inventory`) rests on. No new game address: it reuses the slot sites
above (ItemSlot `0xc046c0` raw link, SlotGeometry mid `0xc05433`, GameUiFrame) and the HUD screen byte. Feature:
`sigdb/symbols/client.d/ui-hooks.json`.
- **The press is not the game's.** Kiln does not hook the game's own slot input (the ItemSlot result's action bytes
  `+0x10..+0x1f` are not decoded). kiln.ui's window procedure sees every left/right press first (raw record and window
  message, paired as mouse_press.h pairs the middle and side buttons), hands it to `subscribe_pointer_press`
  subscribers, and keeps a taken press from the game the way a key binding's press is kept (the raw record's button flag
  taken out through the game's own GetRawInputData import, the message not passed on); releases always pass. **[src]**
- **Hit test:** the core hit-tests the press against the last completed slot frame's outer rectangles (the last drawn
  wins: a slot drawn over another, a dragged item's preview). The frame must be this client session's and at most
  250 ms old. **[src]**
- **Pixels:** `pixel = viewport + unit * px_per_unit` on the game's canvas (`ui/sys.viewport`, `ui/sys.px_per_unit`,
  chapter 05a), then scaled by client (or overlay display) size / the picture's size (viewport w + 2x, h + 2y) when they
  differ. Through 0.2.0-dev.30.2 the divisor was the canvas size, which is in units: at 3840x2160 (2 px per unit) every
  mark and hit test landed at twice its place (GitHub issue 107, verified against the reporter's numbers).
  Static only; check `ui-hooks.c1` (marks on the slots at two resolutions/UI scales) confirms it, which is also 05a-c5.
  Expected to hold: the game renders its UI at the swapchain size and kiln.ui's overlay draws at that size. **[hyp]**
- **Screens with a pointer** (HUD screen byte, play `+0x1cd8`): 4 InGameMenu, 8 PlayerCrafting, 9 Backpack,
  22 StorageChest, 25 StationBasedCrafting, 27 NpcCrafting, 28 FactoryCrafting, 41 the chest-style transfer screen's mode 1
  (`0xb9d0f0(1)`; which game screen that is was not traced). 1 InGame is refused: the HUD's hotbar is drawn through
  ItemSlot there too, but a click is an attack. **[refl] [dis] [rt for 4, 9, 22, 25]**
- **Identity:** the slot's ItemView `+0x14` (pide) is the server's entity id for a replicated item (05a section 6,
  network.md). A plain stack has none (pide 0): the owner's dev.30 pass Ctrl+clicked Archaic Essence IV x10 (backpack)
  and Springlands Dirt x12 (chest) and both views said pide 0 **[rt]**. Kiln reports it as the client entity and leaves `server_entity` 0; `ui-hooks.c2` compares it with the
  server world's own inventory in a hosted world. **[dis]**
- **Hover byte:** ItemSlot's result `+0x10` (`ui/slot.hoverCandidate`) is still only a candidate (mouse hover or gamepad
  focus). **dev.30.5 static read (client 26430625) [dis]:** ItemSlot calls the focus test `0xa4d2d0` twice (`0xc04800` and
  `0xc04f12`, both with `dl = 1`) on its slot frame's scope (`rbp-0x70`); `0xa4d2d0` passes the scope's node (`+8`) and the
  UI's user index (`[[UiApp]+0x10]+0x44`, low 4 bits) to `0x11b95a0`, which compares the node's id (`+0x30`) with the
  layouter's focused id of that user (`[[node+0x50] + user*8 + 0x68]`), and with `dl = 1` also accepts a focused descendant
  (`0x11b93a0`, a walk over first child `+0x18` / next sibling `+0x10`); `0x11992b0` clears that focus. So the byte is the
  layouter's per-user navigation focus, the one the d-pad / stick and keyboard navigation move (section "How an entry
  works"), not a pointer test of its own. Whether the pointer moves that focus too was not found statically (the writers
  of `+0x68` were not located in a linear sweep). Kiln's `Ui.Inventory.Selected` (0.2.0-dev.30.5) is the first slot draw of a
  fresh frame with the byte set; a client check must move selection with the d-pad and then the mouse to confirm which
  moves the focus. `ui-hooks.c4` logs it next to the cursor's slot (read while the cursor rests on the slot, not at a
  verdict: Kiln
  then has the input for its check window and the cursor is on that window). A gamepad slot action waits for it. **[hyp]**

### Slice 2 (dev.30): the chest's entity and panels beside the screens
- **The chest's entity:** PlayStateUi's switch sends 22, 41 and 28 to `0xb9d0f0(mode)`, which finds the other inventory by
  play `+0x2c50` (`ui/play.target`). kiln.ui reads it on all three since dev.30 (`screen_target`; a change is a screen
  change). For a chest the game replicated it is the server's id (the same rule as an item's pide). The host test checks
  it against the holder of the chest's drawn slots (`KUI_VIEW_HOLDER_MATCH`); `ui-hooks.c6` checks it in a game against
  the server world's inventory and position. **[dis] [src]**
- **Panel placement:** no new site. kiln.ui.inventory v2's `screen_view` takes the latest slot frame's outer rectangles
  (all, the local player's, the container's by holder) through the same mapping as marks; Kiln.Ui docks a panel to their
  right (else left). Game test pending: `ui-hooks.c5`. **[src] [hyp]**

### For the next slices (not built)
- **A button in the chest screen (where Enlarge / Enchant sits):** the chest-style transfer screen is `0xb9d0f0(mode)`
  (PlayStateUi's switch `0xc61135`: 22 → mode 0, 41 → 1, 28 → 2), the other inventory found through play `+0x2c50`. Its
  button row, the button renderer it uses, and the upgrade-storage action (`consumedUpgradeStorageAction`,
  `ecs.InventoryTransferAction` are strings in the exe) are not traced. The menu entries' recipe (a frame through the
  game's own entry renderer inside the game's layout scope, identified by return address) is the model.
- **The game's own slot tint:** ItemSlot's descriptor `+0x7d` and context `+0x51` gate the fuel bar; the slot's background
  colour parameter is not mapped.
