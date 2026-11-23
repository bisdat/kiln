// SPDX-License-Identifier: MIT
// In-game tests for Kiln mods (Forge's GameTest framework). A test is a method marked [KilnTest] in a mod's
// assembly: static or instance (on the Mod subclass itself the running mod instance is used; other classes need a
// parameterless constructor), returning void, Task or ValueTask, taking nothing or a TestContext. Tests run one after
// another on the server's tick thread, so they may touch game state and await ticks. Docs: docs/kiln/modding/testing.md.
namespace Kiln.Testing;

/// <summary>Marks a test method.</summary>
[AttributeUsage(AttributeTargets.Method, AllowMultiple = false)]
public sealed class KilnTestAttribute : Attribute
{
    /// <summary>Marks a method as a test. <paramref name="name"/> is its display name; null uses
    /// <c>Class.Method</c>.</summary>
    public KilnTestAttribute(string? name = null) => Name = name;

    /// <summary>Display name (default <c>Class.Method</c>).</summary>
    public string? Name { get; }

    /// <summary>Per-test timeout in seconds (0 = the mod's <c>tests.timeoutSeconds</c>, else 30 s). A test that
    /// blocks the tick thread synchronously cannot be interrupted: only awaits are timed.</summary>
    public int TimeoutSeconds { get; init; }

    /// <summary>When set, the test is reported as skipped with this reason.</summary>
    public string? Skip { get; init; }

    /// <summary>Lower runs first; equal orders run in declaration order.</summary>
    public int Order { get; init; }
}

/// <summary>What a running test gets (optional parameter of a test method).</summary>
public sealed class TestContext
{
    internal TestContext(string name, ModContext mod, CancellationToken cancellation)
    {
        Name = name;
        Mod = mod;
        Cancellation = cancellation;
    }

    /// <summary>The test's display name.</summary>
    public string Name { get; }

    /// <summary>The context of the mod under test.</summary>
    public ModContext Mod { get; }

    /// <summary>The log of the mod under test.</summary>
    public Logger Log => Mod.Log;

    /// <summary>Signalled when the test timed out; awaits on this context then throw.</summary>
    public CancellationToken Cancellation { get; }

    /// <summary>Continues on the tick thread after <paramref name="count"/> server-world steps.</summary>
    public async Task Ticks(int count = 1)
    {
        for (var i = 0; i < count; i++)
        {
            Cancellation.ThrowIfCancellationRequested();
            await Mod.Scheduler.NextTick();
        }
        Cancellation.ThrowIfCancellationRequested();
    }

    /// <summary>Checks <paramref name="condition"/> once per step until it holds; fails the test after
    /// <paramref name="timeout"/>.</summary>
    public async Task Until(Func<bool> condition, TimeSpan timeout, string? what = null)
    {
        var deadline = DateTime.UtcNow + timeout;
        while (!condition())
        {
            if (DateTime.UtcNow > deadline)
                throw new AssertionException($"timed out after {timeout.TotalSeconds:0.#} s waiting for {what ?? "the condition"}");
            await Ticks();
        }
    }

    /// <summary>Waits wall time, coming back on the tick thread.</summary>
    public async Task Delay(TimeSpan time)
    {
        var until = DateTime.UtcNow + time;
        while (DateTime.UtcNow < until) await Ticks();
    }
}

/// <summary>A failed check.</summary>
public sealed class AssertionException(string message) : Exception(message);

/// <summary>Thrown by <see cref="Assert.Skip"/>.</summary>
public sealed class SkipException(string reason) : Exception(reason);

/// <summary>Checks for test methods. Each throws <see cref="AssertionException"/> on failure.</summary>
public static class Assert
{
    /// <summary>Fails when <paramref name="condition"/> is false, with <paramref name="message"/> or "expected
    /// true".</summary>
    public static void True(bool condition, string? message = null)
    {
        if (!condition) Fail(message ?? "expected true");
    }

    /// <summary>Fails when <paramref name="condition"/> is true, with <paramref name="message"/> or "expected
    /// false".</summary>
    public static void False(bool condition, string? message = null)
    {
        if (condition) Fail(message ?? "expected false");
    }

    /// <summary>Fails unless <paramref name="actual"/> equals <paramref name="expected"/> (the default equality of
    /// <typeparamref name="T"/>); the failure names both values.</summary>
    public static void Equal<T>(T expected, T actual, string? message = null)
    {
        if (!EqualityComparer<T>.Default.Equals(expected, actual))
            Fail($"{(message is null ? "" : message + ": ")}expected <{expected}>, got <{actual}>");
    }

    /// <summary>Fails when <paramref name="actual"/> equals <paramref name="unexpected"/> (the default equality of
    /// <typeparamref name="T"/>).</summary>
    public static void NotEqual<T>(T unexpected, T actual, string? message = null)
    {
        if (EqualityComparer<T>.Default.Equals(unexpected, actual))
            Fail($"{(message is null ? "" : message + ": ")}did not expect <{actual}>");
    }

    /// <summary>Fails unless <paramref name="value"/> is null.</summary>
    public static void Null(object? value, string? message = null)
    {
        if (value is not null) Fail(message ?? $"expected null, got <{value}>");
    }

    /// <summary>Fails when <paramref name="value"/> is null; otherwise returns it, so a test can go on with the
    /// non-null value.</summary>
    public static T NotNull<T>(T? value, string? message = null) where T : class =>
        value ?? throw new AssertionException(message ?? "expected a value, got null");

    /// <summary>Fails unless <paramref name="actual"/> is the very same instance as <paramref name="expected"/>
    /// (reference equality).</summary>
    public static void Same(object? expected, object? actual, string? message = null)
    {
        if (!ReferenceEquals(expected, actual)) Fail(message ?? $"expected the same instance as <{expected}>, got <{actual}>");
    }

    /// <summary>Fails unless <paramref name="actual"/> contains the text <paramref name="expected"/> (ordinal,
    /// case-sensitive). A null <paramref name="actual"/> fails.</summary>
    public static void Contains(string expected, string? actual, string? message = null)
    {
        if (actual is null || !actual.Contains(expected, StringComparison.Ordinal))
            Fail($"{(message is null ? "" : message + ": ")}expected text containing <{expected}>, got <{actual}>");
    }

    /// <summary>Fails unless <paramref name="actual"/> is between <paramref name="low"/> and <paramref name="high"/>,
    /// both included.</summary>
    public static void InRange(double actual, double low, double high, string? message = null)
    {
        if (actual < low || actual > high) Fail($"{(message is null ? "" : message + ": ")}expected {low}..{high}, got {actual}");
    }

    /// <summary>Runs <paramref name="action"/> and returns the <typeparamref name="TException"/> it throws (or a type
    /// derived from it). Fails when it throws another exception or none.</summary>
    public static TException Throws<TException>(Action action, string? message = null) where TException : Exception
    {
        try
        {
            action();
        }
        catch (TException e)
        {
            return e;
        }
        catch (Exception e)
        {
            Fail($"{(message is null ? "" : message + ": ")}expected {typeof(TException).Name}, got {e.GetType().Name}: {e.Message}");
        }
        throw new AssertionException($"{(message is null ? "" : message + ": ")}expected {typeof(TException).Name}, nothing was thrown");
    }

    /// <summary>Awaits <paramref name="action"/> and returns the <typeparamref name="TException"/> it throws (or a
    /// type derived from it). Fails when it throws another exception or none.</summary>
    public static async Task<TException> ThrowsAsync<TException>(Func<Task> action, string? message = null) where TException : Exception
    {
        try
        {
            await action();
        }
        catch (TException e)
        {
            return e;
        }
        catch (Exception e)
        {
            Fail($"{(message is null ? "" : message + ": ")}expected {typeof(TException).Name}, got {e.GetType().Name}: {e.Message}");
        }
        throw new AssertionException($"{(message is null ? "" : message + ": ")}expected {typeof(TException).Name}, nothing was thrown");
    }

    /// <summary>Fails the test with <paramref name="message"/>.</summary>
    [System.Diagnostics.CodeAnalysis.DoesNotReturn]
    public static void Fail(string message) => throw new AssertionException(message);

    /// <summary>Ends the test as skipped (e.g. a feature is unavailable on this build).</summary>
    [System.Diagnostics.CodeAnalysis.DoesNotReturn]
    public static void Skip(string reason) => throw new SkipException(reason);
}

/// <summary>Outcome of one test.</summary>
public enum TestOutcome
{
    /// <summary>The test ran and no check failed.</summary>
    Pass,
    /// <summary>A check failed, the test threw, or its test method could not be run (the message says why).</summary>
    Fail,
    /// <summary>The test was skipped: marked with a skip reason, or ended with <see cref="Assert.Skip"/>.</summary>
    Skip,
    /// <summary>The test did not finish within its timeout.</summary>
    Timeout,
}

/// <summary>Outcome of one test.</summary>
/// <param name="Name">The test's display name.</param>
/// <param name="Outcome">How it ended.</param>
/// <param name="Ms">How long it ran, in milliseconds.</param>
/// <param name="Message">The failure message or the skip reason; null for a pass.</param>
/// <param name="Stack">The stack trace of a failure, with the <c>Kiln.Testing.Assert</c> frames left out for a failed
/// check; null when there is none.</param>
public sealed record TestResult(string Name, TestOutcome Outcome, double Ms, string? Message, string? Stack);

/// <summary>Results of one run (also written to <c>kiln/logs/tests/&lt;mod&gt;.json</c>).</summary>
/// <param name="Mod">The id of the mod that was tested.</param>
/// <param name="Version">The mod's version, from its manifest.</param>
/// <param name="Started">When the run started.</param>
/// <param name="Ms">How long the whole run took, in milliseconds.</param>
/// <param name="Results">One result per test, in the order they ran.</param>
public sealed record TestReport(string Mod, string Version, DateTime Started, double Ms, IReadOnlyList<TestResult> Results)
{
    /// <summary>How many tests passed.</summary>
    public int Passed => Results.Count(r => r.Outcome == TestOutcome.Pass);
    /// <summary>How many tests failed or timed out.</summary>
    public int Failed => Results.Count(r => r.Outcome is TestOutcome.Fail or TestOutcome.Timeout);
    /// <summary>How many tests were skipped.</summary>
    public int Skipped => Results.Count(r => r.Outcome == TestOutcome.Skip);
    /// <summary>True when no test failed or timed out (skipped tests do not count against it).</summary>
    public bool Ok => Failed == 0;

    /// <summary>Where the JSON report was written, if anywhere.</summary>
    public string? File { get; init; }

    /// <summary>A one-line summary: the mod, how many tests passed, failed and were skipped, and how long the run
    /// took.</summary>
    public override string ToString() => $"{Mod}: {Passed} passed, {Failed} failed, {Skipped} skipped in {Ms / 1000:F2} s";
}

/// <summary>Runs a loaded mod's tests from code (a chat command, a dashboard op). The control folder does the same:
/// <c>test &lt;mod&gt; [filter]</c>.</summary>
public static class Tests
{
    /// <summary>Set by Kiln.Host: starts a run on the tick thread and completes with its report.</summary>
    internal static Func<string, string?, Task<TestReport>>? Starter;

    /// <summary>Runs the <see cref="KilnTestAttribute"/> methods of <paramref name="modId"/> (those whose name contains
    /// <paramref name="filter"/>), starting at the next step. Completes on the caller's context (the tick thread for
    /// mod code) with the report; faults when the mod is not loaded or already testing.</summary>
    public static Task<TestReport> RunAsync(string modId, string? filter = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(modId);
        return Starter?.Invoke(modId, filter) ?? Task.FromException<TestReport>(new InvalidOperationException("Kiln.Host is not running"));
    }
}
