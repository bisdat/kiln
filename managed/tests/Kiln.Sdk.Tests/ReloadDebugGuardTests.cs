// SPDX-License-Identifier: MIT
// 0.2.0-dev.31 (12a-debugger): KilnDeploy with KilnReload=true is refused (KILN012) while kiln/state/debug/session.json says
// a debugger is attached, as `kiln reload` and `kiln deploy --reload` refuse (code refused, reason debugger_attached).
// Kiln.Mod.targets' guard target runs on a stub project (dotnet msbuild -t:_KilnReloadDebugGuard).
using Xunit;

namespace Kiln.SdkTests;

public sealed class ReloadDebugGuardTests
{
    private static string Targets => Path.Combine(AppContext.BaseDirectory, "Kiln.Mod.targets");

    /// <summary>The session file kiln_manage::debug::write_session writes (pretty JSON), in the given state.</summary>
    private static string Session(string state) => $$"""
        {
          "format": 1,
          "proxyPid": 4321,
          "proxyStarted": 1763650000,
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
            return StubMsBuild.Run(Path.Combine(dir, "stub.proj"), ["-t:_KilnReloadDebugGuard", .. props.Select(p => p.Replace("SERVER", server))]);
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
    public void ReloadIsRefusedWhileADebuggerIsAttached(string state)
    {
        var (code, output) = Guard(Session(state), "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.NotEqual(0, code);
        Assert.Contains("KILN012", output);
        Assert.Contains("session.json", output);
        // the same through KilnModsDir (<server>/kiln/mods)
        (code, output) = Guard(Session(state), "-p:KilnReload=true", "-p:KilnModsDir=SERVER/kiln/mods");
        Assert.NotEqual(0, code);
        Assert.Contains("KILN012", output);
    }

    [Theory]
    [InlineData("detached")]
    [InlineData("ended")]
    public void AnEndedSessionDoesNotBlockAReload(string state)
    {
        var (code, output) = Guard(Session(state), "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
    }

    [Fact]
    public void NoSessionOrNoReloadPasses()
    {
        var (code, output) = Guard(null, "-p:KilnReload=true", "-p:KilnServer=SERVER");
        Assert.True(code == 0, output);
        (code, output) = Guard(Session("paused"), "-p:KilnServer=SERVER");  // a deploy without a reload only copies files
        Assert.True(code == 0, output);
    }
}
