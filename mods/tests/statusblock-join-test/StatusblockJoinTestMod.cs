// SPDX-License-Identifier: MIT
using Kiln;

namespace StatusblockJoinTest;

/// <summary>
/// The mod of Roger de R'lyeh's report as a fixture: a buff and a debuff from StatusBlocks (each one buff clone and one
/// generated Impact program) on a server, with "clients": "kiln". A Kiln client joining the server gets both through
/// the content sync: the two definitions and the two programs ("@programs"). Joining must work with the mod on the
/// server, and with only its placeholders left after it was removed (a restart without the mod keeps the buffs' records
/// as placeholders, which keep the server's added-buff check on).
/// <list type="bullet">
/// <item>0.2.0-dev.29 to .22, mod hot-loaded with a player in: the player was refused with "kiln.abilities did not register
/// the programs (4)".</item>
/// <item>0.2.0-dev.29 to .22, any join while the mod or its placeholders are on the server: held at the join and refused
/// after 25 s with "Waiting for this game's content and added-buff capacity check".</item>
/// </list>
/// kiln.log: "statusblock-join: rage &lt;guid&gt;, hunted &lt;guid&gt;" at load.
/// </summary>
public sealed class StatusblockJoinTestMod : Mod
{
    public override void OnLoad()
    {
        var rage = Blocks.Define("essai-rage", new StatusBlock
        {
            Name = "Rage",
            Kind = StatusKind.Buff,
            Seconds = 60,
            Modifies = { [Stat.MeleeDamage] = 0.5 },
        });
        var hunted = Blocks.Define("essai-traque", new StatusBlock
        {
            Name = "Hunted",
            Kind = StatusKind.Debuff,
            Seconds = 60,
            Modifies = { [Stat.MoveSpeed] = -0.4 },
        });
        Log.Info($"statusblock-join: rage {rage.Guid}, hunted {hunted.Guid}");
    }
}
