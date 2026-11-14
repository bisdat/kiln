// SPDX-License-Identifier: MIT
using Xunit;

namespace Kiln.Rules.Tests;

/// <summary>The book of players who are in NoClip, by their character: what is kept, when it goes, and whose it is
/// (GitHub issue #25: leaving or closing the world inside the ground).</summary>
public class NoClipReturnsTests
{
    private sealed class Store : INoClipReturnStore
    {
        public NoClipReturnBook? Current = new();
        public NoClipReturnBook? Book => Current;
        public void Wrote(string key, NoClipReturnEntry? entry) { }
    }

    // Kiln.Saves as the store sees it: whether it is there, whether the world's data is loaded, the world, its book.
    private sealed class World
    {
        public bool Saves = true, Ready = true;
        public string? Id = "world-a";
        public NoClipReturnBook Data = new();  // the world's live data; a load replaces it with what the sidecar held

        public NoClipReturnStore Store() => new(() => Saves, () => Ready, () => Id, () => Data);

        /// <summary>The world closes and loads again; its sidecar holds what the last successful save wrote.</summary>
        public void Reload(NoClipReturnBook sidecar, NoClipReturnStore store)
        {
            Ready = false;
            store.WorldClosing();
            Data = sidecar;
            Ready = true;
        }
    }

    private static readonly DateTime Now = new(2026, 11, 14, 12, 0, 0, DateTimeKind.Utc);
    private static readonly Guid Char1 = Guid.Parse("11111111-2222-4333-8444-555555555555");

    [Fact]
    public void APlayerIsKeptByTheirCharacterNotTheSessionEntity()
    {
        var k = NoClipReturns.KeyOf(new CharacterKey(0xabc, Char1), 76561198000000001, EntityId.None);
        Assert.Equal("character/0000000000000abc.11111111222243338444555555555555", k);
        Assert.NotEqual(k, NoClipReturns.KeyOf(new CharacterKey(0xabc, Guid.NewGuid()), 76561198000000001, EntityId.None));  // another character
        Assert.Equal("steam/76561198000000001", NoClipReturns.KeyOf(CharacterKey.None, 76561198000000001, EntityId.None));
        Assert.Equal("steam/42", NoClipReturns.KeyOf(new CharacterKey(7, Guid.Empty), 42, EntityId.None));  // an account-only key is weaker than Steam
        Assert.Equal("character/0000000000000007.account", NoClipReturns.KeyOf(new CharacterKey(7, Guid.Empty), 0, EntityId.None));
        Assert.Null(NoClipReturns.KeyOf(CharacterKey.None, 0, EntityId.None));  // nobody to tell apart: nothing kept
        Assert.Equal("lab-character/2542", NoClipReturns.KeyOf(new CharacterKey(1, Char1), 5, new EntityId(2542)));
    }

    [Fact]
    public void TheLastFreePlaceIsKeptAndMovesOnlyWhenItMoves()
    {
        var store = new Store();
        var r = new NoClipReturns(store);
        Assert.False(r.Any);
        Assert.True(r.Note("character/a", new WorldPosition(1, 2, 3), "Bis", Now));
        Assert.True(r.Any);
        Assert.False(r.Note("character/a", new WorldPosition(1.1, 2, 3), "Bis", Now));  // under half a metre: not written again
        Assert.True(r.Note("character/a", new WorldPosition(4, 2, 3), "Bis", Now));
        var e = r.Of("character/a")!;
        Assert.Equal(new WorldPosition(4, 2, 3), e.Position);
        Assert.Equal("Bis", e.Name);
        Assert.Null(r.Of("character/b"));
    }

    [Fact]
    public void SeenFreeTheEntryGoes()
    {
        var r = new NoClipReturns(new Store());
        r.Note("character/a", new WorldPosition(1, 2, 3), "", Now);
        Assert.True(r.Forget("character/a"));
        Assert.False(r.Forget("character/a"));
        Assert.False(r.Any);
        Assert.Null(r.Of("character/a"));
    }

    [Fact]
    public void BeforeTheWorldIsLoadedNothingIsDecided()
    {
        var store = new Store { Current = null };
        var r = new NoClipReturns(store);
        Assert.False(r.Note("character/a", new WorldPosition(1, 2, 3), "", Now));
        Assert.Null(r.Of("character/a"));
        Assert.False(r.Forget("character/a"));
        Assert.False(r.Any);
    }

    [Fact]
    public void TheBookReadsBackFromTheWorldsData()
    {
        var book = new NoClipReturnBook();
        new NoClipReturns(new Store { Current = book }).Note("character/a", new WorldPosition(1.5, -2, 3), "Bis", Now);
        var json = System.Text.Json.JsonSerializer.Serialize(book);
        var back = System.Text.Json.JsonSerializer.Deserialize<NoClipReturnBook>(json)!;
        Assert.Equal(new WorldPosition(1.5, -2, 3), back.Players["character/a"].Position);
        Assert.DoesNotContain("Position", json);
    }

    [Fact]
    public void ASaveThatFailsDoesNotLoseTheEntryWithinTheRun()
    {
        var w = new World();
        var store = w.Store();
        var r = new NoClipReturns(store);
        r.Note("character/a", new WorldPosition(1, 2, 3), "Bis", Now);
        // the world's last save failed: its sidecar does not have the entry; the same game loads the world again
        w.Reload(new NoClipReturnBook(), store);
        Assert.Equal(new WorldPosition(1, 2, 3), r.Of("character/a")?.Position);
        Assert.True(w.Data.Players.ContainsKey("character/a"));  // back in the world's data: the next save carries it
    }

    [Fact]
    public void WithoutASaveThatCarriedItARestartHasNoEntry()
    {
        // a new process (a server restart) after a failed save: only the sidecar is left, and it never had the entry
        var w = new World();
        var r = new NoClipReturns(w.Store());
        r.Note("character/a", new WorldPosition(1, 2, 3), "Bis", Now);
        var restarted = new World { Data = new NoClipReturnBook() };
        Assert.Null(new NoClipReturns(restarted.Store()).Of("character/a"));
    }

    [Fact]
    public void AForgottenEntryDoesNotComeBackFromTheCopy()
    {
        var w = new World();
        var store = w.Store();
        var r = new NoClipReturns(store);
        r.Note("character/a", new WorldPosition(1, 2, 3), "Bis", Now);
        r.Forget("character/a");
        w.Reload(new NoClipReturnBook(), store);
        Assert.Null(r.Of("character/a"));
    }

    [Fact]
    public void TheCopyBelongsToItsWorld()
    {
        var w = new World();
        var store = w.Store();
        var r = new NoClipReturns(store);
        r.Note("character/a", new WorldPosition(1, 2, 3), "Bis", Now);
        w.Id = "world-b";
        w.Reload(new NoClipReturnBook(), store);
        Assert.Null(r.Of("character/a"));  // another world: its coordinates mean nothing here
        w.Id = "world-a";
        w.Reload(new NoClipReturnBook(), store);
        Assert.NotNull(r.Of("character/a"));
    }

    [Fact]
    public void BeforeTheWorldLoadsTheStoreDecidesNothingAndWithoutSavesItKeepsMemoryUntilTheWorldCloses()
    {
        var w = new World { Ready = false };
        var store = w.Store();
        var r = new NoClipReturns(store);
        Assert.False(r.Note("character/a", new WorldPosition(1, 2, 3), "", Now));
        w.Saves = false;  // no Kiln.Saves: memory
        Assert.True(r.Note("character/a", new WorldPosition(1, 2, 3), "", Now));
        Assert.NotNull(r.Of("character/a"));
        store.WorldClosing();
        Assert.Null(r.Of("character/a"));
    }
}
