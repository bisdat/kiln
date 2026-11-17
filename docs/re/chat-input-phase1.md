# Native chat input: phase-1 history groundwork

**Status (fork experiment):** managed/services/Kiln.Ui/ChatHistory.cs and
managed/tests/Kiln.Ui.Tests/ChatHistoryTests.cs implement/test the history model.
**They are not connected to the live Enshrouded chat.** No game process, native
input hook or completion protocol has been tested by this change.

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

## Missing integration (required for a user-visible feature)

1. Add build-scoped sigdb symbols for the client's chat input and edit-state
   functions. The proposed TextInputProcess hook must discriminate the HUD
   and Social chat call sites; other edit controls must be left alone.
2. Add a narrow native input service that reports focused chat state and text
   on the game's **UI thread**. On that thread, use the game's SetText to
   apply one history result, preserving the game's edit buffer and cursor
   invariants. A hook failure must leave vanilla input entirely intact.
3. Consume Up/Down in the existing native WndProc only when the actual chat
   edit has focus. Never consume during gameplay, menu rebinds or an unrelated
   textbox. Queue requests from the window thread to the UI thread; do not
   call SetText from the WndProc.
4. On the native submit path, call Submit with the precise sent line, not
   with a reflected chat-log line. When the user types over recalled text,
   call Edited. Keep the history state on one thread.
5. Add the optional native service tail and managed host lifecycle plumbing
   without removing or changing existing kiln.ui ABI slots.
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
