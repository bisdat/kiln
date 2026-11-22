# Kiln kiln-mode probe (test mod)

A small ShroudForge-format test mod for kiln mode, the way Kiln runs ShroudForge-format mods on its own interpreter.
In a world it calls the entity, cursor, prop, attribute, named service, loaded resource, voxel grid and building
members of the mod API once each and writes one line per call to `<game>/kiln/logs/kiln.log`, so one run shows what
answers and what refuses, and why. The guided checks of Kiln's client pass read these lines. It is for testing only.

## Install

1. Copy this folder (`mods/tests/sf-kiln-probe` in Kiln's repository) into `<game>/mods`.
2. In the Kiln Launcher, switch **Kiln kiln-mode probe** on in your profile.
3. In **Settings**, choose **Kiln's interpreter** on the ShroudForge card (kiln mode), then press **Play**.
4. Enter a **copied** world (host it, or join a friend who hosts one).

The probe runs once at its first update, then every 30 seconds, and each time you press its shortcut (**F5** by
default; F5 is none of the game's default keys) or its **Run probes now** button on Kiln's ShroudForge page (F9). Between runs it logs the building cursor
once a second. Its settings on that page change the shortcut, the times, the size of the prop box and the writes;
they apply at the next start.

## The log lines

Every line holds `[kiln-sf-kiln-probe] sf-kiln-probe:`, then the probe, the run number and the outcome:

```
sf-kiln-probe: <probe> run=<n> ok=<true|false|skip> <key>=<value> ... reason=<text>
```

- `ok=true`: the member answered; the fields say what it gave.
- `ok=false`: the member refused (it gave nil or false and a reason) or raised an error (`reason=error: ...`). A
  member the mod API does not have shows as `reason=<name> is missing`.
- `ok=skip`: the probe had nothing to call the member with in this run (no entity, no prop), or the writes are off.
- `has=` is what `runtime.has` answers for the member's operation; the probe calls the member either way.
- `reason=` is always the end of the line. Values never hold spaces; numbers have four decimals.

| Probe | What it calls | Fields when it answers |
|---|---|---|
| `loaded` (run 0) | `shroudforge.ui.on_action`, `shroudforge.input.keybind_label` | `button`, `key`, `interval`, `writes` |
| `run` | `runtime.world.session_id`, `context_kind` | `via` (timer, key or button), `session`, `context`, `writes` |
| `ecs.catalog` | `runtime.ecs.get_catalog()` | `entries`, `registry`, `reads_ready`, `writes_ready`, `writable` (rows marked `write_fields_available`), `transform_read`, `transform_write` |
| `ecs.query` | `runtime.ecs.query('keen::ecs::CurrentTransform')` | `count`, `first` (a handle) |
| `ecs.players` | `runtime.ecs.query('keen::ecs::PlayerInput')` | `count`, `first` |
| `ecs.read_bytes` | `runtime.ecs.read_bytes(first, CurrentTransform)` | `size` (`expected=56`), `x`, `y`, `z` in metres |
| `ecs.read_fields` | `runtime.ecs.read_fields(first, CurrentTransform, paths)`, the paths from the catalog's type | `paths`, `values`, `first` (up to four `path:value`) |
| `ecs.get_template` | `runtime.ecs.get_template(first)` | `guid`, `type`, `source`; when refused, the reason text |
| `world.cursor` | `runtime.world.cursor.get()` | `sequence`, `x`, `y`, `z` (the primary position in metres), `attachedToProp`, `material` |
| `resources.by_type` | `game.assets.get_loaded_resources_by_type('keen::ItemInfo')` | `count` |
| `resources.get` | `game.assets.get_loaded_resource(guid, 'keen::ItemInfo', part)` of the first | `guid`, `part`, `source`, `loaded`, `session` |
| `world.props.before` | `runtime.world.entity.query_props_in_bounds(box)`, once per world, before any recipe | `count`, or the reason |
| `world.props.nearby` | `runtime.world.entity.query_props(box, 1)` | `count` |
| `world.props.recipes` | `runtime.world.entity.register_prop_recipes(recipes)` | `recipes`, `from_iteminfo` (placement boxes of the loaded items), `generic` (a 1 m box for a nearby item without one) |
| `world.props.query` | `runtime.world.entity.query_props_in_bounds(box)` | `center` (player, cursor or origin), `box` (min x,y,z then max x,y,z), `count` |
| `world.props.transform` | `runtime.world.entity.get_transform(handle)` of the first prop | `handle`, `entityId`, `itemId`, `x`, `y`, `z`, `inside` (the position lies in the box) |
| `ecs.resolve` | `runtime.ecs.resolve(entityId)` of that prop | `handle`, `same` (the prop's own handle) |
| `attributes.list` | `runtime.ecs.get_attributes()` | `count`, `entries`, `version` |
| `attributes.read` | `runtime.ecs.read_attribute(player, name)`, the first of up to 16 listed names that reads | `entity`, `name`, `value`, `tried` |
| `services.get` | `runtime.services.get('ecs::GameSettings')` | `key`, `type`, `session`, `write_supported` |
| `services.read_fields` | `service:read_fields({'enableDurability'})` | `enableDurability` |
| `voxel.grid_spec` | `runtime.world.voxel.get_grid_spec('voxel')` | `id`, `origin`, `cellSize`, `maximum` |
| `building.status` | `runtime.world.building.status(1)`; it never submits a request | `status` (`unknown`: no request was made) |
| `ecs.write_fields` | `runtime.ecs.write_fields`, see below | `component`, `entity`, `field`, `kind`, `before`, `wrote`, `confirmed` |
| `ecs.write_fields.restore` | the old value written back | `value`, `confirmed`, `restored` |
| `done` | | `via`, `probes` |

The box around you is your player's position (its `CurrentTransform`) plus and minus the prop box half size (12 m
by default) on every axis, else the cursor's position, else the world origin.

## Writes (off by default)

With the **Allow writes (copied world only)** setting off, the probe only reads: `ecs.write_fields` logs
`ok=skip reason=Allow writes is off`. With it on, each run you start with the shortcut or the button (never a timed
run) picks one component: the one named in **Component to write**, or else the first, by name, that the catalog marks
`write_fields_available` and that has an entity and a plain number or flag field. It writes a slightly changed value
of that field (expecting the value it just read), logs what the write confirmed, then writes the old value back
(expecting the one it wrote) and logs that too. Use a copied world for this, and do not save it afterwards.

## Which checks read which lines

- `sfi.world-cursor`: `world.cursor` (the sequence grows, the position follows the cursor, `attachedToProp`).
- `sfi.world-props`: `world.props.before`, `world.props.recipes`, `world.props.query` (the count) and
  `world.props.transform` (`inside=true`); press the shortcut or the button after each placement.
- `sfi.world-ecs` on a copied world: `ecs.catalog`, `ecs.query`, `ecs.players`, `ecs.read_bytes` (`size=56`),
  `ecs.read_fields`, `ecs.get_template`, `ecs.resolve`, and with writes on `ecs.write_fields` and `ecs.write_fields.restore`
  (`restored=true`), hosting and joined.
- `sfi.attribute-writes`: `attributes.list` and `attributes.read` show what the attribute reads answer.
- `sfi.named-services`: `services.get` and `services.read_fields` (the `enableDurability` that SF Survival changes).
- `sfi.loaded-resources`: `resources.by_type`, `resources.get` and `ecs.get_template`; no line may say the
  ResourceService is missing.
- building input: `building.status`. Voxels: `voxel.grid_spec`.
