// SPDX-License-Identifier: MIT
// Kiln's optional, self-only glider command. No Creative preset, free-character flight or general stamina rules.
namespace Kiln;

internal static class UnlimitedFlight
{
    private const string Name = "unlimitedflight";
    private const string PermissionNode = "kiln.command.unlimitedflight";
    private static readonly PlayerRules Requested = new() { UnlimitedGliding = true, GlideSteer = GlideSteerRule.Default };
    private static readonly Dictionary<EntityId, RuleSet> Sets = new();
    private static int _registered;

    internal static void RegisterCommand()
    {
        if (Interlocked.Exchange(ref _registered, 1) != 0) return;
        Commands.Register(Commands.Literal(Name).Requires(CommandPermission.Admin, PermissionNode)
            .Describe("Unlimited Flight: your glider costs no stamina; S pitches up, W dives (independent of Creative)")
            .Then(Commands.Literal("on").Executes(c => Run(c, true)))
            .Then(Commands.Literal("off").Executes(c => Run(c, false)))
            .Then(Commands.Literal("status").Executes(c => Run(c, null))), CommandPermission.Admin);
    }

    private static bool IsOn(EntityId player) => Sets.TryGetValue(player, out var set) && set.Active;

    private static void Run(CommandContext c, bool? on)
    {
        Game.AssertTickThread();
        // A joined game's console has no server authority; its local rules.json is not the server's policy.
        if (Client.IsRemote) c.Fail("Unlimited Flight belongs to the server you joined. Use /unlimitedflight on|off|status in server chat.");
        if (!c.Source.IsPlayer || c.Source.Player.IsNone)
            c.Fail("Unlimited Flight needs your player: use /unlimitedflight on|off|status in chat or the hosting game's console.");
        var player = c.Source.Player;
        RulesRuntime.StartPolicy();
        if (on == false)
        {
            if (Sets.Remove(player, out var set)) set.Dispose();
            c.Reply("Unlimited Flight is off; only its own rule set is removed, other rule sets are unchanged. " + Describe(player).Text);
            return;
        }
        if (on == true && !IsOn(player))
        {
            if (ClientProblem(player) is { } problem) c.Fail("Unlimited Flight stays off: " + problem);
            // Service-owned even when a mod invokes Commands.Execute: unloading that mod must not revoke the command's set.
            // Capture before resolution, so a failure cannot strand an unowned set.
            var outer = ModContext.CurrentMod;
            RuleSet? made = null;
            ModContext.CurrentMod = null;
            try
            {
                Rules.Apply(RuleScope.ForPlayer(player), Requested, null, Name, set => Sets[player] = made = set);
                var applied = Describe(player);
                if (!applied.Gliding && !applied.Steering)
                {
                    Forget(player);
                    made?.Dispose();
                    c.Fail("Unlimited Flight stays off: " + applied.Text);
                }
            }
            catch
            {
                Forget(player);
                made?.Dispose();
                throw;
            }
            finally { ModContext.CurrentMod = outer; }
        }
        var now = Describe(player);
        var state = !IsOn(player) ? "off" : now.Gliding && now.Steering ? "on" : now.Gliding || now.Steering ? "on (partial)" : "on (inactive)";
        c.Reply($"Unlimited Flight is {state}. {now.Text}");
    }

    // These are the combined rules, not a claim that this command overrode another mod, a server limit or a missing hook.
    private static (bool Gliding, bool Steering, string Text) Describe(EntityId player)
    {
        var effective = RulesRuntime.Fresh(player);
        var hooks = Rules.Status();
        var limits = RulesRuntime.Limits;
        var client = ClientProblem(player);
        var gliding = limits.AllowUnlimitedGliding && hooks?.Gliding == true && client is null && effective.UnlimitedGliding;
        var steering = limits.AllowGlideSteer && hooks?.GlideSteer == true && client is null &&
                       RulesRuntime.CanPredictSteer(player) && effective.GlideSteer is not null;
        var stamina = !limits.AllowUnlimitedGliding ? "glider stamina denied by rules.json (allowUnlimitedGliding)"
            : hooks?.Gliding != true ? "glider stamina unavailable (this build has no gliding hook)"
            : client is not null ? "glider stamina held back: " + client
            : gliding ? "glider stamina drain off"
            : "glider stamina unchanged (not requested or blocked by another rule set)";
        var steer = !limits.AllowGlideSteer ? "S/W steering denied by rules.json (allowGlideSteer)"
            : hooks?.GlideSteer != true ? "S/W steering unavailable (this build has no steering hook)"
            : client is not null ? "S/W steering held back: " + client
            : !RulesRuntime.CanPredictSteer(player) ? "S/W steering waiting for your game to confirm its steering hook (older or unsupported clients keep the game's steering)"
            : steering ? $"S/W steering on (S pitch up, W dive; up to {effective.GlideSteer!.MaxClimbAngle:0.##} degrees)"
            : "S/W steering unchanged (not requested)";
        return (gliding, steering, $"Current combined glide: {stamina}; {steer}.");
    }

    private static string? ClientProblem(EntityId player) => Rules.ClientOf(player) switch
    {
        RulesClient.Host or RulesClient.Kiln or RulesClient.Simulated => null,
        RulesClient.Pending => "your game is still joining; try again in a moment",
        RulesClient.KilnWithoutRules => "your game has no Kiln rules channel; update Kiln in your game",
        _ => "your game has no Kiln rules connection; Kiln is needed in your game as well as the host",
    };

    // The runtime disposes player-scoped sets on leave/world close; drop our handles in the same lifecycle.
    internal static void Forget(EntityId player) => Sets.Remove(player);
    internal static void Forget() => Sets.Clear();
}
