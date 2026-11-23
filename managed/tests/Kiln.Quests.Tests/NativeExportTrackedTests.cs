// SPDX-License-Identifier: MIT
// Tracked items and recipes (Kiln.Blocks KnowledgeBlock, since 0.2.0-dev.30.5) in the file kiln-quests' boot build reads: the quests file's
// "knowledge" entries with an id, next to a baked file, and what a baked file that differs says.
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.QuestTests;

public sealed class NativeExportTrackedTests
{
    [Fact]
    public void A_tracked_id_is_a_knowledge_entry_of_the_mods_file_until_it_goes()
    {
        NativeExport.Track("export-tracked", "track.brush", 3155237504u, "pickup", "both");
        NativeExport.Track("export-tracked", "track.made", 3289458456u, "craft", "player");
        var doc = NativeExport.Document("export-tracked", out _)!;
        Assert.Equal("""{"track.brush":{"id":"0xbc111a80","track":"pickup","scope":"both"},"track.made":{"id":"0xc4112718","track":"craft","scope":"player"}}""",
                     doc["knowledge"]!.ToJsonString());
        Assert.Null(NativeExport.Document("export-other", out _));
        NativeExport.Untrack("export-tracked", "track.brush");
        NativeExport.Untrack("export-tracked", "track.made");
        Assert.Null(NativeExport.Document("export-tracked", out _));
    }

    [Fact]
    public void A_baked_file_keeps_its_entries_and_a_difference_is_named()
    {
        var doc = new JsonObject { ["format"] = 1, ["knowledge"] = new JsonObject { ["track.a"] = new JsonObject { ["id"] = "0x1", ["track"] = "pickup" } } };
        var baked = (JsonObject)doc.DeepClone();
        Assert.Null(NativeExport.ForDataFile(doc, baked));          // the baked file has it: nothing more to write
        Assert.Null(NativeExport.BakedDifference(baked, doc));
        var now = (JsonObject)doc.DeepClone();
        now["knowledge"]!["track.b"] = new JsonObject { ["id"] = "0x2", ["track"] = "craft" };
        Assert.Contains("tracked item or recipe track.b is not in it", NativeExport.BakedDifference(baked, now), StringComparison.Ordinal);
        Assert.Equal(new[] { "track.b" }, ((JsonObject)NativeExport.ForDataFile(now, baked)!["knowledge"]!).Select(x => x.Key));
    }
}
