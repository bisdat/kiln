// SPDX-License-Identifier: MIT
// BlockUi's commands on the joined server (dossier 09 §4.5, §5.2; DESIGN.md §5): a line runs on the server this game joined, as
// this game's player there, through kiln:ui-command (Ui/UiCommandWire.cs). The server takes the player from the connection
// and runs Commands.Execute(CommandSource.ForPlayer(player), line) on its tick thread: the command's own permission, parsing
// and CommandExecutingEvent decide. Never the local console: without the server's channel nothing is sent and nothing runs.
namespace Kiln;

public static partial class BlockUi
{
    /// <summary>The capability of <see cref="ExecuteOnServer"/> and <see cref="CommandStatus"/>: available on a client while the
    /// server it joined (or hosts) has said hello on kiln:ui-command.</summary>
    internal const string ServerCommandCapability = "ui.command.server";

    /// <summary>
    /// Runs <paramref name="literalCommand"/> on the server this game joined, as this game's player there (the server takes
    /// the player from the connection: the command's own permission, parsing and cancel events decide). The line is sent as
    /// it is, once: no binding is filled in, and nothing is ever sent again on its own. The task ends with the server's final
    /// answer, or after 15 s without one (<see cref="UiCommandCode.TimedOutUnknown"/>: it may or may not have run; ask
    /// <see cref="CommandStatus"/> later). Cancelling before the line is sent sends nothing
    /// (<see cref="UiCommandCode.CanceledBeforeSend"/>); after, one cancel goes out: a command still waiting on the server is
    /// removed (<see cref="UiCommandCode.CanceledBeforeStart"/>, not run), one that started runs on
    /// (<see cref="UiCommandCode.CanceledAfterStart"/>, outcome unknown). One <see cref="UiCommandCompletedEvent"/> is
    /// posted when the task ends; later news of the same request posts <see cref="UiCommandStatusChangedEvent"/>.
    /// </summary>
    /// <param name="literalCommand">One command line, as typed in chat (the prefix is optional): at most 1024 UTF-8 bytes, no NUL, CR or LF.</param>
    /// <param name="cancellation">Stops the wait (see above); never rolls back a command that started.</param>
    /// <returns>The outcome. A refusal is <c>Ok = false</c> with <see cref="UiCommandResult.Reason"/> and <see cref="UiCommandResult.Code"/>.</returns>
    /// <exception cref="ArgumentNullException"><paramref name="literalCommand"/> is null.</exception>
    /// <exception cref="ArgumentException">The line is blank, longer than 1024 UTF-8 bytes, has a NUL, CR or LF, or is not valid text.</exception>
    /// <exception cref="KilnUnavailableException"><c>ui.command.server</c> is off: not a client, not joined, or the server has no
    /// Kiln (or an older one) and said no hello. Nothing was sent.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability(ServerCommandCapability)]
    public static Task<UiCommandResult> ExecuteOnServer(string literalCommand, CancellationToken cancellation = default) =>
        UiCommandClient.Shared.Execute(literalCommand, cancellation);

    /// <summary>
    /// The state of a request <see cref="ExecuteOnServer"/> sent: from this game's receipt when its answer was final, else
    /// asked of the server (never runs anything there). A request of an earlier connection is answered from the receipt only.
    /// Waits at most 15 s for the server; then the receipt's last known state.
    /// </summary>
    /// <param name="nonce"><see cref="UiCommandResult.Nonce"/> of the request.</param>
    /// <param name="requestId"><see cref="UiCommandResult.RequestId"/> of the request (not 0).</param>
    /// <exception cref="ArgumentException"><paramref name="nonce"/> is empty or <paramref name="requestId"/> is 0.</exception>
    /// <exception cref="KilnUnavailableException"><c>ui.command.server</c> is off and this game has no receipt of the request.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability(ServerCommandCapability)]
    public static Task<UiCommandResult> CommandStatus(string nonce, ulong requestId) =>
        UiCommandClient.Shared.Status(nonce, requestId);
}

/// <summary>Where a command of a button or a key runs (<see cref="UiAction.CommandTarget"/>).</summary>
public enum UiCommandTarget
{
    /// <summary>This game's command registry, as its console runs a line (the default).</summary>
    Local = 0,
    /// <summary>The server this game joined (or hosts), as this game's player there (<see cref="BlockUi.ExecuteOnServer"/>).</summary>
    Server = 1,
}

/// <summary>Where a request to the server stands.</summary>
public enum UiCommandState
{
    /// <summary>The server took it and it waits for a free slot (at most 5 s).</summary>
    Queued = 0,
    /// <summary>The server started it: its handler is running (an async command) or ran.</summary>
    Started = 1,
    /// <summary>It started more than 45 s ago and still runs. Not a cancellation: it may still end either way.</summary>
    RunningOverBudget = 2,
    /// <summary>The answer will not change.</summary>
    Final = 3,
}

/// <summary>Whether a command ran, as far as this game can know.</summary>
public enum UiCommandOutcome
{
    /// <summary>Nothing was sent.</summary>
    NotSent = 0,
    /// <summary>The server did not run it, for certain.</summary>
    NotStarted = 1,
    /// <summary>It ran to its end (<see cref="UiCommandResult.Ok"/> says whether it succeeded).</summary>
    Completed = 2,
    /// <summary>It may or may not have run (or may still be running).</summary>
    Unknown = 3,
}

/// <summary>Why a request to the server ended as it did.</summary>
public enum UiCommandCode
{
    /// <summary>It ran and succeeded.</summary>
    None,
    /// <summary>This player may not use the command (the command's own text in the reason). Not run.</summary>
    PermissionDenied,
    /// <summary>No such command, or the line did not match its arguments. Not run.</summary>
    ParseFailure,
    /// <summary>A <c>CommandExecutingEvent</c> handler on the server stopped it. Not run.</summary>
    CanceledByCommandEvent,
    /// <summary>Too many of this player's (or everyone's) commands were waiting. Not run.</summary>
    QueueFull,
    /// <summary>This player sent commands too fast (4 a second, 8 at once). Not run.</summary>
    RateLimited,
    /// <summary>It waited 5 s for a free slot. Not run.</summary>
    ExpiredBeforeStart,
    /// <summary>The cancel reached it while it waited. Not run.</summary>
    CanceledBeforeStart,
    /// <summary>The cancel came after it started: it runs on.</summary>
    TooLateToCancel,
    /// <summary>The connection's player changed (another character, a reconnect) before it started. Not run.</summary>
    AssociationChanged,
    /// <summary>Its handler failed (the reason says how).</summary>
    HandlerFailure,
    /// <summary>It has run for more than 45 s and still runs (with <see cref="UiCommandState.RunningOverBudget"/>).</summary>
    RunningTimeout,
    /// <summary>The server no longer remembers this request id: whether it ran is unknown. Never run again.</summary>
    ReplayExpired,
    /// <summary>The request id came again with another line. That line was not run.</summary>
    ProtocolConflict,
    /// <summary>The server has no request of that id (a cancel or a status of one it never had).</summary>
    UnknownRequest,
    /// <summary>Canceled before it was sent. Nothing was sent.</summary>
    CanceledBeforeSend,
    /// <summary>Canceled after it started on the server: it may have run, or may still run.</summary>
    CanceledAfterStart,
    /// <summary>No final answer within 15 s: it may or may not have run.</summary>
    TimedOutUnknown,
}

/// <summary>
/// A request's outcome or state. <see cref="Ok"/> is true only when the command ran and succeeded
/// (<see cref="UiCommandState.Final"/> with <see cref="UiCommandCode.None"/>); otherwise <see cref="Reason"/> says why.
/// </summary>
/// <param name="Ok">The command ran and succeeded.</param>
/// <param name="Reason">Why not, in plain words (the command's own error text where it gave one); null when <paramref name="Ok"/>.</param>
/// <param name="Output">What the command answered, at most 16 lines and 4096 bytes.</param>
/// <param name="RequestId">The request's id on its connection; 0 when nothing was sent.</param>
/// <param name="Nonce">The connection's challenge the request was sent under (for <see cref="BlockUi.CommandStatus"/>).</param>
/// <param name="State">Where the request stands.</param>
/// <param name="Outcome">Whether it ran, as far as this game can know.</param>
/// <param name="Code">Why it ended as it did.</param>
/// <param name="Truncated">The server cut <paramref name="Output"/> to fit.</param>
public sealed record UiCommandResult(bool Ok, string? Reason, IReadOnlyList<string> Output, ulong RequestId, string Nonce,
    UiCommandState State, UiCommandOutcome Outcome, UiCommandCode Code, bool Truncated) : IKilnResult;

/// <summary>A wait of <see cref="BlockUi.ExecuteOnServer"/> (or of a block's server command) ended: exactly one per request. Posted
/// on the client's main thread.</summary>
public sealed class UiCommandCompletedEvent : KilnEvent
{
    internal UiCommandCompletedEvent(UiCommandResult result) => Result = result;

    /// <summary>What the wait ended with (the same result its task returned).</summary>
    public UiCommandResult Result { get; }
}

/// <summary>News of a request from the server: it was queued, started, ran over its budget, or ended (also after its wait
/// ended: a late final answer). Posted on the client's main thread.</summary>
public sealed class UiCommandStatusChangedEvent : KilnEvent
{
    internal UiCommandStatusChangedEvent(string nonce, ulong requestId, UiCommandState state, UiCommandResult? result)
    {
        Nonce = nonce;
        RequestId = requestId;
        State = state;
        Result = result;
    }

    /// <summary>The connection's challenge the request was sent under.</summary>
    public string Nonce { get; }

    /// <summary>The request's id.</summary>
    public ulong RequestId { get; }

    /// <summary>Its state now.</summary>
    public UiCommandState State { get; }

    /// <summary>The final result once <see cref="State"/> is <see cref="UiCommandState.Final"/>; null before.</summary>
    public UiCommandResult? Result { get; }
}
