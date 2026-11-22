// SPDX-License-Identifier: MIT
// 0.2.0-dev.31 (12a-debugger): while a debugger is attached the host refuses the control commands that unload mod code
// (unload, reload, reload-test), whoever wrote the command: kiln reload, the SDK's KilnReload, the VS Code extension.
using Xunit;

namespace Kiln.Host.Tests;

public sealed class ControlDebuggerTests
{
    [Theory]
    [InlineData("reload my-mod")]
    [InlineData("unload my-mod")]
    [InlineData("reload-test my-mod 20")]
    public void UnloadingCommandsAreRefusedWhileADebuggerIsAttached(string command)
    {
        var args = command.Split(' ');
        var refused = HostRuntime.RefusedWhileDebugging(args, debuggerAttached: true);
        Assert.NotNull(refused);
        Assert.Contains("debugger_attached", refused);
        Assert.Null(HostRuntime.RefusedWhileDebugging(args, debuggerAttached: false));
    }

    [Theory]
    [InlineData("mods")]
    [InlineData("gc")]
    [InlineData("load my-mod")]
    [InlineData("test my-mod")]
    public void OtherCommandsRunWhileADebuggerIsAttached(string command) =>
        Assert.Null(HostRuntime.RefusedWhileDebugging(command.Split(' '), debuggerAttached: true));
}
