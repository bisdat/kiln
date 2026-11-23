// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln;

public sealed partial class PlayerStorageStoreTests
{
    [Fact]
    public void Held_storage_is_listed_and_released_with_the_command_the_notice_names()
    {
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var persist = new Persist { ExtraPages = true };
        store.Native = persist;
        var released = new List<(EntityId Player, string Key, bool Backpack)>();
        var oldRelease = PlayerStorageCommand.Release;
        Commands.Unregister(PlayerStorageCommand.Name);
        PlayerStorageCommand.ResetForTests();
        try
        {
            _ = typeof(Commands).Assembly; // Kiln.Chat is loaded, as the first service tick does in a game
            Assert.True(PlayerStorageCommand.TryRegister(final: true));
            var info = Commands.Find(PlayerStorageCommand.Name)!;
            Assert.Equal(("kiln", CommandPermission.Admin), (info.Owner, info.Permission));
            PlayerStorageCommand.Release = (player, key, target) =>
            {
                released.Add((player, key, target.IsBackpack));
                return Task.FromResult(new MoveResult(ItemsStatus.Done, MoveKind.All, player, player, 1, 1, 1, [], "", 0, false, 1, false));
            };
            // A vanilla client brings a character with extra main pages: the newer server copy is held.
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
            store.Join(character, new EntityId(7), "vanilla-returned", null, true, name: "Hero");
            var notice = store.Notice(character)!;
            Assert.Contains("/playerstorage list Hero", notice);
            Assert.Contains("/playerstorage release Hero <key>", notice);

            var list = Commands.Execute(CommandSource.Console, "/playerstorage list Hero");
            Assert.True(list.Ok, list.Text);
            Assert.Contains(list.Output, l => l.Contains("mod:bag") && l.Contains("1 stack") && l.Contains("returned outside Kiln"));
            Assert.Contains(list.Output, l => l.Contains("/playerstorage release Hero mod:bag"));
            // The character id names the same player (a name may be unknown, as for a lab peer).
            Assert.True(Commands.Execute(CommandSource.Console, $"/playerstorage list {character.Id}").Ok);

            var release = Commands.Execute(CommandSource.Console, "/playerstorage release Hero mod:bag");
            Assert.True(release.Ok, release.Text);
            Assert.Equal([(new EntityId(7), "mod:bag", true)], released);
            Assert.Contains(release.Output, l => l.Contains("mod:bag") && l.Contains("backpack"));

            // Hero left and another character now plays in that slot: a release by Hero's name must not touch it.
            persist.InSlot = new CharacterKey(2, Guid.Parse("00000000-0000-4000-8000-000000000999"));
            var stale = Commands.Execute(CommandSource.Console, "/playerstorage release Hero mod:bag");
            Assert.False(stale.Ok);
            Assert.Contains("not in game", stale.Error);
            persist.InSlot = null;
            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage release Hero other:key").Ok);
            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage list Nobody").Ok);
            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage").Ok);
            Assert.True(Commands.Execute(CommandSource.ForSteamId(76561198000000001, "Guest"), "/playerstorage list Hero").Denied);
            Assert.Single(released);
        }
        finally
        {
            PlayerStorageCommand.Release = oldRelease;
            Commands.Unregister(PlayerStorageCommand.Name);
            PlayerStorageCommand.ResetForTests();
            store.Native = null;
        }
    }
    [Fact]
    public void A_name_two_characters_of_one_account_share_reaches_the_one_in_game()
    {
        // One account's characters share its platform name, and a visit stays after its character left. Hero played a
        // first character on entity 3 and left; the second plays on entity 8 with a hold. The command the notice prints
        // must reach the second, not the first (whose listing has nothing held and who is not in game).
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        var first = new CharacterKey(1, Guid.Parse("00000000-0000-4000-8000-000000000111"));
        var second = new CharacterKey(1, PlayerStorageDocumentTests.Character);
        var persist = new Persist { ExtraPages = true };
        store.Native = persist;
        var released = new List<(EntityId Player, string Key)>();
        var oldRelease = PlayerStorageCommand.Release;
        Commands.Unregister(PlayerStorageCommand.Name);
        PlayerStorageCommand.ResetForTests();
        try
        {
            Assert.True(PlayerStorageCommand.TryRegister(final: true));
            PlayerStorageCommand.Release = (player, key, target) =>
            {
                released.Add((player, key));
                return Task.FromResult(new MoveResult(ItemsStatus.Done, MoveKind.All, player, player, 1, 1, 1, [], "", 0, false, 1, false));
            };
            persist.Who = e => e.Value == 3 ? first : null;
            store.Join(first, new EntityId(3), "join", null, true, name: "Hero");
            Assert.Null(store.Notice(first));
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + second.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
            persist.Who = e => e.Value == 8 ? second : null;  // the first character left; the second plays on entity 8
            store.Join(second, new EntityId(8), "vanilla-returned", null, true, name: "Hero");
            Assert.Contains("/playerstorage release Hero <key>", store.Notice(second)!);

            Assert.Equal(new EntityId(8), store.FindPlayer("Hero"));
            var list = Commands.Execute(CommandSource.Console, "/playerstorage list Hero");
            Assert.True(list.Ok, list.Text);
            Assert.Contains(list.Output, l => l.Contains(second.ToString()));
            var release = Commands.Execute(CommandSource.Console, "/playerstorage release Hero mod:bag");
            Assert.True(release.Ok, release.Error);
            Assert.Equal([(new EntityId(8), "mod:bag")], released);
            // The first character's own id still names it (not in game: refused, nothing released).
            Assert.False(Commands.Execute(CommandSource.Console, $"/playerstorage release {first.Id} mod:bag").Ok);
            Assert.Single(released);
        }
        finally
        {
            PlayerStorageCommand.Release = oldRelease;
            Commands.Unregister(PlayerStorageCommand.Name);
            PlayerStorageCommand.ResetForTests();
            store.Native = null;
        }
    }

    private sealed class LogTap(IKilnBackend inner) : IKilnBackend
    {
        public readonly List<string> Lines = [];
        public string Version => inner.Version;
        public bool IsTickThread => inner.IsTickThread;
        public bool HasFeature(string name) => inner.HasFeature(name);
        public nint Service(string name, uint minVersion) => inner.Service(name, minVersion);
        public void Log(ulong owner, LogLevel level, string text)
        {
            lock (Lines) Lines.Add(text);
            inner.Log(owner, level, text);
        }
    }

    [Fact]
    public void Hold_notice_and_admin_log_name_the_player_and_character_never_a_reusable_entity_id()
    {
        // An entity id is the player's slot + 1: the next player in that slot gets it, so a command copied from an old
        // log line would act on someone else's hold under the same key. The name (quoted if it has a space) and the
        // character id identify the player.
        using var h = new InstanceHarness();
        var store = InstanceHarness.Store.Storage;
        store.Native = new Persist { ExtraPages = true };
        var backend = Backend.Current;
        var tap = new LogTap(backend);
        Backend.Current = tap;
        try
        {
            var character = new CharacterKey(1, PlayerStorageDocumentTests.Character);
            InstanceHarness.Store.GetBundleRaw(PlayerStorageStore.Prefix + character.Text, "kiln-items").Keys["storage"] =
                new WorldStore.Slot { Json = PlayerStorageDocumentTests.Doc("old", 1000).Write(32) };
            store.Join(character, new EntityId(7), "vanilla-returned", null, true, name: "Fuzen Q");
            var notice = store.Notice(character)!;
            Assert.Contains("/playerstorage list \"Fuzen Q\"", notice);
            Assert.Contains("/playerstorage release \"Fuzen Q\" <key>", notice);
            Assert.DoesNotContain(" 7", notice);
            string held;
            lock (tap.Lines) held = Assert.Single(tap.Lines, l => l.Contains("stacks held"));
            Assert.Contains("Fuzen Q", held);
            Assert.Contains(character.ToString(), held);
            Assert.Contains("/playerstorage release \"Fuzen Q\" mod:bag", held);
            Assert.Contains(store.Listing(new EntityId(7))!, l => l.Contains("/playerstorage release \"Fuzen Q\" mod:bag"));
        }
        finally
        {
            Backend.Current = backend;
            store.Native = null;
        }
    }
}
