# New gems

<!-- gem wave G2: the "New gems" section of docs/kiln/services/gems.md (work-00 merges it there). -->

A mod can add gems of its own. A new gem is an item: a copy of one of the game's gems with its own GUID, name, texts,
icon, rarity and level range. Its effect is one of three things:

1. **the base gem's program with the gem's own numbers** (data only);
2. **another Impact program** that the game or a mod already loaded (another gem's, a perk's, or one a mod cooked with
   `tools/impact`), with the gem's own numbers;
3. **a C# handler**: Kiln gives the gem one of its trigger programs, and the program calls your code when the trigger
   happens (a hit, a critical hit, a kill, a backstab, a hit taken, or every few seconds).

The forge treats a new gem like a game gem: it lists it in its GEMS grid, sockets it, upgrades it and recycles it. The
game runs its program when the weapon that holds it is in hand, exactly as for its own gems.

Samples: `mods/samples/GemSample` (data only) and `mods/samples/GemEffects` (C# effects).

## A new gem from a data pack

A gem is a kiln.content clone (content.md, "New items and recipes") whose base is a game gem. The `items` section's
`"add"` list makes it; rules after the list set its numbers (kiln.gamedata's item paths, gamedata.md "Paths"):

```json
{ "format": 1,
  "items": {
    "add": [ { "guid": "580482cb-9771-4d5e-bdc2-24a268f33407", "name": "GemSample_Gem_DeepCuts",
               "base": "T1_Gem_GemOfBleeding", "program": "b199646f-31a1-495e-b3d1-d5fef2fcaab3",
               "icon": "icons/deep-cut.png", "displayName": "Gem of Deep Cuts",
               "description": "Critical hits with cutting damage open deep wounds that bleed for a long time.",
               "texts": { "de": { "name": "Edelstein der tiefen Schnitte" } },
               "fields": { "rarity": "Rare", "itemLevelRange.minLevel": 1, "itemLevelRange.maxLevel": 105 } } ],
    "GemSample_Gem_DeepCuts": { "impactValues.0x5e34f09b": 14, "impactValues.0x3ff81f2b": 9 } } }
```

- `guid`: fixed forever. The item id is FNV-1a of it, and saves keep the id.
- `base`: the game gem to copy. Its model, icon, category (so the forge lists it) and program come with it.
- `program`: optional. Any loaded Impact program's GUID: another gem's (the config ids per gem are in this guide's table
  of the game's gems), a perk's, or one your mod serves through kiln.assets. Without it the gem keeps its base's program.
- `fields.itemLevelRange`: **set both**. A new item gets one level unless the definition says otherwise (content.md,
  "Rarity, level and perks of new gear"), and a gem with one level cannot be upgraded. The game's gems go from 1 to 105.
- The rule after the list sets the configs the program reads: `impactValues.<alias>` or `impactValues.0x<config id>`,
  with `.scaleFactor` for how much a scaled config grows per gem level.

**Tooltips.** The gem's description is your text. The tooltip's number rows (`uiValues`) are the base's and follow the
configs they show (gamedata.md, "What follows a change").

## A new gem from C#: `Gems.Register`

```csharp
using Kiln;

public override void OnLoad()          // on every side that has the mod: a client needs the item to show it
{
    if (!Capabilities.Is("gems.triggers")) return;
    var gem = Gems.Register(new GemDefinition
    {
        Id = "gemeffects:frostbite",               // "<modid>:<name>"
        Guid = Guid.Parse("74d4a512-0500-4c43-94d4-b742981959c6"),   // optional: made from Id when left out
        BaseOn = "T1_Gem_GemOfBleeding",
        Icon = "icons/frostbite.png", Rarity = ItemRarity.Rare,
        DisplayName = "Gem of Frostbite", Description = "Critical hits chill the target.",
        Texts = { ["de"] = new ItemTexts { Name = "Edelstein des Frostbisses" } },
        Trigger = GemTrigger.OnHit(crit: true),    // OnHit, OnKill, OnBackstab, OnHitTaken, Passive(TimeSpan)
        OnTrigger = ctx =>                         // the server's tick thread, one step after the hit
            _ = Abilities.ApplyBuff(ctx.Target, FrostSlow, TimeSpan.FromSeconds(2 + ctx.Level * 0.05), ctx.Player),
    });
    if (!gem.Ok) Log.Warn(gem.Reason!);
}
```

`GemDefinition`:

| Field | Meaning |
|---|---|
| `Id` | `"<modid>:<name>"`, unique |
| `Guid` | the item's GUID, fixed forever. Null: one made from `Id` (the same on every start and every side) |
| `Name` | the item's debug name. Null: `Gem_<modid>_<name>` |
| `BaseOn` | the gem to copy: a debug name (`T1_Gem_GemOfBleeding`), a `0x` id or a GUID |
| `DisplayName`, `Description`, `Texts` | the texts, as kiln.content's item texts |
| `Icon` | a PNG of the mod, `item:<name>` or an item GUID. Null: the base's |
| `Rarity` | null: the base's |
| `LevelMin`, `LevelMax` | 1..105. Null: 1 and 105, as the game's gems. A new gem is made at `LevelMin`; `LevelMax` is the highest level an upgrade reaches (each forge also has its own cap) |
| `Program` | another loaded Impact program, with `Configs`. Not with `Trigger` |
| `Configs` | the gem's config values: key = alias, `"0x5e34f09b"`, a program's own string id (`"mymod.Radius"`), optionally `+ ".scaleFactor"`; value = number, bool, string (enum, template or buff name), `Guid`, `TimeSpan`. Applied at every world load as your mod's layer. Not with `Trigger` |
| `Trigger` + `OnTrigger` | the C# effect |

Without `Program` and `Trigger` the gem runs its base's program, with `Configs` if given (the data gem of the pack above,
from code).

`Gems.Register` returns a `GemHandle`: `Ok`, `Reason` (refused when the id, GUID or debug name is taken: "gem
gemeffects:frostbite is registered already (by gemeffects)"), `Item`, `Guid`, `Name`, `Dispose()`. A wrong definition
throws `ArgumentException` (a bad id, levels outside 1..105, a program and a trigger at once, a trigger without a
handler, configs with a trigger); a call outside mod code throws `InvalidOperationException`; with kiln.content's new
items or (for a trigger) kiln.assets missing it throws `KilnUnavailableException` naming `gems.triggers`. Tick thread.

**Unload.** When the mod unloads (or the handle is disposed) its handlers stop. The items stay registered until the
server restarts, as every kiln.content item does, so gems in sockets and bags stay valid; a gem whose handler is gone
does nothing. After a restart without the mod the gems become kiln.content's `[Missing: ...]` placeholders.

### Triggers

| Trigger | The handler runs on | `ctx.Target` |
|---|---|---|
| `GemTrigger.OnHit()` | each hit the wearer lands | the one hit |
| `GemTrigger.OnHit(crit: true)` | each critical hit the wearer lands | the one hit |
| `GemTrigger.OnKill()` | each killing blow | the one killed |
| `GemTrigger.OnBackstab()` | each backstab | the one hit |
| `GemTrigger.OnHitTaken()` | each hit the wearer takes | the attacker |
| `GemTrigger.Passive(interval)` | every `interval` (250 ms to 1 hour) while the gem is worn | none |

`enemiesOnly` (default true) keeps only hits whose other side is an enemy faction, the mask the game's own gems use
(creatures; not players, props or buildings). Hits made by a skill's or a gem's own damage do not count, as in the game's
gems, so one gem does not set off another.

### The context (`GemTriggerContext`)

`Player` (the wearer), `Target`, `TargetPart` (the entity the hit struck), `Gem` (the gem item), `Level` (the gem's
level: scale your effect with it), `Carrier` (the socketed item), `Slot` (its equipment slot), `Impact` (the gem's
running program), `Damage` (health the hit took; `HealthChange` is the raw value, negative for damage), `Position`,
`Weapon`, `Source`, `Crit`, `KillingBlow`, `Backstab`, `Blocked`, `OtherIsEnemy`, `Trigger`, `Definition`, `Step`.

The handler runs on the server's tick thread with your mod as the caller, so every tick-thread API works
(`Abilities.ApplyBuff`, `Abilities.Attributes.Add`, `Combat`, `Spawn`, ...). An exception is logged in your mod's log
(three times) and the next trigger runs again. On a client the handler is never called.

### How it works

- Kiln ships one trigger program per kind (`managed/services/Kiln.Abilities/Gems/programs/*.kimp`, assembled with
  `tools/impact`; GUIDs `6b696c6e-6765-6d73-8000-00000000000N`). `Gems.Register` serves the program of its trigger
  through kiln.assets under your mod and points the new gem's `equipment.program` at it.
- The game runs it like its own gem programs: `equipment_impacts` spawns it for the socketed gem of an item in hand (or in
  any equipment slot whose item has a socket), with the gem's level. Like the game's gem programs it walks each step's
  hits and keeps its wearer's. For each one it sends the hit with `kiln.Signal` (48 bytes: attacker, target, part,
  health change, position, weapon, source, crit, kill, backstab, blocked, enemy).
- Kiln.Abilities receives the signal in the next Pre, finds the gem through the wearer's equipped gems
  (`Gems.Equipped`: the gem item, its level, its carrier, matched by the program's entity), and calls the gem's handler.
- The program keeps an `impact.DamageSetup` as its first such variable: the game writes the gem's item power into a
  program's first DamageSetup (docs/re/abilities.md, "Facts for programs Kiln generates"), and this one takes it.

### Limits

- **One step late.** The handler runs in the Pre after the hit (about 40 ms on a dedicated server). It can react to a
  hit, not change it: to change a hit's damage use `Abilities.OnDamage` or `Abilities.Natives.Attach`.
- **Damage you deal can set the gem off again.** Damage from the handler (`Combat.Damage`, an impact rooted at the
  player) is a hit of the wearer. Skip it with `ctx.Weapon` / `ctx.Source`, or keep a cooldown.
- A passive gem's interval rounds to the program's 250 ms steps.
- Several gems on one player each run their own handler for the same hit (as duplicates of a game gem do).
- Clients: install the mod on the players' games (`side: both`, `clients: mod`) so they show the gem (name, icon,
  tooltip). A vanilla client shows "[Invalid Item Stack]" for a new gem; the effects are the server's and work for every
  player. With `clients: kiln` the server sends the definitions and the trigger program's bytes (no PNG icon: the base's).

## Capability

`gems.triggers` (managed, static, with G1's `GemTriggeredEvent`): what `Gems.Register` needs: kiln.abilities, kiln.content
with new items (`content.new`, table v3) and kiln.assets (for a trigger program). A data gem from a pack needs only
kiln.content. The handlers also need `Gems.Equipped` (`gems.read`) on the server.

## Tests

- `managed/tests/Kiln.Blocks.Tests/GemRegistryTests.cs`: registration (the clone, the program served once per mod and
  trigger, refusals, the definition errors, the capability), dispatch of the programs' signals (context, filters,
  passive intervals, a throwing handler, unload) on a fake game side.
- `tools/impact/tests/test_gem_programs.py`: each program assembles to the embedded `.bin`, lints, keeps the DamageSetup
  placeholder first, and lays out its payload as the C# side reads it.
- Lab (`mods/tests/gems-custom-test`, Wine dedicated server 26430253): the programs registered from kiln.assets, the gems
  defined with them, a data gem's configs, each program's signals on a player-template entity's hits through the game's
  applyDamage, and (with `Gems.Equipped`) a new gem written into a held dagger's socket whose C# handler runs on a kill:
  `PASS the socketed gem's C# handler ran on the kill: 1 call(s); player 2022, target 2061, level 7, carrier ..., slot Held,
  damage 5000` (39 PASS, 0 FAIL).

## Client checklist

1. GemSample and GemEffects on the server and the client. The Gem of Deep Cuts, Gem of Frostbite and Gem of Renewal show
   their names, PNG icons, rarities and descriptions (German texts with the game in German).
2. `/gemeffects 10` gives both gems at level 10. A Shroud Forge lists them in its GEMS grid; socket the Gem of Frostbite
   into a weapon. With it in hand, a critical hit on an enemy puts the frost slow (snail icon) on it; kiln.log:
   `gemeffects: Gem of Frostbite on <player>'s crit (level 10): 2.5 s frost slow on <enemy>`.
3. The Gem of Renewal in a weapon: a kill heals the player by 10 at level 10.
4. Upgrade a new gem at a forge: the level rises (its own cap is 105, the forge's cap applies).
5. The Gem of Deep Cuts socketed: a critical cutting hit bleeds for 9 s (the game's Gem of Bleeding, with GemSample's
   tweak: 9 s instead of 6 at level 1).
