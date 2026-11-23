// SPDX-License-Identifier: MIT
// The camera protocol's state machines (chapter 03 §4.6) on a fake clock and a lossy fake link: the binding and its
// token, the serial rules, End's checks, the ACK deadline and retries, the heartbeat and the authority timeout, the rate
// limit, the negotiation window, the frame deadline, the caches, the client's own expiry.
using Kiln.CameraTransport;
using Xunit;

namespace Kiln.CameraTests.Transport;

public sealed class ProtocolTests
{
    private static Rig Bound(Action<Rig>? setup = null)
    {
        var rig = new Rig();
        setup?.Invoke(rig);
        rig.Bind();
        Assert.Equal(ServerBindingState.Bound, rig.Server.State);
        Assert.Equal(ClientBindingState.Bound, rig.Client.State);
        return rig;
    }

    private static TransportResult Last(Rig rig) => Assert.Single(rig.Results.TakeLast(1));

    [Fact]
    public void Hello_and_Welcome_bind_both_ends_on_one_tuple_and_a_repeated_Hello_gets_the_same_token()
    {
        var rig = new Rig();
        var welcomes = 0;
        rig.DropToClient = (id, _) => id == CameraMessage.Welcome && ++welcomes == 1;  // the first Welcome is lost
        rig.Server.PeerReady();
        rig.Client.Start();
        rig.Pump();
        Assert.Equal(ClientBindingState.Hello, rig.Client.State);
        var token = rig.Server.Binding.Token;
        rig.Advance(1000);  // the client says Hello again after a second
        Assert.Equal(2, rig.Count(CameraMessage.Hello, toClient: false));
        Assert.Equal(token, rig.Server.Binding.Token);  // idempotent: the same Welcome, not a new binding
        Assert.Equal(rig.Server.Binding, rig.Client.Binding);
        Assert.Equal((0x5E4Eul, 9u, 0xC11E47ul, 3u, 42u, 77ul),
            (rig.Client.Binding.ServerNonce, rig.Client.Binding.ServerEpoch, rig.Client.Binding.ClientNonce, rig.Client.Binding.ClientEpoch,
             rig.Client.Binding.Player, rig.Client.Binding.Incarnation));
        Assert.Equal(CameraWireAbi.RemoteParts, rig.Server.ClientCaps);
    }

    [Fact]
    public void A_request_round_trips_to_a_client_frame()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Path());
        Assert.Empty(rig.Results);  // never inline in Submit
        rig.Tick();
        var r = Last(rig);
        Assert.True(r.Ok);
        Assert.Equal((CameraWireAbi.CommitClientFrame, 1000ul, 1ul), (r.Scope, r.Lease, r.Revision));
        var started = Assert.Single(rig.Endpoint.Started);
        Assert.True(Requests.Path().SameAs(started.Request));
        Assert.Equal(1000ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void A_request_larger_than_the_negotiated_body_is_refused_BodyTooLarge_before_sending()
    {
        var rig = new Rig(clientMaxBody: 4000);
        rig.Bind();
        Assert.Equal(4000, rig.Server.EffectiveMaxBody);
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Path());
        rig.Tick();
        var r = Last(rig);
        Assert.Equal((false, CameraReasons.BodyTooLarge), (r.Ok, r.Reason));
        Assert.Equal(0, rig.Count(CameraMessage.Apply));
        Assert.False(r.MayHaveApplied);
    }

    [Fact]
    public void An_equal_serial_replays_its_ACK_and_never_applies_again()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        var apply = rig.SentToClient.Last(m => m.Id == CameraMessage.Apply).Body;
        var ack = rig.SentToServer.Last(m => m.Id == CameraMessage.Ack).Body;
        rig.Client.Receive(CameraMessage.Apply, apply);  // the same message again (a retry that crossed the ACK)
        Assert.Single(rig.Endpoint.Started);
        Assert.Equal(ack, rig.SentToServer.Last(m => m.Id == CameraMessage.Ack).Body);
        Assert.Equal(2, rig.SentToServer.Count(m => m.Id == CameraMessage.Ack));
    }

    [Fact]
    public void A_retry_while_the_request_is_pending_waits_for_its_result()
    {
        var rig = Bound(r => r.Endpoint.Auto = false);
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Pump();
        var apply = rig.SentToClient.Last(m => m.Id == CameraMessage.Apply).Body;
        rig.Client.Receive(CameraMessage.Apply, apply);
        Assert.Single(rig.Endpoint.Started);
        Assert.Equal(0, rig.Count(CameraMessage.Ack, toClient: false));
    }

    [Fact]
    public void A_lower_serial_cannot_mutate_also_after_its_ACK_left_the_cache()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        var first = rig.SentToClient.Last(m => m.Id == CameraMessage.Apply).Body;
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Tick();
        rig.Submit(TransportOp.Apply, lease: 2000, request: Requests.Lens(durationMs: 60000));
        rig.Tick();
        Assert.Equal(2000ul, rig.Client.ActiveLease);
        rig.Advance(10_500, 500);  // every cached ACK and tombstone is older than 10 s now
        Assert.Equal(0, rig.Client.AckCount);
        var starts = rig.Endpoint.Started.Count;
        rig.Client.Receive(CameraMessage.Apply, first);  // serial 1 again, uncached: the highest serial still refuses it
        Assert.Equal(starts, rig.Endpoint.Started.Count);
        Assert.Equal(2000ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void An_old_End_cannot_end_a_successor()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Tick();
        rig.Submit(TransportOp.Apply, lease: 2000);
        rig.Tick();
        // an End for the old lease with a fresh serial: it is tombstoned, so AlreadyEnded; the successor stays
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Tick();
        var r = Last(rig);
        Assert.True(r.Ok && r.AlreadyEnded);
        Assert.Equal(2000ul, rig.Client.ActiveLease);
        // an End for a lease the client never had: refused UnknownLease, the successor stays
        rig.Submit(TransportOp.End, lease: 3000);
        rig.Tick();
        Assert.Equal((false, CameraReasons.UnknownLease), (Last(rig).Ok, Last(rig).Reason));
        Assert.Equal(2000ul, rig.Client.ActiveLease);
        // an End with the right lease but another owner token cannot end it either
        rig.Submit(TransportOp.End, lease: 2000, owner: 6);
        rig.Tick();
        Assert.Equal(CameraReasons.UnknownLease, Last(rig).Reason);
        Assert.Equal(2000ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void An_ended_lease_is_never_revived_by_an_Apply()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Tick();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        Assert.Equal((false, CameraReasons.Cancelled), (Last(rig).Ok, Last(rig).Reason));
        Assert.Equal(0ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void Rebinding_needs_a_new_token_and_the_old_token_s_messages_do_nothing()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        var oldToken = rig.Server.Binding.Token;
        var oldApply = rig.SentToClient.Last(m => m.Id == CameraMessage.Apply).Body;
        // the player's incarnation changes: the server ends the old binding and sends a new Welcome with a new token
        rig.Server.Rebind(78);
        rig.Pump();
        Assert.NotEqual(oldToken, rig.Server.Binding.Token);
        Assert.Equal(rig.Server.Binding, rig.Client.Binding);
        Assert.Equal(78ul, rig.Client.Binding.Incarnation);
        Assert.Contains(rig.Ended, e => e.Lease == 1000 && e.EndReason == CameraEndReasons.PeerDisconnected);
        Assert.Contains(rig.Endpoint.Released, e => e.Lease == 1000);  // the client cleared its old remote owners
        Assert.Equal(0ul, rig.Client.HighestSerial);
        var starts = rig.Endpoint.Started.Count;
        rig.Client.Receive(CameraMessage.Apply, oldApply);   // old token: dropped without effect
        Assert.Equal(starts, rig.Endpoint.Started.Count);
        Assert.Equal(0ul, rig.Client.ActiveLease);
        // a new client world (new nonce): a new binding again, not the old Welcome
        var before = rig.Server.Binding.Token;
        rig.Client = rig.NewClient(0xBEEF, 4);
        rig.Client.Start();
        rig.Pump();
        Assert.NotEqual(before, rig.Server.Binding.Token);
        Assert.Equal(0xBEEFul, rig.Server.Binding.ClientNonce);
        Assert.Equal(rig.Server.Binding, rig.Client.Binding);
    }

    [Fact]
    public void A_packet_of_another_binding_is_dropped_without_effect()
    {
        var rig = Bound();
        var b = rig.Server.Binding;
        foreach (var wrong in new[] { b with { Token = b.Token + 1 }, b with { ServerEpoch = 10 }, b with { ClientEpoch = 4 }, b with { Player = 43 },
                     b with { Incarnation = 78 }, b with { ServerNonce = 1 } })
        {
            var body = CameraWire.Mutation(CameraMessage.Apply, new WireHeader(wrong, 5, 1000, 1, 50, 1), 0, Requests.Lens(), CameraWireAbi.MaxBody)!;
            rig.Client.Receive(CameraMessage.Apply, body);
        }
        Assert.Empty(rig.Endpoint.Started);
        Assert.Equal(0, rig.Count(CameraMessage.Ack, toClient: false));
        Assert.Equal(0ul, rig.Client.HighestSerial);
    }

    [Fact]
    public void A_malformed_request_on_a_valid_binding_gets_a_bounded_refusal_and_changes_nothing()
    {
        var rig = Bound();
        var body = CameraWire.Mutation(CameraMessage.Apply, new WireHeader(rig.Server.Binding, 5, 1000, 1, 50, 1), 0,
            Requests.Lens() with { InputPolicy = 3 }, CameraWireAbi.MaxBody)!;
        rig.Client.Receive(CameraMessage.Apply, body);
        Assert.Empty(rig.Endpoint.Started);
        Assert.True(CameraWire.TryAck(rig.SentToServer.Last().Body, out var ack));
        Assert.Equal((false, CameraReasons.MalformedRemote, 50ul), (ack.Ok, ack.Reason, ack.Header.Serial));
        Assert.Equal(50ul, rig.Client.HighestSerial);  // a refusal advances the serial
    }

    [Fact]
    public void A_lost_ACK_retries_the_same_serial_twice_then_answers_TransportTimeout_with_MayHaveApplied()
    {
        var rig = Bound();
        rig.DropToServer = (id, _) => id == CameraMessage.Ack;
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Pump();
        var sent = rig.SentToClient.Where(m => m.Id == CameraMessage.Apply).ToList();
        Assert.Single(sent);
        rig.Advance(950, 50);
        Assert.Single(rig.SentToClient, m => m.Id == CameraMessage.Apply);   // not before 1 s
        rig.Advance(50, 50);
        Assert.Equal(2, rig.Count(CameraMessage.Apply));                      // retry 1 at 1 s
        rig.Advance(1000, 50);
        Assert.Equal(3, rig.Count(CameraMessage.Apply));                      // retry 2 at 2 s
        var applies = rig.SentToClient.Where(m => m.Id == CameraMessage.Apply).Select(m => m.Body).ToList();
        Assert.All(applies, a => Assert.Equal(applies[0], a));                // the SAME serial and request, no new Apply
        Assert.Single(rig.Endpoint.Started);                                  // the client applied once
        Assert.Empty(rig.Results);
        rig.Advance(950, 50);
        Assert.Empty(rig.Results);                                            // not before 3 s
        rig.Advance(50, 50);
        var r = Last(rig);
        Assert.Equal((false, CameraReasons.TransportTimeout, true), (r.Ok, r.Reason, r.MayHaveApplied));
        Assert.Equal(1000ul, r.Lease);
        Assert.Equal(3, rig.Count(CameraMessage.Apply));                      // no third retry
        // a higher-serial End followed, and the heartbeat no longer covers the lease: the client let it go
        Assert.True(CameraWire.TryMutation(CameraMessage.End, rig.SentToClient.Last(m => m.Id == CameraMessage.End).Body, 16384, 256, out var end));
        Assert.True(CameraWire.TryHeaderOnly(applies[0], 16384, out var applyHeader));
        Assert.True(end!.Header.Serial > applyHeader.Serial);
        Assert.Equal(0ul, rig.Client.ActiveLease);
        rig.Advance(1000);
        Assert.True(CameraWire.TryHeartbeat(rig.SentToClient.Last(m => m.Id == CameraMessage.Heartbeat).Body, out var hb));
        Assert.Equal(0ul, hb.Lease);
    }

    [Fact]
    public void One_request_in_flight_per_endpoint_and_End_preempts_it()
    {
        var rig = Bound(r => r.Endpoint.Auto = false);
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Submit(TransportOp.Apply, lease: 2000);
        rig.Tick();
        Assert.Equal((false, CameraReasons.Busy, 2000ul), (Last(rig).Ok, Last(rig).Reason, Last(rig).Lease));
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Tick();
        var cancelled = rig.Results.Single(r => r.Lease == 1000 && r.Reason == CameraReasons.Cancelled);
        Assert.True(cancelled.MayHaveApplied);                               // it reached the client
        Assert.Contains(rig.Endpoint.Started[0].Serial, rig.Endpoint.Cancelled);  // the client cancelled its pending Apply
        var end = rig.Results.Last();
        Assert.True(end.Ok);                                                 // cancelled before apply: ended
    }

    [Fact]
    public void The_client_has_1_s_to_a_confirming_frame_then_refuses_NoCameraFrame()
    {
        var rig = Bound(r => r.Endpoint.Auto = false);
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Pump();
        rig.Advance(950, 50);
        Assert.Empty(rig.Results);
        Assert.Empty(rig.Endpoint.Cancelled);
        rig.Advance(50, 50);
        var r = Last(rig);
        Assert.Equal((false, CameraReasons.NoCameraFrame, true), (r.Ok, r.Reason, r.MayHaveApplied));
        Assert.Equal([rig.Endpoint.Started[0].Serial], rig.Endpoint.Cancelled);
        rig.Client.Complete(rig.Endpoint.Started[0].Serial, new NativeOutcome(true, 0, 2, 0, 9));  // too late: ignored
        Assert.Equal(0ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void A_heartbeat_comes_every_second_and_the_client_answers_with_Status()
    {
        var rig = Bound();
        var before = rig.Count(CameraMessage.Heartbeat);
        rig.Advance(950, 50);
        Assert.Equal(before, rig.Count(CameraMessage.Heartbeat));
        rig.Advance(50, 50);
        Assert.Equal(before + 1, rig.Count(CameraMessage.Heartbeat));
        rig.Advance(3000, 100);
        Assert.Equal(before + 4, rig.Count(CameraMessage.Heartbeat));
        Assert.True(CameraWire.TryHeartbeat(rig.SentToClient.Last(m => m.Id == CameraMessage.Heartbeat).Body, out var hb));
        Assert.True(CameraWire.TryStatus(rig.SentToServer.Last(m => m.Id == CameraMessage.Status).Body, out var st));
        Assert.Equal(hb.Sequence, st.Sequence);   // Status echoes the heartbeat
    }

    [Fact]
    public void Without_a_valid_heartbeat_for_3_s_the_client_clears_its_remote_lease()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 60000));
        rig.Tick();
        Assert.Equal(1000ul, rig.Client.ActiveLease);
        rig.DropToClient = (id, _) => id == CameraMessage.Heartbeat;
        rig.Advance(2900, 100);
        Assert.Equal(1000ul, rig.Client.ActiveLease);
        rig.Advance(200, 100);
        Assert.Equal(0ul, rig.Client.ActiveLease);
        Assert.Contains((1000ul, CameraEndReasons.AuthorityTimeout), rig.Endpoint.Released);
    }

    [Fact]
    public void Mutating_requests_are_limited_to_10_a_second_with_a_burst_of_16()
    {
        var rig = Bound();
        for (var i = 1; i <= 16; i++)
        {
            rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: (ulong)i);
            rig.Pump();
            rig.Server.Tick();
        }
        Assert.DoesNotContain(rig.Results, r => r.Reason == CameraReasons.RateLimit);
        rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 17);
        rig.Server.Tick();
        Assert.Equal(CameraReasons.RateLimit, Last(rig).Reason);
        rig.Now += 100;  // a tenth of a second: one more
        rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 18);
        rig.Pump();
        rig.Server.Tick();
        Assert.NotEqual(CameraReasons.RateLimit, Last(rig).Reason);
        rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 19);
        rig.Server.Tick();
        Assert.Equal(CameraReasons.RateLimit, Last(rig).Reason);
        rig.Submit(TransportOp.End, lease: 1000);   // End is never rate limited: cleanup stays possible
        rig.Pump();
        rig.Server.Tick();
        Assert.NotEqual(CameraReasons.RateLimit, Last(rig).Reason);
    }

    [Fact]
    public void A_request_before_the_Hello_waits_at_most_2_s_then_is_refused_Negotiation()
    {
        var rig = new Rig();
        rig.Server.PeerReady();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Server.Tick();
        Assert.Empty(rig.Results);
        rig.Now += 1999;
        rig.Server.Tick();
        Assert.Empty(rig.Results);
        rig.Now += 1;
        rig.Server.Tick();
        Assert.Equal(CameraReasons.Negotiation, Last(rig).Reason);
        Assert.Equal(ServerBindingState.Failed, rig.Server.State);
        // a late Hello still binds; requests go out from then on
        rig.Client.Start();
        rig.Pump();
        Assert.Equal(ServerBindingState.Bound, rig.Server.State);
    }

    [Fact]
    public void A_request_parked_before_the_Hello_goes_out_once_the_binding_exists()
    {
        var rig = new Rig();
        rig.Server.PeerReady();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Now += 1500;
        rig.Client.Start();
        rig.Pump();
        rig.Tick();
        Assert.True(Last(rig).Ok);
        Assert.Equal(1000ul, rig.Client.ActiveLease);
    }

    [Fact]
    public void The_client_keeps_256_ACKs_and_256_tombstones_for_10_s()
    {
        var rig = Bound();
        for (ulong lease = 1; lease <= 150; lease++)
        {
            rig.Submit(TransportOp.Apply, lease: lease * 10);
            rig.Pump();
            rig.Submit(TransportOp.End, lease: lease * 10);
            rig.Pump();
            rig.Server.Tick();
            rig.Now += 100;  // inside the rate limit (10 Apply a second; End is not limited)
        }
        Assert.Equal(256, rig.Client.AckCount);
        Assert.Equal(150, rig.Client.TombstoneCount);
        for (ulong lease = 151; lease <= 300; lease++)
        {
            rig.Submit(TransportOp.Apply, lease: lease * 10);
            rig.Pump();
            rig.Submit(TransportOp.End, lease: lease * 10);
            rig.Pump();
            rig.Server.Tick();
            rig.Now += 100;
        }
        Assert.Equal(256, rig.Client.TombstoneCount);
        Assert.Equal(256, rig.Client.AckCount);
        rig.Advance(10_000, 1000);
        Assert.Equal(0, rig.Client.TombstoneCount);
        Assert.Equal(0, rig.Client.AckCount);
        Assert.True(rig.Results.Take(600).All(r => r.Ok));
    }

    [Fact]
    public void The_client_expires_at_Duration_even_when_the_End_is_lost()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 2000));
        rig.Tick();
        // the End is lost, and so is the heartbeat that would stop authorizing the lease (its own timeout is 3 s)
        rig.DropToClient = (id, _) => id is CameraMessage.End or CameraMessage.Heartbeat;
        rig.Submit(TransportOp.End, lease: 1000);
        rig.Advance(1900, 100);
        Assert.Equal(1000ul, rig.Client.ActiveLease);
        rig.Advance(100, 100);
        Assert.Equal(0ul, rig.Client.ActiveLease);
        Assert.Contains((1000ul, CameraEndReasons.Expired), rig.Endpoint.Released);
        Assert.True(CameraWire.TryEnded(rig.SentToServer.Last(m => m.Id == CameraMessage.Ended).Body, out var ended));
        Assert.Equal((1000ul, CameraEndReasons.Expired), (ended.Header.Lease, ended.EndReason));
    }

    [Fact]
    public void A_client_End_is_reported_through_OnEnded_and_a_stale_one_is_not()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        var stale = CameraWire.Ended(new EndedMessage(new WireHeader(rig.Server.Binding, 5, 1000, 0, 9, Revision: 7), CameraEndReasons.TargetLost, 0, 3));
        rig.Server.Receive(CameraMessage.Ended, stale);   // another revision: ignored
        Assert.Empty(rig.Ended);
        rig.Client.LocalEnded(1000, CameraEndReasons.TargetLost, CameraReasons.TargetUnavailable, 44);
        rig.Pump();
        var e = Assert.Single(rig.Ended);
        Assert.Equal((1000ul, 1ul, CameraEndReasons.TargetLost, CameraReasons.TargetUnavailable, 44ul), (e.Lease, e.Revision, e.EndReason, e.Reason, e.Frame));
    }

    [Fact]
    public void The_server_lets_a_silent_lease_go_after_its_remaining_time_and_a_3_s_grace()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 2000));
        rig.Tick();
        rig.DropToServer = (_, _) => true;        // the client goes silent (no Status, no Ended)
        rig.DropToClient = (_, _) => true;
        rig.Advance(4900, 100);                   // remaining 2000 + grace 3000
        Assert.Empty(rig.Ended);
        rig.Advance(100, 100);
        Assert.Equal(CameraEndReasons.Expired, Assert.Single(rig.Ended).EndReason);
    }

    [Fact]
    public void An_update_preserves_or_restarts_the_timeline()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 60000));
        rig.Tick();
        rig.Advance(4000, 500);
        rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 2, request: Requests.Lens(70, 60000));
        rig.Tick();
        var preserved = Last(rig);
        Assert.True(preserved.Ok);
        Assert.InRange(preserved.ElapsedMs, 4000u, 4100u);   // T0 kept
        Assert.Equal(2ul, rig.Client.ActiveRevision);
        rig.Advance(1000, 500);
        rig.Submit(TransportOp.UpdateRestart, lease: 1000, revision: 3, request: Requests.Lens(80, 60000));
        rig.Tick();
        var restarted = Last(rig);
        Assert.Equal(0u, restarted.ElapsedMs);                // T0 reset on the committing update
        Assert.Equal(60000u, restarted.RemainingMs);
        Assert.Equal(CameraWireAbi.TimingRestart, rig.Endpoint.Started.Last().Timing);
    }

    [Fact]
    public void A_rejected_update_advances_the_serial_but_keeps_the_active_revision()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Tick();
        rig.Endpoint.Refuse = req => req.Kind == CameraMessage.Update ? CameraReasons.InsufficientLifetime : CameraReasons.None;
        rig.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 2);
        rig.Tick();
        Assert.Equal((false, CameraReasons.InsufficientLifetime), (Last(rig).Ok, Last(rig).Reason));
        Assert.Equal(1ul, rig.Client.ActiveRevision);
        Assert.Equal(rig.Server.HighestSerial, rig.Client.HighestSerial);
    }

    [Fact]
    public void A_part_the_client_does_not_offer_is_refused_ClientCapability()
    {
        var rig = new Rig(clientCaps: CameraWireAbi.CapLocalFov);
        rig.Bind();
        Assert.Equal(CameraWireAbi.CapLocalFov, rig.Server.ClientCaps);
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Profile());
        rig.Tick();
        Assert.Equal(CameraReasons.ClientCapability, Last(rig).Reason);
        Assert.Empty(rig.Endpoint.Started);
        rig.Submit(TransportOp.Apply, lease: 2000, request: Requests.Lens());
        rig.Tick();
        Assert.True(Last(rig).Ok);
    }

    [Fact]
    public void Closing_ends_everything_on_both_ends()
    {
        var rig = Bound(r => r.Endpoint.Auto = false);
        rig.Submit(TransportOp.Apply, lease: 1000);
        rig.Pump();
        rig.Server.Close(CameraEndReasons.WorldStopped);
        var r = Last(rig);   // delivered at once, on the closing thread
        Assert.Equal((false, CameraReasons.PeerUnavailable, true), (r.Ok, r.Reason, r.MayHaveApplied));
        rig.Submit(TransportOp.Apply, lease: 2000);
        rig.Server.Tick();
        Assert.Equal(CameraReasons.PeerUnavailable, Last(rig).Reason);
        rig.Client.Close(CameraEndReasons.WorldStopped);
        Assert.Contains(rig.Endpoint.Started[0].Serial, rig.Endpoint.Cancelled);
        Assert.Equal(ClientBindingState.Closed, rig.Client.State);
    }

    [Fact]
    public void Bad_submit_arguments_throw_and_send_nothing()
    {
        var rig = Bound();
        Assert.Throws<ArgumentException>(() => rig.Server.Submit(TransportOp.End, 5, 1000, 1, Requests.Lens(), _ => { }));
        Assert.Throws<ArgumentException>(() => rig.Server.Submit(TransportOp.Apply, 5, 1000, 1, null, _ => { }));
        Assert.Throws<ArgumentException>(() => rig.Server.Submit(TransportOp.Apply, 5, 0, 1, Requests.Lens(), _ => { }));
        Assert.Throws<ArgumentOutOfRangeException>(() => rig.Server.Submit((TransportOp)9, 5, 1000, 1, Requests.Lens(), _ => { }));
        Assert.Equal(0, rig.Count(CameraMessage.Apply));
    }

    [Fact]
    public void An_Ack_cannot_stretch_a_lease_past_its_duration_and_an_ok_visual_answer_is_ClientFrame()
    {
        var rig = Bound(r => r.Endpoint.Auto = false);
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 2000));
        rig.Pump();
        Assert.True(CameraWire.TryHeaderOnly(rig.SentToClient.Last(m => m.Id == CameraMessage.Apply).Body, CameraWireAbi.MaxBody, out var h));
        // a hostile client answers by itself: ServerState scope and the largest elapsed and remaining times the wire holds
        rig.DropToServer = (_, _) => true;
        rig.DropToClient = (_, _) => true;
        rig.Server.Receive(CameraMessage.Ack, CameraWire.Ack(new AckMessage(h, true, CameraReasons.None, CameraWireAbi.CommitServerState,
            0, 9, uint.MaxValue, uint.MaxValue, "")));
        rig.Server.Tick();
        var r = Last(rig);
        Assert.True(r.Ok);
        Assert.Equal(CameraWireAbi.CommitClientFrame, r.Scope);       // a remote visual success is ClientFrame, whatever the client wrote
        Assert.Equal((2000u, 2000u), (r.ElapsedMs, r.RemainingMs));   // never more than the request's Duration
        // the bookkeeping lets the silent lease go after its Duration plus the 3 s grace, not after ~49 days
        rig.Advance(4900, 100);
        Assert.Empty(rig.Ended);
        rig.Advance(100, 100);
        var e = Assert.Single(rig.Ended);
        Assert.Equal((1000ul, CameraEndReasons.Expired), (e.Lease, e.EndReason));
        rig.Advance(1000);
        Assert.True(CameraWire.TryHeartbeat(rig.SentToClient.Last(m => m.Id == CameraMessage.Heartbeat).Body, out var hb));
        Assert.Equal(0ul, hb.Lease);                                  // and the heartbeat no longer authorizes it
    }

    [Fact]
    public void A_Status_cannot_keep_a_lease_past_its_own_duration()
    {
        var rig = Bound();
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 2000));
        rig.Tick();   // the ACK arrives now
        Assert.True(Last(rig).Ok);
        rig.DropToServer = (_, _) => true;   // the honest client is cut off; a hostile one keeps saying its lease has time left
        rig.DropToClient = (_, _) => true;
        var binding = rig.Server.Binding;
        byte[] Status(ulong seq, uint remaining) => CameraWire.Status(new StatusMessage(binding, seq, 1000, 1, 0, remaining));
        for (var t = 0; t < 4900; t += 100)
        {
            rig.Server.Receive(CameraMessage.Status, Status((ulong)t + 1, t % 200 == 0 ? uint.MaxValue : 2000u));
            rig.Advance(100, 100);
        }
        Assert.Empty(rig.Ended);             // Duration 2000 + grace 3000 after the ACK: not yet
        rig.Server.Receive(CameraMessage.Status, Status(5000, 2000));
        rig.Advance(100, 100);
        Assert.Equal(CameraEndReasons.Expired, Assert.Single(rig.Ended).EndReason);
    }

    private static byte[] HelloFrom(ulong nonce) =>
        CameraWire.Hello(new HelloMessage(CameraWireAbi.Protocol, nonce, 3, 1001, CameraWireAbi.RemoteParts, CameraWireAbi.MaxKeys, CameraWireAbi.MaxBody));

    [Fact]
    public void A_new_client_world_rebinds_at_most_once_a_second_and_the_rest_change_nothing()
    {
        var rig = Bound();
        rig.Client = rig.NewClient(0xBEEF, 4);   // a new client world: its rebind is taken at once
        rig.Client.Start();
        rig.Pump();
        Assert.Equal(0xBEEFul, rig.Server.Binding.ClientNonce);
        Assert.Equal(rig.Server.Binding, rig.Client.Binding);
        rig.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 60000));
        rig.Tick();
        Assert.True(Last(rig).Ok);
        var binding = rig.Server.Binding;
        var welcomes = rig.Count(CameraMessage.Welcome);
        // a client churning Hellos with new nonces in the same second: no Welcome, no new token, the lease stays
        for (ulong n = 1; n <= 50; n++) rig.Server.Receive(CameraMessage.Hello, HelloFrom(0xD000 + n));
        Assert.Equal(binding, rig.Server.Binding);
        Assert.Equal(welcomes, rig.Count(CameraMessage.Welcome));
        Assert.Empty(rig.Ended);
        rig.Now += 999;
        rig.Server.Receive(CameraMessage.Hello, HelloFrom(0xE001));
        Assert.Equal(binding, rig.Server.Binding);
        Assert.Equal(welcomes, rig.Count(CameraMessage.Welcome));
        rig.Now += 1;   // a second after the last rebind: the next one is taken
        rig.Server.Receive(CameraMessage.Hello, HelloFrom(0xE002));
        Assert.Equal(0xE002ul, rig.Server.Binding.ClientNonce);
        Assert.NotEqual(binding.Token, rig.Server.Binding.Token);
        Assert.Equal(welcomes + 1, rig.Count(CameraMessage.Welcome));
        Assert.Contains(rig.Ended, e => e.Lease == 1000 && e.EndReason == CameraEndReasons.PeerDisconnected);
        rig.Server.Receive(CameraMessage.Hello, HelloFrom(0xE003));
        Assert.Equal(0xE002ul, rig.Server.Binding.ClientNonce);
    }

    [Fact]
    public void A_repeated_Hello_gets_the_same_Welcome_at_most_once_a_second()
    {
        var rig = Bound();
        var hello = rig.SentToServer.First(m => m.Id == CameraMessage.Hello).Body;
        var welcome = rig.SentToClient.First(m => m.Id == CameraMessage.Welcome).Body;
        var binding = rig.Server.Binding;
        // the Welcome of the bind went out this millisecond: replays of the same Hello are not an amplifier
        for (var i = 0; i < 50; i++) rig.Server.Receive(CameraMessage.Hello, hello);
        Assert.Equal(1, rig.Count(CameraMessage.Welcome));
        rig.Now += 999;
        rig.Server.Receive(CameraMessage.Hello, hello);
        Assert.Equal(1, rig.Count(CameraMessage.Welcome));
        rig.Now += 1;
        for (var i = 0; i < 50; i++) rig.Server.Receive(CameraMessage.Hello, hello);
        Assert.Equal(2, rig.Count(CameraMessage.Welcome));
        Assert.Equal(welcome, rig.SentToClient.Last(m => m.Id == CameraMessage.Welcome).Body);   // still idempotent: the same Welcome
        Assert.Equal(binding, rig.Server.Binding);
    }
}
