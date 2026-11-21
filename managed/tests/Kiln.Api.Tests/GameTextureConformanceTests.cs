// SPDX-License-Identifier: MIT
using System.Buffers.Binary;
using System.Text;
using System.Text.Json;
using Xunit;

namespace Kiln.Api.Tests;

/// <summary>
/// GameTexture against the conformance fixtures of kiln mode's Lua functions image.decode_texture and
/// image.encode_texture (crates/sfi/sfi-core/tests/conformance/texture): the same cases as decode.lua and encode.lua
/// (written by sfi-core's fixture generator, rebuilt here case for case: the same inputs, the same digests), checked
/// against the same golden files. sfi-core's texture_conformance test runs the Lua fixtures against those goldens, so a
/// pass on both sides means the C# API and the Lua functions answer the same. Every record of both goldens is checked.
/// </summary>
public sealed class GameTextureConformanceTests
{
    // the game's own names, the fixtures' (the members' names are their .NET forms)
    private static readonly string[] Names = Enum.GetValues<GamePixelFormat>().Select(PixelCodec.GameName).ToArray();

    private sealed class Golden
    {
        private readonly Dictionary<string, JsonElement> _records = new(StringComparer.Ordinal);
        private readonly HashSet<string> _checked = new(StringComparer.Ordinal);
        public readonly List<string> Failures = [];

        public Golden(string file)
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(ConformanceDir(), file)));
            Assert.Equal(2, doc.RootElement.GetProperty("version").GetInt32());
            foreach (var r in doc.RootElement.GetProperty("records").EnumerateArray())
                _records.Add(r.GetProperty("label").GetString()!, r.Clone());
        }

        private JsonElement Take(string label)
        {
            Assert.True(_records.TryGetValue(label, out var r), $"no golden record {label}");
            Assert.True(_checked.Add(label), $"record {label} checked twice");
            return r;
        }

        private static string Text(JsonElement value) => value.GetProperty("t").GetString() switch
        {
            "string" => JsonSerializer.Deserialize<string>(value.GetProperty("v").GetString()!)!,
            "boolean" => value.GetProperty("v").GetBoolean() ? "true" : "false",
            _ => value.GetProperty("v").GetString()!,
        };

        private void Expect(string label, string expected, string actual)
        {
            if (expected != actual) Failures.Add($"{label}: golden {expected}, C# {actual}");
        }

        /// <summary>A T.call record: its values, or for a failed call the first line of its message.</summary>
        public void Call(string label, Func<string[]> run)
        {
            var r = Take(label);
            string[]? actual = null;
            Exception? error = null;
            try { actual = run(); }
            catch (Exception e) { error = e; }
            if (r.GetProperty("ok").GetBoolean())
            {
                var expected = r.GetProperty("values").EnumerateArray().Select(Text).ToArray();
                Expect(label, string.Join(" ", expected), error is null ? string.Join(" ", actual!) : $"threw {error.GetType().Name}: {error.Message}");
            }
            else
            {
                var message = r.GetProperty("message").GetString()!.Split('\n')[0];
                Expect(label, message, error is null ? "no failure" : Prefix(label) + error.Message);
            }
        }

        private static string Prefix(string label) => label.StartsWith("enc.", StringComparison.Ordinal) ? "failed to encode image: " : "failed to decode texture: ";

        /// <summary>A fails() record: whether the call failed.</summary>
        public void Fails(string label, Action run)
        {
            var r = Take(label + ".fails");
            var failed = false;
            try { run(); }
            catch (Exception) { failed = true; }
            Expect(label + ".fails", Text(r.GetProperty("values")[0]), failed ? "true" : "false");
        }

        /// <summary>A T.value record of one value.</summary>
        public void Value(string label, string actual) => Expect(label, Text(Take(label).GetProperty("values")[0]), actual);

        public void AllChecked() =>
            Assert.True(_checked.Count == _records.Count, "records not checked: " + string.Join(", ", _records.Keys.Where(k => !_checked.Contains(k)).Take(20)));
    }

    private static string ConformanceDir([System.Runtime.CompilerServices.CallerFilePath] string source = "")
    {
        // from this source file up to the repository (the build output may live outside it)
        for (var dir = new DirectoryInfo(Path.GetDirectoryName(source)!); dir is not null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "crates", "sfi", "sfi-core", "tests", "conformance", "texture");
            if (Directory.Exists(candidate)) return candidate;
        }
        throw new DirectoryNotFoundException("crates/sfi/sfi-core/tests/conformance/texture above " + source);
    }

    // ---- the fixtures' helpers --------------------------------------------------------------------------------

    private static byte[] Bytes(int n, long seed)
    {
        var x = seed;
        var t = new byte[n];
        for (var i = 0; i < n; i++)
        {
            x = (x * 1103515245 + 12345) & 0x7fffffff;
            t[i] = (byte)((x >> 16) & 255);
        }
        return t;
    }

    /// <summary>CRC-64/ECMA-182 (hasher.crc64): polynomial 0x42F0E1EBA9EA3693, no reflection, no final xor.</summary>
    private static long Crc64(ReadOnlySpan<byte> data)
    {
        ulong crc = 0;
        foreach (var b in data)
        {
            crc ^= (ulong)b << 56;
            for (var i = 0; i < 8; i++) crc = (crc & 0x8000000000000000) != 0 ? (crc << 1) ^ 0x42F0E1EBA9EA3693 : crc << 1;
        }
        return (long)crc;
    }

    private static string Hex(ReadOnlySpan<byte> s) => Convert.ToHexStringLower(s);

    private static string[] Digest(byte[] s) => [s.Length.ToString(), Crc64(s).ToString(), Hex(s.AsSpan(0, Math.Min(24, s.Length)))];

    private static GamePixelFormat F(string name) => Enum.GetValues<GamePixelFormat>().Single(f => PixelCodec.GameName(f) == name);

    private static string[] Dec(byte[] data, string n, int w, int h) => Digest(GameTexture.Decode(data, F(n), w, h));

    private static byte[] Enc(byte[] rgba, int w, int h, string n) => GameTexture.Encode(rgba, w, h, F(n));

    /// <summary>model.py's parse: (channel count, bits of the first component, kind) of a plain format, or null.</summary>
    private static (int Channels, int Bits, string Kind)? Parse(string n)
    {
        var m = System.Text.RegularExpressions.Regex.Match(n, @"^([RGBA])(\d+)(?:([RGBA])\d+)?(?:([RGBA])\d+)?(?:([RGBA])\d+)?_([a-z]+)(_pack32)?$");
        if (!m.Success) return null;
        var chans = string.Concat(new[] { 1, 3, 4, 5 }.Select(i => m.Groups[i].Value));
        if (chans is not ("R" or "RG" or "RGB" or "BGR" or "RGBA" or "BGRA" or "ABGR")) return null;
        return (chans.Length, int.Parse(m.Groups[2].Value), m.Groups[6].Value);
    }

    private static bool Refused(string n) =>
        n == "None" || n.StartsWith("A2", StringComparison.Ordinal) || n.StartsWith('D') || n.StartsWith("S8", StringComparison.Ordinal) || n.StartsWith("X8", StringComparison.Ordinal)
        || (n.Contains("_srgb", StringComparison.Ordinal) && !n.StartsWith("BC", StringComparison.Ordinal)) || n == "BC6H_sfloat_block";

    private static int Bpp(string n)
    {
        if (n is "B10G11R11_ufloat_pack32" or "E5B9G9R9_ufloat_pack32") return 4;
        if (n.EndsWith("pack8", StringComparison.Ordinal)) return 1;
        if (n.EndsWith("pack16", StringComparison.Ordinal)) return 2;
        var p = Parse(n)!.Value;
        return p.Bits / 8 * p.Channels;
    }

    private static bool WideInt(string n) => Parse(n) is { } p && p.Bits >= 32 && p.Kind is "uint" or "sint";

    // ---- decode.lua ---------------------------------------------------------------------------------------------

    [Fact]
    public void Decode_answers_as_the_lua_fixture_golden()
    {
        var g = new Golden("decode.golden.json");
        foreach (var n in Names)
        {
            if (Refused(n))
            {
                g.Fails($"dec.{n}", () => GameTexture.Decode(new byte[64], F(n), 1, 1));
                g.Fails($"dec.{n}.0x0", () => GameTexture.Decode([], F(n), 0, 0));
                continue;
            }
            if (n.StartsWith("BC", StringComparison.Ordinal))
            {
                var bs = n.StartsWith("BC1", StringComparison.Ordinal) || n.StartsWith("BC4", StringComparison.Ordinal) ? 8 : 16;
                var d = Bytes(bs * 16, n.Length);
                g.Call($"dec.{n}.16x16", () => Dec(d, n, 16, 16));
                g.Call($"dec.{n}.8x4.long", () => Dec(d, n, 8, 4));
                g.Call($"dec.{n}.0x5", () => Dec([], n, 0, 5));
                g.Fails($"dec.{n}.short", () => GameTexture.Decode(d.AsSpan(0, bs * 16 - 1), F(n), 16, 16));
                foreach (var (w, h) in new[] { (5, 4), (4, 3), (1, 1) })
                    g.Fails($"dec.{n}.{w}x{h}", () => GameTexture.Decode(d, F(n), w, h));
                continue;
            }
            var b = Bpp(n);
            var data = Bytes(b * 1024, b * 31 + n.Length);
            if (WideInt(n))
            {
                g.Fails($"dec.{n}", () => GameTexture.Decode(data, F(n), 32, 32));
                g.Fails($"dec.{n}.1x1", () => GameTexture.Decode(data, F(n), 1, 1));
                g.Call($"dec.{n}.0x3", () => Dec([], n, 0, 3));
            }
            else
            {
                g.Call($"dec.{n}", () => Dec(data, n, 32, 32));
                g.Call($"dec.{n}.long", () => Dec(data, n, 5, 3));
                g.Call($"dec.{n}.0x0", () => Dec([], n, 0, 0));
                g.Fails($"dec.{n}.short", () => GameTexture.Decode(data.AsSpan(0, b * 1024 - 1), F(n), 32, 32));
            }
        }

        foreach (var n in new[] { "R16_unorm", "R16_snorm", "R16_uscaled", "R16_sscaled", "R16_uint", "R16_sint", "R16_sfloat", "R16G16B16A16_sfloat" })
        {
            var t = new byte[65536 * 2];
            for (var v = 0; v < 65536; v++) BinaryPrimitives.WriteUInt16LittleEndian(t.AsSpan(v * 2), (ushort)v);
            var a16 = n.Contains("A16", StringComparison.Ordinal);
            var w = a16 ? 128 : 256;
            g.Call($"all16.{n}", () => Dec(t, n, w, (a16 ? 16384 : 65536) / w));
        }

        var f32 = new List<byte[]>();
        var f64 = new List<byte[]>();
        for (var k = 0; k <= 256; k++)
        {
            var b32 = BitConverter.SingleToUInt32Bits((float)(k / 255.0));
            var b64 = BitConverter.DoubleToInt64Bits(k / 255.0);
            for (var d = -3; d <= 3; d++)
            {
                f32.Add(BitConverter.GetBytes((uint)((b32 + d) & 0xffffffff)));
                f64.Add(BitConverter.GetBytes(b64 + d < 0 ? 0 : b64 + d));
            }
        }
        foreach (var v in new[] { double.PositiveInfinity, double.NegativeInfinity, double.NaN, 1e30, -1e30, 255.0, -0.0, 0.5 })
        {
            f32.Add(BitConverter.GetBytes((float)v));
            f64.Add(BitConverter.GetBytes(v));
        }
        static byte[] Cat(IEnumerable<byte[]> parts) => parts.SelectMany(p => p).ToArray();
        g.Call("bound.R32_sfloat", () => Dec(Cat(f32), "R32_sfloat", f32.Count, 1));
        g.Call("bound.R64_sfloat", () => Dec(Cat(f64), "R64_sfloat", f64.Count, 1));
        g.Call("bound.R32G32B32A32_sfloat", () => Dec(Cat(f32.Take(1796)), "R32G32B32A32_sfloat", 449, 1));
        g.Call("bound.R64G64B64A64_sfloat", () => Dec(Cat(f64.Take(1796)), "R64G64B64A64_sfloat", 449, 1));

        foreach (var n in new[] { "B10G11R11_ufloat_pack32", "E5B9G9R9_ufloat_pack32" })
        {
            var walk = new List<uint>();
            for (uint v = 0; v <= 2047; v++) { walk.Add(v); walk.Add(v << 11); }
            for (uint v = 0; v <= 1023; v++) walk.Add(v << 22);
            var mix = new List<uint>();
            for (ulong v = 0; v <= 4095; v++) mix.Add((uint)((v * 2654435761) & 0xffffffff));
            static byte[] Words(List<uint> words) => words.SelectMany(BitConverter.GetBytes).ToArray();
            g.Call($"walk.{n}", () => Dec(Words(walk), n, walk.Count, 1));
            g.Call($"mix.{n}", () => Dec(Words(mix), n, 64, 64));
        }

        Assert.True(g.Failures.Count == 0, string.Join("\n", g.Failures.Take(40)));
        g.AllChecked();
    }

    // ---- encode.lua ---------------------------------------------------------------------------------------------

    [Fact]
    public void Encode_answers_as_the_lua_fixture_golden()
    {
        var g = new Golden("encode.golden.json");
        var rows = new List<byte>();
        for (var c = 0; c < 4; c++)
            for (var x = 0; x < 256; x++)
            {
                var p = new byte[4];
                p[c] = (byte)x;
                rows.AddRange(p);
            }
        for (var x = 0; x < 256; x++) rows.AddRange(new[] { (byte)x, (byte)x, (byte)x, (byte)x });
        for (var x = 0; x < 256; x++) rows.AddRange(new[] { (byte)x, (byte)(255 - x), (byte)(x ^ 0x55), (byte)((x * 7) & 255) });
        var sweep = rows.ToArray(); // 256 x 6
        var small = Bytes(24, 7); // 3 x 2
        var one = new byte[4];
        var bc = new[] { (4, 4, 11), (8, 8, 12), (16, 8, 13), (12, 4, 14) };

        foreach (var n in Names)
        {
            if (n == "None")
            {
                g.Fails("enc.None", () => Enc(one, 1, 1, n));
                g.Fails("enc.None.0x0", () => Enc([], 0, 0, n));
                continue;
            }
            if (Refused(n) || n is "B10G11R11_ufloat_pack32" or "E5B9G9R9_ufloat_pack32")
            {
                g.Call($"enc.{n}", () => [Hex(Enc(sweep, 256, 6, n))]);
                g.Call($"enc.{n}.0x0", () => [Hex(Enc([], 0, 0, n))]);
                continue;
            }
            if (n.StartsWith("BC", StringComparison.Ordinal))
            {
                foreach (var (w, h, seed) in bc)
                    g.Call($"enc.{n}.{w}x{h}", () => Digest(Enc(Bytes(w * h * 4, seed), w, h, n)));
                foreach (var (w, h) in new[] { (0, 0), (4, 0), (0, 8) })
                    g.Call($"enc.{n}.{w}x{h}", () => Digest(Enc(new byte[w * h * 4], w, h, n)));
                foreach (var (w, h) in new[] { (8, 3), (5, 4), (1, 1), (5, 0) })
                    g.Fails($"enc.{n}.{w}x{h}", () => Enc(new byte[w * h * 4], w, h, n));
                continue;
            }
            var (channels, bits, kind) = Parse(n) ?? (0, 8, "unorm");
            var narrow = n.EndsWith("pack8", StringComparison.Ordinal) || n.EndsWith("pack16", StringComparison.Ordinal) || bits == 8 || kind is "unorm" or "uscaled" or "uint";
            if (narrow)
            {
                g.Call($"enc.{n}", () => Digest(Enc(sweep, 256, 6, n)));
                g.Call($"enc.{n}.3x2", () => Digest(Enc(small, 3, 2, n)));
            }
            else
            {
                g.Fails($"enc.{n}", () => Enc(sweep, 256, 6, n));
                g.Fails($"enc.{n}.2x1", () => Enc(new byte[8], 2, 1, n));
                if (channels == 1)
                {
                    var t = new List<byte[]>();
                    for (var v = 0; v < 256; v++) t.Add(Enc([(byte)v, (byte)(255 - v), (byte)(v ^ 0x55), 7], 1, 1, n));
                    g.Value($"enc.{n}.single", Crc64(t.SelectMany(e => e).ToArray()).ToString());
                    g.Value($"enc.{n}.single128", Hex(t[128]));
                }
                else
                {
                    g.Fails($"enc.{n}.1x1", () => Enc(one, 1, 1, n));
                }
            }
            g.Call($"enc.{n}.0x0", () => Digest(Enc([], 0, 0, n)));
        }

        Assert.True(g.Failures.Count == 0, string.Join("\n", g.Failures.Take(40)));
        g.AllChecked();
    }

    // ---- arguments.lua: the records that are not about Lua argument conversion -----------------------------------

    [Fact]
    public void Arguments_that_apply_to_csharp_answer_as_the_golden()
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(Path.Combine(ConformanceDir(), "arguments.golden.json")));
        var records = doc.RootElement.GetProperty("records").EnumerateArray().ToDictionary(r => r.GetProperty("label").GetString()!, r => r.Clone());
        byte[] rgba = [1, 2, 3, 4, 5, 6, 7, 8];
        string Px(byte[] pixels, int w, int h) => $"{w} {h} {Hex(pixels)}";
        string Expected(string label) => string.Join(" ", records[label].GetProperty("values").EnumerateArray().Select(v =>
            v.GetProperty("t").GetString() == "string" ? JsonSerializer.Deserialize<string>(v.GetProperty("v").GetString()!)! : v.GetProperty("v").ToString()));

        // The Lua function ignores its mip argument: the first level is what Decode gives.
        Assert.Equal(Expected("mip.nil"), Px(GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 2, 1), 2, 1));
        Assert.Equal(Expected("mip.7"), Px(GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 2, 1), 2, 1));
        Assert.True(records["dim.wrap.fails"].GetProperty("values")[0].GetProperty("v").GetBoolean());
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 65536, 65536));
        Assert.Throws<ArgumentOutOfRangeException>(() => GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 1073741825, 1));
        Assert.True(records["dim.empty.fails"].GetProperty("values")[0].GetProperty("v").GetBoolean());
        Assert.Throws<ArgumentException>(() => GameTexture.Decode([], GamePixelFormat.R8G8B8A8Unorm, 1, 1));
        Assert.Equal(Expected("dim.zero_w"), Px(GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 0, 5), 0, 5));
        Assert.Equal(Expected("dim.zero_h"), Px(GameTexture.Decode(rgba, GamePixelFormat.R8G8B8A8Unorm, 5, 0), 5, 0));
        var encoded = GameTexture.Encode(rgba, 2, 1, GamePixelFormat.R8G8B8A8Unorm);
        Assert.Equal(Expected("enc.extra"), $"Buffer {encoded.Length} 0 {Hex(encoded)}");
        Assert.Equal(Expected("roundtrip"), Px(GameTexture.Decode(GameTexture.Encode(rgba, 2, 1, GamePixelFormat.B8G8R8A8Unorm), GamePixelFormat.B8G8R8A8Unorm, 2, 1), 2, 1));
    }
}
