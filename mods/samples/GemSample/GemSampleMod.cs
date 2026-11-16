// SPDX-License-Identifier: MIT
using Kiln;

namespace GemSample;

/// <summary>
/// Sample: a new gem and a tweak of a game gem, from content/gems.json alone (no gem code). The Gem of Deep Cuts is a
/// kiln.content clone of the Gem of Bleeding with its own GUID, texts, icon, rarity and numbers; the game's Gem of
/// Bleeding bleeds 1.5 times as long. This class only logs the values the world got, once per world load:
/// <c>gemsample: Gem of Deep Cuts 0x...: cut damage 14 (game gem 8), duration 9 s; Gem of Bleeding duration 9 s (game 6 s)</c>.
/// Get the gem with Kiln.Admin or a mod's <c>Items.Give(player, "GemSample_Gem_DeepCuts")</c>; socket it at a Shroud Forge
/// like any gem.
/// </summary>
public sealed class GemSampleMod : Mod
{
    public static readonly Guid DeepCuts = Guid.Parse("580482cb-9771-4d5e-bdc2-24a268f33407");

    public override void OnLoad()
    {
        if (!Content.Available || !Content.GameDataAvailable)
        {
            Log.Warn("kiln.content or kiln.gamedata is not here: the pack does nothing on this game");
            return;
        }
        Content.OnReady(_ =>
        {
            var gem = Content.Items.Get(DeepCuts.ToString());
            var bleeding = Content.Items.Get("T1_Gem_GemOfBleeding");
            if (gem is null || bleeding is null)
            {
                Log.Warn($"gemsample: the gems are not in the registry (Deep Cuts {(gem is null ? "missing" : "ok")}, Bleeding {(bleeding is null ? "missing" : "ok")})");
                return;
            }
            Log.Info($"gemsample: Gem of Deep Cuts 0x{NewContent.IdOf(DeepCuts):x8}: cut damage {gem.Config("0x5e34f09b").Get()} " +
                     $"(game gem {bleeding.Config("0x5e34f09b").Original()}), duration {gem.Config("0x3ff81f2b").Get()}; " +
                     $"Gem of Bleeding duration {bleeding.Config("0x3ff81f2b").Get()} (game {bleeding.Config("0x3ff81f2b").Original()})");
        });
    }
}
