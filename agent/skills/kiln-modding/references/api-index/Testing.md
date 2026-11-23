# Testing (Kiln.Testing)

## Testing.Assert — Checks for test methods.
- `static Contains(string expected, string? actual, string? message = null) -> void` — Fails unless `actual` contains the text `expected` (ordinal, case-sensitive).
- `static Equal<T>(T expected, T actual, string? message = null) -> void` — Fails unless `actual` equals `expected` (the default equality of `T`); the failure names both values.
- `static Fail(string message) -> void` — Fails the test with `message`.
- `static False(bool condition, string? message = null) -> void` — Fails when `condition` is true, with `message` or "expected false".
- `static InRange(double actual, double low, double high, string? message = null) -> void` — Fails unless `actual` is between `low` and `high`, both included.
- `static NotEqual<T>(T unexpected, T actual, string? message = null) -> void` — Fails when `actual` equals `unexpected` (the default equality of `T`).
- `static NotNull<T>(T? value, string? message = null) -> T` — Fails when `value` is null; otherwise returns it, so a test can go on with the non-null value.
- `static Null(object? value, string? message = null) -> void` — Fails unless `value` is null.
- `static Same(object? expected, object? actual, string? message = null) -> void` — Fails unless `actual` is the very same instance as `expected` (reference equality).
- `static Skip(string reason) -> void` — Ends the test as skipped (e.g. a feature is unavailable on this build).
- `static Throws<TException>(Action action, string? message = null) -> TException` — Runs `action` and returns the `TException` it throws (or a type derived from it).
- `static ThrowsAsync<TException>(Func<Task> action, string? message = null) -> Task<TException>` — Awaits `action` and returns the `TException` it throws (or a type derived from it).
- `static True(bool condition, string? message = null) -> void` — Fails when `condition` is false, with `message` or "expected true".

## Testing.AssertionException — A failed check.
- `AssertionException(string message)`

## Testing.CheckContext — What a running check gets.
- `Cancellation { get; } -> CancellationToken` — Signalled when the check ends early: the player stopped the run, gave a verdict before the body asked for one, or...
- `Delay(TimeSpan time) -> Task` — Waits wall time, coming back on the tick thread.
- `Expect(bool condition, string what, string? detail = null) -> bool` — Records an automatic assertion next to the player's verdict.
- `Id { get; } -> string` — The check's id.
- `Instruct(string text) -> void` — Shows `text` as the current instruction: in the list and on the HUD line, until the next instruction or the end of...
- `Log { get; } -> Logger` — The mod's log (`kiln/logs/kiln.log`).
- `Mod { get; } -> ModContext` — The context of the mod the check belongs to.
- `OnEnd(Action cleanup) -> void` — Registers `cleanup` to run when the check ends, however it ends (verdict, stop, exception, unload): on the tick...
- `OnEnd(Func<Task> cleanup) -> void` — Registers `cleanup` to run when the check ends, however it ends (verdict, stop, exception, unload): on the tick...
- `Player { get; } -> EntityId` — The entity of the player who runs the check: the local player when hosted, the joining player's entity on the server...
- `Session { get; } -> Testing.ClientCheckSession` — Where the check runs.
- `Skip(string reason) -> Task<Testing.ClientCheckVerdict>` `0.2.3` — Ends the check with the verdict skip and `reason` as its note, without asking the player: for a check that can judge...
- `Ticks(int count = 1) -> Task` — Continues on the tick thread after `count` server-world steps.
- `Title { get; } -> string` — The check's title.
- `Until(Func<bool> condition, TimeSpan timeout, string what) -> Task<bool>` — Checks `condition` once per step until it holds or `timeout` passes.
- `Verdict() -> Task<Testing.ClientCheckVerdict>` — Waits for the player's verdict: pass, fail (with a one-line note) or skip.

## Testing.ClientCheckAttribute — Marks a guided client check: an async method of a mod taking one `CheckContext` and returning `Task` or `ValueTask`.
- `ClientCheckAttribute(string id, string title)` — Marks an async method `Task M(CheckContext c)` as a guided check with this id and title.
- `Area { get; init; } -> string?` — The group the list shows the check under (default: the mod's name).
- `Confirm { get; init; } -> string?` — A sentence the runner shows before the body runs; the body starts only after the player answers yes to it with its...
- `Id { get; } -> string` — The check's id, unique within the mod (`area.slug`).
- `Needs { get; init; } -> Testing.ClientCheckNeeds` — Which session the check needs; in another one it is refused with the reason shown.
- `Order { get; init; } -> int` — Lower runs first in "run all"; equal orders keep the mod's load order, then class, then declaration order.
- `Since { get; init; } -> string?` — The Kiln or mod release that added what the check covers, e.g. `0.2.0-dev.26`.
- `Tags { get; init; } -> string[]` — Tags: the names in `ClientCheckTags`, or free ones for grouping.
- `Title { get; } -> string` — What the check list shows.

## Testing.ClientCheckNeeds — The session a check needs (`ClientCheckAttribute.Needs`).
enum: Any, Hosted, Joined

## Testing.ClientChecks — Registers a mod's guided checks with the runner.
- `static Register(Mod mod) -> int` — Lists the `ClientCheckAttribute` methods of `mod`'s assemblies with the runner.

## Testing.ClientCheckSession — Where a check runs, as written into its ledger entry.
enum: Hosted, Joined, Server

## Testing.ClientCheckTags — Tag names of `ClientCheckAttribute.Tags`.
- `const Audio = "audio" -> string` — The player judges something they hear.
- `const Cheats = "cheats" -> string` — The check uses level, experience, skill-point or knowledge commands (or anything else that changes what the game may...
- `const GuardProof = "guard-proof" -> string` — The check that shows the achievement guard working on this game build and Kiln version.
- `const Launcher = "launcher" -> string` — Done (partly) in the Kiln launcher, outside the game.
- `const SteamDeck = "steam-deck" -> string` — Needs a Steam Deck.
- `const TwoPlayers = "two-players" -> string` — Needs a second player.
- `const Visual = "visual" -> string` — The player judges something they see.
- `const Windows = "windows" -> string` — Needs a Windows PC.

## Testing.ClientCheckVerdict — The player's verdict on a check.
enum: Pass, Fail, Skip

## Testing.KilnTestAttribute — Marks a test method.
- `KilnTestAttribute(string? name = null)` — Marks a method as a test.
- `Name { get; } -> string?` — Display name (default `Class.Method`).
- `Order { get; init; } -> int` — Lower runs first; equal orders run in declaration order.
- `Skip { get; init; } -> string?` — When set, the test is reported as skipped with this reason.
- `TimeoutSeconds { get; init; } -> int` — Per-test timeout in seconds (0 = the mod's `tests.timeoutSeconds`, else 30 s).

## Testing.SkipException — Thrown by `Assert.Skip`.
- `SkipException(string reason)`

## Testing.TestContext — What a running test gets (optional parameter of a test method).
- `Cancellation { get; } -> CancellationToken` — Signalled when the test timed out; awaits on this context then throw.
- `Delay(TimeSpan time) -> Task` — Waits wall time, coming back on the tick thread.
- `Log { get; } -> Logger` — The log of the mod under test.
- `Mod { get; } -> ModContext` — The context of the mod under test.
- `Name { get; } -> string` — The test's display name.
- `Ticks(int count = 1) -> Task` — Continues on the tick thread after `count` server-world steps.
- `Until(Func<bool> condition, TimeSpan timeout, string? what = null) -> Task` — Checks `condition` once per step until it holds; fails the test after `timeout`.

## Testing.TestOutcome — Outcome of one test.
enum: Pass, Fail, Skip, Timeout

## Testing.TestReport — Results of one run (also written to `kiln/logs/tests/<mod>.json`).
- `TestReport(string Mod, string Version, DateTime Started, double Ms, IReadOnlyList<Testing.TestResult> Results)`
- `Failed { get; } -> int` — How many tests failed or timed out.
- `File { get; init; } -> string?` — Where the JSON report was written, if anywhere.
- `Mod { get; init; } -> string` — The id of the mod that was tested.
- `Ms { get; init; } -> double` — How long the whole run took, in milliseconds.
- `Ok { get; } -> bool` — True when no test failed or timed out (skipped tests do not count against it).
- `Passed { get; } -> int` — How many tests passed.
- `Results { get; init; } -> IReadOnlyList<Testing.TestResult>` — One result per test, in the order they ran.
- `Skipped { get; } -> int` — How many tests were skipped.
- `Started { get; init; } -> DateTime` — When the run started.
- `Version { get; init; } -> string` — The mod's version, from its manifest.

## Testing.TestResult — Outcome of one test.
- `TestResult(string Name, Testing.TestOutcome Outcome, double Ms, string? Message, string? Stack)`
- `Message { get; init; } -> string?` — The failure message or the skip reason; null for a pass.
- `Ms { get; init; } -> double` — How long it ran, in milliseconds.
- `Name { get; init; } -> string` — The test's display name.
- `Outcome { get; init; } -> Testing.TestOutcome` — How it ended.
- `Stack { get; init; } -> string?` — The stack trace of a failure, with the `Kiln.Testing.Assert` frames left out for a failed check; null when there is...

## Testing.Tests — Runs a loaded mod's tests from code (a chat command, a dashboard op).
- `static RunAsync(string modId, string? filter = null) -> Task<Testing.TestReport>` — Runs the `KilnTestAttribute` methods of `modId` (those whose name contains `filter`), starting at the next step.
