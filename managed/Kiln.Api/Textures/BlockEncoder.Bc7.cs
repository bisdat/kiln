// SPDX-License-Identifier: MIT
// Ported from block_compression 0.6.0 (src/encode/bc7.rs, with BC7Settings::alpha_basic), MIT, Copyright (c) 2025
// Nils Hasenbanck, Copyright (c) 2016-2024 Intel Corporation (licenses/block_compression-LICENSE.txt).
namespace Kiln;

internal static partial class BlockEncoder
{
    /// <summary>One BC7 block (block_compression's BlockCompressorBC7 with the "alpha basic" settings Kiln's texture
    /// code uses: 4 channels, every mode group, mode 2 skipped).</summary>
    private sealed class Bc7Compressor
    {
        // BC7Settings::alpha_basic()
        private const int Channels = 4;
        private const int FastSkipThresholdMode1 = 12;
        private const int FastSkipThresholdMode3 = 8;
        private const int FastSkipThresholdMode7 = 8;
        private const int Mode45Channel0 = 0;
        private const int RefineIterationsChannel = 2;
        private const int RefineIterations = 2; // every mode

        private static readonly int[] PairsTable = [3, 2, 3, 2, 1, 1, 1, 2];

        private readonly float[] _block = new float[64];
        private float _bestErr = float.PositiveInfinity;
        private float _opaqueErr;

        public uint[] Data { get; } = new uint[5];

        private struct Mode45Parameters
        {
            public int[] Qep;
            public uint Qblock0, Qblock1;
            public int Aqep0, Aqep1;
            public uint Aqblock0, Aqblock1;
            public uint Rotation;
            public uint Swap;
        }

        public void LoadInterleavedRgba(ReadOnlySpan<byte> rgba, int xx, int yy, int stride) => LoadBlock(_block, rgba, xx, yy, stride);

        public void ComputeOpaqueErr()
        {
            var err = 0f;
            for (var k = 0; k < 16; k++) err += Sq(_block[48 + k] - 255f);
            _opaqueErr = err;
        }

        public void Compress()
        {
            EncMode02();
            EncMode13();
            EncMode7();
            EncMode45();
            EncMode6();
        }

        private static int UnpackToByte(int v, int bits)
        {
            var vv = v << (8 - bits);
            return vv + (vv >> bits);
        }

        private static void EpQuant0367(Span<int> qep, ReadOnlySpan<float> ep, int mode, int channels)
        {
            var bits = mode == 0 ? 4 : mode == 7 ? 5 : 7;
            var levels = 1 << bits;
            var levels2 = levels * 2 - 1;
            Span<int> qepB = stackalloc int[8];
            Span<float> epB = stackalloc float[8];
            for (var i = 0; i < 2; i++)
            {
                for (var b = 0; b < 2; b++)
                    for (var p = 0; p < 4; p++)
                    {
                        var v = Sat.ToInt((ep[i * 4 + p] / 255f * levels2 - b) / 2f + 0.5f) * 2 + b;
                        qepB[b * 4 + p] = Math.Clamp(v, b, levels2 - 1 + b);
                    }
                for (var j = 0; j < 8; j++) epB[j] = qepB[j];
                if (mode == 0)
                    for (var j = 0; j < 8; j++) epB[j] = UnpackToByte(qepB[j], 5);
                var err0 = 0f;
                var err1 = 0f;
                for (var p = 0; p < channels; p++)
                {
                    err0 += Sq(ep[i * 4 + p] - epB[p]);
                    err1 += Sq(ep[i * 4 + p] - epB[4 + p]);
                }
                for (var p = 0; p < 4; p++) qep[i * 4 + p] = err0 < err1 ? qepB[p] : qepB[4 + p];
            }
        }

        private static void EpQuant1(Span<int> qep, ReadOnlySpan<float> ep)
        {
            Span<int> qepB = stackalloc int[16];
            for (var b = 0; b < 2; b++)
                for (var i = 0; i < 8; i++)
                {
                    var v = Sat.ToInt((ep[i] / 255f * 127f - b) / 2f + 0.5f) * 2 + b;
                    qepB[b * 8 + i] = Math.Clamp(v, b, 126 + b);
                }
            // dequantised
            Span<float> epB = stackalloc float[16];
            for (var k = 0; k < 16; k++) epB[k] = UnpackToByte(qepB[k], 7);
            var err0 = 0f;
            var err1 = 0f;
            for (var j = 0; j < 2; j++)
                for (var p = 0; p < 3; p++)
                {
                    err0 += Sq(ep[j * 4 + p] - epB[j * 4 + p]);
                    err1 += Sq(ep[j * 4 + p] - epB[8 + j * 4 + p]);
                }
            for (var i = 0; i < 8; i++) qep[i] = err0 < err1 ? qepB[i] : qepB[8 + i];
        }

        private static void EpQuant245(Span<int> qep, ReadOnlySpan<float> ep, int mode)
        {
            var bits = mode == 5 ? 7 : 5;
            var levels = 1 << bits;
            for (var i = 0; i < 8; i++)
            {
                var v = Sat.ToInt(ep[i] / 255f * (levels - 1) + 0.5f);
                qep[i] = Math.Clamp(v, 0, levels - 1);
            }
        }

        private static void EpQuant(Span<int> qep, Span<float> ep, int mode, int channels)
        {
            var pairs = PairsTable[mode];
            if (mode is 0 or 3 or 6 or 7)
                for (var i = 0; i < pairs; i++) EpQuant0367(qep[(i * 8)..], ep[(i * 8)..], mode, channels);
            else if (mode == 1)
                for (var i = 0; i < pairs; i++) EpQuant1(qep[(i * 8)..], ep[(i * 8)..]);
            else if (mode is 2 or 4 or 5)
                for (var i = 0; i < pairs; i++) EpQuant245(qep[(i * 8)..], ep[(i * 8)..], mode);
        }

        private static void EpDequant(Span<float> ep, ReadOnlySpan<int> qep, int mode)
        {
            var pairs = PairsTable[mode];
            // modes 3 and 6 are 8-bit
            if (mode is 3 or 6)
                for (var i = 0; i < 8 * pairs; i++) ep[i] = qep[i];
            else if (mode is 1 or 5)
                for (var i = 0; i < 8 * pairs; i++) ep[i] = UnpackToByte(qep[i], 7);
            else if (mode is 0 or 2 or 4)
                for (var i = 0; i < 8 * pairs; i++) ep[i] = UnpackToByte(qep[i], 5);
            else if (mode == 7)
                for (var i = 0; i < 8 * pairs; i++) ep[i] = UnpackToByte(qep[i], 6);
        }

        private static void EpQuantDequant(Span<int> qep, Span<float> ep, int mode, int channels)
        {
            EpQuant(qep, ep, mode, channels);
            EpDequant(ep, qep, mode);
        }

        private static float OptChannel(Span<uint> qblock, Span<int> qep, ReadOnlySpan<float> channelBlock, int bits, int epbits)
        {
            Span<float> ep = [255f, 0f];
            for (var k = 0; k < 16; k++)
            {
                ep[0] = Sat.Min(ep[0], channelBlock[k]);
                ep[1] = Sat.Max(ep[1], channelBlock[k]);
            }
            ChannelQuantDequant(qep, ep, epbits);
            var err = ChannelOptQuant(qblock, channelBlock, bits, ep);
            for (var i = 0; i < RefineIterationsChannel; i++)
            {
                ChannelOptEndpoints(ep, channelBlock, bits, qblock[0], qblock[1]);
                ChannelQuantDequant(qep, ep, epbits);
                err = ChannelOptQuant(qblock, channelBlock, bits, ep);
            }
            return err;
        }

        private static void ChannelQuantDequant(Span<int> qep, Span<float> ep, int epbits)
        {
            var elevels = 1 << epbits;
            for (var i = 0; i < 2; i++)
            {
                var v = Sat.ToInt(ep[i] / 255f * (elevels - 1) + 0.5f);
                qep[i] = Math.Clamp(v, 0, elevels - 1);
                ep[i] = UnpackToByte(qep[i], epbits);
            }
        }

        private static float ChannelOptQuant(Span<uint> qblock, ReadOnlySpan<float> channelBlock, int bits, ReadOnlySpan<float> ep)
        {
            var levels = 1 << bits;
            qblock[0] = 0;
            qblock[1] = 0;
            var totalErr = 0f;
            for (var k = 0; k < 16; k++)
            {
                var proj = (channelBlock[k] - ep[0]) / (ep[1] - ep[0] + 0.001f);
                var q1 = Sat.ToInt(proj * levels + 0.5f);
                var q1Clamped = Math.Clamp(q1, 1, levels - 1);
                var err0 = 0f;
                var err1 = 0f;
                var w0 = GetUnquantValue(bits, q1Clamped - 1);
                var w1 = GetUnquantValue(bits, q1Clamped);
                float decV0 = ((64 - w0) * Sat.ToInt(ep[0]) + w0 * Sat.ToInt(ep[1]) + 32) / 64;
                float decV1 = ((64 - w1) * Sat.ToInt(ep[0]) + w1 * Sat.ToInt(ep[1]) + 32) / 64;
                err0 += Sq(decV0 - channelBlock[k]);
                err1 += Sq(decV1 - channelBlock[k]);
                var bestErr = err0 < err1 ? err0 : err1;
                var bestQ = err0 < err1 ? q1Clamped - 1 : q1Clamped;
                qblock[k / 8] |= (uint)bestQ << (4 * (k % 8));
                totalErr += bestErr;
            }
            return totalErr;
        }

        private static void ChannelOptEndpoints(Span<float> ep, ReadOnlySpan<float> channelBlock, int bits, uint qblock0, uint qblock1)
        {
            var levels = 1 << bits;
            var atb1 = 0f;
            var sumQ = 0f;
            var sumQq = 0f;
            var sum = 0f;
            for (var k1 = 0; k1 < 2; k1++)
            {
                var qbitsShifted = k1 == 0 ? qblock0 : qblock1;
                for (var k2 = 0; k2 < 8; k2++)
                {
                    var k = k1 * 8 + k2;
                    float q = qbitsShifted & 15;
                    qbitsShifted >>= 4;
                    var x = (levels - 1) - q;
                    sumQ += q;
                    sumQq += q * q;
                    sum += channelBlock[k];
                    atb1 += x * channelBlock[k];
                }
            }
            var atb2 = (levels - 1) * sum - atb1;
            var cxx = 16f * Sq(levels - 1) - 2f * (levels - 1) * sumQ + sumQq;
            var cyy = sumQq;
            var cxy = (levels - 1) * sumQ - sumQq;
            var scale = (levels - 1) / (cxx * cyy - cxy * cxy);
            ep[0] = (atb1 * cyy - atb2 * cxy) * scale;
            ep[1] = (atb2 * cxx - atb1 * cxy) * scale;
            ep[0] = Math.Clamp(ep[0], 0f, 255f);
            ep[1] = Math.Clamp(ep[1], 0f, 255f);
            if (MathF.Abs(cxx * cyy - cxy * cxy) < 0.001f)
            {
                ep[0] = sum / 16f;
                ep[1] = ep[0];
            }
        }

        private static void BlockSegment(Span<float> ep, float[] block, uint mask, int channels)
        {
            BlockSegmentCore(ep, block, mask, channels);
            for (var i = 0; i < 2; i++)
                for (var p = 0; p < channels; p++)
                    ep[4 * i + p] = Math.Clamp(ep[4 * i + p], 0f, 255f);
        }

        private void CodeMode01237(Span<int> qep, uint qblock0, uint qblock1, int partId, int mode)
        {
            var bits = mode is 0 or 1 ? 3 : 2;
            var pairs = mode is 0 or 2 ? 3 : 2;
            var channels = mode == 7 ? 4 : 3;
            var flips = Bc7CodeApplySwapMode01237(qep, qblock0, qblock1, mode, partId);
            Array.Clear(Data);
            uint pos = 0;
            // mode
            PutBits(Data, ref pos, (uint)mode + 1, 1u << mode);
            // partition
            if (mode == 0) PutBits(Data, ref pos, 4, (uint)(partId & 15));
            else PutBits(Data, ref pos, 6, (uint)(partId & 63));
            // endpoints
            for (var p = 0; p < channels; p++)
                for (var j = 0; j < pairs * 2; j++)
                {
                    var q = (uint)qep[j * 4 + p];
                    if (mode == 0) PutBits(Data, ref pos, 4, q >> 1);
                    else if (mode == 1) PutBits(Data, ref pos, 6, q >> 1);
                    else if (mode == 2) PutBits(Data, ref pos, 5, q);
                    else if (mode == 3) PutBits(Data, ref pos, 7, q >> 1);
                    else if (mode == 7) PutBits(Data, ref pos, 5, q >> 1);
                }
            // P-bits
            if (mode == 1)
                for (var j = 0; j < 2; j++) PutBits(Data, ref pos, 1, (uint)qep[j * 8] & 1);
            if (mode is 0 or 3 or 7)
                for (var j = 0; j < pairs * 2; j++) PutBits(Data, ref pos, 1, (uint)qep[j * 4] & 1);
            // quantised values
            Bc7CodeQblock(Data, ref pos, qblock0, qblock1, bits, flips);
            Bc7CodeAdjustSkipMode01237(Data, mode, partId);
        }

        private void CodeMode45(in Mode45Parameters parameters, int mode)
        {
            Span<int> qep = stackalloc int[8];
            parameters.Qep.CopyTo(qep);
            Span<uint> qblock = [parameters.Qblock0, parameters.Qblock1];
            Span<int> aqep = [parameters.Aqep0, parameters.Aqep1];
            Span<uint> aqblock = [parameters.Aqblock0, parameters.Aqblock1];
            var rotation = parameters.Rotation;
            var swap = parameters.Swap;

            const int bits = 2;
            var abits = mode == 4 ? 3 : 2;
            var epbits = mode == 4 ? 5u : 7u;
            var aepbits = mode == 4 ? 6u : 8u;

            if (swap == 0)
            {
                Bc7CodeApplySwapMode456(qep, 4, qblock, bits);
                Bc7CodeApplySwapMode456(aqep, 1, aqblock, abits);
            }
            else
            {
                (qblock[0], aqblock[0]) = (aqblock[0], qblock[0]);
                (qblock[1], aqblock[1]) = (aqblock[1], qblock[1]);
                Bc7CodeApplySwapMode456(aqep, 1, qblock, bits);
                Bc7CodeApplySwapMode456(qep, 4, aqblock, abits);
            }

            Array.Clear(Data);
            uint pos = 0;
            // mode
            PutBits(Data, ref pos, (uint)mode + 1, 1u << mode);
            // rotation
            PutBits(Data, ref pos, 2, (rotation + 1) & 3);
            if (mode == 4) PutBits(Data, ref pos, 1, swap);
            // endpoints
            for (var p = 0; p < 3; p++)
            {
                PutBits(Data, ref pos, epbits, (uint)qep[p]);
                PutBits(Data, ref pos, epbits, (uint)qep[4 + p]);
            }
            // alpha endpoints
            PutBits(Data, ref pos, aepbits, (uint)aqep[0]);
            PutBits(Data, ref pos, aepbits, (uint)aqep[1]);
            // quantised values
            Bc7CodeQblock(Data, ref pos, qblock[0], qblock[1], bits, 0);
            Bc7CodeQblock(Data, ref pos, aqblock[0], aqblock[1], abits, 0);
        }

        private void CodeMode6(Span<int> qep, Span<uint> qblock)
        {
            Bc7CodeApplySwapMode456(qep, 4, qblock, 4);
            Array.Clear(Data);
            uint pos = 0;
            // mode 6
            PutBits(Data, ref pos, 7, 64);
            // endpoints
            for (var p = 0; p < 4; p++)
            {
                PutBits(Data, ref pos, 7, (uint)qep[p] >> 1);
                PutBits(Data, ref pos, 7, (uint)qep[4 + p] >> 1);
            }
            // P-bits
            PutBits(Data, ref pos, 1, (uint)qep[0] & 1);
            PutBits(Data, ref pos, 1, (uint)qep[4] & 1);
            // quantised values
            Bc7CodeQblock(Data, ref pos, qblock[0], qblock[1], 4, 0);
        }

        private float EncMode01237PartFast(Span<int> qep, Span<uint> qblock, int partId, int mode)
        {
            var pattern = GetPattern(partId);
            var bits = mode is 0 or 1 ? 3 : 2;
            var pairs = mode is 0 or 2 ? 3 : 2;
            var channels = mode == 7 ? 4 : 3;
            Span<float> ep = stackalloc float[24];
            ep.Clear();
            for (var j = 0; j < pairs; j++)
                BlockSegment(ep[(j * 8)..], _block, GetPatternMask(partId, j), channels);
            EpQuantDequant(qep, ep, mode, channels);
            return BlockQuant(qblock, _block, bits, ep, pattern, channels);
        }

        private void EncMode01237(int mode, ReadOnlySpan<int> partList, int partCount)
        {
            if (partCount == 0) return;
            var bits = mode is 0 or 1 ? 3 : 2;
            var pairs = mode is 0 or 2 ? 3 : 2;
            var channels = mode == 7 ? 4 : 3;

            Span<int> bestQep = stackalloc int[24];
            Span<uint> bestQblock = stackalloc uint[2];
            bestQep.Clear();
            bestQblock.Clear();
            var bestPartId = -1;
            var bestErr = float.PositiveInfinity;
            Span<int> qep = stackalloc int[24];
            Span<uint> qblock = stackalloc uint[2];
            foreach (var part in partList[..partCount])
            {
                var partId = part & 63;
                partId = pairs == 3 ? partId + 64 : partId;
                qep.Clear();
                qblock.Clear();
                var err = EncMode01237PartFast(qep, qblock, partId, mode);
                if (err < bestErr)
                {
                    qep[..(8 * pairs)].CopyTo(bestQep);
                    qblock.CopyTo(bestQblock);
                    bestPartId = partId;
                    bestErr = err;
                }
            }

            Span<float> ep = stackalloc float[24];
            for (var i = 0; i < RefineIterations; i++)
            {
                ep.Clear();
                for (var j = 0; j < pairs; j++)
                    OptEndpoints(ep[(j * 8)..], _block, bits, bestQblock[0], bestQblock[1], GetPatternMask(bestPartId, j), channels);
                qep.Clear();
                qblock.Clear();
                EpQuantDequant(qep, ep, mode, channels);
                var err = BlockQuant(qblock, _block, bits, ep, GetPattern(bestPartId), channels);
                if (err < bestErr)
                {
                    qep[..(8 * pairs)].CopyTo(bestQep);
                    qblock.CopyTo(bestQblock);
                    bestErr = err;
                }
            }

            if (mode != 7) bestErr += _opaqueErr;
            if (bestErr < _bestErr)
            {
                _bestErr = bestErr;
                CodeMode01237(bestQep, bestQblock[0], bestQblock[1], bestPartId, mode);
            }
        }

        private void EncMode02()
        {
            Span<int> partList = stackalloc int[64];
            for (var part = 0; part < 64; part++) partList[part] = part;
            EncMode01237(0, partList, 16);
            // mode 2 is skipped (skip_mode2)
        }

        private void EncMode13()
        {
            Span<float> fullStats = stackalloc float[15];
            fullStats.Clear();
            ComputeStatsMasked(fullStats, _block, 0xFFFFFFFF, 3);
            Span<int> partList = stackalloc int[64];
            for (var part = 0; part < 64; part++)
            {
                var bound12 = BlockPcaBoundSplit(_block, GetPatternMask(part, 0), fullStats, 3);
                partList[part] = part + Sat.ToInt(bound12) * 64;
            }
            PartialSortList(partList, 64, Math.Max(FastSkipThresholdMode1, FastSkipThresholdMode3));
            EncMode01237(1, partList, FastSkipThresholdMode1);
            EncMode01237(3, partList, FastSkipThresholdMode3);
        }

        private void EncMode45Candidate(ref Mode45Parameters best, ref float bestErr, int mode, uint rotation, uint swap)
        {
            var bits = 2;
            var abits = 2;
            var aepbits = 8;
            if (mode == 4)
            {
                abits = 3;
                aepbits = 6;
            }
            // mode 4's index selection
            if (swap == 1)
            {
                bits = 3;
                abits = 2;
            }

            var candidate = new float[64];
            for (var k = 0; k < 16; k++)
            {
                for (var p = 0; p < 3; p++) candidate[k + p * 16] = _block[k + p * 16];
                // the rotated channel takes alpha's place
                if (rotation < 3) candidate[k + (int)rotation * 16] = _block[k + 3 * 16];
            }

            Span<float> ep = stackalloc float[8];
            ep.Clear();
            BlockSegment(ep, candidate, 0xFFFFFFFF, 3);
            Span<int> qep = stackalloc int[8];
            qep.Clear();
            EpQuantDequant(qep, ep, mode, 3);
            Span<uint> qblock = stackalloc uint[2];
            qblock.Clear();
            var err = BlockQuant(qblock, candidate, bits, ep, 0, 3);
            for (var i = 0; i < RefineIterations; i++)
            {
                OptEndpoints(ep, candidate, bits, qblock[0], qblock[1], 0xFFFFFFFF, 3);
                EpQuantDequant(qep, ep, mode, 3);
                err = BlockQuant(qblock, candidate, bits, ep, 0, 3);
            }

            Span<float> channelData = stackalloc float[16];
            for (var k = 0; k < 16; k++) channelData[k] = _block[k + (int)rotation * 16];

            // the selected channel
            Span<int> aqep = stackalloc int[2];
            Span<uint> aqblock = stackalloc uint[2];
            aqep.Clear();
            aqblock.Clear();
            err += OptChannel(aqblock, aqep, channelData, abits, aepbits);

            if (err < bestErr)
            {
                best.Qep = qep.ToArray();
                best.Qblock0 = qblock[0];
                best.Qblock1 = qblock[1];
                best.Aqblock0 = aqblock[0];
                best.Aqblock1 = aqblock[1];
                best.Aqep0 = aqep[0];
                best.Aqep1 = aqep[1];
                best.Rotation = rotation;
                best.Swap = swap;
                bestErr = err;
            }
        }

        private void EncMode45()
        {
            var best = new Mode45Parameters { Qep = new int[8] };
            var bestErr = _bestErr;
            for (var p = (uint)Mode45Channel0; p < Channels; p++)
            {
                EncMode45Candidate(ref best, ref bestErr, 4, p, 0);
                EncMode45Candidate(ref best, ref bestErr, 4, p, 1);
            }
            // mode 4
            if (bestErr < _bestErr)
            {
                _bestErr = bestErr;
                CodeMode45(best, 4);
            }
            for (var p = (uint)Mode45Channel0; p < Channels; p++)
                EncMode45Candidate(ref best, ref bestErr, 5, p, 0);
            // mode 5
            if (bestErr < _bestErr)
            {
                _bestErr = bestErr;
                CodeMode45(best, 5);
            }
        }

        private void EncMode6()
        {
            const int mode = 6;
            const int bits = 4;
            Span<float> ep = stackalloc float[8];
            ep.Clear();
            BlockSegment(ep, _block, 0xFFFFFFFF, Channels);
            Span<int> qep = stackalloc int[8];
            qep.Clear();
            EpQuantDequant(qep, ep, mode, Channels);
            Span<uint> qblock = stackalloc uint[2];
            qblock.Clear();
            var err = BlockQuant(qblock, _block, bits, ep, 0, Channels);
            for (var i = 0; i < RefineIterations; i++)
            {
                OptEndpoints(ep, _block, bits, qblock[0], qblock[1], 0xFFFFFFFF, Channels);
                EpQuantDequant(qep, ep, mode, Channels);
                err = BlockQuant(qblock, _block, bits, ep, 0, Channels);
            }
            if (err < _bestErr)
            {
                _bestErr = err;
                CodeMode6(qep, qblock);
            }
        }

        private void EncMode7()
        {
            Span<float> fullStats = stackalloc float[15];
            fullStats.Clear();
            ComputeStatsMasked(fullStats, _block, 0xFFFFFFFF, Channels);
            Span<int> partList = stackalloc int[64];
            for (var part = 0; part < 64; part++)
            {
                var bound12 = BlockPcaBoundSplit(_block, GetPatternMask(part, 0), fullStats, Channels);
                partList[part] = part + Sat.ToInt(bound12) * 64;
            }
            PartialSortList(partList, 64, FastSkipThresholdMode7);
            EncMode01237(7, partList, FastSkipThresholdMode7);
        }
    }
}
