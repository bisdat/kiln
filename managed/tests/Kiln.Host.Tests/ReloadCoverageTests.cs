// SPDX-License-Identifier: MIT
// What `reload <id>` covers (docs/kiln/architecture.md "What reload <id> covers"): what it cannot change
// is refused with the reason first rather than half-applied.
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

public sealed class ReloadCoverageTests : ModsFixture
{
    // `reload <id>` of a native mod (native.json: kiln-core's DLLs, never unloaded) said "mod <id> is not loaded"; it now says why.
    [Fact]
    public void AReloadOfANativeModIsRefusedAsRestartRequired()
    {
        using var manager = NewManager(new FakeHost());
        var drop = Directory.CreateDirectory(Path.Combine(Root, "native", "fps-unlock")).FullName;
        File.WriteAllText(Path.Combine(drop, "native.json"), """{"format": 1, "id": "fps-unlock", "version": "1.0.0", "dlls": ["a.dll"]}""");
        Directory.CreateDirectory(Path.Combine(Root, "state"));
        File.WriteAllText(Path.Combine(Root, "state", "native.json"),
            """{"format": 1, "pid": 1, "written": "2026-11-25T20:00:00Z", "policy": "always", "mods": [{"id": "creative-mode"}]}""");
        var r = manager.ReloadCore("fps-unlock");
        Assert.False(r.Ok);
        Assert.StartsWith("RestartRequired: fps-unlock is a native mod (native.json)", r.Message);
        Assert.StartsWith("RestartRequired: creative-mode is a native mod", manager.ReloadCore("creative-mode").Message);  // a profile's, from kiln-core's state
        Assert.Equal("mod nothing-here is not loaded", manager.ReloadCore("nothing-here").Message);
    }
}
