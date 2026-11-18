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
kiln/locales/en.json
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

### Updating an existing test installation

If you installed an earlier Phase 1b overlay, **restore that overlay
first**, while Enshrouded is closed, using the installer in its
originally extracted directory. This preserves the first overlay's
original three-DLL backup.

Only then install this newer artifact. The new package backs up and
replaces **four** files: the three DLLs plus
`<game>/kiln/locales/en.json`. It contains the two missing UI
translations (`chat.completion.title`, `chat.completion.hint`) and
restores the old locale automatically when rolled back.

### Enable completion (opt-in)

If completion is already enabled in your valid `kiln.json`, leave it
unchanged. Otherwise the launcher offers **Chat completion** under
Kiln settings (off by default), or a Steam launch option can enable the
native side without editing JSON:
`KILN_CHAT_COMPLETION=1 %command%`.

To enable completion explicitly in the existing `kiln.json` without
hand-editing JSON, run this entire command **with the game closed**.
It backs up the file, validates the original JSON and preserves other
settings. It refuses a JSONC file with comments or trailing commas
rather than risking a bad rewrite:

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
python3 - "$GAME/kiln/kiln.json" <<'PY'
import json
import shutil
import sys
from pathlib import Path

path = Path(sys.argv[1])
config = json.loads(path.read_text(encoding="utf-8"))
if not isinstance(config, dict):
    raise SystemExit("kiln.json must contain a JSON object")
ui = config.setdefault("ui", {})
if not isinstance(ui, dict):
    raise SystemExit("kiln.json: ui must be a JSON object")
backup = path.with_suffix(path.suffix + ".before-chat-completion")
if backup.exists():
    raise SystemExit(f"Backup exists: {backup}; refusing to overwrite")
shutil.copy2(path, backup)
ui["chatCompletion"] = True
updated = json.dumps(config, ensure_ascii=False, indent=2) + "\n"
json.loads(updated)
temporary = path.with_suffix(path.suffix + ".phase1b-new")
try:
    temporary.write_text(updated, encoding="utf-8")
    temporary.replace(path)
except BaseException:
    temporary.unlink(missing_ok=True)
    raise
print(f"Valid configuration installed; prior file: {backup}")
PY
```

Do not paste a lone JSON key/value fragment into a file. Restart
Enshrouded after changing the boot-time setting.

## In-game checks

1. **HUD chat:** type `/he`; suggestions appear without consuming Up or
   Down. Tab accepts a suggestion into the game's editor and dismisses the
   popup. A fully typed or recalled `/help` must not open a self-suggestion
   popup. Esc dismisses an active suggestion list; Enter must still submit
   through Enshrouded's normal chat widget. Steam captures Shift+Tab to
   open its own overlay by default, so completion does not advertise it.
2. **Social chat:** repeat the key tests. Verify the hint is fully visible,
   the command choices use the game's font at a readable size, the popup
   does not overlap the input, and the window stays within bounds at
   different UI scales/resolutions. Click and scroll through suggestions.
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

The script restores the three original DLLs and the locale file, preserves a timestamped
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
