// SPDX-License-Identifier: MIT
// Ported from block_compression 0.6.0 (src/encode.rs, src/encode/bc1_to_5.rs, src/encode/common.rs), MIT,
// Copyright (c) 2025 Nils Hasenbanck, Copyright (c) 2016-2024 Intel Corporation (licenses/block_compression-LICENSE.txt),
// itself derived from Intel's ISPC Texture Compressor. Kiln's texture code for kiln mode
// (crates/sfi/sfi-core/src/eml/texture.rs) encodes with that crate (BC6H "basic", BC7 "alpha basic" settings); this is
// the same code in C#, operation for operation in single-precision floats, so both write the same bytes.
using System.Buffers.Binary;

namespace Kiln;

/// <summary>RGBA8 to BC1 to BC7 blocks, as block_compression's <c>compress_rgba8</c> does it.</summary>
internal static partial class BlockEncoder
{
    /// <summary>Encodes <paramref name="rgba"/> (tightly packed, the size a multiple of 4) into <paramref name="blocks"/>.</summary>
    public static void Encode(BlockKind kind, ReadOnlySpan<byte> rgba, Span<byte> blocks, int width, int height)
    {
        var stride = width * 4;
        var blockWidth = (width + 3) / 4;
        var blockHeight = (height + 3) / 4;
        var bc15 = new Bc15();
        Span<uint> data = stackalloc uint[4];
        for (var yy = 0; yy < blockHeight; yy++)
        {
            for (var xx = 0; xx < blockWidth; xx++)
            {
                switch (kind)
                {
                    case BlockKind.Bc1:
                    {
                        bc15.Clear();
                        bc15.LoadInterleavedRgba(rgba, xx, yy, stride);
                        var (c0, c1) = bc15.CompressBc1Core();
                        data[0] = c0; data[1] = c1;
                        Store(blocks, blockWidth, xx, yy, data[..2]);
                        break;
                    }
                    case BlockKind.Bc2:
                    {
                        bc15.Clear();
                        var (a0, a1) = Bc15.LoadAlpha4Bit(rgba, xx, yy, stride);
                        bc15.LoadInterleavedRgba(rgba, xx, yy, stride);
                        var (c0, c1) = bc15.CompressBc1Core();
                        data[0] = a0; data[1] = a1; data[2] = c0; data[3] = c1;
                        Store(blocks, blockWidth, xx, yy, data);
                        break;
                    }
                    case BlockKind.Bc3:
                    {
                        bc15.Clear();
                        bc15.LoadInterleavedRgba(rgba, xx, yy, stride);
                        var (a0, a1) = bc15.CompressBc3Alpha();
                        var (c0, c1) = bc15.CompressBc1Core();
                        data[0] = a0; data[1] = a1; data[2] = c0; data[3] = c1;
                        Store(blocks, blockWidth, xx, yy, data);
                        break;
                    }
                    case BlockKind.Bc4:
                    {
                        bc15.Clear();
                        bc15.LoadChannel(rgba, xx, yy, stride, 0);
                        var (r0, r1) = bc15.CompressBc3Alpha();
                        data[0] = r0; data[1] = r1;
                        Store(blocks, blockWidth, xx, yy, data[..2]);
                        break;
                    }
                    case BlockKind.Bc5:
                    {
                        bc15.Clear();
                        bc15.LoadChannel(rgba, xx, yy, stride, 0);
                        var (r0, r1) = bc15.CompressBc3Alpha();
                        bc15.LoadChannel(rgba, xx, yy, stride, 1);
                        var (g0, g1) = bc15.CompressBc3Alpha();
                        data[0] = r0; data[1] = r1; data[2] = g0; data[3] = g1;
                        Store(blocks, blockWidth, xx, yy, data);
                        break;
                    }
                    case BlockKind.Bc6h:
                    {
                        var c = new Bc6hCompressor();
                        c.LoadInterleaved8Bit(rgba, xx, yy, stride);
                        c.Compress();
                        Store(blocks, blockWidth, xx, yy, c.Data.AsSpan(0, 4));
                        break;
                    }
                    case BlockKind.Bc7:
                    {
                        var c = new Bc7Compressor();
                        c.LoadInterleavedRgba(rgba, xx, yy, stride);
                        c.ComputeOpaqueErr();
                        c.Compress();
                        Store(blocks, blockWidth, xx, yy, c.Data.AsSpan(0, 4));
                        break;
                    }
                }
            }
        }
    }

    private static void Store(Span<byte> blocks, int blockWidth, int xx, int yy, ReadOnlySpan<uint> data)
    {
        var offset = (yy * blockWidth + xx) * (data.Length * 4);
        for (var i = 0; i < data.Length; i++)
            BinaryPrimitives.WriteUInt32LittleEndian(blocks[(offset + i * 4)..], data[i]);
    }

    /// <summary>A block as 64 floats: 16 red, 16 green, 16 blue, 16 alpha.</summary>
    private static void LoadBlock(float[] block, ReadOnlySpan<byte> rgba, int xx, int yy, int stride)
    {
        for (var y = 0; y < 4; y++)
            for (var x = 0; x < 4; x++)
            {
                var offset = (yy * 4 + y) * stride + (xx * 4 + x) * 4;
                block[y * 4 + x] = rgba[offset];
                block[16 + y * 4 + x] = rgba[offset + 1];
                block[32 + y * 4 + x] = rgba[offset + 2];
                block[48 + y * 4 + x] = rgba[offset + 3];
            }
    }

    private static float Sq(float x) => x * x;

    /// <summary>BC1 to BC5 (block_compression's BlockCompressorBC15).</summary>
    private sealed class Bc15
    {
        private readonly float[] _block = new float[64];

        public void Clear() => Array.Clear(_block);

        public void LoadInterleavedRgba(ReadOnlySpan<byte> rgba, int xx, int yy, int stride) => LoadBlock(_block, rgba, xx, yy, stride);

        /// <summary>One channel of the pixels into the alpha quarter (BC4's red, BC5's red then green).</summary>
        public void LoadChannel(ReadOnlySpan<byte> rgba, int xx, int yy, int stride, int channel)
        {
            for (var y = 0; y < 4; y++)
                for (var x = 0; x < 4; x++)
                    _block[48 + y * 4 + x] = rgba[(yy * 4 + y) * stride + (xx * 4 + x) * 4 + channel];
        }

        public static (uint, uint) LoadAlpha4Bit(ReadOnlySpan<byte> rgba, int xx, int yy, int stride)
        {
            uint bits0 = 0, bits1 = 0;
            for (var y = 0; y < 4; y++)
                for (var x = 0; x < 4; x++)
                {
                    var alpha = rgba[(yy * 4 + y) * stride + (xx * 4 + x) * 4 + 3] / 255f;
                    var alpha4 = Sat.ToUInt(alpha * 15f);
                    var bitPosition = y * 16 + x * 4;
                    if (bitPosition < 32) bits0 |= alpha4 << bitPosition;
                    else bits1 |= alpha4 << (bitPosition - 32);
                }
            return (bits0, bits1);
        }

        private void ComputeCovarDc(Span<float> covar, Span<float> dc)
        {
            for (var p = 0; p < 3; p++)
            {
                var acc = 0f;
                for (var k = 0; k < 16; k++) acc += _block[k + p * 16];
                dc[p] = acc / 16f;
            }
            float covar0 = 0f, covar1 = 0f, covar2 = 0f, covar3 = 0f, covar4 = 0f, covar5 = 0f;
            for (var k = 0; k < 16; k++)
            {
                var rgb0 = _block[k] - dc[0];
                var rgb1 = _block[k + 16] - dc[1];
                var rgb2 = _block[k + 32] - dc[2];
                covar0 += rgb0 * rgb0;
                covar1 += rgb0 * rgb1;
                covar2 += rgb0 * rgb2;
                covar3 += rgb1 * rgb1;
                covar4 += rgb1 * rgb2;
                covar5 += rgb2 * rgb2;
            }
            covar[0] = covar0; covar[1] = covar1; covar[2] = covar2; covar[3] = covar3; covar[4] = covar4; covar[5] = covar5;
        }

        private static void Ssymv(Span<float> result, ReadOnlySpan<float> covar, ReadOnlySpan<float> a)
        {
            result[0] = covar[0] * a[0] + covar[1] * a[1] + covar[2] * a[2];
            result[1] = covar[1] * a[0] + covar[3] * a[1] + covar[4] * a[2];
            result[2] = covar[2] * a[0] + covar[4] * a[1] + covar[5] * a[2];
        }

        private static void ComputeAxis3(Span<float> axis, ReadOnlySpan<float> covar, int powerIterations)
        {
            Span<float> a = stackalloc float[3];
            a.Fill(1f);
            for (var i = 0; i < powerIterations; i++)
            {
                Ssymv(axis, covar, a);
                axis[..3].CopyTo(a);
                if (i % 2 == 1)
                {
                    var normSq = 0f;
                    for (var p = 0; p < 3; p++) normSq += axis[p] * axis[p];
                    var rnorm = 1f / MathF.Sqrt(normSq);
                    for (var p = 0; p < 3; p++) a[p] *= rnorm;
                }
            }
            a.CopyTo(axis);
        }

        private void PickEndpoints(Span<float> c0, Span<float> c1, ReadOnlySpan<float> axis, ReadOnlySpan<float> dc)
        {
            var minDot = 256f * 256f;
            var maxDot = 0f;
            for (var y = 0; y < 4; y++)
                for (var x = 0; x < 4; x++)
                {
                    var dot = 0f;
                    for (var p = 0; p < 3; p++) dot += (_block[p * 16 + y * 4 + x] - dc[p]) * axis[p];
                    minDot = Sat.Min(minDot, dot);
                    maxDot = Sat.Max(maxDot, dot);
                }
            if (maxDot - minDot < 1f)
            {
                minDot -= 0.5f;
                maxDot += 0.5f;
            }
            var normSq = 0f;
            for (var p = 0; p < 3; p++) normSq += axis[p] * axis[p];
            var rnormSq = 1f / normSq;
            for (var p = 0; p < 3; p++)
            {
                c0[p] = Math.Clamp(dc[p] + minDot * rnormSq * axis[p], 0f, 255f);
                c1[p] = Math.Clamp(dc[p] + maxDot * rnormSq * axis[p], 0f, 255f);
            }
        }

        private static void DecRgb565(Span<float> c, int p)
        {
            var b5 = p & 31;
            var g6 = (p >> 5) & 63;
            var r5 = (p >> 11) & 31;
            c[0] = (r5 << 3) + (r5 >> 2);
            c[1] = (g6 << 2) + (g6 >> 4);
            c[2] = (b5 << 3) + (b5 >> 2);
        }

        private static int EncRgb565(ReadOnlySpan<float> c)
        {
            var r = Sat.ToInt(c[0]);
            var g = Sat.ToInt(c[1]);
            var b = Sat.ToInt(c[2]);
            var r5 = (r * 31 + 128 + ((r * 31) >> 8)) >> 8;
            var g6 = (g * 63 + 128 + ((g * 63) >> 8)) >> 8;
            var b5 = (b * 31 + 128 + ((b * 31) >> 8)) >> 8;
            return (r5 << 11) + (g6 << 5) + b5;
        }

        private uint FastQuant(int p0, int p1)
        {
            Span<float> c0 = stackalloc float[3];
            Span<float> c1 = stackalloc float[3];
            DecRgb565(c0, p0);
            DecRgb565(c1, p1);
            Span<float> dir = stackalloc float[3];
            for (var p = 0; p < 3; p++) dir[p] = c1[p] - c0[p];
            var sqNorm = 0f;
            for (var p = 0; p < 3; p++) sqNorm += dir[p] * dir[p];
            var rsqNorm = 1f / sqNorm;
            for (var p = 0; p < 3; p++) dir[p] *= rsqNorm * 3f;
            var bias = 0.5f;
            for (var p = 0; p < 3; p++) bias -= c0[p] * dir[p];
            uint bits = 0, scaler = 1;
            for (var k = 0; k < 16; k++)
            {
                var dot = 0f;
                for (var p = 0; p < 3; p++) dot += _block[k + p * 16] * dir[p];
                var q = Math.Clamp(Sat.ToInt(dot + bias), 0, 3);
                bits += (uint)q * scaler;
                scaler *= 4;
            }
            return bits;
        }

        private void Bc1Refine(Span<int> pe, uint bits, ReadOnlySpan<float> dc)
        {
            Span<float> c0 = stackalloc float[3];
            Span<float> c1 = stackalloc float[3];
            if ((bits ^ (bits * 4)) < 4)
            {
                dc[..3].CopyTo(c0);
                dc[..3].CopyTo(c1);
            }
            else
            {
                Span<float> atb1 = stackalloc float[3];
                atb1.Clear();
                var sumQ = 0f;
                var sumQq = 0f;
                var shifted = bits;
                for (var k = 0; k < 16; k++)
                {
                    float q = shifted & 3;
                    shifted >>= 2;
                    var x = 3f - q;
                    sumQ += q;
                    sumQq += q * q;
                    for (var p = 0; p < 3; p++) atb1[p] += x * _block[k + p * 16];
                }
                Span<float> sum = stackalloc float[3];
                Span<float> atb2 = stackalloc float[3];
                for (var p = 0; p < 3; p++)
                {
                    sum[p] = dc[p] * 16f;
                    atb2[p] = 3f * sum[p] - atb1[p];
                }
                var cxx = 16f * 9f - 2f * 3f * sumQ + sumQq;
                var cyy = sumQq;
                var cxy = 3f * sumQ - sumQq;
                var scale = 3f * (1f / (cxx * cyy - cxy * cxy));
                for (var p = 0; p < 3; p++)
                {
                    c0[p] = (atb1[p] * cyy - atb2[p] * cxy) * scale;
                    c1[p] = (atb2[p] * cxx - atb1[p] * cxy) * scale;
                    c0[p] = Math.Clamp(c0[p], 0f, 255f);
                    c1[p] = Math.Clamp(c1[p], 0f, 255f);
                }
            }
            pe[0] = EncRgb565(c0);
            pe[1] = EncRgb565(c1);
        }

        private static uint FixQbits(uint qbits)
        {
            const uint mask01 = 0x55555555;
            const uint mask10 = 0xAAAAAAAA;
            var qbits0 = qbits & mask01;
            var qbits1 = qbits & mask10;
            return (qbits1 >> 1) + (qbits1 ^ (qbits0 << 1));
        }

        public (uint, uint) CompressBc1Core()
        {
            const int powerIterations = 4;
            const int refineIterations = 1;
            Span<float> covar = stackalloc float[6];
            Span<float> dc = stackalloc float[3];
            ComputeCovarDc(covar, dc);
            covar[0] += FloatEpsilon;
            covar[3] += FloatEpsilon;
            covar[5] += FloatEpsilon;
            Span<float> axis = stackalloc float[3];
            axis.Clear();
            ComputeAxis3(axis, covar, powerIterations);
            Span<float> c0 = stackalloc float[3];
            Span<float> c1 = stackalloc float[3];
            PickEndpoints(c0, c1, axis, dc);
            Span<int> p = stackalloc int[2];
            p[0] = EncRgb565(c0);
            p[1] = EncRgb565(c1);
            if (p[0] < p[1]) (p[0], p[1]) = (p[1], p[0]);
            var data0 = ((uint)p[1] << 16) | (uint)p[0];
            var data1 = FastQuant(p[0], p[1]);
            for (var i = 0; i < refineIterations; i++)
            {
                Bc1Refine(p, data1, dc);
                if (p[0] < p[1]) (p[0], p[1]) = (p[1], p[0]);
                data0 = ((uint)p[1] << 16) | (uint)p[0];
                data1 = FastQuant(p[0], p[1]);
            }
            data1 = FixQbits(data1);
            return (data0, data1);
        }

        public (uint, uint) CompressBc3Alpha()
        {
            float ep0 = 255f, ep1 = 0f;
            for (var k = 0; k < 16; k++)
            {
                ep0 = Sat.Min(ep0, _block[48 + k]);
                ep1 = Sat.Max(ep1, _block[48 + k]);
            }
            if (ep0 == ep1) ep1 = ep0 + 0.1f;
            uint q0 = 0, q1 = 0;
            var scale = 7f / (ep1 - ep0);
            for (var k = 0; k < 16; k++)
            {
                var v = _block[48 + k];
                var proj = (v - ep0) * scale + 0.5f;
                var q = Math.Clamp(Sat.ToInt(proj), 0, 7);
                q = 7 - q;
                if (q > 0) q++;
                if (q == 8) q = 1;
                if (k / 8 == 0) q0 |= (uint)q << (k % 8 * 3);
                else q1 |= (uint)q << (k % 8 * 3);
            }
            var data0 = (Math.Clamp(Sat.ToUInt(ep0), 0u, 255u) << 8) | Math.Clamp(Sat.ToUInt(ep1), 0u, 255u);
            data0 |= q0 << 16;
            var data1 = q0 >> 16;
            data1 |= q1 << 8;
            return (data0, data1);
        }
    }

    /// <summary>Rust's f32::EPSILON (2^-23), not .NET's float.Epsilon (the smallest subnormal).</summary>
    private const float FloatEpsilon = 1f / 8388608f;

    // ---- shared by BC6H and BC7 (block_compression's encode/common.rs) -------------------------------------------

    private static int GetUnquantValue(int bits, int index) => bits switch
    {
        2 => Unquant2[index],
        3 => Unquant3[index],
        _ => Unquant4[index],
    };

    private static readonly int[] Unquant2 = [0, 21, 43, 64, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    private static readonly int[] Unquant3 = [0, 9, 18, 27, 37, 46, 55, 64, 0, 0, 0, 0, 0, 0, 0, 0];
    private static readonly int[] Unquant4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

    private static readonly uint[] PatternTable =
    [
        0x50505050, 0x40404040, 0x54545454, 0x54505040, 0x50404000, 0x55545450, 0x55545040, 0x54504000,
        0x50400000, 0x55555450, 0x55544000, 0x54400000, 0x55555440, 0x55550000, 0x55555500, 0x55000000,
        0x55150100, 0x00004054, 0x15010000, 0x00405054, 0x00004050, 0x15050100, 0x05010000, 0x40505054,
        0x00404050, 0x05010100, 0x14141414, 0x05141450, 0x01155440, 0x00555500, 0x15014054, 0x05414150,
        0x44444444, 0x55005500, 0x11441144, 0x05055050, 0x05500550, 0x11114444, 0x41144114, 0x44111144,
        0x15055054, 0x01055040, 0x05041050, 0x05455150, 0x14414114, 0x50050550, 0x41411414, 0x00141400,
        0x00041504, 0x00105410, 0x10541000, 0x04150400, 0x50410514, 0x41051450, 0x05415014, 0x14054150,
        0x41050514, 0x41505014, 0x40011554, 0x54150140, 0x50505500, 0x00555050, 0x15151010, 0x54540404,
        0xAA685050, 0x6A5A5040, 0x5A5A4200, 0x5450A0A8, 0xA5A50000, 0xA0A05050, 0x5555A0A0, 0x5A5A5050,
        0xAA550000, 0xAA555500, 0xAAAA5500, 0x90909090, 0x94949494, 0xA4A4A4A4, 0xA9A59450, 0x2A0A4250,
        0xA5945040, 0x0A425054, 0xA5A5A500, 0x55A0A0A0, 0xA8A85454, 0x6A6A4040, 0xA4A45000, 0x1A1A0500,
        0x0050A4A4, 0xAAA59090, 0x14696914, 0x69691400, 0xA08585A0, 0xAA821414, 0x50A4A450, 0x6A5A0200,
        0xA9A58000, 0x5090A0A8, 0xA8A09050, 0x24242424, 0x00AA5500, 0x24924924, 0x24499224, 0x50A50A50,
        0x500AA550, 0xAAAA4444, 0x66660000, 0xA5A0A5A0, 0x50A050A0, 0x69286928, 0x44AAAA44, 0x66666600,
        0xAA444444, 0x54A854A8, 0x95809580, 0x96969600, 0xA85454A8, 0x80959580, 0xAA141414, 0x96960000,
        0xAAAA1414, 0xA05050A0, 0xA0A5A5A0, 0x96000000, 0x40804080, 0xA9A8A9A8, 0xAAAAAA44, 0x2A4A5254,
    ];

    private static readonly uint[] PatternMaskTable =
    [
        0xCCCC3333, 0x88887777, 0xEEEE1111, 0xECC81337, 0xC880377F, 0xFEEC0113, 0xFEC80137, 0xEC80137F,
        0xC80037FF, 0xFFEC0013, 0xFE80017F, 0xE80017FF, 0xFFE80017, 0xFF0000FF, 0xFFF0000F, 0xF0000FFF,
        0xF71008EF, 0x008EFF71, 0x71008EFF, 0x08CEF731, 0x008CFF73, 0x73108CEF, 0x3100CEFF, 0x8CCE7331,
        0x088CF773, 0x3110CEEF, 0x66669999, 0x366CC993, 0x17E8E817, 0x0FF0F00F, 0x718E8E71, 0x399CC663,
        0xAAAA5555, 0xF0F00F0F, 0x5A5AA5A5, 0x33CCCC33, 0x3C3CC3C3, 0x55AAAA55, 0x96966969, 0xA55A5AA5,
        0x73CE8C31, 0x13C8EC37, 0x324CCDB3, 0x3BDCC423, 0x69969669, 0xC33C3CC3, 0x99666699, 0x0660F99F,
        0x0272FD8D, 0x04E4FB1B, 0x4E40B1BF, 0x2720D8DF, 0xC93636C9, 0x936C6C93, 0x39C6C639, 0x639C9C63,
        0x93366CC9, 0x9CC66339, 0x817E7E81, 0xE71818E7, 0xCCF0330F, 0x0FCCF033, 0x774488BB, 0xEE2211DD,
        0x08CC0133, 0x8CC80037, 0xCC80006F, 0xEC001331, 0x330000FF, 0x00CC3333, 0xFF000033, 0xCCCC0033,
        0x0F0000FF, 0x0FF0000F, 0x00F0000F, 0x44443333, 0x66661111, 0x22221111, 0x136C0013, 0x008C8C63,
        0x36C80137, 0x08CEC631, 0x3330000F, 0xF0000333, 0x00EE1111, 0x88880077, 0x22C0113F, 0x443088CF,
        0x0C22F311, 0x03440033, 0x69969009, 0x9960009F, 0x03303443, 0x00660699, 0xC22C3113, 0x8C0000EF,
        0x1300007F, 0xC4003331, 0x004C1333, 0x22229999, 0x00F0F00F, 0x24929249, 0x29429429, 0xC30C30C3,
        0xC03C3C03, 0x00AA0055, 0xAA0000FF, 0x30300303, 0xC0C03333, 0x90900909, 0xA00A5005, 0xAAA0000F,
        0x0AAA0555, 0xE0E01111, 0x70700707, 0x6660000F, 0x0EE01111, 0x07707007, 0x06660999, 0x660000FF,
        0x00660099, 0x0CC03333, 0x03303003, 0x60000FFF, 0x80807777, 0x10100101, 0x000A0005, 0x08CE8421,
    ];

    private static readonly uint[] SkipTable =
    [
        0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0,
        0xF0, 0x20, 0x80, 0x20, 0x20, 0x80, 0x80, 0xF0, 0x20, 0x80, 0x20, 0x20, 0x80, 0x80, 0x20, 0x20,
        0xF0, 0xF0, 0x60, 0x80, 0x20, 0x80, 0xF0, 0xF0, 0x20, 0x80, 0x20, 0x20, 0x20, 0xF0, 0xF0, 0x60,
        0x60, 0x20, 0x60, 0x80, 0xF0, 0xF0, 0x20, 0x20, 0xF0, 0xF0, 0xF0, 0xF0, 0xF0, 0x20, 0x20, 0xF0,
        0x3F, 0x38, 0xF8, 0xF3, 0x8F, 0x3F, 0xF3, 0xF8, 0x8F, 0x8F, 0x6F, 0x6F, 0x6F, 0x5F, 0x3F, 0x38,
        0x3F, 0x38, 0x8F, 0xF3, 0x3F, 0x38, 0x6F, 0xA8, 0x53, 0x8F, 0x86, 0x6A, 0x8F, 0x5F, 0xFA, 0xF8,
        0x8F, 0xF3, 0x3F, 0x5A, 0x6A, 0xA8, 0x89, 0xFA, 0xF6, 0x3F, 0xF8, 0x5F, 0xF3, 0xF6, 0xF6, 0xF8,
        0x3F, 0xF3, 0x5F, 0x5F, 0x5F, 0x8F, 0x5F, 0xAF, 0x5F, 0xAF, 0x8F, 0xDF, 0xF3, 0xCF, 0x3F, 0x38,
    ];

    private static uint GetPattern(int partId) => PatternTable[partId];

    private static uint GetPatternMask(int partId, int j)
    {
        var packed = PatternMaskTable[partId];
        var mask0 = packed & 0xFFFF;
        var mask1 = packed >> 16;
        return j == 2 ? ~mask0 & ~mask1 : j == 0 ? mask0 : mask1;
    }

    private static (uint, uint, uint) GetSkips(int partId)
    {
        var packed = SkipTable[partId];
        return (0, packed >> 4, packed & 15);
    }

    private static void PutBits(uint[] data, ref uint pos, uint bits, uint v)
    {
        data[pos / 32] |= v << (int)(pos % 32);
        if (pos % 32 + bits > 32) data[pos / 32 + 1] |= v >> (int)(32 - pos % 32);
        pos += bits;
    }

    private static void DataShl1BitFrom(uint[] data, int fromBits)
    {
        if (fromBits < 96)
        {
            var shifted = (data[2] >> 1) | (data[3] << 31);
            var mask = ((1u << (fromBits - 64)) - 1) >> 1;
            data[2] = (mask & data[2]) | (~mask & shifted);
            data[3] = (data[3] >> 1) | (data[4] << 31);
            data[4] >>= 1;
        }
        else if (fromBits < 128)
        {
            var shifted = (data[3] >> 1) | (data[4] << 31);
            var mask = ((1u << (fromBits - 96)) - 1) >> 1;
            data[3] = (mask & data[3]) | (~mask & shifted);
            data[4] >>= 1;
        }
    }

    private static void PartialSortList(Span<int> list, int length, int partialCount)
    {
        for (var k = 0; k < partialCount; k++)
        {
            var bestIdx = k;
            var bestValue = list[k];
            for (var i = k + 1; i < length; i++)
            {
                if (bestValue > list[i])
                {
                    bestValue = list[i];
                    bestIdx = i;
                }
            }
            (list[k], list[bestIdx]) = (list[bestIdx], list[k]);
        }
    }

    private static void OptEndpoints(Span<float> ep, float[] block, int bits, uint qblock0, uint qblock1, uint mask, int channels)
    {
        var levels = 1 << bits;
        Span<float> atb1 = stackalloc float[4];
        Span<float> sum = stackalloc float[5];
        atb1.Clear();
        sum.Clear();
        var sumQ = 0f;
        var sumQq = 0f;
        var maskShifted = mask << 1;
        for (var k1 = 0; k1 < 2; k1++)
        {
            var qbitsShifted = k1 == 0 ? qblock0 : qblock1;
            for (var k2 = 0; k2 < 8; k2++)
            {
                var k = k1 * 8 + k2;
                float q = qbitsShifted & 15;
                qbitsShifted >>= 4;
                maskShifted >>= 1;
                if ((maskShifted & 1) == 0) continue;
                var x = (levels - 1) - q;
                sumQ += q;
                sumQq += q * q;
                sum[4] += 1f;
                for (var p = 0; p < channels; p++)
                {
                    sum[p] += block[k + p * 16];
                    atb1[p] += x * block[k + p * 16];
                }
            }
        }
        Span<float> atb2 = stackalloc float[4];
        atb2.Clear();
        for (var p = 0; p < channels; p++) atb2[p] = (levels - 1) * sum[p] - atb1[p];
        var cxx = sum[4] * Sq(levels - 1) - 2f * (levels - 1) * sumQ + sumQq;
        var cyy = sumQq;
        var cxy = (levels - 1) * sumQ - sumQq;
        var scale = (levels - 1) / (cxx * cyy - cxy * cxy);
        for (var p = 0; p < channels; p++)
        {
            ep[p] = (atb1[p] * cyy - atb2[p] * cxy) * scale;
            ep[4 + p] = (atb2[p] * cxx - atb1[p] * cxy) * scale;
        }
        if (MathF.Abs(cxx * cyy - cxy * cxy) < 0.001f)
        {
            // flatten
            for (var p = 0; p < channels; p++)
            {
                ep[p] = sum[p] / sum[4];
                ep[4 + p] = ep[p];
            }
        }
    }

    // ε² of the PCA helpers: sq(0.001) in f32.
    private static readonly float PcaEps = Sq(0.001f);

    /// <summary>A bound from the principal component (PCA) of a covariance.</summary>
    private static float GetPcaBound(ReadOnlySpan<float> covar, int channels)
    {
        const int powerIterations = 4; // rough, but enough for a bound
        Span<float> covarScaled = stackalloc float[10];
        covar[..10].CopyTo(covarScaled);
        var invVar = 1f / (256f * 256f);
        for (var i = 0; i < 10; i++) covarScaled[i] *= invVar;
        covarScaled[0] += PcaEps;
        covarScaled[4] += PcaEps;
        covarScaled[7] += PcaEps;
        Span<float> axis = stackalloc float[4];
        axis.Clear();
        ComputeAxis(axis, covarScaled, powerIterations, channels);
        Span<float> a = stackalloc float[4];
        a.Clear();
        if (channels == 3) Ssymv3(a, covarScaled, axis);
        else if (channels == 4) Ssymv4(a, covarScaled, axis);
        var sqSum = 0f;
        for (var p = 0; p < channels; p++) sqSum += Sq(a[p]);
        var lambda = MathF.Sqrt(sqSum);
        var bound = covarScaled[0] + covarScaled[4] + covarScaled[7];
        if (channels == 4) bound += covarScaled[9];
        bound -= lambda;
        return Sat.Max(bound, 0f);
    }

    private static void Ssymv3(Span<float> a, ReadOnlySpan<float> covar, ReadOnlySpan<float> b)
    {
        a[0] = covar[0] * b[0] + covar[1] * b[1] + covar[2] * b[2];
        a[1] = covar[1] * b[0] + covar[4] * b[1] + covar[5] * b[2];
        a[2] = covar[2] * b[0] + covar[5] * b[1] + covar[7] * b[2];
    }

    private static void Ssymv4(Span<float> a, ReadOnlySpan<float> covar, ReadOnlySpan<float> b)
    {
        a[0] = covar[0] * b[0] + covar[1] * b[1] + covar[2] * b[2] + covar[3] * b[3];
        a[1] = covar[1] * b[0] + covar[4] * b[1] + covar[5] * b[2] + covar[6] * b[3];
        a[2] = covar[2] * b[0] + covar[5] * b[1] + covar[7] * b[2] + covar[8] * b[3];
        a[3] = covar[3] * b[0] + covar[6] * b[1] + covar[8] * b[2] + covar[9] * b[3];
    }

    private static void ComputeAxis(Span<float> axis, ReadOnlySpan<float> covar, int powerIterations, int channels)
    {
        Span<float> a = stackalloc float[4];
        a.Fill(1f);
        for (var i = 0; i < powerIterations; i++)
        {
            if (channels == 3) Ssymv3(axis, covar, a);
            else if (channels == 4) Ssymv4(axis, covar, a);
            axis[..channels].CopyTo(a);
            // renormalise every other iteration
            if (i % 2 == 1)
            {
                var normSq = 0f;
                for (var p = 0; p < channels; p++) normSq += Sq(axis[p]);
                var rnorm = 1f / MathF.Sqrt(normSq);
                for (var p = 0; p < channels; p++) a[p] *= rnorm;
            }
        }
        a[..channels].CopyTo(axis);
    }

    /// <summary>Adds the masked pixels' sums into <paramref name="stats"/> (15 values, zero to start).</summary>
    private static void ComputeStatsMasked(Span<float> stats, float[] block, uint mask, int channels)
    {
        var maskShifted = mask << 1;
        Span<float> rgba = stackalloc float[4];
        for (var k = 0; k < 16; k++)
        {
            maskShifted >>= 1;
            float flag = maskShifted & 1;
            rgba.Clear();
            for (var p = 0; p < channels; p++) rgba[p] = block[k + p * 16] * flag;
            stats[14] += flag;
            stats[10] += rgba[0];
            stats[11] += rgba[1];
            stats[12] += rgba[2];
            stats[0] += rgba[0] * rgba[0];
            stats[1] += rgba[0] * rgba[1];
            stats[2] += rgba[0] * rgba[2];
            stats[4] += rgba[1] * rgba[1];
            stats[5] += rgba[1] * rgba[2];
            stats[7] += rgba[2] * rgba[2];
            if (channels == 4)
            {
                stats[13] += rgba[3];
                stats[3] += rgba[0] * rgba[3];
                stats[6] += rgba[1] * rgba[3];
                stats[8] += rgba[2] * rgba[3];
                stats[9] += rgba[3] * rgba[3];
            }
        }
    }

    private static void CovarFromStats(Span<float> covar, ReadOnlySpan<float> stats, int channels)
    {
        covar[0] = stats[0] - stats[10] * stats[10] / stats[14];
        covar[1] = stats[1] - stats[10] * stats[11] / stats[14];
        covar[2] = stats[2] - stats[10] * stats[12] / stats[14];
        covar[4] = stats[4] - stats[11] * stats[11] / stats[14];
        covar[5] = stats[5] - stats[11] * stats[12] / stats[14];
        covar[7] = stats[7] - stats[12] * stats[12] / stats[14];
        if (channels == 4)
        {
            covar[3] = stats[3] - stats[10] * stats[13] / stats[14];
            covar[6] = stats[6] - stats[11] * stats[13] / stats[14];
            covar[8] = stats[8] - stats[12] * stats[13] / stats[14];
            covar[9] = stats[9] - stats[13] * stats[13] / stats[14];
        }
    }

    private static void ComputeCovarDcMasked(Span<float> covar, Span<float> dc, float[] block, uint mask, int channels)
    {
        Span<float> stats = stackalloc float[15];
        stats.Clear();
        ComputeStatsMasked(stats, block, mask, channels);
        for (var p = 0; p < channels; p++) dc[p] = stats[10 + p] / stats[14];
        CovarFromStats(covar, stats, channels);
    }

    private static void BlockPcaAxis(Span<float> axis, Span<float> dc, float[] block, uint mask, int channels)
    {
        const int powerIterations = 8; // 4 is not enough for high quality
        Span<float> covar = stackalloc float[10];
        covar.Clear();
        ComputeCovarDcMasked(covar, dc, block, mask, channels);
        const float invVar = 1f / (256f * 256f);
        for (var i = 0; i < 10; i++) covar[i] *= invVar;
        covar[0] += PcaEps;
        covar[4] += PcaEps;
        covar[7] += PcaEps;
        covar[9] += PcaEps;
        ComputeAxis(axis, covar, powerIterations, channels);
    }

    private static float BlockPcaBoundSplit(float[] block, uint mask, ReadOnlySpan<float> fullStats, int channels)
    {
        Span<float> stats = stackalloc float[15];
        stats.Clear();
        ComputeStatsMasked(stats, block, mask, channels);
        Span<float> covar1 = stackalloc float[10];
        covar1.Clear();
        CovarFromStats(covar1, stats, channels);
        for (var i = 0; i < 15; i++) stats[i] = fullStats[i] - stats[i];
        Span<float> covar2 = stackalloc float[10];
        covar2.Clear();
        CovarFromStats(covar2, stats, channels);
        var bound = 0f;
        bound += GetPcaBound(covar1, channels);
        bound += GetPcaBound(covar2, channels);
        return MathF.Sqrt(bound) * 256f;
    }

    /// <summary>Quantises every pixel to its region's endpoints; returns the squared error.</summary>
    private static float BlockQuant(Span<uint> qblock, float[] block, int bits, ReadOnlySpan<float> ep, uint pattern, int channels)
    {
        var totalErr = 0f;
        var levels = 1 << bits;
        qblock[0] = 0;
        qblock[1] = 0;
        var patternShifted = pattern;
        for (var k = 0; k < 16; k++)
        {
            var j = (int)(patternShifted & 3);
            patternShifted >>= 2;
            var proj = 0f;
            var div = 0f;
            for (var p = 0; p < channels; p++)
            {
                var epA = ep[8 * j + p];
                var epB = ep[8 * j + 4 + p];
                proj += (block[k + p * 16] - epA) * (epB - epA);
                div += Sq(epB - epA);
            }
            proj /= div;
            var q1 = Sat.ToInt(proj * levels + 0.5f);
            var q1Clamped = Math.Clamp(q1, 1, levels - 1);
            var err0 = 0f;
            var err1 = 0f;
            var w0 = GetUnquantValue(bits, q1Clamped - 1);
            var w1 = GetUnquantValue(bits, q1Clamped);
            for (var p = 0; p < channels; p++)
            {
                var epA = ep[8 * j + p];
                var epB = ep[8 * j + 4 + p];
                float decV0 = ((64 - w0) * Sat.ToInt(epA) + w0 * Sat.ToInt(epB) + 32) / 64;
                float decV1 = ((64 - w1) * Sat.ToInt(epA) + w1 * Sat.ToInt(epB) + 32) / 64;
                err0 += Sq(decV0 - block[k + p * 16]);
                err1 += Sq(decV1 - block[k + p * 16]);
            }
            var bestErr = err1;
            var bestQ = q1Clamped;
            if (err0 < err1)
            {
                bestErr = err0;
                bestQ = q1Clamped - 1;
            }
            qblock[k / 8] |= (uint)bestQ << (4 * (k % 8));
            totalErr += bestErr;
        }
        return totalErr;
    }

    private static void BlockSegmentCore(Span<float> ep, float[] block, uint mask, int channels)
    {
        Span<float> axis = stackalloc float[4];
        Span<float> dc = stackalloc float[4];
        axis.Clear();
        dc.Clear();
        BlockPcaAxis(axis, dc, block, mask, channels);
        float ext0 = float.PositiveInfinity, ext1 = float.NegativeInfinity;
        var maskShifted = mask << 1;
        for (var k = 0; k < 16; k++)
        {
            maskShifted >>= 1;
            if ((maskShifted & 1) == 0) continue;
            var dot = 0f;
            for (var p = 0; p < channels; p++) dot += axis[p] * (block[16 * p + k] - dc[p]);
            ext0 = Sat.Min(ext0, dot);
            ext1 = Sat.Max(ext1, dot);
        }
        // some distance when the endpoints collapse
        if (ext1 - ext0 < 1f)
        {
            ext0 -= 0.5f;
            ext1 += 0.5f;
        }
        for (var p = 0; p < channels; p++)
        {
            ep[p] = ext0 * axis[p] + dc[p];
            ep[4 + p] = ext1 * axis[p] + dc[p];
        }
    }

    private static void Bc7CodeQblock(uint[] data, ref uint qpos, uint qblock0, uint qblock1, int bits, uint flips)
    {
        var levels = 1u << bits;
        var flipsShifted = flips;
        for (var k1 = 0; k1 < 2; k1++)
        {
            var qbitsShifted = k1 == 0 ? qblock0 : qblock1;
            for (var k2 = 0; k2 < 8; k2++)
            {
                var q = qbitsShifted & 15;
                if ((flipsShifted & 1) > 0) q = levels - 1 - q;
                if (k1 == 0 && k2 == 0) PutBits(data, ref qpos, (uint)bits - 1, q);
                else PutBits(data, ref qpos, (uint)bits, q);
                qbitsShifted >>= 4;
                flipsShifted >>= 1;
            }
        }
    }

    private static void Bc7CodeAdjustSkipMode01237(uint[] data, int mode, int partId)
    {
        var pairs = mode is 0 or 2 ? 3 : 2;
        var bits = mode is 0 or 1 ? 3 : 2;
        var (s0, s1, s2) = GetSkips(partId);
        if (pairs > 2 && s1 < s2) (s1, s2) = (s2, s1);
        _ = s0;
        DataShl1BitFrom(data, 128 + (pairs - 1) - (15 - (int)s1) * bits);
        if (pairs > 2) DataShl1BitFrom(data, 128 + (pairs - 1) - (15 - (int)s2) * bits);
    }

    private static void Bc7CodeApplySwapMode456(Span<int> qep, int channels, Span<uint> qblock, int bits)
    {
        var levels = 1u << bits;
        if ((qblock[0] & 15) >= levels / 2)
        {
            for (var p = 0; p < channels; p++) (qep[p], qep[channels + p]) = (qep[channels + p], qep[p]);
            for (var i = 0; i < 2; i++) qblock[i] = 0x11111111 * (levels - 1) - qblock[i];
        }
    }

    private static uint Bc7CodeApplySwapMode01237(Span<int> qep, uint qblock0, uint qblock1, int mode, int partId)
    {
        var bits = mode is 0 or 1 ? 3 : 2;
        var pairs = mode is 0 or 2 ? 3 : 2;
        uint flips = 0;
        var levels = 1u << bits;
        var (s0, s1, s2) = GetSkips(partId);
        for (var j = 0; j < pairs; j++)
        {
            var k0 = (int)(j == 0 ? s0 : j == 1 ? s1 : s2);
            // the 4 bits of pixel k0
            var q = ((k0 >> 3 == 0 ? qblock0 : qblock1) << (28 - (k0 & 7) * 4)) >> 28;
            if (q >= levels / 2)
            {
                for (var p = 0; p < 4; p++) (qep[8 * j + p], qep[8 * j + 4 + p]) = (qep[8 * j + 4 + p], qep[8 * j + p]);
                flips |= GetPatternMask(partId, j);
            }
        }
        return flips;
    }
}
