# Kiln's graphics tools: frame cap, DLSS settings, readout (0.2.0-dev.29.18)

Kiln's own versions of what players use OptiScaler, DLSSTweaks and frame cap mods for, on Kiln's own hooks. Client
only. Everything is off until the player turns it on: a player who changes nothing gets the game as before, with no
hook on DLSS, no new hook on the game's library loads, and nothing done in a frame but two reads of a flag. Code:
`native/services/ui/graphics.cpp` (plain logic in `frame_pacer.h`, `graphics_settings.h`, `graphics_aside.h`,
`ngx_param.h`; host test `tests/graphics_test.cpp`), `managed/services/Kiln.Ui/UiGraphics.cs`. How the game uses DLSS:
[docs/re/client-upscale.md](../re/client-upscale.md).

## Where the player sets them
- In the game: the Kiln menu (Insert, or **Mods** in the game's menus), tab **Graphics**. Changes apply at once and are
  saved to `kiln/kiln.json` half a second later (the file's comments and other settings stay). Under "Now" the page says
  what the tools do: the cap, the frame rates, the DLSS in use (mode, render and output size, preset), what they step
  aside for, and when a restart is needed.
- In the launcher: Settings, card **Graphics** (the same `kiln.json` entries; it also names the tools Kiln steps aside
  for, from the game folder).
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
| `upscaler` | `game` (the game's own FSR 3), `amd` (AMD FSR: FSR 4 where the GPU runs it, else FSR 3.1), `xess` (Intel XeSS) | `game` | at the game's next FSR 3 set-up; Kiln's hooks go in only when it is not `game` at the game's start ([below](#upscaler-behind-the-games-fsr-3-020-dev2918)) |
| `amdUpscaler` | `auto` (AMD's own choice), `fsr3` (FSR 3.1 even where FSR 4 runs) | `auto` | at the game's next FSR 3 set-up |

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
- It lives in Kiln's present hook, so it needs Kiln's overlay hooks: with the overlay's drawing off (`KILN_UI_OVERLAY=0`,
  the overlay crash guard, Streamline, OptiScaler) the cap is off too, and the page and kiln.log say why.

## DLSS settings
For players whose game upscales with DLSS (an NVIDIA RTX card and DLSS chosen in the game's settings). On another card
the game never sets DLSS up and these settings do nothing (the page says "The game has not set DLSS up in this session").
- **Render preset.** The game's DLSS (`nvngx_dlss.dll` 310.9.1) has models for presets E, F, J, K, L and M; Kiln offers
  J and K (DLSS 4's transformer models) and L and M (DLSS 4.5's newer ones). `default` leaves the game's hint and DLSS's
  own choice. Kiln sets the documented hints `DLSS.Hint.Render.Preset.{Quality, Balanced, Performance,
  UltraPerformance}` right before DLSS is created, after the game's own (the game sets the same four, and DLAA's, before
  every creation, so "default" brings its own back at the next one); **DLAA keeps the game's preset**.
  The NVIDIA app's DLSS override, when the player set one there, wins over any app's hint, Kiln's included; Kiln cannot
  read back which preset runs, so the readout shows the one asked for. A newer `nvngx_dlss.dll` without a preset falls
  back to its default.
- **Render size.** `game`: DLSS's own size for the game's DLSS mode. `perMode`: a scale for each of Quality, Balanced,
  Performance and Ultra Performance (the defaults are DLSS's own; Ultra Quality keeps DLSS's). `custom`: one scale for
  every mode. Kiln puts its own step in front of DLSS's optimal-settings callback (the documented
  `DLSSOptimalSettingsCallback` parameter): DLSS answers, then Kiln sets `OutWidth`/`OutHeight` to the output times the
  scale, within DLSS's own range for that mode (`DLSS.Get.Dynamic.Min/Max.Render.*`). The game renders at that size; a
  size within a pixel of DLSS's own is left as DLSS's. **DLAA always renders at the output's size.** A scale outside
  DLSS's range for the mode is held to the nearest size DLSS allows: the Graphics page shows the size in effect and says
  so, and kiln.log names the range (`graphics: DLSS Quality for 2560x1440: Kiln's scale asks for 845x475, outside
  DLSS's range ...`). Where that range is narrow, the setting does little in that mode.
- How: Kiln hooks five exports of NVIDIA's NGX core (`_nvngx.dll`, the driver's; Proton has it too):
  `GetCapabilityParameters`, `CreateFeature`, `CreateFeature1`, `EvaluateFeature`, `ReleaseFeature`. The game's
  statically linked NGX SDK reaches them through `GetProcAddress`; no game code is patched and only NGX's own parameter
  object is used, through the documented parameter names. Kiln's work inside those calls runs under a fault guard: a
  fault turns Kiln's DLSS settings off for the session (logged), never the game's call. Should the game unload the
  core and load it again, Kiln hooks the new one by the same rules (`graphics: NVIDIA's DLSS core ... was loaded
  again`).

## Readout
A few lines in the top right corner of the game, drawn by Kiln's overlay:
- rendered and output frames a second and the mean frame time over the last half second;
- `DLSS Quality: 1707x960 -> 2560x1440, preset K` while the game upscales with DLSS (the render size is this frame's:
  dynamic resolution may lower it); else "Not DLSS now" (the game's FSR 3, XeSS or no upscaler: Kiln does not read the
  game's own upscalers) or, without Kiln's DLSS hooks, "read from the next game start";
- the frame cap and whether it is on.
No latency: the game sets no latency markers, so there is nothing for Kiln to read honestly. Turning the readout on makes
the overlay draw every frame (an ImGui frame and one submit per present); off, it costs nothing.

## Living next to other tools
Kiln's frame cap and DLSS settings step aside, with one kiln.log line (when one of them is on at start) and a line on
the Graphics page, when another tool does the same (the files are looked up in the game folder at start;
`graphics_aside.h`):

| found | frame cap | DLSS settings |
|---|---|---|
| OptiScaler's DLL (any name; [ui.md](ui.md#optiscaler)), with or without its overlay menu | aside | aside |
| `sl.interposer.dll` in the game folder or `Streamline/` (NVIDIA Streamline, frame generation mods) | aside | aside |
| `EnshroudedVKFG.ini` (a DLSS frame generation mod) | aside | aside |
| `dlsstweaks.ini` (DLSSTweaks) | stays | aside |
| `nvngx.dll` or `_nvngx.dll` next to the game (another DLSS loader: NGX loads it before the driver's) | stays | aside |
| NGX's exports already patched when the core loads (another tool hooks DLSS) | stays | aside |
| Kiln's overlay hooks off (`KILN_UI_OVERLAY=0`, the crash guard) | aside | aside |

The readout stays where Kiln's overlay draws. Kiln never patches over another tool's hook and never changes another
loader's files or mods. Example log line:
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
  `_XESS` 2) and `amd_upscaler` (`KILN_UI_AMD_UPSCALER_AUTO` 0, `_FSR3` 1); still 48 bytes.
- Phase B's status fields follow at offset 72 (`KilnUiGraphicsStatus` is 248 bytes, `KILN_UI_GRAPHICS_STATUS_SIZE_V2`;
  a 72-byte caller gets the fields before only): `upscaler`, `amd_upscaler`, `upscale_running`
  (`KILN_UI_UPSCALE_RUN_OFF` 0, `_NOT_FSR` 1, `_GAME_FSR3` 2, `_AMD_FSR4` 3, `_AMD_FSR3` 4, `_XESS` 5), `upscale_reason`
  (`KILN_UI_UPSCALE_WHY_*`, the list above), `upscale_flags` (`KILN_UI_UPSCALE_HOOKED`, `_FSR_SEEN`, `_FILES_OK`,
  `_DEVICE_OK`, `_FELL_BACK`, `_AMD_PINNED`), `aside_upscale`, `upscale_version` (`(major << 22) | (minor << 12) |
  patch`), the FSR 3 pass's render and output size, and `upscale_detail` (128 bytes, English).
There is no managed (mod API) member for it in this release.

## Upscaler files (part D, a preview)
What phase B (an upscaler swap behind the game's FSR 3 option) and later frame generation will load: AMD's FSR DLLs and
Intel's XeSS DLL. **They do nothing yet**: no code in the game loads them in this release. The launcher downloads them
only when the player asks, on a screen that says what will happen. Contract: [launcher/contracts.md](launcher/contracts.md)
section 5.15; code `crates/kiln-manage/src/vendor/`, `launcher/src/components/UpscalerSettings.tsx`.

| component | version | files | size | licence | from |
|---|---|---|---|---|---|
| AMD FSR (`amd-fsr`) | FidelityFX SDK 2.4.0 | `amd_fidelityfx_loader_vk.dll`, `amd_fidelityfx_upscaler_vk.dll` (FSR 3.1.6 and FSR 4.1.2), `amd_fidelityfx_framegeneration_vk.dll` | 72,981,808 bytes | MIT licence | `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK` at tag `v2.4.0`, `Kits/FidelityFX/signedbin/` |
| Intel XeSS (`intel-xess`) | XeSS SDK 3.0.2 | `libxess.dll` | 77,795,704 bytes | Intel Simplified Software License (October 2022) | `intel/xess` at tag `v3.0.2`, `bin/` |

- **Where the player sees it:** the launcher's Settings, section **Upscalers** right under Graphics, for a client only
  (a dedicated server draws no frames). It is behind the launcher's preview `upscalers` and hidden by default: put
  `"preview": ["upscalers"]` into the launcher's `settings.json` (in its data folder: Settings, Library, data folder)
  while the launcher is closed, or start it once with `KILN_LAUNCHER_PREVIEW=upscalers`. One row per component: not
  downloaded (with the size and the host), downloading (progress, Cancel), downloaded (version, size on disk, Remove),
  update available, not available in this build, failed (the reason, Try again). The line under the title says what
  they are for and that downloading them changes nothing yet.
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
  verified. Phase B checks each file's SHA-256 again before it loads it, against the hashes built into the runtime, never
  the receipt's (contracts.md 5.15, "What Kiln in the game will read").
- **Steam Deck UI:** no matching screen yet. It would take a Deck Settings row per component (the same `vendor_*`
  commands), a licence screen that scrolls the text with the stick and takes A for "I agree, download", and progress in
  the Deck's task line. The desktop UI on the Deck has the section as it is.
- **Later:** NVIDIA's frame generation (`nvidia-dlssg`, from NVIDIA's own GitHub, never inside Kiln) is a new component
  in the same manifest, with NVIDIA's licence.

## Upscaler behind the game's FSR 3 (0.2.0-dev.29.18)
Phase B. In the game's own settings the player chooses FSR 3, as before; `graphics.upscaler` chooses what runs behind
it: the game's own FSR 3 (`game`, the default: nothing changes), AMD FSR (`amd`) or Intel XeSS (`xess`). Kiln never calls
any of it DLSS. Code: `native/services/ui/upscale.cpp` (the swap), `upscale_amd.cpp` (AMD FSR), `upscale_xess.cpp`
(XeSS), the interface `upscale_backend.h`, plain logic `upscale_logic.h` and `upscale_amd_map.h` (host test
`tests/upscale_test.cpp`), the vendor files `vendor_file.h`, the device set-up `device_merge.h`; sigdb
`client.d/upscale.json` (feature `upscale.swap`).
- **When it applies.** Kiln hooks the game's FSR 3 pass only when an upscaler other than `game` is chosen **when the
  game starts**, and only then adds what that upscaler needs to the game's Vulkan device (extensions and features, as
  the game's device is made). A choice made while the game runs applies at the game's next FSR 3 set-up (loading a
  world, changing FSR's quality, the resolution or the 3D render scale) when it is the start's choice or `game`, else
  from the next start (status reason `RESTART`). `amdUpscaler` applies at the next set-up.
- **What happens.** The game builds its frame as a list of its own render commands, then runs them. Where the game would
  write its FSR 3 passes, Kiln makes the game flush its pending barriers (as the game does for DLSS) and writes one
  command of its own instead; when the game runs that command, Kiln has the chosen upscaler record its work into the
  frame's own Vulkan command buffer, at the same place. Its inputs are the game's own, as the game's FSR 3 gets them:
  colour (linear HDR), depth (reverse-Z), motion vectors (Kiln copies them into a format AMD's API takes; the values
  are unchanged), the reactive and transparency masks the game makes for its FSR, the jitter, sizes, camera planes,
  field of view, pre-exposure and frame time (Kiln's own measured time while the game's is 0, about 500 frames after
  a world load). History is dropped when the game sets FSR 3 up, after a load and when a size changes.
- **AMD FSR.** AMD's signed Vulkan DLLs from the FidelityFX SDK 2.4.0, from `<game>/kiln/vendor/amd/2.4.0/` (the
  launcher's Settings, Upscalers). Kiln checks each file's size and SHA-256 against the pins built into Kiln (the same
  values as the launcher's manifest; `tools/release/vendor_pins_header.py --check`) before it maps them, keeps them
  open so they cannot change (on Windows; under Proton that binds only Wine's own programs, contracts.md 5.15), and
  maps them only from that folder. AMD's own selection picks FSR 4 where the GPU and driver run it (RX 9000, RX 7000
  and 6000, some recent APUs), else FSR 3.1; `fsr3` pins FSR 3.1. Where FSR 4 is
  chosen but does not start, Kiln runs FSR 3.1 instead and says so in kiln.log. The device additions: everything AMD's
  Vulkan backend uses when the GPU offers it (subgroup size control, 16-bit floats, synchronization2, AMD's buffer
  markers and coherent memory, descriptor indexing) and FSR 4's set (cooperative matrix, FP8, integer dot product,
  compute-shader derivatives, 8- and 16-bit storage), each only where the GPU has it.
- **Failures.** A missing or changed file, a GPU without what the upscaler needs, a set-up that fails: the game's own
  FSR 3 runs and the status says why. A failure while upscaling: that frame gets a plain stretch of the image, then
  the game's own FSR 3 for the rest of the session. Every call of Kiln's around the game's runs under a fault guard.
- **Stepping aside.** The swap stays off, with the reason, for OptiScaler, NVIDIA Streamline, a DLSS frame generation
  mod (`EnshroudedVKFG.ini`), AMD FidelityFX or Intel XeSS DLLs in the game folder (another tool's; `ASIDE_UPSCALER`),
  the game's FSR 3 pass already patched by another module, and Kiln's overlay hooks being off (the swap needs Kiln's
  Vulkan hooks).
- **What the player sees.** The readout's line `Behind FSR 3: AMD FSR 4 (4.1.2): 960x540 -> 1920x1080`, or the game's
  own FSR 3 with the reason the chosen one does not run; the Graphics page and the launcher read the same status.
- **kiln.log** (`graphics: upscaler: ...`): the choice at start and the hooks, the files' check, what was added to the
  Vulkan device and what the GPU lacks, the versions AMD offers for the GPU, each set-up (`AMD FSR 4.1.2 set up for
  960x540 -> 1920x1080`), and every fallback with its reason.
- **Limits:** after a fault inside an upscaler's own code Kiln never calls that upscaler again (it may hold a lock), so
  its GPU memory stays allocated until the game ends; the game's FSR 3 runs meanwhile.
- **Not proven yet:** a real game with AMD FSR on any GPU; FSR 4 needs an AMD tester (RX 9000, or RX 6000/7000), the
  Steam Deck too. The swap itself was proven on the real client with a plain stretch (spike B0, RTX 3080 Ti, Proton).

Status reasons (`KilnUiGraphicsStatus.upscale_reason`, stable numbers): 0 none, 1 restart, 2 files missing, 3 files
not the pinned ones, 4 did not load, 5 the GPU or driver lacks a Vulkan feature, 6 set-up failed, 7 failed while
upscaling, 8 fault, 9 stepped aside, 10 this game build's FSR 3 pass not found (or not hooked), 11 the game's textures
are not what it takes, 12 not in this build or system, 13 waiting for the next FSR 3 set-up, 14 the game's Vulkan device
not seen.

## Not in phase A
FSR 3's ratio and RCAS sharpness (compiled into the game: game-code hooks per build, [client-upscale.md](../re/client-upscale.md#fsr-3))
and frame generation (phase C). The upscaler swap is phase B, above.
