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
            store.Join(character, new EntityId(7), "vanilla-returned", null, true);
            var notice = store.Notice(character)!;
            Assert.Contains("/playerstorage list 7", notice);
            Assert.Contains("/playerstorage release 7 <key>", notice);

            var list = Commands.Execute(CommandSource.Console, "/playerstorage list 7");
            Assert.True(list.Ok, list.Text);
            Assert.Contains(list.Output, l => l.Contains("mod:bag") && l.Contains("1 stack") && l.Contains("returned outside Kiln"));
            Assert.Contains(list.Output, l => l.Contains("/playerstorage release 7 mod:bag"));

            var release = Commands.Execute(CommandSource.Console, "/playerstorage release 7 mod:bag");
            Assert.True(release.Ok, release.Text);
            Assert.Equal([(new EntityId(7), "mod:bag", true)], released);
            Assert.Contains(release.Output, l => l.Contains("mod:bag") && l.Contains("backpack"));

            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage release 7 other:key").Ok);
            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage list 99").Ok);
            Assert.False(Commands.Execute(CommandSource.Console, "/playerstorage").Ok);
            Assert.True(Commands.Execute(CommandSource.ForSteamId(76561198000000001, "Guest"), "/playerstorage list 7").Denied);
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
}
