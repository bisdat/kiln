# Station Fill client probe (Issue #68)

**Read-only diagnostic mod for every production station.** The Charcoal Kiln → Forge is just one sample; opening a Smelter, Kiln, Loom, Seed Bed, etc. follows the same path. This is not a BSMS replacement and is **not** a native Fill UI fix.

Branch: `research/issue-68-station-fill-client` in `bisdat/kiln`. No PR or change to `main`.

## What it measures

In a **Kiln-hosted world** (single-player or hosting friends), open any production station's normal screen. Kiln's `CraftingScreen.IsFactory` and `Station` identify the target. A non-interactive side panel compares the server's read-only `Stations.Plan` for **+1 craft** (exact Add mode, player bag included):

- With the player's bag and Magic Chest stock, **excluding** station outputs.
- With opted-in output sources **included**.
- Per-ingredient station input amount, ordinary stock and shared output amount.
- Actual `StationPlayerActionEvent` Fill/Select/use-all actions received by the server are logged, without cancellation.

Every change of the plan is logged as `fillprobe:` in `kiln/logs/kiln.log`. No `Stations.Fill`, `Items.Move`, `Stations.Collect`, `e.Cancel()`, native detours, game memory patches or changes to the vanilla UI are performed.

**Limitations:** The panel does *not* read the native button's enabled byte, the native displayed `Available` quantity, the current slider amount or every dynamically selected ingredient option; compare these visually. The probe shows the *server's* +1 craft case, not the complete live native request. Absence of a Fill event is only evidence that **no Fill action reached the server**, not proof of the client's exact rejection reason. The server-side plan and panel use the same process: joined clients are not yet supported for this first probe.

## Build for Kiln dev.29.17 (API 0.2.9)

**Important:** This research branch's own SDK is already API **0.2.10**. A normal `dotnet build -t:KilnPack` there creates an incompatible ZIP for dev.29.17. Do **not** override the generated manifest's `apiVersion` to disguise this mismatch.

Requires Git, .NET SDK 10 and Python 3. From the research checkout **inside the Kiln build Distrobox**, run:

```bash
bash mods/tests/station-fill-client-probe/build-for-dev29.17.sh
```

The script uses a separate cached copy of upstream **tag `v0.2.0-dev.29.17`**, verifies that its SDK is **API 0.2.9**, copies only this probe's source/project into the tagged tree, builds it with the actual matching SDK, and validates the resulting ZIP's manifest. It does not alter the research branch's `main` or any other Git branch.

The verified local output is written directly to **`~/Downloads`** (the GitHub Actions build overrides this to its own artifact staging directory):

```text
~/Downloads/station-fill-client-probe-0.1.1.zip
```

Import **0.1.1** using the normal Kiln Launcher procedure; disable/remove the previously imported **0.1.0** which requires API 0.2.10. Never enable two versions of the same mod.

The branch-only packaging workflow also uses the pinned script and creates no release.

## Hosted-game check

1. Keep BSMS `ShareOutputs=true`, `CollectOutputs=false`; base enabled and relevant station types opted in. Avoid other automatic fill/collect changes during the check.
2. Put eligible finished outputs into one producing station; have another station's normal recipe require them. Repeat across several station **types**, not just Forge.
3. Open the *receiving station's* normal interface. Capture the native **Available** label and enabled/disabled Fill control, alongside the Kiln probe panel's `+1 craft` comparison.
4. Record the receiving station's type for each observation; cover different input counts and water-dependent stations, not only the charcoal example. Optionally try the native Fill hotkey while the button is greyed out, only after preserving your save and choosing harmless test quantities. Check whether an `ACTION-RECEIVED ... kind=Fill` line appears. A transmitted Fill *may change inventory* through the game's real handler; the probe itself never sends one.
5. Close that station; repeat for other station types. Compare results and capture `fillprobe:` log lines.

From the Bazzite terminal while the game is running, retrieve the last probe messages:

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
grep 'fillprobe:' "$GAME/kiln/logs/kiln.log" | tail -n 120
```

Optional atomic console command (with exactly one player hosting):

```bash
GAME="$HOME/.local/share/Steam/steamapps/common/Enshrouded"
CONSOLE="$GAME/kiln/control/console"
RUN="fill-probe-$(date +%s)"
printf 'fillprobe status\n' > "$CONSOLE/$RUN.tmp"
mv "$CONSOLE/$RUN.tmp" "$CONSOLE/$RUN.cmd"
for i in {1..20}; do
    [ -f "$CONSOLE/$RUN.result.json" ] && break
    sleep 0.5
done
cat "$CONSOLE/$RUN.result.json"
```

## Interpretation

- **Plain fails, shared succeeds, normal Fill disabled:** verified client-side workflow gap for that station.
- **Both fail:** inspect another missing ingredient, capacity/water, selected recipe and options; don't attribute it to station-output sharing.
- **Both succeed:** the vanilla button may be enabled anyway; it is not a proof that native Fill uses shared outputs.
- **`ACTION-RECEIVED kind=Fill`:** the native client emitted the action; check the already-present Kiln server-side Fill handler separately before claiming success.
- **No action line:** nothing was observed reaching the server, not proof of a particular reason.

Do not merge/release this research mod as a feature. No upstream issue comment or PR until the owner authorises consolidated results.
