// SPDX-License-Identifier: MIT
// What a web panel needs to turn every command into a form (Kiln.Admin's commands_describe) and to run a line as a named
// remote source capped at a level (command_run's source + level), docs/kiln/launcher/admin-protocol.md.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.Api.Tests;

public sealed class CommandFormsTests : KilnTest
{
    private static readonly Player Alder = new()
    {
        Entity = new EntityId(1), Name = "Alder", SteamId = 76561190000000001, State = PlayerState.InGame,
        Permissions = PlayerPermissions.KickBan | PlayerPermissions.EditWorld, PermissionsKnown = true,
    };

    public CommandFormsTests()
    {
        Arg.OnlinePlayers = () => [Alder];
        Permissions.FindPlayer = e => e.Value == 1 ? Alder : null;
        Permissions.FindPlayerBySteamId = id => id == Alder.SteamId ? Alder : null;
        As(NewMod("attr"), () => Commands.RegisterAll(new AttributeCommands()));
    }

    private static JsonObject Command(string name, CommandSource? visibleTo = null) =>
        Commands.Describe(visibleTo).Select(n => n!.AsObject()).Single(o => o["name"]!.GetValue<string>() == name);

    /// <summary>The argument nodes from the root down the first child each time (the command's own arguments).</summary>
    private static List<JsonObject> Args(JsonObject command)
    {
        var list = new List<JsonObject>();
        var node = command["tree"]!.AsObject();
        while (node["children"] is JsonArray { Count: > 0 } kids)
        {
            node = kids[0]!.AsObject();
            if (node["kind"]!.GetValue<string>() == "argument") list.Add(node);
        }
        return list;
    }

    private static JsonObject Child(JsonObject node, string name) =>
        node["children"]!.AsArray().Select(n => n!.AsObject()).Single(o => o["name"]!.GetValue<string>() == name);

    [Fact]
    public void Describe_gives_each_argument_type_range_options_and_default()
    {
        var heal = Command("heal");
        Assert.Equal("attr", heal["owner"]!.GetValue<string>());
        Assert.Equal("attr.command.heal", heal["node"]!.GetValue<string>());
        Assert.Equal("admin", heal["permission"]!.GetValue<string>());
        var (target, amount) = (Args(heal)[0], Args(heal)[1]);
        Assert.Equal("player", target["type"]!.GetValue<string>());
        Assert.False(target["optional"]!.GetValue<bool>());
        Assert.False(target["many"]!.GetValue<bool>());
        Assert.Equal(["@s", "@p", "@r", "@a"], target["selectors"]!.AsArray().Select(n => n!.GetValue<string>()));
        Assert.Equal("int", amount["type"]!.GetValue<string>());
        Assert.Equal(1, amount["min"]!.GetValue<int>());
        Assert.Equal(1000, amount["max"]!.GetValue<int>());
        Assert.True(amount["optional"]!.GetValue<bool>());
        Assert.Equal(100, amount["default"]!.GetValue<int>());

        var say = Args(Command("say"))[0];
        Assert.Equal("greedy", say["type"]!.GetValue<string>());
        Assert.True(say["greedy"]!.GetValue<bool>());
        Assert.False(say["quotable"]!.GetValue<bool>());

        var wait = Args(Command("wait"));
        Assert.Equal("duration", wait[0]["type"]!.GetValue<string>());
        Assert.Equal("s", wait[0]["unit"]!.GetValue<string>());
        Assert.Equal("time", wait[1]["type"]!.GetValue<string>());
        Assert.Equal(24, wait[1]["max"]!.GetValue<int>());
        Assert.Equal(["forward", "reset"], wait[2]["options"]!.AsArray().Select(n => n!.GetValue<string>()));
        Assert.True(wait[2]["optional"]!.GetValue<bool>());
        Assert.Equal("forward", wait[2]["default"]!.GetValue<string>());

        var pick = Args(Command("pick"));
        Assert.Equal(["red", "green"], pick[0]["options"]!.AsArray().Select(n => n!.GetValue<string>()));
        Assert.False(pick[1]["default"]!.GetValue<bool>());

        var who = Args(Command("all"))[0];
        Assert.True(who["many"]!.GetValue<bool>());

        var zone = Command("zone")["tree"]!.AsObject();
        var create = Child(zone, "create");
        var name = Child(create, "name");
        Assert.Equal("string", name["type"]!.GetValue<string>());
        Assert.True(name["quotable"]!.GetValue<bool>());
        Assert.False(name.ContainsKey("default"));
        var at = Child(name, "at");
        Assert.True(at["relative"]!.GetValue<bool>());
        var radius = Child(at, "radius");
        Assert.Equal(10, radius["default"]!.GetValue<double>());
        var delete = Child(zone, "delete");
        Assert.Equal("attr.command.zone.delete", delete["permission"]!.GetValue<string>());
        Assert.Equal("console", delete["level"]!.GetValue<string>());
        Assert.False(delete.ContainsKey("optional"));  // a subcommand, not an argument
    }

    [Fact]
    public void A_capped_remote_runs_only_commands_up_to_its_level()
    {
        var mod = CommandSource.RemoteCapped("web:alice", CommandPermission.Everyone);
        var admin = CommandSource.RemoteCapped("web:bob", CommandPermission.Admin);
        var owner = CommandSource.RemoteCapped("web:owner", CommandPermission.Console);
        Assert.Equal("web:alice", mod.Name);
        Assert.Equal(CommandSourceKind.Remote, mod.Kind);
        Assert.False(mod.IsAdmin);
        Assert.True(admin.IsAdmin);

        Assert.True(Commands.Execute(mod, "zone list").Ok);
        Assert.True(Commands.Execute(mod, "heal alder").Denied);
        Assert.True(Commands.Execute(admin, "heal alder").Ok);
        var del = Commands.Execute(admin, "zone delete x");
        Assert.True(del.Denied, del.Text);
        Assert.True(Commands.Execute(owner, "zone delete x").Ok);
        // the uncapped remote of today is unchanged
        Assert.True(Commands.Execute(CommandSource.Remote("dashboard"), "zone delete y").Ok);

        // what each may see
        Assert.DoesNotContain(Commands.Describe(mod), n => n!["name"]!.GetValue<string>() == "heal");
        Assert.Contains(Commands.Describe(admin), n => n!["name"]!.GetValue<string>() == "heal");
        var zone = Command("zone", admin)["tree"]!["children"]!.AsArray().Select(n => n!["name"]!.GetValue<string>());
        Assert.DoesNotContain("delete", zone);
        Assert.DoesNotContain("delete", Commands.Complete(admin, "/zone ").Suggestions);
    }

    [Fact]
    public void Logs_and_events_name_the_web_user_and_the_cap()
    {
        var seen = new List<CommandExecutedEvent>();
        Events.On<CommandExecutedEvent>(seen.Add);
        var src = CommandSource.RemoteCapped("web:alice", CommandPermission.Everyone);
        Assert.True(Commands.Execute(src, "zone list").Ok);
        Assert.Equal("web:alice", seen.Single().Source.Name);
        Assert.Equal("remote web:alice (up to everyone)", src.ToString());
        Assert.Equal("remote dashboard", CommandSource.Remote("dashboard").ToString());
        lock (Backend.Lines) Assert.Contains(Backend.Lines, l => l.Contains("command /zone by remote web:alice (up to everyone): ok", StringComparison.Ordinal));
    }

    [Fact]
    public void A_cap_holds_for_a_player_and_inside_permission_checks()
    {
        var alder = CommandSource.ForSteamId(Alder.SteamId, "Alder");
        Assert.True(Commands.Execute(alder, "heal alder").Ok);  // kick/ban: admin
        var capped = alder.WithCap(CommandPermission.Everyone);
        Assert.True(Commands.Execute(capped, "heal alder").Denied);
        Assert.False(capped.IsAdmin);
        var d = Permissions.Explain(capped.Subject, "attr.command.heal");
        Assert.False(d.Allowed);
        Assert.Equal("cap:everyone", d.From);
        // a Console node granted by name stays above an Admin cap
        Permissions.SetPlayerNode(Alder.SteamId, "attr.command.zone.delete", true);
        Assert.True(Commands.Execute(alder, "zone delete x").Ok);
        Assert.True(Commands.Execute(alder.WithCap(CommandPermission.Admin), "zone delete x").Denied);
        // a cap never rises
        Assert.Same(capped, capped.WithCap(CommandPermission.Console));
        // a remote subject with a cap, as a handler's own Has check sees it
        var remote = CommandSource.RemoteCapped("web:bob", CommandPermission.Admin);
        Assert.False(remote.Has("attr.command.zone.delete"));
        Assert.True(remote.Has("attr.command.heal"));
        Assert.False(Permissions.IsAdmin(CommandSource.RemoteCapped("web:alice", CommandPermission.Everyone).Subject));
        Assert.True(Permissions.IsAdmin(remote.Subject));
    }
}
