// SPDX-License-Identifier: MIT
// The one way these tests run `dotnet msbuild` on a stub project. Shared by the bake tests and the manifest tests: each had
// its own copy that removed every MSBuild* variable (so the child does not inherit the test host's toolset paths), which also
// removed MSBUILDDISABLENODEREUSE. The child then went looking for reusable MSBuild nodes (a busy PC keeps some alive for
// days) and left new ones behind, and a fixed wait of two or three minutes ended in "Process must exit before requested
// information can be determined" from ExitCode, with nothing of the build's output.
using System.Diagnostics;
using System.Globalization;

namespace Kiln.SdkTests;

internal static class StubMsBuild
{
    /// <summary>How long one stub build may take before it is stopped. Generous: a stub build takes seconds, but the
    /// suite runs at the lowest priority beside other builds.</summary>
    public static readonly TimeSpan Limit = TimeSpan.FromMinutes(10);

    /// <summary>Runs <c>dotnet msbuild <paramref name="project"/> -nologo -v:m</c> with <paramref name="args"/>, in one
    /// process (<c>-m:1</c>), without node reuse. Returns the exit code and stdout followed by stderr. A build still
    /// running after <see cref="Limit"/> is killed with its process tree and the test fails with what it printed.</summary>
    public static (int Code, string Output) Run(string project, params string[] args)
    {
        var psi = new ProcessStartInfo("dotnet") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        foreach (var a in new[] { "msbuild", project, "-nologo", "-v:m", "-nodeReuse:false", "-m:1" }) psi.ArgumentList.Add(a);
        foreach (var a in args) psi.ArgumentList.Add(a);
        // not the test host's toolset (MSBuildExtensionsPath, MSBuildSDKsPath, MSBUILD_EXE_PATH ...): the child finds its own
        foreach (var k in psi.Environment.Keys.Where(k => k.StartsWith("MSBuild", StringComparison.OrdinalIgnoreCase)).ToList()) psi.Environment.Remove(k);
        psi.Environment["MSBUILDDISABLENODEREUSE"] = "1";
        var sw = Stopwatch.StartNew();
        using var p = Process.Start(psi)!;
        var o = p.StandardOutput.ReadToEndAsync();
        var e = p.StandardError.ReadToEndAsync();
        if (!p.WaitForExit(Limit))
        {
            try { p.Kill(entireProcessTree: true); }
            catch (InvalidOperationException) { /* it ended just now */ }
            p.WaitForExit(TimeSpan.FromSeconds(30));
            Task.WaitAll([o, e], TimeSpan.FromSeconds(10));
            throw new TimeoutException(
                $"dotnet msbuild {project} {string.Join(' ', args)} was still running after {sw.Elapsed.TotalSeconds.ToString("0", CultureInfo.InvariantCulture)} s " +
                $"and was killed. Its output:\n{Text(o)}{Text(e)}");
        }
        p.WaitForExit();  // the output pipes are drained too
        return (p.ExitCode, o.Result + e.Result);
    }

    private static string Text(Task<string> read) => read.IsCompletedSuccessfully ? read.Result : "";
}
