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
- A patch needs at least one change: `set`, or `clear`, `replace`, `append`, `addComponents`, `removeComponents`,
  `copyComponents` (below). A member the patch does not know (`"sett"`) is refused.
- The file is read as every blocks file is: comments (`//`, `/* */`) and trailing commas are fine.

## Paths

| Form | Means |
|---|---|
| `playerLevelCap` | a member |
| `a.b.c` | a member of a nested struct |
| `list[3]` | element 3 of a list that exists (counted from 0) |
| `list[*]` | every element of the list, however many this game build has |

A path must end in a number, a bool or an enum. Strings, references to other resources and whole structs cannot be
set. A list is emptied with `clear`, given new values with `replace` and made longer with `append`; a template's
components are changed with `addComponents`, `removeComponents` and `copyComponents` (below).

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
on the result: `removeComponents`, `addComponents`, `copyComponents`, `clear`, `replace`, `append`, `set`. On one
resource every patch's `removeComponents` come first, then every patch's `addComponents`, and so on, each group in load
order. The order in which a patch writes its keys does not matter.

| Key | On | Does |
|---|---|---|
| `removeComponents` | a template (`keen::ecs::TemplateResource`) | takes out the components of these types (`"SpawnTime"`); one the template hasn't got is skipped. `CurrentTransform`, and what an actor needs to live, are refused. |
| `addComponents` | a template | gives it components of these game types (`"PickupItemZone"`), zeroed; one it has already stays as it is. The list stays sorted by type name, as the game's lists are. |
| `copyComponents` | a template | `{ "<component type>": "<template GUID>" }`: copies the whole component, with all its data (references, nested structs, lists, strings, variants), from that other template. Added when the template has none of that type, its own (the first of that type) replaced when it has. The source must have the component. `CurrentTransform` is refused, and on a template that is an actor so is what an actor needs to live. The source is read as the game has it, before any resource patch of this start: another patch on the source template does not carry over. The same patch's own `set`, `append`, `clear` and `replace` paths on the copied component then apply to the copy; from another mod only a plain value `set` does (anything that counts on what the component held is refused, see [Two mods on one resource](#two-mods-on-one-resource)). |
| `clear` | lists of anything | `["<list path>", …]`: each list made empty, whatever its elements are (structs, references, numbers). |
| `replace` | a list of numbers, bools or enum members | `{ "<list path>": { "values": [ … ] } }`: the list holds exactly these values afterwards, as many as given (fewer or more than before). At least one value; to empty a list use `clear`. A value is plain. |
| `append` | a list of numbers, bools or enum members | adds `values` at its end. `"aboveLast": true` adds only a value greater than the list's last (the last added counts), so a sorted list stays sorted and values the game's list already reaches are left out. A value is plain: `{ "mul": 2 }` has no game's value to change and is refused. |

A list path of `clear`, `replace` or `append` names one list: `[i]` on the way is fine, `[*]` is not.

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

Copying a component, clearing a list, replacing a list's values (the mod API 0.2.9 and later):

```json
{
  "format": 1,
  "resourcePatches": {
    "area-deposit-chest": {
      "resource": "65baa890-022a-454b-9cb7-b92cb8c06cd4",
      "type": "keen::ecs::TemplateResource",
      "copyComponents": { "InventoryCraftingStock": "955ba32f-08ea-414d-bd98-ac75990aef04" }
    },
    "build-anywhere": {
      "resource": "509feadb-4c60-425f-9c7c-deeefd9b6920",
      "type": "keen::SceneResource",
      "clear": ["noBuildZones", "resetPlayersOutOfBounds.playableAreas"]
    },
    "damage-steps": {
      "resource": "46e14df6-fb8f-4f66-9463-e23b9e19e48e",
      "type": "keen::FbUiBundle",
      "replace": { "difficultySettings.settingValues.enemyDamage.steps": { "values": [0.25, 0.5, 1, 2, 4] } }
    }
  }
}
```

(`65baa890-…` is `Prop_Decoration_T1_Storage_24`, `955ba32f-…` the Magic chest `Prop_Decoration_T1_Storage_24_Magic`;
`509feadb-…` is the world's `keen::SceneResource`, on the client and on a dedicated server, with 8 no-build zones and 14
playable areas in November 2026; `names:resources/keen::SceneResource` lists a build's scenes, see
[Reading the game's data](onboot.md#reading-the-games-data). `46e14df6-…` is the client's `keen::FbUiBundle`, which a
dedicated server does not have: a patch of it belongs in a mod that runs on the client.)

```csharp
var d = BootDefinitions.For(boot);
d.Define("area-deposit-chest", new ResourcePatchBlock
{
    Resource = Guid.Parse("65baa890-022a-454b-9cb7-b92cb8c06cd4"), Type = "keen::ecs::TemplateResource",
    CopyComponents = new() { ["InventoryCraftingStock"] = Guid.Parse("955ba32f-08ea-414d-bd98-ac75990aef04") },
});
d.Define("build-anywhere", new ResourcePatchBlock
{
    Resource = Guid.Parse("509feadb-4c60-425f-9c7c-deeefd9b6920"), Type = "keen::SceneResource",
    Clear = ["noBuildZones", "resetPlayersOutOfBounds.playableAreas"],
});
d.Define("damage-steps", new ResourcePatchBlock
{
    Resource = Guid.Parse("46e14df6-fb8f-4f66-9463-e23b9e19e48e"), Type = "keen::FbUiBundle",
    Replace = new() { ["difficultySettings.settingValues.enemyDamage.steps"] = new ResourceListReplace { Values = [0.25, 0.5, 1, 2, 4] } },
});
```

How it is done: the changed resource is the copy Kiln serves (the game's own bytes stay as they were). A moved list and
an added component's data go behind the resource's bytes; every offset in the game's data is self-relative, so nothing
else has to move. A copied component's data and everything it points at (its lists, strings, optionals, variants, and
theirs) are laid out behind the bytes the same way, each offset made relative to its new place; the component's layout
comes from the game's reflection (kiln.types). A cleared list keeps nothing; a replaced list gets a new place behind the
bytes, as an appended one does.

### Two mods on one resource

Changes of different mods to one resource are combined when they cannot step on each other. When they can, the mod that
comes later in the load order is refused before the game starts, and the report names both:

- two mods set one field (the same plain value from both is fine);
- one mod adds a component that another removes;
- one mod removes a component that another mod's `set` or `append` path starts at;
- two mods append to one list;
- one mod appends to a list and another sets every element of it with `[*]` (the `[*]` would change the appended values
  too). Another mod's `[i]` of an element the list already had is fine: an index names the same element either way. An
  `[i]` of an element another mod appended is not (it would write over that mod's value): its patch is refused when the
  resource is written, in either load order, since every append is made before every set. Within one mod, `set` works
  on the list as its own `append` left it;
- one mod copies a component that another removes, or two mods copy one component from different templates (the same
  copy from both is fine). One mod's `addComponents` and another's copy of that component are fine: the copy is made
  after the add, so the copied data is what the template gets;
- one mod copies a component and another has a path into it that counts on what the component holds: an index on the
  way (`"Stock.items[5].x"`), a change of the game's value (`{ "mul": 2 }` would change the copied value instead), or a
  `clear`, `replace` or `append` of a list in it. Either order: the later mod is refused. Another mod's plain value set
  on a copied component (`"InventoryCraftingStock.canReceiveAreaDeposit": true`) is fine: it is written after the copy.
  Within one mod, every path into its own copy is fine;
- two mods clear or replace one list, unless both make the same change (two `clear`s, or two `replace`s with the same
  values, are combined; `1` and `1.0` are the same value);
- one mod clears or replaces a list and another appends to it, sets one or all of its elements (`list[2]`, `list[*]`),
  or names a path through it (`list[2].x`, a list inside its elements): the elements the other mod counts on are gone or
  others. Within one mod this is fine: its `clear` or `replace`, then its `append`, then its `set` are made in that order;
- a path that names a component by its place (`"components[2]…"`) on a template whose components a patch changes.

Paths are compared as the game reads them: case does not matter, `list[03]` is `list[3]`, and a trailing `.` is dropped.

## What is checked, and when

Before the game starts (a mistake here keeps your mod's boot content out, or stops the start when your mod says
`"boot": "required"`):

- the patch's shape: `resource` is a GUID, at least one change, no unknown member, the values of `append` and `replace`
  plain and not empty, `copyComponents` naming another template by its GUID, at most 4,096 changes in one start (every
  field set, list cleared, replaced or appended to, and component added, removed or copied counts one);
- the type exists in this game build, and a path names real members and ends in a scalar (`set`), in a list of
  numbers, bools or enum members (`append`, `replace`, whose values must fit the list's elements) or in a list of
  anything (`clear`);
- on a template: a component named in `addComponents`, `removeComponents`, `copyComponents` or at the start of a path is
  a component type of the game's (`keen::ecs::Component` itself, or a base type that is no component of its own, is
  refused), the rest of the path is checked against that component's members, and neither `removeComponents` nor
  `copyComponents` takes `CurrentTransform`;
- the value fits the member (an operation on a bool or an enum is refused);
- no other mod's patch steps on it ([Two mods on one resource](#two-mods-on-one-resource)).

A path that goes through a variant or an optional is checked as far as the types go; the rest is checked when the write
is made. When Kiln has a view of this start's game data that is newer than the boot snapshot (the snapshot itself is never
used for this), what needs the data (the list below) is checked against that view before the start as well, as far as
the view shows it, and only for a resource nothing else in the same start creates or overrides (Kiln, a boot mod, a data
pack or another mod's `assets.json` can); for any other resource the check is left to the write.

When the write is made (the game's data is in memory only then):

- the resource exists in this game build;
- the template has the component a path starts at (one of the game's, or added by a patch), and an index like `[25]`
  is inside the list, and not an element another mod appended. `[*]` on an empty list changes nothing and is not an
  error;
- `removeComponents` does not take what an actor needs to live (`Actor`, `NetworkActor`, its transforms, …) from a
  template that is an actor, and `copyComponents` does not copy one of those over onto it;
- the template `copyComponents` copies from exists and has the component, and everything of the component can be
  copied within the 64 MiB the patched resource may reach (every copied component of the patch counted together):
  every member's type is in the game's reflection (a variant of a type it does not name, or one of the
  editor's own `Ds…` kinds, is refused rather than guessed), and every list, string and variant lies inside the source's
  bytes;
- the template's component list and the list a `clear`, `replace` or `append` names can be read, and the patched
  resource stays below 64 MiB;
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

## Example: three changes of a data mod, in Kiln

A data mod that edits the game's archive often does three kinds of thing at once. Here is how each looks as a resource
patch (the EMBER mod's area deposit, Build Anywhere / No Boundary, and longer difficulty sliders; mod API 0.2.9). The
mod says `"boot": true` and `"side": "both"` in its kiln.mod.json, so its patches are made in the server's process and
in each player's game.

```csharp
public override void OnBoot(BootContext boot)
{
    var d = BootDefinitions.For(boot);

    // 1. Every storage chest and crafting station takes area deposits: the Magic chest's InventoryCraftingStock,
    //    copied whole onto each template with Furniture or FactoryStation that has an inventory and has none of its own.
    var magic = Guid.Parse("955ba32f-08ea-414d-bd98-ac75990aef04");  // Prop_Decoration_T1_Storage_24_Magic
    var guidOf = boot.GameData.Read("names:resources/keen::ecs::TemplateResource").Data!.Value
        .GetProperty("resources").EnumerateArray()
        .GroupBy(r => r.GetProperty("name").GetString()!)
        .ToDictionary(g => g.Key, g => Guid.Parse(g.First().GetProperty("resourceId").GetString()!));
    var stocked = Names(boot, "names:templates/InventoryCraftingStock");
    var storage = Names(boot, "names:templates/InventorySetup");
    var i = 0;
    foreach (var name in Names(boot, "names:templates/Furniture").Union(Names(boot, "names:templates/FactoryStation")))
        if (storage.Contains(name) && !stocked.Contains(name) && guidOf.TryGetValue(name, out var template))
            d.Define($"deposit-{i++}", new ResourcePatchBlock
            {
                Resource = template, Type = "keen::ecs::TemplateResource",
                CopyComponents = new() { ["InventoryCraftingStock"] = magic },
            });

    // 2. Build anywhere, no world boundary: every scene's no-build zones and playable areas cleared.
    var s = 0;
    foreach (var scene in boot.GameData.Read("names:resources/keen::SceneResource").Data!.Value.GetProperty("resources").EnumerateArray())
        d.Define($"scene-{s++}", new ResourcePatchBlock
        {
            Resource = Guid.Parse(scene.GetProperty("resourceId").GetString()!), Type = "keen::SceneResource",
            Clear = ["noBuildZones", "resetPlayersOutOfBounds.playableAreas"],
        });

    // 3. The difficulty slider's steps: 32 values instead of the game's few. The FbUiBundle is the game client's
    //    (a dedicated server has none), so only where there is one.
    if (boot.GameData.Read("names:resources/keen::FbUiBundle").Ok)
        d.Define("damage-steps", new ResourcePatchBlock
        {
            Resource = Guid.Parse("46e14df6-fb8f-4f66-9463-e23b9e19e48e"), Type = "keen::FbUiBundle",
            Replace = new()
            {
                ["difficultySettings.settingValues.enemyDamage.steps"] = new ResourceListReplace
                {
                    Values = Enumerable.Range(1, 32).Select(x => (ResourceValue)(x * 0.25)).ToList(),
                },
            },
        });
}

static HashSet<string> Names(BootContext boot, string key)
{
    var r = boot.GameData.Read(key);
    return r.Ok ? r.Data!.Value.GetProperty("names").EnumerateArray().Select(n => n.GetString()!).ToHashSet() : [];
}
```

- `names:resources/<type>` lists every resource of a type with its GUID and name; `names:templates/<component>` the
  templates that have that component (see [Reading the game's data](onboot.md#reading-the-games-data)). Both need the
  boot snapshot (`mods.boot.game_data`); a mod that can do without checks the row first.
- The same patches can be written in a blocks file (`copyComponents`, `clear`, `replace`), one per resource, when the
  list of resources is fixed.
- Every patch counts against the 4,096 changes of a start; a hundred chests are a hundred changes.
- Other settings sliders (`enemyHealth`, `playerHealth`, …) are siblings of `enemyDamage` under
  `difficultySettings.settingValues`; each is its own `replace` path.
