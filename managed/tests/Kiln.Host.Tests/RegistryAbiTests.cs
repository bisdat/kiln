// SPDX-License-Identifier: MIT
#pragma warning disable KILNX031
using System.Runtime.InteropServices;
using Kiln;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class RegistryAbiTests
{
    [Fact]
    public void ManagedMirrorsMatchTheFixedNativeTables()
    {
        Assert.Equal(88, Marshal.SizeOf<RegistryStageTable>());
        Assert.Equal(56, Marshal.SizeOf<RegistryLiveTable>());
        Assert.Equal(40, Marshal.SizeOf<RegistryStageStatus>());
        Assert.Equal(24, Marshal.SizeOf<RegistryStageInfo>());
        Assert.Equal(24, Marshal.SizeOf<RegistryReadyInfo>());
        Assert.Equal(16, Marshal.SizeOf<RegistrySubmitInfo>());
        Assert.Equal(32, Marshal.SizeOf<RegistryLiveStatus>());
        Assert.Equal(232, Marshal.SizeOf<RegistryLiveReply>());
        Assert.Equal(40, (int)Marshal.OffsetOf<RegistryLiveReply>(nameof(RegistryLiveReply.Reason)));
        Assert.Equal(48, (int)Marshal.OffsetOf<RegistryLiveTable>(nameof(RegistryLiveTable.Status)));
        Assert.Equal(80, (int)Marshal.OffsetOf<RegistryStageTable>(nameof(RegistryStageTable.Unsubscribe)));
    }
    [Theory]
    [InlineData(RegistryEntryPoint.ModLoad, 1u)]
    [InlineData(RegistryEntryPoint.WorldLoad, 2u)]
    [InlineData(RegistryEntryPoint.HotReload, 3u)]
    [InlineData(RegistryEntryPoint.Manual, 4u)]
    public void EveryLaterPointUsesItsNativeTag(RegistryEntryPoint point, uint tag) => Assert.Equal(tag, RegistryNative.Tag(point));
}
