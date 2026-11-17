# Resource patches: change a field of the game's data before the game reads it

Some of the game's data is read once, when the game starts: the balancing table (experience per level, Flame Altars
per flame level, the level cap) is the best-known case. A change made in `OnLoad` comes too late for it: Kiln serves
your changed copy, but the game already holds what it read.

A resource patch is made earlier. You state it in `Mod.OnBoot` (or in a blocks file), Kiln checks it before the game
starts, and writes it at the moment the game's resource service is up and nothing has read from it yet.

Needs a mod with `"boot": true` in `kiln.mod.json` (see [OnBoot](onboot.md)) and the capability
`mods.boot.resource_patches`.

A start in which the game's resource service was already up before the boot generation refuses mods with resource patches (an optional mod is left out; a required mod stops the start).

## In a blocks file

```json
{
  "format": 1,
  "resourcePatches": {
    "more-altars": {
      "format": 1,
      "resource": "82706b40-61b1-4b8f-8b23-dcec6971bda1",
      "type": "keen::BalancingTable",
      "set": {
        "altarsPerFlameLevel[*]": { "min": 16 },
        "experienceNeedPerLevel[0]": 1500
      }
    }
  }
}
```

- `resource`: the resource's GUID. `type`: its qualified type name. `part`: the part index, 0 when left out.
  [Typed fields](typed-fields.md), "Any resource", shows how to list a type's resources and their members.
- `set`: member path to value.
- A patch needs at least one change: `set`, or `append`, `addComponents`, `removeComponents` (below). A member the patch
  does not know (`"sett"`) is refused.
- The file is read as every blocks file is: comments (`//`, `/* */`) and trailing commas are fine.

## Paths

| Form | Means |
|---|---|
| `playerLevelCap` | a member |
| `a.b.c` | a member of a nested struct |
| `list[3]` | element 3 of a list that exists (counted from 0) |
| `list[*]` | every element of the list, however many this game build has |

A path must end in a number, a bool or an enum. Strings, references to other resources and whole structs cannot be
patched. A list is made longer with `append`, a template's components changed with `addComponents` and
`removeComponents` (below).

## Values

| Value | Result |
|---|---|
| `1500`, `0.5`, `true`, `"EnumName"` | the member is set to it |
| `{ "mul": 2 }` | the game's value times 2 |
| `{ "add": 5 }` | the game's value plus 5 |
| `{ "min": 16 }` | at least 16 |
| `{ "max": 100 }` | at most 100 |
| `{ "raiseOnly": true, … }` | never below the game's value |
| `{ "lowerOnly": true, … }` | never above the game's value |

The keys combine: the result is the game's value times `mul`, plus `add`, then held between `min` and `max`. Whole
numbers are rounded. A bool or an enum member takes a plain value only.

## In C#

```csharp
public override void OnBoot(BootContext boot)
{
    var definitions = BootDefinitions.For(boot);
    definitions.Define("more-altars", new ResourcePatchBlock
    {
        Resource = Guid.Parse("82706b40-61b1-4b8f-8b23-dcec6971bda1"),
        Type = "keen::BalancingTable",
        Set =
        {
            ["altarsPerFlameLevel[*]"] = ResourceValue.AtLeast(16),
            ["experienceNeedPerLevel[0]"] = 1500,
        },
    });
}
```

## Templates and lists

Besides `set`, a patch can change the shape of a resource. These are made first, in this order, and `set` then works
on the result (on one resource, every patch's `removeComponents` come first, then every `addComponents`, then every
`append`, then every `set`, each group in load order):

| Key | On | Does |
|---|---|---|
| `removeComponents` | a template (`keen::ecs::TemplateResource`) | takes out the components of these types (`"SpawnTime"`); one the template hasn't got is skipped. `CurrentTransform`, and what an actor needs to live, are refused. |
| `addComponents` | a template | gives it components of these game types (`"PickupItemZone"`), zeroed; one it has already stays as it is. The list stays sorted by type name, as the game's lists are. |
| `append` | a list of numbers, bools or enum members | adds `values` at its end. `"aboveLast": true` adds only a value greater than the list's last (the last added counts), so a sorted list stays sorted and values the game's list already reaches are left out. The path names one list: `[i]` on the way is fine, `[*]` is not. A value is plain: `{ "mul": 2 }` has no game's value to change and is refused. |

On a template, a `set` or `append` path starts with a component's type name, as template paths do elsewhere
(`"PickupItemZone.lastAttempt.value"`, `"Gravity.isActive"`); that is how the fields of an added component are filled in.
`"Gravity#1.isActive"` is the second `Gravity` of a template that has two. Name a component by its type, not by its place
in the list: a path such as `"components[3].isActive"` is refused on a template whose components a patch adds or removes,
since the place then belongs to another component.

```json
{
  "format": 1,
  "resourcePatches": {
    "ground-pickup": {
      "resource": "9bf5aac3-6ae0-43b1-9f27-18c2f98abfca",
      "type": "keen::ecs::TemplateResource",
      "removeComponents": ["SpawnTime"],
      "addComponents": ["PickupItemZone"],
      "set": { "PickupItemZone.lastAttempt.value": 0, "Gravity.isActive": false }
    },
    "more-steps": {
      "resource": "46e14df6-fb8f-4f66-9463-e23b9e19e48e",
      "type": "keen::FbUiBundle",
      "append": { "difficultySettings.settingValues.enemyDamage.steps": { "values": [3, 5, 10], "aboveLast": true } }
    }
  }
}
```

In C#: `RemoveComponents = ["SpawnTime"]`, `AddComponents = ["PickupItemZone"]`,
`Append = new() { ["…steps"] = new ResourceListAppend { Values = [3, 5, 10], AboveLast = true } }`.

(`9bf5aac3-…` is the template `LootPickup_Gravity_Twigs`.)

How it is done: the changed resource is the copy Kiln serves (the game's own bytes stay as they were). A moved list and
an added component's data go behind the resource's bytes; every offset in the game's data is self-relative, so nothing
else has to move.

### Two mods on one resource

Changes of different mods to one resource are combined when they cannot step on each other. When they can, the mod that
comes later in the load order is refused before the game starts, and the report names both:

- two mods set one field (the same plain value from both is fine);
- one mod adds a component that another removes;
- one mod removes a component that another mod's `set` or `append` path starts at;
- two mods append to one list;
- one mod appends to a list and another sets every element of it with `[*]` (the `[*]` would change the appended values
  too). Another mod's `[i]` on that list is fine: an index names the same element either way. Within one mod, `set`
  works on the list as its own `append` left it;
- a path that names a component by its place (`"components[2]…"`) on a template whose components a patch changes.

Paths are compared as the game reads them: case does not matter, `list[03]` is `list[3]`, and a trailing `.` is dropped.

## What is checked, and when

Before the game starts (a mistake here keeps your mod's boot content out, or stops the start when your mod says
`"boot": "required"`):

- the patch's shape: `resource` is a GUID, at least one change, no unknown member, `append`'s `values` plain and not
  empty, at most 4,096 changes in one start (every field set, list appended to, and component added or removed counts);
- the type exists in this game build, and a path names real members and ends in a scalar (`set`) or in a list of
  numbers, bools or enum members (`append`, whose values must fit the list's elements);
- on a template: a component named in `addComponents`, `removeComponents` or at the start of a path is a component type
  of the game's (`keen::ecs::Component` itself, or a base type that is no component of its own, is refused), the rest
  of the path is checked against that component's members, and `removeComponents` never takes `CurrentTransform`;
- the value fits the member (an operation on a bool or an enum is refused);
- no other mod's patch steps on it ([Two mods on one resource](#two-mods-on-one-resource)).

A path that goes through a variant or an optional is checked as far as the types go; the rest is checked when the write
is made.

When the write is made (the game's data is in memory only then):

- the resource exists in this game build;
- the template has the component a path starts at (one of the game's, or added by a patch), and an index like `[25]`
  is inside the list. `[*]` on an empty list changes nothing and is not an error;
- `removeComponents` does not take what an actor needs to live (`Actor`, `NetworkActor`, its transforms, …) from a
  template that is an actor;
- the template's component list and the list an `append` names can be read, and the patched resource stays below 64 MiB;
- kiln.assets takes the patched copy.

A patch is applied whole or not at all. If any of its changes fails then, none of them is kept: Kiln builds the
resource again from the game's bytes without that patch, so it is what the other patches make it. The failure is
reported once, in `kiln.log` and in `state/boot.json`, with your mod, the patch's key and the reason (the path when one
failed). The start goes on without that patch. If your mod is `"boot": "required"`, the start stops instead.

## Limits

- **Both sides need the mod.** The patch changes the data of the process your mod runs in. A player who joins your
  server without the mod keeps the game's own values on their side, so what their game shows and predicts can differ
  from what the server does. A mod that patches data both sides read should say `"clients": "mod"`.
- **`OnBoot`'s game-data view shows the game's own values**, not patched ones: patches are written after `OnBoot`.
- A patch lasts for the run of the game. It is not written to disk and not stored in a save.
- The game's own hard limits still hold. Example: the game never allows more than 16 Flame Altars, whatever the table
  says.
