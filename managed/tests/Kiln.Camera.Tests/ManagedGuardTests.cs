// SPDX-License-Identifier: MIT
// Camera.End and Camera.Update are tick-thread calls as SetMode and Set are: a program wrap's handler runs inside the
// world's step (where an End could cancel a write its update holds), and a server lease is never ended off the tick
// thread. Nothing reaches the service then.
using Kiln;
using Kiln.TestKit;
using Xunit;
namespace CameraTests;
public sealed class ManagedGuardTests
{
    private static CameraLease LeaseOf(CameraEndpoint endpoint) =>
        new(42, endpoint, endpoint == CameraEndpoint.Local ? EntityId.None : new EntityId(5), 1);

    private static Task<CameraResult> Call(bool update, CameraLease lease) =>
        update ? Camera.Update(lease, new CameraOverride(Lens: new CameraLens(40))) : Camera.End(lease);

    [Theory]
    [InlineData(false, CameraEndpoint.Local)]
    [InlineData(true, CameraEndpoint.Local)]
    [InlineData(false, CameraEndpoint.ServerMode)]
    [InlineData(true, CameraEndpoint.ServerMode)]
    [InlineData(false, CameraEndpoint.Remote)]
    [InlineData(true, CameraEndpoint.Remote)]
    public void End_and_Update_reject_program_wrap_callbacks_even_on_the_tick_thread(bool update, CameraEndpoint endpoint)
    {
        using var kiln = new KilnHarness(); kiln.Step(2);
        kiln.Run(() =>
        {
            SliceGuard.Depth++;
            try
            {
                var e = Assert.Throws<InvalidOperationException>(() => { _ = Call(update, LeaseOf(endpoint)); });
                Assert.Contains("program wrap", e.Message);
            }
            finally { SliceGuard.Depth--; }
        });
        Assert.Empty(kiln.Camera.Calls);
    }

    [Theory]
    [InlineData(false, CameraEndpoint.ServerMode)]
    [InlineData(true, CameraEndpoint.ServerMode)]
    [InlineData(false, CameraEndpoint.Remote)]
    [InlineData(true, CameraEndpoint.Remote)]
    public void End_and_Update_of_a_server_lease_refuse_off_the_tick_thread(bool update, CameraEndpoint endpoint)
    {
        using var kiln = new KilnHarness(); kiln.Step(2);
        var e = Assert.Throws<InvalidOperationException>(() => { _ = Call(update, LeaseOf(endpoint)); });
        Assert.Contains("tick thread", e.Message);
        Assert.Empty(kiln.Camera.Calls);
    }
}
