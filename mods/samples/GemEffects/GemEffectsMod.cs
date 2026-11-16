// SPDX-License-Identifier: MIT
using Kiln;

namespace GemEffects;

/// <summary>
/// Sample: new gems whose effect is C# (<see cref="Gems.Register"/> with a <see cref="GemTrigger"/>). Each gem is a clone
/// of a game gem with its own GUID, texts, icon and rarity; its program is one of Kiln's trigger programs, which the game
/// runs like its own gem programs (socket the gem at a Shroud Forge, hold the weapon). The handlers run on the server's
/// tick thread, one step after the hit.
/// <list type="bullet">
/// <item>Gem of Frostbite: a critical hit on an enemy chills it (the game's frost slow) for 2 s + 0.05 s per gem level.</item>
/// <item>Gem of Renewal: a killing blow on an enemy heals the wearer by 5 + half the gem's level.</item>
/// <item><c>/gemeffects [level]</c> (admins): both gems into your backpack, at a level (default 1).</item>
/// </list>
/// kiln.log: <c>gemeffects: Gem of Frostbite on 2543's crit (level 1): 2.05 s frost slow on 6976</c>.
/// </summary>
public sealed class GemEffectsMod : Mod
{
    public static readonly Guid Frostbite = Guid.Parse("74d4a512-0500-4c43-94d4-b742981959c6");
    public static readonly Guid Renewal = Guid.Parse("150524f9-7031-4629-b080-d4320a111c27");

    // The game's frost slow is buff 0x5f5d2f5c. The game hashes a buff reference and reads nothing else
    // (docs/re/abilities.md, "Buff references are hashes"), so these 16 bytes, whose FNV-1a is 0x5f5d2f5c, name it.
    private static readonly Guid FrostSlow = Guid.Parse("6e6c696b-662d-6f72-7374-736c6d0f97c8");

    private GemHandle? _frostbite, _renewal;
    private IDisposable? _give;

    public override void OnLoad()
    {
        if (!Capabilities.Is("gems.triggers"))
        {
            Log.Warn($"new gems are not available here: {Capabilities.Why("gems.triggers")}");
            return;
        }
        try
        {
            RegisterGems();
        }
        catch (KilnUnavailableException e)  // the row does not cover kiln.content's new items or kiln.assets: Register says it
        {
            Log.Warn($"new gems are not available here: {e.Message}");
            return;
        }
        if (Abilities.IdOf(FrostSlow) != 0x5f5d2f5c) Log.Error("gemeffects: the frost slow reference does not hash to 0x5f5d2f5c");
        _give = Commands.Register("gemeffects", c => _ = Give(c), CommandPermission.Admin, usage: "[level]",
            description: "the Gem of Frostbite and the Gem of Renewal into your backpack");
    }

    private void RegisterGems()
    {
        _frostbite = Gems.Register(new GemDefinition
        {
            Id = "gemeffects:frostbite",
            Guid = Frostbite,
            Name = "GemEffects_Gem_Frostbite",
            BaseOn = "T1_Gem_GemOfBleeding",
            Icon = "icons/frostbite.png",
            Rarity = ItemRarity.Rare,
            DisplayName = "Gem of Frostbite",
            Description = "Critical hits chill the target.",
            Texts = { ["de"] = new ItemTexts { Name = "Edelstein des Frostbisses", Description = "Kritische Treffer unterkühlen das Ziel." } },
            Trigger = GemTrigger.OnHit(crit: true),
            OnTrigger = ctx => _ = Chill(ctx),
        });
        _renewal = Gems.Register(new GemDefinition
        {
            Id = "gemeffects:renewal",
            Guid = Renewal,
            Name = "GemEffects_Gem_Renewal",
            BaseOn = "T1_Gem_GemOfSkulls",
            Icon = "icons/renewal.png",
            Rarity = ItemRarity.Uncommon,
            DisplayName = "Gem of Renewal",
            Description = "Killing blows heal you.",
            Texts = { ["de"] = new ItemTexts { Name = "Edelstein der Erneuerung", Description = "Tödliche Treffer heilen dich." } },
            Trigger = GemTrigger.OnKill(),
            OnTrigger = Heal,
        });
        foreach (var h in new[] { _frostbite, _renewal })
            if (!h.Ok) Log.Warn($"gemeffects: {h}");
    }

    public override void OnUnload()
    {
        _give?.Dispose();
        _frostbite?.Dispose();
        _renewal?.Dispose();
    }

    private async Task Chill(GemTriggerContext ctx)
    {
        var seconds = 2 + ctx.Level * 0.05;
        try
        {
            await Abilities.ApplyBuff(ctx.Target, FrostSlow, TimeSpan.FromSeconds(seconds), ctx.Player);
            Log.Info($"gemeffects: Gem of Frostbite on {ctx.Player.Value}'s crit (level {ctx.Level}): {seconds:0.##} s frost slow on {ctx.Target.Value}");
        }
        catch (Exception e) when (e is ArgumentException or KilnUnavailableException or InvalidOperationException)
        {
            Log.Warn($"gemeffects: no frost slow on {ctx.Target.Value}: {e.Message}");
        }
    }

    private void Heal(GemTriggerContext ctx)
    {
        var amount = 5 + ctx.Level / 2;
        var ok = Abilities.Attributes.Add(ctx.Player, AbilityResource.Health, amount);
        Log.Info($"gemeffects: Gem of Renewal on {ctx.Player.Value}'s kill of {ctx.Target.Value} (level {ctx.Level}): +{amount} health{(ok ? "" : " (not applied)")}");
    }

    private async Task Give(CommandContext c)
    {
        var who = c.Source.Player;
        if (who.IsNone)
        {
            c.Reply("run it as a player");
            return;
        }
        if (!Capabilities.Is("kiln.items"))
        {
            c.Reply($"cannot give items here: {Capabilities.Why("kiln.items")}");
            return;
        }
        var level = c.Count > 0 && int.TryParse(c.Args[0], out var l) ? Math.Clamp(l, 1, 105) : 1;
        foreach (var gem in new[] { Frostbite, Renewal })
        {
            var r = await Items.Give(who, new ItemId(NewContent.IdOf(gem)), 1, new Gear(Level: level));
            c.Reply(r.Done > 0 ? $"gave {gem} at level {level}" : $"not given: {r.Status} {r.Message}");
        }
    }
}
