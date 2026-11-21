# Debugging mods

`kiln debug` attaches a .NET debugger to the mods in a running game process: breakpoints in your C#, the call stack,
locals, watch expressions, stepping. The debugger is [netcoredbg](https://github.com/Samsung/netcoredbg) (MIT), pinned to
one release. Kiln never uses Microsoft's vsdbg: its licence allows it only inside Microsoft's own editors.

There are four targets:

| Target | How | State |
|---|---|---|
| Dedicated server under Wine (Linux lab or server instance) | `kiln debug --server <lab>` | lab-checked (table below) |
| Dedicated server on Windows | `kiln debug --server <folder>` | built, not verified |
| Game client on Windows | `kiln debug --client <install> --allow-client-pause` | built, not verified |
| Game client under Proton (Linux) | `kiln debug --client <install> --allow-client-pause` | built, not verified |

"Built, not verified" means the code and its fake-process tests are in place, but nobody has attached to a real process
of that kind yet. The guided checks `12-debug.client-windows` and `12-debug.client-proton` and the tester matrix
([debugging-windows-matrix.md](debugging-windows-matrix.md)) are what turn them into "verified".

**A breakpoint pauses the whole game.** Mods run on the game's tick thread, so nothing moves while you look around. On a
server everyone connected freezes; a stopped client can lose its connection to the server it joined. Debug on a test
server and a disposable world, never on one people play on.

## Switch it on
The game or server must allow it: **kiln.json `"developer_debugging": true`** (the launcher: Settings, the Kiln card,
"Developer debugging"; a server: `<server>/kiln/kiln.json`), then start it again. Off is the default, and then Kiln
writes no file, opens no pipe and starts no thread for debugging.

With it on, Kiln writes `kiln/state/debug/<Windows PID>.json` (the descriptor, `schema/state-debug.schema.json`) once
.NET runs, and answers on a local named pipe that only the same user can open. The descriptor and the pipe are how
`kiln debug` proves which process it attaches to. They are an accident guard, not a sandbox: any program of the same
user can talk to them.

## Set up
1. netcoredbg, the **win64** build, because the game's .NET runtime is a Windows one, even under Wine and Proton. Pinned:
   `netcoredbg-win64.zip` of release 3.2.0-1092, sha256
   `3c410a45fa502415203a94fcb88654af65bf8e3dac158a5527a722e7a6b9274a`. The VS Code extension bundles it. For the CLI on its
   own, unpack it so that `netcoredbg.exe` is at `<data>/tools/netcoredbg/netcoredbg.exe`, or set `KILN_NETCOREDBG`.
   `<data>` is `~/.local/share/kiln` on Linux and `%LOCALAPPDATA%\Kiln` on Windows. `kiln debug` checks every file of
   that folder against the pinned release: at the default place (and for the extension's bundled copy) another build is
   refused (`tool_missing`, reason `netcoredbg_unpinned`); a copy you name with `KILN_NETCOREDBG` or the extension's
   `kiln.debug.netcoredbg` setting starts with a warning. Another release is not supported until it passes the same
   checks.
2. On Linux, `kiln debug` needs the Windows helper `tools/debug-inspect/win-x64/kiln.exe` with
   `tools/debug-inspect/manifest.json` (its SHA-256). The SDK folder of a release has it next to `kiln`; for a `kiln`
   downloaded on its own, unpack the release's `kiln-debug-inspect-<version>.zip` into `<data>` (it holds
   `tools/debug-inspect/`). `kiln debug` looks next to itself first, then in `<data>`, runs the helper under the
   target's own Wine or Proton to ask Windows about the process, and only when its hash matches the manifest. Missing or
   changed: `tool_missing`. `kiln debug-inspect --verify-bundle` says which helper it would use.
3. Build and deploy your mod in the Debug configuration, so its `.pdb` goes along and breakpoints bind.

## Attach
**In VS Code** with the Kiln extension ([vscode.md](vscode.md#debugging)): Run and Debug, "Add Configuration…", and pick
one of the four Kiln configurations. A client configuration only attaches with `"allowClientPause": true`:
```json
{ "type": "kiln", "request": "attach", "name": "Kiln: attach to Proton client", "client": "<install id or game folder>",
  "allowClientPause": true, "maxPauseMs": 5000 }
```

**In another editor** that speaks the Debug Adapter Protocol, use this as the adapter and send an `attach` request with no
`processId`:
```sh
kiln debug --server <lab> --dap
kiln debug --client <install> --allow-client-pause --dap
```

**To see what it found,** without starting a debugger: `kiln debug --server <lab> --json` (or `--client …
--allow-client-pause --json`). It answers with the proven identity: Windows PID, creation time, image SHA-256, session
nonce, and for Wine and Proton the prefix and the runner.

**To watch a client without pausing it:** `kiln debug --client <install> --observe` follows Kiln's log and the session
mode (`menu`, `hosted`, `joined`, `unknown`) and starts no debugger, so it needs no consent. `--json` gives one object
per line. A counter of your own goes in your mod's log, as the sample `mods/samples/DebugCounter` does.

`--server` takes a lab name, a server instance id, or their folder. `--client` takes a Kiln install id (the launcher's) or
the game folder.

## How the target is proven
`kiln debug` never picks a process by its name or a guessed PID. It reads the descriptors of the install you named and
then asks Windows about each one (in the same Wine prefix, through the helper; on Windows directly): the process with
that PID must exist with the same creation time, its image must hash to the descriptor's SHA-256, and the pipe must echo a
fresh challenge with the descriptor's session nonce. The whole tuple (install, prefix, runner, Windows PID, creation
time, image hash, nonce) is checked again right before the debugger is started and right before the `attach` goes to it.
Two servers with the same exe in other folders, a PID used again by another process, two prefixes with the same Windows
PID: each is refused, never guessed. Descriptors that are proven stale (no such process, or another creation time) are
removed by the next `kiln debug`, but only when that same check also shows the install alive (its server runs in its
prefix, the game's runner exists, or another descriptor is the live process): a helper that could have reached another
wineserver proves nothing, and then the files are kept and named. While attached, a check that times out proves nothing
either: the session goes on, and only the debuggee's exit or a field proven different ends it.

Under Proton, Kiln reads `/proc` only to find the runner of the game: the processes whose `WINEPREFIX` is that
install's prefix and whose image is that install's game. All of them must agree on the runner and on the allowed
environment (`WINEPREFIX`, `STEAM_COMPAT_DATA_PATH`, `STEAM_COMPAT_CLIENT_INSTALL_PATH`, `LD_LIBRARY_PATH`, `WINEDLLPATH`,
`WINEDLLOVERRIDES`, `WINEESYNC`, `WINEFSYNC`, `WINENTSYNC`); `LD_PRELOAD` is never copied, and none of these variables is taken from `kiln`'s own environment: netcoredbg and the helper get the game's values or none. Disagreement is
`ambiguous_runner`. A game inside the Steam Linux Runtime container cannot be reached from outside it:
`unsupported_identity` (reason `runner_unreachable`).

## Consent and pauses
- **A client is never paused without consent.** `--client` without `--allow-client-pause` (`"allowClientPause": true` in
  VS Code) answers `pause_consent_required` and starts nothing. Every attach can stop the game: there is no "safe" attach
  mode, and a DAP logpoint is not promised to run without a pause. Use `--observe` to look without stopping.
- **`maxPauseMs`** (100 to 60000; a client defaults to 5000, a server has no limit unless you set one): when a breakpoint
  holds the game longer, `kiln debug` sends `continue` itself and tells the editor (`kiln/debugState` with reason
  `timeout_continue`). This is best effort: an evaluation that runs game code, or a lock held by a stopped thread, can delay
  it, and a remote server or player can drop the connection anyway. It is not timeout protection.
- The editor gets every state change as a `kiln/debugState` event (`attaching`, `running`, `paused`, `detaching`,
  `detached`, `ended`, with a reason), and `kiln/state/debug/session.json` holds the current one.

## What `kiln debug` protects
| Rule | What it does |
|---|---|
| Only the proven process | See above. An `attach` with any other `processId` gets an error |
| Steam libraries stay out for servers | A server inside a `steamapps` folder is always refused. A client in Steam is allowed only through `--client` with consent |
| The game survives the session | Every `disconnect` goes out with `terminateDebuggee: false`, whatever the client asked. `launch`, `terminate` and `restart` get an error and are not advertised, so an editor's Stop button detaches |
| A vanishing editor detaches cleanly | stdin closing, SIGINT, SIGTERM or SIGHUP (on Windows: Ctrl-C, Ctrl-Break, closing the console, logoff, shutdown) make the proxy send its own `disconnect` and wait for the answer |
| The debugger is never killed | netcoredbg runs in its own process group (Windows: detached from the console and out of a kill-on-close job when the job allows it). If it does not answer, the proxy closes its stdin and leaves it running |
| Breakpoints go to the live mod | Before attaching, the proxy asks the game to collect unloaded mod contexts (control command `gc`) |
| No reload under a debugger | `kiln reload`, `kiln deploy --reload`, the SDK's `KilnReload` (KILN012) and VS Code's Reload Mod and Control command refuse while a live `kiln debug` session is attached; Kiln.Host itself refuses `reload`, `unload` and `reload-test` while a .NET debugger is attached to the process (reason `debugger_attached`) |
| The game going away ends the session | When the debuggee exits or stops matching its identity, the session ends with a detach, never a kill |

## What it cannot protect
- **Killing netcoredbg while it is attached ends the game,** with no shutdown and no save. Don't `kill -9` it, or a process
  tree that contains it (on Windows: don't end its process tree in Task Manager).
- **A stopped game can lose its connection,** whatever `maxPauseMs` says.
- **Your own expressions run in the game.** A watch expression can call anything; `System.Environment.Exit(0)` in one ends
  the game.
- **A mod that leaks its old copy leaves a trap** after a reload: breakpoints may miss. `kiln debug` warns when `gc`
  reports `N unloaded mod context(s) still alive`. Restart the game before debugging.
- **Don't drive netcoredbg by hand with `--interpreter=cli`.** Console sessions that ended while the server was paused
  killed it during testing.

## Good to know
- Breakpoints match on the source path recorded in the `.pdb`, so debug on the machine that built the mod.
- The stack below your code is Kiln's: `Kiln.Host.ModManager.Pre`, `HostRuntime.Pre`, `Entry.OnPre`.
- Under Wine, netcoredbg needs `DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1`; `kiln debug` sets it, along with
  `WINEDEBUG=-all`.
- **After a reload, attach again.** Breakpoints in a mod reloaded during a session stop nowhere; `kiln debug` says so.

## Lab check (netcoredbg 3.2.0-1092, GE-Proton11-11)
Run with `kiln debug --server <lab> --dap`, driven by a DAP test client, against the sample `mods/samples/DebugCounter`
(Debug build) on a disposable lab with `"developer_debugging": true` (0.2.0-dev.31, server build 26430253; run again after the review fixes on 2026-11-21). "Survives" means
the same process afterwards and the counter still counting in kiln.log.

| Check | Result |
|---|---|
| `kiln debug --json` | Windows PID 252 proven: creation time, image SHA-256, session nonce, prefix device and inode, runner SHA-256 |
| Breakpoint on the DEBUG-COUNTER-BREAK line | Bound about 3-4 s after the adapter started; stopped in `DebugCounterMod.Count` above `OnPre`, `Kiln.Host.ModManager.Pre`, `HostRuntime.Pre`, `Entry.OnPre` |
| Step, locals, evaluate | Next line; `count` = 9; `_total + 1` = 9 |
| `kiln reload` while stopped at the breakpoint | Refused: `refused` / `debugger_attached` |
| Continue, then disconnect with `terminateDebuggee: true` | Survives, proxy exits 0 |
| Attach again, disconnect while stopped | Survives, proxy exits 0; `kiln reload` works again afterwards |
| `--max-pause-ms 1000`, a breakpoint held without an answer | Continued by the proxy after 1.0 s (`kiln/debugState` reason `timeout_continue`); survives |
| SIGTERM, SIGINT, SIGHUP to `kiln debug --dap` while stopped at the breakpoint | Each: a safe detach (`detaching`, `detached`), proxy exits 0; survives |
| `kill -9` on `kiln debug --dap` while stopped | Survives (netcoredbg detaches when its input closes) |
| Two labs running, the same Windows PID (252) in both | `--json` proves each lab's own prefix and creation time; while A is paused its counter stands still and B counts on; B untouched |
| A server folder inside a `steamapps` library, by folder and through a symlink | Refused (`steam_library`) before anything runs |
| netcoredbg with one changed file | At the default place (or vouched for by the extension): refused, `netcoredbg_unpinned`; named with `KILN_NETCOREDBG`: starts with a warning |
| `kill -9` on netcoredbg while stopped (dev.27 lab) | **The server dies.** Nothing Kiln can do about it |

## When it says no
| Code | Means | Do |
|---|---|---|
| `refused` (`developer_debugging_off`) | No descriptor: the game was started without `"developer_debugging": true` | Switch it on, start again |
| `refused` | A Steam library server, several descriptors match, or a reload while attached (`debugger_attached`) | Name one server; detach first |
| `not_running` | The process of the descriptor is gone | Start the game or server |
| `runtime_not_ready` | .NET did not start in it | Check kiln.log |
| `identity_changed` | PID, creation time, image, nonce or runner changed between the check and the attach (the reason names which) | Attach again |
| `privilege_mismatch` | The game runs elevated (as administrator) and `kiln` does not | Run both the same way |
| `ambiguous_runner` | Processes of this Proton prefix disagree on the runner or its environment | Close other programs in that prefix |
| `unsupported_identity` | Windows (or Wine) cannot answer a query the proof needs, or the game is inside a container | See the reason |
| `pause_consent_required` | A client without `--allow-client-pause` | Add it, or use `--observe` |
| `tool_missing` | No netcoredbg, or the helper is missing or its hash does not match | See [Set up](#set-up) |
| `not_found`, `unsupported` | The server or install could not be found, or is remote | Check the name |

With `--dap`, the same message comes back as the error of every DAP request (with `kilnCode` and `kilnReason`), so your
editor shows it. Only `disconnect` succeeds; the adapter then exits 0.
