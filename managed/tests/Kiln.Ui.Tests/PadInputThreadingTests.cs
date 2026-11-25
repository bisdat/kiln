// SPDX-License-Identifier: MIT
// PadInput's members say "Any thread" (0.2.0-dev.31.13, mod API 0.2.22): the class carries [AnyThread] as Crosshair,
// ClientStations and ClientSkills do, so the thread analyzers and a mod's Kiln task worker treat it as such (review 31.13).
using Xunit;

namespace Kiln;

public sealed class PadInputThreadingTests
{
    [Fact]
    public void PadInput_is_any_thread_and_client_only()
    {
        Assert.True(Attribute.IsDefined(typeof(PadInput), typeof(AnyThreadAttribute)));
        Assert.True(Attribute.IsDefined(typeof(PadInput), typeof(ClientOnlyAttribute)));
    }
}
