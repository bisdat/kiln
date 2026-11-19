// SPDX-License-Identifier: MIT
// Phase B of Kiln's graphics tools (0.2.0-dev.29.18, docs/kiln/graphics.md "Behind the game's FSR 3"): the upscaler the
// player chooses to run behind the game's own FSR 3 option (kiln.json graphics.upscaler, graphics.amdUpscaler), what
// runs now and why not (from kiln.ui's graphics_status), the state of the vendor files in kiln/vendor/ (for the Graphics
// page; the game side checks their SHA-256 before it loads any), and kiln/state/graphics.json, the status the launcher
// shows (schema/state-graphics.schema.json), rewritten only when it changes.
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>What runs behind the game's FSR 3 option (kiln.json <c>graphics.upscaler</c>).</summary>
internal enum UpscalerChoice
{
    /// <summary>The game's own FSR 3 (nothing changes).</summary>
    Game = 0,
    /// <summary>AMD FSR: FSR 4 where the card and driver support it, else FSR 3.1, by AMD's own selection.</summary>
    Amd = 1,
    /// <summary>Intel XeSS.</summary>
    Xess = 2,
}

/// <summary>Which AMD FSR (kiln.json <c>graphics.amdUpscaler</c>).</summary>
internal enum AmdUpscalerMode
{
    /// <summary>AMD's own selection: FSR 4 where it runs, else FSR 3.1.</summary>
    Auto = 0,
    /// <summary>FSR 3.1 even where FSR 4 runs.</summary>
    Fsr3 = 1,
}

/// <summary>What upscales behind the game's FSR 3 option now: <c>KILN_UI_UPSCALE_RUN_*</c> (kiln_ui.h), plus
/// <see cref="Unknown"/> for a kiln-ui.dll that does not say. The names are the state file's (<see cref="UpscalerCodes"/>).</summary>
internal enum UpscalerRunning
{
    /// <summary>Kiln's swap is not in ("game" chosen, or its hooks are not in: the reason says why). Whatever the game
    /// chose runs, and Kiln does not see it.</summary>
    Off = 0,
    /// <summary>The swap is in, but the game did not upscale with FSR 3 in the last two seconds (FSR 3 is not the game's
    /// chosen upscaler, or no world is loaded).</summary>
    NotFsr = 1,
    /// <summary>The game's own FSR 3 (the chosen one does not run: the reason says why).</summary>
    GameFsr3 = 2,
    AmdFsr4 = 3,
    AmdFsr31 = 4,
    Xess = 5,
    /// <summary>Not known: this kiln-ui.dll does not say (older than 0.2.0-dev.29.18).</summary>
    Unknown = -1,
}

/// <summary>Why the chosen upscaler does not run: <c>KILN_UI_UPSCALE_WHY_*</c> (kiln_ui.h; stable numbers, only ever
/// added to). One list for the Graphics page, the state file, the launcher and the docs (docs/kiln/graphics.md
/// "What runs now, and why not").</summary>
internal enum UpscalerReason
{
    /// <summary>It runs, or the game's own is chosen.</summary>
    None = 0,
    Restart = 1,
    FilesMissing = 2,
    FilesBad = 3,
    LoadFailed = 4,
    Device = 5,
    CreateFailed = 6,
    RuntimeFailed = 7,
    Fault = 8,
    Aside = 9,
    NoSignatures = 10,
    Inputs = 11,
    Unavailable = 12,
    Waiting = 13,
    NoVulkan = 14,
}

/// <summary>The upscaler part of a graphics status (kiln_ui.h's phase B fields, from offset 72): what runs, why the chosen
/// one does not (<see cref="Code"/> keeps a number this build has no name for), the flags, the tools Kiln steps aside
/// for (<c>KILN_UI_GFX_ASIDE_*</c>), the version that runs, the FSR 3 pass's sizes and kiln.ui's English detail.</summary>
internal sealed record UpscalerNow(UpscalerRunning Running, UpscalerReason Reason, uint Code = 0, uint Flags = 0, uint Aside = 0,
    uint Version = 0, uint RenderWidth = 0, uint RenderHeight = 0, uint OutputWidth = 0, uint OutputHeight = 0, string Detail = "")
{
    // KILN_UI_UPSCALE_* (KilnUiGraphicsStatus.upscale_flags)
    public const uint Hooked = 1, FsrSeen = 2, FilesOk = 4, DeviceOk = 8, FellBack = 16, AmdPinned = 32;

    /// <summary>"4.1.2" from <c>upscale_version</c>, null for 0.</summary>
    public string? VersionText => Version == 0 ? null
        : string.Create(CultureInfo.InvariantCulture, $"{Version >> 22}.{(Version >> 12) & 0x3ff}.{Version & 0xfff}");
}

/// <summary>The codes the state file, the launcher and the docs use for the enums above (camelCase).</summary>
internal static class UpscalerCodes
{
    public static string Choice(UpscalerChoice c) => c switch { UpscalerChoice.Amd => "amd", UpscalerChoice.Xess => "xess", _ => "game" };

    public static UpscalerChoice? ChoiceOf(string? s) => s switch
    {
        "game" => UpscalerChoice.Game, "amd" => UpscalerChoice.Amd, "xess" => UpscalerChoice.Xess, _ => null,
    };

    public static string Amd(AmdUpscalerMode m) => m == AmdUpscalerMode.Fsr3 ? "fsr3" : "auto";

    public static AmdUpscalerMode? AmdOf(string? s) => s switch { "auto" => AmdUpscalerMode.Auto, "fsr3" => AmdUpscalerMode.Fsr3, _ => null };

    public static string Running(UpscalerRunning r) => r switch
    {
        UpscalerRunning.Off => "off", UpscalerRunning.NotFsr => "notFsr", UpscalerRunning.GameFsr3 => "gameFsr3",
        UpscalerRunning.AmdFsr4 => "amdFsr4", UpscalerRunning.AmdFsr31 => "amdFsr31", UpscalerRunning.Xess => "xess", _ => "unknown",
    };

    /// <summary>Every reason but <see cref="UpscalerReason.None"/>, by its code (kiln_ui.h's KILN_UI_UPSCALE_WHY_* in
    /// camelCase; GraphicsTests checks the two lists against each other).</summary>
    public static readonly (UpscalerReason Reason, string Code)[] Reasons =
    [
        (UpscalerReason.Restart, "restart"), (UpscalerReason.FilesMissing, "filesMissing"), (UpscalerReason.FilesBad, "filesBad"),
        (UpscalerReason.LoadFailed, "loadFailed"), (UpscalerReason.Device, "device"), (UpscalerReason.CreateFailed, "createFailed"),
        (UpscalerReason.RuntimeFailed, "runtimeFailed"), (UpscalerReason.Fault, "fault"), (UpscalerReason.Aside, "aside"),
        (UpscalerReason.NoSignatures, "noSignatures"), (UpscalerReason.Inputs, "inputs"), (UpscalerReason.Unavailable, "unavailable"),
        (UpscalerReason.Waiting, "waiting"), (UpscalerReason.NoVulkan, "noVulkan"),
    ];

    /// <summary>A reason's code; null for none; "unknown" for a number this build has no name for (a newer kiln-ui.dll).</summary>
    public static string? Reason(UpscalerReason r) => r == UpscalerReason.None ? null
        : Reasons.Where(x => x.Reason == r).Select(x => x.Code).FirstOrDefault() ?? "unknown";

    /// <summary>A code's key in Kiln's texts (runtime/locales/en.json keys are snake_case): "files_missing".</summary>
    public static string Key(string code) => string.Concat(code.Select(ch => char.IsUpper(ch) ? "_" + char.ToLowerInvariant(ch) : ch.ToString()));

    /// <summary>The vendor folder of a choice (kiln/vendor/&lt;vendor&gt;), null for the game's own.</summary>
    public static string? Vendor(UpscalerChoice c) => c switch { UpscalerChoice.Amd => "amd", UpscalerChoice.Xess => "intel", _ => null };
}

/// <summary>The state of one vendor's files in <c>kiln/vendor/&lt;vendor&gt;/&lt;version&gt;/</c> as the Graphics page shows it.</summary>
internal enum VendorFilesState
{
    /// <summary>No file of this build's version is there.</summary>
    Missing = 0,
    /// <summary>Every file is there with the size this build pinned, with the launcher's receipt of the component (the
    /// game loads only such a folder, and checks the SHA-256 before it loads them).</summary>
    Present,
    /// <summary>Some are there, some are missing or of another size, or the receipt is missing (a download that did not
    /// finish: the game does not use the folder).</summary>
    Changed,
}

/// <summary>The vendor files this build knows (the launcher's manifest, embedded at build time: one source of truth) and
/// what of them is in the game's kiln/vendor/ folder.</summary>
internal static class VendorFiles
{
    internal sealed record Component(string Id, string Vendor, string Name, string Version, IReadOnlyList<(string Name, long Size)> Files);

    private static IReadOnlyList<Component>? _components;
    private static readonly object CacheLock = new();
    private static readonly Dictionary<string, (DateTime At, VendorFilesState State)> Cache = [];

    /// <summary>Tests: the kiln folder somewhere else.</summary>
    internal static string? RootOverride;

    internal static string Root => RootOverride ?? KilnPaths.Root;

    /// <summary>The manifest's components (launcher/src/data/vendor-components.json, embedded).</summary>
    internal static IReadOnlyList<Component> Components => _components ??= Load();

    private static IReadOnlyList<Component> Load()
    {
        try
        {
            using var s = typeof(VendorFiles).Assembly.GetManifestResourceStream("Kiln.vendor-components.json");
            if (s is null) return [];
            using var doc = JsonDocument.Parse(s);
            var list = new List<Component>();
            foreach (var c in doc.RootElement.GetProperty("components").EnumerateArray())
            {
                var files = c.GetProperty("files").EnumerateArray().Select(f => (f.GetProperty("name").GetString()!, f.GetProperty("size").GetInt64())).ToList();
                list.Add(new Component(c.GetProperty("id").GetString()!, c.GetProperty("vendor").GetString()!, c.GetProperty("name").GetString()!,
                    c.GetProperty("version").GetString()!, files));
            }
            return list;
        }
        catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException)
        {
            return [];
        }
    }

    /// <summary>The component of a vendor folder (amd, intel), or null.</summary>
    internal static Component? Of(string? vendor) => vendor is null ? null : Components.FirstOrDefault(c => c.Vendor == vendor);

    /// <summary>kiln/vendor/&lt;vendor&gt;/&lt;version&gt; relative to the game folder, forward slashes.</summary>
    internal static string Folder(Component c) => $"kiln/vendor/{c.Vendor}/{c.Version}";

    /// <summary>What is in the component's folder now (looked at once every 2 seconds at most).</summary>
    internal static VendorFilesState State(Component c)
    {
        lock (CacheLock)
        {
            if (Cache.TryGetValue(c.Id, out var hit) && DateTime.UtcNow - hit.At < TimeSpan.FromSeconds(2)) return hit.State;
            var st = Look(c);
            Cache[c.Id] = (DateTime.UtcNow, st);
            return st;
        }
    }

    /// <summary>The launcher's receipt in a version folder (kiln_manage::vendor::store::RECEIPT).</summary>
    internal const string Receipt = "kiln-vendor.json";

    internal static VendorFilesState Look(Component c)
    {
        var dir = Path.Combine(Root, "vendor", c.Vendor, c.Version);
        int there = 0, right = 0;
        foreach (var (name, size) in c.Files)
        {
            try
            {
                var f = new FileInfo(Path.Combine(dir, name));
                if (!f.Exists) continue;
                there++;
                if (f.Length == size) right++;
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        if (right == c.Files.Count && c.Files.Count > 0) return HasReceipt(dir, c) ? VendorFilesState.Present : VendorFilesState.Changed;
        return there == 0 ? VendorFilesState.Missing : VendorFilesState.Changed;
    }

    /// <summary>The folder has the launcher's receipt of this component, format 1: what the game asks of a folder before
    /// it checks the files (native/services/ui/vendor_file.h; the launcher writes it last, after every file verified).</summary>
    private static bool HasReceipt(string dir, Component c)
    {
        try
        {
            var f = new FileInfo(Path.Combine(dir, Receipt));
            if (!f.Exists || f.Length > 64 * 1024) return false;
            using var doc = JsonDocument.Parse(File.ReadAllBytes(f.FullName));
            var r = doc.RootElement;
            return r.ValueKind == JsonValueKind.Object && r.TryGetProperty("format", out var fmt) && fmt.ValueKind == JsonValueKind.Number &&
                fmt.TryGetInt32(out var n) && n == 1 && r.TryGetProperty("component", out var id) && id.ValueKind == JsonValueKind.String &&
                id.GetString() == c.Id;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return false;
        }
    }

    internal static void ResetForTests()
    {
        lock (CacheLock) Cache.Clear();
        RootOverride = null;
    }
}

/// <summary>kiln.ui's graphics status as the upscaler's: the one place that reads the phase B fields of
/// <c>KilnUiGraphicsStatus</c> (kiln_ui.h, from offset 72).</summary>
internal static unsafe class UpscalerStatus
{
    /// <summary>What runs behind the game's FSR 3 now, or null when this kiln-ui.dll does not say (it answered with the
    /// 72 bytes of phase A: its size field says so).</summary>
    internal static UpscalerNow? FromNative(in UiNative.GraphicsStatus st)
    {
        if (st.Size < UiNative.GraphicsStatusSizeV2) return null;
        var running = st.UpscaleRunning <= (uint)UpscalerRunning.Xess ? (UpscalerRunning)st.UpscaleRunning : UpscalerRunning.Unknown;
        var reason = Enum.IsDefined(typeof(UpscalerReason), (int)Math.Min(st.UpscaleReason, int.MaxValue)) ? (UpscalerReason)st.UpscaleReason
            : st.UpscaleReason == 0 ? UpscalerReason.None : (UpscalerReason)(-1);
        string detail;
        fixed (byte* d = st.UpscaleDetail)
        {
            var n = 0;
            while (n < UiNative.UpscaleDetailBytes && d[n] != 0) n++;
            detail = Encoding.UTF8.GetString(d, n);
        }
        return new UpscalerNow(running, reason, st.UpscaleReason, st.UpscaleFlags, st.AsideUpscale, st.UpscaleVersion, st.UpscaleRenderWidth,
            st.UpscaleRenderHeight, st.UpscaleOutputWidth, st.UpscaleOutputHeight, detail);
    }
}

/// <summary><c>kiln/state/graphics.json</c>: what Kiln's graphics tools do, for the launcher (Settings, Graphics: "Now"
/// while this game runs, "Last game" after). Format 1, schema/state-graphics.schema.json. Written atomically (a temporary
/// file, then a rename), and only when something in it changed: the frame rates are left out.</summary>
internal static class GraphicsStateFile
{
    private static readonly object WriteLock = new();
    private static string? _last;

    /// <summary>Tests: the file somewhere else.</summary>
    internal static string? PathOverride;

    internal static string FilePath => PathOverride ?? Path.Combine(KilnPaths.State, "graphics.json");

    /// <summary>The tools of <c>KILN_UI_GFX_ASIDE_*</c> bits by their state-file names ("optiscaler", "ngxPatched").</summary>
    internal static JsonArray Aside(uint bits) =>
        new([.. GraphicsPage.AsideKeys.Where(a => (bits & a.Bit) != 0).Select(a => (JsonNode)Camel(a.Key))]);

    private static string Camel(string snake)
    {
        var parts = snake.Split('_');
        return parts[0] + string.Concat(parts.Skip(1).Select(p => p.Length == 0 ? p : char.ToUpperInvariant(p[0]) + p[1..]));
    }

    /// <summary>The file's content without its time and pid (what decides whether it is written again).</summary>
    internal static JsonObject Body(GraphicsSettings s, UiNative.GraphicsStatus? status, UpscalerNow? now)
    {
        // no phase B fields (a kiln-ui.dll without them, or none): unknown, for the reason "unavailable"
        var up = new JsonObject
        {
            ["chosen"] = UpscalerCodes.Choice(s.Upscaler),
            ["amdUpscaler"] = UpscalerCodes.Amd(s.AmdUpscaler),
            ["running"] = UpscalerCodes.Running(now?.Running ?? UpscalerRunning.Unknown),
            ["reason"] = now is null ? "unavailable" : UpscalerCodes.Reason(now.Reason),
        };
        if (now is not null)
        {
            if (now.VersionText is { } v) up["version"] = v;
            up["hooked"] = (now.Flags & UpscalerNow.Hooked) != 0;
            up["fsrSeen"] = (now.Flags & UpscalerNow.FsrSeen) != 0;
            up["filesOk"] = (now.Flags & UpscalerNow.FilesOk) != 0;
            up["deviceOk"] = (now.Flags & UpscalerNow.DeviceOk) != 0;
            up["fellBack"] = (now.Flags & UpscalerNow.FellBack) != 0;
            up["amdPinned"] = (now.Flags & UpscalerNow.AmdPinned) != 0;
            up["aside"] = Aside(now.Aside);
            if (UpscalerCodes.Reason(now.Reason) == "unknown") up["reasonNumber"] = now.Code;
            if (now.Detail.Length > 0) up["detail"] = now.Detail;
        }
        var files = new JsonObject();
        foreach (var c in VendorFiles.Components)
            files[c.Vendor] = new JsonObject
            {
                ["version"] = c.Version,
                ["state"] = VendorFiles.State(c) switch { VendorFilesState.Present => "present", VendorFilesState.Changed => "changed", _ => "missing" },
            };
        var body = new JsonObject { ["upscaler"] = up, ["files"] = files };
        if (status is { } st)
        {
            body["frameCap"] = new JsonObject
            {
                ["fps"] = s.FpsCap, ["active"] = (st.Flags & GraphicsPage.CapActive) != 0, ["aside"] = Aside(st.AsideCap),
            };
            var dlss = new JsonObject
            {
                ["hooked"] = (st.Flags & GraphicsPage.DlssHooked) != 0, ["active"] = (st.Flags & GraphicsPage.DlssActive) != 0,
                ["restart"] = (st.Flags & GraphicsPage.DlssRestart) != 0, ["aside"] = Aside(st.AsideDlss),
            };
            if ((st.Flags & GraphicsPage.DlssActive) != 0)
            {
                dlss["mode"] = st.DlssQuality switch
                {
                    0 => "performance", 1 => "balanced", 2 => "quality", 3 => "ultraPerformance", 4 => "ultraQuality", 5 => "dlaa", _ => null,
                };
                dlss["preset"] = st.DlssPreset == 0 ? "default" : GraphicsPage.PresetName(st.DlssPreset);
            }
            body["dlss"] = dlss;
        }
        return body;
    }

    /// <summary>Writes the file when its body differs from the last one written by this process; true when it wrote.</summary>
    internal static bool Update(GraphicsSettings s, UiNative.GraphicsStatus? status, UpscalerNow? now)
    {
        var body = Body(s, status, now);
        var key = body.ToJsonString();
        lock (WriteLock)
        {
            if (key == _last) return false;
            var file = new JsonObject
            {
                ["format"] = 1,
                ["pid"] = Environment.ProcessId,
                ["written"] = DateTime.UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", CultureInfo.InvariantCulture),
                ["kiln"] = KilnVersion(),
            };
            foreach (var (k, v) in body.ToList())
            {
                body.Remove(k);
                file[k] = v;
            }
            Write(FilePath, file);
            _last = key;
            return true;
        }
    }

    private static string? KilnVersion()
    {
        try { return Game.KilnVersion; }
        catch (Exception e) when (e is InvalidOperationException or NullReferenceException) { return null; }  // no backend (tests)
    }

    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };

    internal static void Write(string path, JsonObject file)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var tmp = path + ".kiln-" + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            using (var f = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
            {
                f.Write(new UTF8Encoding(false).GetBytes(file.ToJsonString(Indented) + "\n"));
                f.Flush(flushToDisk: true);
            }
            File.Move(tmp, path, overwrite: true);
        }
        finally
        {
            if (File.Exists(tmp)) File.Delete(tmp);
        }
    }

    internal static void ResetForTests()
    {
        lock (WriteLock) _last = null;
        PathOverride = null;
    }
}
