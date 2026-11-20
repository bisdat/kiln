// SPDX-License-Identifier: MIT
// The kit's boot phase (chapter 08, Mod.OnBoot): runs a boot mod's OnBoot as Kiln.Host does at a start, on an instance of its own, with
// the staging of BootDefinitions and the lowering of Kiln.Blocks (BootPhase.LowerStaged), and records the outcome as the process's boot
// generation, so the mod then loads in the harness with its boot content counted as committed. The native kiln.boot is not modelled:
// its table answers KILN_ERR_UNAVAILABLE (no deferred mode), the game's data comes from what the test scripts.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.RegularExpressions;
using Kiln.Host;

namespace Kiln.TestKit;

/// <summary>One harness's boot phase: the game-data records the test scripted and the runs so far.</summary>
internal sealed class BootFakeState
{
    public readonly object Lock = new();
    public readonly Dictionary<string, BootDataValue> Records = new(StringComparer.Ordinal);
    public readonly Dictionary<string, BootModReport> Reports = new(StringComparer.Ordinal);
    public readonly Dictionary<string, IReadOnlyList<BootStaged>> Committed = new(StringComparer.Ordinal);
    public readonly List<BootRun> Runs = [];
}

internal sealed unsafe class BootFake : IKitFake
{
    internal const string Service = "kiln.boot";
    internal static readonly string[] Features = ["mods.boot", "mods.boot.game_data"];
    internal const string PhaseRow = "mods.boot.phase";
    private static KilnBootApi* _table;

    string IKitFake.Service => Service;

    public void Register()
    {
        // a table, so that Kiln.Blocks finds kiln.boot (it reports mods.boot offline without one); every call answers as a process
        // without a boot phase does
        KitBackend.Instance.RegisterTable(Service, Create());
        FakeTables.Features(Features);
        FakeTables.Session(PhaseRow, false, "no boot-author frame is open");
    }

    public void Start(KilnHarness harness) => BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());

    public void BeforeStep(KilnHarness harness, ulong step) { }

    public void End(KilnHarness harness) => BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Boot phase (Mod.OnBoot)", CoverageKind.RecordingFake,
        [Service, .. Features, PhaseRow],
        "KilnHarness.BootPhase.Run runs a boot mod's OnBoot as Kiln.Host does at a start: once, on an instance of its own, with BootDefinitions' " +
        "staging (canonical JSON, DuplicateDefinition, NotInBootKinds, PhaseClosed, Blocks.Define refused) and Kiln.Blocks' lowering of what it " +
        "staged (a definition that does not compile, BootReferenceOutside, ShadowedByFile), and records the outcome as the process's boot " +
        "generation: a committed mod then loads in the harness (its definitions' runtime parts, BootDefinitionsCommittedEvent), a failed one is " +
        "refused RestartRequiredBoot. boot.GameData answers from KilnHarness.BootPhase.SetRecord. Not modelled: the native kiln.boot (its table " +
        "answers unavailable), the collectors' prepare and the generation's registrations (the kit's fakes do not get the boot generation's " +
        "items, clones or nodes), several mods' drop and dependency rules, the exit code of a stopped start"));

    public static BootFakeState StateOf(KilnHarness harness) => KitFakes.State<BootFakeState>(harness);

    private static nint Create()
    {
        if (_table != null) return (nint)_table;
        var t = (KilnBootApi*)NativeMemory.AllocZeroed((nuint)sizeof(KilnBootApi));  // process lifetime, never freed
        t->Size = (uint)sizeof(KilnBootApi);
        t->Version = 1;
        t->Status = &Status;
        t->BeginOwner = &BeginOwner;
        t->Stage = &Stage;
        t->FinishOwner = &FinishOwner;
        t->ReadResource = &ReadResource;
        t->Commit = &Commit;
        t->OwnerState = &OwnerState;
        t->Issues = &Issues;
        t->Collector = &Collector;
        t->ActivePlan = &ActivePlan;
        _table = t;
        return (nint)t;
    }

    private const int Unavailable = 3;  // KILN_ERR_UNAVAILABLE: no deferred mode in this process

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Status(KilnBootCommitResultV1* r) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int BeginOwner(ulong o, KilnStr id, uint roles, ulong* gen) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Stage(ulong o, ulong gen, KilnStr kind, KilnStr key, KilnStr json, KilnBootStageResultV1* r) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int FinishOwner(ulong o, ulong gen, uint accept) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int ReadResource(ulong o, KilnStr query, byte* json, nuint capacity, nuint* required) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Commit(ulong host, ulong gen, KilnBootCommitResultV1* r) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int OwnerState(KilnStr id, KilnBootOwnerStateV1* s) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Issues(ulong gen, KilnBootIssueV1* issues, uint capacity, uint stride, uint* required) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Collector(ulong o, KilnStr id, void* fn, void* user) => Unavailable;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static void* ActivePlan(KilnStr id, ulong* gen) => null;

    /// <summary>OnBoot of the mod in <paramref name="directory"/>, as Kiln.Host's boot phase runs it (BootPhase.cs): the outcome, recorded.</summary>
    internal static BootRun Run(KilnHarness harness, string directory, bool commit)
    {
        var state = StateOf(harness);
        var m = Manifest.Read(directory);
        if (m.Boot == ManifestBoot.None)
            throw new InvalidOperationException($"mod {m.Id} has no \"boot\" in its kiln.mod.json: it has no boot phase (OnBoot is not called)");
        var mods = harness.Manager;
        var data = new KitGameData(state);
        var bootOn = Capabilities.Is("mods.boot");
        var boot = new BootContext(m.Id, 1, mods.BootRoles(m), data) { BootOn = bootOn };
        data.Frame = boot;
        var issues = new List<BootIssue>();
        var lowered = new List<string>();
        BootModState outcome;
        string? reason = null;
        var owner = mods.Host.OwnerCreate("boot-author:" + m.Id);
        FakeTables.Session(PhaseRow, true, "");
        try
        {
            if (!bootOn)
            {
                outcome = BootModState.ExcludedCallback;
                reason = $"mods.boot is off: {Capabilities.Why("mods.boot")}; its OnBoot does not run";
                issues.Add(new BootIssue(m.Id, "", "", "callback", "BootCapOff", reason));
            }
            else if (!mods.RunBootAuthor(m, directory, owner, boot, out var why)
                     || (boot.Staged.Count > 0 && Lower(m, directory, boot, issues, lowered, out why) == false))
            {
                outcome = BootModState.ExcludedCallback;
                reason = why;
                issues.Add(new BootIssue(m.Id, "", "", "callback", "CallbackFailed", why));
            }
            else outcome = commit ? BootModState.Committed : BootModState.Staged;
        }
        finally
        {
            boot.Open = false;
            FakeTables.Session(PhaseRow, false, "no boot-author frame is open");
            mods.Host.OwnerRevoke(owner);
        }
        var staged = boot.Staged.Select(s => new BootRunDefinition(s.Kind, s.Key, s.Json)).ToList();
        var ok = outcome is BootModState.Committed or BootModState.Staged;
        var run = new BootRun(m.Id, outcome, reason, staged, ok ? lowered : [], issues);
        lock (state.Lock)
        {
            state.Runs.Add(run);
            if (commit)
            {
                state.Reports[m.Id] = new BootModReport(m.Id, 1, outcome, run.Definitions, issues);
                if (ok) state.Committed[m.Id] = boot.Staged.Where(s => lowered.Contains($"{s.Kind}/{s.Key}")).ToList();
                else state.Committed.Remove(m.Id);
                BootGeneration.Record(1, state.Reports.Values, state.Committed);
            }
        }
        if (commit && ok) mods.SetBootInputs(m.Id, mods.ReadBootInputs(directory));
        return run;
    }

    private static bool Lower(Manifest m, string directory, BootContext boot, List<BootIssue> issues, List<string> lowered, out string why)
    {
        if (BootPhase.LowerStaged(m, directory, boot.Staged, out why) is not { } answer) return false;
        foreach (var issue in answer["issues"]!.AsArray())
            issues.Add(new BootIssue(m.Id, (string)issue!["kind"]!, (string)issue["key"]!, "freeze", (string)issue["code"]!, (string)issue["reason"]!));
        lowered.AddRange(answer["definitions"]!.AsArray().Select(d => (string)d!));
        return true;
    }

    /// <summary>boot.GameData in the kit: the records the test scripted (KilnHarness.BootPhase.SetRecord), else NotFound.
    /// <c>resource:&lt;GUID&gt;/&lt;qualified type name&gt;/&lt;part&gt;</c> is the hash form (the name's FNV-1a); <c>names:resources/&lt;type&gt;</c>
    /// lists the records scripted under <c>resource:</c> keys of that type, and the <c>item:</c>/<c>template:</c> records whose
    /// resourceId and typeHash name a resource of it (with their names).</summary>
    internal sealed class KitGameData(BootFakeState state) : IReadOnlyGameData
    {
        private static readonly Regex Query = new(@"^(item:.+|template:.+|skill:0x[0-9a-fA-F]{8}|material:.+|names:(items|templates|skills|materials|templates/[A-Za-z0-9_]{1,96}|resources/(?=[A-Za-z0-9_:]*::)[A-Za-z0-9_:]{1,160})|resource:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}/([0-9a-fA-F]{8}|(?=[A-Za-z0-9_:]*::)[A-Za-z0-9_:]{1,160})/[0-9]+)$",
            RegexOptions.CultureInvariant);

        public BootContext? Frame { get; set; }

        public BootDataResult Read(string key)
        {
            ArgumentNullException.ThrowIfNull(key);
            if (Frame is { Open: false }) return new BootDataResult(false, "PhaseClosed: the boot-author frame is closed; the game's data is read inside OnBoot only", null);
            if (FakeTables.Off("mods.boot.game_data")) throw new KilnUnavailableException("mods.boot.game_data", "switched off by the test");
            if (!Query.IsMatch(key)) throw new ArgumentException($"\"{key}\" is not a game-data query (item:, template:, skill:0x..., material:, resource:, names:<kind>)", nameof(key));
            lock (state.Lock)
            {
                if (key.StartsWith("names:resources/", StringComparison.Ordinal) && !state.Records.ContainsKey(key))
                    return ResourceNames(key["names:resources/".Length..]);
                if (key.StartsWith("resource:", StringComparison.Ordinal) && !state.Records.ContainsKey(key))
                {
                    var want = ParseResource(key)!.Value;
                    foreach (var (k, rec) in state.Records)
                        if (ParseResource(k) is { } got && got == want) return new BootDataResult(true, null, rec);
                }
                if (key.StartsWith("names:", StringComparison.Ordinal) && !state.Records.ContainsKey(key))
                {
                    // names:<kind>: the names of the records the test scripted for that kind, sorted
                    var what = key[6..];
                    var component = what.StartsWith("templates/", StringComparison.Ordinal) ? what[10..] : null;
                    var prefix = (component != null ? "templates" : what)[..^1] + ":";
                    // names:templates/<component>: the template's own components only, as kiln.boot answers (a variant of that type
                    // nested in a component's data is not one)
                    static bool HasComponent(JsonElement record, string component) =>
                        record.ValueKind == JsonValueKind.Object && record.TryGetProperty("components", out var list) && list.ValueKind == JsonValueKind.Array &&
                        list.EnumerateArray().Any(c => c.ValueKind == JsonValueKind.Object && c.TryGetProperty("$type", out var t) &&
                                                       t.ValueKind == JsonValueKind.String && t.GetString() == component);
                    var names = state.Records.Where(r => r.Key.StartsWith(prefix, StringComparison.Ordinal))
                        .Where(r => component == null || HasComponent(r.Value.Value, component))
                        .Select(r => r.Key[prefix.Length..]).Order(StringComparer.Ordinal).ToArray();
                    using var doc = System.Text.Json.JsonDocument.Parse(System.Text.Json.JsonSerializer.Serialize(new { names }));
                    return new BootDataResult(true, null, new BootDataValue(Guid.Empty, 0, 0, doc.RootElement.Clone()));
                }
                return state.Records.TryGetValue(key, out var v)
                    ? new BootDataResult(true, null, v)
                    : new BootDataResult(false, $"NotFound: no record {key} in the game's data (the test scripted none: KilnHarness.BootPhase.SetRecord)", null);
            }
        }

        /// <summary>32-bit FNV-1a of the name's UTF-8 bytes: the game's type hash of a qualified type name.</summary>
        internal static uint TypeHash(string name)
        {
            var h = 0x811C9DC5u;
            foreach (var b in System.Text.Encoding.UTF8.GetBytes(name)) h = (h ^ b) * 0x01000193u;
            return h;
        }

        // resource:<GUID>/<8 hex or qualified name>/<part> as (GUID, type hash, part); null for another key
        private static (Guid Id, uint Type, long Part)? ParseResource(string key)
        {
            if (!key.StartsWith("resource:", StringComparison.Ordinal)) return null;
            var parts = key["resource:".Length..].Split('/');
            if (parts.Length != 3 || !Guid.TryParse(parts[0], out var id) || !long.TryParse(parts[2], System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var part))
                return null;
            var type = parts[1].Length == 8 && uint.TryParse(parts[1], System.Globalization.NumberStyles.HexNumber, System.Globalization.CultureInfo.InvariantCulture, out var h) ? h : TypeHash(parts[1]);
            return (id, type, part);
        }

        // names:resources/<type>, as kiln.boot answers it: {"names", "resources"} sorted by (name, resourceId, part)
        private BootDataResult ResourceNames(string type)
        {
            var hash = TypeHash(type);
            var found = new Dictionary<(Guid, long), string>();
            foreach (var (k, v) in state.Records)
            {
                if (ParseResource(k) is { } r && r.Type == hash) found.TryAdd((r.Id, r.Part), "");
                else if ((k.StartsWith("item:", StringComparison.Ordinal) || k.StartsWith("template:", StringComparison.Ordinal)) && v.TypeHash == hash && v.ResourceId != Guid.Empty)
                    found[(v.ResourceId, v.Part)] = k[(k.IndexOf(':') + 1)..];
            }
            if (found.Count == 0)
                return new BootDataResult(false, $"NotFound: no resource of {type} in the game's data (the test scripted none: KilnHarness.BootPhase.SetRecord under resource:)", null);
            var rows = found.Select(f => (Id: f.Key.Item1.ToString("D"), Part: f.Key.Item2, Name: f.Value))
                .OrderBy(r => r.Name, StringComparer.Ordinal).ThenBy(r => r.Id, StringComparer.Ordinal).ThenBy(r => r.Part).ToArray();
            var value = new
            {
                names = rows.Select(r => r.Name.Length > 0 ? r.Name : r.Id).Distinct().Order(StringComparer.Ordinal).ToArray(),
                resources = rows.Select(r => new { resourceId = r.Id, part = r.Part, name = r.Name }).ToArray(),
            };
            using var doc = JsonDocument.Parse(JsonSerializer.Serialize(value));
            return new BootDataResult(true, null, new BootDataValue(Guid.Empty, hash, 0, doc.RootElement.Clone()));
        }
    }
}
