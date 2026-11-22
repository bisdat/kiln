// SPDX-License-Identifier: MIT
//
// The persistent do-not-save notice in game. kiln-sfi says in kiln/state/sfi.json which
// packages do not run whole this start, or were save-sensitive and active at the previous start and are off now
// (schema/sfi/sfi-state.schema.json, written by crates/sfi/kiln-sfi/src/state). Kiln.Ui shows it:
//  - a banner at the earliest main-menu frame (or in the world, when the warning comes later), again on entering a world,
//    on a new warning revision and at each kiln.saves SAVING event; hiding it hides only the banner;
//  - a badge on the package's row on Kiln's Mods page, the whole text in its details (and the Hide button);
//  - the game's attention marker on Kiln's own Mods entry in the pause menu (always drawn, whatever modEntries says);
//  - "N mods off: do not save" (or "N mods off this start" when no package can lose data) at the bottom of the main
//    menu, whatever the footer setting says.
// Only a start of this process with the kiln backend counts: hosted, installed, aside, another process or no file
// changes nothing (no thread after the first minute, no frame demand, no event, no entry flag).
// Once Kiln.Ui is up and can draw in a kiln start, it creates the named event Local\Kiln.Sfi.UiReady.<pid>: kiln-sfi's
// native fallback banner draws only while that event is missing.
// Threads: the file and kiln.saves are read on this file's own worker; the render thread reads only the published view.
// No save interception, no world or game calls.
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;

namespace Kiln;

/// <summary>Whether a package's data is in the game (sfi-state warning.presence).</summary>
internal enum SfiPresence { NotApplied, DataAppliedRuntimeStopped, Unknown }

/// <summary>One package's do-not-save warning as Kiln.Ui shows it.</summary>
internal sealed record SfiWarning(string Id, string Label, bool Skipped, SfiPresence Presence, bool SaveSensitive, long Revision,
    string Text, bool Late = false)
{
    /// <summary>Saving now can lose data: a save-sensitive package, or one whose runtime stopped during this start (its
    /// data applied, nothing maintaining it). Only then do the banner, footer, badge and details say "do not save".</summary>
    internal bool MustNotSave => SaveSensitive || Late;
}

/// <summary>What sfi.json says about this start, published whole to the render thread.</summary>
internal sealed record SfiSafetyView(string? LaunchId, long WarningRevision, IReadOnlyList<SfiWarning> Warnings);

/// <summary>One delivery of the banner: a frame drew it (kept by Kiln.Ui: kiln-sfi alone writes sfi.json).</summary>
internal readonly record struct SfiNoticeShown(long Revision, string Why, int WorldSession, long AtMs);

/// <summary>sfi.json read for Kiln.Ui (pure: tests).</summary>
internal static class SfiSafetyReader
{
    public const int MaxText = 8192, MaxPackages = 4096;
    /// <summary>processStartMs may differ by this much from what .NET reads for the same process.</summary>
    public const long StartSlackMs = 2000;

    /// <summary>The warnings of this process's kiln start; null when the file is not one (hosted, installed, aside, another
    /// process or start, another format).</summary>
    public static SfiSafetyView? Read(JsonElement d, int pid, long processStartMs)
    {
        if (d.ValueKind != JsonValueKind.Object || Int(d, "format") != 1) return null;
        if (!d.TryGetProperty("launch", out var l) || l.ValueKind != JsonValueKind.Object || Int(l, "pid") != pid) return null;
        if (processStartMs > 0 && Int(l, "processStartMs") is { } ps && Math.Abs(ps - processStartMs) > StartSlackMs) return null;
        if (!l.TryGetProperty("backend", out var b) || b.ValueKind != JsonValueKind.Object || Str(b, "selected") != "kiln") return null;
        if (Str(d, "phase") is not ("starting" or "running" or "ended" or "refused")) return null;
        var rows = new List<SfiWarning>();
        if (d.TryGetProperty("packages", out var pk) && pk.ValueKind == JsonValueKind.Array)
            foreach (var p in pk.EnumerateArray().Take(MaxPackages))
                if (Row(p) is { } w) rows.Add(w);
        return new SfiSafetyView(Str(l, "launchId"), Math.Max(0, Int(d, "warningRevision") ?? 0), rows);
    }

    /// <summary>A package's warning, or null. Disabled and other-side rows never warn, except a save-sensitive package that
    /// was active at the previous start; a presence the record does not back is unknown, never "not applied".</summary>
    internal static SfiWarning? Row(JsonElement p)
    {
        if (p.ValueKind != JsonValueKind.Object || Str(p, "id") is not { Length: > 0 } id) return null;
        if (!p.TryGetProperty("warning", out var w) || w.ValueKind != JsonValueKind.Object) return null;
        var intent = Str(p, "intent");
        var trigger = Str(w, "trigger");
        var sensitive = p.TryGetProperty("saveSensitive", out var ss) && ss.ValueKind == JsonValueKind.Object && Bool(ss, "value") == true;
        JsonElement sk = default;
        var skipped = intent == "selected" && trigger == "skipped" && p.TryGetProperty("skip", out sk) && sk.ValueKind == JsonValueKind.Object;
        var late = skipped && Bool(sk, "late") == true;
        var off = intent is "disabled" or "absent" && trigger == "save-sensitive-off" && sensitive;
        if (!skipped && !off) return null;
        var archive = Bool(p, "archivePresent");
        var runtime = Bool(p, "runtimeActive");
        var presence = Str(w, "presence") switch
        {
            "not-applied" when archive == false => SfiPresence.NotApplied,
            "data-applied-runtime-stopped" when archive == true && runtime != true => SfiPresence.DataAppliedRuntimeStopped,
            _ => SfiPresence.Unknown,
        };
        sensitive |= Bool(w, "saveSensitive") == true;
        var label = Label(id, Str(p, "version"), Str(p, "name"));
        var text = Str(w, "text")?.Trim() ?? "";
        if (text.Length == 0) text = label + ": " + (sensitive || late ? DoNotSave : OffThisStart);
        if (text.Length > MaxText) text = text[..MaxText];
        return new SfiWarning(id, label, skipped, presence, sensitive, Math.Max(0, Int(w, "revision") ?? 0), text, late);
    }

    public const string DoNotSave = "Do not save while this mod is off; restore it and restart first.";
    public const string OffThisStart = "off this start; the game and the other mods go on without it.";

    /// <summary>"Name (id version)" as kiln-sfi's warning texts name a package.</summary>
    internal static string Label(string id, string? version, string? name)
    {
        var v = version is { Length: > 0 } ? id + " " + version : id;
        return name is { Length: > 0 } && name != id ? $"{name} ({v})" : v;
    }

    private static string? Str(JsonElement e, string k) =>
        e.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;

    private static long? Int(JsonElement e, string k) =>
        e.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt64(out var n) ? n : null;

    private static bool? Bool(JsonElement e, string k) =>
        e.TryGetProperty(k, out var v) ? v.ValueKind switch { JsonValueKind.True => true, JsonValueKind.False => false, _ => null } : null;
}

/// <summary>When the banner shows (pure: tests). Every member is safe from any thread.</summary>
internal sealed class SfiNoticeModel
{
    private readonly object _lock = new();
    private SfiSafetyView? _view;
    private bool _armed;
    private string _why = "";
    private long _armSerial, _ackedSerial, _seenRevision = -1;
    private ulong _savingSeq;
    private int _world;
    private readonly List<SfiNoticeShown> _shown = [];

    public SfiSafetyView? View => Volatile.Read(ref _view);
    public bool Warns => View is { Warnings.Count: > 0 };
    /// <summary>The banner is to be drawn (where it can show: <see cref="SfiSafetyNotice.ShowsOn"/>).</summary>
    public bool BannerUp { get { lock (_lock) return _armed && Warns; } }
    public long ArmSerial { get { lock (_lock) return _armSerial; } }
    public int WorldSession { get { lock (_lock) return _world; } }
    public IReadOnlyList<SfiNoticeShown> Shown { get { lock (_lock) return _shown.ToArray(); } }

    /// <summary>A view read from sfi.json. The first warnings and every higher warning revision show the banner again.
    /// True when something drawn changed.</summary>
    public bool Publish(SfiSafetyView v)
    {
        lock (_lock)
        {
            var before = (_view?.Warnings.Count ?? -1, _armed);
            Volatile.Write(ref _view, v);
            if (v.Warnings.Count == 0) _armed = false;
            else if (v.WarningRevision > _seenRevision)
            {
                Arm(_seenRevision < 0 ? "start" : "revision");
                _seenRevision = v.WarningRevision;
            }
            return before != (v.Warnings.Count, _armed);
        }
    }

    /// <summary>The game's screen changed: entering a world shows the banner again.</summary>
    public bool ScreenChanged(bool wasInGame, bool inGame)
    {
        if (wasInGame || !inGame) return false;
        lock (_lock)
        {
            _world++;
            if (!Warns) return false;
            Arm("world");
            return true;
        }
    }

    /// <summary>kiln.saves SAVING with this seq: the banner shows again (a notice only: the save goes on).</summary>
    public bool Saving(ulong seq)
    {
        lock (_lock)
        {
            if (seq <= _savingSeq) return false;
            _savingSeq = seq;
            if (!Warns) return false;
            Arm("saving");
            return true;
        }
    }

    /// <summary>The player hid the banner: the badge, the details, the marker, the footer line and the next showing stay.</summary>
    public void Dismiss()
    {
        lock (_lock) _armed = false;
    }

    /// <summary>A frame drew the banner armed as <paramref name="serial"/>: one delivery per showing. False when this
    /// showing was already counted, or another showing armed meanwhile (the next frame counts that one).</summary>
    public bool Acknowledge(long serial, long nowMs)
    {
        lock (_lock)
        {
            if (!_armed || serial != _armSerial || serial == _ackedSerial) return false;
            _ackedSerial = serial;
            _shown.Add(new SfiNoticeShown(_view?.WarningRevision ?? 0, _why, _world, nowMs));
            return true;
        }
    }

    private void Arm(string why)
    {
        _armed = true;
        _why = why;
        _armSerial++;
    }
}

/// <summary>One line of the banner: its text and how it is drawn.</summary>
internal readonly record struct SfiBannerLine(string Text, SfiBannerStyle Style);

internal enum SfiBannerStyle { Title, Package, Status, Body, Hint }

internal static unsafe class SfiSafetyNotice
{
    internal const int BannerPackages = 4, BannerBodyChars = 280;
    internal const string BadgeText = "do not save", BadgeOffText = "off this start";

    /// <summary>The row badge's text: "do not save" only where saving can lose data (SfiWarning.MustNotSave).</summary>
    internal static string BadgeFor(SfiWarning w) => w.MustNotSave ? BadgeText : BadgeOffText;
    internal const long GiveUpMs = 60_000;

    internal static SfiNoticeModel Model = new();
    private static int _started;
    private static nint _ready;
    private static bool _readyRefusedLogged;  // a failed CreateEventW was logged (once: Apply calls EnsureReady every 2 s)
    private static bool _attention;
    private static int _faults;
    private static ulong _saveSeq;
    private static readonly object ReadyLock = new();

    internal static Action<LogLevel, string> Log = (level, text) => UiEngine.Log(level, "do-not-save notice: " + text);
    internal static Func<long> NowMs = () => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
    /// <summary>The screen now and whether the game's screens are known at all (tests replace it).</summary>
    internal static Func<(ScreenKind Kind, bool InGame, bool Known)> ScreenNow = () =>
    {
        try
        {
            var c = Ui.Screens.Current;
            return (c.Kind, c.InGame, Ui.Screens.Available);
        }
        catch (Exception) { return (ScreenKind.None, false, false); }
    };
    /// <summary>Kiln.Ui can draw (kiln.ui's overlay drawing is on); tests replace it.</summary>
    internal static Func<bool> DrawingOn = () => UiNative.DrawingOffReason == null;
    /// <summary>Draws the banner and the footer line; tests replace it to see what a frame draws.</summary>
    internal static Action<UiDraw, IReadOnlyList<SfiBannerLine>?, string?> Paint = PaintNative;
    /// <summary>The next kiln.saves event after a seq: (kind, seq), or null (none, or no kiln.saves). Worker thread.</summary>
    internal static Func<ulong, (uint Kind, ulong Seq)?>? NextSave;

    internal const uint SaveEventSaving = 1;

    // ---- worker ----

    /// <summary>Kiln.Ui is up (kiln.ui subscribed): watch sfi.json and kiln.saves on a worker. Once.</summary>
    public static void Start()
    {
        if (Interlocked.Exchange(ref _started, 1) != 0) return;
        new Thread(Watch) { IsBackground = true, Name = "kiln sfi notice" }.Start();
    }

    private static void Watch()
    {
        try
        {
            var src = ModCatalog.Sources();
            var startMs = src.ProcessStartUtc == DateTime.MinValue ? 0 : new DateTimeOffset(src.ProcessStartUtc).ToUnixTimeMilliseconds();
            var path = Path.Combine(src.StateDir, "sfi.json");
            var clock = Stopwatch.StartNew();
            var nextRead = 0L;
            var kiln = false;
            while (true)
            {
                if (clock.ElapsedMilliseconds >= nextRead)
                {
                    nextRead = clock.ElapsedMilliseconds + (kiln ? 2000 : 1000);
                    if (ReadFile(path, src.Pid, startMs) is { } view)
                    {
                        if (!kiln) StartSaves();
                        kiln = true;
                        Apply(view);
                    }
                    else if (!kiln && clock.ElapsedMilliseconds > GiveUpMs) return;  // not a kiln start: nothing changes
                }
                if (kiln) PollSaves();
                Thread.Sleep(250);
            }
        }
        catch (Exception e)
        {
            Log(LogLevel.Warn, "stopped: " + e.Message);
        }
    }

    /// <summary>sfi.json, read on the worker; null when it is missing, unreadable or not this process's kiln start.</summary>
    internal static SfiSafetyView? ReadFile(string path, int pid, long processStartMs)
    {
        try
        {
            if (ReadBytes(path) is not { } bytes) return null;
            using var doc = JsonDocument.Parse(bytes, new JsonDocumentOptions { MaxDepth = 64 });
            return SfiSafetyReader.Read(doc.RootElement, pid, processStartMs);
        }
        catch (Exception) { return null; }  // a rename in progress or a torn file: the next read decides
    }

    /// <summary>Tests only: called with the open stream, before it is read.</summary>
    internal static Action<FileStream>? OnOpenForTests;

    /// <summary>sfi.json's bytes, read as SfiState and SfiBridge.ReadClaim read it: delete sharing (kiln-sfi replaces the
    /// file by rename while this reads), at most <paramref name="max"/> bytes however much the file grows meanwhile, and a
    /// short retry on a sharing error. Null when it is missing, empty, not a regular file, too large or unreadable.</summary>
    internal static byte[]? ReadBytes(string path, int max = SfiBridge.MaxStateBytes)
    {
        for (var attempt = 0; attempt < 3; attempt++)
        {
            if (attempt > 0) Thread.Sleep(50);
            try
            {
                var info = new FileInfo(path);
                // a FIFO or socket has no length; opening one could block this worker
                if (!info.Exists || info.Length <= 0 || info.Length > max) return null;
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                OnOpenForTests?.Invoke(fs);
                var buf = new byte[(int)Math.Min(fs.Length, max) + 1];
                var n = 0;
                for (int r; n < buf.Length && (r = fs.Read(buf, n, buf.Length - n)) > 0;) n += r;
                if (n > max) return null;
                return buf[..n];
            }
            catch (Exception e) when (e is FileNotFoundException or DirectoryNotFoundException) { return null; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return null;
    }

    /// <summary>A view of this process's kiln start (worker).</summary>
    internal static void Apply(SfiSafetyView view)
    {
        var changed = Model.Publish(view);
        EnsureReady(Environment.ProcessId, true, DrawingOn());
        var warns = view.Warnings.Count > 0;
        if (warns != _attention)
        {
            _attention = warns;
            GameMenuEntries.SafetyAttention(warns);
        }
        if (changed)
        {
            Log(LogLevel.Warn, warns ? $"{view.Warnings.Count} package(s) off this start (warning revision {view.WarningRevision}): " +
                                       string.Join("; ", view.Warnings.Select(w => w.Id)) : "no package off this start");
            UiEngine.Recount();
        }
    }

    // ---- kiln.saves: SAVING by polling next_event (any thread; Kiln.Saves keeps its own pump) ----

    [StructLayout(LayoutKind.Sequential)]
    internal struct SaveEvent
    {
        public uint Size, Kind;
        public ulong Seq;
        public uint Reason, Flags;
        public ulong Step;
        public long UnixMs;
        public uint Revision, Result;
        public ushort Slot, Reserved16;
        public uint WorldUnix;
        public fixed byte Guid[16];
        public double DurationMs;
        public ulong SaveSeq;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct SavesStatus
    {
        public uint Size, Features, State, LastReason;
        public fixed byte Guid[16];
        public uint HasGuid, Revision;
        public ushort Slot, PendingRequest;
        public uint WorldUnix;
        public long LastWrittenUnixMs, LastAcceptedUnixMs;
        public double LastDurationMs;
        public long AutosaveIntervalNs;
        public ulong Accepted, Refused, Written, Failed, LastSeq, Step;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct SavesTable
    {
        public ServiceHeader Header;
        public delegate* unmanaged[Cdecl]<SavesStatus*, int> GetStatus;
        public delegate* unmanaged[Cdecl]<uint, int> Request;
        public delegate* unmanaged[Cdecl]<ulong, SaveEvent*, int> NextEvent;
    }

    private static void StartSaves()
    {
        if (NextSave is not null) return;  // a test's
        var t = Services.Get<SavesTable>("kiln.saves", 1);
        if (t == null || t->NextEvent == null) return;
        SavesStatus st = default;
        st.Size = (uint)sizeof(SavesStatus);
        if (t->GetStatus != null && t->GetStatus(&st) == 0) _saveSeq = st.LastSeq;  // earlier saves are history
        NextSave = after =>
        {
            SaveEvent e = default;
            e.Size = (uint)sizeof(SaveEvent);
            return t->NextEvent(after, &e) == 0 ? (e.Kind, e.Seq) : null;
        };
    }

    /// <summary>Reads the kiln.saves events since the last read: each SAVING shows the banner again (worker).</summary>
    internal static void PollSaves()
    {
        var next = NextSave;
        if (next is null) return;
        var changed = false;
        for (var i = 0; i < 64 && next(_saveSeq) is { } e && e.Seq > _saveSeq; i++)
        {
            _saveSeq = e.Seq;
            if (e.Kind == SaveEventSaving && Model.Saving(e.Seq)) changed = true;
        }
        if (changed)
        {
            Log(LogLevel.Warn, "a world save began while packages are off: banner shown again");
            UiEngine.Recount();
        }
    }

    // ---- the named event kiln-sfi's native fallback probes ----

    internal static string ReadyEventName(int pid) => $"Local\\Kiln.Sfi.UiReady.{pid}";

    /// <summary>Creates Local\Kiln.Sfi.UiReady.&lt;pid&gt; (manual reset, signalled, held for the process) once Kiln.Ui is up
    /// and can draw in a kiln start. Hosted, installed or drawing off: none (kiln-sfi's fallback decides then). A name
    /// someone else made first (perhaps unsignalled) is kept and signalled, as ours starts signalled; a CreateEventW that
    /// fails (the name held by another object type, say) is logged once, and later calls try again quietly.</summary>
    internal static bool EnsureReady(int pid, bool kilnStart, bool drawingOn)
    {
        if (!kilnStart || !drawingOn || !OperatingSystem.IsWindows()) return false;
        lock (ReadyLock)
        {
            if (_ready != 0) return true;
            var h = CreateEventW(0, true, true, ReadyEventName(pid));
            var error = Marshal.GetLastPInvokeError();
            if (h == 0)
            {
                if (!_readyRefusedLogged)
                    Log(LogLevel.Warn, $"{ReadyEventName(pid)} not created (error {error}): kiln-sfi may draw its own banner too");
                _readyRefusedLogged = true;
                return false;
            }
            if (error == ErrorAlreadyExists)
            {
                // CreateEventW ignores the initial state of an event that exists: a squatter's may be unsignalled
                var signalled = SetEvent(h);
                Log(LogLevel.Warn, $"{ReadyEventName(pid)} already existed (another holder); " +
                                   (signalled ? "signalled it" : $"SetEvent failed (error {Marshal.GetLastPInvokeError()})"));
            }
            _ready = h;
            Log(LogLevel.Info, $"Kiln.Ui shows the do-not-save notice ({ReadyEventName(pid)})");
            return true;
        }
    }

    // ---- what is drawn (pure: tests) ----

    /// <summary>Screens where the banner shows: the main menu (the earliest usable menu) and the world; every screen when
    /// the game's screens are not known.</summary>
    internal static bool ShowsOn(ScreenKind kind, bool inGame, bool known) => !known || kind == ScreenKind.MainMenu || inGame;

    /// <summary>The line at the bottom of the main menu (shown whatever Kiln's footer setting says).</summary>
    internal static string? FooterText(SfiSafetyView? v) =>
        v is { Warnings.Count: > 0 } ? MainMenuFooter.WarningLine(v.Warnings.Count, v.Warnings.Any(w => w.MustNotSave)) : null;

    /// <summary>The package's state words: why it is off and whether its data is in the game.</summary>
    internal static string StatusLine(SfiWarning w)
    {
        var why = !w.Skipped ? "Active at the previous start, off now" : w.Late ? "Stopped during this start" : "Skipped this start";
        var data = w.Presence switch
        {
            SfiPresence.DataAppliedRuntimeStopped => "Startup data still applied, runtime stopped: cannot be undone until restart",
            SfiPresence.NotApplied => "Not applied: its data is not in the game",
            _ => "Unknown: whether its data is in the game could not be verified",
        };
        return w.SaveSensitive ? $"{why}. {data}. Save-sensitive: saving can lose items or saved data." : $"{why}. {data}.";
    }

    /// <summary>The banner's lines: the first packages with their state and the start of their text (the whole text is
    /// in the row's details on Kiln's Mods page).</summary>
    internal static IReadOnlyList<SfiBannerLine> Banner(SfiSafetyView v)
    {
        var n = v.Warnings.Count;
        var lines = new List<SfiBannerLine>
        {
            new((v.Warnings.Any(w => w.MustNotSave) ? "Do not save: " : "") + (n == 1 ? "1 mod is off this start" : $"{n} mods are off this start"),
                SfiBannerStyle.Title),
        };
        foreach (var w in v.Warnings.Take(BannerPackages))
        {
            lines.Add(new(w.Label, SfiBannerStyle.Package));
            lines.Add(new(StatusLine(w), SfiBannerStyle.Status));
            lines.Add(new(w.Text.Length <= BannerBodyChars ? w.Text : w.Text[..BannerBodyChars].TrimEnd() + "...", SfiBannerStyle.Body));
        }
        if (n > BannerPackages) lines.Add(new($"and {n - BannerPackages} more", SfiBannerStyle.Body));
        lines.Add(new("The whole text is on Kiln's Mods page (the Mods entry), where this banner can be hidden. " +
                      "It shows again on entering a world, on a new warning and at each save.", SfiBannerStyle.Hint));
        return lines;
    }

    /// <summary>The warning of a Mods page row (a ShroudForge package with a warning this start), or null.</summary>
    internal static SfiWarning? Badge(CatalogMod m)
    {
        if (m.Engine != "shroudforge" || Model.View is not { Warnings.Count: > 0 } v) return null;
        return v.Warnings.FirstOrDefault(w => string.Equals(w.Id, m.Id, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Frames kiln.ui must draw for the notice (UiEngine.Recount).</summary>
    internal static uint Demand => Model.Warns ? 1u : 0u;

    // ---- render thread ----

    /// <summary>The game's screen changed (game UI thread).</summary>
    public static void ScreenChanged(GameScreen before, GameScreen now)
    {
        if (Model.ScreenChanged(before.InGame, now.InGame)) UiEngine.Recount();
    }

    /// <summary>Kiln.Ui's frame (render thread): the footer line on the main menu, the banner where it shows. A showing is
    /// delivered only once a frame drew it; a frame that faults delivers nothing and the banner stays up.</summary>
    public static void Draw(UiDraw ui)
    {
        var v = Model.View;
        if (v is not { Warnings.Count: > 0 }) return;
        var (kind, inGame, known) = ScreenNow();
        var footer = !known || kind == ScreenKind.MainMenu ? FooterText(v) : null;
        var serial = Model.ArmSerial;
        var banner = Model.BannerUp && ShowsOn(kind, inGame, known) ? Banner(v) : null;
        if (footer is null && banner is null) return;
        try
        {
            Paint(ui, banner, footer);
        }
        catch (Exception e)
        {
            if (Interlocked.Increment(ref _faults) <= 3) Log(LogLevel.Error, "drawing failed: " + e.Message);
            return;
        }
        if (banner is not null && Model.Acknowledge(serial, NowMs()))
        {
            var last = Model.Shown[^1];
            Log(LogLevel.Info, $"banner shown (warning revision {last.Revision}, {last.Why}, world session {last.WorldSession})");
        }
    }

    private static void PaintNative(UiDraw ui, IReadOnlyList<SfiBannerLine>? banner, string? footer)
    {
        var s = ui.Scale;
        var p = KilnLook.Player;
        if (footer is not null)
        {
            var (fw, fh) = ui.MeasureText(footer, 18 * s, 0);
            var fx = (ui.Width - fw) / 2;
            var fy = ui.Height - 96 * s;
            ui.DrawRect(UiLayer.Foreground, fx - 14 * s, fy - 6 * s, fx + fw + 14 * s, fy + fh + 6 * s, GameColors.Night.WithAlpha(225), 4 * s);
            ui.DrawText(UiLayer.Foreground, fx, fy, footer, p.Warning, 18 * s);
        }
        if (banner is null) return;
        var wrap = Math.Min(ui.Width - 80 * s, 820 * s);
        var x = (ui.Width - wrap) / 2;
        var y0 = 48 * s;
        var sizes = banner.Select(l => l.Style switch
        {
            SfiBannerStyle.Title => 22 * s,
            SfiBannerStyle.Package => 18 * s,
            SfiBannerStyle.Hint => 14 * s,
            _ => 15 * s,
        }).ToArray();
        var heights = banner.Select((l, i) => ui.MeasureText(l.Text, sizes[i], wrap).Height).ToArray();
        var total = heights.Sum() + 6 * s * banner.Count + 24 * s;
        ui.DrawRect(UiLayer.Foreground, x - 22 * s, y0, x + wrap + 22 * s, y0 + total, GameColors.Night.WithAlpha(238), 6 * s);
        ui.DrawRect(UiLayer.Foreground, x - 22 * s, y0, x - 16 * s, y0 + total, p.Warning, 3 * s);
        var y = y0 + 12 * s;
        for (var i = 0; i < banner.Count; i++)
        {
            var c = banner[i].Style switch
            {
                SfiBannerStyle.Title or SfiBannerStyle.Status => p.Warning,
                SfiBannerStyle.Hint => p.Text.WithAlpha(170),
                _ => p.Text,
            };
            ui.DrawText(UiLayer.Foreground, x, y, banner[i].Text, c, sizes[i], wrap);
            y += heights[i] + 6 * s;
        }
    }

    /// <summary>The do-not-save part of a row's details on Kiln's Mods page (render thread; nothing for other rows).</summary>
    internal static void DrawDetails(UiDraw ui, CatalogMod m, float s, Action<UiDraw, string, float> label)
    {
        if (Badge(m) is not { } w) return;
        label(ui, w.MustNotSave ? "DO NOT SAVE" : "OFF THIS START", s);
        using (ui.PushColor(UiColorSlot.Text, GameColors.Warning))
            ui.TextWrapped(StatusLine(w));
        ui.TextWrapped(w.Text);
        if (Model.BannerUp)
        {
            if (ui.Button("Hide the banner##kiln.sfi.notice")) Model.Dismiss();
        }
        else
            using (ui.PushColor(UiColorSlot.Text, GameColors.Muted))
                ui.TextWrapped("The banner is hidden. It shows again on entering a world, on a new warning and at each save.");
    }

    internal static void ResetForTests()
    {
        Model = new SfiNoticeModel();
        lock (ReadyLock)
        {
            if (_ready != 0) CloseHandle(_ready);
            _ready = 0;
            _readyRefusedLogged = false;
        }
        _attention = false;
        _faults = 0;
        _saveSeq = 0;
        NextSave = null;
        OnOpenForTests = null;
        Paint = PaintNative;
    }

    internal static ulong SaveSeqForTests { get => _saveSeq; set => _saveSeq = value; }

    private const int ErrorAlreadyExists = 183;

    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern nint CreateEventW(nint attributes, bool manualReset, bool initialState, string name);

    [DllImport("kernel32")]
    private static extern bool CloseHandle(nint h);

    [DllImport("kernel32", SetLastError = true)]
    private static extern bool SetEvent(nint h);
}
