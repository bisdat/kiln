# Kiln chat Tab completion — Phase 1b test overlay

This is a **development test overlay**, NOT a complete Kiln runtime or
launcher update. It builds a matched native `kiln-ui.dll` and managed
`Kiln.Ui.dll` / `Kiln.Chat.dll` from the same
`bisdat/kiln:dev/chat-tab-completion` commit.

## Requirements

- Enshrouded client running under Bazzite/Steam/Proton.
- Installed upstream Kiln **v0.2.0-dev.29.17**, with its normal client
  runtime, plugins, sigdb and native/managed services intact.
- No automatic Kiln update while the overlay is installed.
- **Exit the game completely before installing or restoring.**
- Keep a copy of the whole game's `kiln/` folder if you want a second
  fallback (the script separately backs up the three replaced files).

This overlay deliberately does **not** modify the game's saves, mods,
`kiln.json`, server binaries, third-party loaders, or launcher.

## Install

Download the `kiln-phase1b-test-overlay` artifact from the green
"Phase 1b native Windows DLL verification" run in the fork Actions tab
and extract it. It contains:

```text
README.md
install-test-overlay.sh
kiln/services/kiln-ui.dll
kiln/host/Kiln.Ui.dll
kiln/host/Kiln.Chat.dll
```

Then run from the extracted folder:

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
bash ./install-test-overlay.sh "$GAME" install
```

The script checks that Kiln and Enshrouded are present and that the game
is stopped. It copies each installed DLL into
`$GAME/kiln/data/phase1b-test-backup/` before replacing anything, and
refuses repeated installations while a backup is active.

### Enable completion (opt-in)

Edit the installed `$GAME/kiln/kiln.json` and add **one property** inside
the existing `"ui"` object:

```json
"chatCompletion": true
```

Preserve all of your existing options, including `"chatHistory"`.
If you prefer not to edit the config, the native override
`KILN_CHAT_COMPLETION=1` can be passed to the game's process, e.g.
through Steam Launch Options: `KILN_CHAT_COMPLETION=1 %command%`.
The feature remains OFF when neither option is set. Reopen the game
after changing this boot-time option.

## In-game checks

1. **HUD chat:** type `/he`; suggestions appear without consuming Up or
   Down. Tab accepts the first suggestion, repeated Tab cycles,
   Shift+Tab goes backward, Up/Down navigate when selection is active,
   and Esc dismisses only an active suggestion list. Enter should still
   submit through Enshrouded's normal chat widget.
2. **Social chat:** repeat all key tests. The overlay should follow the
   current skin and render within bounds at different UI scales /
   resolutions; click and scroll through suggestions.
3. **Empty/no match:** Tab and Esc behave as before; gameplay controls,
   gamepad, unrelated text fields and held/released keys are unaffected.
4. **History regression:** with `ui.chatHistory` enabled, passive Up/Down
   still recalls chat and restores a draft. With an active completion list,
   Ctrl+Up/Down is reserved for history; manual editing cancels recall.
5. **Input safety:** type Unicode, move the caret into the middle of a
   command, compose with an IME, switch focus or Alt+Tab; do not replace
   text after focus changes. No completion for ordinary (non-`/`) chat.
6. **Server permissions:** verify commands the player cannot execute are
   never offered; test both hosted worlds and a remote server. Completion
   only includes Kiln/Kiln mod commands, not other loaders' commands.
7. **Vanilla/older peers:** the optional protocol must not prevent joining
   or cause disconnects. Confirm any server/client log warnings without
   exposing the text that was typed.

**Do not propose upstream merge before all relevant tests pass.**
Record the precise Kiln version, Enshrouded build, and testing mode.

## Restore

Exit the game and run:

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
bash ./install-test-overlay.sh "$GAME" restore
```

The script restores the three original DLLs, preserves a timestamped
copy of the backup, and reminds you to disable `ui.chatCompletion`.
The launcher can subsequently update Kiln normally.

## CI provenance

The artifact comes from GitHub Actions on the fork's
`dev/chat-tab-completion` branch. The job checks for a successful
Windows x64 ClangCL native build and matching .NET 10 release DLLs
before uploading the candidate.

The ordinary Phase 1b CI separately runs the portable chat router
and ABI asserts, managed xUnit tests, and launcher i18n/typecheck.
A successful build is **not** an in-game acceptance result.

Draft upstream PR: https://github.com/frostdev-ops/kiln/pull/73
