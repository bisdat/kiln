using System.Text.Json.Nodes;
using Xunit;
namespace Kiln.Contract.Tests;
public sealed class ContentSyncQuotaTests
{
    [Fact]
    public void A_truncated_native_report_cannot_hide_definition_refusals()
    {
        foreach (var report in new[] { "{\"applied\":0,\"errors\":[\"no room", "{}", "[]", "{\"applied\":\"bad\",\"errors\":[]}", "{\"applied\":-1,\"errors\":[]}" })
        {
            var refused = ContentSync.ReadApplyReport(report);
            Assert.False(ContentSync.ApplySucceeded(0, refused.Errors));
        }
        var accepted = ContentSync.ReadApplyReport("{\"applied\":3,\"errors\":[]}");
        Assert.Equal(3, accepted.Applied);
        Assert.True(ContentSync.ApplySucceeded(0, accepted.Errors));
    }

    [Fact]
    public void A_small_client_refuses_the_whole_server_buff_set()
    {
        var ids = Enumerable.Range(0, 4).Select(_ => Guid.NewGuid()).ToArray();
        var document = new JsonObject { ["buffs"] = new JsonObject { ["add"] = new JsonArray(ids.Select(g => (JsonNode)new JsonObject { ["guid"] = g.ToString() }).ToArray()) } };
        var room = new CapacityInfo(3, 0, 0, 0, CapacityReadiness.Ready, false, CapacityScope.Process, false, null, 1);
        var reason = ContentSync.BuffRoomRefusal(document, room, new HashSet<Guid>());
        Assert.NotNull(reason);
        Assert.Contains("4 added buffs", reason);
        Assert.Contains("room for 3", reason);
        Assert.Null(ContentSync.BuffRoomRefusal(document, room with { Capacity = 4 }, new HashSet<Guid>()));
        Assert.Null(ContentSync.BuffRoomRefusal(document, room with { KilnUsed = 1, Capacity = 4 }, new HashSet<Guid> { ids[0] }));
        Assert.False(ContentSync.ApplySucceeded(0, new[] { "clone refused" }));
    }
}

public sealed class ContentAdmissionStartTests : FakeTableTest
{
    [Fact]
    public void An_absent_native_server_role_does_not_cache_a_noop_admission_registration()
    {
        Assert.True(ContentSync.ServerRole);
        Assert.Null(NetNative.ReadStatus());
        Swap(typeof(ContentSync), "_contentAdmission", null);
        var flags = System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic;
        typeof(ContentSync).GetMethod("EnsureContentAdmission", flags)!.Invoke(null, null);
        Assert.Null(typeof(ContentSync).GetField("_contentAdmission", flags)!.GetValue(null));
    }
}
