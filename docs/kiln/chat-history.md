# Chat history

Up and Down in the game's chat field bring back lines you sent, as in a terminal. Your `/` commands are kept for the
next time you play. Client only: the server and other players see nothing different. First built by bisdat (GitHub
issue 44).

## Switching it on
Off by default. In the launcher: **Settings**, Kiln's settings, **In the game**, "Chat history". Or in the game's
`kiln/kiln.json`:
```json
{ "ui": { "chatHistory": true } }
```
Restart the game after a change. kiln.log says at start whether it is on (`ui: chat history on (kiln.json
ui.chatHistory): ...`), and on a game build Kiln has no definitions for yet, that the chat field stays the game's own.

## Using it
- In the HUD chat (Enter) and in the Social tab of the menu.
- **Up**: the line before. **Down**: the line after; past the newest one, the text you had typed comes back.
- Typing, Backspace, Delete, paste, cut, undo or redo in a recalled line keeps your change and starts over from the
  newest line next time you press Up.
- Enter sends the line as usual; Escape closes the chat as usual.
- Up and Down with Ctrl, Shift or Alt stay the game's. While an IME (Chinese, Japanese, Korean input) is composing,
  the arrows stay the IME's.
- Nothing else changes: arrows in play, other text fields, Kiln's own windows and other mods' key bindings get their
  keys as before. While the chat field has the keyboard, Up and Down go to the history only.

## What is kept
| | Kept |
|---|---|
| While the game runs | the last 200 lines you sent, commands and chat |
| After a restart | only lines that start with `/`, the last 200 of them, in `kiln/data/kiln.ui/chat-history.json` in the game folder |
| Never | lines other players sent; lines over 1024 bytes or with control characters; lines with a word that looks like a secret (below) |

A line is never kept, not even until you quit, when it holds one of these words, in any case: `login`, `register`,
`password`, `passwd`, `pass`, `pwd`, `token`, `key`, `apikey`, `secret`, `auth`. Words are runs of letters, so
`api_key` and `password123` count, and `keyboard` or `monkey` do not. A word written together with another
(`myToken`) is not caught: type secrets with care.

The file is plain JSON, oldest first. Delete it to forget your commands. It belongs to the game folder, not to a
Steam account or a server: everyone who plays from this folder shares it.

## For mod authors
- There is no API: the history is the player's, and Kiln never reads or logs the text (kiln.log gets counts only).
- It does not see your mod's chat messages or replies, only what the player sends from these two fields.
- Kiln hooks the game's text-field function and acts only on the HUD and Social chat calls (by return address). A
  mod that draws its own text field, or another loader that hooks the same function, is not touched.
- `subscribe_key` handlers do not get Up/Down presses (no modifiers) while the chat field has typing focus. Releases
  always reach everyone.

## Diagnostics
Environment variables, for testers; they win over the setting:

| Variable | Effect |
|---|---|
| `KILN_CHAT_HISTORY=1` / `=0` | on / off, whatever kiln.json says |
| `KILN_CHAT_HISTORY_TRACE=1` | with history on: counts and flags in kiln.log (lengths, whether a line was a command), never text |
| `KILN_CHAT_HISTORY_DIAG=1` | watches keys and sent lines without taking any key or keeping anything |
| `KILN_CHAT_INPUT_PROBE=1` | only logs that the HUD and Social chat calls were seen |

## Tab completion
Tab in the same two chat fields completes Kiln commands: `/he` and Tab gives `/help`. First built by bisdat (GitHub
issue 44, pull request 73). Off by default, and separate from the history: launcher **Settings**, Kiln's settings, **In
the game**, "Chat completion", or `{ "ui": { "chatCompletion": true } }` in `kiln/kiln.json`; restart the game after a
change (`KILN_CHAT_COMPLETION=1` / `=0` wins over the setting, for testers).
- Start a line with `/`: a list of matches shows above the chat field (command names, subcommands, choices, player
  names, the same as the server's own `/help` knows).
- **Tab** puts the first match in. With one match that's it; with several, the list stays and Tab again puts the next
  one in (it keeps what you typed, so it goes round all of them). Once Tab has put one in, **Up** or **Shift+Tab** puts
  the one before in and **Down** the next (Shift+Tab may be Steam's overlay key; Up does the same), and **Esc** closes
  the list; a second Esc closes the chat as usual. Clicking a match puts it in too. Typing anything starts over.
- Only commands **you** may use are offered: the server answers with your own permissions, so an admin command never
  shows for a guest, and console-only commands never show in chat.
- Only Kiln's commands (Kiln's own and those of Kiln mods). Other loaders' chat commands are not known to Kiln and are
  not offered; they still work as before.
- It needs Kiln on the server (or your own hosted world). On a server without Kiln, or with a Kiln from before
  completion, Tab and the chat field stay the game's own and nothing is asked again until you join another server.
- Tab with no matches showing stays the game's, and so do Up and Down until Tab has put a match in (then the history
  waits until the list closes). Kiln never logs what you type: a `/` line goes to the server only to ask for matches,
  and the server neither runs it nor writes it anywhere.
- On a game build Kiln has no definitions for yet, completion stays off on its own (kiln.log: `ui: chat completion
  unavailable: ...; history unaffected`).

How it works and what to check after a game update: [docs/re/chat-input.md](../re/chat-input.md). Definitions:
`sigdb/symbols/client.d/chatui.json` (feature `chat.ui.input`; completion also `chat.ui.completion`). The server side of
completion: [services/chat.md](services/chat.md#tab-completion-in-the-games-chat-kilnchat).
