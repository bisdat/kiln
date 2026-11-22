// SPDX-License-Identifier: MIT
// Ported from block_compression 0.6.0 (src/encode/bc6h.rs, with BC6HSettings::basic), MIT, Copyright (c) 2025
// Nils Hasenbanck, Copyright (c) 2016-2024 Intel Corporation (licenses/block_compression-LICENSE.txt).
namespace Kiln;

internal static partial class BlockEncoder
{
    /// <summary>One BC6H (unsigned) block from RGBA8 (block_compression's BlockCompressorBC6H with the "basic"
    /// settings Kiln's texture code uses). The 8-bit channels are taken as sRGB and made linear half floats first.</summary>
    private sealed class Bc6hCompressor
    {
        // BC6HSettings::basic()
        private const int FastSkipThreshold = 4;
        private const int RefineIterations1P = 2;
        private const int RefineIterations2P = 2;

        private static readonly uint[] ModePrefix = [0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15];

        private static readonly float[] SpanTable =
        [
            0.9f * 65535f / 64f, // (0) 4 / 10
            0.9f * 65535f / 4f, // (1) 5 / 7
            0.8f * 65535f / 256f, // (2) 3 / 11
            -1f,
            -1f,
            0.9f * 65535f / 32f, // (5) 4 / 9
            0.9f * 65535f / 16f, // (6) 4 / 8
            -1f,
            -1f,
            65535f, // (9) absolute
            65535f, // (10) absolute
            0.95f * 65535f / 8f, // (11) 8 / 11
            0.95f * 65535f / 32f, // (12) 7 / 12
            6f, // (13) 3 / 16
        ];

        private static readonly uint[] ModeBits = [10, 7, 11, 0xFFFFFFFF, 0xFFFFFFFF, 9, 8, 0xFFFFFFFF, 0xFFFFFFFF, 6, 10, 11, 12, 16];

        private readonly float[] _block = new float[64];
        private float _bestErr = float.PositiveInfinity;
        private readonly float[] _rgbBounds = new float[6];
        private float _maxSpan;
        private int _maxSpanIdx;
        private int _mode;
        private int _epb;
        private readonly int[] _qbounds = new int[8];

        public uint[] Data { get; } = new uint[5];

        private static float SrgbToLinear(byte srgb)
        {
            var v = srgb / 255f;
            return v <= 0.04045f ? v / 12.92f : Sat.Pow((v + 0.055f) / 1.055f, 2.4f);
        }

        /// <summary>The bits of the half float nearest to <paramref name="value"/>, as a float.</summary>
        private static float HalfBits(float value) => BitConverter.HalfToUInt16Bits((Half)value);

        public void LoadInterleaved8Bit(ReadOnlySpan<byte> rgba, int xx, int yy, int stride)
        {
            for (var y = 0; y < 4; y++)
                for (var x = 0; x < 4; x++)
                {
                    var offset = (yy * 4 + y) * stride + (xx * 4 + x) * 4;
                    _block[y * 4 + x] = HalfBits(SrgbToLinear(rgba[offset]));
                    _block[16 + y * 4 + x] = HalfBits(SrgbToLinear(rgba[offset + 1]));
                    _block[32 + y * 4 + x] = HalfBits(SrgbToLinear(rgba[offset + 2]));
                    _block[48 + y * 4 + x] = 0f;
                }
        }

        private void EpQuantBc6h8(ReadOnlySpan<float> ep, int bits, int pairs)
        {
            var levels = 1 << bits;
            for (var i = 0; i < 8 * pairs; i++)
            {
                var v = Sat.ToInt(ep[i] / (256f * 256f - 1f) * (levels - 1) + 0.5f);
                _qbounds[i] = Math.Clamp(v, 0, levels - 1);
            }
        }

        private void ComputeQboundsCore(float spanR, float spanG, float spanB)
        {
            Span<float> rgbSpan = [spanR, spanG, spanB];
            Span<float> bounds = stackalloc float[8];
            bounds.Clear();
            for (var p = 0; p < 3; p++)
            {
                var middle = (_rgbBounds[p] + _rgbBounds[3 + p]) / 2f;
                bounds[p] = middle - rgbSpan[p] / 2f;
                bounds[4 + p] = middle + rgbSpan[p] / 2f;
            }
            EpQuantBc6h8(bounds, _epb, 1);
        }

        private void ComputeQbounds(float span) => ComputeQboundsCore(span, span, span);

        private void ComputeQbounds2(float span, int maxSpanIdx)
        {
            Span<float> rgbSpan = [span, span, span];
            if (maxSpanIdx < 3) rgbSpan[maxSpanIdx] *= 2f;
            ComputeQboundsCore(rgbSpan[0], rgbSpan[1], rgbSpan[2]);
        }

        private static uint UnpackToUf16(uint v, int bits)
        {
            if (bits >= 15) return v;
            if (v == 0) return 0;
            if (v == (1u << bits) - 1) return 0xFFFF;
            return (v * 2 + 1) << (15 - bits);
        }

        private static void EpQuantBc6h(Span<int> qep, ReadOnlySpan<float> ep, int bits, int pairs)
        {
            var levels = 1 << bits;
            for (var i = 0; i < 8 * pairs; i++)
            {
                var v = Sat.ToInt(ep[i] / (256f * 256f - 1f) * (levels - 1) + 0.5f);
                qep[i] = Math.Clamp(v, 0, levels - 1);
            }
        }

        private static void EpDequantBc6h(Span<float> ep, ReadOnlySpan<int> qep, int bits, int pairs)
        {
            for (var i = 0; i < 8 * pairs; i++) ep[i] = UnpackToUf16((uint)qep[i], bits);
        }

        private void EpQuantDequantBc6h(Span<int> qep, Span<float> ep, int pairs)
        {
            var bits = _epb;
            EpQuantBc6h(qep, ep, bits, pairs);
            for (var i = 0; i < 2 * pairs; i++)
                for (var p = 0; p < 3; p++)
                    qep[i * 4 + p] = Math.Clamp(qep[i * 4 + p], _qbounds[p], _qbounds[4 + p]);
            EpDequantBc6h(ep, qep, bits, pairs);
        }

        private void Code2P(Span<int> qep, uint qblock0, uint qblock1, int partId, int mode)
        {
            const int bits = 3;
            var flips = Bc7CodeApplySwapMode01237(qep, qblock0, qblock1, 1, partId);
            Array.Clear(Data);
            uint pos = 0;
            Span<uint> packed = stackalloc uint[4];
            packed.Clear();
            Pack(packed, qep, mode);
            // mode
            PutBits(Data, ref pos, 5, packed[0]);
            // endpoints
            PutBits(Data, ref pos, 30, packed[1]);
            PutBits(Data, ref pos, 30, packed[2]);
            PutBits(Data, ref pos, 12, packed[3]);
            // partition
            PutBits(Data, ref pos, 5, (uint)partId);
            // quantised values
            Bc7CodeQblock(Data, ref pos, qblock0, qblock1, bits, flips);
            Bc7CodeAdjustSkipMode01237(Data, 1, partId);
        }

        private void Code1P(Span<int> qep, Span<uint> qblock, int mode)
        {
            Bc7CodeApplySwapMode456(qep, 4, qblock, 4);
            Array.Clear(Data);
            uint pos = 0;
            Span<uint> packed = stackalloc uint[4];
            packed.Clear();
            Pack(packed, qep, mode);
            // mode
            PutBits(Data, ref pos, 5, packed[0]);
            // endpoints
            PutBits(Data, ref pos, 30, packed[1]);
            PutBits(Data, ref pos, 30, packed[2]);
            // quantised values
            Bc7CodeQblock(Data, ref pos, qblock[0], qblock[1], 4, 0);
        }

        private void Enc2P()
        {
            Span<float> fullStats = stackalloc float[15];
            fullStats.Clear();
            ComputeStatsMasked(fullStats, _block, 0xFFFFFFFF, 3);
            Span<int> partList = stackalloc int[32];
            for (var part = 0; part < 32; part++)
            {
                var bound12 = BlockPcaBoundSplit(_block, GetPatternMask(part, 0), fullStats, 3);
                partList[part] = part + Sat.ToInt(bound12) * 64;
            }
            PartialSortList(partList, 32, FastSkipThreshold);
            Enc2PList(partList, FastSkipThreshold);
        }

        private float Enc2PPartFast(Span<int> qep, Span<uint> qblock, int partId)
        {
            var pattern = GetPattern(partId);
            const int bits = 3;
            const int pairs = 2;
            const int channels = 3;
            Span<float> ep = stackalloc float[24];
            ep.Clear();
            for (var j = 0; j < pairs; j++)
                BlockSegmentCore(ep[(j * 8)..], _block, GetPatternMask(partId, j), channels);
            EpQuantDequantBc6h(qep, ep, 2);
            return BlockQuant(qblock, _block, bits, ep, pattern, channels);
        }

        private void Enc2PList(ReadOnlySpan<int> partList, int partCount)
        {
            if (partCount == 0) return;
            const int bits = 3;
            const int pairs = 2;
            const int channels = 3;
            Span<int> bestQep = stackalloc int[24];
            Span<uint> bestQblock = stackalloc uint[2];
            bestQep.Clear();
            bestQblock.Clear();
            var bestPartId = -1;
            var bestErr = float.PositiveInfinity;
            Span<int> qep = stackalloc int[24];
            Span<uint> qblock = stackalloc uint[2];
            for (var part = 0; part < partCount; part++)
            {
                var partId = partList[part] & 31;
                qep.Clear();
                qblock.Clear();
                var err = Enc2PPartFast(qep, qblock, partId);
                if (err < bestErr)
                {
                    qep[..(8 * pairs)].CopyTo(bestQep);
                    qblock.CopyTo(bestQblock);
                    bestPartId = partId;
                    bestErr = err;
                }
            }
            // refine
            Span<float> ep = stackalloc float[24];
            for (var i = 0; i < RefineIterations2P; i++)
            {
                ep.Clear();
                for (var j = 0; j < pairs; j++)
                    OptEndpoints(ep[(j * 8)..], _block, bits, bestQblock[0], bestQblock[1], GetPatternMask(bestPartId, j), channels);
                qep.Clear();
                qblock.Clear();
                EpQuantDequantBc6h(qep, ep, 2);
                var err = BlockQuant(qblock, _block, bits, ep, GetPattern(bestPartId), channels);
                if (err < bestErr)
                {
                    qep[..(8 * pairs)].CopyTo(bestQep);
                    qblock.CopyTo(bestQblock);
                    bestErr = err;
                }
            }
            if (bestErr < _bestErr)
            {
                _bestErr = bestErr;
                Code2P(bestQep, bestQblock[0], bestQblock[1], bestPartId, _mode);
            }
        }

        private void Enc1P()
        {
            Span<float> ep = stackalloc float[24];
            ep.Clear();
            BlockSegmentCore(ep, _block, 0xFFFFFFFF, 3);
            Span<int> qep = stackalloc int[24];
            qep.Clear();
            EpQuantDequantBc6h(qep, ep, 1);
            Span<uint> qblock = stackalloc uint[2];
            qblock.Clear();
            var err = BlockQuant(qblock, _block, 4, ep, 0, 3);
            // refine
            for (var i = 0; i < RefineIterations1P; i++)
            {
                OptEndpoints(ep, _block, 4, qblock[0], qblock[1], 0xFFFFFFFF, 3);
                EpQuantDequantBc6h(qep, ep, 1);
                err = BlockQuant(qblock, _block, 4, ep, 0, 3);
            }
            if (err < _bestErr)
            {
                _bestErr = err;
                Code1P(qep, qblock, _mode);
            }
        }

        private void TestMode(int mode, bool enc, float margin)
        {
            var modeBits = (int)ModeBits[mode];
            var span = SpanTable[mode];
            var maxSpan = _maxSpan;
            var maxSpanIdx = _maxSpanIdx;
            if (maxSpan * margin > span) return;
            if (mode >= 10)
            {
                _epb = modeBits;
                _mode = mode;
                ComputeQbounds(span);
                if (enc) Enc1P();
            }
            else if (mode <= 1 || mode == 5 || mode == 9)
            {
                _epb = modeBits;
                _mode = mode;
                ComputeQbounds(span);
                if (enc) Enc2P();
            }
            else
            {
                _epb = modeBits;
                _mode = mode + maxSpanIdx;
                ComputeQbounds2(span, maxSpanIdx);
                if (enc) Enc2P();
            }
        }

        private static uint BitAt(int v, int pos) => (uint)((v >> pos) & 1);

        private static uint ReverseBits(uint v, int bits)
        {
            if (bits == 2) return (v >> 1) + (v & 1) * 2;
            if (bits == 6)
            {
                var vv = (v & 0x5555) * 2 + ((v >> 1) & 0x5555);
                return (vv >> 4) + ((vv >> 2) & 3) * 4 + (vv & 3) * 16;
            }
            throw new InvalidOperationException("BC6H bit reversal of " + bits + " bits");
        }

        private static void Pack(Span<uint> packed, ReadOnlySpan<int> qep, int mode)
        {
            if (mode == 0)
            {
                Span<int> predQep = stackalloc int[16];
                for (var p = 0; p < 3; p++)
                {
                    predQep[p] = qep[p];
                    predQep[4 + p] = (qep[4 + p] - qep[p]) & 31;
                    predQep[8 + p] = (qep[8 + p] - qep[p]) & 31;
                    predQep[12 + p] = (qep[12 + p] - qep[p]) & 31;
                }

                Span<uint> pqep = stackalloc uint[10];

                pqep[4] = (uint)predQep[4] + (uint)(predQep[8 + 1] & 15) * 64;
                pqep[5] = (uint)predQep[5] + (uint)(predQep[12 + 1] & 15) * 64;
                pqep[6] = (uint)predQep[6] + (uint)(predQep[8 + 2] & 15) * 64;

                pqep[4] += BitAt(predQep[12 + 1], 4) << 5;
                pqep[5] += BitAt(predQep[12 + 2], 0) << 5;
                pqep[6] += BitAt(predQep[12 + 2], 1) << 5;

                pqep[8] = (uint)predQep[8] + BitAt(predQep[12 + 2], 2) * 32;
                pqep[9] = (uint)predQep[12] + BitAt(predQep[12 + 2], 3) * 32;

                packed[0] = ModePrefix[0];
                packed[0] += BitAt(predQep[8 + 1], 4) << 2;
                packed[0] += BitAt(predQep[8 + 2], 4) << 3;
                packed[0] += BitAt(predQep[12 + 2], 4) << 4;

                packed[1] =
                    (((uint)predQep[2]) << 20) + (((uint)predQep[1]) << 10) + (uint)predQep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (pqep[9] << 6) + pqep[8];
            }
            else if (mode == 1)
            {
                Span<int> predQep = stackalloc int[16];
                for (var p = 0; p < 3; p++)
                {
                    predQep[p] = qep[p];
                    predQep[4 + p] = (qep[4 + p] - qep[p]) & 63;
                    predQep[8 + p] = (qep[8 + p] - qep[p]) & 63;
                    predQep[12 + p] = (qep[12 + p] - qep[p]) & 63;
                }

                Span<uint> pqep = stackalloc uint[8];

                pqep[0] = (uint)predQep[0];
                pqep[0] += BitAt(predQep[12 + 2], 0) << 7;
                pqep[0] += BitAt(predQep[12 + 2], 1) << 8;
                pqep[0] += BitAt(predQep[8 + 2], 4) << 9;

                pqep[1] = (uint)predQep[1];
                pqep[1] += BitAt(predQep[8 + 2], 5) << 7;
                pqep[1] += BitAt(predQep[12 + 2], 2) << 8;
                pqep[1] += BitAt(predQep[8 + 1], 4) << 9;

                pqep[2] = (uint)predQep[2];
                pqep[2] += BitAt(predQep[12 + 2], 3) << 7;
                pqep[2] += BitAt(predQep[12 + 2], 5) << 8;
                pqep[2] += BitAt(predQep[12 + 2], 4) << 9;

                pqep[4] = (uint)predQep[4] + ((uint)(predQep[8 + 1] & 15) * 64);
                pqep[5] = (uint)predQep[5] + ((uint)(predQep[12 + 1] & 15) * 64);
                pqep[6] = (uint)predQep[6] + ((uint)(predQep[8 + 2] & 15) * 64);

                packed[0] = ModePrefix[1];
                packed[0] += BitAt(predQep[8 + 1], 5) << 2;
                packed[0] += BitAt(predQep[12 + 1], 4) << 3;
                packed[0] += BitAt(predQep[12 + 1], 5) << 4;

                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (((uint)predQep[12]) << 6) + (uint)predQep[8];
            }
            else if (mode == 2 || mode == 3 || mode == 4)
            {
                Span<int> dqep = stackalloc int[16];
                for (var p = 0; p < 3; p++)
                {
                    var mask = p == mode - 2 ? 31 : 15;
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & mask;
                    dqep[8 + p] = (qep[8 + p] - qep[p]) & mask;
                    dqep[12 + p] = (qep[12 + p] - qep[p]) & mask;
                }

                Span<uint> pqep = stackalloc uint[10];

                pqep[0] = (uint)(dqep[0] & 1023);
                pqep[1] = (uint)(dqep[1] & 1023);
                pqep[2] = (uint)(dqep[2] & 1023);

                pqep[4] = (uint)dqep[4] + ((uint)(dqep[8 + 1] & 15) * 64);
                pqep[5] = (uint)dqep[5] + ((uint)(dqep[12 + 1] & 15) * 64);
                pqep[6] = (uint)dqep[6] + ((uint)(dqep[8 + 2] & 15) * 64);

                pqep[8] = (uint)dqep[8];
                pqep[9] = (uint)dqep[12];

                if (mode == 2)
            {
                    packed[0] = ModePrefix[2];

                    pqep[5] += BitAt(dqep[1], 10) << 4;
                    pqep[6] += BitAt(dqep[2], 10) << 4;

                    pqep[4] += BitAt(dqep[0], 10) << 5;
                    pqep[5] += BitAt(dqep[12 + 2], 0) << 5;
                    pqep[6] += BitAt(dqep[12 + 2], 1) << 5;
                    pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                    pqep[9] += BitAt(dqep[12 + 2], 3) << 5;
                }
            else if (mode == 3)
            {
                    packed[0] = ModePrefix[3];

                    pqep[4] += BitAt(dqep[0], 10) << 4;
                    pqep[6] += BitAt(dqep[2], 10) << 4;
                    pqep[8] += BitAt(dqep[12 + 2], 0) << 4;
                    pqep[9] += BitAt(dqep[8 + 1], 4) << 4;

                    pqep[4] += BitAt(dqep[12 + 1], 4) << 5;
                    pqep[5] += BitAt(dqep[1], 10) << 5;
                    pqep[6] += BitAt(dqep[12 + 2], 1) << 5;
                    pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                    pqep[9] += BitAt(dqep[12 + 2], 3) << 5;
                }
            else if (mode == 4)
            {
                    packed[0] = ModePrefix[4];

                    pqep[4] += BitAt(dqep[0], 10) << 4;
                    pqep[5] += BitAt(dqep[1], 10) << 4;
                    pqep[8] += BitAt(dqep[12 + 2], 1) << 4;
                    pqep[9] += BitAt(dqep[12 + 2], 4) << 4;

                    pqep[4] += BitAt(dqep[8 + 2], 4) << 5;
                    pqep[5] += BitAt(dqep[12 + 2], 0) << 5;
                    pqep[6] += BitAt(dqep[2], 10) << 5;
                    pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                    pqep[9] += BitAt(dqep[12 + 2], 3) << 5;
                }

                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (pqep[9] << 6) + pqep[8];
            }
            else if (mode == 5)
            {
                Span<int> dqep = stackalloc int[16];
                for (var p = 0; p < 3; p++)
                {
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & 31;
                    dqep[8 + p] = (qep[8 + p] - qep[p]) & 31;
                    dqep[12 + p] = (qep[12 + p] - qep[p]) & 31;
                }

                Span<uint> pqep = stackalloc uint[10];

                pqep[0] = (uint)dqep[0];
                pqep[1] = (uint)dqep[1];
                pqep[2] = (uint)dqep[2];
                pqep[4] = (uint)dqep[4] + (uint)(dqep[8 + 1] & 15) * 64;
                pqep[5] = (uint)dqep[5] + (uint)(dqep[12 + 1] & 15) * 64;
                pqep[6] = (uint)dqep[6] + (uint)(dqep[8 + 2] & 15) * 64;
                pqep[8] = (uint)dqep[8];
                pqep[9] = (uint)dqep[12];

                pqep[0] += BitAt(dqep[8 + 2], 4) << 9;
                pqep[1] += BitAt(dqep[8 + 1], 4) << 9;
                pqep[2] += BitAt(dqep[12 + 2], 4) << 9;

                pqep[4] += BitAt(dqep[12 + 1], 4) << 5;
                pqep[5] += BitAt(dqep[12 + 2], 0) << 5;
                pqep[6] += BitAt(dqep[12 + 2], 1) << 5;

                pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                pqep[9] += BitAt(dqep[12 + 2], 3) << 5;

                packed[0] = ModePrefix[5];

                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (pqep[9] << 6) + pqep[8];
            }
            else if (mode == 6 || mode == 7 || mode == 8)
            {
                Span<int> dqep = stackalloc int[16];
                for (var p = 0; p < 3; p++)
                {
                    var mask = p == mode - 6 ? 63 : 31;
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & mask;
                    dqep[8 + p] = (qep[8 + p] - qep[p]) & mask;
                    dqep[12 + p] = (qep[12 + p] - qep[p]) & mask;
                }

                Span<uint> pqep = stackalloc uint[10];

                pqep[0] = (uint)dqep[0];
                pqep[0] += BitAt(dqep[8 + 2], 4) << 9;

                pqep[1] = (uint)dqep[1];
                pqep[1] += BitAt(dqep[8 + 1], 4) << 9;

                pqep[2] = (uint)dqep[2];
                pqep[2] += BitAt(dqep[12 + 2], 4) << 9;

                pqep[4] = (uint)dqep[4] + (uint)(dqep[8 + 1] & 15) * 64;
                pqep[5] = (uint)dqep[5] + (uint)(dqep[12 + 1] & 15) * 64;
                pqep[6] = (uint)dqep[6] + (uint)(dqep[8 + 2] & 15) * 64;

                pqep[8] = (uint)dqep[8];
                pqep[9] = (uint)dqep[12];

                if (mode == 6)
            {
                    packed[0] = ModePrefix[6];

                    pqep[0] += BitAt(dqep[12 + 1], 4) << 8;
                    pqep[1] += BitAt(dqep[12 + 2], 2) << 8;
                    pqep[2] += BitAt(dqep[12 + 2], 3) << 8;
                    pqep[5] += BitAt(dqep[12 + 2], 0) << 5;
                    pqep[6] += BitAt(dqep[12 + 2], 1) << 5;
                }
            else if (mode == 7)
            {
                    packed[0] = ModePrefix[7];

                    pqep[0] += BitAt(dqep[12 + 2], 0) << 8;
                    pqep[1] += BitAt(dqep[8 + 1], 5) << 8;
                    pqep[2] += BitAt(dqep[12 + 1], 5) << 8;
                    pqep[4] += BitAt(dqep[12 + 1], 4) << 5;
                    pqep[6] += BitAt(dqep[12 + 2], 1) << 5;
                    pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                    pqep[9] += BitAt(dqep[12 + 2], 3) << 5;
                }
            else if (mode == 8)
            {
                    packed[0] = ModePrefix[8];

                    pqep[0] += BitAt(dqep[12 + 2], 1) << 8;
                    pqep[1] += BitAt(dqep[8 + 2], 5) << 8;
                    pqep[2] += BitAt(dqep[12 + 2], 5) << 8;
                    pqep[4] += BitAt(dqep[12 + 1], 4) << 5;
                    pqep[5] += BitAt(dqep[12 + 2], 0) << 5;
                    pqep[8] += BitAt(dqep[12 + 2], 2) << 5;
                    pqep[9] += BitAt(dqep[12 + 2], 3) << 5;
                }

                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (pqep[9] << 6) + pqep[8];
            }
            else if (mode == 9)
            {
                Span<uint> pqep = stackalloc uint[10];

                pqep[0] = (uint)qep[0];
                pqep[0] += BitAt(qep[12 + 1], 4) << 6;
                pqep[0] += BitAt(qep[12 + 2], 0) << 7;
                pqep[0] += BitAt(qep[12 + 2], 1) << 8;
                pqep[0] += BitAt(qep[8 + 2], 4) << 9;

                pqep[1] = (uint)qep[1];
                pqep[1] += BitAt(qep[8 + 1], 5) << 6;
                pqep[1] += BitAt(qep[8 + 2], 5) << 7;
                pqep[1] += BitAt(qep[12 + 2], 2) << 8;
                pqep[1] += BitAt(qep[8 + 1], 4) << 9;

                pqep[2] = (uint)qep[2];
                pqep[2] += BitAt(qep[12 + 1], 5) << 6;
                pqep[2] += BitAt(qep[12 + 2], 3) << 7;
                pqep[2] += BitAt(qep[12 + 2], 5) << 8;
                pqep[2] += BitAt(qep[12 + 2], 4) << 9;

                pqep[4] = (uint)qep[4] + (uint)(qep[8 + 1] & 15) * 64;
                pqep[5] = (uint)qep[5] + (uint)(qep[12 + 1] & 15) * 64;
                pqep[6] = (uint)qep[6] + (uint)(qep[8 + 2] & 15) * 64;

                packed[0] = ModePrefix[9];
                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
                packed[3] = (uint)(qep[12] << 6) + (uint)qep[8];
            }
            else if (mode == 10)
            {
                packed[0] = ModePrefix[10];
                packed[1] = (uint)(qep[2] << 20) + (uint)(qep[1] << 10) + (uint)qep[0];
                packed[2] = (uint)(qep[6] << 20) + (uint)(qep[5] << 10) + (uint)qep[4];
            }
            else if (mode == 11)
            {
                Span<int> dqep = stackalloc int[8];
                for (var p = 0; p < 3; p++)
                {
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & 511;
                }

                Span<uint> pqep = stackalloc uint[10];

                pqep[0] = (uint)(dqep[0] & 1023);
                pqep[1] = (uint)(dqep[1] & 1023);
                pqep[2] = (uint)(dqep[2] & 1023);

                pqep[4] = (uint)dqep[4] + (uint)(dqep[0] >> 10) * 512;
                pqep[5] = (uint)dqep[5] + (uint)(dqep[1] >> 10) * 512;
                pqep[6] = (uint)dqep[6] + (uint)(dqep[2] >> 10) * 512;

                packed[0] = ModePrefix[11];
                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
            }
            else if (mode == 12)
            {
                Span<int> dqep = stackalloc int[8];
                for (var p = 0; p < 3; p++)
                {
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & 255;
                }

                Span<uint> pqep = stackalloc uint[8];

                pqep[0] = (uint)(dqep[0] & 1023);
                pqep[1] = (uint)(dqep[1] & 1023);
                pqep[2] = (uint)(dqep[2] & 1023);

                pqep[4] = (uint)dqep[4] + ReverseBits((uint)(dqep[0] >> 10), 2) * 256;
                pqep[5] = (uint)dqep[5] + ReverseBits((uint)(dqep[1] >> 10), 2) * 256;
                pqep[6] = (uint)dqep[6] + ReverseBits((uint)(dqep[2] >> 10), 2) * 256;

                packed[0] = ModePrefix[12];
                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
            }
            else if (mode == 13)
            {
                Span<int> dqep = stackalloc int[8];
                for (var p = 0; p < 3; p++)
                {
                    dqep[p] = qep[p];
                    dqep[4 + p] = (qep[4 + p] - qep[p]) & 15;
                }

                Span<uint> pqep = stackalloc uint[8];

                pqep[0] = (uint)(dqep[0] & 1023);
                pqep[1] = (uint)(dqep[1] & 1023);
                pqep[2] = (uint)(dqep[2] & 1023);

                pqep[4] = (uint)dqep[4] + ReverseBits((uint)(dqep[0] >> 10), 6) * 16;
                pqep[5] = (uint)dqep[5] + ReverseBits((uint)(dqep[1] >> 10), 6) * 16;
                pqep[6] = (uint)dqep[6] + ReverseBits((uint)(dqep[2] >> 10), 6) * 16;

                packed[0] = ModePrefix[13];
                packed[1] = (pqep[2] << 20) + (pqep[1] << 10) + pqep[0];
                packed[2] = (pqep[6] << 20) + (pqep[5] << 10) + pqep[4];
            }
        }

        private void Setup()
        {
            for (var p = 0; p < 3; p++)
            {
                _rgbBounds[p] = 0xFFFF;
                _rgbBounds[3 + p] = 0f;
            }
            // min/max bounds
            for (var p = 0; p < 3; p++)
                for (var k = 0; k < 16; k++)
                {
                    var value = _block[p * 16 + k] / 31f * 64f;
                    _block[p * 16 + k] = value;
                    _rgbBounds[p] = Sat.Min(_rgbBounds[p], value);
                    _rgbBounds[3 + p] = Sat.Max(_rgbBounds[3 + p], value);
                }
            _maxSpan = 0f;
            _maxSpanIdx = 0;
            for (var p = 0; p < 3; p++)
            {
                var span = _rgbBounds[3 + p] - _rgbBounds[p];
                if (span > _maxSpan)
                {
                    _maxSpanIdx = p;
                    _maxSpan = span;
                }
            }
        }

        public void Compress()
        {
            Setup();
            // basic: not slow mode, not fast mode, a fast-skip threshold above 0
            TestMode(9, false, 0f);
            TestMode(6, false, 1f / 1.2f);
            TestMode(5, false, 1f / 1.2f);
            TestMode(0, false, 1f / 1.2f);
            TestMode(2, false, 1f);
            Enc2P();
            TestMode(1, true, 0f);
            TestMode(10, false, 0f);
            TestMode(11, false, 1f);
            TestMode(12, false, 1f);
            TestMode(13, false, 1f);
            Enc1P();
        }
    }
}
