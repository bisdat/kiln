// SPDX-License-Identifier: MIT
// 0.2.0-dev.31 (12a-debugger): KilnDeploy with KilnReload=true is refused (KILN012) while kiln/state/debug/session.json says
// a debugger is attached and the kiln debug that wrote it still runs (its PID and start, as kiln reload checks); a file
// left by a kiln debug that ended is a warning. Kiln.Mod.targets' guard target runs on a stub project
// (dotnet msbuild -t:_KilnReloadDebugGuard) with the built Kiln.Sdk.Tasks.dll.
using System.Diagnostics;
using Kiln.Sdk;
using Xunit;

namespace Kiln.SdkTests;

public sealed class ReloadDebugGuardTests
{
    private static string Targets => Path.Combine(AppContext.BaseDirectory, "Kiln.Mod.targets");

    private static long MyStart()
    {
        using var me = Process.GetCurrentProcess();
        return new DateTimeOffset(me.StartTime.ToUniversalTime()).ToUnixTimeSeconds();
    }

    /// <summary>A PID that ran and ended (a process of ours, waited for).</summary>
    private static int GonePid()
    {
        using var p = Process.Start(new ProcessStartInfo("dotnet", "--version") { RedirectStandardOutput = true, UseShellExecute = false })!;
        p.StandardOutput.ReadToEnd();
        p.WaitForExit();
        return p.Id;
    }

    /// <summary>The session file kiln_manage::debug::write_session writes (pretty JSON), in the given state.</summary>
    private static string Session(string state, int pid, long? started) => $$"""
        {
          "format": 1,
          "proxyPid": {{pid}},
          "proxyStarted": {{(started is null ? "null" : started.ToString())}},
          "state": {
            "identity": { "installId": "i", "windowsPid": 32 },
            "state": "{{state}}",
            "sessionMode": "hosted",
            "maxPauseMs": null
          }
        }
        """;

    private static (int Code, string Output) Guard(string? session, params string[] props)
    {
        var dir = Directory.CreateTempSubdirectory("kiln-reload-guard-").FullName;
        try
        {
            var server = Path.Combine(dir, "server");
            Directory.CreateDirectory(Path.Combine(server, "kiln", "mods"));
            if (session != null)
            {
                Directory.CreateDirectory(Path.Combine(server, "kiln", "state", "debug"));
                File.WriteAllText(Path.Combine(server, "kiln", "state", "debug", "session.json"), session);
            }
            File.WriteAllText(Path.Combine(dir, "stub.proj"), $"""
                <Project>
                  <PropertyGroup>
                    <AssemblyName>Stub</AssemblyName>
                    <KilnRepoMode>true</KilnRepoMode>
                    <KilnRepoRoot>{dir}/</KilnRepoRoot>
                    <OutDir>{dir}/out/</OutDir>
                    <IntermediateOutputPath>{dir}/obj/</IntermediateOutputPath>
                    <KilnModApiVersion>0.2</KilnModApiVersion>
                  </PropertyGroup>
                  <Import Project="{Targets}" />
                </Project>
                """);
            string[] args = ["-t:_KilnReloadDebugGuard", $"-p:_KilnTasksDir={AppContext.BaseDirectory}", .. props.Select(p => p.Replace("SERVER", server))];
            return StubMsBuild.Run(Path.Combine(dir, "stub.proj"), args);
        }
        finally
        {
            Directory.Delete(dir, true);
        }
    }

    [Theory]
    [InlineData("attaching")]
    [InlineData("running")]
    [InlineData("paused")]
    [InlineData("detaching")]
    public void ReloadIsRefusedWhileALiveKilnDebugIsAttached(string state)
    {
        // this test process stands in for the kiln debug that wrote the file
        var session = Session(state, Environment.ProcessId, MyStart());
        var (code, output) = Guard(session, "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.NotEqual(0, code);
        Assert.Contains("KILN012", output);
        Assert.Contains($"pid {Environment.ProcessId}, {state}", output);
        (code, output) = Guard(session, "-p:KilnReload=true", "-p:KilnModsDir=SERVER/kiln/mods");
        Assert.NotEqual(0, code);
        Assert.Contains("KILN012", output);
    }

    [Fact]
    public void AFileLeftByAKilnDebugThatEndedWarnsAndTheReloadGoesAhead()
    {
        var (code, output) = Guard(Session("paused", GonePid(), 1_700_000_000), "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
        Assert.Contains("warning KILN012", output);
        // the same PID now another process (another start): not the writer either
        (code, output) = Guard(Session("paused", Environment.ProcessId, MyStart() - 3600), "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
    }

    [Theory]
    [InlineData("detached")]
    [InlineData("ended")]
    public void AnEndedSessionDoesNotBlockAReload(string state)
    {
        var (code, output) = Guard(Session(state, Environment.ProcessId, MyStart()), "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
        Assert.DoesNotContain("KILN012", output);
    }

    [Fact]
    public void NoSessionOrNoReloadPasses()
    {
        var (code, output) = Guard(null, "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
        (code, output) = Guard(Session("paused", Environment.ProcessId, MyStart()), "-p:KilnServer=SERVER");  // no reload: files only
        Assert.True(code == 0, output);
    }

    [Fact]
    public void CheckReadsTheStateAndTheWriter()
    {
        long? Alive(int _) => 1000;
        Assert.Equal(("paused", 7, true), KilnDebugSession.Check(Session("paused", 7, 1001), Alive));
        Assert.Equal(("paused", 7, true), KilnDebugSession.Check(Session("paused", 7, null), Alive));
        Assert.Equal(("paused", 7, false), KilnDebugSession.Check(Session("paused", 7, 1000), _ => null));
        Assert.Equal(("paused", 7, false), KilnDebugSession.Check(Session("paused", 7, 5000), Alive));
        Assert.Equal(("ended", 7, false), KilnDebugSession.Check(Session("ended", 7, 1000), Alive));
        Assert.Equal((null, 0, false), KilnDebugSession.Check("not json", Alive));
    }
}
