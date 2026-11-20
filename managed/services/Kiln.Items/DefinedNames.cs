// SPDX-License-Identifier: MIT
namespace Kiln;

/// <summary>
/// The names of kiln.content's new items for <see cref="Items.DisplayName"/>: Kiln.Content's <c>ContentNames.NameOf(uint id,
/// string language)</c>, found by reflection (Kiln.Content compiles against Kiln.Items, not the other way round), as
/// Kiln.Api finds Kiln.Ui's <c>GameLanguage</c>. Absent (no Kiln.Content beside this assembly): no new items' names.
/// </summary>
internal static class DefinedNames
{
    /// <summary>Tests: what Kiln.Content would answer.</summary>
    internal static Func<uint, string, string?>? Override;

    private static Func<uint, string, string?>? _content;
    private static int _tried;

    public static string? Of(uint id, string language)
    {
        if (Override is { } o) return o(id, language);
        if (Interlocked.Exchange(ref _tried, 1) == 0)
        {
            try
            {
                var m = Type.GetType("Kiln.ContentNames, Kiln.Content")?.GetMethod("NameOf",
                    System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.Public | System.Reflection.BindingFlags.NonPublic,
                    [typeof(uint), typeof(string)]);
                if (m != null) Volatile.Write(ref _content, m.CreateDelegate<Func<uint, string, string?>>());
            }
            catch (Exception e) when (e is TypeLoadException or System.IO.FileNotFoundException or System.IO.FileLoadException or ArgumentException
                                           or MemberAccessException)
            {
                Backend.Current.Log(0, LogLevel.Debug, $"Kiln.Items: new items' names are not available: {e.Message}");
            }
        }
        return Volatile.Read(ref _content) is { } f ? f(id, language) : null;
    }

    internal static void ResetForTests()
    {
        Override = null;
        _content = null;
        _tried = 0;
    }
}
