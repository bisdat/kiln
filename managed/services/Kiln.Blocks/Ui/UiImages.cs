// SPDX-License-Identifier: MIT
// Pictures in UI blocks (dossier 09 §4.1 ImageRow, §2): a PNG of the mod's folder, checked from its header before anything
// is decoded (at most 8 MiB, 2048 x 2048), then read and handed to Kiln.Ui's image loader once, on the tick thread when the
// window is registered (never from a frame). A mod gets at most 8 pictures and 32 MiB of decoded pixels; the pictures go
// with the mod's UI lifetime (Ui.LoadImage's own retirement waits for the GPU).
using System.Buffers.Binary;

namespace Kiln;

internal static class UiImages
{
    internal const long MaxFileBytes = 8L * 1024 * 1024;
    internal const int MaxSide = 2048, MaxPerMod = 8;
    internal const long MaxModPixelsBytes = 32L * 1024 * 1024;

    private static readonly byte[] Signature = [0x89, (byte)'P', (byte)'N', (byte)'G', 0x0D, 0x0A, 0x1A, 0x0A];
    private static readonly object Gate = new();
    private static readonly Dictionary<string, (int Count, long Bytes)> Used = new(StringComparer.Ordinal);

    /// <summary>The picture's size from its header, or a problem (not a PNG, too big). Reads 33 bytes.</summary>
    internal static string? Preflight(string file) => Header(file, out _, out _);

    internal static string? Header(string file, out int width, out int height)
    {
        width = height = 0;
        try
        {
            var info = new FileInfo(file);
            if (info.Length > MaxFileBytes) return $"{info.Length} bytes is more than {MaxFileBytes / (1024 * 1024)} MiB";
            Span<byte> head = stackalloc byte[33];
            using (var s = info.OpenRead())
                if (s.ReadAtLeast(head, head.Length, throwOnEndOfStream: false) < head.Length) return "not a PNG (too short)";
            return HeaderOf(head, out width, out height);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return $"cannot read it ({e.Message})"; }
    }

    /// <summary>A PNG's signature and IHDR: width and height within 1..2048.</summary>
    internal static string? HeaderOf(ReadOnlySpan<byte> head, out int width, out int height)
    {
        width = height = 0;
        if (head.Length < 33 || !head[..8].SequenceEqual(Signature)) return "not a PNG";
        if (BinaryPrimitives.ReadUInt32BigEndian(head[8..]) != 13 || !head.Slice(12, 4).SequenceEqual("IHDR"u8)) return "not a PNG (no header chunk first)";
        var w = BinaryPrimitives.ReadUInt32BigEndian(head[16..]);
        var h = BinaryPrimitives.ReadUInt32BigEndian(head[20..]);
        if (w == 0 || h == 0 || w > MaxSide || h > MaxSide) return $"{w} x {h} pixels: a picture is 1..{MaxSide} pixels each way";
        width = (int)w;
        height = (int)h;
        return null;
    }

    /// <summary>Reads, checks and loads a picture for <paramref name="mod"/> within its budget; the release gives the
    /// budget back. Tick thread. A refusal is a reason (shown as text in place of the picture).</summary>
    internal static (UiImage? Image, string? Problem, Action Release) Load(string mod, string file)
    {
        if (Header(file, out var w, out var h) is { } bad) return (null, bad, static () => { });
        var pixels = (long)w * h * 4;
        lock (Gate)
        {
            var used = Used.GetValueOrDefault(mod);
            if (used.Count + 1 > MaxPerMod) return (null, $"mod {mod} already shows {MaxPerMod} pictures", static () => { });
            if (used.Bytes + pixels > MaxModPixelsBytes) return (null, $"mod {mod}'s pictures would take more than {MaxModPixelsBytes / (1024 * 1024)} MiB", static () => { });
            Used[mod] = (used.Count + 1, used.Bytes + pixels);
        }
        var released = 0;
        void Release()
        {
            if (Interlocked.Exchange(ref released, 1) != 0) return;
            lock (Gate)
            {
                var u = Used.GetValueOrDefault(mod);
                if (u.Count <= 1) Used.Remove(mod);
                else Used[mod] = (u.Count - 1, u.Bytes - pixels);
            }
        }
        try
        {
            var bytes = File.ReadAllBytes(file);
            if (bytes.Length > MaxFileBytes || HeaderOf(bytes, out var w2, out var h2) != null || w2 != w || h2 != h)
            {
                Release();
                return (null, "the file changed since it was checked", static () => { });
            }
            var image = UiRuntime.Backend.LoadImage(bytes);
            if (image == null)
            {
                Release();
                return (null, "the overlay could not load it", static () => { });
            }
            return (image, null, () => { image.Dispose(); Release(); });
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException or ArgumentException)
        {
            Release();
            return (null, e.Message, static () => { });
        }
    }

    internal static (int Count, long Bytes) UsedBy(string mod)
    {
        lock (Gate) return Used.GetValueOrDefault(mod);
    }

    internal static void ResetForTests()
    {
        lock (Gate) Used.Clear();
    }
}
