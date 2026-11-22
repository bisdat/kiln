// SPDX-License-Identifier: MIT
// Items.SetContainerName offline (kiln.items v9, 0.2.0-dev.30.3, GitHub issue #112): the ABI mirrors, what a refusal at the
// call throws, and the whole path on the test kit's kiln.items (its rename model ports native/services/items/rename_logic.h
// and rename.cpp): the name stored at the next step and its task completing then, the game's Rename field's limits, an
// empty name clearing it, the entity checked at the call and again at the step, and the capability row. What the game does
// (replication, the save, the Rename screen) is the lab and client check list in docs/kiln/modding/inventory.md.
using System.Runtime.InteropServices;
using Kiln;
using Kiln.TestKit;
using Xunit;

namespace Kiln.ItemsTests;

public sealed unsafe class ContainerRenameTests
{
    private const ulong AdaSteam = 76561198000000951;

    // ---- the ABI ----

    [Fact]
    public void TheMirrorsHaveTheHeadersSizes()
    {
        // native items.cpp: sizeof(KilnItemsRenameRequest) == 272, Result 192, Status 184, sizeof(KilnItemsApi) == 384
        Assert.Equal((272, 192, 184, 384), (sizeof(ItemsNative.RenameRequest), sizeof(ItemsNative.RenameResult), sizeof(ItemsNative.RenameStatus),
            sizeof(ItemsNative.TableV9)));
        Assert.Equal(16, (int)Marshal.OffsetOf<ItemsNative.RenameRequest>(nameof(ItemsNative.RenameRequest.Text)));
        Assert.Equal((20, 28, 32), ((int)Marshal.OffsetOf<ItemsNative.RenameResult>(nameof(ItemsNative.RenameResult.GameError)),
            (int)Marshal.OffsetOf<ItemsNative.RenameResult>(nameof(ItemsNative.RenameResult.Changed)),
            (int)Marshal.OffsetOf<ItemsNative.RenameResult>(nameof(ItemsNative.RenameResult.Message))));
        Assert.Equal((8, 24), ((int)Marshal.OffsetOf<ItemsNative.RenameStatus>(nameof(ItemsNative.RenameStatus.Pending)),
            (int)Marshal.OffsetOf<ItemsNative.RenameStatus>(nameof(ItemsNative.RenameStatus.Detail))));
        Assert.Equal((int)ItemsNative.V8ScaleEnd, (int)Marshal.OffsetOf<ItemsNative.TableV9>(nameof(ItemsNative.TableV9.ContainerRename)));
        Assert.Equal((int)ItemsNative.V9RenameEnd, sizeof(ItemsNative.TableV9));
        Assert.Equal((48, 64), (Items.ContainerNameMaxChars, Items.ContainerNameMaxBytes));
    }

    // ---- pure parts ----

    [Fact]
    public void ARefusalAtTheCallThrowsWhatTheCallerGotWrong()
    {
        var e = new EntityId(5000);
        var tooLong = Assert.IsType<ArgumentException>(Items.RenameRefusal(ItemsNative.InvalidArgument, ItemsNative.RenameTooLong, "the name is 49 characters", e));
        Assert.Equal("name", tooLong.ParamName);
        Assert.Contains("49 characters", tooLong.Message);
        Assert.Equal("name", Assert.IsType<ArgumentException>(Items.RenameRefusal(ItemsNative.InvalidArgument, ItemsNative.RenameBadText, "control", e)).ParamName);
        var gone = Assert.IsType<ArgumentException>(Items.RenameRefusal(ItemsNative.NotFound, ItemsNative.RenameGone, "no such entity", e));
        Assert.Equal("entity", gone.ParamName);
        Assert.Contains("5000", gone.Message);
        var box = Assert.IsType<ArgumentException>(Items.RenameRefusal(ItemsNative.NotFound, ItemsNative.RenameNotContainer, "not a container", e));
        Assert.Contains("is not a container", box.Message);
        Assert.IsType<InvalidOperationException>(Items.RenameRefusal(ItemsNative.Unavailable, ItemsNative.RenameNotServer, "no server world", e));
        var off = Assert.IsType<KilnUnavailableException>(Items.RenameRefusal(ItemsNative.Unavailable, ItemsNative.RenameUnavailable, "off", e));
        Assert.Equal("items.rename", off.Capability);
    }

    // ---- on the test kit's kiln.items ----

    private sealed class Run : IDisposable
    {
        public readonly KilnHarness Kiln = new();
        public readonly FakePlayer Ada;
        public readonly EntityId Chest;

        public Run()
        {
            Ada = Kiln.Players.Join("Ada", AdaSteam, position: new WorldPosition(0, 0, 0));
            Chest = Kiln.Entities.Create(new WorldPosition(3, 0, 3));
            Kiln.Items.AddContainer(Chest);
            Kiln.Step(2);
        }

        public bool Rename(EntityId e, string name)
        {
            var t = Kiln.Run(() => Items.SetContainerName(e, name));
            Assert.False(t.IsCompleted);  // the rename runs at the start of the next step
            Kiln.Step();
            Assert.True(t.IsCompletedSuccessfully);
            return t.Result;
        }

        public ArgumentException Bad(EntityId e, string name) => Kiln.Run(() => Assert.Throws<ArgumentException>(() => { _ = Items.SetContainerName(e, name); }));

        public void Dispose() => Kiln.Dispose();
    }

    [Fact]
    public void TheRowIsOnInTheKit()
    {
        using var run = new Run();
        Assert.True(run.Kiln.Run(() => Items.ContainerRenameAvailable && Capabilities.Is("items.rename")), run.Kiln.Run(() => Capabilities.Why("items.rename")));
    }

    [Fact]
    public void ANameIsStoredAtTheNextStepAndAnEmptyOneClearsIt()
    {
        using var run = new Run();
        Assert.Null(run.Kiln.Items.ContainerName(run.Chest));
        Assert.True(run.Rename(run.Chest, "Ores"));
        Assert.Equal("Ores", run.Kiln.Items.ContainerName(run.Chest));
        Assert.True(run.Rename(run.Chest, "Ores"));  // the name it already has: still taken
        Assert.True(run.Rename(run.Chest, "  Wood & Stone #2  "));  // kept as given, no trimming
        Assert.Equal("  Wood & Stone #2  ", run.Kiln.Items.ContainerName(run.Chest));
        Assert.True(run.Rename(run.Chest, "\u6728\u6750 \U0001F525"));  // any script, an emoji
        Assert.Equal("\u6728\u6750 \U0001F525", run.Kiln.Items.ContainerName(run.Chest));
        Assert.True(run.Rename(run.Chest, ""));
        Assert.Null(run.Kiln.Items.ContainerName(run.Chest));
    }

    [Fact]
    public void TheGamesRenameFieldLimitsAreCheckedAtTheCall()
    {
        using var run = new Run();
        Assert.True(run.Rename(run.Chest, new string('a', 48)));
        Assert.Contains("49 characters", run.Bad(run.Chest, new string('a', 49)).Message);
        Assert.True(run.Rename(run.Chest, string.Concat(Enumerable.Repeat("\u00e9", 32))));  // 64 bytes of UTF-8
        Assert.Contains("66 bytes", run.Bad(run.Chest, string.Concat(Enumerable.Repeat("\u00e9", 33))).Message);
        Assert.Contains("300 bytes", run.Bad(run.Chest, new string('a', 300)).Message);
        Assert.True(run.Rename(run.Chest, string.Concat(Enumerable.Repeat("\U0001F525", 16))));  // 16 emoji, 64 bytes
        Assert.Contains("control character", run.Bad(run.Chest, "a\nb").Message);
        Assert.Contains("control character", run.Bad(run.Chest, "a\0b").Message);
        Assert.Contains("control character", run.Bad(run.Chest, "a\u007f").Message);
        Assert.True(run.Rename(run.Chest, "a\tb"));  // the field lets a tab through
        Assert.Contains("not valid text", run.Bad(run.Chest, "a\ud800b").Message);  // a lone surrogate
        Assert.Equal("name", run.Bad(run.Chest, new string('a', 49)).ParamName);
        Assert.Equal("a\tb", run.Kiln.Items.ContainerName(run.Chest));  // a refused call changed nothing
        run.Kiln.Run(() => Assert.Throws<ArgumentNullException>(() => { _ = Items.SetContainerName(run.Chest, null!); }));
    }

    [Fact]
    public void OnlyAContainerThatExistsCanBeRenamed()
    {
        using var run = new Run();
        Assert.Equal("entity", run.Bad(EntityId.None, "X").ParamName);
        Assert.Contains("no such entity", run.Bad(new EntityId(987654), "X").Message);
        Assert.Contains("not a container", run.Bad(run.Ada.Entity, "X").Message);  // a player's inventory
        var prop = run.Kiln.Entities.Create(new WorldPosition(5, 0, 5));
        Assert.Contains("not a container", run.Bad(prop, "X").Message);  // no inventory at all
    }

    [Fact]
    public void AContainerGoneBeforeTheStepEndsFalse()
    {
        using var run = new Run();
        var t = run.Kiln.Run(() => Items.SetContainerName(run.Chest, "Ores"));
        run.Kiln.Entities.Destroy(run.Chest);
        run.Kiln.Step();
        Assert.False(t.Result);
        Assert.Null(run.Kiln.Items.ContainerName(run.Chest));
    }

    [Fact]
    public void OffTheTickThreadThrows()
    {
        using var run = new Run();
        Assert.Throws<InvalidOperationException>(() => { _ = Items.SetContainerName(run.Chest, "Ores"); });
    }

    [Fact]
    public void ASwitchedOffFeatureThrowsNamingTheRow()
    {
        using var run = new Run();
        run.Kiln.Capabilities.Override("items.rename", false, "test: off");
        var off = run.Kiln.Run(() => Assert.Throws<KilnUnavailableException>(() => { _ = Items.SetContainerName(run.Chest, "Ores"); }));
        Assert.Equal("items.rename", off.Capability);
        Assert.False(run.Kiln.Run(() => Items.ContainerRenameAvailable));
    }

    [Fact]
    public void ARenameSwitchedOffAfterTheCallEndsFalse()
    {
        using var run = new Run();
        var t = run.Kiln.Run(() => Items.SetContainerName(run.Chest, "Ores"));
        run.Kiln.Capabilities.Override("items.rename", false);
        run.Kiln.Step();
        Assert.False(t.Result);
        run.Kiln.Capabilities.ClearOverride("items.rename");
        Assert.Null(run.Kiln.Items.ContainerName(run.Chest));
    }

    [Fact]
    public void RenamesOfOneChestRunInOrderInOneStep()
    {
        using var run = new Run();
        var a = run.Kiln.Run(() => Items.SetContainerName(run.Chest, "A"));
        var b = run.Kiln.Run(() => Items.SetContainerName(run.Chest, "B"));
        run.Kiln.Step();
        Assert.True(a.Result && b.Result);
        Assert.Equal("B", run.Kiln.Items.ContainerName(run.Chest));
    }
}
