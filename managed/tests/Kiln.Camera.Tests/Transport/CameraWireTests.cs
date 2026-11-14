// SPDX-License-Identifier: MIT
// The wire of channel kiln.camera v1 (chapter 03 §4.6): fixed ids, the layout's sizes (the reference model's numbers),
// round trips of every message, and the limits checked before allocation on decode.
using System.Buffers.Binary;
using Kiln.CameraTransport;
using Xunit;

namespace Kiln.CameraTests.Transport;

public sealed class CameraWireTests
{
    private static readonly WireBinding B = new(0xABCDEF, 0x5E4E, 9, 0xC11E47, 3, 42, 77);
    private static readonly WireHeader H = new(B, 5, 1000, 3, 17, 2);

    [Fact]
    public void Message_ids_are_the_chapter_s_and_never_move()
    {
        Assert.Equal([0, 1, 2, 3, 4, 5, 6, 7, 8], Enum.GetValues<CameraMessage>().Select(m => (int)m));
        Assert.Equal(
            ["Hello", "Welcome", "Apply", "Update", "End", "Ack", "Ended", "Heartbeat", "Status"],
            Enum.GetValues<CameraMessage>().Select(m => m.ToString()));
        Assert.Equal((27, 13), (CameraReasons.BodyTooLarge, CameraEndReasons.CancelledBeforeApply));
    }

    [Fact]
    public void A_256_key_request_round_trips_under_16384_bytes()
    {
        var r = Requests.Path();
        var apply = CameraWire.Mutation(CameraMessage.Apply, H, 0, r, CameraWireAbi.MaxBody)!;
        // the reference model's arithmetic: header 84, fixed blob 22, count 2, 256 keys of 48, shake 16 (keyed lens: no static f32)
        Assert.Equal(84 + 22 + 16 + 2 + 256 * 48, apply.Length);
        Assert.True(apply.Length < CameraWireAbi.MaxBody);
        Assert.True(CameraWire.TryMutation(CameraMessage.Apply, apply, CameraWireAbi.MaxBody, CameraWireAbi.MaxKeys, out var m));
        Assert.Equal(H, m!.Header);
        Assert.True(r.SameAs(m.Request));
        var update = CameraWire.Mutation(CameraMessage.Update, H, CameraWireAbi.TimingRestart, r, CameraWireAbi.MaxBody)!;
        Assert.Equal(apply.Length + 1, update.Length);
        Assert.True(CameraWire.TryMutation(CameraMessage.Update, update, CameraWireAbi.MaxBody, CameraWireAbi.MaxKeys, out var u));
        Assert.Equal(CameraWireAbi.TimingRestart, u!.Timing);
        Assert.True(r.SameAs(u.Request));
    }

    [Fact]
    public void The_layout_has_the_reference_model_s_sizes()
    {
        var oneKey = CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Path(1, 1000), CameraWireAbi.MaxBody)!;
        var twoKeys = CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Path(2, 1000), CameraWireAbi.MaxBody)!;
        Assert.Equal(CameraWireAbi.KeyBytes, twoKeys.Length - oneKey.Length);
        Assert.Equal(84, CameraWire.Mutation(CameraMessage.End, H, 0, null, CameraWireAbi.MaxBody)!.Length);
        Assert.Equal(84 + 22 + 4, CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Lens(), CameraWireAbi.MaxBody)!.Length);
        Assert.Equal(84 + 22 + 42 + 16, CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Profile(), CameraWireAbi.MaxBody)!.Length);
        // a key: at_ms u32, then three little-endian doubles, then the floats
        var key = oneKey.AsSpan(84 + 22 + 16 + 2);
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(key));
        Assert.Equal(1000.5, BinaryPrimitives.ReadDoubleLittleEndian(key[4..]));
        Assert.Equal(1e6, BinaryPrimitives.ReadDoubleLittleEndian(key[20..]));
        Assert.Equal(40f, BinaryPrimitives.ReadSingleLittleEndian(key[44..]));
        // the common header: token first, revision last
        Assert.Equal(0xABCDEFul, BinaryPrimitives.ReadUInt64LittleEndian(oneKey));
        Assert.Equal(2ul, BinaryPrimitives.ReadUInt64LittleEndian(oneKey.AsSpan(76)));
    }

    [Fact]
    public void Every_message_round_trips()
    {
        var hello = new HelloMessage(1, 0xC11E47, 3, 1001, CameraWireAbi.RemoteParts, 256, 16384);
        Assert.True(CameraWire.TryHello(CameraWire.Hello(hello), out var h2));
        Assert.Equal(hello, h2);
        var welcome = new WelcomeMessage(0xC11E47, 3, 0x5E4E, 9, 42, 77, 0xABCDEF, CameraWireAbi.CapLocalFov, 9000, 128);
        Assert.True(CameraWire.TryWelcome(CameraWire.Welcome(welcome), out var w2));
        Assert.Equal(welcome, w2);
        var ack = new AckMessage(H, false, CameraReasons.NoCameraFrame, CameraWireAbi.CommitNone, CameraWireAbi.FlagMayHaveApplied, 9, 10, 20, "no frame ä");
        Assert.True(CameraWire.TryAck(CameraWire.Ack(ack), out var a2));
        Assert.Equal(ack, a2);
        var ended = new EndedMessage(H, CameraEndReasons.Expired, CameraReasons.None, 77);
        Assert.True(CameraWire.TryEnded(CameraWire.Ended(ended), out var e2));
        Assert.Equal(ended, e2);
        var hb = new HeartbeatMessage(B, 4, 17, 1000, 2);
        Assert.True(CameraWire.TryHeartbeat(CameraWire.Heartbeat(hb), out var hb2));
        Assert.Equal(hb, hb2);
        var st = new StatusMessage(B, 4, 0, 0, 0, 0);
        Assert.True(CameraWire.TryStatus(CameraWire.Status(st), out var st2));
        Assert.Equal(st, st2);
        var profile = CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Profile(), CameraWireAbi.MaxBody)!;
        Assert.True(CameraWire.TryMutation(CameraMessage.Apply, profile, CameraWireAbi.MaxBody, 256, out var p2));
        Assert.True(Requests.Profile().SameAs(p2!.Request));
        var mode = new CameraWireRequest { Parts = CameraWireAbi.PartMode, InputPolicy = CameraWireAbi.StockMode, DurationMs = 5000, CameraId = 34, Transition = 2 };
        Assert.True(CameraWire.TryMutation(CameraMessage.Apply, CameraWire.Mutation(CameraMessage.Apply, H, 0, mode, CameraWireAbi.MaxBody), CameraWireAbi.MaxBody, 256, out var m2));
        Assert.Equal(mode, m2!.Request! with { Keys = mode.Keys });
        Assert.True(CameraWire.TryMutation(CameraMessage.End, CameraWire.Mutation(CameraMessage.End, H, 0, null, CameraWireAbi.MaxBody), CameraWireAbi.MaxBody, 256, out var end));
        Assert.Null(end!.Request);
    }

    [Fact]
    public void An_oversized_request_is_not_encoded_and_an_oversized_body_is_not_decoded()
    {
        Assert.Null(CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Path(), 4000));
        var apply = CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Path(), CameraWireAbi.MaxBody)!;
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, apply, 4000, 256, out _));   // the negotiated body maximum
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, new byte[CameraWireAbi.MaxBody + 1], int.MaxValue, 256, out _));  // 16384 in any case
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, apply, CameraWireAbi.MaxBody, 128, out _));  // more keys than negotiated
    }

    [Fact]
    public void A_key_count_is_checked_against_the_limit_and_the_bytes_before_the_array_is_made()
    {
        var oneKey = CameraWire.Mutation(CameraMessage.Apply, H, 0, Requests.Path(1, 1000), CameraWireAbi.MaxBody)!;
        var countAt = 84 + 22 + 16;
        var lying = (byte[])oneKey.Clone();
        BinaryPrimitives.WriteUInt16LittleEndian(lying.AsSpan(countAt), 60000);   // claims 60000 keys in a body holding one
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, lying, CameraWireAbi.MaxBody, 256, out _));
        BinaryPrimitives.WriteUInt16LittleEndian(lying.AsSpan(countAt), 257);
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, lying, CameraWireAbi.MaxBody, 256, out _));
        BinaryPrimitives.WriteUInt16LittleEndian(lying.AsSpan(countAt), 0);
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, lying, CameraWireAbi.MaxBody, 256, out _));
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, oneKey[..^1], CameraWireAbi.MaxBody, 256, out _));         // truncated
        Assert.False(CameraWire.TryMutation(CameraMessage.Apply, [.. oneKey, 0], CameraWireAbi.MaxBody, 256, out _));       // trailing byte
        Assert.Equal(CameraReasons.Bounds, CameraWire.Validate(Requests.Path(257, 120000)));
    }

    public static TheoryData<string> Corruptions() => ["parts 32", "parts 0", "policy 3", "lens source 3", "rotation 3", "collision 1", "fov NaN",
        "camera id 46", "transition 3", "timing 2", "path and profile", "mode and lens", "keyed fov without keys", "first key not at 0",
        "keys out of order", "key after duration", "infinite position", "quaternion not unit", "duration 0", "duration 120001",
        "blends past duration", "priority 1001", "distance 0.2", "shake amplitude 0.6", "falloff past duration", "lens 4 degrees"];

    [Theory]
    [MemberData(nameof(Corruptions))]
    public void A_wrong_enum_part_or_number_decodes_to_nothing(string what)
    {
        CameraWireRequest r = what switch
        {
            "parts 32" => Requests.Lens() with { Parts = CameraWireAbi.PartLens | 32 },
            "parts 0" => Requests.Lens() with { Parts = 0, LensSource = 0 },
            "policy 3" => Requests.Lens() with { InputPolicy = 3 },
            "lens source 3" => Requests.Lens() with { LensSource = 3 },
            "rotation 3" => Requests.Profile() with { RotationMode = 3 },
            "collision 1" => Requests.Profile() with { Collision = 1 },
            "fov NaN" => Requests.Lens(float.NaN),
            "camera id 46" => new CameraWireRequest { Parts = CameraWireAbi.PartMode, DurationMs = 1000, CameraId = 46 },
            "transition 3" => new CameraWireRequest { Parts = CameraWireAbi.PartMode, DurationMs = 1000, Transition = 3 },
            "timing 2" => Requests.Lens(),
            "path and profile" => Requests.Profile() with { Parts = CameraWireAbi.PartProfile | CameraWireAbi.PartPath, Keys = Requests.Path(1, 1000).Keys },
            "mode and lens" => Requests.Lens() with { Parts = CameraWireAbi.PartLens | CameraWireAbi.PartMode },
            "keyed fov without keys" => Requests.Path(2, 1000) with { LensSource = CameraWireAbi.LensConstant, FovDegrees = 50 },
            "first key not at 0" => Requests.Path(2, 1000) with { Keys = [Requests.Path(2, 1000).Keys[0] with { AtMs = 1 }, Requests.Path(2, 1000).Keys[1]] },
            "keys out of order" => Requests.Path(3, 2000) with { Keys = [.. Requests.Path(3, 2000).Keys.Select((k, i) => i == 2 ? k with { AtMs = 400 } : k)] },
            "key after duration" => Requests.Path(3, 700) with { BlendInMs = 0, BlendOutMs = 0 },
            "infinite position" => Requests.Path(1, 1000) with { Keys = [Requests.Path(1, 1000).Keys[0] with { X = double.PositiveInfinity }] },
            "quaternion not unit" => Requests.Path(1, 1000) with { Keys = [Requests.Path(1, 1000).Keys[0] with { Qw = 0.9f }] },
            "duration 0" => Requests.Lens(durationMs: 0),
            "duration 120001" => Requests.Lens(durationMs: 120001),
            "blends past duration" => Requests.Lens(durationMs: 1000) with { BlendInMs = 600, BlendOutMs = 500 },
            "priority 1001" => Requests.Lens() with { Priority = 1001 },
            "distance 0.2" => Requests.Profile() with { Distance = 0.2f },
            "shake amplitude 0.6" => Requests.Profile() with { Amplitude = 0.6f },
            "falloff past duration" => Requests.Profile() with { FalloffMs = 9000 },
            "lens 4 degrees" => Requests.Lens(4),
            _ => throw new ArgumentException(what),
        };
        if (what != "timing 2") Assert.NotEqual(CameraReasons.None, CameraWire.Validate(r));
        var kind = what == "timing 2" ? CameraMessage.Update : CameraMessage.Apply;
        // encoded past the validation, the receiver must refuse it
        var body = Raw(kind, r, what == "timing 2" ? (byte)2 : (byte)0);
        Assert.False(CameraWire.TryMutation(kind, body, CameraWireAbi.MaxBody, 256, out _));
        Assert.False(CameraWire.TryHeaderOnly(body[..83], CameraWireAbi.MaxBody, out _));
    }

    // CameraWire.Mutation encodes whatever it is given (validation is Submit's and the receiver's): the test's sender
    private static byte[] Raw(CameraMessage kind, CameraWireRequest r, byte timing) => CameraWire.Mutation(kind, H, timing, r, CameraWireAbi.MaxBody)!;

    [Fact]
    public void Bad_framing_of_the_small_messages_is_refused()
    {
        var ack = CameraWire.Ack(new AckMessage(H, true, 0, 2, 0, 1, 2, 3, ""));
        Assert.False(CameraWire.TryAck([.. ack, 1], out _));                                       // trailing
        var badOk = (byte[])ack.Clone();
        badOk[84] = 2;
        Assert.False(CameraWire.TryAck(badOk, out _));                                               // ok is 0 or 1
        var badUtf8 = CameraWire.Ack(new AckMessage(H, true, 0, 2, 0, 1, 2, 3, "ab"));
        badUtf8[^1] = 0xFF;
        Assert.False(CameraWire.TryAck(badUtf8, out _));                                             // not UTF-8
        var hello = CameraWire.Hello(new HelloMessage(2, 1, 1, 1, 0, 256, 16384));
        Assert.False(CameraWire.TryHello(hello, out _));                                             // protocol 2
        Assert.False(CameraWire.TryHeartbeat(CameraWire.Heartbeat(new HeartbeatMessage(B, 1, 1, 5, 0)), out _));  // lease without revision
        Assert.False(CameraWire.TryEnded(CameraWire.Ended(new EndedMessage(H, 14, 0, 0)), out _));   // end reason past 13
        Assert.False(CameraWire.TryWelcome(CameraWire.Welcome(new WelcomeMessage(1, 1, 1, 1, 1, 1, 1, 1ul << 6, 100, 1)), out _));  // remote bit is not a part
    }

    [Fact]
    public void A_diagnostic_is_cut_to_256_bytes_at_a_character_boundary()
    {
        var text = new string('a', 255) + "äöü";
        var cut = CameraWire.Diagnostic(text);
        Assert.Equal(255, cut.Length);   // 'ä' would straddle byte 256: left out whole
        Assert.True(CameraWire.TryAck(CameraWire.Ack(new AckMessage(H, false, 1, 0, 0, 0, 0, 0, text)), out var a));
        Assert.Equal(new string('a', 255), a.Diagnostic);
    }
}
