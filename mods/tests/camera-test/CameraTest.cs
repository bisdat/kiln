// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;
using Kiln;
using Kiln.Testing;
namespace CameraTest;
public sealed class CameraTestMod : Mod
{
    [DllImport("camera-fixture.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern int camera_fixture_result(uint index);
    public override void OnLoad() { }
    private static async Task Check(TestContext c, uint index, string name)
    {
        await c.Until(() => camera_fixture_result(index) != 0, TimeSpan.FromSeconds(90), name);
        Assert.Equal(1, camera_fixture_result(index), "native fixture " + name + "; see camera-fixture log");
        c.Log.Info("camera-test PASS " + name);
    }
    [KilnTest("maintained20", Order = 0, TimeoutSeconds = 120)] public Task Maintained(TestContext c) => Check(c, 0, "maintained20");
    [KilnTest("isolated_other_entity", Order = 1)] public Task Isolation(TestContext c) => Check(c, 1, "isolated_other_entity");
    [KilnTest("end_cancels_queued", Order = 2)] public Task Cancel(TestContext c) => Check(c, 2, "end_cancels_queued");
    [KilnTest("no_late_revision", Order = 3)] public Task Revision(TestContext c) => Check(c, 3, "no_late_revision");
    [KilnTest("events_once", Order = 4)] public Task Events(TestContext c) => Check(c, 4, "events_once");
    [KilnTest("pending_cancel", Order = 5)] public Task Pending(TestContext c) => Check(c, 5, "pending_cancel");
    [KilnTest("owner_epoch_incarnation", Order = 6)] public Task Epoch(TestContext c) => Check(c, 6, "owner_epoch_incarnation");
    [KilnTest("unchanged_scope", Order = 7)] public Task Unchanged(TestContext c) => Check(c, 7, "unchanged_scope");
}
