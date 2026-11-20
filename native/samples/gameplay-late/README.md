# gameplay-late: first demand in a running world

A native `kiln_plugin_main` lab plugin. It obtains the gameplay table but requests/subscribes to nothing at load.
It reads one atomic-renamed command per server Pre from `kiln/control/gameplay-late.command` (not a `*.cmd` file:
Kiln.Host's control reader takes every `*.cmd` in that folder as one of its own commands). It needs
`kiln/control/gameplay-late.lab` **before load**. The service's separate failure control also needs
`kiln/control/gameplay.lab` **before service load**. Never put these markers in a player's installation.

Build only the two targets, serially, from the worktree (do not execute the DLLs):

```sh
export KILN_OUT=<your build dir>
native/build.sh kiln-gameplay kiln-gameplay-late
```

With the lab stopped, stage the reviewed DLLs:

```sh
mkdir -p "$LAB/kiln/plugins"
cp "$KILN_OUT/cmake/bin/services/kiln-gameplay.dll" "$LAB/kiln/services/"
cp "$KILN_OUT/cmake/bin/samples/gameplay-late/kiln-gameplay-late.dll" "$LAB/kiln/plugins/"
```

With an explicit
profile, include it in `mods.plugin` as `{ "id": "gameplay-late", "path": "<absolute Windows DLL path>" }`;
with plugin drop-ins enabled the plugins folder suffices. This is the same plugin entry mechanism as
`mods/tests/native-test/stage.py`'s native-plugin case; it is not a managed mod or a native.json library.

Keep the **new demand-aware Kiln.Host enabled**, with no Gameplay/facade-consuming managed mods/data packs in the
profile. No host means legacy fallback and every group is already installed, so it cannot exercise late demand.
The fixture's load line is `gameplay-late: ready; passive until control command (tick API 0)`.

Coordinator shell setup (LAB is the lab game directory, not this worktree):

```sh
mkdir -p "$LAB/kiln/control"
touch "$LAB/kiln/control/gameplay-late.lab"
rm -f "$LAB/kiln/control/gameplay.unchecked"
send() {
  printf '%s\n' "$*" > "$LAB/kiln/control/gameplay-late.tmp"
  mv "$LAB/kiln/control/gameplay-late.tmp" "$LAB/kiln/control/gameplay-late.command"
}
```

Start the lab using its existing coordinator procedure, then wait until its world is running. Send each command
only after the previous `gameplay-late: step ...`/result line. Do not preload gameplay-test/events-test or any other
managed Gameplay consumer: their metadata prearms the hooks before the experiment.

## Subscriptions and real payloads

```sh
send status
send craft
send build
send spawn
send joined
send left
```

`status` must first show each group at `state 0 targets 0: resolved, not installed: no mod asked`, with no
Gameplay-owned links in the map. Each subscription prints its API result and the group's before/after state:
`gameplay-late: step craft API 0; craft state 0 targets 0 -> state 2 targets 0; ...`.

For every group requested, require the service lines:

- `kiln.gameplay: <group> hooks waiting for safe Pre (owner <owner>); events and rules begin after installation`
- `kiln.gameplay: <group> hooks installed on first use by <owner>`

And the fixture line:

- `gameplay-late: PASS <group> waiting then installed after <n> Pre(s)`

Each write while the world runs is timed by the service, one line per target (the installer stops the game's other
threads while it writes):

- `kiln.gameplay: <target> went in during play: the installer call took <ms> ms on the world's thread (write <k> in this Pre)`

and the fixture prints the step intervals around it (`gameplay-late: step interval before the command: median ...`, then
`gameplay-late: step <n> interval <ms> ms` for every step while a group waits and three after). The service's Pre runs
before the fixture's, so a write in step N shows in the interval of step N+1. `log_level` `debug` in `kiln/kiln.json`
adds kiln.hooks' own line with the time the other threads stood still.

Craft/spawn normally need one eligible Pre, entities one (two targets), build three. BUILD also requests terraform;
when terraform was idle, six combined targets still need three Pres. Concurrent all-group ordinary demand needs
five Pres. The count is relative to the command, not elapsed wall time. A real thread refusal can extend it.
Earlier operations are not replayed. Unavailable groups print FAIL with their reason rather than a false PASS.

Perform actual operations after installation: player crafting, placing and dismantling/picking up pieces,
terraforming, a natural random-spawner activation, entity creation and destruction. These print
`gameplay-late: event <group> kind ... op ... actor ... target ... flags ... zone ...`.
Then:

```sh
send verify craft
send verify build
send verify terraform
send verify spawn
send verify entities
send unsubscribe
send status
```

Require `gameplay-late: PASS no event before installation, <k> after (<group>, early 0)` for each, with k > 0.
A status transition alone cannot pass that check. Repeat real operations and check that no fixture event arrives
after unsubscribe; require `gameplay-late: PASS unsubscribe leaves installed/pending hooks in place` and unchanged
installed target counts. `joined` and `left` subscribe independently to the two lifecycle kinds and enable entity_filter ALL (API 0).
`entities` is an optional combined shorthand. The entity verification requires at least one payload of each kind;
inspect the `entity payload counts joined <n> left <n>` line as well.
Unsubscribe restores the isolated fixture's category mask to zero (API 0).
The fixture avoids duplicate subscription masks even when commands are repeated.

## Zones: use a fresh process for each first-use deny

Each first-use zone experiment needs its group idle. Restart the lab without consuming mods, then choose one:

```sh
send zone build 100 10 -50
send zone break 100 10 -50
send zone terraform 100 10 -50
send zone spawn 100 10 -50
```

Do **one** per fresh process. Coordinates are an example; use a disposable spot in the lab world. The first API
answer must be UNAVAILABLE (3), with `gameplay-late: PASS pending zone refused without storage`. Once installed,
the fixture retries that same zone and must print:

`gameplay-late: PASS zone with <deny> refused while waiting, accepted after (API 0, zone <id>)`.

Deny numbers: build=1, break=2, terraform=4, spawn=64. Exercise a real denied operation inside the zone and an allowed
one outside. Then `send zone-remove`: API 0. `send zone craft ...`, `send zone interact ...`, and `send zone loot ...`
exercise the other deny bits (32/8/16); they use Pre interception and should succeed immediately, without requesting
a build/craft observer. Remove the zone between commands. No deny bits are silently dropped.

## Pending natural-spawn replacement: a fresh process

Choose two real template GUID byte strings (32 hex digits in **memory order**), FROM from a natural spawn in the
chosen camp, TO a valid compatible template. They may be identical to test activation without changing species.
In a fresh idle-spawn process:

```sh
send replace "$FROM" "$TO"
```

Require the step API 0 and pending state, then the spawn installation line. Activate that camp's random spawner.
Only a real NaturalSpawnEvent with Replaced and the TO GUID permits:

`gameplay-late: PASS replacement kept pending, active after (real replaced spawn <entity>)`.

Until that payload appears there is **no** replacement PASS. Finally `send replace-clear` must print API 0, removed 1;
`send unsubscribe` removes the payload subscription. Repeated rule commands overwrite the fixture's same pair as the
API normally does; use one pair per experiment so cleanup's expected count is unambiguous.

## Refusal and inert partial links: another fresh process

Before starting it, add the second marker:

```sh
touch "$LAB/kiln/control/gameplay.lab"
rm -f "$LAB/kiln/control/gameplay.unchecked"
```

Service startup must log `kiln.gameplay: LAB unchecked control enabled by gameplay.lab; never enable on a player's server`.
Then, **during its world**, issue:

```sh
send force-unchecked build 2
send build
```

This writes `build 2` into `gameplay.unchecked`. Target indices are zero-based: 0 blueprint, 1 prop, 2 dismantle,
3 dismantle2, 4 pickup. The control returns UNCHECKED **before calling the installer for that target**. Thus the
first two build entries become pass-through links; target 2 and later entries are untouched by gameplay.
Terraform may install too because BUILD interest requests it. Require three distinct eligible Pres with:

`kiln.gameplay: LAB build target 2 unchecked attempt <1|2|3> (nothing written)`.

Refused for threads it could not check three times while the world runs, the group is not refused for the process:
it waits for the next moment no world runs (a client's menu; on a dedicated server, which has none, the next server
start). Require exactly one line

`kiln.gameplay: build hooks wait for the next no-world moment (the next server start): gameplay/DismantleBlock thread check refused on three attempts during play; see kiln.log`

and the fixture's

`gameplay-late: PASS build waits for the next no-world moment after 3 attempts: <reason>; nothing written past target 2`.

Check build state 2 (pending, not refused), BuildHooks false, mask 3, no gameplay entries for the final three targets
in the hook map, no build payloads/denies from the first two links, and no fourth attempt after further ticks. The
fixture's PASS reads status/reason; **the hook map and real operations are required to prove physical
writes/pass-through behavior**. `send zone build ...` must answer UNAVAILABLE while it waits and store nothing.
`send unsubscribe` still succeeds. A refusal for another reason (the code does not fit its descriptor) stays a
refusal: `gameplay-late: ... refused after ...`.

The same forced refusal **before any world** (the marker files written before the start, and a mod in the profile
that makes Kiln ask for the group before the world, e.g. gameplay-test) shows the fallback instead: with no world
running a refusal for unchecked threads installs the target unchecked, as dev.28 installed these hooks at start:

`kiln.gameplay: LAB build target 2 unchecked attempt 1 (nothing written)`, then
`kiln.gameplay: build hooks: gameplay/DismantleBlock installed unchecked: no world running (the checked install refused by gameplay.unchecked)`.

Repeat in a fresh process with `force-unchecked craft 0`, `terraform 0` (demand via `zone terraform ...`), `spawn 0`
(demand via `replace FROM TO`: the pending rule stays registered while the group waits), and `entities 1` (the first
lifecycle link stays inert; demand with `joined`, then `left`). `send replace-clear` removes the pending rule normally.
A single-entry group's installed count is 0; entities' partial count is 1.

With `gameplay.unchecked` present **without** gameplay.lab at service load, require the explicit IGNORED warning
and ordinary successful installation; the switch must not force refusal. Delete both service control files and the
fixture marker after the lab. Nothing here certifies host/client thread safety without the coordinator's real run.
