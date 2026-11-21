// SPDX-License-Identifier: MIT
// The dedicated-server part of joined-server UI commands (dossier 09 §4.5, §6; DESIGN.md §5), driven by uicmdnet.py.
// A lab transport peer has no player (tools/nettest), so this mod stands one in: over "ui-blocks-test:ctl" the fake client
// asks to be bound to a synthetic entity, and the receiver's player lookup (Kiln.UiCommandChannel.PlayerOf, internal, set by
// reflection here) returns it for that lab peer only. That stands in for the game's own connection-to-player record; nothing
// a request carries can choose it. Test commands, run by the receiver as CommandSource.ForPlayer(<that entity>):
//   uicmd-echo <tag>          everyone: "source=<kind> player=<entity> tag=<tag>"
//   uicmd-admin <tag>         admin only
//   uicmd-cancelme <tag>      everyone, but a CommandExecutingEvent handler of this mod stops it
//   uicmd-slow <tag> <steps>  everyone, async: ends after that many server steps with "slow done <tag>"
//   uicmd-count <tag>         how many times a test command with that tag reached its handler
using System.Collections.Concurrent;
using System.Reflection;
using Kiln;

namespace UiBlocksTest;

public sealed record UiCtl(int Id, string Op, uint Entity, string Tag);

public sealed record UiCtlReply(int Id, bool Ok, int Count, bool Started, bool UiAvailable, string Text);

internal static class UiCommandLab
{
    private static readonly ConcurrentDictionary<string, int> Counts = new();
    private static readonly ConcurrentDictionary<uint, uint> Bound = new();  // lab peer id -> synthetic entity
    private static Func<Task> _nextTick = static () => Task.CompletedTask;
    private static NetChannel? _ctl;
    private static Action<bool, string> _check = static (_, _) => { };

    private static void Hit(string tag) => Counts.AddOrUpdate(tag, 1, static (_, n) => n + 1);

    public static void Start(Func<Task> nextTick, Action<bool, string> check, Action<string> info)
    {
        _nextTick = nextTick;
        _check = check;
        Commands.RegisterAll(typeof(UiCommandLab));
        Events.On<CommandExecutingEvent>(e =>
        {
            if (e.Command.Name != "uicmd-cancelme") return;
            e.CancelMessage = "stopped by ui-blocks-test's CommandExecutingEvent handler";
            e.Cancel();
        });
        var channel = typeof(BlockUi).Assembly.GetType("Kiln.UiCommandChannel", throwOnError: true)!;
        var playerOf = channel.GetField("PlayerOf", BindingFlags.NonPublic | BindingFlags.Static)!;
        var real = (Func<NetPeer, EntityId>)playerOf.GetValue(null)!;
        playerOf.SetValue(null, (Func<NetPeer, EntityId>)(p => p.IsLab && Bound.TryGetValue(p.Id, out var e) ? new EntityId(e) : real(p)));
        _ctl = Network.Channel("ui-blocks-test:ctl", "1", required: false, maxMessageBytes: 4096)
            .RegisterJson<UiCtl>(Receive, NetDirection.ToServer)
            .RegisterJson<UiCtlReply>(null, NetDirection.ToClient);
        var started = (bool)channel.GetProperty("Started", BindingFlags.NonPublic | BindingFlags.Static)!.GetValue(null)!;
        check(started && !Ui.Available && Network.Channels.Any(c => c.Name == "kiln:ui-command" && !c.Required),
            $"ui-command: dedicated server without Ui receives (receiver started {started}, overlay {(Ui.Available ? "present" : "absent")}, kiln:ui-command optional)");
        info("ui-command: lab binding ready (ui-blocks-test:ctl)");
    }

    private static void Receive(UiCtl m, NetContext ctx)
    {
        var p = ctx.Sender;
        var text = "";
        switch (m.Op)
        {
            case "bind" when p.IsLab && m.Entity != 0:
                Bound[p.Id] = m.Entity;
                text = $"lab peer 0x{p.Id:x} stands for entity {m.Entity}";
                break;
            case "unbind":
                Bound.TryRemove(p.Id, out _);
                text = $"lab peer 0x{p.Id:x} has no player now";
                break;
            case "count":
                break;
            case "check":
                // the fake client's verdicts, in the server's log next to the receiver's own lines
                _check(m.Entity == 1, m.Tag);
                break;
        }
        ctx.Reply(new UiCtlReply(m.Id, true, Counts.GetValueOrDefault(m.Tag), UiCommandStarted(), Ui.Available, text));
    }

    private static bool UiCommandStarted() =>
        (bool)typeof(BlockUi).Assembly.GetType("Kiln.UiCommandChannel")!.GetProperty("Started", BindingFlags.NonPublic | BindingFlags.Static)!.GetValue(null)!;

    [Command("uicmd-echo", Description = "ui-blocks-test: says who ran it")]
    private static string Echo(CommandContext ctx, string tag)
    {
        Hit(tag);
        return $"source={ctx.Source.Kind} player={ctx.Source.Player.Value} tag={tag}";
    }

    [Command("uicmd-admin", Description = "ui-blocks-test: admins only", Permission = CommandPermission.Admin)]
    private static string Admin(string tag)
    {
        Hit(tag);
        return $"admin ran tag={tag}";
    }

    [Command("uicmd-cancelme", Description = "ui-blocks-test: stopped by a CommandExecutingEvent handler")]
    private static string CancelMe(string tag)
    {
        Hit(tag);
        return $"not stopped tag={tag}";
    }

    [Command("uicmd-slow", Description = "ui-blocks-test: an async command that ends after some server steps")]
    private static async Task Slow(CommandContext ctx, string tag, int steps)
    {
        Hit(tag);
        for (var i = 0; i < steps; i++) await _nextTick();
        ctx.Reply($"slow done {tag}");
    }

    [Command("uicmd-count", Description = "ui-blocks-test: how often a tag ran")]
    private static string Count(string tag) => $"count {tag}={Counts.GetValueOrDefault(tag)}";
}
