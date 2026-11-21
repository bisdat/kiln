// SPDX-License-Identifier: MIT
//
// The UI bridge's files between Kiln.Ui and kiln-sfi's Lua runtime, the interpreter's stand-in for ShroudForge 1.7.9's
// ui/actions and news/events folders (schema/sfi/sfi-ui-envelope.schema.json). This file owns the files only: Kiln.Ui
// writes actions and its launch receipt, reads notices, and prunes ended launches' folders. What a click means, which
// owner is active and at which epoch, and the mod page are the callers'; the runtime's side (draining the
// actions, publishing notices with its own durable write, so a failed publish fails the same Lua call) is kiln-sfi's.
// Both sides share schema/fixtures/sfi/ui/: wire/ (the exact bytes), bounds.json, drain/cases.json.
//
//   <game>/kiln/sfi/ui/<launchId>/launch.json          this launch's receipt (kind launch), written once
//   <game>/kiln/sfi/ui/<launchId>/staging/<random>.tmp  a record being written: never inside actions/
//   <game>/kiln/sfi/ui/<launchId>/actions/<requestId>.json   renamed in whole; the runtime reads every entry here
//   <configDir>/sfi/notifications/<sha256(key)>.json   notices, kept across launches (written by the runtime only)
//
// Every call does file IO: call it from a worker, never from the render thread.
using System.Diagnostics;
using System.Diagnostics.CodeAnalysis;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Kiln;

/// <summary>The launch whose folder the bridge writes: sfi.json launch.launchId, planHash, pid and processStartMs.</summary>
internal readonly record struct SfiLaunch(string LaunchId, string PlanHash, int Pid, long ProcessStartMs);

/// <summary>One action request as it goes into actions/ (schema $defs/action).</summary>
internal sealed record SfiActionRecord(string RequestId, long Sequence, string LaunchId, string PlanHash, long OwnerEpoch,
    string ModId, string Action, string? Value, long CreatedMs);

internal enum SfiQueueStatus
{
    /// <summary>The whole record is in actions/: the runtime will see it.</summary>
    Queued,
    /// <summary>An identifier, the launch, the epoch or the value is not what the contract allows; nothing was written.</summary>
    Invalid,
    /// <summary>The owner already has bounds.json pendingActionsPerOwner requests waiting; nothing was written.</summary>
    Full,
    /// <summary>The record would be over bounds.json recordBytes; nothing was written.</summary>
    TooLarge,
    /// <summary>The folder cannot be used (a link in its path, a write error); nothing reached actions/.</summary>
    Unavailable,
}

/// <summary>What became of a request. Only <see cref="SfiQueueStatus.Queued"/> may be shown as sent.</summary>
internal readonly record struct SfiQueueResult(SfiQueueStatus Status, string? RequestId, string Reason)
{
    public bool Sent => Status == SfiQueueStatus.Queued;
}

/// <summary>One notice as ShroudForge 1.7.9's notifications.publish stores it, with the launch that published it
/// (provenance only: a notice of an earlier launch stays visible). <see cref="ActionUrl"/> is https:// or null and is
/// opened only on an explicit click.</summary>
internal sealed record SfiNotice(string Key, string Id, string ModId, string Title, string Message, string Level,
    string? ActionUrl, long UpdatedAt, string LaunchId, long OwnerEpoch);

internal enum SfiLiveness { Alive, Ended, Unknown }

/// <summary>One entry of <c>&lt;game&gt;/kiln/sfi/ui</c> the cleanup looked at: removed, or kept and why.</summary>
internal readonly record struct SfiCleanupOutcome(string Name, bool Removed, string Why);

/// <summary>A test's stand-in for the process being killed at a step of a write (see <see cref="SfiBridgeFiles.KillPoint"/>).</summary>
internal sealed class SfiKillPointException(string step) : Exception("killed at " + step);

internal static class SfiBridgeFiles
{
    // schema/fixtures/sfi/ui/bounds.json; a test holds them equal
    public const int RecordBytes = 2 * 1024 * 1024;
    public const int PendingActionsPerOwner = 1024;
    public const int NoticeKeysPerProfile = 4096;
    public const int EntriesPerTurn = 32;
    public const int MaxReceiptBytes = 64 * 1024;
    public const int KeepEndedLaunchDays = 7;

    internal static Action<LogLevel, string> Log = (level, text) => UiEngine.Log(level, "sfi bridge: " + text);
    /// <summary>Whether the process of a receipt still runs (tests replace it).</summary>
    internal static Func<int, long, SfiLiveness> Liveness = ProcessLiveness;
    internal static Func<long> NowMs = () => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
    /// <summary>Tests: called at each step of a write (<c>action:half-written</c>, <c>receipt:before-rename</c>, ...); a
    /// <see cref="SfiKillPointException"/> thrown there leaves the files as a killed process would.</summary>
    internal static Action<string>? KillPoint;

    // one writer at a time in this process: the owner's count and the rename are one step
    private static readonly object Gate = new();
    private static long _sequence = -1;

    public static string UiRoot(string gameDir) => Path.Combine(gameDir, "kiln", "sfi", "ui");
    public static string LaunchDir(string gameDir, string launchId) => Path.Combine(UiRoot(gameDir), launchId);
    public static string ActionsDir(string gameDir, string launchId) => Path.Combine(LaunchDir(gameDir, launchId), "actions");
    public static string StagingDir(string gameDir, string launchId) => Path.Combine(LaunchDir(gameDir, launchId), "staging");
    public static string ReceiptPath(string gameDir, string launchId) => Path.Combine(LaunchDir(gameDir, launchId), "launch.json");
    public static string NoticesDir(string configDir) => Path.Combine(configDir, "sfi", "notifications");

    // ---- identifiers (the schema's patterns)

    /// <summary>ShroudForge's identifier: 1 to 80 of A-Z a-z 0-9 . _ - (create_mod_api.rs valid_identifier).</summary>
    public static bool SfId([NotNullWhen(true)] string? s) => s is { Length: > 0 and <= 80 } && s.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-');
    private static bool LowerHex([NotNullWhen(true)] string? s, int length) => s is not null && s.Length == length && s.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    public static bool LaunchId([NotNullWhen(true)] string? s) => LowerHex(s, 16);
    private static bool Sha256Hex([NotNullWhen(true)] string? s) => LowerHex(s, 64);
    private static bool WellFormed(string s)
    {
        for (var i = 0; i < s.Length; i++)
        {
            if (char.IsHighSurrogate(s[i]) && i + 1 < s.Length && char.IsLowSurrogate(s[i + 1])) i++;
            else if (char.IsSurrogate(s[i])) return false;
        }
        return true;
    }

    // ---- the bytes (wire/: what serde_json::to_vec and JSON.stringify write for the same members in the same order)

    private static void Str(StringBuilder b, string s)
    {
        b.Append('"');
        foreach (var c in s)
        {
            switch (c)
            {
                case '"': b.Append("\\\""); break;
                case '\\': b.Append("\\\\"); break;
                case '\b': b.Append("\\b"); break;
                case '\f': b.Append("\\f"); break;
                case '\n': b.Append("\\n"); break;
                case '\r': b.Append("\\r"); break;
                case '\t': b.Append("\\t"); break;
                default:
                    if (c < 0x20) b.Append("\\u00").Append(((int)c).ToString("x2", System.Globalization.CultureInfo.InvariantCulture));
                    else b.Append(c);
                    break;
            }
        }
        b.Append('"');
    }

    private static StringBuilder Member(StringBuilder b, string name)
    {
        b.Append(b.Length == 1 ? "" : ",");
        Str(b, name);
        return b.Append(':');
    }

    private static readonly UTF8Encoding Utf8 = new(false, true);

    internal static byte[] ActionBytes(SfiActionRecord r)
    {
        var b = new StringBuilder("{");
        Member(b, "format").Append('1');
        Member(b, "kind"); Str(b, "action");
        Member(b, "requestId"); Str(b, r.RequestId);
        Member(b, "sequence").Append(r.Sequence);
        Member(b, "launchId"); Str(b, r.LaunchId);
        Member(b, "planHash"); Str(b, r.PlanHash);
        Member(b, "ownerEpoch").Append(r.OwnerEpoch);
        Member(b, "modId"); Str(b, r.ModId);
        Member(b, "action"); Str(b, r.Action);
        if (r.Value is { } v) { Member(b, "value"); Str(b, v); }
        Member(b, "createdMs").Append(r.CreatedMs);
        return Utf8.GetBytes(b.Append('}').ToString());
    }

    internal static byte[] ReceiptBytes(SfiLaunch l, long createdMs)
    {
        var b = new StringBuilder("{");
        Member(b, "format").Append('1');
        Member(b, "kind"); Str(b, "launch");
        Member(b, "launchId"); Str(b, l.LaunchId);
        Member(b, "planHash"); Str(b, l.PlanHash);
        Member(b, "pid").Append(l.Pid);
        Member(b, "processStartMs").Append(l.ProcessStartMs);
        Member(b, "createdMs").Append(createdMs);
        return Utf8.GetBytes(b.Append('}').ToString());
    }

    // ---- actions

    /// <summary>Puts one action request for <paramref name="modId"/>'s callback <paramref name="action"/> into this launch's
    /// actions/ folder: written whole under staging/, flushed, then renamed in, so the runtime sees all of it or nothing.
    /// <paramref name="ownerEpoch"/> is the owner's reload epoch as the caller last read it; the runtime removes a request
    /// of another epoch unread. Never waits on the runtime, never retries.</summary>
    public static SfiQueueResult EnqueueAction(string gameDir, SfiLaunch launch, string modId, long ownerEpoch, string action, string? value)
    {
        string? why = !LaunchId(launch.LaunchId) ? "the launch id is not 16 lower-case hex digits"
            : !Sha256Hex(launch.PlanHash) ? "the plan hash is not a lower-case sha256"
            : launch.Pid < 1 || launch.ProcessStartMs < 0 ? "the launch has no process identity"
            : !SfId(modId) ? "the mod id is not a ShroudForge identifier"
            : !SfId(action) ? "the action id is not a ShroudForge identifier"
            : ownerEpoch < 1 ? "owner epochs start at 1"
            : value is not null && !WellFormed(value) ? "the value is not well-formed text"
            : null;
        if (why is not null) return new(SfiQueueStatus.Invalid, null, why);
        lock (Gate)
        {
            var requestId = Guid.NewGuid().ToString("D");
            var record = new SfiActionRecord(requestId, Interlocked.Increment(ref _sequence), launch.LaunchId, launch.PlanHash,
                ownerEpoch, modId, action, value, NowMs());
            var bytes = ActionBytes(record);
            if (bytes.Length > RecordBytes) return new(SfiQueueStatus.TooLarge, null, $"the request would be {bytes.Length} bytes (at most {RecordBytes})");
            try
            {
                if (LinkOnPath(gameDir, launch.LaunchId) is { } link)
                    return new(SfiQueueStatus.Unavailable, null, $"{link} is a link: the bridge never writes through one");
                var actions = ActionsDir(gameDir, launch.LaunchId);
                var staging = StagingDir(gameDir, launch.LaunchId);
                Directory.CreateDirectory(staging);
                var receipt = ReceiptPath(gameDir, launch.LaunchId);
                if (!File.Exists(receipt)) Publish("receipt", staging, receipt, ReceiptBytes(launch, record.CreatedMs));
                Directory.CreateDirectory(actions);
                var pending = CountPending(actions, modId, PendingActionsPerOwner);
                if (pending >= PendingActionsPerOwner)
                    return new(SfiQueueStatus.Full, null, $"{modId} already has {pending} requests waiting");
                Publish("action", staging, Path.Combine(actions, requestId + ".json"), bytes);
                return new(SfiQueueStatus.Queued, requestId, "queued");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException or System.Security.SecurityException)
            {
                Log(LogLevel.Warn, $"action {action} of {modId} not queued: {e.Message}");
                return new(SfiQueueStatus.Unavailable, null, e.Message);
            }
        }
    }

    /// <summary>How many requests for <paramref name="modId"/> wait in this launch's actions/ (an unreadable or foreign
    /// entry counts for nobody).</summary>
    public static int PendingActions(string gameDir, string launchId, string modId)
    {
        var actions = ActionsDir(gameDir, launchId);
        try { return LaunchId(launchId) && Directory.Exists(actions) ? CountPending(actions, modId, int.MaxValue) : 0; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return 0; }
    }

    private static int CountPending(string actions, string modId, int stopAt)
    {
        var n = 0;
        foreach (var f in new DirectoryInfo(actions).EnumerateFiles())
        {
            if (n >= stopAt) break;
            if (OwnerOf(f) == modId) n++;
        }
        return n;
    }

    // the owner from the record's head (canonical records name it in their first few hundred bytes): a click never reads
    // a thousand 2 MiB values just to count
    private static string? OwnerOf(FileInfo f)
    {
        try
        {
            var head = new byte[4096];
            var n = 0;
            using (var s = new FileStream(f.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                for (int r; n < head.Length && (r = s.Read(head, n, head.Length - n)) > 0;) n += r;
            return ModIdOf(head.AsSpan(0, n), n < head.Length);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    private static string? ModIdOf(ReadOnlySpan<byte> head, bool whole)
    {
        try
        {
            var reader = new Utf8JsonReader(head, isFinalBlock: whole, default);
            if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject) return null;
            while (reader.Read() && reader.TokenType == JsonTokenType.PropertyName)
            {
                var isModId = reader.ValueTextEquals("modId"u8);
                if (!reader.Read()) return null;
                if (isModId) return reader.TokenType == JsonTokenType.String ? reader.GetString() : null;
                if (reader.TokenType is JsonTokenType.StartObject or JsonTokenType.StartArray && !reader.TrySkip()) return null;
            }
            return null;
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException) { return null; }
    }

    /// <summary>Writes <paramref name="bytes"/> to a new file under staging/ and renames it to <paramref name="final"/>
    /// (never over an existing file). On an error the staged file goes; a kill leaves it in staging/, never in actions/.</summary>
    private static void Publish(string what, string staging, string final, byte[] bytes)
    {
        var tmp = Path.Combine(staging, Guid.NewGuid().ToString("N") + ".tmp");
        try
        {
            using (var f = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                f.Write(bytes, 0, bytes.Length / 2);
                f.Flush();
                KillPoint?.Invoke(what + ":half-written");
                f.Write(bytes, bytes.Length / 2, bytes.Length - bytes.Length / 2);
                KillPoint?.Invoke(what + ":written");
                f.Flush(flushToDisk: true);
            }
            KillPoint?.Invoke(what + ":before-rename");
            File.Move(tmp, final, overwrite: false);
            KillPoint?.Invoke(what + ":after-rename");
        }
        catch (Exception e) when (e is not SfiKillPointException)
        {
            try { File.Delete(tmp); }
            catch (Exception d) when (d is IOException or UnauthorizedAccessException) { Log(LogLevel.Warn, $"{tmp} left behind: {d.Message}"); }
            // a receipt another writer put there first is as good as ours
            if (what == "receipt" && File.Exists(final)) return;
            throw;
        }
    }

    /// <summary>The first folder of the bridge's path, from <c>kiln</c> down to the launch's own folders, that is a link.</summary>
    private static string? LinkOnPath(string gameDir, string launchId)
    {
        var parts = new[]
        {
            Path.Combine(gameDir, "kiln"), Path.Combine(gameDir, "kiln", "sfi"), UiRoot(gameDir), LaunchDir(gameDir, launchId),
            ActionsDir(gameDir, launchId), StagingDir(gameDir, launchId),
        };
        return parts.FirstOrDefault(IsLink);
    }

    private static bool IsLink(string path)
    {
        try
        {
            var i = new FileInfo(path);
            if (!i.Exists && !Directory.Exists(path))
            {
                // nothing there, or a link to nothing: a dangling link is still a link (opening it would create its target)
                try { return i.LinkTarget is not null; }
                catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return false; }
            }
            return i.LinkTarget is not null || (i.Attributes & FileAttributes.ReparsePoint) != 0;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; }
    }

    // ---- notices

    /// <summary>The notices in <c>&lt;configDir&gt;/sfi/notifications</c>, newest first: every launch's, at most
    /// bounds.json noticeKeysPerProfile (by file name). A file that is partial, malformed, over recordBytes, named for
    /// another key or outside what 1.7.9 can store is left out (<paramref name="skipped"/> counts it), never changed:
    /// this side only reads; the runtime writes. Never read through a link (the folder, or a notice file), and only
    /// regular files are opened: a FIFO or a device named like a notice is left out unopened, so no read blocks.</summary>
    public static IReadOnlyList<SfiNotice> ReadNotices(string configDir, out int skipped)
    {
        skipped = 0;
        var dir = NoticesDir(configDir);
        var list = new List<SfiNotice>();
        IEnumerable<FileInfo> files;
        try
        {
            if (!Directory.Exists(dir)) return list;
            if (new[] { Path.Combine(configDir, "sfi"), dir }.FirstOrDefault(IsLink) is { } link)
            {
                Log(LogLevel.Warn, $"notices not read: {link} is a link");
                return list;
            }
            files = new DirectoryInfo(dir).EnumerateFiles().ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Log(LogLevel.Warn, $"notices not read: {e.Message}");
            return list;
        }
        var named = new List<FileInfo>();
        foreach (var f in files)
        {
            var stem = f.Name.EndsWith(".json", StringComparison.Ordinal) ? f.Name[..^5] : null;
            if (Sha256Hex(stem)) named.Add(f);
            else skipped++;
        }
        named.Sort((a, b) => string.CompareOrdinal(a.Name, b.Name));
        if (named.Count > NoticeKeysPerProfile)
        {
            Log(LogLevel.Warn, $"{named.Count} notice files, more than {NoticeKeysPerProfile}: the rest are not shown");
            skipped += named.Count - NoticeKeysPerProfile;
            named.RemoveRange(NoticeKeysPerProfile, named.Count - NoticeKeysPerProfile);
        }
        foreach (var f in named)
        {
            if (ReadNotice(f) is { } n) list.Add(n);
            else skipped++;
        }
        list.Sort((a, b) => a.UpdatedAt != b.UpdatedAt ? b.UpdatedAt.CompareTo(a.UpdatedAt) : string.CompareOrdinal(a.Key, b.Key));
        return list;
    }

    private static readonly string[] NoticeRecordMembers = ["format", "kind", "key", "keySha256", "launchId", "ownerEpoch", "notice"];
    private static readonly string[] NoticeMembers = ["id", "modId", "title", "message", "level", "actionUrl", "updatedAt"];

    private static SfiNotice? ReadNotice(FileInfo f)
    {
        byte[] bytes;
        try
        {
            // a link is not followed; a FIFO, socket or device reports no length (a notice is never empty) and opening
            // it could block this thread
            f.Refresh();
            if (f.LinkTarget is not null || (f.Attributes & (FileAttributes.ReparsePoint | FileAttributes.Directory | FileAttributes.Device)) != 0 ||
                f.Length <= 0) return null;
            if (f.Length > RecordBytes) return null;
            using var s = new FileStream(f.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            bytes = new byte[RecordBytes + 1];
            var n = 0;
            for (int r; n < bytes.Length && (r = s.Read(bytes, n, bytes.Length - n)) > 0;) n += r;
            if (n > RecordBytes) return null;
            Array.Resize(ref bytes, n);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        try
        {
            using var doc = JsonDocument.Parse(bytes);
            var r = doc.RootElement;
            if (!Exactly(r, NoticeRecordMembers) || !Int(r, "format", out var format) || format != 1 || Text(r, "kind") != "notice") return null;
            var key = Text(r, "key");
            var sha = Text(r, "keySha256");
            var launchId = Text(r, "launchId");
            if (key is null || sha is null || !LaunchId(launchId) || !Int(r, "ownerEpoch", out var epoch) || epoch < 1) return null;
            if (sha != f.Name[..^5] || sha != Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(key)))) return null;
            var n = r.GetProperty("notice");
            if (!Exactly(n, NoticeMembers) || !Int(n, "updatedAt", out var updatedAt) || updatedAt < 0) return null;
            var (id, modId, title, message, level) = (Text(n, "id"), Text(n, "modId"), Text(n, "title"), Text(n, "message"), Text(n, "level"));
            if (!SfId(modId) || id is null || title is null || message is null || level is not ("info" or "error")) return null;
            // 1.7.9: id = modId.local, key = modId-local, local an identifier; title and message limits are bytes
            if (!id.StartsWith(modId + ".", StringComparison.Ordinal) || !SfId(id[(modId.Length + 1)..]) ||
                key != modId + "-" + id[(modId.Length + 1)..]) return null;
            if (Utf8.GetByteCount(title) > 120 || Utf8.GetByteCount(message) > 2000) return null;
            var url = n.GetProperty("actionUrl");
            string? actionUrl = url.ValueKind switch { JsonValueKind.Null => null, JsonValueKind.String => url.GetString(), _ => "" };
            if (actionUrl is not null && !actionUrl.StartsWith("https://", StringComparison.Ordinal)) return null;
            return new SfiNotice(key, id, modId, title, message, level, actionUrl, updatedAt, launchId, epoch);
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException or KeyNotFoundException or ArgumentException) { return null; }
    }

    /// <summary>An object with exactly these members, each once.</summary>
    private static bool Exactly(JsonElement e, string[] members)
    {
        if (e.ValueKind != JsonValueKind.Object) return false;
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var p in e.EnumerateObject())
            if (!members.Contains(p.Name, StringComparer.Ordinal) || !seen.Add(p.Name)) return false;
        return seen.Count == members.Length;
    }

    private static string? Text(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static bool Int(JsonElement e, string name, out long value)
    {
        value = 0;
        return e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out value) &&
               !v.GetRawText().Contains('.') && !v.GetRawText().Contains('e') && !v.GetRawText().Contains('E');
    }

    // ---- cleanup

    /// <summary>Prunes the launch folders under <c>&lt;game&gt;/kiln/sfi/ui</c> whose launch provably ended (a readable
    /// receipt naming the folder, and its process gone or another start) and that nothing changed for
    /// bounds.json keepEndedLaunchDays: kept until then for diagnostics. Kept, always: <paramref name="currentLaunchId"/>,
    /// a running or unknown launch, a folder without a receipt, anything that is or holds a link, anything not named
    /// like a launch. Nothing outside that folder is ever touched (notices, <c>&lt;game&gt;/shroudforge</c>), and a
    /// failure only logs: nothing is moved, so no ended launch's request can reach a live launch.</summary>
    public static IReadOnlyList<SfiCleanupOutcome> Cleanup(string gameDir, string? currentLaunchId)
    {
        var outcomes = new List<SfiCleanupOutcome>();
        var root = UiRoot(gameDir);
        try
        {
            if (!Directory.Exists(root)) return outcomes;
            var lockPath = Path.Combine(root, ".cleanup.lock");
            if (new[] { Path.Combine(gameDir, "kiln"), Path.Combine(gameDir, "kiln", "sfi"), root, lockPath }.FirstOrDefault(IsLink) is { } link)
            {
                Log(LogLevel.Warn, $"cleanup skipped: {link} is a link");
                outcomes.Add(new("", false, "a link on the path"));
                return outcomes;
            }
            FileStream lockFile;
            try { lockFile = new FileStream(lockPath, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None); }
            catch (IOException)
            {
                outcomes.Add(new("", false, "another cleanup runs"));
                return outcomes;
            }
            using (lockFile)
            {
                var full = Path.GetFullPath(root) + Path.DirectorySeparatorChar;
                foreach (var entry in new DirectoryInfo(root).EnumerateFileSystemInfos().OrderBy(e => e.Name, StringComparer.Ordinal).ToList())
                {
                    if (entry.Name == ".cleanup.lock") continue;
                    outcomes.Add(Prune(entry, currentLaunchId, full));
                }
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Log(LogLevel.Warn, $"cleanup stopped: {e.Message}");
            outcomes.Add(new("", false, "cleanup stopped: " + e.Message));
        }
        return outcomes;
    }

    private static SfiCleanupOutcome Prune(FileSystemInfo entry, string? current, string root)
    {
        var name = entry.Name;
        if (entry is not DirectoryInfo dir || !LaunchId(name)) return new(name, false, "not a launch folder");
        if (name == current) return new(name, false, "this launch");
        if (IsLink(dir.FullName)) return new(name, false, "a link");
        var receipt = Path.Combine(dir.FullName, "launch.json");
        if (!File.Exists(receipt) || IsLink(receipt)) return new(name, false, "no receipt");
        if (ReadReceipt(receipt, name) is not { } who) return new(name, false, "unreadable receipt");
        switch (Liveness(who.Pid, who.ProcessStartMs))
        {
            case SfiLiveness.Alive: return new(name, false, "running");
            case SfiLiveness.Unknown: return new(name, false, "liveness unknown");
        }
        var all = new List<FileSystemInfo>();
        var newest = dir.LastWriteTimeUtc;
        var pending = new Stack<DirectoryInfo>([dir]);
        while (pending.Count > 0)
            foreach (var e in pending.Pop().EnumerateFileSystemInfos())
            {
                if (e.LinkTarget is not null || (e.Attributes & FileAttributes.ReparsePoint) != 0) return new(name, false, "holds a link");
                if (!Path.GetFullPath(e.FullName).StartsWith(root, StringComparison.OrdinalIgnoreCase)) return new(name, false, "outside the bridge");
                all.Add(e);
                if (e.LastWriteTimeUtc > newest) newest = e.LastWriteTimeUtc;
                if (e is DirectoryInfo d) pending.Push(d);
            }
        var idleMs = NowMs() - new DateTimeOffset(newest).ToUnixTimeMilliseconds();
        if (idleMs < KeepEndedLaunchDays * 86_400_000L) return new(name, false, "ended, kept for diagnostics");
        try
        {
            // deepest first, so each folder is empty when its turn comes; the receipt last: it is the only proof the launch
            // ended, so a removal that fails part way leaves a folder a later cleanup can still prove and finish
            foreach (var e in all.Where(e => !string.Equals(e.FullName, receipt, StringComparison.Ordinal)).OrderByDescending(e => e.FullName.Length))
            {
                if (e is DirectoryInfo d) d.Delete(recursive: false);
                else e.Delete();
            }
            File.Delete(receipt);
            dir.Delete(recursive: false);
            Log(LogLevel.Info, $"removed ended launch folder {name}");
            return new(name, true, "ended");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Log(LogLevel.Warn, $"ended launch folder {name} not removed: {e.Message}");
            return new(name, false, "removal failed: " + e.Message);
        }
    }

    private static SfiLaunch? ReadReceipt(string path, string folder)
    {
        try
        {
            var f = new FileInfo(path);
            if (f.Length > MaxReceiptBytes) return null;
            using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object || !Int(r, "format", out var format) || format != 1 || Text(r, "kind") != "launch" ||
                Text(r, "launchId") != folder || !Int(r, "pid", out var pid) || pid is < 1 or > int.MaxValue ||
                !Int(r, "processStartMs", out var start) || start < 0) return null;
            return new SfiLaunch(folder, Text(r, "planHash") ?? "", (int)pid, start);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    /// <summary>Alive: a process <paramref name="pid"/> runs and started within a second of <paramref name="startMs"/>.
    /// Ended: no such process, or another start under a reused pid. Unknown: it cannot be told (access refused).</summary>
    internal static SfiLiveness ProcessLiveness(int pid, long startMs)
    {
        Process p;
        try { p = Process.GetProcessById(pid); }
        catch (ArgumentException) { return SfiLiveness.Ended; }
        catch (Exception) { return SfiLiveness.Unknown; }
        using (p)
        {
            try
            {
                if (p.HasExited) return SfiLiveness.Ended;
                var started = new DateTimeOffset(p.StartTime.ToUniversalTime()).ToUnixTimeMilliseconds();
                return Math.Abs(started - startMs) <= 1000 ? SfiLiveness.Alive : SfiLiveness.Ended;
            }
            catch (Exception) { return SfiLiveness.Unknown; }
        }
    }

    internal static void ResetForTests()
    {
        Log = (level, text) => UiEngine.Log(level, "sfi bridge: " + text);
        Liveness = ProcessLiveness;
        NowMs = () => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        KillPoint = null;
        Interlocked.Exchange(ref _sequence, -1);
    }
}
