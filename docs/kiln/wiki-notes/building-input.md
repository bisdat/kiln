# Wiki note: building input (BuildingInput), 0.2.0-dev.30.3

What changed for the public guide (frostdev.io/kiln/wiki, page `building-input`). Suggested text below; the guide for
mod makers is `docs/kiln/modding/building-input.md`, the technical reference `docs/kiln/services/client-building-input.md`.

## Suggested page: "Building as the local player"

> Mods can now build for you. Kiln's `BuildingInput` (Kiln.Client) sends your character's ordinary building input:
> select a building item, place, remove, undo and dismantle, as if you pressed the keys. The game's server still checks
> what you may build, your resources and the range, exactly as for your own input.
>
> - Works on the current game client (build 26430625); on other builds a mod sees it as unavailable.
> - One request at a time for the whole game; your own building input always goes first.
> - Kiln only hooks the game's input when a mod first uses it, so nothing changes for players without such a mod.
> - ShroudForge-format mods in kiln mode that build for you use the same input.
> - **Not checked in a real game yet.** If a mod builds the wrong thing, or nothing, tell us with your kiln.log.
>
> For mod makers: `BuildingInput.Select`, `Place`, `Remove`, `Undo`, `Dismantle`; ask `BuildingInput.Available`
> first and read each result (queued is not built). Native mods: kiln.client's `building_input_submit`, `_status` and
> `_cancel` in the SDK's `kiln_client.h`.
