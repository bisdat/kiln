# The client gate's list

**0.2.0-dev.30 ships with this list empty too** (owner, 2026-11-20: "nothing held, I will test before release"; the equipment tooltips' `equip-tooltips.json` was removed). **0.2.0-dev.29 ships with this list empty.** The owner decided on 2026-11-14 that nothing ships held: every capability
0.2.0-dev.28 held (22 rows: world queries and paths, terrain overlays, tool wear, values in the game's UI, the client
asset catalog, a mod's OnBoot, resource patches and boot VFX on a client, program wraps and replication watching on a
client, weapon follow-ups, transform scale) is on
for every player, and the guided checks stay as the list of what a real game has not confirmed yet
(`tools/client-checks/plan.py`, the release notes' "On, not yet confirmed in a real game"). The mechanism below stays
for a capability the owner decides to hold; a held default is the owner's decision at each release, asked as its own
question.

**Since 0.2.0-dev.30.2 one row ships held:** `kiln-sfi.sf18` in `sfi.json` (anything that needs ShroudForge 1.8). It
stays held whatever `"preview"` says: the release policy holds it in every build
([docs/kiln/services/launch.md](../../../docs/kiln/services/launch.md#the-held-row-and-the-kiln-sfisf18-veto)). Kiln's
interpreter for ShroudForge mods has no row: it is a normal choice in Settings > ShroudForge (kiln.json
`shroudforge.backend: "kiln"`), and hosted ShroudForge stays the default. A `kiln-sfi.interpreter` row on both sides
would hold it as a preview until kiln.json `"preview"` lifts it; a row on one side only holds it on both.

**Since 0.2.0-dev.30.3 a second row ships held:** `client.building_input` in `client-input.json` (the game client only):
kiln.client's building input, until its five guided checks pass in a real game
([docs/kiln/services/client-building-input.md](../../../docs/kiln/services/client-building-input.md#held-until-its-guided-checks-pass)).
While it is held the producer is not installed at all.

Kiln ships some capabilities switched off because only a real game client can confirm them: nobody but a player
can say that a camera does not clip, that a marker shows on the compass, that a joined vanilla player still sees a
sane world. Such a capability is **held** until its guided client checks (`mods/tests/client-pass/`,
[docs/kiln/client-pass.md](../../../docs/kiln/client-pass.md)) have passed in the real game.

Each file here lists held capabilities. One file per work area, so two areas never edit the same file:

```json
{ "gates": [
    { "name": "camera.fov", "checks": ["03-c11", "03-c12"] },
    { "name": "markers.compass", "checks": ["04-m03"], "side": "client" }
] }
```

- `name`: the capability's name in kiln.caps ([docs/kiln/services/caps.md](../../../docs/kiln/services/caps.md)). A
  sigdb feature, a service's own row or a managed row: the gate holds any of them. A name appears in one file only.
- `checks`: the ids of the guided client checks that open it, lower-case, at least one.
- `side` (optional): `"server"` or `"client"` when the capability is held on that side only. Without it, on both.

The list is compiled into `kiln-core.dll` (`gen.py` writes `kiln_gates.g.h` when the core is built), so an install
cannot lose it. While a name is held the core reports it `unavailable`, reason `policy`, text
`game test pending: <check ids>`; `KilnApi.feature()` answers false for it; a service installs the hooks of that
capability only when the row is not held (`kiln_caps_held` in `kiln_caps.h`, `Capabilities.IsHeld` in managed code).
`"preview"` in `kiln.json` lifts the gate: `true` for every name, or a list of names. The guided pass runs with
`"preview": true`.

To open a capability for good once its checks have passed: remove its entry here, in the commit that records the pass.
The owner may also open one on a live test of their own instead of the guided check; the commit then says what was
run and which of the entry's checks were not (0.2.0-dev.29: `mods.boot.client` and `mods.boot.resource_patches`, on a
hosted game with a resource-patch mod; `08-loader-order` was not played).
