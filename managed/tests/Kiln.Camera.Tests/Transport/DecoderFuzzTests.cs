// SPDX-License-Identifier: MIT
// A seeded fuzz of channel kiln.camera v1's nine decoders (chapter 03 §4.6): every truncation of a valid body, trailing
// bytes, random bodies, random single-byte mutations, and counts and lengths at and over the limits. No exception
// escapes, a refused decode leaves no partial message, and an accepted one re-encodes to its own bytes (and a request
// passes Validate). The same malformed bodies then go through both bindings' Receive: they change nothing and send
// nothing (on a valid binding a malformed mutation only gets its bounded MalformedRemote refusal).
using System.Buffers.Binary;
using System.Text;
using Kiln.CameraTransport;
using Xunit;

namespace Kiln.CameraTests.Transport;

public sealed class DecoderFuzzTests
{
    private const int Seed = 0x03CA;
    private static readonly WireBinding B = new(0xABCDEF, 0x5E4E, 9, 0xC11E47, 3, 42, 77);
    private static readonly WireHeader H = new(B, 5, 1000, 3, 17, 2);

    /// <summary>One decoder: its message id, the decode (ok, message, whether the out value is empty) and the encoder.</summary>
    private sealed record Codec(string Name, CameraMessage Id, Func<byte[], (bool Ok, object? Message, bool Empty)> Decode,
        Func<object, byte[]> Encode, Func<object, bool> Valid);

    private static Codec Small<T>(CameraMessage id, TryDecode<T> decode, Func<T, byte[]> encode) where T : struct =>
        new(id.ToString(), id, b => (decode(b, out var m), m, m.Equals(default(T))), o => encode((T)o), _ => true);

    private delegate bool TryDecode<T>(ReadOnlySpan<byte> s, out T m);

    private static Codec Mutation(CameraMessage kind, int maxBody = CameraWireAbi.MaxBody, int maxKeys = CameraWireAbi.MaxKeys) => new(
        kind.ToString(), kind,
        b => (CameraWire.TryMutation(kind, b, maxBody, maxKeys, out var m), m, m is null),
        o => { var m = (MutationMessage)o; return CameraWire.Mutation(m.Kind, m.Header, m.Timing, m.Request, CameraWireAbi.MaxBody)!; },
        o => o is MutationMessage m && m.Kind == kind && (m.Request is null ? kind == CameraMessage.End : CameraWire.Validate(m.Request, maxKeys) == CameraReasons.None));

    private static readonly Codec[] Codecs =
    [
        Small<HelloMessage>(CameraMessage.Hello, CameraWire.TryHello, m => CameraWire.Hello(m)),
        Small<WelcomeMessage>(CameraMessage.Welcome, CameraWire.TryWelcome, m => CameraWire.Welcome(m)),
        Mutation(CameraMessage.Apply),
        Mutation(CameraMessage.Update),
        Mutation(CameraMessage.End),
        Small<AckMessage>(CameraMessage.Ack, CameraWire.TryAck, m => CameraWire.Ack(m)),
        Small<EndedMessage>(CameraMessage.Ended, CameraWire.TryEnded, m => CameraWire.Ended(m)),
        Small<HeartbeatMessage>(CameraMessage.Heartbeat, CameraWire.TryHeartbeat, m => CameraWire.Heartbeat(m)),
        Small<StatusMessage>(CameraMessage.Status, CameraWire.TryStatus, m => CameraWire.Status(m)),
    ];

    private static Codec Of(CameraMessage id) => Codecs.Single(c => c.Id == id);

    private static CameraWireRequest ModeRequest => new()
    {
        Parts = CameraWireAbi.PartMode, InputPolicy = CameraWireAbi.StockMode, DurationMs = 5000, CameraId = 34, Transition = 2,
    };

    /// <summary>Valid bodies of every message (a request of every part shape; diagnostics empty, multi-byte and 256 bytes).</summary>
    private static byte[][] Samples(CameraMessage id, WireHeader h) => id switch
    {
        CameraMessage.Hello => [CameraWire.Hello(new HelloMessage(1, h.Binding.ClientNonce, h.Binding.ClientEpoch, 1001, CameraWireAbi.RemoteParts, 256, 16384))],
        CameraMessage.Welcome =>
        [
            CameraWire.Welcome(new WelcomeMessage(h.Binding.ClientNonce, h.Binding.ClientEpoch, h.Binding.ServerNonce, h.Binding.ServerEpoch,
                h.Binding.Player, h.Binding.Incarnation, h.Binding.Token, CameraWireAbi.RemoteParts, 16384, 256)),
        ],
        CameraMessage.Apply =>
        [
            .. new[] { Requests.Path(), Requests.Path(3, 2000), Requests.Profile(), Requests.Lens(), ModeRequest }
                .Select(r => CameraWire.Mutation(CameraMessage.Apply, h, 0, r, CameraWireAbi.MaxBody)!),
        ],
        CameraMessage.Update =>
        [
            CameraWire.Mutation(CameraMessage.Update, h, CameraWireAbi.TimingRestart, Requests.Path(16, 8000), CameraWireAbi.MaxBody)!,
            CameraWire.Mutation(CameraMessage.Update, h, CameraWireAbi.TimingPreserve, Requests.Lens(70), CameraWireAbi.MaxBody)!,
        ],
        CameraMessage.End => [CameraWire.Mutation(CameraMessage.End, h, 0, null, CameraWireAbi.MaxBody)!],
        CameraMessage.Ack =>
        [
            CameraWire.Ack(new AckMessage(h, true, 0, CameraWireAbi.CommitClientFrame, 0, 9, 10, 20, "")),
            CameraWire.Ack(new AckMessage(h, false, CameraReasons.NoCameraFrame, 0, CameraWireAbi.FlagMayHaveApplied, 0, 0, 0, "no frame ä")),
            CameraWire.Ack(new AckMessage(h, false, CameraReasons.Bounds, 0, 0, 0, 0, 0, new string('ä', 128))),   // 256 bytes exactly
        ],
        CameraMessage.Ended => [CameraWire.Ended(new EndedMessage(h, CameraEndReasons.Expired, CameraReasons.None, 77))],
        CameraMessage.Heartbeat =>
        [
            CameraWire.Heartbeat(new HeartbeatMessage(h.Binding, 4, 17, h.Lease, h.Revision)),
            CameraWire.Heartbeat(new HeartbeatMessage(h.Binding, 5, 18, 0, 0)),
        ],
        CameraMessage.Status => [CameraWire.Status(new StatusMessage(h.Binding, 4, h.Lease, h.Revision, 100, 500))],
        _ => throw new ArgumentOutOfRangeException(nameof(id)),
    };

    /// <summary>Decodes once and checks the invariants; true when the body was accepted.</summary>
    private static bool Run(Codec c, byte[] body)
    {
        (bool Ok, object? Message, bool Empty) r;
        try
        {
            r = c.Decode(body);
        }
        catch (Exception e)
        {
            throw new InvalidOperationException($"{c.Name}: a {body.Length}-byte body starting {Convert.ToHexString(body.AsSpan(0, Math.Min(32, body.Length)))} threw", e);
        }
        if (!r.Ok)
        {
            Assert.True(r.Empty, $"{c.Name}: a refused {body.Length}-byte body left a partial message: {r.Message}");
            return false;
        }
        var again = c.Encode(r.Message!);
        Assert.True(again.AsSpan().SequenceEqual(body), $"{c.Name}: an accepted {body.Length}-byte body does not re-encode to its own bytes");
        Assert.True(c.Valid(r.Message!), $"{c.Name}: an accepted {body.Length}-byte body does not pass Validate");
        return true;
    }

    private static byte[] RandomBytes(Random rng, int n)
    {
        var b = new byte[n];
        rng.NextBytes(b);
        return b;
    }

    private static byte[] Mutate(Random rng, byte[] sample)
    {
        var b = (byte[])sample.Clone();
        var at = rng.Next(b.Length);
        b[at] ^= (byte)rng.Next(1, 256);
        return b;
    }

    [Fact]
    public void Every_truncation_and_every_trailing_byte_is_refused()
    {
        var rng = new Random(Seed);
        foreach (var c in Codecs)
            foreach (var sample in Samples(c.Id, H))
            {
                Assert.True(Run(c, sample), $"{c.Name}: the valid {sample.Length}-byte sample is refused");
                for (var n = 0; n < sample.Length; n++)
                    Assert.False(Run(c, sample[..n]), $"{c.Name}: a body truncated to {n} of {sample.Length} bytes is accepted");
                Assert.False(Run(c, [.. sample, 0]), $"{c.Name}: a trailing zero byte is accepted");
                for (var k = 1; k <= 8; k++)
                    Assert.False(Run(c, [.. sample, .. RandomBytes(rng, k)]), $"{c.Name}: {k} trailing bytes are accepted");
            }
    }

    [Fact]
    public void Random_bodies_and_single_byte_mutations_never_throw_or_decode_partly()
    {
        var rng = new Random(Seed);
        foreach (var c in Codecs)
        {
            var samples = Samples(c.Id, H);
            for (var i = 0; i < 3000; i++)
            {
                var n = (i % 4) switch
                {
                    0 => rng.Next(0, 160),
                    1 => Math.Max(0, samples[rng.Next(samples.Length)].Length + rng.Next(-3, 4)),   // near a real length
                    2 => rng.Next(0, CameraWireAbi.MaxBody + 64),
                    _ => samples[rng.Next(samples.Length)].Length,                                     // exactly a real length
                };
                Run(c, RandomBytes(rng, n));
            }
            foreach (var sample in samples)
                for (var i = 0; i < 2000; i++)
                    Run(c, Mutate(rng, sample));
        }
    }

    // A Path request's body with `count` in its key count field and `present` keys of 48 bytes after it (keys at 0, 300, ...).
    private static byte[] RawPath(int count, int present, CameraMessage kind = CameraMessage.Apply)
    {
        var prefix = CameraWire.Mutation(kind, H, 0, Requests.Path(1, 120000), CameraWireAbi.MaxBody)!;
        var head = prefix.Length - 2 - CameraWireAbi.KeyBytes;   // up to the count
        var b = new byte[head + 2 + present * CameraWireAbi.KeyBytes];
        prefix.AsSpan(0, head).CopyTo(b);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(head), (ushort)count);
        for (var i = 0; i < present; i++)
        {
            var k = b.AsSpan(head + 2 + i * CameraWireAbi.KeyBytes, CameraWireAbi.KeyBytes);
            BinaryPrimitives.WriteUInt32LittleEndian(k, (uint)(i * 300));
            BinaryPrimitives.WriteSingleLittleEndian(k[40..], 1f);   // rotation (0, 0, 0, 1)
            BinaryPrimitives.WriteSingleLittleEndian(k[44..], 50f);  // keyed fov
        }
        return b;
    }

    // An Ack whose diagnostic length field says `declared` and that carries the bytes of `text`.
    private static byte[] RawAck(int declared, byte[] text)
    {
        var head = CameraWire.Ack(new AckMessage(H, false, CameraReasons.Bounds, 0, 0, 0, 0, 0, ""));
        var b = new byte[head.Length + text.Length];
        head.CopyTo(b, 0);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(head.Length - 2), (ushort)declared);
        text.CopyTo(b, head.Length);
        return b;
    }

    [Fact]
    public void Counts_and_lengths_at_and_over_the_limits_are_refused()
    {
        foreach (var kind in new[] { CameraMessage.Apply, CameraMessage.Update })
        {
            var c = Of(kind);
            // keys: 256 is the limit; 257 is refused, also when every byte is there; a negotiated 128 refuses 256
            Assert.True(Run(c, RawPath(256, 256, kind)));
            Assert.False(Run(c, RawPath(257, 257, kind)));
            Assert.False(Run(c, CameraWire.Mutation(kind, H, 0, Requests.Path(257, 120000), CameraWireAbi.MaxBody)!));
            Assert.False(Run(Mutation(kind, maxKeys: 128), RawPath(256, 256, kind)));
            // a count that claims more bytes than the body holds
            Assert.False(Run(c, RawPath(200, 1, kind)));
            Assert.False(Run(c, RawPath(256, 255, kind)));
            Assert.False(Run(c, RawPath(ushort.MaxValue, 1, kind)));
            Assert.False(Run(c, RawPath(ushort.MaxValue, 340, kind)));
            // the body: over 16384 is refused before anything else, whatever maximum the caller passes
            var big = RawPath(341, 341, kind);
            Assert.True(big.Length > CameraWireAbi.MaxBody);
            Assert.False(Run(c, big));
            Assert.False(Run(Mutation(kind, maxBody: int.MaxValue, maxKeys: int.MaxValue), big));
            Assert.False(Run(c, [.. RawPath(256, 256, kind), .. new byte[CameraWireAbi.MaxBody - RawPath(256, 256, kind).Length]]));  // 16384 with padding
            Assert.False(Run(c, new byte[CameraWireAbi.MaxBody]));
            Assert.False(Run(c, new byte[CameraWireAbi.MaxBody + 1]));
            Assert.False(Run(Mutation(kind, maxBody: 1000), CameraWire.Mutation(kind, H, 0, Requests.Path(32, 20000), CameraWireAbi.MaxBody)!));
        }
        // the diagnostic: 256 UTF-8 bytes at most, and its length field must match the bytes
        var ack = Of(CameraMessage.Ack);
        Assert.True(Run(ack, RawAck(256, Encoding.UTF8.GetBytes(new string('a', 256)))));
        Assert.False(Run(ack, RawAck(257, Encoding.UTF8.GetBytes(new string('a', 257)))));
        Assert.False(Run(ack, RawAck(258, Encoding.UTF8.GetBytes(new string('ä', 129)))));
        Assert.False(Run(ack, RawAck(300, Encoding.UTF8.GetBytes("short"))));
        Assert.False(Run(ack, RawAck(5, Encoding.UTF8.GetBytes(new string('a', 300)))));
        Assert.False(Run(ack, RawAck(ushort.MaxValue, [])));
        Assert.False(Run(ack, RawAck(1, [0xC3])));        // half a character
        // the negotiated limits in Hello and Welcome
        var hello = Of(CameraMessage.Hello);
        Assert.True(Run(hello, CameraWire.Hello(new HelloMessage(1, 7, 1, 1, 0, 1, 1))));
        Assert.False(Run(hello, CameraWire.Hello(new HelloMessage(1, 7, 1, 1, 0, 0, 16384))));
        Assert.False(Run(hello, CameraWire.Hello(new HelloMessage(1, 7, 1, 1, 0, 256, 0))));
        var welcome = Of(CameraMessage.Welcome);
        WelcomeMessage W(uint maxBody, ushort maxKeys) => new(7, 1, 2, 3, 4, 5, 6, CameraWireAbi.RemoteParts, maxBody, maxKeys);
        Assert.True(Run(welcome, CameraWire.Welcome(W(16384, 256))));
        Assert.False(Run(welcome, CameraWire.Welcome(W(16385, 256))));
        Assert.False(Run(welcome, CameraWire.Welcome(W(16384, 257))));
        Assert.False(Run(welcome, CameraWire.Welcome(W(0, 256))));
    }

    /// <summary>Decoder-refused bodies near a valid one: truncations, trailing bytes, mutations and random bodies.</summary>
    private static IEnumerable<byte[]> Malformed(Codec c, byte[][] samples, Random rng)
    {
        foreach (var sample in samples)
        {
            for (var n = 0; n < sample.Length; n++) yield return sample[..n];
            yield return [.. sample, 0];
            yield return [.. sample, .. RandomBytes(rng, rng.Next(1, 9))];
            for (var i = 0; i < 500; i++) yield return Mutate(rng, sample);
        }
        for (var i = 0; i < 300; i++) yield return RandomBytes(rng, rng.Next(0, 200));
    }

    [Fact]
    public void Malformed_bodies_change_neither_binding_and_send_nothing()
    {
        var rng = new Random(Seed);

        // the server: a bound peer with an active lease and an Update in flight
        var srv = new Rig();
        srv.Bind();
        srv.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 60000));
        srv.Tick();
        srv.Endpoint.Auto = false;
        srv.Submit(TransportOp.UpdatePreserve, lease: 1000, revision: 2, request: Requests.Lens(70, 60000));
        srv.Pump();
        Assert.True(srv.Server.Busy);
        Assert.True(CameraWire.TryHeaderOnly(srv.SentToClient.Last(m => m.Id == CameraMessage.Update).Body, CameraWireAbi.MaxBody, out var inFlight));
        object ServerState() => (srv.Server.State, srv.Server.Binding, srv.Server.ClientCaps, srv.Server.EffectiveMaxBody, srv.Server.HighestSerial,
            srv.Server.Busy, srv.SentToClient.Count, srv.Ended.Count, srv.Results.Count);
        var before = ServerState();
        var fedServer = 0;
        foreach (var c in Codecs)
        {
            var samples = c.Id == CameraMessage.Hello ? [srv.SentToServer.First(m => m.Id == CameraMessage.Hello).Body] : Samples(c.Id, inFlight);
            foreach (var body in Malformed(c, samples, rng))
            {
                if (Run(c, body)) continue;   // only what the decoder refuses
                srv.Server.Receive(c.Id, body);
                fedServer++;
                Assert.Equal(before, ServerState());
            }
        }
        srv.Server.Tick();   // nothing was answered: the Update is still in flight
        Assert.Equal(before, ServerState());

        // the client: bound, holding lease 1000
        var cli = new Rig();
        cli.Bind();
        cli.Submit(TransportOp.Apply, lease: 1000, request: Requests.Lens(durationMs: 60000));
        cli.Tick();
        Assert.Equal(1000ul, cli.Client.ActiveLease);
        var hdr = new WireHeader(cli.Client.Binding, 5, 1000, 9, 0, 2);
        object CameraState() => (cli.Client.State, cli.Client.Binding, cli.Client.ActiveLease, cli.Client.ActiveRevision, cli.Client.TombstoneCount,
            cli.Client.AllowedCaps, cli.Endpoint.Started.Count, cli.Endpoint.Cancelled.Count, cli.Endpoint.Released.Count);
        object AllState() => (CameraState(), cli.Client.HighestSerial, cli.Client.AckCount, cli.SentToServer.Count);
        var camera = CameraState();
        ulong fresh = 1_000_000;
        var refusals = 0;
        foreach (var c in Codecs)
        {
            var mutation = c.Id is CameraMessage.Apply or CameraMessage.Update or CameraMessage.End;
            foreach (var malformed in Malformed(c, Samples(c.Id, hdr), rng))
            {
                var body = malformed;
                if (mutation && body.Length >= 76)
                {
                    // a fresh serial each, so the refusal path runs (not the equal-serial replay or the lower-serial drop)
                    body = (byte[])body.Clone();
                    BinaryPrimitives.WriteUInt64LittleEndian(body.AsSpan(68), ++fresh);
                }
                if (Run(c, body)) continue;
                var all = AllState();
                var sent = cli.SentToServer.Count;
                cli.Client.Receive(c.Id, body);
                if (!mutation)
                {
                    Assert.Equal(all, AllState());
                    continue;
                }
                // a malformed mutation changes no camera; on this binding it may get only its bounded refusal
                Assert.Equal(camera, CameraState());
                foreach (var (id, ack) in cli.SentToServer.Skip(sent))
                {
                    Assert.Equal(CameraMessage.Ack, id);
                    Assert.True(CameraWire.TryAck(ack, out var a));
                    Assert.Equal((false, CameraReasons.MalformedRemote), (a.Ok, a.Reason));
                    refusals++;
                }
            }
        }
        Assert.True(fedServer > 1000 && refusals > 100, $"the fuzz fed {fedServer} bodies to the server and drew {refusals} refusals");
    }
}
