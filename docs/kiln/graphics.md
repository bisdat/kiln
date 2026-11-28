# Kiln's graphics tools: frame cap, DLSS settings, readout, an upscaler behind FSR 3 (0.2.0-dev.29.18)

Kiln's own versions of what players use OptiScaler, DLSSTweaks and frame cap mods for, on Kiln's own hooks. Client
only. Everything is off until the player turns it on: a player who changes nothing gets the game as before, with no
hook on DLSS or on the game's FSR 3, no new hook on the game's library loads, and nothing done in a frame but two reads
of a flag. Phase B adds the choice of what runs behind the game's own FSR 3 option: the game's FSR 3, AMD FSR (FSR 4
where the card runs it) or Intel XeSS ([below](#behind-the-games-fsr-3-phase-b)). Code:
`native/services/ui/graphics.cpp` (plain logic in `frame_pacer.h`, `graphics_settings.h`, `graphics_aside.h`,
`ngx_param.h`; host test `tests/graphics_test.cpp`), `managed/services/Kiln.Ui/UiGraphics.cs`. How the game uses DLSS:
[docs/re/client-upscale.md](../re/client-upscale.md).

## Where the player sets them
- In the game: the Kiln menu (Insert, or **Mods** in the game's menus), tab **Graphics**. Changes apply at once and are
  saved to `kiln/kiln.json` half a second later (the file's comments and other settings stay). Under "Now" the page says
  what the tools do: the cap, the frame rates, the DLSS in use (mode, render and output size, preset), the upscaler
  behind the game's FSR 3 and why the chosen one does not run, what they step aside for, and when a restart is needed.
- In the launcher: Settings, card **Graphics** (the same `kiln.json` entries, the upscaler choice with its files and
  what ran in the last game; it also names the tools Kiln steps aside for, from the game folder). On the Steam Deck:
  Deck mode's Settings, rows **Upscaler behind the game's FSR 3**, **AMD FSR version**, the files' rows and **Upscaler
  now** ([launcher/deck.md](launcher/deck.md#upscaler)).
- By hand: `kiln/kiln.json`, section `graphics` (the game reads the file again every 2 seconds; a file that is not valid
  JSON keeps the settings in use, with a warning in kiln.log):

| key | values | default | applies |
|---|---|---|---|
| `fpsCap` | 0 (off) or 20..500 frames a second (1..19 count as 20) | 0 | at once |
| `dlssPreset` | `default`, `J`, `K`, `L`, `M` | `default` | when the game next sets DLSS up |
| `dlssScale` | `game` (DLSS's own), `perMode`, `custom` | `game` | when the game next sets DLSS up |
| `dlssRenderScale` | 0.33..1: with `custom`, the render width and height as a share of the output | 0.67 | " |
| `dlssQualityScale`, `dlssBalancedScale`, `dlssPerformanceScale`, `dlssUltraPerformanceScale` | 0.33..1 each: with `perMode` | DLSS's own: 0.667, 0.58, 0.5, 0.333 | " |
| `readout` | `true` / `false` | `false` | at once |
| `upscaler` | `game` (the game's own FSR 3), `amd` (AMD FSR: FSR 4 where the GPU runs it, else FSR 3.1), `xess` (Intel XeSS): what runs in place of the game's FSR 3 | `game` | at the next FSR 3 frame (in a world with FSR 3 chosen in the game), else when a world loads; Kiln's hooks go in only when it is not `game` at the game's start ([below](#behind-the-games-fsr-3-phase-b)) |
| `amdUpscaler` | `auto` (FSR 3.1 until FSR 4 is confirmed in this game), `fsr3` (FSR 3.1), `fsr4` (AMD's selection: FSR 4 where it runs, else FSR 3.1; experimental): with `amd` | `auto` | at the next FSR 3 frame |

"When the game next sets DLSS up": loading a world, or changing the DLSS mode or the resolution in the game's own
settings. One more rule: Kiln hooks DLSS only when a DLSS setting or the readout is on **when the game starts**. A DLSS
setting turned on for the first time while the game runs applies from the next start (the page says so); one changed
from one non-default value to another applies at the next DLSS set-up. kiln.log names what was read
(`graphics: frame cap 60 fps, DLSS preset K, ...; DLSS is hooked when the game loads NVIDIA's DLSS core`).

## Frame cap
- Paces the frames the game renders, one per 1/fps second. Right after each of the game's presents, in Kiln's present
  hook, the game's render thread waits for the next slot before it goes back to the game: the finished frame is shown
  at once, and the game starts its next frame at the slot, as a frame limiter in the game would. So the cap adds no
  latency to a finished frame. The cost: frames are shown when they are done, so display times follow the game's render
  times (a frame that takes longer shows later), where holding each frame to its slot would show them evenly but later.
- It waits on a high-resolution waitable timer for most of the gap (a plain timer where the system has none) and spins
  on the clock for the last part; how long it spins is learnt from how late the timer wakes (0.25 to 4 ms, 2 ms at
  first). A frame that comes late by more than half an interval starts a new schedule from itself, so a hitch never
  turns into a burst of catch-up frames. Nothing is held while it waits (no lock, no overlay state).
- **What wins:** the lowest limit. With the game's own frame limit (`fpsCap` in the game's settings) or V-Sync at a
  lower rate than Kiln's cap, the game's frames come later than Kiln's schedule and Kiln never holds them: the game's
  limit decides. At a higher rate, Kiln's cap decides.
- **With V-Sync:** a cap equal to the display's refresh rate can stutter (the cap and the display's clock drift
  against each other, and now and then a frame misses its refresh). Set it a few frames a second below (57 on a 60 Hz
  display, 141 on 144 Hz, best with a variable refresh rate display), or use a divisor of the refresh rate (60 on
  120 Hz). A cap below the refresh rate that does not divide it gives uneven frame times with V-Sync on.
- It counts **rendered** frames. A later frame generation phase will add frames of its own between the game's: those
  are output frames, and the cap stays on the rendered ones (a cap of 60 with 2x generation shows 120), as frame cap
  add-ons for frame generation mods do. The readout already shows both counts (equal today).
- It lives in Kiln's present hook, so it needs Kiln's overlay hooks: with the overlay's drawing off from the start
  (`KILN_UI_OVERLAY=0`, kiln.json `"ui": {"overlay": "off"}`, the overlay crash guard, Streamline, OptiScaler) the cap
  is off too, and the page and kiln.log say why. When the overlay's drawing turns off during a session (a Vulkan
  failure: [ui.md](ui.md#when-the-overlay-fails-during-a-session)) the hooks stay, and so does the cap; the readout goes
  with the drawing.

## DLSS settings
For players whose game upscales with DLSS (an NVIDIA RTX card and DLSS chosen in the game's settings). On another card
the game never sets DLSS up and these settings do nothing (the page says "The game has not set DLSS up in this session").
- **Render preset.** The game's DLSS (`nvngx_dlss.dll` 310.9.1) has models for presets E, F, J, K, L and M; Kiln offers
  J and K (DLSS 4's transformer models) and L and M (DLSS 4.5's newer ones). `default` leaves the game's hint and DLSS's
  own choice. Where the game sets no preset of its own, the NGX SDK code in the game asks for NVIDIA's defaults: K for
  DLAA, Quality and Balanced, M for Performance, L for Ultra Performance. The readout and the Graphics page show that
  letter; kiln.log adds the raw value (`render preset K (the game's default; raw 0x8000000B)`, bit 31 marking the
  SDK's default). Kiln sets the documented hints `DLSS.Hint.Render.Preset.{Quality, Balanced, Performance,
  UltraPerformance}` right before DLSS is created, after the game's own (the game sets the same four, and DLAA's, before
  every creation, so "default" brings its own back at the next one); **DLAA keeps the game's preset**.
  The NVIDIA app's DLSS override, when the player set one there, wins over any app's hint, Kiln's included; Kiln cannot
  read back which preset runs, so the readout shows the one asked for. A newer `nvngx_dlss.dll` without a preset falls
  back to its default.
- **Render size.** `game`: DLSS's own size for the game's DLSS mode. `perMode`: a scale for each of Quality, Balanced,
  Performance and Ultra Performance (the defaults are DLSS's own; Ultra Quality keeps DLSS's). `custom`: one scale for
  every mode DLSS offers (a mode DLSS answers with 0x0, Ultra Quality in the game's DLSS, is left alone: `graphics: DLSS
  Ultra Quality for 1920x1080: not offered ...`). Kiln puts its own step in front of DLSS's optimal-settings callback
  (the documented `DLSSOptimalSettingsCallback` parameter): DLSS answers, then Kiln sets `OutWidth`/`OutHeight` to the
  output times the scale, within DLSS's own range for that mode (`DLSS.Get.Dynamic.Min/Max.Render.*`). The game renders
  at that size; a size within a pixel of DLSS's own is left as DLSS's. **DLAA always renders at the output's size.** A
  scale outside DLSS's range for the mode is held to the nearest size DLSS allows: the Graphics page shows the size in
  effect and says so, and kiln.log names the range (`graphics: DLSS Quality for 2560x1440: Kiln's scale asks for
  845x475, outside DLSS's range ...`). Where that range is narrow, the setting does little in that mode.
- How: Kiln hooks five exports of NVIDIA's NGX core (`_nvngx.dll`, the driver's; Proton has it too):
  `GetCapabilityParameters`, `CreateFeature`, `CreateFeature1`, `EvaluateFeature`, `ReleaseFeature`. The game's
  statically linked NGX SDK reaches them through `GetProcAddress`; no game code is patched and only NGX's own parameter
  object is used, through the documented parameter names. Kiln's work inside those calls runs under a fault guard: a
  fault turns Kiln's DLSS settings off for the session (logged), never the game's call. Should the game unload the
  core and load it again, Kiln hooks the new one by the same rules (`graphics: NVIDIA's DLSS core ... was loaded
  again`).

## Behind the game's FSR 3 (phase B)
The game offers its own FSR 3 on every graphics card. With FSR 3 chosen as the upscaler in the game's graphics settings,
Kiln can run another upscaler in its place, on the same inputs (colour, depth, motion vectors, jitter, camera, frame
time, exposure), into the same command buffer. Nothing is called DLSS, and nothing gets around a vendor's hardware
limits: AMD's and Intel's own libraries decide what runs on a card.

### The choice
- **The game's own FSR 3** (`game`, the default): nothing changes, and Kiln puts no hook on the game's FSR 3.
- **AMD FSR** (`amd`): AMD's FidelityFX SDK 2.4.0. With `amdUpscaler` `auto` (and `fsr3`), Kiln runs FSR 3.1 (3.1.6)
  until FSR 4 is confirmed in this game: FSR 4 drew dark scenes too dark on an RX 9070 XT (0.2.0-dev.29.19 betas 1 and
  3). `fsr4`, shown as experimental, leaves it to AMD's own selection: FSR 4 (4.1.2) where the card and driver run it,
  else FSR 3.1.
- **Intel XeSS** (`xess`): Intel's XeSS SDK 3.0.2, its Super Resolution only (which reports itself as version 2.0.2),
  on Intel, AMD and NVIDIA cards.

**When it applies:** at the game's next FSR set-up: a world load, or a change of FSR's quality, the resolution or the 3D
render scale in the game's settings. Kiln hooks the game's FSR 3 pass and adds what the chosen upscaler needs to the
game's Vulkan device only when an upscaler other than the game's own is chosen **when the game starts**: one chosen for
the first time while the game runs applies from the next start (the reason `restart`). The game's own FSR quality modes
and render scale stay what decides the render size.

**What each graphics card gets** (AMD's and Intel's lists; in a real game so far: FSR 4 on an RX 9070 XT, dark scenes too dark
(0.2.0-dev.29.19), and on an RX 6900 XT, a black screen (0.2.0-dev.31.11); FSR 3.1 and XeSS on an NVIDIA card):

| card | AMD FSR (`fsr4`, experimental) | AMD FSR (`auto`, `fsr3`) | Intel XeSS |
|---|---|---|---|
| AMD RX 9000 | FSR 4 (FP8 model) | FSR 3.1 | XeSS |
| AMD RX 7000 | FSR 4 (INT8 model, new in SDK 2.4.0) | FSR 3.1 | XeSS |
| AMD RX 6000 | FSR 4 (AMD's RDNA 2 INT8 model, new in SDK 2.4.0; black on an RX 6900 XT in 0.2.0-dev.31.11, see below) | FSR 3.1 | XeSS |
| AMD Strix Halo, Strix Point, Ryzen Z2 handhelds | FSR 4 (INT8, a lightweight model on the smaller chips) | FSR 3.1 | XeSS |
| Steam Deck (Van Gogh) and older AMD cards | FSR 3.1 (AMD's list has no FSR 4 for them; AMD's selection decides) | FSR 3.1 | XeSS |
| NVIDIA, Intel | FSR 3.1 | FSR 3.1 | XeSS |

AMD's selection needs no particular driver version for FSR 4 on Vulkan (AMD's notes); whether a card gets it is AMD's
answer at the FSR set-up, and the status says which ran.

**No black picture (0.2.0-dev.31.13).** After each set-up Kiln looks once at the upscaler's picture: about 30 frames in,
it reads a grid of 1,024 points of the upscaler's output and of the game's own picture going into it. If the output is
black or not a number while the game's picture is not black, FSR 4 gives way to FSR 3.1 for the rest of the game (any
other upscaler to the game's own FSR 3), kiln.log says what it read, and the readout says "AMD FSR 4 is not running: its
picture came out wrong on this graphics card (...): FSR 3.1 runs until the game restarts". A loading screen or a fade is
not judged: Kiln looks again a second later. After the check nothing of it runs.

### The files
AMD's and Intel's libraries are not inside Kiln. The launcher downloads them when the player asks: Settings, Graphics,
"Upscaler files" (or the offer under the choice), on the Deck the files' rows in Settings. Before a vendor's first
download the vendor's licence is shown in full with "I agree, download"; then progress with Cancel, and Remove.
Details: [Upscaler files](#upscaler-files) below.
- Choosing AMD FSR or Intel XeSS without the files is allowed: the game's own FSR 3 runs, and the status says why
  (`filesMissing`). The launcher offers the download right under the choice.
- The game loads them only from `<game>/kiln/vendor/<vendor>/<version>/`, never from the game's own folder, and only
  after each file's SHA-256 matched the one built into the runtime (the same values as the launcher's manifest). A file
  that differs is not loaded (`filesBad`).
- The game's Graphics page says whether the chosen vendor's files are there ("AMD FSR 2.4.0: the files are in
  kiln/vendor/amd/2.4.0", or "not in this game yet. Download them in the Kiln launcher").

### What runs now, and why not
The Graphics page's "Now" block, the launcher's Graphics card and the Deck's "Upscaler now" row say what upscales behind
the game's FSR 3 option: the game's own FSR 3, AMD FSR 4, AMD FSR 3.1, Intel XeSS, or nothing because FSR 3 is not the
game's chosen upscaler right now (DLSS or none chosen, or no world loaded). With the version and the render and output
size while it runs: the upscaler's own version, not its SDK's (FSR 4.1.2 or 3.1.6 from AMD's SDK 2.4.0; 2.0.2 for XeSS,
the Super Resolution version Intel's SDK 3.0.2 reports). When the chosen one does not run, one of these reasons follows
(the same English in the game and the launcher; shown here for AMD FSR):

| code | number | text |
|---|---|---|
| `restart` | 1 | AMD FSR runs from the next start of the game: Kiln sets it up when the game starts. |
| `filesMissing` | 2 | AMD FSR's files are not downloaded yet, so the game's own FSR 3 runs. |
| `filesBad` | 3 | A file of AMD FSR is not the one Kiln expects (its size or SHA-256 differs), so Kiln does not load it. Download it again. |
| `loadFailed` | 4 | A file of AMD FSR did not load, so the game's own FSR 3 runs. |
| `device` | 5 | Your graphics card or driver lacks a Vulkan feature AMD FSR needs, so the game's own FSR 3 runs. |
| `createFailed` | 6 | AMD FSR could not be set up for this resolution, so the game's own FSR 3 runs. |
| `runtimeFailed` | 7 | AMD FSR failed while it upscaled. The game's own FSR 3 runs until you restart the game. |
| `fault` | 8 | AMD FSR or Kiln's code around it faulted. The game's own FSR 3 runs until you restart the game. |
| `aside` | 9 | Kiln steps aside for OptiScaler, so it does not change the game's FSR 3. |
| `noSignatures` | 10 | Kiln does not know this game build's FSR 3 yet, so the game's own FSR 3 runs. A Kiln update for this game build fixes that. |
| `inputs` | 11 | The game's images are not what AMD FSR takes, so the game's own FSR 3 runs. |
| `unavailable` | 12 | AMD FSR is not available in this build of Kiln or on this system, so the game's own FSR 3 runs. |
| `waiting` | 13 | AMD FSR is ready. It starts as soon as the game runs FSR 3: load a world with FSR 3 chosen in the game's graphics settings. |
| `noVulkan` | 14 | Kiln did not see the game set up its graphics device in this session. Restart the game for AMD FSR. |
| `unknown` | any other | AMD FSR does not run (reason 15 from a newer Kiln), so the game's own FSR 3 runs. |

A reason that ends the chosen upscaler for the session (`runtimeFailed`, `fault`) leaves the game's own FSR 3 running
until the game restarts. The game's English detail (a missing file's or Vulkan extension's name, a fault) is shown as a
muted "Details:" line and written to kiln.log. The numbers are kiln_ui.h's `KILN_UI_UPSCALE_WHY_*`; the codes are the
state file's.

**In the launcher:** "Now" while the game runs, from what Kiln in the game wrote to `<game>/kiln/state/graphics.json`
(rewritten only when it changes; `schema/state-graphics.schema.json`); "Last game" after the game closed, while the
choice is still the one that game had and its reason still holds; otherwise "When you play": what Kiln will do from the
choice, the files in `kiln/vendor/` and the tools in the game folder ("With FSR 3 chosen in the game's graphics
settings, Kiln runs AMD FSR in its place: FSR 4 where your graphics card and driver run it, else FSR 3.1.").

### Stepping aside
Kiln's upscaler stays off, with the reason `aside` and the tool's name, next to: OptiScaler, NVIDIA Streamline
(`sl.interposer.dll`), a DLSS frame generation mod (`EnshroudedVKFG.ini`), another tool's upscaler DLLs next to the game
(`amd_fidelityfx_*.dll`, `libxess*.dll` in the game's own folder: AMD's loader would find those first), the game's FSR 3
pass already patched by another module, and Kiln's overlay hooks off. DLSSTweaks and another DLSS loader leave it alone
(they only touch DLSS). See the table in [Living next to other tools](#living-next-to-other-tools).

### Limits
- Only behind the game's **FSR 3** option. With DLSS chosen in the game, Kiln's DLSS settings apply instead; the game's
  XeSS entry is a name only (the game has no XeSS code) and is not where Kiln's XeSS runs.
- Upscaling only: no frame generation in this release (AMD's frame generation DLL is downloaded with AMD FSR for a later
  phase, and not loaded).
- Kiln does not change the render size or sharpness of FSR: the game's FSR quality and render scale decide.
- Kiln finds the game's FSR 3 pass by pattern in each game build; on a build it does not know yet the swap is off
  (`noSignatures`) until a Kiln update.
- Steam Deck: AMD FSR 3.1 and XeSS; FSR 4 only where AMD's selection says it runs.
- FSR 4 has not drawn a correct picture in this game yet (RX 9070 XT: dark scenes too dark; RX 6900 XT: black up to
  0.2.0-dev.31.12, seen on 31.11). Kiln checks its picture once after each set-up and runs FSR 3.1 when it is black or not a number.
  First reports from testers decide the defaults.

## Readout
A few lines in the top right corner of the game, drawn by Kiln's overlay:
- first, in a warm colour, "Restart the game to use AMD FSR" (or Intel XeSS) while the chosen upscaler runs only from the
  next game start (it was chosen after the game started with another; the Graphics page, right under the choice, and the
  launcher's Graphics card lead with the same, and the Graphics page shows it once as a notice when the choice is made);
- rendered and output frames a second and the mean frame time over the last half second;
- `DLSS Quality: 1707x960 -> 2560x1440, preset K` while the game upscales with DLSS (the render size is this frame's:
  dynamic resolution may lower it); else "Not DLSS now" (the game's FSR 3, XeSS or no upscaler: Kiln does not read the
  game's own upscalers) or, without Kiln's DLSS hooks, "DLSS: read from the next game start";
- (phase B, with an upscaler other than the game's own chosen, or Kiln's swap in) `Behind FSR 3: AMD FSR 4 (4.1.2):
  960x540 -> 1920x1080`, or the game's own FSR 3 and "AMD FSR is not running: its files are not downloaded (launcher:
  Settings, Graphics)": the reason in kiln.log's short words, to keep the corner small (the Graphics page says it in
  full, [What runs now, and why not](#what-runs-now-and-why-not));
- the frame cap and whether it is on.
No latency: the game sets no latency markers, so there is nothing for Kiln to read honestly. Turning the readout on makes
the overlay draw every frame (an ImGui frame and one submit per present); off, it costs nothing.

## Living next to other tools
Kiln's frame cap and DLSS settings step aside, with one kiln.log line (when one of them is on at start) and a line on
the Graphics page, when another tool does the same (the files are looked up in the game folder at start;
`graphics_aside.h`):

| found | frame cap | DLSS settings | upscaler behind FSR 3 |
|---|---|---|---|
| OptiScaler's DLL (any name; [ui.md](ui.md#optiscaler)), with or without its overlay menu | aside | aside | aside |
| `sl.interposer.dll` in the game folder or `Streamline/` (NVIDIA Streamline, frame generation mods) | aside | aside | aside |
| `EnshroudedVKFG.ini` (a DLSS frame generation mod) | aside | aside | aside |
| `dlsstweaks.ini` (DLSSTweaks) | stays | aside | stays |
| `nvngx.dll` or `_nvngx.dll` next to the game (another DLSS loader: NGX loads it before the driver's) | stays | aside | stays |
| NGX's exports already patched when the core loads (another tool hooks DLSS) | stays | aside | stays |
| another tool's upscaler DLLs next to the game (`amd_fidelityfx_*.dll`, `libxess*.dll`), or the game's FSR 3 pass already patched | stays | stays | aside |
| Kiln's overlay hooks off (`KILN_UI_OVERLAY=0`, kiln.json `ui.overlay` "off", the crash guard) | aside | aside | aside |

The readout stays where Kiln's overlay draws. Kiln's overlay itself steps aside for NVIDIA Streamline and for OptiScaler
with its own menu or frame generation, unless the player switches on "Draw Kiln's overlay anyway" (kiln.json
`ui.overlay` "on", since 0.2.0-dev.31.25: [ui.md](ui.md#drawing-it-anyway-kilnjson-on-and-kiln_ui_overlay1)); the
readout then draws, and the frame cap, the DLSS settings and the upscaler still step aside for those tools as in the
table. Kiln never patches over another tool's hook and never changes another loader's files or mods. The launcher's Graphics card names the same tools from the game folder, and which of Kiln's
tools step aside for each (`GameStatus.graphicsStepAside`, with `upscaler` for the third column). Example log line:
> graphics: frame cap 60 fps, DLSS preset K; Kiln's frame cap and DLSS settings step aside for OptiScaler (dxgi.dll)

## Wine and Proton
The same on Linux: the frame cap's timer and clock are Wine's (the learnt spin margin covers a timer that wakes late),
and DLSS through Proton's NVAPI loads the same `_nvngx.dll`, which Kiln hooks the same way. With nothing turned on,
nothing runs.

## For native plugins (kiln_ui.h)
Two slots after `set_texts` (byte offsets 1016 and 1024; the table is 1032 bytes; check `header.size` and the pointer):
- `graphics_status(&KilnUiGraphicsStatus)`: what the tools do now, for anyone: `flags` (`KILN_UI_GFX_CAP_ACTIVE`,
  `_DLSS_HOOKED`, `_DLSS_ACTIVE`, `_DLSS_RESTART`, `_DLSS_PRESET`, `_DLSS_SCALED`, `_COUNTING`, `_DLSS_LIMITED`), `aside_cap` /
  `aside_dlss` (`KILN_UI_GFX_ASIDE_*`), the cap, rendered and output frames a second and the frame time (while the cap or
  the readout counts), the swapchain's size, and the DLSS in use: its mode (`NVSDK_NGX_PerfQuality_Value`, -1 none), the
  preset asked for, this frame's render size and the output size. Set `size` first.
- `graphics_set(&KilnUiGraphics)`: Kiln.Ui's only (the player's settings); a native plugin's call is refused with
  `KILN_ERR_UNAVAILABLE`. Since phase B its two last words are `upscaler` (`KILN_UI_UPSCALER_GAME` 0, `_AMD` 1,
  `_XESS` 2) and `amd_upscaler` (`KILN_UI_AMD_UPSCALER_AUTO` 0, `_FSR3` 1, `_FSR4` 2 since 0.2.0-dev.29.19); still 48 bytes.
- Phase B's status fields follow at offset 72 (`KilnUiGraphicsStatus` is 248 bytes, `KILN_UI_GRAPHICS_STATUS_SIZE_V2`;
  a 72-byte caller gets the fields before only): `upscaler`, `amd_upscaler`, `upscale_running`
  (`KILN_UI_UPSCALE_RUN_OFF` 0, `_NOT_FSR` 1, `_GAME_FSR3` 2, `_AMD_FSR4` 3, `_AMD_FSR3` 4, `_XESS` 5), `upscale_reason`
  (`KILN_UI_UPSCALE_WHY_*`, the list above), `upscale_flags` (`KILN_UI_UPSCALE_HOOKED`, `_FSR_SEEN`, `_FILES_OK`,
  `_DEVICE_OK`, `_FELL_BACK`, `_AMD_PINNED`), `aside_upscale`, `upscale_version` (`(major << 22) | (minor << 12) |
  patch`), the FSR 3 pass's render and output size, and `upscale_detail` (128 bytes, English).
There is no managed (mod API) member for it in this release.

## Upscaler files
What Kiln runs behind the game's FSR 3 option (phase B) and a later frame generation phase loads: AMD's FSR DLLs and
Intel's XeSS DLL. The launcher downloads them only when the player asks, on a screen that says what will happen.
Contract: [launcher/contracts.md](launcher/contracts.md) section 5.15; code `crates/kiln-manage/src/vendor/`,
`launcher/src/components/UpscalerSettings.tsx`.

| component | version | files | size | licence | from |
|---|---|---|---|---|---|
| AMD FSR (`amd-fsr`) | FidelityFX SDK 2.4.0 | `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` (FSR 3.1.6 and FSR 4.1.2), `amd_fidelityfx_framegeneration_vk.dll` (not loaded in this release) | 72,981,808 bytes | MIT licence | `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` at tag `v2.4.0`, `Kits/FidelityFX/signedbin/` |
| Intel XeSS (`intel-xess`) | XeSS SDK 3.0.2 | `libxess.dll` (needs the VC++ runtime 14.40 or later, as Proton and current Windows have) | 77,795,704 bytes | Intel Simplified Software License (October 2022) | `intel/xess` at tag `v3.0.2`, `bin/` |

- **Where the player sees it:** the launcher's Settings, card **Graphics**, part **Upscaler files** (`#upscalers`)
  under the upscaler choice, for a client only (a dedicated server draws no frames). Out of preview since 0.2.0-dev.29.18
  (it was behind the launcher's preview `upscalers` before; that setting now changes nothing). One row per component:
  not downloaded (with the size and the host), downloading (progress, Cancel), downloaded (version, size on disk,
  Remove), update available, not available in this build, failed (the reason, Try again). Choosing AMD FSR or Intel
  XeSS without its files shows an offer right under the choice with a Download button, which takes the same steps.
- **On the Steam Deck:** Deck mode's Settings has a row per component (A downloads, cancels a download, or asks to
  remove; the row's end says the state, with the percentage while it downloads). The licence screen shows the whole
  text (the right stick scrolls it), with "I agree, download" and B to go back.
- **On demand, AMD's too:** nothing ships inside Kiln (the owner's decision, although AMD's MIT licence would allow it),
  and nothing is downloaded before the player's click.
- **The licence:** before a vendor's first download a dialog shows what it is, the size, the host
  (`raw.githubusercontent.com`), where it goes, the vendor's licence text in full and what the host sees, with "I agree,
  download" and Cancel. The agreement is kept in the launcher (`<data>/state/vendor-licenses.json`) with the hash of the
  text; a changed text asks again. The names are true: AMD's files are under the MIT licence, Intel's under Intel's own
  licence; nothing here is called DLSS.
- **Checked:** the files come only from the tag-pinned URLs of Kiln's manifest on the vendor's own GitHub organisation;
  each is accepted only with the size and SHA-256 Kiln pinned (`tools/release/vendor_pin.py`, from copies checked against
  the vendors' git blobs); one that differs is deleted and the row says so.
- **Stored** in `<game>/kiln/vendor/<vendor>/<version>/` with a receipt, never in the game's own folder (other loaders
  and Kiln's step-aside list look there). Remove deletes that folder; an update keeps the old version until the new one
  verified. The game checks each file's SHA-256 again before it loads it, against the hashes built into the runtime,
  never the receipt's (contracts.md 5.15, "What Kiln in the game reads"). The game's Graphics page reads only names and
  sizes, from the same manifest built into Kiln.Ui.
- **Later:** NVIDIA's frame generation (`nvidia-dlssg`, from NVIDIA's own GitHub, never inside Kiln) is a new component
  in the same manifest, with NVIDIA's licence.

## Upscaler behind the game's FSR 3: how it works (0.2.0-dev.29.18)
Phase B, for the curious and for testers ([the player's side above](#behind-the-games-fsr-3-phase-b)). In the game's own settings the player chooses FSR 3, as before; `graphics.upscaler` chooses what runs behind
it: the game's own FSR 3 (`game`, the default: nothing changes), AMD FSR (`amd`) or Intel XeSS (`xess`). Kiln never calls
any of it DLSS. Code: `native/services/ui/upscale.cpp` (the swap), `upscale_amd.cpp` (AMD FSR), `upscale_xess.cpp`
(XeSS), the interface `upscale_backend.h`, plain logic `upscale_logic.h` and `upscale_amd_map.h` (host test
`tests/upscale_test.cpp`), the vendor files `vendor_file.h`, the device set-up `device_merge.h`; sigdb
`client.d/upscale.json` (feature `upscale.swap`).
- **When it applies.** Kiln hooks the game's FSR 3 pass only when an upscaler other than `game` is chosen **when the
  game starts**, and only then adds what that upscaler needs to the game's Vulkan device (extensions and features, as
  the game's device is made). A choice made while the game runs applies at the game's next FSR 3 frame when it is the
  start's choice or `game`, else from the next start (status reason `RESTART`); `amdUpscaler` the same. (Up to
  0.2.0-dev.29.19 beta 1 a choice waited for the game's next FSR 3 set-up, which the game does not do for every FSR
  quality change: seen at native AA.)
- **What happens.** The game builds its frame as a list of its own render commands, then runs them. Where the game would
  write its FSR 3 passes, Kiln makes the game flush its pending barriers (as the game does for DLSS) and writes one
  command of its own instead; when the game runs that command, Kiln has the chosen upscaler record its work into the
  frame's own Vulkan command buffer, at the same place. Its inputs are the game's own, as the game's FSR 3 gets them:
  colour (linear HDR), depth (reverse-Z), motion vectors (Kiln copies them into a format AMD's API takes; the values
  are unchanged), the reactive and transparency masks the game makes for its FSR, the jitter, sizes, camera planes,
  field of view, pre-exposure and frame time (Kiln's own measured time while the game's is 0, about 500 frames after
  a world load). The game gives its FSR 3 no exposure texture; its pre-exposure is 1 right after a load and follows
  its eye adaptation in the world (0.077 seen, 29.19 beta 3), and Kiln passes it on. Kiln asks AMD FSR for its own
  auto-exposure (AMD's recommendation). FSR 3.1 looks right with that; FSR 4 drew dark scenes far too dark (29.19 betas
  1 and 3, RX 9070 XT) and was not dark through OptiScaler, fed from the game's DLSS path with pre-exposure 1 and no masks.
  So FSR 4 gets pre-exposure 1 and none of the game's masks; FSR 3.1 keeps the game's (which part matters is not proven).
  Test knobs, one per line in `<game>/kiln/control/amd`, read at each FSR set-up and named in kiln.log's `set up for`
  line (the first value is the default): `check auto|off` (off: no output check, so a black FSR 4 stays, for an A/B),
  `exposure auto|one`, `preexposure auto|game|one`, `reactive auto|on|off`,
  `transparency auto|on|off`, `masks auto|on|off` (both), `hdr on|off`, `colour linear|srgb|pq`, `dynres auto|on|off` (auto: no DYNAMIC_RESOLUTION flag for FSR 4, which
  otherwise runs its dynamic-resolution network; OptiScaler never sets it), `planes auto|game|swap` (auto: the camera planes
  swapped for FSR 4, as OptiScaler passes them with inverted depth). kiln.log's `upscaled N frames` lines
  give the range of the game's pre-exposure since the line before. History is dropped when the game sets FSR 3 up, after a load and when a size changes.
- **AMD FSR.** AMD's signed Vulkan DLLs from the FidelityFX SDK 2.4.0, from `<game>/kiln/vendor/amd/2.4.0/` (the
  launcher's Settings, Graphics card). Kiln checks each file's size and SHA-256 against the pins built into Kiln (the same
  values as the launcher's manifest; `tools/release/vendor_pins_header.py --check`) before it maps them, keeps them
  open so they cannot change (on Windows; under Proton that binds only Wine's own programs, contracts.md 5.15), and
  maps them only from that folder. With `fsr4` AMD's own selection picks FSR 4 where the GPU and driver run it (RX 9000,
  RX 7000 and 6000, some recent APUs), else FSR 3.1; `auto` and `fsr3` pin FSR 3.1. Where FSR 4 is
  chosen but does not start, Kiln runs FSR 3.1 instead and says so in kiln.log. The device additions: everything AMD's
  Vulkan backend uses when the GPU offers it (subgroup size control, 16-bit floats, synchronization2, AMD's buffer
  markers and coherent memory, descriptor indexing), 16-bit storage, float16/int8 and integer dot product, each only
  where the GPU has it. Of AMD's sample's FSR 4 set (cooperative matrix, FP8, compute-shader derivatives, 8-bit storage,
  the memory model, maintenance4), since 0.2.0-dev.31.13 what the GPU's own FSR 4 model declares
  (`upscale_amd_map.h` `choose_fsr4_set`, from the SPIR-V inside AMD's 4.1.2 DLL):
  - **where AMD's selection takes an INT8 model** (an AMD GPU without cooperative matrix + FP8: RX 6000, RX 7000, the
    APUs): `VK_KHR_compute_shader_derivatives` (`computeDerivativeGroupLinear`), the "INT8 set". FSR 4's own auto
    exposure declares it on every model, and RDNA 3's and the lite model's INT8 prepass too; no INT8 shader declares
    8-bit storage, the memory model or maintenance4, so they stay off. Up to 0.2.0-dev.31.12 these GPUs got none of the
    six, and FSR 4 drew a black screen on an RX 6900 XT;
  - **where AMD's selection takes the FP8 model** (RX 9000): none of the six, as since 0.2.0-dev.29.21 (with all of them
    AMD's Vulkan FSR 4 drew dark scenes too dark on an RX 9070 XT, 29.19, while OptiScaler's pure-Vulkan FSR 4 from the
    same DLL ran correctly on a device without them);
  - not an AMD GPU: none (AMD's selection runs FSR 3.1 there).
  `kiln/control/amd` `fsr4ext auto|off|on`, read when the game makes its device (from the next game start): `off` none
  of the six (as 29.21 to 31.12), `on` all six (as up to 29.20). kiln.log says at the game's start which set the device
  got and why ("AMD FSR on AMD Radeon RX 6900 XT: the game's device also gets FSR 4's INT8 set, ..."), and the AMD FSR
  set-up line names it ("INT8 device: compute-shader derivatives on").
- **The output check (0.2.0-dev.31.13).** `upscale_probe.h`, `upscale_probe_logic.h`, `shaders/output_probe.comp`. Once
  per context, 30 FSR frames after its first, Kiln records one 256-thread compute dispatch of its own into the frame's
  command buffer right after the upscaler's work: it reads a 32x32 grid of the game's colour (through the game's view,
  SHADER_READ_ONLY_OPTIMAL) and of the output (the game's R16G16B16A16_SFLOAT view as a storage image, GENERAL), changes
  no layout, and writes per point whether it is finite and its luminance into an 8 KB host-visible buffer. Eight FSR
  frames later (the GPU is done with that frame) Kiln reads it: black (output mean and brightest point at most 2% of the
  colour's) or NaN (a quarter of the points or more not finite), while the colour is not black and is finite. FSR 4 then
  gives way to FSR 3.1 for the rest of the game (the context is made again for the same sizes; FSR 3.1 is checked too),
  any other upscaler to the game's own FSR 3 (reason `runtimeFailed`). kiln.log: one line with the readout, for
  example `AMD FSR 4.1.2's output check: the picture is black (output: mean luminance 0, brightest 0, 0 of 1024 points
  not finite; the game's colour: mean 0.183, brightest 4.21, 0 not finite): AMD FSR 3.1.6 runs in its place until the
  game restarts`, or `...'s output check: the picture is fine (...)`. A black or not-finite game picture (a load, a
  fade) is not judged: again 60 frames later, up to 120 times. Nothing of it runs per frame after the check.
- **Intel XeSS.** XeSS Super Resolution from the XeSS SDK 3.0.2: Intel's own `libxess.dll`, from
  `<game>/kiln/vendor/intel/3.0.2/` (the launcher's Settings, Graphics card), checked and kept open as AMD's files are,
  mapped only from that folder, and never patched or hooked (Intel's licence: the software unmodified, also while it
  runs). The library reports its Super Resolution version, 2.0.2 for the SDK 3.0.2 (Intel's 3.0 SDK added multi-frame
  generation in other libraries): Kiln loads a Super Resolution 2.x that has every export it calls, and refuses 1.x and
  later majors (kiln.log: "libxess.dll loaded: XeSS SDK 3.0.2 (Super Resolution 2.0.2)"). On a GPU that is not Intel's
  it runs its cross-vendor (DP4a) path. Kiln adds what XeSS itself lists for the Vulkan instance and device at the
  game's start (XeSS chosen later: reason `restart`), picks the first XeSS quality preset whose input range holds the
  game's render size, copies the motion vectors into the format Intel's guide names (values unchanged), and makes a new
  XeSS context at each FSR set-up (the old one goes four FSR frames later). Code `xess_backend.h` (host test
  `tests/xess_test.cpp`), `upscale_xess.cpp`; needs the VC++ runtime 14.40 or later, as Proton and current Windows have.
- **Failures.** A missing or changed file, a GPU without what the upscaler needs, a set-up that fails: the game's own
  FSR 3 runs and the status says why. A failure while upscaling: that frame gets a plain stretch of the image, then
  the game's own FSR 3 for the rest of the session. A black or NaN picture (the output check above): FSR 3.1 in place
  of FSR 4, else the game's own FSR 3. Every call of Kiln's around the game's runs under a fault guard.
- **Stepping aside.** The swap stays off, with the reason, for OptiScaler, NVIDIA Streamline, a DLSS frame generation
  mod (`EnshroudedVKFG.ini`), AMD FidelityFX or Intel XeSS DLLs in the game folder (another tool's; `ASIDE_UPSCALER`),
  the game's FSR 3 pass already patched by another module, and Kiln's overlay hooks being off (the swap needs Kiln's
  Vulkan hooks).
- **What the player sees.** The readout's line `Behind FSR 3: AMD FSR 4 (4.1.2): 960x540 -> 1920x1080`, or the game's
  own FSR 3 with the reason the chosen one does not run (kiln.log's short words, `upscale_logic.h` `why_text`); the
  Graphics page and the launcher read the same status and say the reason in the players' sentences.
  After the output check made FSR 3.1 run in place of FSR 4, the readout's line `Behind FSR 3: AMD FSR 3.1 (3.1.6): ...` has
  `AMD FSR 4 is not running: its picture came out wrong on this graphics card (the picture is black): FSR 3.1 runs until
  the game restarts` under it, and the status's English detail says the same (`upscale_logic.h` `fsr4_black_text`); the
  Graphics page and the launcher show AMD FSR 3.1 running (their text for FSR 3.1 behind an FSR 4 choice: "AMD's
  selection chose FSR 3.1").
- **kiln.log** (`graphics: upscaler: ...`): the choice at start and the hooks, the files' check, what was added to the
  Vulkan device and what the GPU lacks, the versions AMD offers for the GPU, each set-up (`AMD FSR 4.1.2 set up for
  960x540 -> 1920x1080`), and every fallback with its reason.
- **Limits:** after a fault inside an upscaler's own code Kiln never calls that upscaler again (it may hold a lock), so
  its GPU memory stays allocated until the game ends; the game's FSR 3 runs meanwhile.
- **Not proven yet:** FSR 4 with a correct picture on any GPU (seen: dark scenes too dark on an RX 9070 XT, 29.19;
  black on an RX 6900 XT, 31.11); whether the INT8 set of 0.2.0-dev.31.13 gives the RX 6900 XT a picture (else the
  output check runs FSR 3.1 there); the output check in a real game; FSR 4 on RX 7000, the APUs and the Steam Deck;
  AMD FSR 3.1 on an AMD card on Windows. Seen: AMD FSR 3.1 and XeSS on an NVIDIA card (Proton); the swap itself on the
  real client with a plain stretch (spike B0, RTX 3080 Ti, Proton).

Status reasons (`KilnUiGraphicsStatus.upscale_reason`, stable numbers): the table in
[What runs now, and why not](#what-runs-now-and-why-not); its texts are the game's and the launcher's.

## Not in this release
FSR 3's ratio and RCAS sharpness (compiled into the game: game-code hooks per build, [client-upscale.md](../re/client-upscale.md#fsr-3)),
Kiln's own render scale for the FSR option (phase B3) and frame generation (phase C).
