# Native chat input: phase-1 history groundwork

## Latest acceptance: HUD and Social draft restoration PASS (17 November 2026)

The tester rebuilt the fork experiment at commit `9950aed` and **confirmed visually that Up/Down and unsent draft restoration work in both HUD and Social chat**. The game trace at 12:14 corroborates the successful capture and application:

```text
capture draft from_postcall=1 bytes=5 precall_bytes=0
recall older entries=1 changed=1 browsing_before=0 browsing_after=1 stale_widget=0
engine edit replacement verified (widget readback)
recall newer entries=1 changed=1 browsing_before=1 browsing_after=0 stale_widget=1
engine edit replacement verified (widget readback)
```

The five-byte post-call snapshot confirms that the earlier failure was due
to reading an outdated/empty pre-call chat widget when first saving the draft.
This **closes the HUD/Social Up–Down/draft-restoration defect** for the tested
Enshrouded client build 26430625. Previous failed traces below are retained
as diagnostic history, not the current result.

**Still pending full acceptance:** confirm manual typing over a recalled
command resets browsing; Up/Down does not steal gameplay or unrelated text
input; Escape and Alt+Tab behave normally; and persisted slash-command history
survives a complete game restart without persisting normal chat or sensitive
commands. The earlier `persisted=1` trace confirms the save path was
invoked, **not** that reload has been verified. The experiment remains
opt-in (`KILN_CHAT_HISTORY=1`) and PR #2 remains **draft/unmerged**. Do not
touch upstream `frostdev-ops/kiln`.


## 12:06 runtime: Down applies an empty draft — post-call capture correction

**Observed on 17 November 2026:** HUD Up visibly restored a saved command;
Down **did perform EditSetText and passed widget readback**, but the original
unsent text `draft` was cleared instead of restored:

```text
recall older entries=1 changed=1 browsing_before=0 browsing_after=1 stale_widget=0
engine edit replacement verified (widget readback)
recall newer entries=1 changed=1 browsing_before=1 browsing_after=0 stale_widget=1
engine edit replacement verified (widget readback)
```

The earlier comparison bug is fixed. The remaining symptom strongly suggests
the *value* captured as `draft_` on the first Up was empty: the adapter
historically called `History::older(current)` with the widget contents
**before** the original `TextInputProcess`, even though previous tests proved
that snapshot can lag behind the visible, edited text.

**Current unverified fix:** cache the caller-owned widget's text **after**
`TextInputProcess` on non-browsing frames, independently for HUD and Social.
On the initial Up, pass this authoritative post-call snapshot into
`History::older` instead of the stale pre-call string. Continue applying the
selected command through the already verified engine `EditSetText` path.
Invalidate the per-field cache on Enter/Escape, sustained focus loss and
unreadable/invalid snapshots. The trace now logs a bounded summary:

```text
chat history trace: capture draft from_postcall=1 bytes=5 precall_bytes=0
```

No draft content is logged. Regression tests reproduce an empty pre-call
widget, a cached `draft`, Up followed by Down, blank drafts, and
independent HUD/Social snapshots.

**Next acceptance:** run standalone host tests and build the fork's UI DLL,
then in a throwaway world type an unsent `draft` in HUD and use Up/Down.
Visually, Down must return the exact original text. The expected
`capture draft` summary is `from_postcall=1 bytes=5` and the Down
`recall newer ... changed=1` should be followed by successful widget
readback. If `bytes=0`, the post-call cache itself is not seeing typed input:
**do not try to repair it by guessing offsets or direct memory writes**;
instead investigate the game's UI flow before enabling another adapter.

This revision is restricted to draft PR #2 in `bisdat/kiln` and has **not
yet been compiled or validated in-game**. The draft remains unmerged.


## 12:01 in-game trace: draft restored in model, but SetText skipped

At 12:01 on 17 Nov 2026, user confirmed Up visibly recalled the command
but Down did not restore the unsent draft. The new browse flags show:

```text
recall older entries=1 changed=1 browsing_before=0 browsing_after=1
engine edit replacement verified (widget readback)
recall newer entries=1 changed=0 browsing_before=1 browsing_after=0
```

**Root cause:** the model correctly exits browsing and returns the saved
draft on Down, but the adapter compares it to the pre-`TextInputProcess`
widget snapshot. That snapshot still equals the original draft even though
the earlier Up applied and displayed the recalled command. The comparison
therefore yields `changed=0` and skips `EditSetText`. Repeated Up at the
oldest history entry was also needlessly resent because it compared against
the stale snapshot rather than the last applied command.

**Fork-only correction (not yet game-tested):** `needs_replacement()`
compares against Kiln's last successfully applied text **while browsing**
and against the live widget text **when starting navigation outside browsing**.
The Down transition now calls `EditSetText` even when its target equals
the pre-call stale widget snapshot, and repeated Up on the oldest entry
becomes a no-op. Logs include `stale_widget=1` when this occurs. The
`chat_history_test.cpp` regression reproduces a stale `draft` snapshot,
Up/Down, repeated Up and an empty draft.

**Next test:** rebuild `kiln-ui.dll`, launch with
`KILN_CHAT_HISTORY=1 KILN_CHAT_HISTORY_TRACE=1`, type unsent `draft`,
press Up once then Down once. Expect:

```text
recall older entries=1 changed=1 browsing_before=0 browsing_after=1 stale_widget=0
engine edit replacement verified (widget readback)
recall newer entries=1 changed=1 browsing_before=1 browsing_after=0 stale_widget=1
engine edit replacement verified (widget readback)
```

Visually `draft` must reappear. Only then test Social, manual editing and
restart persistence. Keep draft PR #2 unmerged until in-game acceptance.


## Latest game test and draft-restoration fix (17 November 2026)

The 11:47 interactive trace **PASSED** recording submissions (including
Social persisted commands), queueing Up/Down, selecting older history
entries and applying engine `EditSetText` with successful post-call widget
readback. However, **Down did not restore the unsent draft** immediately
after Up: `recall newer entries=2 changed=0`. The user explicitly
confirmed that draft restoration failed. This is a **FAIL** for the complete
feature, despite the verified low-level text replacement.

Inspection found that the adapter previously called `History::edited()` on
*every* difference between the live widget buffer and last-applied command,
even if caused by normal game UI synchronization. It also reset browsing on
a single non-editing UI frame. Either path would discard the saved draft
before Down, while subsequent Up/Down still reached the router.

The unverified correction:
- Navigating remains active until the player actually types or edits text.
  WndProc marks WM_CHAR/WM_SYSCHAR/WM_UNICHAR, WM_PASTE,
  Backspace/Delete and Ctrl+V/Ctrl+X as explicit user edits; only these
  clear an active browsing cursor (outside Enter/Escape and focus loss).
- A temporary widget-text mismatch **never** resets history by itself.
- A missing chat focus frame stops arrow interception immediately, but the
  browsing cursor is only forgotten after **four consecutive missing UI frames**.
  Actual Enter/Escape explicitly terminate the browse session.
- The trace includes `browsing_before` and `browsing_after`, and reports
  whether a reset came from actual text editing or sustained loss of focus.
- Added regression coverage for Up→Down draft restoration despite a stale
  widget snapshot / single missing focus frame, explicit manual edit, and
  four-frame focus timeout.

This revision is in fork draft PR #2. **It has not yet been compiled or
tested in-game.** Do not merge until the user confirms the draft visually
restores after Down, and focus isolation / persistence survive relaunch.

### Next concise test

Run the standalone C++ test, rebuild `kiln-ui.dll`, then test only:
enter `draft` in HUD chat without sending, Up, Down. Expect a previous
command on Up and the unsent `draft` on Down. Confirm in the log:

```text
recall older ... changed=1 ... browsing_after=1
recall newer ... changed=1 browsing_before=1 browsing_after=0
engine edit replacement verified (widget readback)
```

Then test manual typing after recall and a normal Escape/close. Submit a
harmless command only if history is empty on startup. Continue to use
`KILN_CHAT_HISTORY=1 KILN_CHAT_HISTORY_TRACE=1` for this test and keep
the known-good UI DLL backup. No raw chat contents are logged.


**Status (fork experiment, client build 26430625):** The read-only native
input probe has been validated in-game by a tester for both HUD and Social
call sites. An opt-in preview now implements Up/Down recall in the native
service, backed by a portable native history model compatible with the existing
managed history file format. **The first preview was compiled and run in-game on 17 Nov 2026:** installation,
HUD call-site and Social call-site/focus detection all PASS; Up/Down history
did not visibly work (FAIL). The tester restored the previous DLL. A new,
read-only diagnostic mode has been added to isolate key routing, submit events
and edit-state readability before another interactive trial. The managed ChatHistory remains a separate reference model;
the managed/native service bridge, Ctrl+R UI and completion are not implemented.

## What the model does

- Up/Down-style older/newer navigation with an unsent draft restored on Down
  beyond the newest entry. Manual edits reset navigation.
- Bounded, recent-first search usable later by Ctrl+R; 200 entries by default,
  capped at 2,000 if configured, with adjacent duplicates collapsed.
- A best-effort, atomic JSON history file. The eventual caller should pass
  GAME/kiln/user/chat-history.json (outside kiln/state, which support
  reports collect). Only slash commands are persisted by default; ordinary chat
  may be retained for the process session. Plain chat persistence requires an
  explicit opt-in.
- User-submitted text only: never record incoming lines, Kiln/EMF protocol
  messages, or programmatically generated replies. Reject malformed or
  over-1024-byte text and common credential-bearing commands/arguments.
  A future server command index can mark additional commands as sensitive
  through Submit(serverSensitive: true).
- Only SHA-256 hashes of server identities, never raw server addresses, are
  written to the file. The history model does **not** log submitted lines.
- Corrupt or unwritable history is nonfatal. Clear() clears the in-memory
  history and overwrites the persisted history with an empty list.

## Remaining integration (for the complete phase-1 feature)

1. Candidate build-scoped sigdb symbols and both call sites now exist and have
   been validated in a read-only runtime probe. Verify the edit-state and
   EditSetText ABI in the next opt-in interactive test; until then the preview
   must remain disabled by default.
2. Add a narrow native input service that reports focused chat state and text
   on the game's **UI thread**. On that thread, use the game's SetText to
   apply one history result, preserving the game's edit buffer and cursor
   invariants. A hook failure must leave vanilla input entirely intact.
3. The opt-in native preview now consumes Up/Down in the existing WndProc
   only while TextInputState reports active edit focus. It queues the requests
   from the window thread to the UI thread; SetText is never called in WndProc.
   **Actual focus-offset and text replacement behavior require game testing.**
4. The preview takes submitted text from the scoped native edit after the
   game's TextInputProcess returns a submit event, before its caller sends or
   clears the line. It does not record incoming chat or programmatic output.
   Cross-check that submit event and input layout in-game.
5. Connect the native preview to the managed history model and settings with
   an additive native ABI tail; the opt-in preview currently uses a separate
   native History implementation (same JSON format and privacy policy) so no
   existing kiln.ui ABI slots change.
6. Add Ctrl+R search UI and then the separate Tab-completion protocol/list.
   Completion requires permission-filtered server suggestions and a way to
   suppress Tab's WM_CHAR event as well as the physical key.

## Acceptance testing (real game required)

- HUD chat and Social-tab chat: repeated Up/Down, changing a recalled line,
  returning to the unsent draft, Enter to send, Escape to close.
- A second Kiln client session restores submitted commands but not ordinary
  chat (unless opted in); secrets never appear in the JSON file.
- Up/Down in gameplay, unrelated game edit fields, the Kiln menu and a keybind
  capture still behave as before. Failed signatures should disable only the
  chat enhancement.
- Test with a remote server, hosting, vanilla server, EMF commands, Steam
  virtual keyboard and non-ASCII input.
- Ensure the overlay does not steal focus and that the normal message-send
  path and its permission checks remain unchanged.

### Local tests (when the .NET 10 SDK is available)

~~~sh
dotnet test managed/tests/Kiln.Ui.Tests/Kiln.Ui.Tests.csproj \
  --filter FullyQualifiedName~ChatHistoryTests
~~~

The tests cover model semantics and file safety only. They do not validate
game signatures, key consumption, native state offsets or the UI display.


## Native probing now on this branch (opt-in only)

The fork also has:

- `sigdb/symbols/client.d/chatui.json`: signatures and scoped HUD/Social
  return addresses transcribed from issue #44 for client Steam build 26430625.
  Their **runtime validity remains unverified** in this fork.
- `native/services/ui/chat_input_probe.cpp`: a transparent hook that logs
  whether the HUD/Social input call sites were actually reached. It does not
  inspect or log text, consume keys, change the edit buffer or send anything.
  It is **off by default** and only installs when the game process has
  `KILN_CHAT_INPUT_PROBE=1` in its environment. If the sigdb feature cannot
  resolve, it does not hook anything.
- `native/services/ui/chat_keys.h`: a small, focus-scoped window-thread key
  queue whose unit test asserts that gameplay keys, modified arrows and
  releases remain untouched. It is *not* wired to WndProc yet, intentionally:
  the edit focus and UI-thread text injection must be validated first.
- `native/services/ui/tests/chat_keys_test.cpp`: included in the regular
  native host test runner; also built and passed with Linux g++ -std=c++20.

### Probe acceptance before any input mutation

1. Build this **fork** and confirm an ordinary boot with the probe unset
   yields no chat hooks or unexpected input behaviour.
2. Launch the game with `KILN_CHAT_INPUT_PROBE=1`; open the HUD chat and
   Social tab. Confirm one read-only log per call site, then normal typing
   and sending. Do not include chat message content in logs or support files.
3. Check that the client build matches the issue's SHA-256 and the sigdb
   function signatures. Game updates require new verification, not blind
   reuse of these offsets.
4. Only after the probe passes should the next step activate focus-tracked
   Up/Down routing, `EditSetText` on the UI thread, and managed history
   submission. In a failed hook state, these must revert to vanilla input.

## Opt-in interactive history preview (not yet runtime validated)

This experimental preview lives in the same `kiln-ui.dll` as the verified
read-only probe. The dedicated environment flags are mutually optional:

- `KILN_CHAT_INPUT_PROBE=1`: exactly the previously validated read-only
  hook. No state reads, key interception, edit replacement or persistence.
- `KILN_CHAT_HISTORY=1`: enables history navigation, state inspection and
  EditSetText. The first in-game test should use a throwaway hosted world,
  with a backup of the previous UI DLL. History mode is **off by default**.

Runtime flow: a recognized HUD/Social TextInputProcess call gets that widget's
persistent edit state. The editing byte at `state+0x1988` gates a focus token
updated at the end of each GameUiFrame. The window's raw and legacy key paths
are already deduplicated; focused Up/Down presses are queued, and releases
always reach the game. The next scoped UI frame applies the queue with the
game's own EditSetText (not a buffer overwrite). Window focus loss clears the
queue. An actual submit result saves commands from the game's edit state.

History is bounded to 200 lines, collapses adjacent duplicates, excludes
common credential-bearing commands from both memory and disk, and retains
ordinary chat only for the current process. Only submitted slash commands are
persisted (atomic best-effort replace) at `<game>/kiln/user/chat-history.json`.
The preview does not yet separate histories by server, expose a Clear button,
implement Ctrl+R, or offer command Tab completion. No typed text is logged.

### Root cause established: invalid private edit representation (17 Nov 2026)

The in-game read-only HUD and Social traces independently confirmed the same
difference after sending `/help` (5 bytes):

| Source | Prefix | Valid | Control bytes | Length |
| --- | --- | --- | --- | --- |
| Internal edit-state bytes at `state+0x10` interpreted as `EditHead` | Slash | **No** | **3** | 5 |
| Caller-owned outgoing string at `widget+0x150` | Slash | **Yes** | **0** | 5 |

This **explains the empty history**: `History::valid` correctly rejects the
control bytes; the adapter was reading the wrong representation. The command
classifier and Up/Down WndProc routing are not the cause of the empty list.

The adapter now uses `widget_text(widget)` for both submitted lines and
the live unsent draft. The private editor remains the target of the
game-owned `EditSetText` function, whose behavior is **not yet proven**.
To avoid false failures from reading the known-invalid private representation,
the preview verifies the requested replacement against the validated widget
string **after** calling the original `TextInputProcess`. On a mismatch it
disables history for the process, leaving subsequent input vanilla.
`chat_history_test.cpp` includes a host regression test with the observed
five-byte/control-byte failure shape and an accepted five-byte `/help`.

**Next interactive acceptance test:** Build the fork and use
`KILN_CHAT_HISTORY=1 KILN_CHAT_HISTORY_TRACE=1` (do **not** include
`KILN_CHAT_HISTORY_DIAG`). Submit `/help` in HUD. The log must show
`widget_valid=1 widget_sensitive=0` and
`submit processed entries=1 added=1 persisted=1`. Reopen chat, type
`draft`, press Up once, and verify the field displays `/help`; Down must
restore `draft`. Confirm `engine edit replacement verified (widget readback)`.
If any edit-replacement warning appears, do not retry: restore the previous DLL
and investigate the engine SetText ABI. Only then test Social and restart
persistence. Logs never include the submitted text.

### Interactive trace after the read-only PASS (17 Nov 2026)

An instrumented `KILN_CHAT_HISTORY=1 KILN_CHAT_HISTORY_TRACE=1` run showed:
- Up/Down were received while chat was focused, `mods=0 captured=0 consumed=1`.
- The adapter called recall, but all calls reported `entries=0 changed=0`.
- HUD submitted a five-byte non-command line (`command=0`) and the
  Social editor submitted a five-byte slash-prefixed line (`command=1`).
- The Social submit reported `entries=0 added=0 persisted=0` despite a
  readable five-byte slash-prefixed line. The history's validation or
  underlying edit-text representation is thus the immediate suspect;
  the SetText replacement path has not been exercised.

Added a read-only comparison between the private edit state's text and the
caller's outgoing widget chat buffer (`widget+0x150`). In diagnostic mode,
both data sources are examined only through guarded/size-bounded reads.
The trace logs only booleans, lengths, control-byte counts, equality and
event codes; **never raw chat text or hashes**. On the next test run, use
`KILN_CHAT_HISTORY_DIAG=1`, submit one harmless command (`/help`) from
each chat input, and examine:

- `edit_valid=0 edit_controls=N`: private edit bytes are invalid.
- `widget_valid=1 same=0`: outgoing caller buffer differs from private edit;
  consider reading the outgoing buffer on submit, after confirming layout.
- `edit_valid=1 edit_sensitive=1`: classification rejects this input.
- `edit_valid=1 edit_sensitive=0`: history model should accept; investigate
  the model invocation/state instead.
- `widget_ok=0`: caller buffer layout is not confirmed; do not write from it.

Do not test interactive history again until the source of the zero-entry
submissions is established. The existing read-only diagnostic does not swallow
keys, replace game text or persist chat.

### Diagnostic results confirmed in-game (17 Nov 2026)

The follow-up **read-only** `KILN_CHAT_HISTORY_DIAG=1` test passed on the
exact SHA-256-pinned client:

- HUD event 1 (Enter) produced readable text, five bytes beginning with `/`;
  event 2 (Escape) was observed.
- Social event 1 produced the same readable command; event 2 was observed.
- WndProc received Up and Down repeatedly with `mods=0 focus=1 captured=0`.
  `consumed=0` was required: diagnostic mode never swallows a key.
- HUD/Social native call sites and edit focus were both observed.

This establishes the native submit event, the input read path and keyboard
routing; it does **not** validate the history model's submission path, the
actual interception, or game SetText. The first interactive preview's
32-bit event read was changed to an 8-bit discriminant before the diagnostic
passed. An additional readback guard now disables interactive history if
SetText returns without updating the edit buffer.

**Next test:** rebuild `kiln-ui.dll` and launch with
`KILN_CHAT_HISTORY=1 KILN_CHAT_HISTORY_TRACE=1` (not the diagnostic flag).
Submit harmless slash commands and press Up/Down. Log should report
`submit processed`, `recall older/newer entries=N changed=1`, and
`engine edit replacement verified`. The logs never include command text.
If `SetText ... readback failed` appears, revert the DLL and investigate the
engine call ABI instead of retrying interactions.

### Read-only key/submission diagnosis after the failed preview (17 Nov 2026)

The first history preview logged `chat history preview installed` and edit
focus for **both** HUD and Social, but no successful recall was observed.
The tester reverted the UI DLL. Neither focus message confirms that the
history model received a submit or an Up/Down key press.

A narrower diagnostic is now available: set **only**
`KILN_CHAT_HISTORY_DIAG=1` in the client's environment. It wins over
`KILN_CHAT_HISTORY=1` if both are set: **no Up/Down key is consumed, no
EditSetText is called, and no history file is written**. It still resolves the
native edit state and reads the submit event after `TextInputProcess`. It logs
only event codes, text byte counts, whether text starts with '/', arrow key
codes, modifier/capture/focus booleans; no chat text or sensitive arguments.

The diagnostic logs (at most 24 per event type):

- `ui: chat history diagnostic installed ...`: mode enabled.
- `ui: chat history: HUD/Social edit focus confirmed ...`: focused edit state.
- `ui: chat history trace: key Up ... focus=1 captured=0 consumed=0`: WndProc
  sees the arrow while focus is active; the diagnostic deliberately passes it on.
- `ui: chat history trace: HUD input event=1 readable=1 bytes=N command=1`:
  pressing Enter produced a readable slash-command submission, without logging
  content. Event 2 is Escape.
- No key line: WndProc is not seeing that key, or input is received from
  another path. A key line with `focus=0` implies a focus timing problem.
  No event-1 line after Enter: result placement / event type needs inspection.
  Event 1 with `readable=0`: edit-state text layout needs inspection.

`KILN_CHAT_HISTORY_TRACE=1` alongside `KILN_CHAT_HISTORY=1` enables the
same privacy-safe counters *during interactive recall*. Use that mode only
after diagnosing the basic key/submit path. The adapter now reads the event
discriminant as a single byte, avoiding mismatches from adjacent bytes.

### Interactive acceptance checklist

1. Test a boot with both environment flags unset: normal vanilla keys and no
   chat hook. With `KILN_CHAT_HISTORY=1`, confirm the log says
   `ui: chat history preview installed`; confirm separate HUD and Social edit
   focus log messages appear once each. **Abort if the game crashes or an
   unrelated textbox changes.**
2. With the chat edit focused, send two harmless commands; Up/Up/Down/Down
   cycles them and restores an unsent draft. Typing over a recalled command
   resets browsing. Enter sends and closes chat normally.
3. Verify Up/Down while gameplay has focus, after closing chat, in inventory
   search, on the menu, and after Alt+Tab are untouched. Escape and key releases
   remain normal. If not, disable the history flag immediately.
4. Quit normally and relaunch. The commands remain in the history JSON; ordinary
   chat and secret-like commands do not. Do **not** paste the contents of a
   private chat-history file into a public issue or support report.
5. Test Social separately and report any focus or text-replacement discrepancy.

#### Local host test

```sh
# Run from the repository root (Linux g++), without the game.
g++ -std=c++20 -Wall -Wextra -Werror \
    native/services/ui/tests/chat_history_test.cpp -o /tmp/kiln-chat-history-test
/tmp/kiln-chat-history-test
```

Native Windows cross-build (uses repo's xwin toolchain):

```sh
native/build.sh kiln-ui
```
