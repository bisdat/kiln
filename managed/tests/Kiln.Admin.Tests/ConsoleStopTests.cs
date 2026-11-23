// SPDX-License-Identifier: MIT
// The stop a restart raises is the one a console's Ctrl-C raises: the process's console control handlers get
// CTRL_C_EVENT (Kiln.Host's first, then the game's, which saves and exits). Windows (and Wine) only.
using System.Runtime.InteropServices;
using Xunit;

namespace Kiln.Admin.Tests;

public sealed class ConsoleStopTests
{
    private delegate bool HandlerRoutine(uint ctrlType);

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool SetConsoleCtrlHandler(HandlerRoutine? handler, [MarshalAs(UnmanagedType.Bool)] bool add);

    [Fact]
    public void Raising_the_stop_runs_the_ctrl_c_handlers()
    {
        Assert.SkipUnless(OperatingSystem.IsWindows(), "the console control routine is Windows' (and Wine's)");
        var got = new List<uint>();
        using var seen = new ManualResetEventSlim(false);
        HandlerRoutine handler = type =>
        {
            lock (got) got.Add(type);
            seen.Set();
            return true;  // handled: the default handler (which would end this test process) never runs
        };
        Assert.True(SetConsoleCtrlHandler(handler, true));
        try
        {
            var how = ConsoleStop.Raise(1);
            Assert.NotNull(how);
            Assert.EndsWith("!CtrlRoutine", how);
            Assert.True(seen.Wait(5000), "no Ctrl-C handler ran");
            lock (got) Assert.Equal([0u], got);  // CTRL_C_EVENT
        }
        finally
        {
            SetConsoleCtrlHandler(handler, false);
            GC.KeepAlive(handler);
        }
    }

    [Fact]
    public void Off_windows_there_is_no_way_to_raise_it()
    {
        Assert.SkipWhen(OperatingSystem.IsWindows(), "Windows has the routine");
        Assert.Null(ConsoleStop.Raise(1));
        Assert.Null(ConsoleStop.Raise(2));
    }
}
