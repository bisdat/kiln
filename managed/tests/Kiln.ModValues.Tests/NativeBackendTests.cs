// SPDX-License-Identifier: MIT
// The managed side of the native facade kiln.mod-values (NativeBackend.cs): the records against the headers' static
// asserts (the public native/core/abi/services/kiln_mod_values.h and the Kiln-internal native/core/src/mod_values_host.h,
// read from the source tree), the backend table taken only at exactly v1, the owner token, the RFC 4122 GUID byte order, the
// reason and event-kind numbers, and the backend's calls on a real ledger (in-memory store): the one error rule
// across the boundary, Before/After, compare-and-set with the world GUID, definitions from their native record.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Kiln;
using Xunit;

namespace ModValuesTests;

public sealed unsafe class NativeBackendTests
{
    /// <summary>UTF-8 strings in native memory for the records (freed at Dispose).</summary>
    private sealed class Pins : IDisposable
    {
        private readonly List<IntPtr> _blocks = [];

        public NativeStr Str(string s)
        {
            var bytes = Encoding.UTF8.GetBytes(s);
            return Bytes(bytes);
        }

        public NativeStr Bytes(byte[] bytes)
        {
            var p = (byte*)NativeMemory.Alloc((nuint)Math.Max(1, bytes.Length));
            _blocks.Add((IntPtr)p);
            bytes.CopyTo(new Span<byte>(p, bytes.Length));
            return new NativeStr { Ptr = p, Len = (nuint)bytes.Length };
        }

        public T* Many<T>(params T[] items) where T : unmanaged
        {
            var p = (T*)NativeMemory.Alloc((nuint)(Math.Max(1, items.Length) * sizeof(T)));
            _blocks.Add((IntPtr)p);
            for (var i = 0; i < items.Length; i++) p[i] = items[i];
            return p;
        }

        public void Dispose()
        {
            foreach (var b in _blocks) NativeMemory.Free((void*)b);
        }
    }

    private static string HeaderPath([CallerFilePath] string here = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(here)!, "../../../native/core/abi/services/kiln_mod_values.h"));

    /// <summary>The Kiln-internal backend table (never in core/abi: only Kiln.ModValues and the core use it).</summary>
    private static string HostHeaderPath([CallerFilePath] string here = "") =>
        Path.GetFullPath(Path.Combine(Path.GetDirectoryName(here)!, "../../../native/core/src/mod_values_host.h"));

    private static long Off(void* field, void* rec) => (byte*)field - (byte*)rec;

    // ------------------------------------------------------------------------------------------- the records ----

    private static Dictionary<string, long> ManagedLayout()
    {
        var m = new Dictionary<string, long>(StringComparer.Ordinal);
        var v = default(ModValuesAbi.VersionRec);
        m["sizeof KilnModValueVersion"] = sizeof(ModValuesAbi.VersionRec);
        m["KilnModValueVersion.revision"] = Off(&v.Revision, &v);
        var c = default(ModValuesAbi.CharacterRec);
        m["sizeof KilnModCharacter"] = sizeof(ModValuesAbi.CharacterRec);
        m["KilnModCharacter.guid_rfc4122"] = Off(c.Guid, &c);
        var d = default(ModValuesAbi.DefinitionRec);
        m["sizeof KilnModValueDef"] = sizeof(ModValuesAbi.DefinitionRec);
        m["KilnModValueDef.kind"] = Off(&d.Kind, &d);
        m["KilnModValueDef.persistence"] = Off(&d.Persistence, &d);
        m["KilnModValueDef.scale"] = Off(&d.Scale, &d);
        m["KilnModValueDef.flags"] = Off(&d.Flags, &d);
        m["KilnModValueDef.version"] = Off(&d.Version, &d);
        m["KilnModValueDef.carry_epoch"] = Off(&d.CarryEpoch, &d);
        m["KilnModValueDef.read_policy"] = Off(&d.ReadPolicy, &d);
        m["KilnModValueDef.sync_policy"] = Off(&d.SyncPolicy, &d);
        m["KilnModValueDef.reserved0"] = Off(&d.Reserved0, &d);
        m["KilnModValueDef.local_id"] = Off(&d.LocalId, &d);
        m["KilnModValueDef.label_json"] = Off(&d.LabelJson, &d);
        m["KilnModValueDef.number_format_json"] = Off(&d.NumberFormatJson, &d);
        m["KilnModValueDef.minimum"] = Off(&d.Minimum, &d);
        m["KilnModValueDef.maximum"] = Off(&d.Maximum, &d);
        m["KilnModValueDef.initial"] = Off(&d.Initial, &d);
        m["KilnModValueDef.xp_thresholds"] = Off(&d.Thresholds, &d);
        m["KilnModValueDef.threshold_count"] = Off(&d.ThresholdCount, &d);
        m["KilnModValueDef.reserved1"] = Off(&d.Reserved1, &d);
        m["KilnModValueDef.read_mods"] = Off(&d.ReadMods, &d);
        m["KilnModValueDef.read_mod_count"] = Off(&d.ReadModCount, &d);
        m["KilnModValueDef.reserved2"] = Off(&d.Reserved2, &d);
        var val = default(ModValuesAbi.ValueRec);
        m["sizeof KilnModValue"] = sizeof(ModValuesAbi.ValueRec);
        m["KilnModValue.level"] = Off(&val.Level, &val);
        m["KilnModValue.definition_version"] = Off(&val.DefinitionVersion, &val);
        m["KilnModValue.carry_epoch"] = Off(&val.CarryEpoch, &val);
        m["KilnModValue.value"] = Off(&val.Value, &val);
        m["KilnModValue.value_revision"] = Off(&val.ValueRevision, &val);
        m["KilnModValue.partition_revision"] = Off(&val.PartitionRevision, &val);
        m["KilnModValue.world_rfc4122"] = Off(val.World, &val);
        var r = default(ModValuesAbi.ResultRec);
        m["sizeof KilnModValueResult"] = sizeof(ModValuesAbi.ResultRec);
        m["KilnModValueResult.flags"] = Off(&r.Flags, &r);
        m["KilnModValueResult.character"] = Off(&r.Character, &r);
        m["KilnModValueResult.before"] = Off(&r.Before, &r);
        m["KilnModValueResult.after"] = Off(&r.After, &r);
        m["sizeof KilnModDefinitionResult"] = sizeof(ModValuesAbi.DefinitionResultRec);
        var ch = default(ModValuesAbi.ChangeRec);
        m["sizeof KilnModValueChange"] = sizeof(ModValuesAbi.ChangeRec);
        m["KilnModValueChange.request_id"] = Off(&ch.RequestId, &ch);
        m["KilnModValueChange.character"] = Off(&ch.Character, &ch);
        m["KilnModValueChange.qualified_key"] = Off(&ch.QualifiedKey, &ch);
        m["KilnModValueChange.result"] = Off(&ch.Result, &ch);
        var x = default(ModValuesAbi.ExportRec);
        m["sizeof KilnModValueExport"] = sizeof(ModValuesAbi.ExportRec);
        m["KilnModValueExport.read_mods"] = Off(&x.ReadMods, &x);
        var f = default(ModValuesAbi.BackendFns);
        m["sizeof KilnModValuesBackend"] = sizeof(ModValuesAbi.BackendFns);
        m["KilnModValuesBackend.describe"] = Off(&f.Describe, &f);
        m["KilnModValuesBackend.define"] = Off(&f.Define, &f);
        m["KilnModValuesBackend.read"] = Off(&f.Read, &f);
        m["KilnModValuesBackend.change"] = Off(&f.Change, &f);
        m["KilnModValuesBackend.read_synced"] = Off(&f.ReadSynced, &f);
        m["KilnModValuesBackend.retire"] = Off(&f.Retire, &f);
        var t = default(ModValuesAbi.BackendTable);
        m["sizeof KilnModValuesBackendApi"] = sizeof(ModValuesAbi.BackendTable);
        m["KilnModValuesBackendApi.bind"] = Off(&t.Bind, &t);
        m["KilnModValuesBackendApi.wanted"] = Off(&t.Wanted, &t);
        m["KilnModValuesBackendApi.dispatch"] = Off(&t.Dispatch, &t);
        return m;
    }

    [Fact]
    public void RecordsMatchTheHeadersStaticAsserts()
    {
        var pub = File.ReadAllText(HeaderPath());
        var host = File.ReadAllText(HostHeaderPath());
        // nothing host-only in the public header: the backend's types live in the Kiln-internal one
        foreach (var name in new[] { "KilnModValuesBackend", "KilnModValueExport", "KMV_WANT_", "kiln.mod-values.backend" })
            Assert.False(pub.Contains(name, StringComparison.Ordinal), $"kiln_mod_values.h names {name}");
        Assert.Contains("#define KILN_MOD_VALUES_BACKEND_SERVICE \"" + ModValuesAbi.BackendService + "\"", host);
        Assert.Contains("#define KILN_MOD_VALUES_BACKEND_VERSION " + ModValuesAbi.BackendVersion + "u", host);
        Assert.Equal("kiln.internal.mod-values.backend", ModValuesAbi.BackendService);
        var text = pub + "\n" + host;
        var managed = ManagedLayout();
        var checkedCount = 0;
        foreach (Match a in Regex.Matches(text, @"sizeof\((\w+)\) == (\d+)"))
        {
            if (a.Groups[1].Value == "KilnModValuesApi") continue;  // the mods' table: no managed mirror
            var key = "sizeof " + a.Groups[1].Value;
            Assert.True(managed.ContainsKey(key), $"{key} has no managed mirror");
            Assert.Equal((key, long.Parse(a.Groups[2].Value)), (key, managed[key]));
            checkedCount++;
        }
        foreach (Match a in Regex.Matches(text, @"offsetof\((\w+), (\w+)\) == (\d+)"))
        {
            if (a.Groups[1].Value == "KilnModValuesApi") continue;
            var key = a.Groups[1].Value + "." + a.Groups[2].Value;
            Assert.True(managed.ContainsKey(key), $"{key} has no managed mirror");
            Assert.Equal((key, long.Parse(a.Groups[3].Value)), (key, managed[key]));
            checkedCount++;
        }
        Assert.True(checkedCount >= 50, $"only {checkedCount} asserts read from the header");
        Assert.Equal(checkedCount, managed.Count);  // and nothing mirrored that the header does not pin
        Assert.Equal(24, Marshal.SizeOf<ModValuesAbi.CharacterRec>());
        Assert.Equal(144, Marshal.SizeOf<ModValuesAbi.DefinitionRec>());
        Assert.Equal(184, Marshal.SizeOf<ModValuesAbi.ResultRec>());
        Assert.Equal(248, Marshal.SizeOf<ModValuesAbi.ChangeRec>());
    }

    private static Dictionary<string, uint> HeaderEnum(string name)
    {
        var text = File.ReadAllText(HeaderPath());
        var start = text.IndexOf("enum " + name + " {", StringComparison.Ordinal);
        Assert.True(start >= 0, name);
        var body = text[start..text.IndexOf("};", start, StringComparison.Ordinal)];
        return Regex.Matches(body, @"KMV_(\w+) = (\d+)").ToDictionary(m => m.Groups[1].Value, m => uint.Parse(m.Groups[2].Value));
    }

    private static string Pascal(string snake) =>
        string.Concat(snake.Split('_').Select(w => w[..1] + w[1..].ToLowerInvariant()));

    [Fact]
    public void ReasonNumbersAreTheHeadersAndTheManagedOrder()
    {
        var header = HeaderEnum("KilnModValueReason");
        Assert.Equal(16, header.Count);
        Assert.Equal(15u, header["DEFINITION_CONFLICT"]);
        foreach (var (name, code) in header)
        {
            var managed = name == "NONE" ? "" : Pascal(name);
            Assert.Equal((name, (int)code), (name, Array.IndexOf(ModValueReasons.ByCode, managed)));
            if (code != 0) Assert.Equal(code, ModValueReasons.Code(managed));
        }
        Assert.Equal(header.Count, ModValueReasons.ByCode.Length);
        Assert.Equal(0u, ModValueReasons.Code(null));
        Assert.Equal(0u, ModValueReasons.Code("NotAReason"));
    }

    [Fact]
    public void EnumsAndEventKindsAreTheHeaders()
    {
        var kinds = HeaderEnum("KilnModEventKind");
        Assert.Equal(kinds["INITIALIZED"], ModValueNative(ModValueCauses.Initialized));
        Assert.Equal(kinds["CHANGED"], ModValueNative(ModValueCauses.Changed));
        Assert.Equal(kinds["MIGRATED"], ModValueNative(ModValueCauses.Migrated));
        Assert.Equal(kinds["IMPORTED"], ModValueNative(ModValueCauses.Imported));
        Assert.Equal(kinds["RESET_VALUE"], ModValueNative(ModValueCauses.Reset));
        Assert.Equal(kinds["EPOCH_RESET"], ModValueNative(ModValueCauses.EpochReset));
        Assert.Equal(kinds["AVAILABILITY_CHANGED"], ModValuesAbi.AvailabilityChanged);
        var kind = HeaderEnum("KilnModValueKind");
        Assert.Equal(((uint)ModValueKind.Counter, (uint)ModValueKind.Currency, (uint)ModValueKind.Experience), (kind["COUNTER"], kind["CURRENCY"], kind["EXPERIENCE"]));
        var read = HeaderEnum("KilnModReadPolicy");
        Assert.Equal(((uint)ModValueReadPolicy.OwnerOnly, (uint)ModValueReadPolicy.Public, (uint)ModValueReadPolicy.AllowList), (read["OWNER_ONLY"], read["PUBLIC_READ"], read["READ_LIST"]));
        var sync = HeaderEnum("KilnModSyncPolicy");
        Assert.Equal(((uint)ModValueSyncPolicy.None, (uint)ModValueSyncPolicy.OwnerMod, (uint)ModValueSyncPolicy.Readers), (sync["SYNC_NONE"], sync["SYNC_OWNER_MOD"], sync["SYNC_READERS"]));
        var ops = HeaderEnum("KilnModChangeOp");
        Assert.Equal(((uint)ChangeOp.Add, (uint)ChangeOp.Spend, (uint)ChangeOp.Set, (uint)ChangeOp.Reset), (ops["ADD"], ops["SPEND"], ops["SET"], ops["RESET"]));
        var flags = HeaderEnum("KilnModDefFlags");
        Assert.Equal((ModValuesAbi.TrustImports, ModValuesAbi.AllowPartitionMove, ModValuesAbi.ResetKindChange, ModValuesAbi.HasInitial),
            (flags["TRUST_IMPORTS"], flags["ALLOW_PARTITION_MOVE"], flags["RESET_KIND_CHANGE"], flags["HAS_INITIAL"]));

        static uint ModValueNative(string cause) => ModValuesNative.EventKind(cause);
    }

    // ------------------------------------------------------------------------------------------- GUID order ----

    [Fact]
    public void GuidsAreRfc4122NotToByteArray()
    {
        var g = Guid.Parse("00112233-4455-6677-8899-aabbccddeeff");
        byte* b = stackalloc byte[16];
        ModValuesNative.WriteGuid(g, b);
        Assert.Equal(Convert.FromHexString("00112233445566778899aabbccddeeff"), new Span<byte>(b, 16).ToArray());
        Assert.NotEqual(g.ToByteArray(), new Span<byte>(b, 16).ToArray());  // .NET's mixed-endian 33 22 11 00 55 44 ...
        Assert.Equal(g, ModValuesNative.GuidOf(b));
        var key = new CharacterKey(0x1111222233334444, g);
        var c = default(ModValuesAbi.CharacterRec);
        ModValuesNative.Write(&c, key);
        Assert.Equal(0x1111222233334444ul, c.Account);
        Assert.Equal(0x00, c.Guid[0]);
        Assert.Equal(0xff, c.Guid[15]);
        Assert.Equal(key, ModValuesNative.CharacterOf(&c));
    }

    // ----------------------------------------------------------------------------------------- calls on a ledger ----

    private static ModValuesAbi.VersionRec VersionOf(Guid world, ulong revision)
    {
        var v = default(ModValuesAbi.VersionRec);
        ModValuesNative.WriteGuid(world, v.World);
        v.Revision = revision;
        return v;
    }

    [Fact]
    public void ChangesAndReadsKeepTheErrorRuleAndCompareTheWorld()
    {
        using var pins = new Pins();
        var rig = new LedgerTests.Rig();
        rig.Define("fish", "coins", new ModValueOptions(ModValueKind.Currency, "Coins", Max: 1_000_000));
        rig.Define("fish", "xp", new ModValueOptions(ModValueKind.Experience, "XP", Thresholds: [0, 100]));
        rig.Define("fish", "c", new ModValueOptions(ModValueKind.Counter, "C", Min: -5, Max: 5));
        var who = rig.Join(1);
        var c = default(ModValuesAbi.CharacterRec);
        ModValuesNative.Write(&c, who);
        Func<Ledger> ledger = () => rig.Ledger;
        var fish = pins.Str("fish");
        var coins = pins.Str("fish:coins");
        var o = default(ModValuesAbi.ResultRec);

        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Add, 11, null, 7, &o));
        Assert.Equal((1u, 0u, ModValuesAbi.Before | ModValuesAbi.After), (o.Ok, o.Reason, o.Flags));
        Assert.Equal((0L, 11L, 0ul, 1ul), (o.Before.Value, o.After.Value, o.Before.ValueRevision, o.After.ValueRevision));
        Assert.Equal((uint)ModValueKind.Currency, o.After.Kind);
        Assert.Equal(rig.Store.World, ModValuesNative.GuidOf(o.After.World));
        Assert.Equal(who, ModValuesNative.CharacterOf(&o.Character));
        var changed = rig.Events.All.OfType<ModValueChangedEvent>().Last();
        Assert.Equal((7ul, ModValueCauses.Changed), (changed.RequestId, changed.Cause));

        // compare-and-set: the world GUID in RFC 4122 order and the value's revision
        var good = VersionOf(rig.Store.World, 1);
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Spend, 3, &good, 0, &o));
        Assert.Equal((1u, 8L, 2ul), (o.Ok, o.After.Value, o.After.ValueRevision));
        var stale = VersionOf(rig.Store.World, 1);
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Spend, 1, &stale, 0, &o));
        Assert.Equal((0u, 2u, 0u), (o.Ok, o.Reason, o.Flags));  // KMV_STALE_REVISION, no snapshot
        var mixed = default(ModValuesAbi.VersionRec);  // the right world in Guid.ToByteArray's order: another world
        rig.Store.World.ToByteArray().CopyTo(new Span<byte>(mixed.World, 16));
        mixed.Revision = 2;
        o = default;
        ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Spend, 1, &mixed, 0, &o);
        Assert.Equal((0u, 2u), (o.Ok, o.Reason));
        var xp = pins.Str("fish:xp");
        var zero = VersionOf(rig.Store.World, 0);  // revision 0 of a fresh value is valid
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, fish, &c, xp, (uint)ChangeOp.Add, 150, &zero, 0, &o));
        Assert.Equal((1u, 150L, 2u), (o.Ok, o.After.Value, o.After.Level));
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Spend, 100, null, 0, &o));
        Assert.Equal((0u, 1u), (o.Ok, o.Reason));  // KMV_INSUFFICIENT_FUNDS

        // reads: AFTER only; refusals with their numbers
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Read(ledger, fish, &c, coins, &o));
        Assert.Equal((1u, ModValuesAbi.After, 8L, 0L), (o.Ok, o.Flags, o.After.Value, o.Before.Value));
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Read(ledger, fish, &c, pins.Str("fish:nope"), &o));
        Assert.Equal((0u, 5u, 0u), (o.Ok, o.Reason, o.Flags));  // KMV_UNDEFINED
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Read(ledger, pins.Str("native.0123456789abcdef0123456789abcdef"), &c, coins, &o));
        Assert.Equal((0u, 6u), (o.Ok, o.Reason));  // KMV_ACCESS_DENIED: owner-only
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Change(ledger, pins.Str("other"), &c, coins, (uint)ChangeOp.Add, 1, null, 0, &o));
        Assert.Equal((0u, 6u), (o.Ok, o.Reason));  // another namespace never writes

        // invalid arguments: Kiln.ModValues' rules come back as INVALID_ARGUMENT, nothing changed
        var counter = pins.Str("fish:c");
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Change(ledger, fish, &c, counter, (uint)ChangeOp.Spend, 1, null, 0, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Change(ledger, fish, &c, coins, (uint)ChangeOp.Spend, 0, null, 0, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Change(ledger, fish, &c, xp, (uint)ChangeOp.Add, -1, null, 0, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Change(ledger, fish, &c, coins, 4, 1, null, 0, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Read(ledger, fish, &c, pins.Str("Fish:coins"), &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Read(ledger, fish, null, coins, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Read(ledger, pins.Str("Fish"), &c, coins, &o));
        var none = default(ModValuesAbi.CharacterRec);
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Read(ledger, fish, &none, coins, &o));
        Assert.Equal(8L, rig.Ledger.Read("fish", who, new ModValueKey("fish", "coins")).Value!.Value);

        // an account-only key is a domain answer
        var accountOnly = default(ModValuesAbi.CharacterRec);
        accountOnly.Account = 5;
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Read(ledger, fish, &accountOnly, coins, &o));
        Assert.Equal((0u, 3u), (o.Ok, o.Reason));  // KMV_CHARACTER_UNAVAILABLE

        // exceptions never cross: the capability, another failure
        Assert.Equal(ModValuesAbi.Unavailable, ModValuesNative.Read(() => throw new KilnUnavailableException("stats.own", "off"), fish, &c, coins, &o));
        Assert.Equal(ModValuesAbi.Internal, ModValuesNative.Read(() => throw new InvalidDataException("broken"), fish, &c, coins, &o));
        Assert.Equal(ModValuesAbi.Internal, ModValuesNative.Code(new InvalidOperationException("not the tick check")));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Code(new ArgumentNullException("x")));
    }

    [Fact]
    public void ReadSyncedNamesTheLocalCharacter()
    {
        using var pins = new Pins();
        var me = new CharacterKey(9, Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));
        var o = default(ModValuesAbi.ResultRec);
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.ReadSynced((_, _) => new ModValueReadResult(false, ModValueReasons.NotSynced, null), () => me,
            pins.Str("fish"), pins.Str("fish:coins"), &o));
        Assert.Equal((0u, 7u, 0u), (o.Ok, o.Reason, o.Flags));
        Assert.Equal(me, ModValuesNative.CharacterOf(&o.Character));
        string? reader = null;
        var snap = new ModValueSnapshot(Guid.NewGuid(), me, new ModValueKey("fish", "coins"), 42, 0, 2, ModValueKind.Currency, ModValuePersistence.World, 3, 1, 5, 6);
        o = default;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.ReadSynced((r, _) => { reader = r; return new ModValueReadResult(true, null, snap); }, () => default,
            pins.Str("native.0123456789abcdef0123456789abcdef"), pins.Str("fish:coins"), &o));
        Assert.Equal("native.0123456789abcdef0123456789abcdef", reader);
        Assert.Equal((1u, ModValuesAbi.After, 42L, 2u, 3u, 5ul, 6ul), (o.Ok, o.Flags, o.After.Value, o.After.Scale, o.After.DefinitionVersion, o.After.ValueRevision, o.After.PartitionRevision));
        Assert.Equal(snap.World, ModValuesNative.GuidOf(o.After.World));
        Assert.Equal(me, ModValuesNative.CharacterOf(&o.Character));
    }

    // ------------------------------------------------------------------------------------------- definitions ----

    private static ModValuesAbi.DefinitionRec Coins(Pins pins, string label = "{\"en\": \"Coins\", \"de\": \"Münzen\"}") => new()
    {
        Size = (uint)sizeof(ModValuesAbi.DefinitionRec), Kind = (uint)ModValueKind.Currency, Persistence = 0, Version = 1, CarryEpoch = 1,
        ReadPolicy = (uint)ModValueReadPolicy.OwnerOnly, SyncPolicy = (uint)ModValueSyncPolicy.OwnerMod,
        LocalId = pins.Str("coins"), LabelJson = pins.Str(label), Maximum = 1000,
    };

    [Fact]
    public void DefinitionsComeFromTheirRecord()
    {
        using var pins = new Pins();
        var rec = Coins(pins);
        var d = ModValuesNative.DefOf("fish", &rec);
        Assert.Equal(("fish", "coins", ModValueKind.Currency, 0L, 1000L, 0L, false), (d.Owner, d.Id, d.Kind, d.Min, d.Max, d.Initial, d.HasInitial));
        Assert.Equal("Münzen", d.Label["de"]);
        Assert.Equal("N0", d.NumberFormat["*"]);
        rec.Flags = ModValuesAbi.HasInitial;
        rec.Initial = 0;  // 0 is a legitimate explicit initial
        Assert.True(ModValuesNative.DefOf("fish", &rec).HasInitial);
        rec.Minimum = 10;
        rec.Initial = 20;
        Assert.Equal((10L, 20L), (ModValuesNative.DefOf("fish", &rec).Min, ModValuesNative.DefOf("fish", &rec).Initial));
        rec.Flags = 0;
        Assert.Equal(10L, ModValuesNative.DefOf("fish", &rec).Initial);  // clear: the minimum

        var xp = Coins(pins, "\"Fishing XP\"");
        xp.Kind = (uint)ModValueKind.Experience;
        xp.LocalId = pins.Str("xp");
        xp.Maximum = long.MaxValue;
        xp.Thresholds = pins.Many(0L, 100L, 400L);
        xp.ThresholdCount = 3;
        xp.Persistence = (uint)ModValuePersistence.Carried;
        xp.Flags = ModValuesAbi.TrustImports | ModValuesAbi.AllowPartitionMove | ModValuesAbi.ResetKindChange;
        xp.ReadPolicy = (uint)ModValueReadPolicy.AllowList;
        xp.ReadMods = pins.Many(pins.Str("other"), pins.Str("native.0123456789abcdef0123456789abcdef"));
        xp.ReadModCount = 2;
        xp.SyncPolicy = (uint)ModValueSyncPolicy.Readers;
        xp.NumberFormatJson = pins.Str("{\"*\": \"N0\"}");
        var e = ModValuesNative.DefOf("fish", &xp);
        Assert.Equal(new long[] { 0, 100, 400 }, e.Thresholds);
        Assert.Equal(new[] { "native.0123456789abcdef0123456789abcdef", "other" }, e.ReadMods);
        Assert.Equal((ModValuePersistence.Carried, true, true, true, ModValueSyncPolicy.Readers), (e.Persistence, e.TrustImports, e.AllowPartitionMove, e.ResetOnKindChange, e.Sync));
        Assert.Equal("Fishing XP", e.Label["*"]);

        // describe: the result's numbers; Kiln.ModValues' rules as INVALID_ARGUMENT
        var o = default(ModValuesAbi.DefinitionResultRec);
        Def? seen = null;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Describe(x => { seen = x; return new ModValueDefinitionResult(true, null, x.Key); }, pins.Str("fish"), &rec, &o));
        Assert.Equal((1u, 0u, "fish:coins"), (o.Ok, o.Reason, seen!.Qualified));
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Describe(x => new ModValueDefinitionResult(false, ModValueReasons.DefinitionConflict, x.Key), pins.Str("fish"), &rec, &o));
        Assert.Equal((0u, 15u), (o.Ok, o.Reason));
        var bad = Coins(pins, "Coins");  // not JSON: a label is UiText JSON ("\"Coins\"" or an object)
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        bad = Coins(pins);
        bad.Persistence = (uint)ModValuePersistence.Carried;
        bad.Flags = ModValuesAbi.TrustImports;
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        bad = Coins(pins);
        bad.Flags = 16;
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        bad = Coins(pins);
        bad.Minimum = 5;
        bad.Maximum = 1;
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        bad = Coins(pins);
        bad.LabelJson = pins.Bytes([0xff, 0xfe]);
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), null, &o));
        Assert.Equal(ModValuesAbi.Unavailable, ModValuesNative.Describe(_ => throw new KilnUnavailableException("stats.definitions", "off"), pins.Str("fish"), &rec, &o));
        foreach (var field in new[] { 0, 1, 2 })
        {
            bad = Coins(pins);
            if (field == 0) bad.Reserved0 = 1;
            else if (field == 1) bad.Reserved1 = 1;
            else bad.Reserved2 = 1;
            Assert.Equal(ModValuesAbi.InvalidArgument, ModValuesNative.Describe(_ => throw new Exception("not reached"), pins.Str("fish"), &bad, &o));
        }
    }

    // ------------------------------------------------------------------------------ owners and their tokens ----

    [Fact]
    public void DescribeAndDefineCarryTheNativeOwnersToken()
    {
        using var pins = new Pins();
        var rec = Coins(pins);
        var o = default(ModValuesAbi.DefinitionResultRec);
        object? seen = null;
        Assert.Equal(ModValuesAbi.Ok, ModValuesNative.Describe((d, token) => { seen = token; return new ModValueDefinitionResult(true, null, d.Key); },
            pins.Str("native.0123456789abcdef0123456789abcdef"), 7, &rec, &o));
        Assert.Equal(ModValuesNative.TokenOf(7), seen);
        Assert.Equal((object)7UL, seen);  // a boxed owner handle: Equals, not reference identity, is what the registry compares
        Assert.NotEqual(ModValuesNative.TokenOf(8), seen);
    }

    [Fact]
    public void ARetireTakesOnlyThatOwnersDefinitions()
    {
        // the registry's contract the native facade relies on: a native owner's token is its boxed handle
        var registry = new Registry();
        var ns = "native.0123456789abcdef0123456789abcdef";
        Def D(string id) => Def.FromOptions(ns, id, new ModValueOptions(ModValueKind.Counter, id));
        Assert.Null(registry.Activate(D("a"), ModValuesNative.TokenOf(1), out _));
        Assert.Null(registry.Activate(D("b"), ModValuesNative.TokenOf(2), out _));
        Assert.Null(registry.Describe(D("c"), ModValuesNative.TokenOf(1), out _));
        var gone = registry.Retire(ns, ModValuesNative.TokenOf(1));  // a fresh box of the same handle, as the revoke passes it
        Assert.Equal(new[] { "a" }, gone.Select(d => d.Id));
        Assert.Null(registry.Active(new ModValueKey(ns, "a")));
        Assert.NotNull(registry.Active(new ModValueKey(ns, "b")));
        Assert.Null(registry.Described(new ModValueKey(ns, "c")));
        Assert.NotNull(registry.Described(new ModValueKey(ns, "b")));

        // the bound retire passes the owner's namespace and its token; a bad namespace is refused inside, nothing thrown
        using var pins = new Pins();
        var calls = new List<(string, object?)>();
        ModValuesNative.Retire((owner, token) => calls.Add((owner, token)), pins.Str(ns), 2);
        ModValuesNative.Retire((owner, token) => calls.Add((owner, token)), pins.Str("Not A Namespace"), 3);
        Assert.Equal(new[] { (ns, (object?)2UL) }, calls);
    }

    // ------------------------------------------------------------------------------- the backend table ----

    [Fact]
    public void TheBackendTableIsTakenOnlyAtExactlyV1()
    {
        var t = default(ModValuesAbi.BackendTable);
        t.Header.Size = (uint)sizeof(ModValuesAbi.BackendTable);
        t.Header.Version = ModValuesAbi.BackendVersion;
        Assert.Null(ModValuesNative.Refusal(&t));
        t.Header.Version = 2;  // a later version of a Kiln-internal table is another build's, not a superset
        Assert.Contains("version 2", ModValuesNative.Refusal(&t));
        t.Header.Version = 0;
        Assert.NotNull(ModValuesNative.Refusal(&t));
        t.Header.Version = ModValuesAbi.BackendVersion;
        t.Header.Size += 8;  // larger: not append-only either
        Assert.Contains("40-byte", ModValuesNative.Refusal(&t));
        t.Header.Size = 24;
        Assert.Contains("24-byte", ModValuesNative.Refusal(&t));
    }

    // ---------------------------------------------------------------------------------------------- events ----

    [Fact]
    public void EventsBecomeChangeRecordsWithTheirExportPolicy()
    {
        var who = new CharacterKey(3, Guid.Parse("00112233-4455-6677-8899-aabbccddeeff"));
        var key = new ModValueKey("fish", "coins");
        var world = Guid.NewGuid();
        var after = new ModValueSnapshot(world, who, key, 12, 0, 0, ModValueKind.Currency, ModValuePersistence.World, 1, 1, 1, 4);
        var seen = new List<string>();
        var rc = ModValuesNative.WithChange(ModValuesAbi.CanonicalEvents, ModValuesAbi.Initialized, 99, who, key, null, after,
            ModValueReadPolicy.AllowList, ModValueSyncPolicy.Readers, ["other", "native.0123456789abcdef0123456789abcdef"], (c, x) =>
            {
                seen.Add(Encoding.UTF8.GetString(c->QualifiedKey.Ptr, (int)c->QualifiedKey.Len));
                seen.Add($"{c->Size} {c->EventKind} {c->EventScope} {c->RequestId} {c->Result.Size} {c->Result.Ok} {c->Result.Flags} {c->Result.After.Value} {c->Result.Before.Size}");
                seen.Add($"{x->Size} {x->ReadPolicy} {x->SyncPolicy} {x->ReadModCount}");
                for (var i = 0; i < x->ReadModCount; i++) seen.Add(Encoding.UTF8.GetString(x->ReadMods[i].Ptr, (int)x->ReadMods[i].Len));
                seen.Add(ModValuesNative.CharacterOf(&c->Character) == who && ModValuesNative.GuidOf(c->Result.After.World) == world ? "ids ok" : "ids wrong");
                return 0;
            });
        Assert.Equal(0, rc);
        Assert.Equal(new[] { "fish:coins", "248 0 0 99 184 1 2 12 72", "24 2 2 2", "other", "native.0123456789abcdef0123456789abcdef", "ids ok" }, seen);
    }

    // ------------------------------------------------------------------------------- the bound functions ----

    [Fact]
    public void TheBoundFunctionsAnswerWithoutThrowing()
    {
        using var pins = new Pins();
        var f = ModValuesNative.Functions();
        Assert.Equal((uint)sizeof(ModValuesAbi.BackendFns), f.Size);
        Assert.True(f.Describe != null && f.Define != null && f.Read != null && f.Change != null && f.ReadSynced != null && f.Retire != null);
        var o = default(ModValuesAbi.DefinitionResultRec);
        Assert.Equal(ModValuesAbi.InvalidArgument, f.Describe(pins.Str("fish"), 7, null, &o));
        Assert.Equal(ModValuesAbi.InvalidArgument, f.Define(pins.Str("fish"), 7, null, &o));
        var r = default(ModValuesAbi.ResultRec);
        Assert.Equal(ModValuesAbi.InvalidArgument, f.Read(pins.Str("fish"), null, pins.Str("fish:coins"), &r));
        Assert.Equal(ModValuesAbi.InvalidArgument, f.Change(pins.Str("fish"), null, pins.Str("fish:coins"), 0, 1, null, 0, &r));
        Assert.Equal(ModValuesAbi.InvalidArgument, f.ReadSynced(pins.Str("fish"), pins.Str("not a key"), &r));
        f.Retire(pins.Str("Not A Namespace"), 7);  // refused inside, nothing thrown
    }
}
