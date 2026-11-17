# Custom game folders

Since 0.2.0-dev.29.17 the Kiln Launcher plays a copy of Enshrouded that no Steam library knows: an older build you keep
for mods, a copy on another drive, a backup of the game folder. Steam's Play only ever starts the copy in its own
library, so for such a folder Kiln starts the game itself.

## Adding one

On Play, use **Add** under the game folder (or **Add a folder** when Kiln found no game), or Settings, Games. Pick any
folder that has `enshrouded.exe`. A folder that is not Steam's own copy of Enshrouded is a **custom folder**: the game
picker and Settings say "custom folder", and the launcher asks how to start it right away, with **Start the game
directly** chosen. "Keep the default" keeps that.

Everything else works as for Steam's copy: Install Kiln, the mods list and profiles, the Library, copy and Z: path
modes, save backups before a start, the start progress bar, the stop watch (exit, copy-back of mod configs in copy
mode, crash reports) and the support report.

## How Kiln starts it

Play, then the game folder line, **Starts directly** / **Starts through Steam** opens the choice for the selected game:

- **Through Steam**: Play asks Steam to start Enshrouded (`steam://rungameid/1203620`), as Steam's Play button does.
  The default for Steam's own copy. For a custom folder this starts Steam's copy, not the folder.
- **Start the game directly**: Play starts this folder's `enshrouded.exe` itself, after the same profile, save backup
  and start steps as through Steam. The default for a custom folder.

The choice is kept per game in the launcher's record of the install (`<data>/installs/<installId>.json`, `start`).

### Windows

Kiln runs `enshrouded.exe` with the game folder as its working folder. Kiln's `dinput8.dll` is next to the exe, which
Windows loads first, so nothing else is set.

### Linux

The game runs under a Proton or Wine you pick:

- **Automatic** (the default): the Proton Steam uses for Enshrouded (Steam's compatibility setting for the game, else
  its default), else the newest one found.
- Any Proton Kiln found: GE-Proton and other compatibility tools (`compatibilitytools.d` of Steam, and the usual GE-Proton
  folders), and Valve's Proton builds in your Steam libraries.
- `wine` on your PATH, or **Another Proton or Wine…**: a Proton folder (the one with the `proton` script) or a `wine`
  program.

GE-Proton is the most reliable choice outside Steam. Valve's Proton builds expect Steam's runtime container and may not
start outside Steam on every system. A plain Wine needs Windows' Steam inside its prefix for the game's Steam sign-in,
so Proton is the choice for most players.

**Prefix.** Kiln makes one prefix per game, at the first start: `~/.local/share/kiln-prefixes/<installId>` (under
`$XDG_DATA_HOME` when set). It is outside the launcher's data folder and never moves with it. The saves of this copy
live in it. Or pick a folder: for Proton the folder that holds `pfx` (a Steam `compatdata` folder), for Wine the prefix
itself. The first start under Proton takes longer while Proton makes the prefix.

Kiln sets what the Steam launch option it advises for Steam's copy sets: `WINEDLLOVERRIDES=dinput8=n,b` (with
`xinput9_1_0` too when Ultimate ASI Loader is chained), and for Proton `STEAM_COMPAT_DATA_PATH` and
`STEAM_COMPAT_CLIENT_INSTALL_PATH` (your Steam folder). The dialog shows the exact command. The Proton or Wine output
of the last direct start is in `<data>/logs/game-start-<installId>.log`, and the support report carries it.

Steam Deck Game Mode is not covered.

## Steam must be running

The game signs in through Steam (its Steam API) however it is started. Start Steam before Play; the launcher says
"Steam isn't running" when it can't see it.

Kiln never touches the game's Steam or ownership checks: it doesn't write or change `steam_appid.txt`, doesn't change
`steam_api64.dll`, and doesn't set `SteamAppId` or `SteamGameId` the way Steam does when it starts a game. If a copy asks
Steam to start it instead, Steam starts the copy in its own library, and the custom folder doesn't run. Kiln doesn't
work around that.

## Unknown game builds

Kiln's hooks run only on game builds it has a verified record of (sigdb's build records, by the SHA-256 of
`enshrouded.exe`). The launcher checks the folder's exe against the records of the Kiln in the game (or, before Kiln is
installed, the Kiln the launcher would install). For a build it doesn't know, Play and Settings say:

> Kiln doesn't know this game build (`<first 12 hex digits>`). Kiln starts with the game but stays idle: no mods load.

The game itself runs normally. A Kiln update that knows the build fixes it. The support report says the same in its
`unknown-build` finding. Known builds say nothing extra.
