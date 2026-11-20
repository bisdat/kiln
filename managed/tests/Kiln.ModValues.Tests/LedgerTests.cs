// SPDX-License-Identifier: MIT
// The ledger (chapter 05b sections 4.1-4.3) on an in-memory store: definitions, initialization, the one error rule,
// revisions, compare-and-set, migration, quarantine, partitions, the carried projection and the maximum merge.
using Kiln;
using Xunit;

namespace ModValuesTests;

public sealed class LedgerTests
{
    internal sealed class Rig
    {
        public readonly Registry Defs = new();
        public readonly MemoryValueStore Store = new();
        public readonly ListValueEvents Events = new();
        public readonly HashSet<CharacterKey> Live = [];
        public readonly Ledger Ledger;
        public readonly List<(CharacterKey, ModValueKey)> Dirty = [];

        public Rig()
        {
            Ledger = new Ledger(Defs, Store, c => Live.Contains(c), Events);
            Ledger.Dirty += (c, k) => Dirty.Add((c, k));
        }

        public Def Define(string owner, string id, ModValueOptions o)
        {
            var d = Def.FromOptions(owner, id, o);
            Assert.Null(Defs.Activate(d, null, out var activated));
            if (activated) Ledger.Activated(d, Live);
            return d;
        }

        public CharacterKey Join(int n)
        {
            var c = new CharacterKey(0x1111222233334444, new Guid(n, 0, 0x4000, 0x80, 0, 0, 0, 0, 0, 0, (byte)n));
            Live.Add(c);
            Ledger.Touch(c);
            return c;
        }
    }

    private static readonly ModValueOptions Coins = new(ModValueKind.Currency, "Coins", Max: 1_000_000);
    private static readonly ModValueOptions Xp = new(ModValueKind.Experience, "XP", Thresholds: [0, 100, 400, 900]);
    private static ModValueKey K(string s) => ModValueKey.TryParse(s, out var k) ? k : throw new ArgumentException(s);

    [Fact]
    public void CounterWithMinTenStartsAtTenNotZero()
    {
        var r = new Rig();
        r.Define("fish", "c", new ModValueOptions(ModValueKind.Counter, "C", Min: 10, Max: 100));
        var a = r.Join(1);
        var v = r.Ledger.Read("fish", a, K("fish:c"));
        Assert.True(v.Ok, v.Reason);
        Assert.Equal(10, v.Value!.Value);
        Assert.Equal(0ul, v.Value.ValueRevision);
        var init = Assert.IsType<ModValueChangedEvent>(Assert.Single(r.Events.All));
        Assert.Equal(ModValueCauses.Initialized, init.Cause);
        Assert.Null(init.Before);
    }

    [Fact]
    public void ResearchRunValuesAwardSpendRevision()
    {
        var r = new Rig();
        r.Define("fish", "xp", Xp);
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        var b = r.Join(2);
        Assert.True(r.Ledger.Change("fish", a, K("fish:xp"), ChangeOp.Add, 7, null, 0).Ok);
        var award = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 11, null, 0);
        Assert.Equal((11L, 1ul), (award.After!.Value, award.After.ValueRevision));
        var poor = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Spend, 20, null, 0);
        Assert.Equal((false, ModValueReasons.InsufficientFunds), (poor.Ok, poor.Reason));
        var spend = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Spend, 3, null, 0);
        Assert.Equal((8L, 2ul), (spend.After!.Value, spend.After.ValueRevision));
        Assert.Equal(0, r.Ledger.Read("fish", b, K("fish:coins")).Value!.Value);
        Assert.Equal(1, r.Ledger.Read("fish", a, K("fish:xp")).Value!.Level);
    }

    [Fact]
    public void ZeroAddIsAnAcceptedNoOp()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        var n = r.Events.All.Count;
        var res = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 0, null, 0);
        Assert.True(res.Ok);
        Assert.Equal(res.Before, res.After);
        Assert.Equal(0ul, res.After!.ValueRevision);
        Assert.Equal(n, r.Events.All.Count);
    }

    [Fact]
    public void OverflowIsBoundExceededWithoutAChange()
    {
        var r = new Rig();
        r.Define("fish", "c", new ModValueOptions(ModValueKind.Counter, "C", Min: long.MinValue, Initial: long.MaxValue - 1));
        var a = r.Join(1);
        var res = r.Ledger.Change("fish", a, K("fish:c"), ChangeOp.Add, 5, null, 0);
        Assert.Equal((false, ModValueReasons.BoundExceeded), (res.Ok, res.Reason));
        Assert.Equal(long.MaxValue - 1, r.Ledger.Read("fish", a, K("fish:c")).Value!.Value);
        Assert.Equal(0ul, r.Ledger.Read("fish", a, K("fish:c")).Value!.ValueRevision);
    }

    [Fact]
    public void ConflictingDuplicateIsRefusedIdenticalIsIdempotent()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        Assert.Null(r.Defs.Activate(Def.FromOptions("fish", "coins", Coins), null, out var again));
        Assert.False(again);
        Assert.Equal(ModValueReasons.DefinitionConflict, r.Defs.Activate(Def.FromOptions("fish", "coins", Coins with { Max = 5 }), null, out _));
        Assert.Equal(ModValueReasons.DefinitionConflict, r.Defs.Describe(Def.FromOptions("fish", "coins", Coins with { Label = "Other" }), null, out _));
    }

    [Fact]
    public void CrossModWriteIsRefusedEvenWhenPublic()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins with { ReadPolicy = ModValueReadPolicy.Public });
        var a = r.Join(1);
        Assert.Equal(ModValueReasons.AccessDenied, r.Ledger.Change("other", a, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        Assert.True(r.Ledger.Read("other", a, K("fish:coins")).Ok);
        r.Define("fish", "secret", Coins);
        Assert.Equal(ModValueReasons.AccessDenied, r.Ledger.Read("other", a, K("fish:secret")).Reason);
        r.Define("fish", "shared", Coins with { ReadPolicy = ModValueReadPolicy.AllowList, ReadMods = ["friend"] });
        Assert.True(r.Ledger.Read("friend", a, K("fish:shared")).Ok);
        Assert.Equal(ModValueReasons.AccessDenied, r.Ledger.Read("other", a, K("fish:shared")).Reason);
    }

    [Fact]
    public void ExpectedRevisionZeroMatchesOnlyInTheSameWorld()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        var other = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, new ModValueVersion(Guid.NewGuid(), 0), 0);
        Assert.Equal(ModValueReasons.StaleRevision, other.Reason);
        var same = r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, new ModValueVersion(r.Store.World, 0), 0);
        Assert.True(same.Ok, same.Reason);
        Assert.Equal(ModValueReasons.StaleRevision, r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, new ModValueVersion(r.Store.World, 0), 0).Reason);
        Assert.True(r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, same.After!.Version, 0).Ok);
    }

    [Fact]
    public void AccountOnlyOfflineAndUninitializedCharacters()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var accountOnly = new CharacterKey(5, Guid.Empty);
        r.Live.Add(accountOnly);
        Assert.Equal(ModValueReasons.CharacterUnavailable, r.Ledger.Change("fish", accountOnly, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        Assert.Equal(ModValueReasons.CharacterUnavailable, r.Ledger.Read("fish", accountOnly, K("fish:coins")).Reason);
        var never = new CharacterKey(5, Guid.NewGuid());
        Assert.Equal(ModValueReasons.NotReady, r.Ledger.Read("fish", never, K("fish:coins")).Reason);
        Assert.Equal(ModValueReasons.CharacterUnavailable, r.Ledger.Change("fish", never, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 4, null, 0);
        r.Live.Remove(a);  // offline: read yes, write no
        Assert.Equal(4, r.Ledger.Read("fish", a, K("fish:coins")).Value!.Value);
        Assert.Equal(ModValueReasons.CharacterUnavailable, r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        Assert.Equal(ModValueReasons.Undefined, r.Ledger.Read("fish", a, K("fish:nothing")).Reason);
    }

    [Fact]
    public void AuthorErrorsThrowArgumentException()
    {
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "Bad", Coins));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { Min = -1 }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { Initial = 2_000_000 }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { Persistence = ModValuePersistence.Carried, TrustClientImports = true }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "x", Xp with { Thresholds = [5, 10] }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "x", Xp with { Thresholds = [0, 10, 10] }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "x", Xp with { Persistence = ModValuePersistence.Carried }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { NumberFormat = "D2" }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { ReadMods = ["x"] }));
        Assert.Throws<ArgumentException>(() => Def.FromOptions("fish", "c", Coins with { Scale = 7 }));
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        Assert.Throws<ArgumentException>(() => r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Spend, 0, null, 0));
        Assert.Throws<ArgumentException>(() => r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, -1, null, 0));
    }

    [Fact]
    public void LevelsFromThresholds()
    {
        var d = Def.FromOptions("fish", "xp", Xp);
        Assert.Equal([1, 1, 2, 2, 3, 4, 4], new long[] { 0, 99, 100, 399, 400, 900, long.MaxValue }.Select(d.LevelOf));
    }

    [Fact]
    public void SameVersionSemanticChangeIsHeldNewerVersionMigrates()
    {
        var r = new Rig();
        r.Define("fish", "w", new ModValueOptions(ModValueKind.Counter, "W", Min: -1000, Scale: 2));
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:w"), ChangeOp.Set, -123, null, 0);
        r.Defs.Retire("fish");
        // the same version with another scale: held, the value intact
        r.Define("fish", "w", new ModValueOptions(ModValueKind.Counter, "W", Min: -5, Max: 100, Scale: 0));
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Read("fish", a, K("fish:w")).Reason);
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Change("fish", a, K("fish:w"), ChangeOp.Add, 1, null, 0).Reason);
        Assert.DoesNotContain(r.Ledger.Visible(a), v => v.Def.Id == "w");
        r.Defs.Retire("fish");
        // version 2: -1.23 at scale 0 truncates toward zero to -1, within -5..100
        r.Define("fish", "w", new ModValueOptions(ModValueKind.Counter, "W", Min: -5, Max: 100, Scale: 0, Version: 2));
        var v = r.Ledger.Read("fish", a, K("fish:w"));
        Assert.True(v.Ok, v.Reason);
        Assert.Equal((-1L, 0, 2ul), (v.Value!.Value, v.Value.Scale, v.Value.ValueRevision));
        var m = r.Events.All.OfType<ModValueChangedEvent>().Last();
        Assert.Equal(ModValueCauses.Migrated, m.Cause);
        Assert.Equal(-123, m.Before!.Value);
    }

    [Fact]
    public void KindChangeIsHeldUnlessResetOnKindChange()
    {
        var r = new Rig();
        r.Define("fish", "v", new ModValueOptions(ModValueKind.Counter, "V"));
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:v"), ChangeOp.Add, 50, null, 0);
        r.Defs.Retire("fish");
        r.Define("fish", "v", new ModValueOptions(ModValueKind.Currency, "V", Version: 2));
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Read("fish", a, K("fish:v")).Reason);
        r.Defs.Retire("fish");
        r.Define("fish", "v", new ModValueOptions(ModValueKind.Currency, "V", Initial: 3, Version: 2, ResetOnKindChange: true));
        var v = r.Ledger.Read("fish", a, K("fish:v"));
        Assert.Equal((true, 3L, ModValueKind.Currency), (v.Ok, v.Value!.Value, v.Value.Kind));
    }

    [Fact]
    public void ThresholdChangeKeepsTotalExperience()
    {
        var r = new Rig();
        r.Define("fish", "xp", Xp);
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:xp"), ChangeOp.Add, 500, null, 0);
        Assert.Equal(3, r.Ledger.Read("fish", a, K("fish:xp")).Value!.Level);
        r.Defs.Retire("fish");
        r.Define("fish", "xp", Xp with { Thresholds = [0, 250, 1000], Version = 2 });
        var v = r.Ledger.Read("fish", a, K("fish:xp")).Value!;
        Assert.Equal((500L, 2), (v.Value, v.Level));
    }

    [Fact]
    public void RetiredDefinitionParksDataAndComesBack()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 9, null, 0);
        r.Defs.Retire("fish");
        Assert.Equal(ModValueReasons.DefinitionRetired, r.Ledger.Read("fish", a, K("fish:coins")).Reason);
        Assert.Equal(ModValueReasons.DefinitionRetired, r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        r.Define("fish", "coins", Coins);
        Assert.Equal(9, r.Ledger.Read("fish", a, K("fish:coins")).Value!.Value);
    }

    [Fact]
    public void PartitionMoveOnlyWithPolicyAndNeverInBoth()
    {
        var r = new Rig();
        r.Define("fish", "p", new ModValueOptions(ModValueKind.Counter, "P"));
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:p"), ChangeOp.Add, 40, null, 0);
        r.Defs.Retire("fish");
        r.Define("fish", "p", new ModValueOptions(ModValueKind.Counter, "P", Persistence: ModValuePersistence.Carried, TrustClientImports: true, Version: 2));
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Read("fish", a, K("fish:p")).Reason);
        r.Defs.Retire("fish");
        r.Define("fish", "p", new ModValueOptions(ModValueKind.Counter, "P", Persistence: ModValuePersistence.Carried, TrustClientImports: true, Version: 2, AllowPartitionMove: true));
        var v = r.Ledger.Read("fish", a, K("fish:p"));
        Assert.Equal((true, 40L, ModValuePersistence.Carried), (v.Ok, v.Value!.Value, v.Value.Persistence));
        Assert.DoesNotContain("\"p\"", r.Store.Read(Partition.World, a, "fish")!);
        Assert.Contains("\"p\"", r.Store.Read(Partition.Projection, a, "fish")!);
    }

    [Fact]
    public void CarriedIsMonotonicAndResetsOnlyByEpoch()
    {
        var r = new Rig();
        var carried = new ModValueOptions(ModValueKind.Experience, "XP", Thresholds: [0, 10], Persistence: ModValuePersistence.Carried, TrustClientImports: true);
        r.Define("fish", "cx", carried);
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:cx"), ChangeOp.Add, 30, null, 0);
        Assert.Equal(ModValueReasons.NonMonotonic, r.Ledger.Change("fish", a, K("fish:cx"), ChangeOp.Set, 10, null, 0).Reason);
        Assert.Equal(ModValueReasons.EpochRequired, r.Ledger.Change("fish", a, K("fish:cx"), ChangeOp.Reset, 0, null, 0).Reason);
        r.Defs.Retire("fish");
        r.Define("fish", "cx", carried with { CarryEpoch = 2 });
        var v = r.Ledger.Read("fish", a, K("fish:cx")).Value!;
        Assert.Equal((0L, 2u), (v.Value, v.CarryEpoch));
        Assert.Equal(ModValueCauses.EpochReset, r.Events.All.OfType<ModValueChangedEvent>().Last().Cause);
        r.Defs.Retire("fish");
        r.Define("fish", "cx", carried);  // a lower epoch than saved
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Read("fish", a, K("fish:cx")).Reason);
    }

    private static (Rig, CharacterKey) CarriedRig()
    {
        var r = new Rig();
        r.Define("fish", "cx", new ModValueOptions(ModValueKind.Experience, "XP", Thresholds: [0, 10], Persistence: ModValuePersistence.Carried, TrustClientImports: true));
        r.Define("fish", "coins", Coins);
        r.Define("fish", "scaled", new ModValueOptions(ModValueKind.Counter, "S", Scale: 1, Max: 1000, Persistence: ModValuePersistence.Carried, TrustClientImports: true));
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:cx"), ChangeOp.Add, 500, null, 0);
        return (r, a);
    }

    [Fact]
    public void ImportTakesTheExactMaximumNeverASum()
    {
        var (r, a) = CarriedRig();
        var s = r.Ledger.Import(a, [new OfferEntry("fish:cx", 300, ModValueKind.Experience, 0, 1, 1)]);
        Assert.Equal((1, 0), (s.Unchanged, s.Applied));
        Assert.Equal(500, r.Ledger.Read("fish", a, K("fish:cx")).Value!.Value);
        s = r.Ledger.Import(a, [new OfferEntry("fish:cx", 700, ModValueKind.Experience, 0, 1, 1)]);
        Assert.Equal(1, s.Applied);
        Assert.Equal(700, r.Ledger.Read("fish", a, K("fish:cx")).Value!.Value);
        Assert.Equal(ModValueCauses.Imported, r.Events.All.OfType<ModValueChangedEvent>().Last().Cause);
    }

    [Fact]
    public void ImportRejectsWorldOldEpochWrongKindAndRescales()
    {
        var (r, a) = CarriedRig();
        var s = r.Ledger.Import(a,
        [
            new OfferEntry("fish:coins", 999999, ModValueKind.Currency, 0, 1, 1),     // World: never imported
            new OfferEntry("fish:cx", 9000, ModValueKind.Experience, 0, 1, 0),        // old epoch
            new OfferEntry("fish:cx", 9000, ModValueKind.Counter, 0, 1, 1),           // wrong kind (a duplicate key too)
            new OfferEntry("fish:scaled", 1239, ModValueKind.Counter, 2, 1, 1),       // 12.39 at scale 1: 12.3 -> raw 123
            new OfferEntry("other:thing", 5, ModValueKind.Counter, 0, 1, 1),
        ]);
        Assert.Equal((1, 4), (s.Applied, s.Rejected));
        Assert.Equal(0, r.Ledger.Read("fish", a, K("fish:coins")).Value!.Value);
        Assert.Equal(500, r.Ledger.Read("fish", a, K("fish:cx")).Value!.Value);
        Assert.Equal(123, r.Ledger.Read("fish", a, K("fish:scaled")).Value!.Value);
    }

    [Fact]
    public void GenericReplaceBeforeAndAfterAMergeLeavesCanonicalRecordsIntact()
    {
        var (r, a) = CarriedRig();
        var world = r.Store.Read(Partition.World, a, "fish");
        // a generic carried import replaces the projection with whatever the client had, forged World ids included
        r.Store.Write(Partition.Projection, a, "fish", "{\"schema\":1,\"partitionRevision\":\"99\",\"entries\":{\"coins\":{\"raw\":\"999999\",\"valueRevision\":\"9\",\"kind\":\"currency\"},\"cx\":{\"raw\":\"99999\",\"valueRevision\":\"9\",\"kind\":\"experience\"}}}");
        r.Ledger.ProjectAll();
        Assert.Equal(world, r.Store.Read(Partition.World, a, "fish"));
        Assert.Equal(500, r.Ledger.Read("fish", a, K("fish:cx")).Value!.Value);
        Assert.Equal(r.Store.Read(Partition.CarriedAuthority, a, "fish"), r.Store.Read(Partition.Projection, a, "fish"));
        Assert.DoesNotContain("coins", r.Store.Read(Partition.Projection, a, "fish")!);
        r.Ledger.Import(a, [new OfferEntry("fish:cx", 800, ModValueKind.Experience, 0, 1, 1)]);
        r.Store.Write(Partition.Projection, a, "fish", "{}");  // and a replace after the merge
        r.Ledger.ProjectAll();
        Assert.Equal(800, r.Ledger.Read("fish", a, K("fish:cx")).Value!.Value);
        Assert.Equal(world, r.Store.Read(Partition.World, a, "fish"));
    }

    [Fact]
    public void ACorruptRecordIsHeldNeverOverwritten()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        var a = r.Join(1);
        r.Store.Write(Partition.World, a, "fish", "{\"not\":\"ours\"}");
        Assert.Equal(ModValueReasons.MigrationRequired, r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 1, null, 0).Reason);
        Assert.Equal("{\"not\":\"ours\"}", r.Store.Read(Partition.World, a, "fish"));
    }

    [Fact]
    public void TwoModsKeepSeparateNamespaces()
    {
        var r = new Rig();
        r.Define("fish", "coins", Coins);
        r.Define("hunt", "coins", Coins);
        var a = r.Join(1);
        r.Ledger.Change("fish", a, K("fish:coins"), ChangeOp.Add, 5, null, 0);
        Assert.Equal(0, r.Ledger.Read("hunt", a, K("hunt:coins")).Value!.Value);
        Assert.NotNull(r.Store.Read(Partition.World, a, "fish"));
        Assert.NotNull(r.Store.Read(Partition.World, a, "hunt"));
    }
}

public sealed class FormatTests
{
    [Theory]
    [InlineData(9007199254740993L, 0, "N0", "en-US", "9,007,199,254,740,993")]
    [InlineData(9007199254740993L, 0, "0", "en-US", "9007199254740993")]
    [InlineData(123456789L, 2, "N2", "en-US", "1,234,567.89")]
    [InlineData(123456789L, 2, "N2", "de-DE", "1.234.567,89")]
    [InlineData(-5L, 3, "F3", "en-US", "-0.005")]
    [InlineData(long.MaxValue, 6, "0.000000", "en-US", "9223372036854.775807")]
    [InlineData(long.MinValue, 0, "N0", "en-US", "-9,223,372,036,854,775,808")]
    [InlineData(1L, 0, "N0", "xx-nope", "1")]
    public void ExactScaledDecimal(long raw, int scale, string format, string lang, string expected) =>
        Assert.Equal(expected, ModValueFormat.Format(raw, scale, format, lang));

    [Fact]
    public void OtherFormatsAreRefused()
    {
        Assert.Throws<ArgumentException>(() => ModValueFormat.Format(1, 0, "D3", "en-US"));
        Assert.Throws<ArgumentException>(() => ModValueFormat.Format(1, 0, "N7", "en-US"));
        Assert.Throws<ArgumentException>(() => ModValueFormat.Format(1, 0, "0.0000000", "en-US"));
        Assert.True(ModValueRules.IsNumberFormat("0.0"));
        Assert.False(ModValueRules.IsNumberFormat("0."));
        Assert.False(ModValueRules.IsNumberFormat("0.00a"));
    }
}
