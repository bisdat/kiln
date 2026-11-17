// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>How long to wait before trying something again that failed <c>failures</c> times in a row: <c>first</c>, then
/// doubling, at most <c>most</c> (milliseconds). The services' background reads use it (the game's names, kiln.content's
/// listing, the item icons) so a thing that keeps failing is not asked for every few seconds forever.</summary>
internal static class RetryDelay
{
    public static long Of(int failures, long first, long most)
    {
        if (failures <= 0) return 0;
        var shift = Math.Min(failures - 1, 30);
        var d = first << shift;
        return d <= 0 || d > most ? most : d;
    }
}
