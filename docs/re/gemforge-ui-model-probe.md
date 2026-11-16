# Gem Forge native UI-model probe (research only)

Experimental fork-only patch, disabled by default. **Not built or validated in a live game**.

**Purpose:** determine whether the full gem catalogue shown by the existing native Gem Forge UI already exists in the game client's inventory model views, which Kiln's `ui_values_slots.cpp` traverses for slot-holder identification. The existing `Ui.Values.SubscribeSlots()` captures only rendered slot widgets and missed the visible gem catalogue.

**Opt in:** Set `KILN_GEMFORGE_UI_MODEL_PROBE=1` in the **game process environment before startup**. Attach the existing read-only `gforge ui-watch on` mod diagnostic (bisdat/kiln-mods draft PR #48), which installs `ui.slots.inspect`. When the game's HudScreen is ShroudForge (45), `kiln/logs/kiln.log` emits lines starting `gemforge-ui-model-probe:`.

**Read-only guarantee:** No game writes, new game hooks, ABI changes, item transactions, or alterations to native candidate rendering. The existing holder-reader already copies each view; this branch optionally extracts its copied `owner:page:slot=itemId*count` fields. The client IDs do not establish any server-side inventory authority.

**Limits:** Up to 512 nonempty view entries captured, up to 144 printed per report, up to four reports per forge-open session spaced 6 seconds apart. If the models cannot be read, `readable=0`; partial rows are never printed. This does not prove where the Gem Forge catalogue originates; absence in the model views narrows the native client provider investigation.

**Build:** From a local checkout of this branch with the documented Linux clang-cl/xwin SDK toolchain: `native/build.sh kiln-core` (output `native/build/cmake/bin/kiln-core.dll`). Building this DLL alone is **not** equivalent to producing or installing a compatible Kiln release. Avoid replacing the existing live Kiln core until the complete test-build and recovery process has been verified. Upstream dependency: https://github.com/frostdev-ops/kiln/issues/42.

**Fork workflow:** Keep this branch/PR unmerged. If validated in a disposable game profile, compare reported item IDs against the long GEMS icon grid. If the view models omit the grid candidates, seek a native item-grid provider hook instead of expanding this observer.