# Issue #68 — Factory station Fill UI: client-side investigation (research only)

Status: source review, **not an implemented fix or in-game acceptance** (2026-11-19).
Repository: `bisdat/kiln`, branch `research/issue-68-station-fill-client`.
Upstream issue: https://github.com/frostdev-ops/kiln/issues/68

## Objective and observed discrepancy

Finished items should stay in an opted-in production station yet be recognised by another station's **ordinary native availability display and Fill controls**, as if they were usable Magic Chest stock. Do not expose queued station inputs, copy stock into a chest, or alter the authoritative payment rules.

Hosted-game BSMS v0.1.4 / Kiln dev.29.17 observations after opting in the base:

- Charcoal Kiln: 20 finished charcoal, `output-source=True`.
- Forge: `output-source=True`; `Stations.Plan` saw `shared station outputs=20` for the charcoal ingredient (2 required) and Magic stock 0.
- The Forge's normal screen displayed **Charcoal Available: 0**. The initial full plan was `NotEnough` for the **other** ingredient (Metal Scraps); that first refusal is not evidence against the source API.
- Hence source registration and read-only stock discovery are demonstrated, but a native Fill execution and player crafting remain **untested**. The UI discrepancy is established independently.

## Source trace

| Concern | Existing implementation / evidence | Consequence |
| --- | --- | --- |
| Native factory-screen Fill enablement | `docs/re/stations.md` §10.3 (C7), client build 26430625. `FUN_1402b8430` performs a dry-run call to `AutoTransferRecipe` (client RVA `0x2be77f`) and rolls back. Error code 4 means missing materials. At client RVA `0xbda219`, a status of 1 is required for the enabled byte (stored at `0xbda22d`, along with two unrelated conditions). | This is a **different** code path from crafting-menu `CraftableAmount`; an opted-in station's stock is absent from the vanilla dry run. Client offsets are **build-specific research evidence**, not safe stable hooks. |
| Ordinary crafting-menu counts | `native/services/rules/creative.cpp`, `hk_craftable`; `sigdb/symbols/client.d/rules.json` (`rules/client.CraftableAmount`); `native/services/items/stations.cpp`, `kiln_stations_craft_adjust`. | An already-supported client hook can raise normal *craftable-recipe* counts using a server snapshot, but it is **not called by the factory Fill enablement path**. |
| Existing snapshot transport | `managed/services/Kiln.Blocks/World/Stations/CraftStock.cs`, channel `kiln:craftstock`. `Build` totals bag + Magic Chests + eligible outputs and `ClientChanged` watches `ScreenKind.Crafting`; factory screens map to that kind. | There is an established client/server transport and permission-gated output stock snapshot; **it does not carry a per-station Fill plan** (current selected recipe, loaded inputs, amount/slider, category selections, water/room or player-specific options). The aggregate alone cannot authorise the factory button. |
| Factory screen identity | `managed/services/Kiln.Ui/GameUi.cs`: `HudScreen.FactoryCrafting` maps to `ScreenKind.Crafting`; `CraftingScreen.IsFactory` and `Station` expose the target entity, but `CraftingScreen.Recipe` is 0 on a factory screen. | We can identify the current client target without guessing its entity ID. The authoritative selected recipe and requested Fill options must be obtained separately; never assume `CraftingScreen.Recipe` contains them. |
| Factory panel extension | `managed/services/Kiln.Ui/CraftingUi.cs`: `Ui.Crafting.AddPanel(..., AtFactories=true)`. | Suitable for **read-only diagnostics** beside a factory screen; does **not** alter the game's native availability text or button enablement. A custom Fill button would be a workaround, **not acceptance of #68**. |
| Existing server-side button interception | `managed/services/Kiln.Blocks/World/Stations/StationsOutputs.cs`, `OnSourceFill` / `SourceFill` via `PlayerClientActionEvent`. It parses station ID, recipe, amount, category choices, player; checks same base and permissions; compares plain and output-enabled `Stations.Plan`; on success calls `Stations.Fill`. | Server transaction and its validation are already implemented. **It cannot intercept a native click that the client never sends because the button is disabled.** Preserve this path rather than implementing a second payment mechanism. |
| Server plan and client limitations | `managed/services/Kiln.Blocks/World/Stations/StationsApi.cs`: `Stations.Plan` is `[ServerOnly, TickThreadOnly]`. | The render/UI thread must **not** call `Stations.Plan` directly. Hosting and joined clients both need a safe server-produced answer. |
| Capability gating | `sigdb/symbols/client.d/stations-craft-ui.json` covers `stations.craft_ui`, not native factory Fill UI. | Any future hook should have its own fail-closed, build-verified client capability; don't repurpose `stations.craft_ui` as evidence of factory support. |

## Recommended investigation sequence (not a patch yet)

1. **Confirm C7 in a real Kiln client**, including whether the native Fill hotkey still emits an action while the button is disabled. Instrument read-only UI state and/or observe the existing `StationPlayerActionEvent`; do not alter the status byte while collecting evidence. Check both Fill button state and displayed `Available` material count (these may be **two separate UI paths**).
2. **Identify version-resolvable client symbols** for the factory dry-run result, enabled-state derivation, and material count presentation. The documented RVAs are for one game build, not version-independent pointers; no hook without verified signatures and calling conventions.
3. **Prototype a read-only, per-station server plan response**, not a mutable stock proxy. The query/result should bind player, base, station entity, selected recipe, requested Fill amount/mode, category choices, and an age/sequence; the server uses the same permissions and `Stations.Plan` semantics as `SourceFill`. Ensure no side effects or inventory moves.
4. Evaluate a client-only **presentation and enablement adjustment** only when vanilla rejects for missing stock and a fresh matching server plan says the remaining materials **can be filled**. Never override unrelated reasons for disablement, or lower the game's count. Any changed or expired context must revert to vanilla.
5. If a permitted click is emitted, **leave the existing server `OnSourceFill` / `Stations.Fill` transaction authoritative**; re-check everything on receipt. Do not trust snapshot counts as transaction authority.

## Acceptance and safeguards before any PR

- Base opted in, Charcoal Kiln has 20 output charcoal (no charcoal in bag/real Magic Chests), Forge has sufficient Metal Scraps: the native display shows shared availability; Fill is enabled; clicking it consumes the required charcoal exactly once; kiln's queued logs remain.
- Native Fill stays disabled on a genuinely insufficient ingredient, no eligible outputs, opted-out station/base, wrong base, no access rights, station/recipe changed, unavailable capability, or stale/late response.
- Slider amounts, category ingredient options, full station, water, and disabled-for-other-reasons checks are not bypassed.
- Test normal Fill versus the possible Fill hotkey separately; hosting client and joined Kiln client, plus vanilla client untouched.
- Test save/reload and station deletion, concurrent inventory consumption, cancel/no duplication, and harmless failure when build signatures change.

## Current finding / handoff

**Confirmed from existing sources:** the client Fill enablement lives in the **factory screen's dry-run result/status flow** (`docs/re/stations.md` C7) and is separate from Kiln's existing crafting-menu count hook. The current `kiln:craftstock` transport is useful infrastructure but its aggregate is insufficient for a precise factory Fill authorisation.

**Not established:** a supported native hook for the factory status/counts, whether the disabled hotkey sends a request, or a complete safe client implementation. No binary signatures, test build, runtime behaviour or upstream acceptance are claimed here. Share consolidated results with upstream #68 **only when the owner authorises the follow-up**.
