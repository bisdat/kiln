// SPDX-License-Identifier: MIT
// Joined-server UI commands (dossier 09 §4.5, §6 "Additional required regressions"; DESIGN.md §5) without a game: the server's
// receiver (UiServerCommands) and the client's sender (UiCommandClient) on a fake clock, fake connections and a fake command
// registry, and a block action's route: a Server-target command never reaches this game's console.
using System.Text;
using Xunit;

namespace Kiln.BlockTests;

[Collection("ui blocks")]
public sealed class UiCommandTests : IDisposable
{
    private long _now = 100_000;
    private readonly List<(object Conn, object Msg)> _sent = new();
    private readonly List<(EntityId Player, string Line)> _runs = new();
    private readonly Dictionary<object, EntityId> _players = new(ReferenceEqualityComparer.Instance);
    private readonly Dictionary<string, TaskCompletionSource<UiCommandRun>> _slow = new();
    private readonly UiServerCommands _s;
    private readonly UiCommandClient _sharedBefore = UiCommandClient.Shared;

    private sealed class Conn(string name)
    {
        public override string ToString() => name;
    }

    public UiCommandTests()
    {
        _s = new UiServerCommands
        {
            Clock = () => _now,
            Send = (c, m) => { _sent.Add((c, m)); return true; },
            Execute = (p, line) =>
            {
                _runs.Add((p, line));
                if (line.StartsWith("slow", StringComparison.Ordinal))
                {
                    var t = new TaskCompletionSource<UiCommandRun>();
                    _slow[line] = t;
                    return new UiCommandRun(false, UiCommandCode.None, null, ["started " + line], t.Task);
                }
                if (line == "denied") return new UiCommandRun(false, UiCommandCode.PermissionDenied, "You may not use /denied.", []);
                return new UiCommandRun(true, UiCommandCode.None, null, [$"ran {line} as {p}"]);
            },
            CurrentPlayer = c => _players.TryGetValue(c, out var p) ? p : EntityId.None,
        };
    }

    public void Dispose() => UiCommandClient.Shared = _sharedBefore;

    // ---- helpers ----------------------------------------------------------------------------------------------------

    private string Join(object conn, uint player)
    {
        _players[conn] = new EntityId(player);
        return _s.Associate(conn, new EntityId(player));
    }

    private void Req(object conn, string? nonce, ulong id, string line) => _s.OnRequest(conn, new UiCommandWire.Request(nonce, id.ToString(), line));
    private void Cancel(object conn, string? nonce, ulong id) => _s.OnCancel(conn, new UiCommandWire.Cancel(nonce, id.ToString()));
    private void Status(object conn, string? nonce, ulong id) => _s.OnStatus(conn, new UiCommandWire.StatusRequest(nonce, id.ToString()));

    private List<UiCommandWire.Reply> Replies(object conn, ulong? id = null) =>
        [.. _sent.Where(x => ReferenceEquals(x.Conn, conn) && x.Msg is UiCommandWire.Reply r && (id is null || r.Id == id.ToString())).Select(x => (UiCommandWire.Reply)x.Msg)];

    private UiCommandWire.Reply Last(object conn, ulong id) => Replies(conn, id).Last();

    private static void Final(UiCommandWire.Reply r, UiCommandCode code)
    {
        Assert.Equal("final", r.State);
        Assert.Equal(UiCommandWire.CodeName(code), r.Code);
        Assert.Equal(code == UiCommandCode.None, r.Ok);
    }

    private int Ran(string line) => _runs.Count(r => r.Line == line);

    // ---- the server ---------------------------------------------------------------------------------------------------

    [Fact]
    public void A_request_runs_in_the_drain_never_inside_its_handler_and_is_started_before_its_command()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Assert.Contains(_sent, x => ReferenceEquals(x.Conn, a) && x.Msg is UiCommandWire.Hello h && h.Nonce == n);
        Assert.True(UiCommandWire.NonceOk(n));
        UiCommandState? seen = null;
        var inner = _s.Execute;
        _s.Execute = (p, l) => { seen = _s.StateOf(a, 1); return inner(p, l); };
        Req(a, n, 1, "hello");
        Assert.Empty(_runs);
        Assert.Equal("queued", Last(a, 1).State);
        _s.Drain();
        Assert.Equal(UiCommandState.Started, seen);
        Assert.Equal([(new EntityId(7), "hello")], _runs);
        Final(Last(a, 1), UiCommandCode.None);
        Assert.Equal(["ran hello as 7"], Last(a, 1).Output);
    }

    [Fact]
    public void Request_1_never_runs_again_after_70_later_replies()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        for (ulong id = 1; id <= 71; id++)
        {
            _now += 300;
            Req(a, n, id, $"cmd {id}");
            _s.Drain();
        }
        Assert.Equal(71, _runs.Count);
        Req(a, n, 71, "cmd 71");  // still cached: the same final again
        Final(Last(a, 71), UiCommandCode.None);
        Req(a, n, 1, "cmd 1");
        Final(Last(a, 1), UiCommandCode.ReplayExpired);
        Req(a, n, 1, "something else");
        Final(Last(a, 1), UiCommandCode.ReplayExpired);
        _now += 1000;
        _s.Drain();
        Assert.Equal(1, Ran("cmd 1"));
        Assert.Equal(71, _runs.Count);
        Assert.Equal(UiCommandOutcome.Unknown, UiCommandWire.OutcomeOf(UiCommandCode.ReplayExpired));
        // and a cached reply is forgotten after 120 s: replay expired, never run
        _now += 120_000;
        _s.Drain();
        Req(a, n, 71, "cmd 71");
        Final(Last(a, 71), UiCommandCode.ReplayExpired);
        _s.Drain();
        Assert.Equal(1, Ran("cmd 71"));
    }

    [Fact]
    public void Same_id_with_another_line_is_a_conflict_and_never_runs_and_a_duplicate_gets_the_cached_reply()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "first");
        Req(a, n, 1, "second");  // while queued
        Final(Last(a, 1), UiCommandCode.ProtocolConflict);
        _s.Drain();
        Req(a, n, 1, "second");  // after the final
        Final(Last(a, 1), UiCommandCode.ProtocolConflict);
        Req(a, n, 1, "first ");  // exact bytes: a trailing space is another line
        Final(Last(a, 1), UiCommandCode.ProtocolConflict);
        Req(a, n, 1, "first");
        Final(Last(a, 1), UiCommandCode.None);
        _s.Drain();
        Assert.Equal([(new EntityId(7), "first")], _runs);
        Assert.Equal(UiCommandOutcome.NotStarted, UiCommandWire.OutcomeOf(UiCommandCode.ProtocolConflict));
    }

    [Fact]
    public void A_cancel_in_the_same_batch_and_a_queued_cancel_are_not_started()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "one");
        Cancel(a, n, 1);
        _s.Drain();
        Assert.Empty(_runs);
        Final(Last(a, 1), UiCommandCode.CanceledBeforeStart);
        Assert.Equal(UiCommandOutcome.NotStarted, UiCommandWire.OutcomeOf(UiCommandCode.CanceledBeforeStart));
        // the same id cannot be made to run later
        Req(a, n, 1, "one");
        _s.Drain();
        Final(Last(a, 1), UiCommandCode.CanceledBeforeStart);
        Assert.Empty(_runs);
    }

    [Fact]
    public void A_request_that_waits_5_s_expires_unrun()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "slow 1");
        Req(a, n, 2, "slow 2");
        _s.Drain();
        Req(a, n, 3, "waits");
        _now += 4_999;
        _s.Drain();
        Assert.Equal("queued", Last(a, 3).State);
        _now += 1;
        _s.Drain();
        Final(Last(a, 3), UiCommandCode.ExpiredBeforeStart);
        _slow["slow 1"].SetResult(new UiCommandRun(true, UiCommandCode.None, null, []));
        _s.Drain();
        _s.Drain();
        Assert.Equal(0, Ran("waits"));
        Assert.Equal(UiCommandOutcome.NotStarted, UiCommandWire.OutcomeOf(UiCommandCode.ExpiredBeforeStart));
    }

    [Fact]
    public void A_cancel_after_the_start_is_too_late_and_a_cancel_of_an_unknown_id_is_UnknownRequest()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "slow 1");
        _s.Drain();
        Assert.Equal("started", Last(a, 1).State);
        Cancel(a, n, 1);
        var r = Last(a, 1);
        Assert.Equal("started", r.State);
        Assert.Equal(UiCommandWire.CodeName(UiCommandCode.TooLateToCancel), r.Code);
        Assert.False(r.Ok);
        Cancel(a, n, 5);
        Final(Last(a, 5), UiCommandCode.UnknownRequest);
        Assert.Null(_s.StateOf(a, 5));
    }

    [Fact]
    public void A_status_request_never_runs_anything_nor_moves_the_watermark()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Status(a, n, 5);
        Final(Last(a, 5), UiCommandCode.UnknownRequest);
        Req(a, n, 1, "one");  // accepted: the watermark is still 0
        Assert.Equal("queued", Last(a, 1).State);
        _s.Drain();
        Status(a, n, 1);
        Final(Last(a, 1), UiCommandCode.None);
        _now += 1000;
        Status(a, n, 3);
        Final(Last(a, 3), UiCommandCode.UnknownRequest);
        Req(a, n, 2, "two");
        _s.Drain();
        Assert.Equal(["one", "two"], _runs.Select(r => r.Line));
        // 2 a second, 4 at once
        var b = new Conn("b");
        var nb = Join(b, 8);
        for (var i = 0; i < 6; i++) Status(b, nb, 9);
        Assert.Equal(4, Replies(b, 9).Count);
        _now += 500;
        Status(b, nb, 9);
        Assert.Equal(5, Replies(b, 9).Count);
    }

    [Fact]
    public void Requests_are_rate_limited_and_the_queue_is_bounded()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        for (ulong id = 1; id <= 9; id++) Req(a, n, id, $"r{id}");
        Assert.All(Enumerable.Range(1, 8), i => Assert.Equal("queued", Last(a, (ulong)i).State));
        Final(Last(a, 9), UiCommandCode.RateLimited);
        _s.Drain();
        Assert.Equal(8, _runs.Count);
        Assert.Equal(0, Ran("r9"));
        Req(a, n, 9, "r9");  // a refused id stays refused
        Final(Last(a, 9), UiCommandCode.RateLimited);
        _now += 250;  // one token
        Req(a, n, 10, "r10");
        Assert.Equal("queued", Last(a, 10).State);
        Req(a, n, 11, "r11");
        Final(Last(a, 11), UiCommandCode.RateLimited);
        Assert.Equal(UiCommandOutcome.NotStarted, UiCommandWire.OutcomeOf(UiCommandCode.RateLimited));

        // active (queued + running) per connection, and in all
        var s = _s;
        s.MaxActivePerPeer = 3;
        var b = new Conn("b");
        var nb = Join(b, 8);
        Req(b, nb, 1, "slow b1");
        Req(b, nb, 2, "slow b2");
        Req(b, nb, 3, "b3");
        Req(b, nb, 4, "b4");
        Final(Last(b, 4), UiCommandCode.QueueFull);
        s.MaxActive = s.MaxActivePerPeer = 64;
        _s.Drain();
        s.MaxActive = 2;  // b's two running hold it
        var c = new Conn("c");
        var nc = Join(c, 9);
        Req(c, nc, 1, "c1");
        Final(Last(c, 1), UiCommandCode.QueueFull);
    }

    [Fact]
    public void A_runner_past_45_s_keeps_one_of_16_process_slots_across_reconnects_and_world_changes()
    {
        var conns = Enumerable.Range(0, 8).Select(i => new Conn($"p{i}")).ToArray();
        var nonces = conns.Select((c, i) => Join(c, (uint)(10 + i))).ToArray();
        for (var i = 0; i < 8; i++)
        {
            Req(conns[i], nonces[i], 1, $"slow {i}a");
            Req(conns[i], nonces[i], 2, $"slow {i}b");
        }
        _s.Drain();
        Assert.Equal(16, _s.RunningSlots);
        _now += 45_000;
        _s.Drain();
        foreach (var c in conns)
        {
            var r = Last(c, 1);
            Assert.Equal("runningOverBudget", r.State);
            Assert.Equal(UiCommandWire.CodeName(UiCommandCode.RunningTimeout), r.Code);
        }
        // everyone reconnects (a world change looks the same: new connections, new players)
        _sent.Clear();
        foreach (var c in conns) _s.Gone(c);
        Assert.Empty(_sent);
        var fresh = new Conn("fresh");
        var nf = Join(fresh, 50);
        Req(fresh, nf, 1, "after");
        _s.Drain();
        Assert.Equal(0, Ran("after"));
        Assert.Equal(16, _s.RunningSlots);
        _now += 5_000;
        _s.Drain();
        Final(Last(fresh, 1), UiCommandCode.ExpiredBeforeStart);
        // one stuck handler ends: its slot is free, and its old connection hears nothing
        _slow["slow 3a"].SetResult(new UiCommandRun(true, UiCommandCode.None, null, ["late"]));
        _s.Drain();
        Assert.Equal(15, _s.RunningSlots);
        Assert.DoesNotContain(_sent, x => ReferenceEquals(x.Conn, conns[3]));
        Req(fresh, nf, 2, "now");
        _s.Drain();
        Assert.Equal(1, Ran("now"));
        Final(Last(fresh, 2), UiCommandCode.None);
    }

    [Fact]
    public void Identity_fields_a_client_claims_are_ignored()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        var json = $$"""{"nonce":"{{n}}","id":"1","line":"who","player":"99","entity":99,"account":"76561198000000000","admin":true,"mod":"other","source":"console"}""";
        var m = UiCommandWire.Decode<UiCommandWire.Request>(Encoding.UTF8.GetBytes(json + "\0\0"));
        _s.OnRequest(a, m);
        _s.Drain();
        Assert.Equal([(new EntityId(7), "who")], _runs);
        Assert.Equal(["ran who as 7"], Last(a, 1).Output);
    }

    [Fact]
    public void Another_nonce_or_a_connection_without_a_player_never_runs()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, "00000000000000000000000000000000", 1, "guess");
        Final(Last(a, 1), UiCommandCode.AssociationChanged);
        var lab = new Conn("lab");  // no player: no hello, nothing runs, nothing answers
        Req(lab, n, 1, "guess");
        _s.Sync([(lab, EntityId.None)]);
        _s.Drain();
        Assert.Empty(_runs);
        Assert.Empty(Replies(lab));
        Assert.DoesNotContain(_sent, x => ReferenceEquals(x.Conn, lab));
        Assert.Equal(UiCommandOutcome.NotStarted, UiCommandWire.OutcomeOf(UiCommandCode.AssociationChanged));
    }

    [Fact]
    public void A_new_player_on_the_connection_ends_its_queued_requests_unrun_and_gets_a_new_nonce()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "slow 1");
        Req(a, n, 2, "slow 2");
        _s.Drain();
        Req(a, n, 3, "queued");
        var n2 = Join(a, 8);  // another character
        Assert.NotEqual(n, n2);
        var r = Last(a, 3);
        Final(r, UiCommandCode.AssociationChanged);
        Assert.Equal(n, r.Nonce);
        Req(a, n, 4, "old nonce");
        Final(Last(a, 4), UiCommandCode.AssociationChanged);
        Req(a, n2, 1, "new");  // ids start again under the new nonce; the old runners keep their slots
        _s.Drain();
        Assert.Equal(0, Ran("queued"));
        Assert.Equal(0, Ran("old nonce"));
        Assert.Equal([(new EntityId(8), "new")], _runs.Where(x => x.Line == "new"));
        Assert.Equal(2, _s.RunningSlots);
    }

    [Fact]
    public void The_association_is_checked_again_right_before_the_start()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "one");
        _players[a] = new EntityId(9);  // a reused slot: the game gave the connection someone else before the drain
        _s.Drain();
        Assert.Empty(_runs);
        Final(Last(a, 1), UiCommandCode.AssociationChanged);
    }

    [Fact]
    public void A_disconnect_drops_queued_requests()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "slow 1");
        Req(a, n, 2, "slow 2");
        _s.Drain();
        Req(a, n, 3, "queued");
        _sent.Clear();
        _s.Sync([]);  // gone
        Assert.Empty(_sent.Where(x => x.Msg is UiCommandWire.Reply r && r.Id == "3" && r.State != "final"));
        _slow["slow 1"].SetResult(new UiCommandRun(true, UiCommandCode.None, null, []));
        _slow["slow 2"].SetResult(new UiCommandRun(true, UiCommandCode.None, null, []));
        _s.Drain();
        _s.Drain();
        Assert.Equal(0, Ran("queued"));
        Assert.Equal(0, _s.RunningSlots);
        Assert.Null(_s.NonceOf(a));
    }

    [Fact]
    public void An_async_command_answers_started_then_its_final_output_clipped_to_16_lines_and_4096_bytes()
    {
        var a = new Conn("a");
        var n = Join(a, 7);
        Req(a, n, 1, "slow 1");
        _s.Drain();
        var started = Last(a, 1);
        Assert.Equal("started", started.State);
        Assert.False(started.Ok);
        var lines = Enumerable.Range(0, 20).Select(i => $"line {i}").ToList();
        _slow["slow 1"].SetResult(new UiCommandRun(true, UiCommandCode.None, null, lines));
        _s.Drain();
        var r = Last(a, 1);
        Final(r, UiCommandCode.None);
        Assert.Equal(16, r.Output!.Length);
        Assert.True(r.Truncated);
        var big = UiCommandWire.ClipOutput([new string('é', 3000)], out var cut);
        Assert.True(cut);
        Assert.True(Encoding.UTF8.GetByteCount(big[0]) <= 4095);
        Assert.Equal(0, Encoding.UTF8.GetByteCount(big[0]) % 2);  // cut at a character boundary
        Req(a, n, 2, "slow 2");
        _s.Drain();
        _slow["slow 2"].SetException(new InvalidOperationException("boom"));
        _s.Drain();
        Final(Last(a, 2), UiCommandCode.HandlerFailure);
        Assert.Contains("boom", Last(a, 2).Reason);
    }

    [Fact]
    public void Starts_are_at_most_16_a_drain_round_robin_and_2_running_per_connection()
    {
        var conns = Enumerable.Range(0, 10).Select(i => new Conn($"p{i}")).ToArray();
        var nonces = conns.Select((c, i) => Join(c, (uint)(10 + i))).ToArray();
        for (var i = 0; i < 10; i++)
            for (ulong id = 1; id <= 3; id++) Req(conns[i], nonces[i], id, $"c{i}.{id}");
        _s.Drain();
        Assert.Equal(16, _runs.Count);
        Assert.All(Enumerable.Range(0, 10), i => Assert.Equal(1, Ran($"c{i}.1")));
        _s.Drain();
        _s.Drain();
        Assert.Equal(30, _runs.Count);
        var b = new Conn("b");
        var nb = Join(b, 99);
        for (ulong id = 1; id <= 3; id++) Req(b, nb, id, $"slow b{id}");
        _s.Drain();
        Assert.Equal(2, _runs.Count(r => r.Line.StartsWith("slow b", StringComparison.Ordinal)));
        Assert.Equal("queued", Last(b, 3).State);
    }

    [Fact]
    public void The_wire_takes_only_canonical_ids_and_one_line()
    {
        Assert.True(UiCommandWire.TryParseId("1", out var one) && one == 1);
        Assert.True(UiCommandWire.TryParseId("18446744073709551615", out var max) && max == ulong.MaxValue);
        foreach (var bad in new[] { "0", "01", "-1", "+1", "1.0", " 1", "18446744073709551616", "", null })
            Assert.False(UiCommandWire.TryParseId(bad, out _), bad);
        Assert.Null(UiCommandWire.LineProblem("say hi"));
        Assert.NotNull(UiCommandWire.LineProblem("a\nb"));
        Assert.NotNull(UiCommandWire.LineProblem("a\rb"));
        Assert.NotNull(UiCommandWire.LineProblem("a\0b"));
        Assert.NotNull(UiCommandWire.LineProblem("\ud800x"));
        Assert.NotNull(UiCommandWire.LineProblem("  "));
        Assert.Null(UiCommandWire.LineProblem(new string('é', 512)));
        Assert.NotNull(UiCommandWire.LineProblem(new string('é', 513)));
        foreach (var code in Enum.GetValues<UiCommandCode>())
            Assert.True(UiCommandWire.TryCode(UiCommandWire.CodeName(code), out var back) && back == code);
        Assert.Equal("permissionDenied", UiCommandWire.CodeName(UiCommandCode.PermissionDenied));
    }

    // ---- the client ---------------------------------------------------------------------------------------------------

    private readonly List<object> _out = new();
    private readonly List<KilnEvent> _events = new();
    private const string Nonce = "0123456789abcdef0123456789abcdef";

    private UiCommandClient NewClient(bool hello = true)
    {
        var c = new UiCommandClient { Clock = () => _now, Send = m => { _out.Add(m); return true; }, Post = _events.Add };
        if (hello) c.OnHello(Nonce);
        return c;
    }

    private static UiCommandWire.Reply Reply(ulong id, string state, UiCommandCode code, bool ok = false, string? reason = null, params string[] output) =>
        new(Nonce, id.ToString(), state, ok, UiCommandWire.CodeName(code), reason, output, false);

    private int Completed => _events.OfType<UiCommandCompletedEvent>().Count();

    [Fact]
    public void Without_the_servers_hello_nothing_is_sent_and_the_capability_is_off()
    {
        var c = NewClient(hello: false);
        var e = Assert.Throws<KilnUnavailableException>(() => { _ = c.Execute("x", default); });
        Assert.Equal("ui.command.server", e.Capability);
        Assert.Empty(_out);
        Assert.Throws<ArgumentException>(() => { _ = c.Execute("a\nb", default); });
        Assert.Throws<ArgumentNullException>(() => { _ = c.Execute(null!, default); });
        c.OnHello("not a nonce");
        Assert.False(c.Available);
    }

    [Fact]
    public void A_cancel_before_the_send_sends_nothing()
    {
        var c = NewClient();
        using var cts = new CancellationTokenSource();
        cts.Cancel();
        var r = c.Execute("x", cts.Token).Result;
        Assert.Equal((UiCommandCode.CanceledBeforeSend, UiCommandOutcome.NotSent, 0ul, UiCommandState.Final), (r.Code, r.Outcome, r.RequestId, r.State));
        Assert.Empty(_out);
        c.Pump();
        Assert.Equal(1, Completed);
    }

    [Fact]
    public async Task The_15_s_wait_ends_unknown_once_is_never_retried_and_a_late_final_updates_the_receipt()
    {
        var c = NewClient();
        var task = c.Execute("long", default);
        var req = Assert.IsType<UiCommandWire.Request>(Assert.Single(_out));
        Assert.Equal(("1", Nonce, "long"), (req.Id, req.Nonce, req.Line));
        c.OnReply(Reply(1, "queued", UiCommandCode.None));
        _now += 14_999;
        c.Pump();
        Assert.False(task.IsCompleted);
        _now += 1;
        c.Pump();
        var r = await task;
        Assert.Equal((UiCommandCode.TimedOutUnknown, UiCommandOutcome.Unknown, false), (r.Code, r.Outcome, r.Ok));
        _now += 60_000;
        c.Pump();
        Assert.Single(_out);  // never sent again
        Assert.Equal(1, Completed);
        c.OnReply(Reply(1, "final", UiCommandCode.None, ok: true, output: "done"));
        Assert.Equal(1, Completed);
        var change = _events.OfType<UiCommandStatusChangedEvent>().Last();
        Assert.Equal((UiCommandState.Final, true, UiCommandOutcome.Completed), (change.State, change.Result!.Ok, change.Result.Outcome));
        var status = await c.Status(Nonce, 1);  // from the receipt: nothing asked
        Assert.True(status.Ok);
        Assert.Equal(["done"], status.Output);
        Assert.Single(_out);
        // the next request takes the next id: nothing reuses one
        _ = c.Execute("next", default);
        Assert.Equal("2", Assert.IsType<UiCommandWire.Request>(_out[^1]).Id);
    }

    [Fact]
    public async Task A_cancel_while_queued_sends_one_cancel_and_ends_not_started()
    {
        var c = NewClient();
        using var cts = new CancellationTokenSource();
        var task = c.Execute("x", cts.Token);
        c.OnReply(Reply(1, "queued", UiCommandCode.None));
        cts.Cancel();
        cts.Cancel();
        Assert.Single(_out.OfType<UiCommandWire.Cancel>());
        Assert.False(task.IsCompleted);
        c.OnReply(Reply(1, "final", UiCommandCode.CanceledBeforeStart, reason: "canceled before it started"));
        var r = await task;
        Assert.Equal((UiCommandCode.CanceledBeforeStart, UiCommandOutcome.NotStarted, UiCommandState.Final), (r.Code, r.Outcome, r.State));
        c.Pump();
        Assert.Equal(1, Completed);
    }

    [Fact]
    public async Task A_cancel_after_the_start_ends_unknown_and_the_command_runs_on()
    {
        var c = NewClient();
        using var cts = new CancellationTokenSource();
        var task = c.Execute("x", cts.Token);
        c.OnReply(Reply(1, "started", UiCommandCode.None));
        cts.Cancel();
        var r = await task;
        Assert.Equal((UiCommandCode.CanceledAfterStart, UiCommandOutcome.Unknown, UiCommandState.Started), (r.Code, r.Outcome, r.State));
        Assert.Empty(_out.OfType<UiCommandWire.Cancel>());
        // a cancel the server found too late ends the same way
        using var cts2 = new CancellationTokenSource();
        var t2 = c.Execute("y", cts2.Token);
        cts2.Cancel();
        Assert.Single(_out.OfType<UiCommandWire.Cancel>());
        c.OnReply(Reply(2, "started", UiCommandCode.TooLateToCancel));
        var r2 = await t2;
        Assert.Equal((UiCommandCode.CanceledAfterStart, UiCommandOutcome.Unknown), (r2.Code, r2.Outcome));
        c.OnReply(Reply(2, "final", UiCommandCode.None, ok: true));
        c.Pump();
        Assert.Equal(2, Completed);
    }

    [Fact]
    public async Task The_client_and_the_server_over_the_wire()
    {
        var a = new Conn("a");
        var c = NewClient(hello: false);
        _s.Send = (conn, m) =>
        {
            _sent.Add((conn, m));
            if (m is UiCommandWire.Hello h) c.OnHello(UiCommandWire.Decode<UiCommandWire.Hello>(UiCommandWire.Encode(h)).Nonce);
            if (m is UiCommandWire.Reply r) c.OnReply(UiCommandWire.Decode<UiCommandWire.Reply>(UiCommandWire.Encode(r)));
            return true;
        };
        c.Send = m =>
        {
            var bytes = m switch
            {
                UiCommandWire.Request q => UiCommandWire.Encode(q),
                UiCommandWire.Cancel q => UiCommandWire.Encode(q),
                UiCommandWire.StatusRequest q => UiCommandWire.Encode(q),
                _ => throw new InvalidOperationException(),
            };
            var json = Encoding.UTF8.GetString(bytes);
            Assert.DoesNotContain("player", json);
            Assert.DoesNotContain("admin", json);
            if (m is UiCommandWire.Request) _s.OnRequest(a, UiCommandWire.Decode<UiCommandWire.Request>(bytes));
            if (m is UiCommandWire.StatusRequest) _s.OnStatus(a, UiCommandWire.Decode<UiCommandWire.StatusRequest>(bytes));
            return true;
        };
        _players[a] = new EntityId(7);
        _s.Sync([(a, new EntityId(7))]);
        Assert.True(c.Available);
        var ok = c.Execute("/heal me", default);
        var denied = c.Execute("denied", default);
        _s.Drain();
        var r = await ok;
        Assert.Equal((true, UiCommandCode.None, UiCommandOutcome.Completed), (r.Ok, r.Code, r.Outcome));
        Assert.Equal(["ran /heal me as 7"], r.Output);
        var d = await denied;
        Assert.Equal((false, UiCommandCode.PermissionDenied, UiCommandOutcome.NotStarted, "You may not use /denied."), (d.Ok, d.Code, d.Outcome, d.Reason));
        Assert.Equal((false, "You may not use /denied."), (UiCommandClient.ToastOf(d, "denied")!.Value.Kind == ToastKind.Info, UiCommandClient.ToastOf(d, "denied")!.Value.Text));
        Assert.Equal("ran /heal me as 7", UiCommandClient.ToastOf(r, "/heal me")!.Value.Text);
        var s = await c.Status(c.Nonce!, 1);
        Assert.True(s.Ok);
        c.Pump();
        Assert.Equal(2, Completed);
    }

    // ---- a block's action ---------------------------------------------------------------------------------------------

    private sealed class Game : IUiBackend
    {
        public readonly List<(string Text, ToastKind Kind)> Toasts = new();
        public readonly List<string> Commands = new();
        public readonly List<Action> Keys = new();
        public bool Available => true;
        public string Language => "en-us";
        public bool JoinedServer => true;
        public IUiSurface CreateSurface(WindowRuntime window, string title) => throw new NotSupportedException();
        public IDisposable? AddMenu(UiMenuIn where, UiText text, int order, Action onClick) => null;
        public IUiHud ShowHud(string id, string text, HudAnchor anchor, Color color) => throw new NotSupportedException();
        public IDisposable? BindKey(KeyPlan plan, string category, Action onPress) { Keys.Add(onPress); return new Off(); }
        public void Toast(string text, ToastKind kind) { lock (Toasts) Toasts.Add((text, kind)); }
        public void RequestCast(string abilityId) { }
        public (bool Ok, string Message) RunCommand(string line) { Commands.Add(line); return (true, ""); }

        private sealed class Off : IDisposable { public void Dispose() { } }
    }

    private static void Until(Func<bool> done)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (!done() && sw.ElapsedMilliseconds < 5000) Thread.Sleep(5);
        Assert.True(done());
    }

    [Fact]
    public void A_server_target_never_reaches_this_games_console()
    {
        var game = new Game();
        UiRuntime.ResetForTests(game);
        try
        {
            var c = UiCommandClient.Shared = NewClient(hello: false);
            UiActions.Run(new CommandPlan("give me", UiCommandTarget.Server), "ui-test", "test");
            UiActions.Drain();
            Assert.Empty(game.Commands);
            Assert.Empty(_out);
            Assert.Equal([("This server cannot run UI commands (no Kiln, or an older Kiln): nothing was run.", ToastKind.Warning)], game.Toasts);

            c.OnHello(Nonce);
            UiActions.Run(new CommandPlan("give me", UiCommandTarget.Server), "ui-test", "test");
            UiActions.Drain();
            Assert.Empty(game.Commands);
            Assert.Equal("give me", Assert.IsType<UiCommandWire.Request>(Assert.Single(_out)).Line);
            c.OnReply(Reply(1, "final", UiCommandCode.PermissionDenied, reason: "You may not use /give."));
            Until(() => game.Toasts.Count == 2);
            Assert.Equal(("You may not use /give.", ToastKind.Error), game.Toasts[1]);

            // the texts of the other refusals
            Assert.Equal("The server refused: too many commands, try again.", UiCommandClient.ToastOf(Result(UiCommandCode.RateLimited), "x")!.Value.Text);
            Assert.Equal("The server refused: too many commands, try again.", UiCommandClient.ToastOf(Result(UiCommandCode.QueueFull), "x")!.Value.Text);
            Assert.Equal("The command was not run: it waited too long.", UiCommandClient.ToastOf(Result(UiCommandCode.ExpiredBeforeStart), "x")!.Value.Text);
            Assert.Equal("No answer from the server: the command may or may not have run.", UiCommandClient.ToastOf(Result(UiCommandCode.TimedOutUnknown), "x")!.Value.Text);
            Assert.Null(UiCommandClient.ToastOf(new UiCommandResult(true, null, [], 1, Nonce, UiCommandState.Final, UiCommandOutcome.Completed, UiCommandCode.None, false), "x"));

            // a local command still runs here, on the tick
            UiActions.Run(new CommandPlan("local one"), "ui-test", "test");
            UiActions.Drain();
            Assert.Equal(["local one"], game.Commands);
        }
        finally
        {
            UiRuntime.ResetForTests(new KilnUiBackend());
        }
    }

    private static UiCommandResult Result(UiCommandCode code) =>
        new(false, "r", [], 1, Nonce, UiCommandState.Final, UiCommandWire.OutcomeOf(code), code, false);

    // ---- the block shape ----------------------------------------------------------------------------------------------

    private static ModBlocks NewMod() => new("ui-test", 1) { ManifestClients = "mod" };

    [Fact]
    public void CommandTarget_goes_with_a_command_only_and_is_the_same_in_JSON()
    {
        static WindowBlock W(UiAction a) => new() { Title = "T", Rows = [new ButtonRow { Label = "B", Does = a }] };
        var e = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "w", W(new UiAction { Toggle = "config.x", CommandTarget = UiCommandTarget.Server })));
        Assert.Equal("rows[0].does.commandTarget", e.Field);
        e = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "w", W(new UiAction { CommandTarget = UiCommandTarget.Local })));
        Assert.Equal("rows[0].does.commandTarget", e.Field);
        e = Assert.Throws<BlockError>(() => Compiler.Lower(NewMod(), "w", W(new UiAction { Command = "x", CommandTarget = (UiCommandTarget)7 })));
        Assert.Equal("rows[0].does.commandTarget", e.Field);
        Compiler.Lower(NewMod(), "w", W(new UiAction { Command = "/camp rest", CommandTarget = UiCommandTarget.Server }));

        var json = BlockJson.Parse<WindowBlock>("""{ "title": "T", "rows": [ { "type": "button", "label": "Rest", "does": { "command": "camp rest", "commandTarget": "server" } } ] }""");
        var does = ((ButtonRow)json.Rows[0]).Does;
        Assert.Equal((UiCommandTarget?)UiCommandTarget.Server, does.CommandTarget);
        Assert.Null(BlockJson.Parse<WindowBlock>("""{ "title": "T", "rows": [ { "type": "button", "label": "R", "does": { "command": "x" } } ] }""").Rows.OfType<ButtonRow>().Single().Does.CommandTarget);
        Assert.Equal("""{"command":"x","commandTarget":"server"}""", new CommandPlan("x", UiCommandTarget.Server).Describe().ToJsonString());
        Assert.Equal("""{"command":"x"}""", new CommandPlan("x").Describe().ToJsonString());
    }

    [Fact]
    public void A_key_with_a_server_command_sends_it_to_the_server()
    {
        var game = new Game();
        UiRuntime.ResetForTests(game);
        try
        {
            UiCommandClient.Shared = NewClient();
            var mod = NewMod();
            var (handle, low, _) = Compiler.Lower(mod, "rest", new KeyBindingBlock { Key = "F8", Does = new UiAction { Command = "camp rest", CommandTarget = UiCommandTarget.Server } });
            mod.Handles["rest"] = handle;
            low.Commit();
            Assert.Single(game.Keys).Invoke();
            UiActions.Drain();
            Assert.Empty(game.Commands);
            Assert.Equal("camp rest", Assert.IsType<UiCommandWire.Request>(Assert.Single(_out)).Line);
        }
        finally
        {
            UiRuntime.ResetForTests(new KilnUiBackend());
        }
    }
}
