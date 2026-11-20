// SPDX-License-Identifier: MIT
using Xunit;
namespace Kiln.Contract.Tests;
public sealed class GameplayAdminHookRowsTests
{
    [Theory]
    [InlineData(CapabilityState.Pending,"resolved, not installed: no mod asked","idle")]
    [InlineData(CapabilityState.Pending,"waiting for safe Pre","waiting")]
    [InlineData(CapabilityState.Available,"installed","installed")]
    [InlineData(CapabilityState.Unavailable,"refused: thread check","refused")]
    public void Admin_preserves_the_group_state_and_reason(CapabilityState state,string reason,string expected)
    {
        var rows=Admin.GameplayHookRows.Snapshot(name=>new Capability(name,state,CapabilityReason.Session,reason,"test"));
        foreach(var group in new[]{"build","terraform","spawn"}) {
            Assert.Equal(expected,rows[group]!["state"]!.GetValue<string>());
            Assert.Equal(reason,rows[group]!["reason"]!.GetValue<string>());
        }
    }
}
