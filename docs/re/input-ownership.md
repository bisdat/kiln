# Owned presses and the controllers' view: the game's input paths (kiln.ui.input, 0.2.0-dev.31.13)

Static analysis of the pinned client, Steam build 26430625 (`enshrouded.exe`, SHA-256 `5d9ef2f1…`, image base
0x140000000), with `objdump -p` / `objdump -d` on the installed file; nothing was run. It qualifies the input paths
kiln.ui.input (`native/core/abi/services/kiln_ui_input.h`, rules `native/services/ui/input_own.h`) relies on to keep an
owned press's release and held state from the game, and checks the addresses XHL's reference
(`XHL-Kiln-Game-Interfaces.md`, 24 November 2026: "Own a complete right-button gesture during construction", "Route an
overlay's controller release", "Observe normalized controller input") gives for the same jobs.

`native/services/ui/tests/input_own_re_test.py` (in `tests/run.sh`) checks every address and claim below on the installed
exe (read only; it skips without the exe or on another build). kiln.ui.input adds **no new game hook**. It uses three hooks kiln.ui already has, found by name, not by pattern: the
game window's procedure (a subclass), the game's `user32!GetRawInputData` import slot, and `XInputGetState` in whichever
XInput DLL the game loads. So there is no new sigdb entry; this note is the evidence that those three cover the release
and the held state.

## Keyboard and mouse: what the game reads

| Evidence | Value | Meaning |
| --- | --- | --- |
| Import `USER32.dll!GetRawInputData` | IAT slot RVA `0x13CFB38` | the only raw-input reader (no `GetRawInputBuffer` import); XHL gives the same slot |
| Its two callers | `0xFA37FC` (size query: `edx = 0x10000003` RID_INPUT, `r8 = 0`, `[rsp+0x20] = 0x18` header size), `0xFA38E8` (the read into a 0x400-byte stack buffer) | one function at `0xFA37B0`; after the read it switches on `RAWINPUTHEADER.dwType` at `[rbp-0x50]`: 0 mouse (`0xFA39BC`), 1 keyboard (`0xFA39A8`), 2 HID (`0xFA3941`, a loop over registered HID handlers) |
| Imports `RegisterRawInputDevices`, `GetRawInputDeviceList`, `GetRawInputDeviceInfoA` | `0x13CFB48`, `0x13CFB50`, `0x13CFB40` | raw mouse, keyboard and HID devices |
| Imports `PeekMessageW`, `TranslateMessage`, `DispatchMessageW` | `0x13CFB88`, `0x13CFB78`, `0x13CFB80` | the window procedure gets the legacy key and mouse messages too |
| `GetKeyState`, `GetAsyncKeyState`, `GetKeyboardState`, `GetRawInputBuffer` | not imported, and no such name among the file's strings (so not resolved through `GetProcAddress` by name either) | the game knows a key or button is held only from the window's messages and raw records |

So the game learns that a button went down or up only from (a) the raw record's `RAWMOUSE.usButtonFlags` /
`RAWKEYBOARD.Flags` it reads through the import slot, and (b) the legacy messages (`WM_RBUTTONDOWN`, `WM_RBUTTONUP`,
`WM_KEYDOWN`, ...), and that a mouse button is held from (b)'s key state (`MK_*` bits in the wParam of every client
mouse message). kiln.ui.input therefore keeps an owned press's release by:

- not passing its legacy message on (`WM_*BUTTONUP`, `WM_KEYUP`, the repeat's `WM_KEYDOWN`);
- swallowing its raw record when it holds nothing else, or clearing only the owned `RI_MOUSE_*_UP` flag in the record
  the game reads through the slot (kiln.ui's `hk_get_raw_input_data`, `input.cpp`) when it also holds motion or another
  button: the same mechanism as kiln.ui's taken presses since 0.2.0-dev.27 (`docs/re/client-render.md`);
- clearing the owned button's `MK_*` bit from the wParam of every client mouse message while it is held.

Where the game reads a press's down after all (a raw record with other input when the slot could not be hooked; Alt+F4,
which kiln.ui always passes), the press is "leaked" and its release is passed too: the game is never left holding a
button (the stuck-press guard). XHL's Blueprint reference does the same three things (the 250 ms twin FIFO is kiln.ui's
`mouse_press.h` Twins, 50 ms of message time).

## Controllers: three backends, kiln.ui sees one

| Evidence | Value | Meaning |
| --- | --- | --- |
| Strings `XInput1_4.dll`, `XInput1_3.dll`, `XInput9_1_0.dll`, `XInputGetState`, `XInputSetState`, `XInputGetCapabilities` | one each | XInput loaded at run time (`LoadLibrary*` + `GetProcAddress` imports): kiln.ui's inline hook on `XInputGetState` sees every poll (`input_hook_xinput`) |
| Import `DINPUT8.dll!DirectInput8Create` | IAT `0x13CF148` | a DirectInput backend |
| Strings `SetupDiGetClassDevsW`, `SetupDiEnumDeviceInfo`, `SetupDiGetDeviceRegistryPropertyW`, `SetupDiGetDevicePropertyW`, `SetupDiGetDeviceInterfaceDetailW` | | device enumeration next to DirectInput (likely the usual filter that keeps XInput pads out of DirectInput; not traced) |
| Raw input `dwType == RIM_TYPEHID` handled at `0xFA3941` | | HID game controllers also through raw input |

`KeyListenFlags.Pad` ownership and `PadInput` cover XInput controllers (an Xbox pad, or any pad Steam Input presents as
one). A pad the game reads through DirectInput or raw HID is outside both, as it is outside kiln.ui's pad bindings since
0.2.0-dev.30.5. Covering those needs the game's own normalized dispatch below.

## The game's normalized input dispatch (XHL's sites; verified, not used yet)

Disassembly of `0x85C33E..0x85C47F` (inside the client input dispatcher):

```
85C33E  mov rbx,[rbx+0x258]          ; the input context
85C34E  mov rcx,[rbx+0x5F8]          ; receiver A
85C35A  lea rdx,[rsp+0x20]           ; XHL "observation instruction": the 40-byte event at rsp+0x20
85C35F  call 0xEC2B30                ; XHL "dispatch normalized event"
85C364  mov rcx,[rbx+0x5F8]          ; the same receiver reloaded
85C36B  call 0xE92CC0                ; XHL "query input mode"
85C377  cmp al,1
85C379  mov [rbx+0x34F73],al         ; the mode recorded
85C387  mov rcx,[rbx+0x5A0]          ; XHL "routing point after active-device selection"
85C38E  call 0x9079F0
...
85C3E9  lea rdx,[rsp+0x20]
85C3EE  call 0xA4FC90                ; XHL "original UI consumer", the stack event in RDX
85C3F3  test al,al
85C3F5  jne 0x85C45B                 ; handled -> mov al,1 / return
...
85C44D  lea rdx,[rsp+0x20]
85C452  call 0xEAADB0                ; the next consumer (gameplay)
85C45B  mov al,1                     ; XHL "handled branch destination"
85C46E  xor al,al                    ; not handled
```

Every address XHL lists for these two sections matches 26430625 byte for byte: the observation point `0x85C35A` (the
`lea` that sets RDX has not run), the calls at `0x85C35F` (to `0xEC2B30`) and `0x85C36B` (to `0xE92CC0`) on one receiver
reloaded from `[rbx+0x5F8]` with `cmp al,1` after the second, the routing point `0x85C387`, the UI consumer call at
`0x85C3EE` (to `0xA4FC90`) whose `al` decides the branch to `mov al,1` at `0x85C45B`. The event's fields (device +0x00,
kind +0x0C, button/axis +0x18, value +0x1C, dead zone +0x24, X/Y +0x1C/+0x20 for kind 5) were not checked here: they
need the producer of the event or a live trace.

A mid-hook at `0x85C35A` (observe) and at `0x85C387` (route: set RIP to `0x85C45B` for an owned button's events) would
give ownership and observation for every controller backend at once, with the game's own button numbering. It is a
follow-up: it needs the event layout qualified (or traced), a mapping of the game's device/button numbers to Kiln's
`KILN_UI_VK_PAD_*`, sigdb patterns for both sites with a feature row, and a decision on how it shares a press with the
XInput path (one press must not arrive twice). Suggested anchors for the patterns, from the bytes above: the
`lea rdx,[rsp+20h]; call; mov rcx,[rbx+5F8h]; call; mov rcx,[rbx+5B0h]; cmp al,1; mov [rbx+34F73h],al` run, and the
`lea rdx,[rsp+20h]; call; test al,al; jne` run that ends at `mov al,1`.

## Not release ownership: EmberCodex's hotbar page on Alt's release

XHL's "Suppress one mapped hotbar page change on modifier release" is a different job: the game saw Alt go down (it is the
modifier of a chord the mod takes later), so Kiln must let its release through (the stuck-press guard), and the game's
hotbar input turns that release into a page change. The site XHL gives checks out on 26430625: in the hotbar input routine
(`0x3AF670`) the `ActionbarSwapPage` path at `0x3AF7EB` loads the current slot, then `sub bl,8` (`80 EB 08`) at `0x3AF7FB`,
`and bl,0Fh` (`80 E3 0F`) at `0x3AF806` and `movzx r8d,bl` (`44 0F B6 C3`) at `0x3AF809` before the submit call to
`0xE980D0`, and the action is consumed at `0x3AF819`. Suppressing one such mapped game action is a follow-up of its own
(a sigdb site and a "consume this game action once" member), not part of kiln.ui.input.

## What is not verified without a live game

- That the game reacts to nothing but the messages and records above for a mouse release (its UI could also poll the
  cursor's buttons some other way not visible statically; the imports and strings say it does not).
- The game's reaction to a `WM_MOUSEMOVE` without `MK_RBUTTON` during an owned right drag (nothing is expected: it never
  saw the press).
- Wine/Proton's order of raw records and messages for a release (kiln.ui pairs them in either order).
- Controllers through DirectInput or raw HID (not covered, above).
