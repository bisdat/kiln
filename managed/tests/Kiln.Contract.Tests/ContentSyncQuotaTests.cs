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
    public void The_content_hold_deadline_is_accepted_by_the_admission_controller() =>
        Assert.InRange(ContentSync.ContentAdmissionDeadline, TimeSpan.FromTicks(1), NetAdmission.MaxDeadline);

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

// The native serializer's full-room and omission bounds are tested by apply_report_test.cpp. This exercises its
// summary through the real managed ApplyJson consumer and the acknowledgement used by the sync send path.
public sealed unsafe class ContentSyncFullRoomTests : FakeTableTest
{
    private static int _mode, _received, _calls;
    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static int Apply(ulong owner, NativeStr document, NativeStr source, byte* report, nuint cap, nuint* len)
    {
        _calls++;
        _received=JsonNode.Parse(document.ToString())!["buffs"]!["add"]!.AsArray().Count;
        var text = _mode == 2 ? "{invalid" : System.Text.Json.JsonSerializer.Serialize(new {
            applied = _mode == 1 ? 4095 : 4096,
            errors = _mode == 1 ? new[] { "CapsBuff_4095: CapacityExceeded: no added-buff room" } : Array.Empty<string>(),
            warnings = Enumerable.Repeat("clone unchanged",8).ToArray(),errorCount = _mode == 1 ? 1 : 0,
            warningCount = 4096,conflictCount = 0,more = 4088 });
        var bytes=System.Text.Encoding.UTF8.GetBytes(text);
        bytes.AsSpan(0,Math.Min(bytes.Length,(int)cap-1)).CopyTo(new Span<byte>(report,(int)cap));
        *len=(nuint)bytes.Length;return 0;
    }
    [Theory]
    [InlineData(0)] [InlineData(1)] [InlineData(2)]
    public void A_full_room_acknowledges_only_complete_success_and_preserves_the_refusal(int mode)
    {
        var table=Alloc<ContentNative.Table>();
        table->Header=new ServiceHeader {Size=(uint)sizeof(ContentNative.Table),Version=4};table->ApplyJson=&Apply;
        Swap(typeof(Content),"_table",Box(table));
        var adds=new JsonArray(Enumerable.Range(0,4096).Select(i=>(JsonNode)new JsonObject { ["guid"]=Guid.NewGuid().ToString(),["name"]=$"CapsBuff_{i:D4}" }).ToArray());
        var document=new JsonObject {["buffs"]=new JsonObject {["add"]=adds}}.ToJsonString();
        _mode=mode;_calls=0;
        var r=ContentSync.ApplyJson(document,"full-room test");
        var ack=ContentSync.MakeApplyAck(123,r.Rc,r.Applied,0,r.Errors,1);
        Assert.Equal(4096,_received);Assert.Equal(1,_calls); // no size-query re-application
        Assert.Equal(mode==0?(byte)0:(byte)1,ack.Result);
        Assert.Equal(123u,ack.StateId);
        if(mode==0) {Assert.Equal(4096u,ack.Applied);Assert.Equal(0u,ack.Errors);}
        if(mode==1) Assert.Contains("CapsBuff_4095: CapacityExceeded",ack.Detail);
        if(mode==2) Assert.Contains("incomplete or invalid",ack.Detail);
    }
}
