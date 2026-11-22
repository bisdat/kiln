// SPDX-License-Identifier: MIT
// Ported from block_compression 0.6.0 (src/decode.rs and src/decode/block.rs), MIT, Copyright (c) 2025 Nils Hasenbanck,
// Copyright (c) 2016-2024 Intel Corporation (licenses/block_compression-LICENSE.txt). Its block decoders are a port of
// bcdec.h v0.98 by Sergii Kudlai (iOrange), public domain (The Unlicense), with optimisations by Aras Pranckevicius and
// Michael Schmidt. Kiln's texture code for kiln mode (crates/sfi/sfi-core/src/eml/texture.rs) decodes with that crate;
// this is the same code in C#, so both give the same pixels.
using System.Buffers.Binary;

namespace Kiln;

/// <summary>BC1 to BC7 blocks to RGBA8, as block_compression's <c>decompress_blocks_as_rgba8</c> does it.</summary>
internal static class BlockDecoder
{
    /// <summary>Decodes <paramref name="blocks"/> (exactly the image's blocks) into <paramref name="rgba"/>
    /// (<c>width * height * 4</c> bytes); the size is a multiple of 4.</summary>
    public static void Decode(BlockKind kind, int width, int height, ReadOnlySpan<byte> blocks, Span<byte> rgba)
    {
        var blockBytes = kind is BlockKind.Bc1 or BlockKind.Bc4 ? 8 : 16;
        var blocksX = (width + 3) / 4;
        var blocksY = (height + 3) / 4;
        var pitch = width * 4;
        for (var by = 0; by < blocksY; by++)
        {
            for (var bx = 0; bx < blocksX; bx++)
            {
                var blockOffset = (by * blocksX + bx) * blockBytes;
                if (blockOffset + blockBytes > blocks.Length) break;
                var outputOffset = by * 4 * pitch + bx * 16;
                if (outputOffset < rgba.Length)
                    DecodeBlock(kind, blocks.Slice(blockOffset, blockBytes), rgba[outputOffset..], pitch);
            }
        }
    }

    private static void DecodeBlock(BlockKind kind, ReadOnlySpan<byte> block, Span<byte> output, int pitch)
    {
        switch (kind)
        {
            case BlockKind.Bc1:
                ColorBlock(block, output, pitch, opaque: false);
                break;
            case BlockKind.Bc2:
                ColorBlock(block[8..], output, pitch, opaque: true);
                SharpAlphaBlock(block, output, pitch);
                break;
            case BlockKind.Bc3:
                ColorBlock(block[8..], output, pitch, opaque: true);
                SmoothAlphaBlock(block, output[3..], pitch, 4);
                break;
            case BlockKind.Bc4:
            {
                Span<byte> r = stackalloc byte[16];
                SmoothAlphaBlock(block, r, 4, 1);
                for (var y = 0; y < 4; y++)
                    for (var x = 0; x < 4; x++)
                    {
                        var o = y * pitch + x * 4;
                        output[o] = r[y * 4 + x];
                        output[o + 1] = 0;
                        output[o + 2] = 0;
                        output[o + 3] = 0;
                    }
                break;
            }
            case BlockKind.Bc5:
            {
                Span<byte> rg = stackalloc byte[32];
                SmoothAlphaBlock(block, rg, 8, 2);
                SmoothAlphaBlock(block[8..], rg[1..], 8, 2);
                for (var y = 0; y < 4; y++)
                    for (var x = 0; x < 4; x++)
                    {
                        var o = y * pitch + x * 4;
                        output[o] = rg[y * 8 + x * 2];
                        output[o + 1] = rg[y * 8 + x * 2 + 1];
                        output[o + 2] = 0;
                        output[o + 3] = 0;
                    }
                break;
            }
            case BlockKind.Bc6h:
            {
                Span<float> rgb = stackalloc float[48];
                Bc6hFloat(block, rgb, 12, isSigned: false);
                for (var y = 0; y < 4; y++)
                    for (var x = 0; x < 4; x++)
                    {
                        var o = y * pitch + x * 4;
                        var i = y * 12 + x * 3;
                        output[o] = LinearToSrgb(rgb[i]);
                        output[o + 1] = LinearToSrgb(rgb[i + 1]);
                        output[o + 2] = LinearToSrgb(rgb[i + 2]);
                        output[o + 3] = 0;
                    }
                break;
            }
            case BlockKind.Bc7:
                Bc7(block, output, pitch);
                break;
        }
    }

    private static byte LinearToSrgb(float linear)
    {
        var v = linear <= 0.0031308f ? linear * 12.92f : 1.055f * Sat.Pow(linear, 1f / 2.4f) - 0.055f;
        return Sat.ToByte(MathF.Round(Math.Clamp(v, 0f, 1f) * 255f, MidpointRounding.AwayFromZero));
    }

    /// <summary>A BC1 colour block (also the colour half of BC2 and BC3, always in 4-colour mode there).</summary>
    private static void ColorBlock(ReadOnlySpan<byte> block, Span<byte> output, int pitch, bool opaque)
    {
        Span<uint> colors = stackalloc uint[4];
        var c0 = BinaryPrimitives.ReadUInt16LittleEndian(block);
        var c1 = BinaryPrimitives.ReadUInt16LittleEndian(block[2..]);

        // 565 reference colours
        uint r0 = (uint)(c0 >> 11) & 0x1F, g0 = (uint)(c0 >> 5) & 0x3F, b0 = c0 & 0x1Fu;
        uint r1 = (uint)(c1 >> 11) & 0x1F, g1 = (uint)(c1 >> 5) & 0x3F, b1 = c1 & 0x1Fu;

        // expanded to 888
        uint r = (r0 * 527 + 23) >> 6, g = (g0 * 259 + 33) >> 6, b = (b0 * 527 + 23) >> 6;
        colors[0] = 0xFF000000 | (b << 16) | (g << 8) | r;
        r = (r1 * 527 + 23) >> 6; g = (g1 * 259 + 33) >> 6; b = (b1 * 527 + 23) >> 6;
        colors[1] = 0xFF000000 | (b << 16) | (g << 8) | r;

        if (c0 > c1 || opaque)
        {
            // colour 2 = 2/3 c0 + 1/3 c1, colour 3 = 1/3 c0 + 2/3 c1
            r = ((2 * r0 + r1) * 351 + 61) >> 7;
            g = ((2 * g0 + g1) * 2763 + 1039) >> 11;
            b = ((2 * b0 + b1) * 351 + 61) >> 7;
            colors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;
            r = ((r0 + r1 * 2) * 351 + 61) >> 7;
            g = ((g0 + g1 * 2) * 2763 + 1039) >> 11;
            b = ((b0 + b1 * 2) * 351 + 61) >> 7;
            colors[3] = 0xFF000000 | (b << 16) | (g << 8) | r;
        }
        else
        {
            // BC1A: colour 2 = 1/2 c0 + 1/2 c1, colour 3 = transparent black
            r = ((r0 + r1) * 1053 + 125) >> 8;
            g = ((g0 + g1) * 4145 + 1019) >> 11;
            b = ((b0 + b1) * 1053 + 125) >> 8;
            colors[2] = 0xFF000000 | (b << 16) | (g << 8) | r;
            colors[3] = 0;
        }

        var indices = BinaryPrimitives.ReadUInt32LittleEndian(block[4..]);
        for (var i = 0; i < 4; i++)
            for (var j = 0; j < 4; j++)
            {
                BinaryPrimitives.WriteUInt32LittleEndian(output.Slice(i * pitch + j * 4, 4), colors[(int)(indices & 3)]);
                indices >>= 2;
            }
    }

    /// <summary>A BC2 alpha block: 4 bits a pixel.</summary>
    private static void SharpAlphaBlock(ReadOnlySpan<byte> block, Span<byte> output, int pitch)
    {
        for (var i = 0; i < 4; i++)
            for (var j = 0; j < 4; j++)
                output[i * pitch + j * 4 + 3] = (byte)(((block[i * 2 + j / 2] >> (j % 2 * 4)) & 0x0F) * 17);
    }

    /// <summary>A BC3 alpha block (also BC4 and each BC5 channel), written every <paramref name="pixelSize"/> bytes.</summary>
    private static void SmoothAlphaBlock(ReadOnlySpan<byte> block, Span<byte> output, int pitch, int pixelSize)
    {
        var bits = BinaryPrimitives.ReadUInt64LittleEndian(block);
        Span<byte> alpha = stackalloc byte[8];
        alpha[0] = (byte)bits;
        alpha[1] = (byte)(bits >> 8);
        int a0 = alpha[0], a1 = alpha[1];
        if (a0 > a1)
        {
            alpha[2] = (byte)((6 * a0 + a1) / 7);
            alpha[3] = (byte)((5 * a0 + 2 * a1) / 7);
            alpha[4] = (byte)((4 * a0 + 3 * a1) / 7);
            alpha[5] = (byte)((3 * a0 + 4 * a1) / 7);
            alpha[6] = (byte)((2 * a0 + 5 * a1) / 7);
            alpha[7] = (byte)((a0 + 6 * a1) / 7);
        }
        else
        {
            alpha[2] = (byte)((4 * a0 + a1) / 5);
            alpha[3] = (byte)((3 * a0 + 2 * a1) / 5);
            alpha[4] = (byte)((2 * a0 + 3 * a1) / 5);
            alpha[5] = (byte)((a0 + 4 * a1) / 5);
            alpha[6] = 0x00;
            alpha[7] = 0xFF;
        }
        var indices = bits >> 16;
        for (var i = 0; i < 4; i++)
            for (var j = 0; j < 4; j++)
            {
                output[i * pitch + j * pixelSize] = alpha[(int)(indices & 7)];
                indices >>= 3;
            }
    }

    // BC6H: per mode, the endpoint precision W and the delta precisions of R, G and B.
    private static ReadOnlySpan<byte> Bc6hBits =>
    [
        10, 7, 11, 11, 11, 9, 8, 8, 8, 6, 10, 11, 12, 16, // W
        5, 6, 5, 4, 4, 5, 6, 5, 5, 6, 10, 9, 8, 4, // dR
        5, 6, 4, 5, 4, 5, 5, 6, 5, 6, 10, 9, 8, 4, // dG
        5, 6, 4, 4, 5, 5, 5, 5, 6, 6, 10, 9, 8, 4, // dB
    ];

    // The 32 two-region shapes of BC6H, a 4x4 block each; fix-up indices have bit 7 set.
    private static ReadOnlySpan<byte> Bc6hPartitions =>
    [
        128, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 129, // 0
        128, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 129, // 1
        128, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 129, // 2
        128, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 129, // 3
        128, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 129, // 4
        128, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 5
        128, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 6
        128, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 129, // 7
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 129, // 8
        128, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 9
        128, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 10
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 129, // 11
        128, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 12
        128, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 129, // 13
        128, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 14
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 129, // 15
        128, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 129, // 16
        128, 1, 129, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, // 17
        128, 0, 0, 0, 0, 0, 0, 0, 129, 0, 0, 0, 1, 1, 1, 0, // 18
        128, 1, 129, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, // 19
        128, 0, 129, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, // 20
        128, 0, 0, 0, 1, 0, 0, 0, 129, 1, 0, 0, 1, 1, 1, 0, // 21
        128, 0, 0, 0, 0, 0, 0, 0, 129, 0, 0, 0, 1, 1, 0, 0, // 22
        128, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 129, // 23
        128, 0, 129, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, // 24
        128, 0, 0, 0, 1, 0, 0, 0, 129, 0, 0, 0, 1, 1, 0, 0, // 25
        128, 1, 129, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, // 26
        128, 0, 129, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, // 27
        128, 0, 0, 1, 0, 1, 1, 1, 129, 1, 1, 0, 1, 0, 0, 0, // 28
        128, 0, 0, 0, 1, 1, 1, 1, 129, 1, 1, 1, 0, 0, 0, 0, // 29
        128, 1, 129, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, // 30
        128, 0, 129, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, // 31
    ];

    private static readonly int[] Weight2 = [0, 21, 43, 64];
    private static readonly int[] Weight3 = [0, 9, 18, 27, 37, 46, 55, 64];
    private static readonly int[] Weight4 = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64];

    /// <summary>A BC6H block to RGB half floats (<paramref name="output"/>: 3 halves a pixel, <paramref name="pitch"/> a row).</summary>
    private static void Bc6h(ReadOnlySpan<byte> block, Span<Half> output, int pitch, bool isSigned)
    {
        var bs = new BitStream(block);
        Span<int> r = stackalloc int[4];
        Span<int> g = stackalloc int[4];
        Span<int> b = stackalloc int[4];
        r.Clear(); g.Clear(); b.Clear();

        var mode = (int)bs.ReadBits(2);
        if (mode > 1) mode |= (int)bs.ReadBits(3) << 2;

        // modes 10 and up (0-based) have no partition
        var partition = 0;
        switch (mode)
        {
            // Mode 1
            case 0b00:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 75 bits (10.555, 10.555, 10.555)
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(5); // rx[4:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(5); // gx[4:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(5); // bx[4:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(5); // ry[4:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(5); // rz[4:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 0;
                break;

            // Mode 2
            case 0b01:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 75 bits (7666, 7666, 7666)
                g[2] |= bs.ReadBitsI32(1) << 5; // gy[5]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[3] |= bs.ReadBitsI32(1) << 5; // gz[5]
                r[0] |= bs.ReadBitsI32(7); // rw[6:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(7); // gw[6:0]
                b[2] |= bs.ReadBitsI32(1) << 5; // by[5]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(7); // bw[6:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                b[3] |= bs.ReadBitsI32(1) << 5; // bz[5]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(6); // rx[5:0]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(6); // gx[5:0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(6); // bx[5:0]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(6); // ry[5:0]
                r[3] |= bs.ReadBitsI32(6); // rz[5:0]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 1;
                break;

            // Mode 3
            case 0b00010:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (11.555, 11.444, 11.444)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(5); // rx[4:0]
                r[0] |= bs.ReadBitsI32(1) << 10; // rw[10]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(4); // gx[3:0]
                g[0] |= bs.ReadBitsI32(1) << 10; // gw[10]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(4); // bx[3:0]
                b[0] |= bs.ReadBitsI32(1) << 10; // bw[10]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(5); // ry[4:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(5); // rz[4:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 2;
                break;
            // Mode 4
            case 0b00110:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (11.444, 11.555, 11.444)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(4); // rx[3:0]
                r[0] |= bs.ReadBitsI32(1) << 10; // rw[10]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(5); // gx[4:0]
                g[0] |= bs.ReadBitsI32(1) << 10; // gw[10]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(4); // bx[3:0]
                b[0] |= bs.ReadBitsI32(1) << 10; // bw[10]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(4); // ry[3:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(4); // rz[3:0]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 3;
                break;
            // Mode 5
            case 0b01010:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (11.444, 11.444, 11.555)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(4); // rx[3:0]
                r[0] |= bs.ReadBitsI32(1) << 10; // rw[10]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(4); // gx[3:0]
                g[0] |= bs.ReadBitsI32(1) << 10; // gw[10]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(5); // bx[4:0]
                b[0] |= bs.ReadBitsI32(1) << 10; // bw[10]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(4); // ry[3:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(4); // rz[3:0]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 4;
                break;
            // Mode 6
            case 0b01110:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (9555, 9555, 9555)
                r[0] |= bs.ReadBitsI32(9); // rw[8:0]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(9); // gw[8:0]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(9); // bw[8:0]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(5); // rx[4:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(5); // gx[4:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gx[3:0]
                b[1] |= bs.ReadBitsI32(5); // bx[4:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(5); // ry[4:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(5); // rz[4:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 5;
                break;
            // Mode 7
            case 0b10010:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (8666, 8555, 8555)
                r[0] |= bs.ReadBitsI32(8); // rw[7:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(8); // gw[7:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(8); // bw[7:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(6); // rx[5:0]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(5); // gx[4:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(5); // bx[4:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(6); // ry[5:0]
                r[3] |= bs.ReadBitsI32(6); // rz[5:0]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 6;
                break;
            // Mode 8
            case 0b10110:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (8555, 8666, 8555)
                r[0] |= bs.ReadBitsI32(8); // rw[7:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(8); // gw[7:0]
                g[2] |= bs.ReadBitsI32(1) << 5; // gy[5]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(8); // bw[7:0]
                g[3] |= bs.ReadBitsI32(1) << 5; // gz[5]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(5); // rx[4:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(6); // gx[5:0]
                g[3] |= bs.ReadBitsI32(4); // zx[3:0]
                b[1] |= bs.ReadBitsI32(5); // bx[4:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(5); // ry[4:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(5); // rz[4:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 7;
                break;
            // Mode 9
            case 0b11010:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (8555, 8555, 8666)
                r[0] |= bs.ReadBitsI32(8); // rw[7:0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(8); // gw[7:0]
                b[2] |= bs.ReadBitsI32(1) << 5; // by[5]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(8); // bw[7:0]
                b[3] |= bs.ReadBitsI32(1) << 5; // bz[5]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(5); // bw[4:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(5); // gx[4:0]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(6); // bx[5:0]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(5); // ry[4:0]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                r[3] |= bs.ReadBitsI32(5); // rz[4:0]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 8;
                break;
            // Mode 10
            case 0b11110:
                // Partition indices: 46 bits
                // Partition: 5 bits
                // Color Endpoints: 72 bits (6666, 6666, 6666)
                r[0] |= bs.ReadBitsI32(6); // rw[5:0]
                g[3] |= bs.ReadBitsI32(1) << 4; // gz[4]
                b[3] |= bs.ReadBitsI32(1); // bz[0]
                b[3] |= bs.ReadBitsI32(1) << 1; // bz[1]
                b[2] |= bs.ReadBitsI32(1) << 4; // by[4]
                g[0] |= bs.ReadBitsI32(6); // gw[5:0]
                g[2] |= bs.ReadBitsI32(1) << 5; // gy[5]
                b[2] |= bs.ReadBitsI32(1) << 5; // by[5]
                b[3] |= bs.ReadBitsI32(1) << 2; // bz[2]
                g[2] |= bs.ReadBitsI32(1) << 4; // gy[4]
                b[0] |= bs.ReadBitsI32(6); // bw[5:0]
                g[3] |= bs.ReadBitsI32(1) << 5; // gz[5]
                b[3] |= bs.ReadBitsI32(1) << 3; // bz[3]
                b[3] |= bs.ReadBitsI32(1) << 5; // bz[5]
                b[3] |= bs.ReadBitsI32(1) << 4; // bz[4]
                r[1] |= bs.ReadBitsI32(6); // rx[5:0]
                g[2] |= bs.ReadBitsI32(4); // gy[3:0]
                g[1] |= bs.ReadBitsI32(6); // gx[5:0]
                g[3] |= bs.ReadBitsI32(4); // gz[3:0]
                b[1] |= bs.ReadBitsI32(6); // bx[5:0]
                b[2] |= bs.ReadBitsI32(4); // by[3:0]
                r[2] |= bs.ReadBitsI32(6); // ry[5:0]
                r[3] |= bs.ReadBitsI32(6); // rz[5:0]
                partition = bs.ReadBitsI32(5); // d[4:0]
                mode = 9;
                break;
            // Mode 11
            case 0b00011:
                // Partition indices: 63 bits
                // Partition: 0 bits
                // Color Endpoints: 60 bits (10.10, 10.10, 10.10)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(10); // rx[9:0]
                g[1] |= bs.ReadBitsI32(10); // gx[9:0]
                b[1] |= bs.ReadBitsI32(10); // bx[9:0]
                mode = 10;
                break;
            // Mode 12
            case 0b00111:
                // Partition indices: 63 bits
                // Partition: 0 bits
                // Color Endpoints: 60 bits (11.9, 11.9, 11.9)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(9); // rx[8:0]
                r[0] |= bs.ReadBitsI32(1) << 10; // rw[10]
                g[1] |= bs.ReadBitsI32(9); // gx[8:0]
                g[0] |= bs.ReadBitsI32(1) << 10; // gw[10]
                b[1] |= bs.ReadBitsI32(9); // bx[8:0]
                b[0] |= bs.ReadBitsI32(1) << 10; // bw[10]
                mode = 11;
                break;
            // Mode 13
            case 0b01011:
                // Partition indices: 63 bits
                // Partition: 0 bits
                // Color Endpoints: 60 bits (12.8, 12.8, 12.8)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(8); // rx[7:0]
                r[0] |= bs.ReadBitsReversed(2) << 10; // rx[10:11]
                g[1] |= bs.ReadBitsI32(8); // gx[7:0]
                g[0] |= bs.ReadBitsReversed(2) << 10; // gx[10:11]
                b[1] |= bs.ReadBitsI32(8); // bx[7:0]
                b[0] |= bs.ReadBitsReversed(2) << 10; // bx[10:11]
                mode = 12;
                break;
            // Mode 14
            case 0b01111:
                // Partition indices: 63 bits
                // Partition: 0 bits
                // Color Endpoints: 60 bits (16.4, 16.4, 16.4)
                r[0] |= bs.ReadBitsI32(10); // rw[9:0]
                g[0] |= bs.ReadBitsI32(10); // gw[9:0]
                b[0] |= bs.ReadBitsI32(10); // bw[9:0]
                r[1] |= bs.ReadBitsI32(4); // rx[3:0]
                r[0] |= bs.ReadBitsReversed(6) << 10; // rw[10:15]
                g[1] |= bs.ReadBitsI32(4); // gx[3:0]
                g[0] |= bs.ReadBitsReversed(6) << 10; // gw[10:15]
                b[1] |= bs.ReadBitsI32(4); // bx[3:0]
                b[0] |= bs.ReadBitsReversed(6) << 10; // bw[10:15]
                mode = 13;
                break;

            default:
                // Reserved modes decode to zero in every colour channel.
                for (var i = 0; i < 4; i++) output.Slice(i * pitch, 4 * 3).Clear();
                return;
        }

        var numPartitions = mode >= 10 ? 0 : 1;
        int bits0 = Bc6hBits[mode];
        if (isSigned)
        {
            r[0] = ExtendSign(r[0], bits0);
            g[0] = ExtendSign(g[0], bits0);
            b[0] = ExtendSign(b[0], bits0);
        }

        // Modes 9 and 10 store both endpoints explicitly (no deltas).
        if ((mode != 9 && mode != 10) || isSigned)
        {
            for (var i = 1; i < (numPartitions + 1) * 2; i++)
            {
                r[i] = ExtendSign(r[i], Bc6hBits[14 + mode]);
                g[i] = ExtendSign(g[i], Bc6hBits[28 + mode]);
                b[i] = ExtendSign(b[i], Bc6hBits[42 + mode]);
            }
        }

        if (mode != 9 && mode != 10)
        {
            for (var i = 1; i < (numPartitions + 1) * 2; i++)
            {
                r[i] = TransformInverse(r[i], r[0], bits0, isSigned);
                g[i] = TransformInverse(g[i], g[0], bits0, isSigned);
                b[i] = TransformInverse(b[i], b[0], bits0, isSigned);
            }
        }

        for (var i = 0; i < (numPartitions + 1) * 2; i++)
        {
            r[i] = Unquantize(r[i], bits0, isSigned);
            g[i] = Unquantize(g[i], bits0, isSigned);
            b[i] = Unquantize(b[i], bits0, isSigned);
        }

        var weights = mode >= 10 ? Weight4 : Weight3;
        for (var i = 0; i < 4; i++)
        {
            for (var j = 0; j < 4; j++)
            {
                var partitionSet = mode >= 10 ? ((i | j) == 0 ? 128 : 0) : Bc6hPartitions[partition * 16 + i * 4 + j];
                var indexBits = mode >= 10 ? 4 : 3;
                // a fix-up index has one bit less; subset 0's is always index 0
                if ((partitionSet & 0x80) != 0) indexBits--;
                partitionSet &= 0x01;

                var index = bs.ReadBitsI32(indexBits);
                var ep = partitionSet * 2;
                var o = i * pitch + j * 3;
                output[o] = BitConverter.UInt16BitsToHalf(FinishUnquantize(Interpolate(r[ep], r[ep + 1], weights, index), isSigned));
                output[o + 1] = BitConverter.UInt16BitsToHalf(FinishUnquantize(Interpolate(g[ep], g[ep + 1], weights, index), isSigned));
                output[o + 2] = BitConverter.UInt16BitsToHalf(FinishUnquantize(Interpolate(b[ep], b[ep + 1], weights, index), isSigned));
            }
        }
    }

    /// <summary>A BC6H block to RGB floats (3 floats a pixel, <paramref name="pitch"/> floats a row).</summary>
    private static void Bc6hFloat(ReadOnlySpan<byte> block, Span<float> output, int pitch, bool isSigned)
    {
        Span<Half> halves = stackalloc Half[48];
        halves.Clear();
        Bc6h(block, halves, 12, isSigned);
        for (var i = 0; i < 4; i++)
            for (var j = 0; j < 4; j++)
            {
                var src = i * 12 + j * 3;
                var dst = i * pitch + j * 3;
                output[dst] = (float)halves[src];
                output[dst + 1] = (float)halves[src + 1];
                output[dst + 2] = (float)halves[src + 2];
            }
    }

    private static ReadOnlySpan<byte> Bc7Bits =>
    [
        4, 6, 5, 7, 5, 7, 7, 5, // RGBA
        0, 0, 0, 0, 6, 8, 7, 5, // alpha
    ];

    // The 64 two-region shapes, then the 64 three-region shapes of BC7, a 4x4 block each; fix-up indices have bit 7 set.
    private static ReadOnlySpan<byte> Bc7Partitions =>
    [
        128, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 129, // 0
        128, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 129, // 1
        128, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 129, // 2
        128, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 129, // 3
        128, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 129, // 4
        128, 0, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 5
        128, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 6
        128, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 1, 0, 1, 1, 129, // 7
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 129, // 8
        128, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 9
        128, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 129, // 10
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 129, // 11
        128, 0, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 12
        128, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 129, // 13
        128, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 129, // 14
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 129, // 15
        128, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 129, // 16
        128, 1, 129, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, // 17
        128, 0, 0, 0, 0, 0, 0, 0, 129, 0, 0, 0, 1, 1, 1, 0, // 18
        128, 1, 129, 1, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0, 0, // 19
        128, 0, 129, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, // 20
        128, 0, 0, 0, 1, 0, 0, 0, 129, 1, 0, 0, 1, 1, 1, 0, // 21
        128, 0, 0, 0, 0, 0, 0, 0, 129, 0, 0, 0, 1, 1, 0, 0, // 22
        128, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 129, // 23
        128, 0, 129, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, // 24
        128, 0, 0, 0, 1, 0, 0, 0, 129, 0, 0, 0, 1, 1, 0, 0, // 25
        128, 1, 129, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, // 26
        128, 0, 129, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 1, 0, 0, // 27
        128, 0, 0, 1, 0, 1, 1, 1, 129, 1, 1, 0, 1, 0, 0, 0, // 28
        128, 0, 0, 0, 1, 1, 1, 1, 129, 1, 1, 1, 0, 0, 0, 0, // 29
        128, 1, 129, 1, 0, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, // 30
        128, 0, 129, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, // 31
        128, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 129, // 32
        128, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 129, // 33
        128, 1, 0, 1, 1, 0, 129, 0, 0, 1, 0, 1, 1, 0, 1, 0, // 34
        128, 0, 1, 1, 0, 0, 1, 1, 129, 1, 0, 0, 1, 1, 0, 0, // 35
        128, 0, 129, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 0, // 36
        128, 1, 0, 1, 0, 1, 0, 1, 129, 0, 1, 0, 1, 0, 1, 0, // 37
        128, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 129, // 38
        128, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 129, // 39
        128, 1, 129, 1, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0, // 40
        128, 0, 0, 1, 0, 0, 1, 1, 129, 1, 0, 0, 1, 0, 0, 0, // 41
        128, 0, 129, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 0, 0, // 42
        128, 0, 129, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 0, 0, // 43
        128, 1, 129, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, // 44
        128, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 129, // 45
        128, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 129, // 46
        128, 0, 0, 0, 0, 1, 129, 0, 0, 1, 1, 0, 0, 0, 0, 0, // 47
        128, 1, 0, 0, 1, 1, 129, 0, 0, 1, 0, 0, 0, 0, 0, 0, // 48
        128, 0, 129, 0, 0, 1, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, // 49
        128, 0, 0, 0, 0, 0, 129, 0, 0, 1, 1, 1, 0, 0, 1, 0, // 50
        128, 0, 0, 0, 0, 1, 0, 0, 129, 1, 1, 0, 0, 1, 0, 0, // 51
        128, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 129, // 52
        128, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 0, 1, 0, 0, 129, // 53
        128, 1, 129, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, // 54
        128, 0, 129, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 1, 1, 0, // 55
        128, 1, 1, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 0, 0, 129, // 56
        128, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1, 1, 1, 0, 0, 129, // 57
        128, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 129, // 58
        128, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 1, 1, 129, // 59
        128, 0, 0, 0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 1, 129, // 60
        128, 0, 129, 1, 0, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, // 61
        128, 0, 129, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, // 62
        128, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 1, 1, 129, // 63
        // three regions
        128, 0, 1, 129, 0, 0, 1, 1, 0, 2, 2, 1, 2, 2, 2, 130, // 0
        128, 0, 0, 129, 0, 0, 1, 1, 130, 2, 1, 1, 2, 2, 2, 1, // 1
        128, 0, 0, 0, 2, 0, 0, 1, 130, 2, 1, 1, 2, 2, 1, 129, // 2
        128, 2, 2, 130, 0, 0, 2, 2, 0, 0, 1, 1, 0, 1, 1, 129, // 3
        128, 0, 0, 0, 0, 0, 0, 0, 129, 1, 2, 2, 1, 1, 2, 130, // 4
        128, 0, 1, 129, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 2, 130, // 5
        128, 0, 2, 130, 0, 0, 2, 2, 1, 1, 1, 1, 1, 1, 1, 129, // 6
        128, 0, 1, 1, 0, 0, 1, 1, 130, 2, 1, 1, 2, 2, 1, 129, // 7
        128, 0, 0, 0, 0, 0, 0, 0, 129, 1, 1, 1, 2, 2, 2, 130, // 8
        128, 0, 0, 0, 1, 1, 1, 1, 129, 1, 1, 1, 2, 2, 2, 130, // 9
        128, 0, 0, 0, 1, 1, 129, 1, 2, 2, 2, 2, 2, 2, 2, 130, // 10
        128, 0, 1, 2, 0, 0, 129, 2, 0, 0, 1, 2, 0, 0, 1, 130, // 11
        128, 1, 1, 2, 0, 1, 129, 2, 0, 1, 1, 2, 0, 1, 1, 130, // 12
        128, 1, 2, 2, 0, 129, 2, 2, 0, 1, 2, 2, 0, 1, 2, 130, // 13
        128, 0, 1, 129, 0, 1, 1, 2, 1, 1, 2, 2, 1, 2, 2, 130, // 14
        128, 0, 1, 129, 2, 0, 0, 1, 130, 2, 0, 0, 2, 2, 2, 0, // 15
        128, 0, 0, 129, 0, 0, 1, 1, 0, 1, 1, 2, 1, 1, 2, 130, // 16
        128, 1, 1, 129, 0, 0, 1, 1, 130, 0, 0, 1, 2, 2, 0, 0, // 17
        128, 0, 0, 0, 1, 1, 2, 2, 129, 1, 2, 2, 1, 1, 2, 130, // 18
        128, 0, 2, 130, 0, 0, 2, 2, 0, 0, 2, 2, 1, 1, 1, 129, // 19
        128, 1, 1, 129, 0, 1, 1, 1, 0, 2, 2, 2, 0, 2, 2, 130, // 20
        128, 0, 0, 129, 0, 0, 0, 1, 130, 2, 2, 1, 2, 2, 2, 1, // 21
        128, 0, 0, 0, 0, 0, 129, 1, 0, 1, 2, 2, 0, 1, 2, 130, // 22
        128, 0, 0, 0, 1, 1, 0, 0, 130, 2, 129, 0, 2, 2, 1, 0, // 23
        128, 1, 2, 130, 0, 129, 2, 2, 0, 0, 1, 1, 0, 0, 0, 0, // 24
        128, 0, 1, 2, 0, 0, 1, 2, 129, 1, 2, 2, 2, 2, 2, 130, // 25
        128, 1, 1, 0, 1, 2, 130, 1, 129, 2, 2, 1, 0, 1, 1, 0, // 26
        128, 0, 0, 0, 0, 1, 129, 0, 1, 2, 130, 1, 1, 2, 2, 1, // 27
        128, 0, 2, 2, 1, 1, 0, 2, 129, 1, 0, 2, 0, 0, 2, 130, // 28
        128, 1, 1, 0, 0, 129, 1, 0, 2, 0, 0, 2, 2, 2, 2, 130, // 29
        128, 0, 1, 1, 0, 1, 2, 2, 0, 1, 130, 2, 0, 0, 1, 129, // 30
        128, 0, 0, 0, 2, 0, 0, 0, 130, 2, 1, 1, 2, 2, 2, 129, // 31
        128, 0, 0, 0, 0, 0, 0, 2, 129, 1, 2, 2, 1, 2, 2, 130, // 32
        128, 2, 2, 130, 0, 0, 2, 2, 0, 0, 1, 2, 0, 0, 1, 129, // 33
        128, 0, 1, 129, 0, 0, 1, 2, 0, 0, 2, 2, 0, 2, 2, 130, // 34
        128, 1, 2, 0, 0, 129, 2, 0, 0, 1, 130, 0, 0, 1, 2, 0, // 35
        128, 0, 0, 0, 1, 1, 129, 1, 2, 2, 130, 2, 0, 0, 0, 0, // 36
        128, 1, 2, 0, 1, 2, 0, 1, 130, 0, 129, 2, 0, 1, 2, 0, // 37
        128, 1, 2, 0, 2, 0, 1, 2, 129, 130, 0, 1, 0, 1, 2, 0, // 38
        128, 0, 1, 1, 2, 2, 0, 0, 1, 1, 130, 2, 0, 0, 1, 129, // 39
        128, 0, 1, 1, 1, 1, 130, 2, 2, 2, 0, 0, 0, 0, 1, 129, // 40
        128, 1, 0, 129, 0, 1, 0, 1, 2, 2, 2, 2, 2, 2, 2, 130, // 41
        128, 0, 0, 0, 0, 0, 0, 0, 130, 1, 2, 1, 2, 1, 2, 129, // 42
        128, 0, 2, 2, 1, 129, 2, 2, 0, 0, 2, 2, 1, 1, 2, 130, // 43
        128, 0, 2, 130, 0, 0, 1, 1, 0, 0, 2, 2, 0, 0, 1, 129, // 44
        128, 2, 2, 0, 1, 2, 130, 1, 0, 2, 2, 0, 1, 2, 2, 129, // 45
        128, 1, 0, 1, 2, 2, 130, 2, 2, 2, 2, 2, 0, 1, 0, 129, // 46
        128, 0, 0, 0, 2, 1, 2, 1, 130, 1, 2, 1, 2, 1, 2, 129, // 47
        128, 1, 0, 129, 0, 1, 0, 1, 0, 1, 0, 1, 2, 2, 2, 130, // 48
        128, 2, 2, 130, 0, 1, 1, 1, 0, 2, 2, 2, 0, 1, 1, 129, // 49
        128, 0, 0, 2, 1, 129, 1, 2, 0, 0, 0, 2, 1, 1, 1, 130, // 50
        128, 0, 0, 0, 2, 129, 1, 2, 2, 1, 1, 2, 2, 1, 1, 130, // 51
        128, 2, 2, 2, 0, 129, 1, 1, 0, 1, 1, 1, 0, 2, 2, 130, // 52
        128, 0, 0, 2, 1, 1, 1, 2, 129, 1, 1, 2, 0, 0, 0, 130, // 53
        128, 1, 1, 0, 0, 129, 1, 0, 0, 1, 1, 0, 2, 2, 2, 130, // 54
        128, 0, 0, 0, 0, 0, 0, 0, 2, 1, 129, 2, 2, 1, 1, 130, // 55
        128, 1, 1, 0, 0, 129, 1, 0, 2, 2, 2, 2, 2, 2, 2, 130, // 56
        128, 0, 2, 2, 0, 0, 1, 1, 0, 0, 129, 1, 0, 0, 2, 130, // 57
        128, 0, 2, 2, 1, 1, 2, 2, 129, 1, 2, 2, 0, 0, 2, 130, // 58
        128, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 129, 1, 130, // 59
        128, 0, 0, 130, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 129, // 60
        128, 2, 2, 2, 1, 2, 2, 2, 0, 2, 2, 2, 129, 2, 2, 130, // 61
        128, 1, 0, 129, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 130, // 62
        128, 1, 1, 129, 2, 0, 1, 1, 130, 2, 0, 1, 2, 2, 2, 0, // 63
    ];

    /// <summary>A BC7 block to RGBA8.</summary>
    private static void Bc7(ReadOnlySpan<byte> block, Span<byte> output, int pitch)
    {
        const int modeHasPBits = 0b11001011;
        var bs = new BitStream(block);

        var mode = 0;
        while (mode < 8 && bs.ReadBits(1) == 0) mode++;

        // an unknown mode clears the block (transparent black)
        if (mode >= 8)
        {
            for (var i = 0; i < 4; i++) output.Slice(i * pitch, 16).Clear();
            return;
        }

        var partition = 0;
        var numPartitions = 1;
        uint rotation = 0;
        uint indexSelectionBit = 0;
        if (mode is 0 or 1 or 2 or 3 or 7)
        {
            numPartitions = mode is 0 or 2 ? 3 : 2;
            partition = (int)bs.ReadBits(mode == 0 ? 4 : 6);
        }
        var numEndpoints = numPartitions * 2;
        if (mode is 4 or 5)
        {
            rotation = bs.ReadBits(2);
            if (mode == 4) indexSelectionBit = bs.ReadBits(1);
        }

        // endpoints[j * 4 + channel]
        Span<int> endpoints = stackalloc int[24];
        endpoints.Clear();
        for (var i = 0; i < 3; i++)
            for (var j = 0; j < numEndpoints; j++)
                endpoints[j * 4 + i] = (int)bs.ReadBits(Bc7Bits[mode]);
        if (Bc7Bits[8 + mode] > 0)
            for (var j = 0; j < numEndpoints; j++)
                endpoints[j * 4 + 3] = (int)bs.ReadBits(Bc7Bits[8 + mode]);

        // P-bits
        if (mode is 0 or 1 or 3 or 6 or 7)
        {
            for (var j = 0; j < numEndpoints * 4; j++) endpoints[j] <<= 1;
            if (mode == 1)
            {
                var i = (int)bs.ReadBits(1);
                var j = (int)bs.ReadBits(1);
                for (var k = 0; k < 3; k++)
                {
                    endpoints[0 * 4 + k] |= i;
                    endpoints[1 * 4 + k] |= i;
                    endpoints[2 * 4 + k] |= j;
                    endpoints[3 * 4 + k] |= j;
                }
            }
            else if ((modeHasPBits & (1 << mode)) != 0)
            {
                for (var e = 0; e < numEndpoints; e++)
                {
                    var j = (int)bs.ReadBits(1);
                    for (var k = 0; k < 4; k++) endpoints[e * 4 + k] |= j;
                }
            }
        }

        // precision to 8 bits, the top bits replicated into the low ones
        for (var i = 0; i < numEndpoints; i++)
        {
            var j = Bc7Bits[mode] + ((modeHasPBits >> mode) & 1);
            for (var k = 0; k < 3; k++)
            {
                endpoints[i * 4 + k] <<= 8 - j;
                endpoints[i * 4 + k] |= endpoints[i * 4 + k] >> j;
            }
            j = Bc7Bits[8 + mode] + ((modeHasPBits >> mode) & 1);
            endpoints[i * 4 + 3] <<= 8 - j;
            endpoints[i * 4 + 3] |= endpoints[i * 4 + 3] >> j;
        }

        // a mode without alpha is opaque
        if (Bc7Bits[8 + mode] == 0)
            for (var e = 0; e < numEndpoints; e++) endpoints[e * 4 + 3] = 0xFF;

        var indexBits = mode switch { 0 or 1 => 3, 6 => 4, _ => 2 };
        var indexBits2 = mode switch { 4 => 3, 5 => 2, _ => 0 };
        var weights = indexBits switch { 2 => Weight2, 3 => Weight3, _ => Weight4 };
        var weights2 = indexBits2 == 2 ? Weight2 : Weight3;

        // pass 1: the colour indices
        Span<int> indices = stackalloc int[16];
        for (var i = 0; i < 4; i++)
        {
            for (var j = 0; j < 4; j++)
            {
                int partitionSet = numPartitions == 1 ? ((i | j) == 0 ? 128 : 0) : Bc7Partitions[(numPartitions - 2) * 1024 + partition * 16 + i * 4 + j];
                var idxBits = mode switch { 0 or 1 => 3, 6 => 4, _ => 2 };
                if ((partitionSet & 0x80) != 0) idxBits--;
                indices[i * 4 + j] = (int)bs.ReadBits(idxBits);
            }
        }

        // pass 2: the alpha indices (if any), interpolation and rotation
        for (var i = 0; i < 4; i++)
        {
            for (var j = 0; j < 4; j++)
            {
                int partitionSet = numPartitions == 1 ? ((i | j) == 0 ? 128 : 0) : Bc7Partitions[(numPartitions - 2) * 1024 + partition * 16 + i * 4 + j];
                partitionSet &= 0x03;
                var index = indices[i * 4 + j];
                var e0 = partitionSet * 2 * 4;
                var e1 = e0 + 4;
                int r, g, b, a;
                if (indexBits2 == 0)
                {
                    r = Interpolate(endpoints[e0], endpoints[e1], weights, index);
                    g = Interpolate(endpoints[e0 + 1], endpoints[e1 + 1], weights, index);
                    b = Interpolate(endpoints[e0 + 2], endpoints[e1 + 2], weights, index);
                    a = Interpolate(endpoints[e0 + 3], endpoints[e1 + 3], weights, index);
                }
                else
                {
                    var index2 = (int)bs.ReadBits((i | j) == 0 ? indexBits2 - 1 : indexBits2);
                    if (indexSelectionBit == 0)
                    {
                        r = Interpolate(endpoints[e0], endpoints[e1], weights, index);
                        g = Interpolate(endpoints[e0 + 1], endpoints[e1 + 1], weights, index);
                        b = Interpolate(endpoints[e0 + 2], endpoints[e1 + 2], weights, index);
                        a = Interpolate(endpoints[e0 + 3], endpoints[e1 + 3], weights2, index2);
                    }
                    else
                    {
                        r = Interpolate(endpoints[e0], endpoints[e1], weights2, index2);
                        g = Interpolate(endpoints[e0 + 1], endpoints[e1 + 1], weights2, index2);
                        b = Interpolate(endpoints[e0 + 2], endpoints[e1 + 2], weights2, index2);
                        a = Interpolate(endpoints[e0 + 3], endpoints[e1 + 3], weights, index);
                    }
                }

                switch (rotation)
                {
                    case 1: (a, r) = (r, a); break; // scalar R, vector AGB
                    case 2: (a, g) = (g, a); break; // scalar G, vector RAB
                    case 3: (a, b) = (b, a); break; // scalar B, vector RGA
                }

                var o = i * pitch + j * 4;
                output[o] = (byte)r;
                output[o + 1] = (byte)g;
                output[o + 2] = (byte)b;
                output[o + 3] = (byte)a;
            }
        }
    }

    private static int Interpolate(int a, int b, int[] weights, int index) => (a * (64 - weights[index]) + b * weights[index] + 32) >> 6;

    private static int ExtendSign(int value, int bits) => (value << (32 - bits)) >> (32 - bits);

    private static int TransformInverse(int value, int a0, int bits, bool isSigned)
    {
        // B0 = (B0 + A0) & ((1 << p) - 1) for an A0 of p bits
        var transformed = (value + a0) & ((1 << bits) - 1);
        return isSigned ? ExtendSign(transformed, bits) : transformed;
    }

    private static int Unquantize(int value, int bits, bool isSigned)
    {
        if (!isSigned)
        {
            if (bits >= 15) return value;
            if (value == 0) return 0;
            if (value == (1 << bits) - 1) return 0xFFFF;
            return ((value << 16) + 0x8000) >> bits;
        }
        if (bits >= 16) return value;
        var negative = value < 0;
        var v = negative ? -value : value;
        var unquantized = v == 0 ? 0 : v >= (1 << (bits - 1)) - 1 ? 0x7FFF : ((v << 15) + 0x4000) >> (bits - 1);
        return negative ? -unquantized : unquantized;
    }

    private static ushort FinishUnquantize(int value, bool isSigned)
    {
        // unsigned: the magnitude times 31/64; signed: times 31/32 with a sign bit
        if (!isSigned) return (ushort)((value * 31) >> 6);
        var scaled = value < 0 ? -((-value * 31) >> 5) : (value * 31) >> 5;
        return scaled < 0 ? (ushort)(0x8000 | -scaled) : (ushort)scaled;
    }

    /// <summary>Reads a 128-bit block from its lowest bit up.</summary>
    private ref struct BitStream(ReadOnlySpan<byte> data)
    {
        private ulong _low = BinaryPrimitives.ReadUInt64LittleEndian(data);
        private ulong _high = BinaryPrimitives.ReadUInt64LittleEndian(data[8..]);

        public uint ReadBits(int count)
        {
            var mask = (1UL << count) - 1;
            var bits = (uint)(_low & mask);
            _low >>= count;
            // the low bits of high into the top of low (a count of 0 moves nothing: its mask is 0)
            _low |= (_high & mask) << (64 - count);
            _high >>= count;
            return bits;
        }

        public int ReadBitsI32(int count) => (int)ReadBits(count);

        public int ReadBitsReversed(int count)
        {
            var bits = ReadBitsI32(count);
            var result = 0;
            for (var i = 0; i < count; i++)
            {
                result = (result << 1) | (bits & 1);
                bits >>= 1;
            }
            return result;
        }
    }
}
