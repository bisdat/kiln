// SPDX-License-Identifier: MIT
// A boot mod's frozen inputs (chapter 08 §4.4, fix round 08-onboot.handles): after its generation committed, a runtime load of the mod
// compares what the boot phase read for it (manifest, assemblies, blocks/, assets/, quests/, skills/) and refuses a changed mod with
// RestartRequiredBoot, a message that names the change and says to restart the game. A rewrite that leaves a JSON value as it was, the
// launcher's markers and the mod's data folder are not changes.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Kiln.Host;
using Xunit;

namespace Kiln.Host.Tests;

[Collection("boot generation")]  // BootGeneration is process-wide
public sealed class BootInputsTests : ModsFixture
{
    private const string Boot = """{ "id": "boot-inputs", "name": "g", "version": "1.0.0", "apiVersion": "0.2", "side": "both", "boot": true }""";

    /// <summary>A file the fake kiln.boot writes while it commits (a change between OnBoot and the commit); null: none.</summary>
    private static string? _editDuringCommit;

    private static readonly List<nint> _tables = [];

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Collecting(KilnBootCommitResultV1* result) { result->Status = BootPhase.StatusCollecting; result->Generation = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Begin(ulong owner, KilnStr id, uint roles, ulong* generation) { *generation = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int Finish(ulong owner, ulong generation, uint accept) => KilnAbi.Ok;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Accept(ulong owner, ulong gen, KilnStr kind, KilnStr key, KilnStr json, KilnBootStageResultV1* result) { result->Ok = 1; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Publish(ulong owner, ulong gen, KilnBootCommitResultV1* result)
    {
        if (_editDuringCommit is { } file) File.WriteAllText(file, """{ "statuses": { "late": { "name": "Late", "seconds": 2 } } }""");
        result->Status = BootPhase.StatusPublished;
        result->Generation = 1;
        return KilnAbi.Ok;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int Committed(KilnStr id, KilnBootOwnerStateV1* result) { result->State = (uint)BootModState.Committed; return KilnAbi.Ok; }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int NoIssues(ulong gen, KilnBootIssueV1* issues, uint capacity, uint stride, uint* required) { *required = 0; return KilnAbi.Ok; }

    /// <summary>A boot phase over the mods folder through a fake kiln.boot that publishes: the mod's inputs are frozen as a start freezes them.</summary>
    private unsafe ModManager Start(FakeHost host)
    {
        var mods = NewManager(host);
        // the table outlives this call: Mods.BootState reads the native state through it later (BootGeneration.ReadState)
        var table = (KilnBootApi*)NativeMemory.AllocZeroed((nuint)sizeof(KilnBootApi));
        *table = new KilnBootApi { Size = (uint)sizeof(KilnBootApi), Version = 1, Status = &Collecting, BeginOwner = &Begin, FinishOwner = &Finish,
            Stage = &Accept, Commit = &Publish, OwnerState = &Committed, Issues = &NoIssues };
        _tables.Add((nint)table);
        var address = (nint)table;
        Capabilities.Report("mods.boot", CapabilityState.Available, CapabilityReason.None);
        BootPhase.Run((_, _) => address, 1, mods, host.Log);
        Assert.Equal(BootModState.Committed, Mods.BootState("boot-inputs").State);
        return mods;
    }

    private string Mod()
    {
        var dir = WriteManifest("boot-inputs", Boot);
        Directory.CreateDirectory(Path.Combine(dir, "blocks"));
        File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), """{ "statuses": { "cold": { "name": "Cold", "seconds": 4 } } }""");
        File.WriteAllBytes(Path.Combine(dir, "Helper.dll"), [1, 2, 3]);
        return dir;
    }

    private static unsafe void Reset()
    {
        _editDuringCommit = null;
        BootGeneration.ReadState = null;  // no reader over a table that goes
        foreach (var t in _tables) NativeMemory.Free((void*)t);
        _tables.Clear();
        Capabilities.Reset();
        BootGeneration.Record(0, [], new Dictionary<string, IReadOnlyList<BootStaged>>());
    }

    [Fact]
    public void BOOT_INPUTS_an_edited_or_added_block_file_needs_a_restart_and_the_message_says_so()
    {
        var dir = Mod();
        try
        {
            using var mods = Start(new FakeHost());
            Assert.Null(mods.BootLoadRefusal(Manifest.Read(dir), dir));  // unchanged: the runtime instance loads (again)

            File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), """{ "statuses": { "cold": { "name": "Cold", "seconds": 9 } } }""");
            var why = mods.BootLoadRefusal(Manifest.Read(dir), dir);
            Assert.StartsWith("RestartRequiredBoot:", why);
            Assert.Contains("blocks/cold.json", why);
            Assert.Contains("restart the game", why);

            File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), """{ "statuses": { "cold": { "name": "Cold", "seconds": 4 } } }""");
            Assert.Null(mods.BootLoadRefusal(Manifest.Read(dir), dir));  // back as committed
            File.WriteAllText(Path.Combine(dir, "blocks", "warm.json"), """{ "statuses": { "warm": { "name": "Warm", "seconds": 4 } } }""");
            Assert.Contains("blocks/warm.json", mods.BootLoadRefusal(Manifest.Read(dir), dir));
        }
        finally { Reset(); }
    }

    [Fact]
    public void BOOT_INPUTS_a_helper_assembly_skills_or_assets_file_is_an_input_too()
    {
        var dir = Mod();
        try
        {
            using var mods = Start(new FakeHost());
            File.WriteAllBytes(Path.Combine(dir, "Helper.dll"), [1, 2, 4]);
            Assert.StartsWith("RestartRequiredBoot:", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            File.WriteAllBytes(Path.Combine(dir, "Helper.dll"), [1, 2, 3]);
            Directory.CreateDirectory(Path.Combine(dir, "skills"));
            File.WriteAllText(Path.Combine(dir, "skills", "tree.json"), "{}");
            Assert.Contains("skills/tree.json", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            File.Delete(Path.Combine(dir, "skills", "tree.json"));
            Directory.CreateDirectory(Path.Combine(dir, "assets", "cooked"));
            File.WriteAllBytes(Path.Combine(dir, "assets", "cooked", "icon.dds"), [9]);
            Assert.Contains("assets/cooked/icon.dds", mods.BootLoadRefusal(Manifest.Read(dir), dir));
        }
        finally { Reset(); }
    }

    [Fact]
    public void BOOT_INPUTS_a_rewrite_that_keeps_the_values_markers_and_files_outside_the_inputs_are_no_change()
    {
        var dir = Mod();
        try
        {
            using var mods = Start(new FakeHost());
            // the same values, written again by a tool: other white space, a comment, CRLF, a byte-order mark
            File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), "﻿{\r\n  // the cold\r\n  \"statuses\": {\r\n    \"cold\": { \"name\": \"Cold\", \"seconds\": 4 },\r\n  }\r\n}\r\n");
            File.WriteAllText(Path.Combine(dir, Manifest.FileName), Boot.Replace(", ", ",\n  "));
            // the launcher's markers, a temporary file, the mod's data folder, files the deploy leaves out
            File.WriteAllText(Path.Combine(dir, ".kiln-source.json"), "{}");
            File.WriteAllText(Path.Combine(dir, "blocks", ".cold.json.swp"), "x");
            File.WriteAllText(Path.Combine(dir, "blocks", "cold.json.kiln-tmp"), "x");
            Directory.CreateDirectory(Path.Combine(DataDir, "boot-inputs"));
            File.WriteAllText(Path.Combine(DataDir, "boot-inputs", "from-boot.txt"), "x");
            // Kiln's shared assemblies beside the mod (the loader takes them from the host; a deploy leaves them out)
            File.WriteAllBytes(Path.Combine(dir, "Kiln.Api.dll"), [7]);
            File.WriteAllBytes(Path.Combine(dir, "Kiln.Host.dll"), [7]);
            File.WriteAllText(Path.Combine(dir, "Kiln.Host.pdb"), "x");
            File.WriteAllText(Path.Combine(dir, "Helper.deps.json"), "{}");
            Assert.Null(mods.BootLoadRefusal(Manifest.Read(dir), dir));

            // a value that changed in the manifest is a change (its key order too: kept, to fail closed)
            File.WriteAllText(Path.Combine(dir, Manifest.FileName), Boot.Replace("\"name\": \"g\"", "\"name\": \"h\""));
            Assert.Contains("kiln.mod.json", mods.BootLoadRefusal(Manifest.Read(dir), dir));
        }
        finally { Reset(); }
    }

    [Fact]
    public void BOOT_INPUTS_any_other_file_of_the_mod_folder_counts_too_as_OnBoot_may_read_it()
    {
        var dir = Mod();
        try
        {
            using var mods = Start(new FakeHost());
            Directory.CreateDirectory(Path.Combine(dir, "tables"));
            File.WriteAllText(Path.Combine(dir, "tables", "odds.csv"), "a,1");
            Assert.Contains("tables/odds.csv added", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            File.Delete(Path.Combine(dir, "tables", "odds.csv"));
            File.WriteAllText(Path.Combine(dir, "readme.txt"), "x");
            Assert.Contains("readme.txt added", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            File.Delete(Path.Combine(dir, "readme.txt"));
            Assert.Null(mods.BootLoadRefusal(Manifest.Read(dir), dir));
        }
        finally { Reset(); }
    }

    [Fact]
    public void A_second_reading_hashes_only_what_changed()
    {
        var dir = Mod();
        var old = DateTime.UtcNow.AddMinutes(-5);
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)) File.SetLastWriteTimeUtc(f, old);
        BootInputs.ForgetDigests();
        var first = BootInputs.Read(dir);
        Assert.Equal(first.Cost.Files, first.Cost.Hashed);
        var second = BootInputs.Read(dir);
        Assert.Equal((first.Hash, 0), (second.Hash, second.Cost.Hashed));
        // an edit (a new write time) is hashed again; a file written just now is never taken from the cache
        File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), """{ "statuses": { "cold": { "name": "Cold", "seconds": 7 } } }""");
        var third = BootInputs.Read(dir);
        Assert.Equal(1, third.Cost.Hashed);
        Assert.Equal(["blocks/cold.json changed"], third.Differences(first));
        File.WriteAllText(Path.Combine(dir, "blocks", "cold.json"), """{ "statuses": { "cold": { "name": "Cold", "seconds": 8 } } }""");
        Assert.Equal(["blocks/cold.json changed"], BootInputs.Read(dir).Differences(third));  // same size, within the time stamp's grain
    }

    [Fact]
    public void A_file_that_is_not_JSON_counts_by_its_bytes_and_the_reading_never_throws()
    {
        var dir = Mod();
        var file = Path.Combine(dir, "blocks", "odd.json");
        File.WriteAllText(file, """{ "statuses": {}, "statuses": {} }""");  // a duplicate key
        var before = BootInputs.Read(dir);
        Assert.Null(before.Error);
        Assert.Equal(before.Hash, BootInputs.Read(dir).Hash);
        File.WriteAllText(file, "not json");
        var odd = BootInputs.Read(dir);
        Assert.Null(odd.Error);
        File.WriteAllText(file, "not  json");  // its bytes count: one space more is a change
        Assert.Equal(["blocks/odd.json changed"], BootInputs.Read(dir).Differences(odd));
        Assert.Empty(BootInputs.Read(Path.Combine(dir, "no-such-folder")).Files);
    }

    [Fact]
    public void BOOT_INPUTS_a_mod_that_changed_while_the_boot_phase_ran_needs_a_restart_whatever_it_looks_like_later()
    {
        var dir = Mod();
        var late = Path.Combine(dir, "blocks", "late.json");
        _editDuringCommit = late;
        try
        {
            var host = new FakeHost();
            using var mods = Start(host);
            _editDuringCommit = null;
            Assert.StartsWith("RestartRequiredBoot:", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            File.Delete(late);  // as OnBoot saw it: still not what the collectors read
            Assert.StartsWith("RestartRequiredBoot:", mods.BootLoadRefusal(Manifest.Read(dir), dir));
            Assert.True(host.Has("changed while the boot phase ran"), host.Dump());
        }
        finally { Reset(); }
    }
}
