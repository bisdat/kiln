// SPDX-License-Identifier: MIT
// Issue 44, phase 1: client-side chat history state and private storage.
// This is deliberately independent of the game's input/edit hooks. The hooks will call Submit/Older/Newer/Edited;
// adding a history store alone must not change the game's input or send path.
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Kiln;

internal sealed class ChatHistory
{
    private const int FormatVersion = 1;
    private const int MaxLineBytes = 1024;
    private readonly record struct Entry(string Text, string? ServerHash, bool Persisted);

    // The server may identify further sensitive commands once the CommandIndex protocol exists. Until then, avoid
    // saving (or even recalling) a command with a commonly credential-bearing word, anywhere in its arguments.
    private static readonly Regex SensitiveWords = new(
        @"\b(login|register|password|pass|token|key|secret)\b",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Compiled);

    private readonly List<Entry> _entries = [];
    private readonly string _path;
    private readonly int _limit;
    private readonly bool _persistChat;
    private int _cursor = -1;  // -1: not browsing; otherwise an index into _entries
    private string _draft = "";

    internal ChatHistory(string path, int limit = 200, bool persistChat = false)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(path);
        _path = path;
        _limit = Math.Clamp(limit, 1, 2000);
        _persistChat = persistChat;
        Load();
    }

    internal int Count => _entries.Count;

    // An immutable snapshot for client UI and unit tests.
    internal IReadOnlyList<string> Lines => _entries.Select(e => e.Text).ToArray();

    /// <summary>
    /// Record text the player actually submitted. Protocol messages and programmatic output must never call this.
    /// Commands are persisted by default; ordinary chat is only kept in memory unless explicitly opted in.
    /// serverIdentity is hashed, never written verbatim (null in single player / when unknown).
    /// </summary>
    internal void Submit(string? text, string? serverIdentity = null, bool serverSensitive = false)
    {
        if (!Valid(text) || serverSensitive || SensitiveWords.IsMatch(text!)) return;
        bool command = text!.StartsWith("/", StringComparison.Ordinal);
        string? serverHash = string.IsNullOrEmpty(serverIdentity) ? null : HashServer(serverIdentity);

        // Consecutive duplicates are one entry, as in a shell. A repeated command still ends active browsing.
        if (_entries.Count > 0 && _entries[^1].Text == text && _entries[^1].ServerHash == serverHash)
        {
            Edited();
            return;
        }

        _entries.Add(new Entry(text, serverHash, command || _persistChat));
        if (_entries.Count > _limit) _entries.RemoveRange(0, _entries.Count - _limit);
        Edited();
        Save();
    }

    /// <summary>The oldest reachable line moves backwards; first Up saves the live, unsent draft.</summary>
    internal string Older(string currentText)
    {
        if (_entries.Count == 0) return currentText;
        if (_cursor < 0)
        {
            _draft = currentText;
            _cursor = _entries.Count - 1;
        }
        else if (_cursor > 0) --_cursor;
        return _entries[_cursor].Text;
    }

    /// <summary>Down moves forward; beyond the newest entry it restores the draft originally typed.</summary>
    internal string Newer()
    {
        if (_cursor < 0) return _draft;
        if (++_cursor >= _entries.Count)
        {
            _cursor = -1;
            return _draft;
        }
        return _entries[_cursor].Text;
    }

    /// <summary>Call when the player changes the input manually, submits, or closes the input.</summary>
    internal void Edited()
    {
        _cursor = -1;
        _draft = "";
    }

    /// <summary>A search primitive for the later Ctrl+R UI; excludes text that was refused by Submit/Load.</summary>
    internal string? SearchBack(string fragment, int skip = 0)
    {
        if (skip < 0) throw new ArgumentOutOfRangeException(nameof(skip));
        if (string.IsNullOrEmpty(fragment)) return null;
        for (int i = _entries.Count - 1; i >= 0; --i)
            if (_entries[i].Text.Contains(fragment, StringComparison.OrdinalIgnoreCase) && skip-- == 0)
                return _entries[i].Text;
        return null;
    }

    /// <summary>Forget the current session and remove the persisted entries from disk.</summary>
    internal void Clear()
    {
        _entries.Clear();
        Edited();
        Save();
    }

    private static bool Valid(string? text) =>
        !string.IsNullOrWhiteSpace(text) &&
        !text.Contains('\0') && !text.Contains('\n') && !text.Contains('\r') &&
        Encoding.UTF8.GetByteCount(text) <= MaxLineBytes;

    private static string HashServer(string identity) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(identity))).ToLowerInvariant();

    private void Load()
    {
        try
        {
            if (!File.Exists(_path)) return;
            using var doc = JsonDocument.Parse(File.ReadAllText(_path));
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("version", out var v) || v.GetInt32() != FormatVersion ||
                !root.TryGetProperty("entries", out var entries) || entries.ValueKind != JsonValueKind.Array)
                return;
            foreach (var row in entries.EnumerateArray())
            {
                if (row.ValueKind != JsonValueKind.Object ||
                    !row.TryGetProperty("text", out var t) || t.ValueKind != JsonValueKind.String) continue;
                var text = t.GetString();
                if (!Valid(text) || SensitiveWords.IsMatch(text!)) continue;
                bool command = text!.StartsWith("/", StringComparison.Ordinal);
                if (!command && !_persistChat) continue;
                string? hash = null;
                if (row.TryGetProperty("server", out var s) && s.ValueKind == JsonValueKind.String)
                {
                    var h = s.GetString();
                    if (h is { Length: 64 } && h.All(Uri.IsHexDigit)) hash = h;
                }
                _entries.Add(new Entry(text, hash, true));
                if (_entries.Count > _limit) _entries.RemoveAt(0);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or InvalidOperationException or FormatException)
        {
            // History is optional. A corrupt, missing or inaccessible file must never prevent the chat from working.
            _entries.Clear();
        }
    }

    private void Save()
    {
        string? tmp = null;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_path) ?? ".");
            tmp = _path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            var persisted = _entries.Where(e => e.Persisted)
                .Select(e => new { text = e.Text, server = e.ServerHash }).ToArray();
            var json = JsonSerializer.Serialize(new { version = FormatVersion, entries = persisted });
            File.WriteAllText(tmp, json);
            File.Move(tmp, _path, overwrite: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            // Keep in-memory history even if the directory or drive cannot be written.
        }
        finally
        {
            if (tmp != null)
            {
                try { File.Delete(tmp); }
                catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { }
            }
        }
    }
}
