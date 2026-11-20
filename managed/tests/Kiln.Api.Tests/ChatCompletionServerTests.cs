// SPDX-License-Identifier: MIT
// The server side of chat Tab completion (kiln:chat, GitHub issue 44): what a player is offered is decided by that
// player's own permissions, requests are rate limited per peer, and a hosting player's requests take the same path.
using Xunit;

namespace Kiln.Api.Tests;

public sealed class ChatCompletionServerTests : KilnTest
{
    private static readonly Player Admin = new()
    {
        Entity = new EntityId(1), Name = "Alder", SteamId = 76561190000000001, State = PlayerState.InGame,
        Permissions = PlayerPermissions.KickBan | PlayerPermissions.EditWorld, PermissionsKnown = true,
    };

    private static readonly Player Guest = new()
    {
        Entity = new EntityId(2), Name = "Alan", SteamId = 76561190000000002, State = PlayerState.InGame,
        Permissions = PlayerPermissions.EditWorld, PermissionsKnown = true,
    };

    public ChatCompletionServerTests()
    {
        Arg.OnlinePlayers = () => [Admin, Guest];
        Permissions.FindPlayer = e => e.Value switch { 1 => Admin, 2 => Guest, _ => null };
        Permissions.FindPlayerBySteamId = id => id == Admin.SteamId ? Admin : id == Guest.SteamId ? Guest : null;
        ChatCompletionNetwork.ResetForTests();
        As(NewMod("attr"), () => Commands.RegisterAll(new AttributeCommands()));
    }

    public override void Dispose()
    {
        ChatCompletionNetwork.ResetForTests();
        base.Dispose();
    }

    private static ChatCompletionReply? Ask(Player who, string line, uint peer = 7, long now = 0) =>
        ChatCompletionNetwork.Complete(who.Entity, new ChatCompletionRequest(1, line), peer, now);

    [Fact]
    public void A_player_is_only_offered_commands_they_may_use()
    {
        var admin = Ask(Admin, "/h");
        Assert.NotNull(admin);
        Assert.Contains("heal", admin!.Suggestions);
        Assert.Contains("help", admin.Suggestions);

        // heal is Admin: a player without the kick/ban right never sees it (nor its alias)
        var guest = Ask(Guest, "/h", peer: 8);
        Assert.NotNull(guest);
        Assert.DoesNotContain("heal", guest!.Suggestions);
        Assert.DoesNotContain("hl", guest.Suggestions);
        // ...while an Everyone command of the same mod is
        Assert.Contains("say", Ask(Guest, "/s", peer: 8)!.Suggestions);

        // zone delete is Console: not even the admin is offered it over chat
        var zone = Ask(Admin, "/zone ", peer: 9);
        Assert.NotNull(zone);
        Assert.Contains("create", zone!.Suggestions);
        Assert.DoesNotContain("delete", zone.Suggestions);
    }

    [Fact]
    public void The_reply_echoes_the_request_and_starts_at_a_utf8_byte_offset()
    {
        var reply = Ask(Admin, "/zone ");
        Assert.NotNull(reply);
        Assert.Equal(1u, reply!.Id);
        Assert.Equal("/zone ", reply.Text);
        Assert.Equal(6, reply.Start);
        Assert.True(ChatCompletionRules.Matches(reply, 1, "/zone "));
    }

    [Fact]
    public void No_player_or_no_command_line_gets_nothing()
    {
        Assert.Null(ChatCompletionNetwork.Complete(EntityId.None, new ChatCompletionRequest(1, "/h"), 7, 0));
        Assert.Null(Ask(Admin, "hello"));
        Assert.Null(Ask(Admin, "/"));
    }

    [Fact]
    public void Ten_requests_a_second_per_peer()
    {
        for (var i = 0; i < 10; i++) Assert.NotNull(Ask(Admin, "/h", peer: 7, now: 1000));
        Assert.Null(Ask(Admin, "/h", peer: 7, now: 1500));        // the 11th within a second is dropped
        Assert.NotNull(Ask(Guest, "/h", peer: 8, now: 1500));     // another peer has its own budget
        Assert.NotNull(Ask(Admin, "/h", peer: 7, now: 2000));     // a second later it answers again
    }

    [Fact]
    public void A_hosting_players_request_is_answered_by_the_server_tick()
    {
        ChatCompletionNetwork.EnqueueLocal(Admin.Entity, new ChatCompletionRequest(42, "/h"));
        Assert.False(ChatCompletionNetwork.TakeReply(out _));
        ChatCompletionNetwork.DrainLocal();
        Assert.True(ChatCompletionNetwork.TakeReply(out var reply));
        Assert.Equal(42u, reply.Id);
        Assert.Contains("heal", reply.Suggestions);
        Assert.False(ChatCompletionNetwork.TakeReply(out _));

        // the host's own permissions decide, here a guest's
        ChatCompletionNetwork.EnqueueLocal(Guest.Entity, new ChatCompletionRequest(43, "/h"));
        ChatCompletionNetwork.DrainLocal();
        Assert.True(ChatCompletionNetwork.TakeReply(out var guest));
        Assert.Equal(43u, guest.Id);
        Assert.DoesNotContain("heal", guest.Suggestions);
    }
}
