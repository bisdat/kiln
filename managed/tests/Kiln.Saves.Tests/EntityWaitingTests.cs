// SPDX-License-Identifier: MIT
// EntityData.Waiting and Matched per mod (0.2.0-dev.29.15): the loaded attachments one mod still waits for, and those
// matched to a loaded entity. Waiting was documented as the calling mod's count but answered every mod's (Busy Hands read
// another mod's records as its own settings still missing).
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln;

[Collection("entity identity")]
public sealed class EntityWaitingTests
{
    private static readonly Guid Kiln = Guid.Parse("00000003-0000-0000-0000-000000000003");
    private static readonly Guid Chest = Guid.Parse("00000002-0000-0000-0000-000000000002");

    public EntityWaitingTests()
    {
        TestWorld.Reset();
        TestWorld.Templates[Kiln] = new(3, 9, "PropKiln");
        TestWorld.Templates[Chest] = new(3, 9, "PropChest");
    }

    private static JsonObject Saved(params (Guid Template, double X)[] at) => JsonNode.Parse(
        "{\"entities\":[" + string.Join(",", at.Select(a => $"{{\"template\":\"{a.Template}\",\"pos\":[{a.X},0,0],\"keys\":{{\"cfg\":1}}}}")) + "]}")!.AsObject();

    [Fact]
    public void WaitingAndMatchedCountTheAskingModsAttachmentsOnly()
    {
        var t = new EntityTable();
        t.Absorb("busy-hands", Saved((Kiln, 10), (Kiln, 20)));
        t.Absorb("other-mod", Saved((Chest, 30), (Kiln, 10)));  // also on busy-hands' kiln at 10: one record, both mods' keys
        t.StartMatching();
        Assert.Equal(3, t.UnresolvedCount);                     // what EntityData.Waiting answered before, for any mod
        Assert.Equal(2, t.UnresolvedFor("busy-hands"));
        Assert.Equal(2, t.UnresolvedFor("other-mod"));
        Assert.Equal(0, t.UnresolvedFor("a-third-mod"));
        Assert.Equal(0, t.MatchedFor("busy-hands"));

        // the world loads the kiln at 10: both mods' records on it are matched, busy-hands' other kiln still waits
        TestWorld.World[5001] = new SpawnLink.Ent(new EntityId(5001), Kiln, null, new WorldPosition(10, 0, 0), 0);
        Assert.NotNull(t.FindOrMatch(new EntityId(5001)));
        Assert.Equal((1, 1), (t.UnresolvedFor("busy-hands"), t.MatchedFor("busy-hands")));
        Assert.Equal((1, 1), (t.UnresolvedFor("other-mod"), t.MatchedFor("other-mod")));
        Assert.Equal(0, t.MatchedFor("a-third-mod"));

        // a new world's data: the counts start again
        t.Clear();
        Assert.Equal((0, 0), (t.UnresolvedFor("busy-hands"), t.MatchedFor("busy-hands")));
    }
}
