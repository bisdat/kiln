// SPDX-License-Identifier: MIT
// Exact display of a scaled value (chapter 05b section 4.1): raw / 10^scale as a System.Decimal (28 digits: every
// long at every scale 0-6 is exact), never a double, in the game's culture with en-US as the fallback.
using System.Globalization;

namespace Kiln;

internal static class ModValueFormat
{
    private static readonly CultureInfo Fallback = CultureInfo.GetCultureInfo("en-US");

    internal static CultureInfo CultureOf(string? languageKey)
    {
        if (string.IsNullOrWhiteSpace(languageKey)) return Fallback;
        try
        {
            var c = CultureInfo.GetCultureInfo(languageKey);
            // invariant globalization (some server builds) gives cultures without data: keep en-US then
            return string.IsNullOrEmpty(c.Name) ? Fallback : c;
        }
        catch (CultureNotFoundException)
        {
            return Fallback;
        }
    }

    internal static decimal Exact(long raw, int scale)
    {
        if (scale is < 0 or > ModValueRules.MaxScale) throw new ArgumentOutOfRangeException(nameof(scale));
        var abs = raw < 0 ? (ulong)(-(Int128)raw) : (ulong)raw;  // long.MinValue's magnitude fits a ulong
        return new decimal(unchecked((int)(uint)abs), unchecked((int)(uint)(abs >> 32)), 0, raw < 0, (byte)scale);
    }

    /// <summary>One of N0-N6, F0-F6, "0", "0.0" to "0.000000"; anything else throws.</summary>
    internal static string Format(long raw, int scale, string format, string? languageKey)
    {
        if (!ModValueRules.IsNumberFormat(format)) throw new ArgumentException($"\"{format}\" is not a mod value number format", nameof(format));
        return Exact(raw, scale).ToString(format, CultureOf(languageKey));
    }
}
