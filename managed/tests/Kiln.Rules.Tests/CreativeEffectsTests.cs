// SPDX-License-Identifier: MIT
using System.Reflection;
using Xunit;

namespace Kiln.Rules.Tests;

public sealed class CreativeEffectsTests
{
    private static readonly EntityId P = new(77);

    [Fact]
    public void FlightOffWithdrawsCreativeRatherThanDenyingAnotherOwner()
    {
        var set = new RuleSet(RuleScope.ForPlayer(P), new PlayerRules { CharacterFlight = CharacterFlightMode.Unlimited }, "kiln", "creative", null);
        var sets = (Dictionary<EntityId, RuleSet>)typeof(Creative).GetField("Sets", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        sets[P] = set;
        try
        {
            Assert.True(Creative.SetFlight(P, CharacterFlightMode.Off).Ok);
            Assert.Null(set.Rules.CharacterFlight);
            var effective = RulesMath.Combine(P, [(set, set.Rules), (null, new PlayerRules { CharacterFlight = CharacterFlightMode.Unlimited })],
                RulesLimits.Default with { AllowCharacterFlight = true });
            Assert.Equal(CharacterFlightMode.Unlimited, effective.CharacterFlight);
        }
        finally { sets.Remove(P); set.Dispose(); Creative.Forget(); }
    }
}

public sealed unsafe class CreativeEffectsIntegrationTests
{
    private static readonly EntityId P = new(77);
    private static FieldInfo Field(Type t, string name) => t.GetField(name, BindingFlags.Static | BindingFlags.NonPublic)!;
    private static T Get<T>(Type t, string name) => (T)Field(t, name).GetValue(null)!;
    private static uint _hooks;

    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static int Status(RulesNative.Status* s) { s->Hooks = _hooks; return 0; }

    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static int SetNative(uint _, RulesNative.Player* rules) => 0;
    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    private static int LocalNative(RulesNative.Player* rules) => 0;

    private sealed class Fixture : IDisposable
    {
        public readonly RuleSet Set;
        private readonly RulesNative.Table* _table;
        private readonly object? _previousTable;
        private readonly object? _started, _limits, _retiring;
        private readonly Func<CreativeManagedSupport>? _managedProbe;
        public Fixture(PlayerRules? rules = null, RulesLimits? limits = null)
        {
            Creative.Forget();
            _managedProbe = CreativeEffects.ManagedProbe;
            CreativeEffects.ManagedProbe = () => new(true, true, true, true);
            _started = Field(typeof(RulesRuntime), "_started").GetValue(null);
            _retiring = Field(typeof(RulesRuntime), "_retiring").GetValue(null);
            Field(typeof(RulesRuntime), "_retiring").SetValue(null, 1);
            _limits = Field(typeof(RulesRuntime), "_limits").GetValue(null);
            Field(typeof(RulesRuntime), "_started").SetValue(null, false);
            Field(typeof(RulesRuntime), "_limits").SetValue(null, limits ?? RulesLimits.Default with { AllowCharacterFlight = true, AllowNoClip = true, AllowIgnoreBasePermissions = true });
            _previousTable = Field(typeof(RulesNative), "_t").GetValue(null);
            _table = (RulesNative.Table*)System.Runtime.InteropServices.NativeMemory.AllocZeroed((nuint)sizeof(RulesNative.Table));
            _table->GetStatus = &Status;
            _table->Set = &SetNative;
            _table->SetLocal = &LocalNative;
            _hooks = uint.MaxValue;
            Field(typeof(RulesNative), "_t").SetValue(null, Pointer.Box(_table, Field(typeof(RulesNative), "_t").FieldType));
            Get<HashSet<EntityId>>(typeof(RulesRuntime), "Characters").Add(P);
            var baseline = rules ?? PlayerRules.CreativeExtras.With(PlayerRules.Creative);
            Set = new(RuleScope.ForPlayer(P), baseline, "kiln", "creative", null);
            Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList").Add(Set);
            Get<Dictionary<EntityId, RuleSet>>(typeof(Creative), "Sets")[P] = Set;
            Get<Dictionary<EntityId, PlayerRules>>(typeof(Creative), "Defaults")[P] = baseline;
        }
        public void Dispose()
        {
            Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList").Remove(Set);
            Get<HashSet<EntityId>>(typeof(RulesRuntime), "Characters").Remove(P);
            Get<Dictionary<EntityId, HashSet<(long, int)>>>(typeof(RulesRuntime), "Spent").Remove(P);
            Creative.Forget();
            CreativeMenu.OnDisconnected();
            CreativeEffects.ManagedProbe = _managedProbe;
            Field(typeof(RulesRuntime), "_started").SetValue(null, _started);
            Field(typeof(RulesRuntime), "_retiring").SetValue(null, _retiring);
            Field(typeof(RulesRuntime), "_limits").SetValue(null, _limits);
            Field(typeof(RulesNative), "_t").SetValue(null, _previousTable);
            System.Runtime.InteropServices.NativeMemory.Free(_table);
        }
    }

    [Fact]
    public void EveryOptionalRuleBelongsToExactlyOneStableEffectGroup()
    {
        var fields = typeof(PlayerRules).GetProperties(BindingFlags.Public | BindingFlags.Instance).Select(f => f.Name).Order().ToArray();
        Assert.Equal(fields, CreativeEffects.All.SelectMany(e => e.Fields).Select(f => f.Name).Order().ToArray());
        Assert.Equal(Enumerable.Range(1, 22).Select(x => (byte)x), CreativeEffects.All.Select(e => e.Id));
    }

    [Fact]
    public void BuiltInDefaultsAndExtrasRoundTripEveryEffectWithoutChangingOtherFields()
    {
        using var f = new Fixture();
        var original = f.Set.Rules;
        foreach (var effect in CreativeEffects.All.Where(e => e.Requested(original)))
        {
            Assert.True(Creative.SetEffect(P, effect.Name, false).Ok);
            Assert.False(effect.Requested(f.Set.Rules));
            Assert.True(Creative.SetEffect(P, effect.Name, true).Ok);
            Assert.Equal(original, f.Set.Rules);
        }
        Assert.Null(original.NoFallDamage);
        Assert.Null(original.CharacterFlight);
        Assert.Null(original.NoClip);
    }

    [Fact]
    public void CustomPresetRestoresConfiguredValuesRatherThanBuiltInGrants()
    {
        var preset = PlayerRules.CreativeExtras.With(PlayerRules.FromJson("""{"minStamina":0.25,"minHealth":0.3,"invulnerable":false,"buildCost":0.5,"buildRefund":0.75,"keepConsumables":false}"""));
        using var f = new Fixture(preset);
        Assert.True(Creative.SetEffect(P, "stamina", false).Ok);
        Assert.True(Creative.SetEffect(P, "STAMINA", true).Ok);
        Assert.Equal(0.25, f.Set.Rules.MinStamina);
        Assert.True(Creative.SetEffect(P, "building-costs", false).Ok);
        Assert.True(Creative.SetEffect(P, "building-costs", true).Ok);
        Assert.Equal(0.5, f.Set.Rules.BuildCost);
        Assert.Equal(0.75, f.Set.Rules.BuildRefund);
        var before = f.Set.Rules;
        Assert.False(Creative.SetEffect(P, "no-consume", true).Ok);
        Assert.False(Creative.SetEffect(P, "building-flight", true).Ok);
        Assert.False(Creative.SetEffect(P, "unknown", true).Ok);
        Assert.Equal(before, f.Set.Rules);
        Assert.True(Creative.SetEffect(P, "invulnerability", false).Ok);
        Assert.Null(f.Set.Rules.MinHealth);
        Assert.False(f.Set.Rules.Invulnerable); // do not lift the preset's intentional denial
    }

    [Fact]
    public void OffWithdrawsHealthAndInvulnerabilityTogetherWithoutTouchingStaminaOrNoFall()
    {
        using var f = new Fixture(PlayerRules.Creative with { NoFallDamage = true });
        Assert.True(Creative.SetEffect(P, "invulnerability", false).Ok);
        Assert.Null(f.Set.Rules.Invulnerable);
        Assert.Null(f.Set.Rules.MinHealth);
        Assert.Equal(1, f.Set.Rules.MinStamina);
        Assert.True(f.Set.Rules.NoFallDamage);
    }

    [Fact]
    public void OtherOwnerStillAppliesButAnExplicitDenialCannotBeReenabledByCreative()
    {
        using var f = new Fixture();
        var other = new RuleSet(RuleScope.ForPlayer(P), new PlayerRules { Invulnerable = true, MinHealth = 0.5 }, "other", "other", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(other);
        try
        {
            Assert.True(Creative.SetEffect(P, "invulnerability", false).Ok);
            var state = Creative.Effects(P).Single(e => e.Name == "invulnerability");
            Assert.False(state.Requested);
            Assert.True(state.OtherOwners);
            Assert.Contains("invulnerable=on", state.Effective);
            Assert.Equal(0.5, RulesRuntime.Fresh(P).MinHealth);
            other.Update(new PlayerRules { Invulnerable = false });
            var before = f.Set.Rules;
            Assert.False(Creative.SetEffect(P, "invulnerability", true).Ok);
            Assert.Equal(before, f.Set.Rules);
        }
        finally { sets.Remove(other); }
    }

    [Fact]
    public void OutOfScopeDenialsAndGrantsDoNotAffectThisPlayersToggleOrStatus()
    {
        using var f = new Fixture();
        var outside = new RuleSet(RuleScope.ForPlayer(new EntityId(99)), new PlayerRules { Invulnerable = false, MinStamina = 1 }, "other", "not-this-player", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(outside);
        try
        {
            Assert.True(Creative.SetEffect(P, "invulnerability", false).Ok);
            Assert.True(Creative.SetEffect(P, "invulnerability", true).Ok);
            Assert.False(Creative.Effects(P).Single(e => e.Name == "stamina").OtherOwners);
        }
        finally { sets.Remove(outside); }
    }

    [Fact]
    public void PolicyAndMissingHooksLockOnWithoutMutationButAllowWithdrawingExistingRequest()
    {
        using var f = new Fixture(limits: RulesLimits.Default with { AllowInvulnerable = false, AllowKeepConsumables = false, MaxStaminaFloor = 0 });
        foreach (var name in new[] { "invulnerability", "no-consume", "stamina" })
        {
            var before = f.Set.Rules;
            Assert.False(Creative.SetEffect(P, name, true).Ok);
            Assert.Equal(before, f.Set.Rules);
            Assert.NotNull(Creative.Effects(P).Single(e => e.Name == name).Locked);
            Assert.True(Creative.SetEffect(P, name, false).Ok);
        }
        _hooks = 0;
        var rules = f.Set.Rules;
        Assert.False(Creative.SetEffect(P, "free-crafting", true).Ok);
        Assert.Equal(rules, f.Set.Rules);
        Assert.False(Creative.Effects(P).Single(e => e.Name == "no-consume").OtherOwners);
    }

    [Theory]
    [InlineData("building-costs")] [InlineData("mining")]
    public void NumericPolicyThatNeutralizesTheWholeCustomEffectLocksWithoutMutation(string name)
    {
        var preset = name == "building-costs" ? new PlayerRules { BuildCost = 0 } : new PlayerRules { DigDamage = 1000 };
        using var f = new Fixture(preset, RulesLimits.Default with { MinBuildCost = 1, MaxDigDamage = 1 });
        var state = Creative.Effects(P).Single(e => e.Name == name);
        Assert.NotNull(state.Locked);
        var before = f.Set.Rules;
        Assert.False(Creative.SetEffect(P, name, true).Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Fact]
    public void NumericCapMustNotLockRemainingHealthGroupsDamageImmunity()
    {
        using var f = new Fixture(PlayerRules.Creative, RulesLimits.Default with { MaxHealthFloor = 0 });
        Assert.Null(Creative.Effects(P).Single(e => e.Name == "invulnerability").Locked);
        Assert.True(Creative.SetEffect(P, "invulnerability", true).Ok);
    }

    [Fact]
    public void PartialCapsAndOtherOwnerRedundancyDoNotLockTheEffect()
    {
        using var f = new Fixture(limits: RulesLimits.Default with { MinBuildCost = 1, MaxDigDamage = 10 });
        var costs = Creative.Effects(P).Single(e => e.Name == "building-costs");
        Assert.Null(costs.Locked); // default refund=0 still has an effect when costs are clamped to normal
        Assert.Contains("buildCost=1", costs.Effective);
        Assert.Contains("buildRefund=0", costs.Effective);
        var other = new RuleSet(RuleScope.ForPlayer(P), new PlayerRules { DigDamage = 100 }, "other", "other-mining", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(other);
        try
        {
            Assert.True(Creative.SetEffect(P, "mining", false).Ok);
            var mining = Creative.Effects(P).Single(e => e.Name == "mining");
            Assert.Null(mining.Locked); // another owner's grant saturating the cap is not a policy exclusion
            Assert.Contains("digDamage=10", mining.Effective);
            Assert.True(mining.OtherOwners);
            Assert.True(Creative.SetEffect(P, "mining", true).Ok);
        }
        finally { sets.Remove(other); }
    }

    [Theory]
    [InlineData("stamina")] [InlineData("mana")] [InlineData("invulnerability")]
    [InlineData("no-fall")] [InlineData("water")]
    public void MissingManagedServiceLocksRestoreAndQualifiesEffectiveStatus(string name)
    {
        using var f = new Fixture(PlayerRules.Creative with { NoFallDamage = true });
        CreativeEffects.ManagedProbe = null;
        Assert.False(Abilities.Available);
        Assert.False(Combat.Available);
        Assert.False(Items.Fuel.Available);
        var state = Creative.Effects(P).Single(e => e.Name == name);
        Assert.NotNull(state.Locked);
        var before = f.Set.Rules;
        Assert.False(Creative.SetEffect(P, name, true).Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Theory]
    [InlineData("stamina", false, true, true, true, "minStamina=0")]
    [InlineData("mana", false, true, true, true, "minMana=0")]
    [InlineData("invulnerability", true, false, true, true, "invulnerable=off")]
    [InlineData("no-fall", true, true, false, true, "noFallDamage=off")]
    [InlineData("water", true, true, true, false, "water=game")]
    public void IndividualManagedPrerequisitesAreIndependentRefreshableAndDoNotMutateOtherOwners(
        string name, bool vitals, bool invulnerability, bool falls, bool water, string effective)
    {
        using var f = new Fixture(PlayerRules.Creative with { NoFallDamage = true });
        var original = f.Set.Rules;
        var other = new RuleSet(RuleScope.ForPlayer(P), original, "other", "other", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(other);
        try
        {
            CreativeEffects.ManagedProbe = () => new(vitals, invulnerability, falls, water);
            var unavailable = Creative.Effects(P).Single(e => e.Name == name);
            Assert.NotNull(unavailable.Locked);
            Assert.Contains(effective, unavailable.Effective);
            // Missing Combat does not hide another owner's still-supported health floor in this explicit group.
            Assert.Equal(name == "invulnerability", unavailable.OtherOwners);
            Assert.False(Creative.SetEffect(P, name, true).Ok);
            Assert.Equal(original, f.Set.Rules);
            Assert.Equal(original, other.Rules);
            if (!vitals)
                Assert.Contains("invulnerable=on", Creative.Effects(P).Single(e => e.Name == "invulnerability").Effective);
            if (!invulnerability)
                Assert.Contains("minHealth=1", unavailable.Effective); // health upkeep is not hidden with missing Combat
            if (!falls) Assert.Contains("invulnerable=on", Creative.Effects(P).Single(e => e.Name == "invulnerability").Effective);
            Assert.True(Creative.SetEffect(P, name, false).Ok);
            Assert.False(CreativeMenu.EffectRow(Creative.Effects(P).Single(e => e.Name == name), true).Enabled);
            CreativeEffects.ManagedProbe = () => new(true, true, true, true);
            Assert.Null(Creative.Effects(P).Single(e => e.Name == name).Locked);
            Assert.True(Creative.SetEffect(P, name, true).Ok);
            Assert.Equal(original, f.Set.Rules);
        }
        finally { sets.Remove(other); }
    }

    [Theory]
    [InlineData("building-flight", RulesNative.HookFly)]
    [InlineData("build-outside", RulesNative.HookZone)]
    [InlineData("no-build-zones", RulesNative.HookCursor)]
    [InlineData("building-costs", RulesNative.HookCosts)]
    [InlineData("mining", RulesNative.HookDig)]
    [InlineData("shroud-timer", RulesNative.HookShroud)]
    [InlineData("unlimited-gliding", RulesNative.HookGlide)]
    [InlineData("glider-sink", RulesNative.HookGlide)]
    [InlineData("glider-boost", RulesNative.HookGlide)]
    public void MissingBundledNativeHookLocksRestoreAndDoesNotClaimAnEffectiveGrant(string name, uint missing)
    {
        using var f = new Fixture();
        _hooks &= ~missing;
        var before = f.Set.Rules;
        Assert.NotNull(Creative.Effects(P).Single(e => e.Name == name).Locked);
        Assert.False(Creative.SetEffect(P, name, true).Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Fact]
    public void CharacterFlightCommandAndEffectWithdrawRatherThanDenyAndRememberSelectedMode()
    {
        using var f = new Fixture();
        Assert.True(Creative.SetFlight(P, CharacterFlightMode.BuildingArea).Ok);
        Assert.True(Creative.SetEffect(P, "character-flight", false).Ok);
        Assert.Null(f.Set.Rules.CharacterFlight);
        Assert.True(Creative.SetEffect(P, "character-flight", true).Ok);
        Assert.Equal(CharacterFlightMode.BuildingArea, f.Set.Rules.CharacterFlight);
        var other = new RuleSet(RuleScope.ForPlayer(P), new PlayerRules { CharacterFlight = CharacterFlightMode.Unlimited }, "other", "flight", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(other);
        try
        {
            Assert.True(Creative.SetFlight(P, CharacterFlightMode.Off).Ok);
            Assert.Null(f.Set.Rules.CharacterFlight);
            Assert.Equal(CharacterFlightMode.Unlimited, RulesRuntime.Fresh(P).CharacterFlight);
            Assert.True(Creative.Effects(P).Single(e => e.Name == "character-flight").OtherOwners);
        }
        finally { sets.Remove(other); }
    }

    [Fact]
    public void DeniedOrUntrustedFlightSelectionDoesNotMutateAndExplicitPresetOffStaysOff()
    {
        using var f = new Fixture(PlayerRules.Creative with { CharacterFlight = CharacterFlightMode.Off });
        var before = f.Set.Rules;
        Assert.False(Creative.SetFlight(P, CharacterFlightMode.Unlimited).Ok);
        Assert.Equal(before, f.Set.Rules);
        Assert.False(Creative.SetEffect(P, "character-flight", true).Ok);
        Get<HashSet<EntityId>>(typeof(RulesRuntime), "Characters").Remove(P);
        Assert.False(Creative.SetFlight(P, CharacterFlightMode.Unlimited).Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Fact]
    public void FlightOffDoesNotLiftAnExplicitPresetDenial()
    {
        using var f = new Fixture(new PlayerRules { CharacterFlight = CharacterFlightMode.Off });
        var before = f.Set.Rules;
        Assert.True(Creative.SetFlight(P, CharacterFlightMode.Off).Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Fact]
    public void OtherOwnersIsNotReportedAsEffectiveWhenCreativePresetDeniesTheirGrant()
    {
        using var f = new Fixture(new PlayerRules { Invulnerable = false });
        var other = new RuleSet(RuleScope.ForPlayer(P), new PlayerRules { Invulnerable = true }, "other", "other", null);
        var sets = Get<List<RuleSet>>(typeof(RulesRuntime), "SetsList");
        sets.Add(other);
        try
        {
            var state = Creative.Effects(P).Single(e => e.Name == "invulnerability");
            Assert.Contains("invulnerable=off", state.Effective);
            Assert.False(state.OtherOwners);
        }
        finally { sets.Remove(other); }
    }

    [Fact]
    public void UnrelatedToggleDoesNotRearmNoClipConsumedByDeathOrTeleport()
    {
        using var f = new Fixture(PlayerRules.Creative with { NoClip = true });
        var spent = Get<Dictionary<EntityId, HashSet<(long, int)>>>(typeof(RulesRuntime), "Spent");
        spent[P] = [(f.Set.Id, f.Set.Version)];
        Assert.False(RulesRuntime.Fresh(P).NoClip);
        Assert.True(Creative.SetEffect(P, "stamina", false).Ok);
        Assert.Contains((f.Set.Id, f.Set.Version), spent[P]);
        Assert.False(RulesRuntime.Fresh(P).NoClip);
        Assert.True(Creative.SetEffect(P, "no-clip", false).Ok);
        Assert.True(Creative.SetEffect(P, "no-clip", true).Ok);
        Assert.True(RulesRuntime.Fresh(P).NoClip);
    }

    [Fact]
    public void CommandsKeepAdminPermissionAndRouteEffectsWhileStatusExplainsRequestedAndEffective()
    {
        using var f = new Fixture();
        Creative.RegisterCommand();
        var admin = (CommandSource)typeof(CommandSource).GetMethod("HostConsole", BindingFlags.Static | BindingFlags.NonPublic)!
            .Invoke(null, [P, "synthetic-host"])!;
        var off = Commands.Execute(admin, "creative effect stamina off");
        Assert.True(off.Ok, off.ToString());
        Assert.Null(f.Set.Rules.MinStamina);
        Assert.True(Commands.Execute(admin, "creative effect stamina on").Ok);
        Assert.Equal(1, f.Set.Rules.MinStamina);
        var before = f.Set.Rules;
        Assert.False(Commands.Execute(CommandSource.Remote("read-only", admin: false), "creative effect stamina off").Ok);
        Assert.Equal(before, f.Set.Rules);
        Assert.Contains("on restores", Creative.EffectStatus(P, "stamina"));
        Assert.False(Commands.Execute(admin, "creative effect stamina maybe").Ok);
        Assert.Equal(before, f.Set.Rules);
    }

    [Fact]
    public void AdminUsesExistingCreativeMutationAndRulesReadOnlyRoutes()
    {
        using var f = new Fixture();
        var bridge = new Kiln.Admin.Bridge();
        Assert.True(Kiln.Admin.Ops.RegisterRulesSafe(bridge, new Kiln.Admin.Logger()));
        var result = (System.Text.Json.Nodes.JsonObject)bridge.Sync["creative"](new() { ["entity"] = P.Value, ["effect"] = "stamina", ["on"] = false })!;
        Assert.Null(f.Set.Rules.MinStamina);
        Assert.True(Creative.IsOn(P));
        Assert.NotNull(result["effects"]);
        var before = f.Set.Rules;
        var read = (System.Text.Json.Nodes.JsonObject)bridge.Sync["rules"](new() { ["entity"] = P.Value, ["effect"] = "stamina", ["on"] = true })!;
        Assert.Equal(before, f.Set.Rules); // passing mutation-shaped fields to rules cannot mutate
        Assert.NotNull(read["effects"]);
        var effects = (System.Text.Json.Nodes.JsonArray)read["effects"]!;
        Assert.False(effects.Single(e => e!["name"]!.GetValue<string>() == "stamina")!["requested"]!.GetValue<bool>());
    }

    [Fact]
    public void ServerEffectRequestsRequireActiveCreativeAndValidateAllFieldsBeforeMutation()
    {
        using var f = new Fixture();
        var before = f.Set.Rules;
        CreativeServer.Handle(P, new(CreativeAsk.Effect, Item: 13, Count: 2), false);
        CreativeServer.Handle(P, new(CreativeAsk.Effect, Item: 255, Count: 0), false);
        Assert.Equal(before, f.Set.Rules);
        CreativeServer.Handle(P, new(CreativeAsk.Effect, Item: 13, Count: 0), false);
        Assert.Null(f.Set.Rules.MinStamina); // a player granted Creative may customize it, like existing flight / Give
        Creative.Set(P, false);
        CreativeServer.Handle(P, new(CreativeAsk.Effect, Item: 13, Count: 1), false);
        Assert.False(Creative.IsOn(P));
    }

    [Fact]
    public void FlightAndInvulnerabilityTogglesLeaveTemporaryLandingGraceIntactAndReportItSeparately()
    {
        using var f = new Fixture();
        var type = typeof(RulesRuntime).GetNestedType("PlayerState", BindingFlags.NonPublic)!;
        var state = Activator.CreateInstance(type, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, [P], null)!;
        var until = DateTime.UtcNow.AddSeconds(30);
        type.GetField("GraceUntil")!.SetValue(state, until);
        var players = (System.Collections.IDictionary)Field(typeof(RulesRuntime), "PlayersByEntity").GetValue(null)!;
        players[P] = state;
        try
        {
            Assert.True(Creative.SetEffect(P, "building-flight", false).Ok);
            Assert.True(Creative.SetEffect(P, "invulnerability", false).Ok);
            Assert.Equal(until, type.GetField("GraceUntil")!.GetValue(state));
            Assert.True(RulesRuntime.LandingGrace(P));
            Assert.Contains("safe-landing protection active", Creative.Effects(P).Single(e => e.Name == "invulnerability").Effective);
            Assert.Null(f.Set.Rules.NoFallDamage); // safety was not implemented by injecting a persistent fall rule
        }
        finally { players.Remove(P); }
    }

    [Theory]
    [InlineData(false)] [InlineData(true)]
    public void PlayerLeaveAndWorldCloseRetireSetAndCapturedDefaults(bool closeWorld)
    {
        using var f = new Fixture();
        var type = typeof(RulesRuntime).GetNestedType("PlayerState", BindingFlags.NonPublic)!;
        var state = Activator.CreateInstance(type, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance, null, [P], null)!;
        if (closeWorld) typeof(RulesRuntime).GetMethod("WorldClosing", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, null);
        else typeof(RulesRuntime).GetMethod("Leave", BindingFlags.Static | BindingFlags.NonPublic)!.Invoke(null, [state]);
        Assert.False(f.Set.Active);
        Assert.False(Creative.IsOn(P));
        Assert.Empty(Get<Dictionary<EntityId, PlayerRules>>(typeof(Creative), "Defaults"));
        Assert.False(Creative.SetEffect(P, "stamina", true).Ok);
    }

    [Fact]
    public void FlameHandoffAndOffOrSessionCleanupDiscardEffectDefaults()
    {
        using var f = new Fixture();
        CreativeFlame.Probe = () => true;
        CreativeFlame.Reset();
        try
        {
            var before = f.Set.Rules;
            Assert.False(Creative.SetEffect(P, "stamina", false).Ok);
            Assert.Equal(before, f.Set.Rules);
            Assert.All(Creative.Effects(P), e => Assert.NotNull(e.Locked));
            Creative.StepAside();
            Assert.False(f.Set.Active);
        }
        finally { CreativeFlame.Probe = null; CreativeFlame.Reset(); }
        Assert.Empty(Get<Dictionary<EntityId, PlayerRules>>(typeof(Creative), "Defaults"));
        Creative.Forget();
        Assert.False(Creative.SetEffect(P, "stamina", true).Ok);
        Assert.All(Creative.Effects(P), e => Assert.False(e.Requested));
    }
}
