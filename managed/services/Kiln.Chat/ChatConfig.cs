// SPDX-License-Identifier: MIT
// kiln/config/chat.json: Kiln.Chat's server settings (docs/kiln/services/chat.md, "Server settings").
using System.Text.Json;

namespace Kiln;

/// <summary><c>chat.json</c> in the config folder (<c>kiln/config</c>, or the active profile's configDir). Unknown keys
/// are ignored; a missing file, or a key it does not set, leaves the code's value. Read when first needed and again
/// whenever the file changes, so an edit applies without a restart.</summary>
internal static class ChatConfig
{
    private static readonly object Lock = new();
    private static string? _path;
    private static DateTime _stamp;
    private static UnknownCommandPolicy? _unknownCommands;

    public static string? FilePath
    {
        get
        {
            try
            {
                return Path.Combine(KilnPaths.Config, "chat.json");
            }
            catch
            {
                return null;
            }
        }
    }

    /// <summary>The file's <c>"unknownCommands"</c>: <c>"reply"</c> or <c>"passThrough"</c>; null when it doesn't say.</summary>
    public static UnknownCommandPolicy? UnknownCommands
    {
        get
        {
            lock (Lock)
            {
                Refresh();
                return _unknownCommands;
            }
        }
    }

    private static void Refresh()
    {
        var path = FilePath;
        var stamp = default(DateTime);
        try
        {
            if (path is not null && File.Exists(path)) stamp = File.GetLastWriteTimeUtc(path);
        }
        catch
        {
            // unreadable: as if missing
        }
        if (path == _path && stamp == _stamp) return;
        _path = path;
        _stamp = stamp;
        _unknownCommands = null;
        if (stamp == default) return;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path!), new JsonDocumentOptions
            {
                CommentHandling = JsonCommentHandling.Skip,
                AllowTrailingCommas = true,
            });
            _unknownCommands = Parse(doc.RootElement, out var error);
            if (error is not null) Backend.Current.Log(0, LogLevel.Warn, $"kiln.chat: {path}: {error}");
            else if (_unknownCommands is { } u)
                Backend.Current.Log(0, LogLevel.Info, $"kiln.chat: {path}: unknown commands {(u == UnknownCommandPolicy.Reply ? "get Kiln's reply" : "pass through")}");
        }
        catch (Exception e)
        {
            Backend.Current.Log(0, LogLevel.Warn, $"kiln.chat: {path} unreadable ({e.Message}); defaults");
        }
    }

    /// <summary>The <c>unknownCommands</c> key of a chat.json root; null (and no error) when absent.</summary>
    internal static UnknownCommandPolicy? Parse(JsonElement root, out string? error)
    {
        error = null;
        if (root.ValueKind != JsonValueKind.Object) return null;
        foreach (var p in root.EnumerateObject())
        {
            if (p.Name != "unknownCommands") continue;
            var v = p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString()!.Replace("-", "").Replace("_", "").ToLowerInvariant() : null;
            switch (v)
            {
                case "reply": return UnknownCommandPolicy.Reply;
                case "passthrough": return UnknownCommandPolicy.PassThrough;
                default:
                    error = $"unknownCommands must be \"reply\" or \"passThrough\", not {p.Value.GetRawText()}; ignored";
                    return null;
            }
        }
        return null;
    }

    /// <summary>Test hook: read the file again at the next use.</summary>
    internal static void ResetForTests()
    {
        lock (Lock)
        {
            _path = null;
            _stamp = default;
            _unknownCommands = null;
        }
    }
}
