// SPDX-License-Identifier: MIT
using Kiln;

namespace ModValuesSample;

/// <summary>
/// "Fishing": a mod's own per-character values (Kiln.ModValues). The values are defined in mod-values/fishing.json
/// (Kiln reads it: both sides know the names and formats, the server activates them). The server owns them:
///  - /fishing-catch: +25 fishing XP and +3 tokens (the stand-in for a real catch);
///  - /fishing-buy-bait: spends 5 tokens, then gives the bait item; if the item cannot be given, the tokens go back
///    (a spend is not a transaction with the inventory, so the refund is explicit);
///  - /fishing-stats: the values as the server has them.
/// A player's Kiln client gets a read-only copy and shows it in a HUD line, a window (F9, with a "Buy bait" button that
/// asks the server through this mod's own channel: the same purchase as the command) and a tooltip line on the bait.
/// </summary>
public sealed class FishingMod : Mod
{
    private const string BaitItem = "Plant Fiber";  // any item name of the game; an unknown name shows the refund path
    private const long BaitPrice = 5;
    private NetChannel? _channel;
    private UiWindow? _window;

    private static ModValueKey Xp => ModValues.Key("xp");
    private static ModValueKey Tokens => ModValues.Key("tokens");
    private static ModValueKey Mastery => ModValues.Key("mastery");  // carried: travels with the character to other worlds

    public sealed record BuyBait;

    public override void OnLoad()
    {
        _channel = Network.Channel("fishing:shop", "1", required: false)
            .RegisterJson<BuyBait>((request, ctx) => { _ = Buy(ctx.Player, r => ctx.Channel.SendTo(ctx.Sender, r)); }, NetDirection.ToServer)
            .RegisterJson<string>((text, _) => Hud.Toast(text), NetDirection.ToClient);
        if (Ui.Available) ClientUi();
    }

    // ------------------------------------------------------------------------------------------------ server ----

    [Command("fishing-catch", Description = "a catch: +25 fishing XP, +3 fishing tokens")]
    private void Catch(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("only a player fishes");
        var who = CharacterData.KeyOf(c.Source.Player);
        var xp = ModValues.Add(who, Xp, 25);
        var tokens = ModValues.Add(who, Tokens, 3);
        ModValues.Add(who, Mastery, 25);
        if (!xp.Ok || !tokens.Ok) throw new CommandException($"not counted: {xp.Reason ?? tokens.Reason}");
        var levelUp = xp.After!.Level > xp.Before!.Level ? $" Level {xp.After.Level}!" : "";
        c.Reply($"{ModValues.Format(xp.After)} fishing XP, {ModValues.Format(tokens.After!)} tokens.{levelUp}");
    }

    [Command("fishing-buy-bait", Description = "spend 5 fishing tokens on bait")]
    private Task BuyCommand(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("only a player buys bait");
        return Buy(c.Source.Player, c.Reply);
    }

    [Command("fishing-big", Description = "admin, for the client check: fishing XP to 9007199254740993 (above 2^53)", Permission = CommandPermission.Admin)]
    private void Big(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("only a player has fishing values");
        var set = ModValues.Set(CharacterData.KeyOf(c.Source.Player), Xp, 9007199254740993);
        c.Reply(set.Ok ? $"fishing XP {ModValues.Format(set.After!)}" : $"not set: {set.Reason}");
    }

    [Command("fishing-stats", Description = "your fishing XP, level and tokens")]
    private void Stats(CommandContext c)
    {
        if (!c.Source.IsPlayer) throw new CommandException("only a player has fishing values");
        var who = CharacterData.KeyOf(c.Source.Player);
        var xp = ModValues.Read(who, Xp);
        var tokens = ModValues.Read(who, Tokens);
        var mastery = ModValues.Read(who, Mastery);
        c.Reply(xp.Ok && tokens.Ok
            ? $"Fishing level {xp.Value!.Level}: {ModValues.Format(xp.Value)} XP, {ModValues.Format(tokens.Value!)} tokens" +
              (mastery.Ok ? $", mastery {ModValues.Format(mastery.Value!)} (travels with you)" : "")
            : $"not available: {xp.Reason ?? tokens.Reason}");
    }

    /// <summary>The purchase: the character comes from the player, never from the request. Spend first, then the
    /// item; the explicit refund keeps the two consistent when the item cannot be given.</summary>
    private async Task Buy(EntityId player, Action<string> reply)
    {
        var who = CharacterData.KeyOf(player);
        if (who.IsNone)
        {
            reply("no character");
            return;
        }
        var paid = ModValues.Spend(who, Tokens, BaitPrice);
        if (!paid.Ok)
        {
            reply(paid.Reason == ModValueReasons.InsufficientFunds ? $"bait costs {BaitPrice} tokens" : $"not now: {paid.Reason}");
            return;
        }
        var given = await Items.Give(player, BaitItem, 1, options: GiveOptions.Notify);
        if (given.Status != ItemsStatus.Done)
        {
            var refund = ModValues.Add(who, Tokens, BaitPrice);  // compensate: the spend is not undone by itself
            reply($"no bait ({given.Status}); {(refund.Ok ? "your tokens are back" : $"refund failed: {refund.Reason}")}");
            Log.Warn($"fishing: bait for {who} not given ({given.Status}), refund {(refund.Ok ? "done" : refund.Reason)}");
            return;
        }
        reply($"bait bought; {ModValues.Format(paid.After!)} tokens left");
    }

    // ------------------------------------------------------------------------------------------------ client ----

    private void ClientUi()
    {
        Hud.Show("fishing.values", HudText, HudAnchor.TopRight, Color.Cyan);
        _window = Ui.Window("Fishing", Draw, new UiWindowOptions { StartOpen = false, Width = 320, Height = 180 });
        KeyBindings.Register("fishing.window", "F9", () => _window.Toggle(), "Fishing values");
        if (Capabilities.Is(Ui.Values.TooltipCapability) && Items.Available && Items.Lookup(BaitItem) is { } bait)
            Ui.Tooltip.AddLines(bait.Id, (in UiValueContext _, in UiItemView _) =>
                UiTooltipLines.With(new TooltipLine(UiText.Of(("en", $"Costs {BaitPrice} fishing tokens"), ("de", $"Kostet {BaitPrice} Angelmarken")), TooltipColor.Default, Small: true)));
    }

    private static string HudText()
    {
        var xp = ModValues.Synced(Xp);
        var tokens = ModValues.Synced(Tokens);
        if (!xp.Ok || !tokens.Ok) return "";  // not on a Kiln server with this mod, or not in game: nothing shown
        return $"Fishing Lv {xp.Value!.Level} | {ModValues.Format(xp.Value)} XP | {ModValues.Format(tokens.Value!)} tokens";
    }

    private void Draw(UiDraw ui)
    {
        var xp = ModValues.Synced(Xp);
        var tokens = ModValues.Synced(Tokens);
        if (!xp.Ok || !tokens.Ok)
        {
            ui.TextDisabled($"No fishing values here ({xp.Reason ?? tokens.Reason}).");
            return;
        }
        ui.Text($"Level {xp.Value!.Level}");
        ui.Text($"{ModValues.Format(xp.Value)} XP");
        ui.Text($"{ModValues.Format(tokens.Value!)} tokens");
        ui.TextDisabled($"revision {tokens.Value!.ValueRevision}");
        if (ui.Button($"Buy bait ({BaitPrice} tokens)")) _channel?.SendToServer(new BuyBait());
    }
}
