# Debugger acceptance matrix (Windows and Proton)

What a tester runs before [debugging.md](debugging.md) may call a target "verified". Each row is one attach session with
the sample `mods/samples/DebugCounter` (mod id `debug-counter`, deployed as a Debug build) and the pinned netcoredbg
3.2.0-1092. Never on a server or world people play on; a disposable character and world only.

Before every row: kiln.json `"developer_debugging": true` in the target, the target started fresh, and
`kiln debug … --json` names the right process (Windows PID, creation time, image path of this install). Write down the
Kiln build, the game build (`kiln/state/boot.json`), Windows or Proton version and the GPU.

"Survives" means: the same process (same Windows PID and creation time) afterwards, the counter keeps counting in
kiln.log, and for a client it still renders and takes input.

## Targets
| Id | Target | Machine (owner's pass) |
|---|---|---|
| WS | Windows dedicated server | W (kiln-win1) |
| WC | Windows game client | W |
| PC | Proton game client | L (this Linux PC) |
| WINE | Wine dedicated server (regression) | L, a lab |

## Rows
| # | Step | Expect | WS | WC | PC | WINE |
|---|---|---|---|---|---|---|
| 1 | `--json` without the kiln.json switch | `refused` (`developer_debugging_off`), nothing starts | | | | lab |
| 2 | Client without `--allow-client-pause` | `pause_consent_required`, nothing starts | n/a | | | n/a |
| 3 | `--observe` (client) | counter lines and the session mode; the game never pauses | n/a | | | n/a |
| 4 | Attach, breakpoint on DEBUG-COUNTER-BREAK | stops within 2 s; source, stack, local `count` | | | | lab |
| 5 | Step, evaluate `_total + 1`, continue | as asked; counter goes on | | | | lab |
| 6 | `kiln reload debug-counter` while attached | refused (`debugger_attached`) | | | | lab |
| 7 | Detach while running | survives | | | | lab |
| 8 | Reattach, detach while stopped | survives | | | | lab |
| 9 | Hold a breakpoint past `maxPauseMs` (5000) | the editor warns `timeout_continue`; the game runs on | | | | |
| 10 | Close the editor window while stopped | the adapter detaches; survives | | | | |
| 11 | Windows only: close the console that runs `kiln debug --dap` while stopped | detach, survives | | | n/a | n/a |
| 12 | Windows only: `kiln debug --dap` inside a kill-on-close job (e.g. a terminal that closes its jobs) | netcoredbg left the job (or a warning said it could not); survives | | | n/a | n/a |
| 13 | Joined: the client joins a server (W joins a lab on L, or L joins one hosted on W); hold 3 s, then 30 s | record whether the connection held and for how long, on both ends | n/a | | | n/a |
| 14 | Hosted world in the client; a second player (the other machine) joined; hold 3 s, then 30 s | record both ends' connection outcome | n/a | | | n/a |
| 15 | Reload (detached), attach again, breakpoint | stops again | | | | lab |
| 16 | Proton only: `--json` names the runner (`runnerSha256`, `env`); note any `ambiguous_runner` or `unsupported_identity` reason | one runner, the game's Proton | n/a | n/a | | n/a |

A row fails when the game ends, the wrong process is attached, or a client is paused without consent. A dropped connection
in rows 13 and 14 is not a failure; an unreported one is. Until every WS, WC and PC row passed on a build, the docs say
"built, not verified" for that target. The guided client checks `12-debug.client-windows` (W) and `12-debug.client-proton`
(L) walk rows 2-10 and 13-16.
