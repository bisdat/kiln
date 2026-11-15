// SPDX-License-Identifier: MIT
// The kit's fake of kiln.abilities' natives (chapter 06c, S7): Kiln.Abilities' own code (Abilities.Natives.RegisterSync,
// Attach, DeclareTyped, an [ImpactNative] method's generated registration, NativeContext's reads and writes) runs offline,
// and a test calls a native as a program's ECALL would (KilnHarness.ImpactNatives.Call). The rules are the native
// service's (native/services/abilities/natives_core.h and abilities.cpp): the stage order and filters of attachments,
// the typed argument check, the native registry's room, Waiting / Live / Refused / Dead, the answers of a removed
// native and of a handler that threw.
using System.Runtime.InteropServices;
using System.Text;

namespace Kiln.TestKit;

/// <summary>Plugs the fake kiln.abilities into the kit (<see cref="IKitFake"/>).</summary>
internal sealed class AbilitiesFake : IKitFake
{
    public string Service => AbilitiesAbi.Service;

    public void Register()
    {
        KitBackend.Instance.RegisterTable(AbilitiesAbi.Service, AbilitiesTable.Create());
        FakeTables.Features([AbilitiesAbi.NativesFeature]);
        foreach (var row in AbilitiesAbi.OfflineFeatures)
            FakeTables.Offline(row, "the test kit's fake of kiln.abilities has its natives only (no programs, instances, attributes or sequences)");
    }

    public void Start(KilnHarness harness) => ModelOf(harness).Start(harness);

    public void BeforeStep(KilnHarness harness, ulong step) => ModelOf(harness).BeforeStep(step);

    public void End(KilnHarness harness) => ModelOf(harness).End();

    public void AddCoverage(List<CoverageRow> rows) => rows.Add(new CoverageRow("Impact natives", CoverageKind.RecordingFake,
        [AbilitiesAbi.Service, AbilitiesAbi.NativesFeature, "abilities.natives.sync", "abilities.natives.attach", AbilitiesAbi.EcsRow],
        "Kiln.Abilities' own natives code on the kit's kiln.abilities: RegisterSync, an [ImpactNative] method's generated " +
        "registration, DeclareTyped, Attach and Detach, StateOf, Natives.All. The fake server world takes the natives registered " +
        "before it at the second step (Waiting, then Live, or Refused once 19 of them hold the registry's room for mods); a " +
        "native taken off answers 0x1F. KilnHarness.ImpactNatives.Call runs one ECALL as the native's thunk does, on a thread " +
        "of its own (no tick-thread API, as in the game): a typed declaration's check (0x2C another type or size, 0x18 a " +
        "required argument not connected), the handler's answer, 0x11 when it threw; for a game native the attachments whose " +
        "program and site match, Before by priority, then one Instead, then the game's native (what the test scripts with " +
        "SetGameNative; else it answers 1), then After. Game programs and their call sites are the ones the test declares " +
        "(AddProgram): Attach refuses as the service does (program not loaded, site not found, a second Instead, not a native), " +
        "and one made before the world is Pending, then Active or Off. NativeContext's TryRead and TryWrite reach the fake " +
        "world's components of impact_simulation's other_read and other_write lists (this build's, or the test's), on from " +
        "the first Kiln native call. KilnHarness.ImpactNatives.Calls records each call. Offline: the Impact programs " +
        "themselves (nothing runs a program's code: a test makes the calls), program registration, instances, attributes " +
        "(no formulas: a written attribute is stored as written), sequences, buffs, the suspending natives of Register, " +
        "the watchdog and the 0.2 ms budget"));

    public static AbilitiesModel ModelOf(KilnHarness harness) => KitFakes.State<AbilitiesModel>(harness);
}

/// <summary>One ECALL the test made (<see cref="HarnessImpactNatives.Call"/>).</summary>
/// <param name="Step">The harness step.</param>
/// <param name="Native">The native's name (a Kiln native's full name, a game native's node name) or its hash.</param>
/// <param name="Hash">The native's hash.</param>
/// <param name="Program">The calling program's id.</param>
/// <param name="Site">The call site.</param>
/// <param name="Error">The ECALL's error (0 ok).</param>
/// <param name="Value">What it pushed.</param>
/// <param name="Handlers">The handlers that ran (a sync native's, the attachments').</param>
public sealed record FakeImpactCallRecord(ulong Step, string Native, uint Hash, uint Program, uint Site, byte Error, byte Value, int Handlers);

/// <summary>The fake kiln.abilities of one harness: the natives, attachments and game programs of its fake server world.</summary>
internal sealed unsafe class AbilitiesModel
{
    // the game takes 128 native keys (half its registry's 256), 107 of them its own; kiln.Log and kiln.Signal take two
    public const int RegistryRoom = 128 - 107 - 2;
    public const int KilnSlots = 160 - 2;  // Kiln's native thunks, without the built-ins'
    public const ulong WorldStep = 2;      // the mods load in the first step; the world's setup comes after them

    private sealed class Native
    {
        public uint Hash, Argc, State;
        public string Name = "";
        public ulong Owner;
        public nint Fn, User;
        public AbilitiesAbi.NativeArg[]? Typed;
        public bool Keyed;  // holds a key of the world's registry
    }

    private sealed class Attachment
    {
        public ulong Id, Owner, Order;
        public uint Hash, Program, Site;
        public int Stage, Priority;
        public nint Fn, User;
        public bool Active = true, Checked;
    }

    private sealed class GameNative
    {
        public uint Hash;
        public string Name = "";
        public FakeGameNativeHandler? Handler;
    }

    // what the running call is (the thunk's thread-locals: t_slot, t_stage, the latch)
    private sealed class Scope
    {
        public AbilitiesAbi.NativeCall* Call;
        public AbilitiesModel Model = null!;
        public GameNative? Game;
        public int Stage = AbilitiesAbi.Before;
        public int Handlers;
    }

    [ThreadStatic] private static Scope? _scope;

    private readonly object _lock = new();
    private KilnHarness? _harness;
    private bool _worldIn, _ecsOn;
    private ulong _step, _attachSeq;
    private readonly List<Native> _natives = [];
    private readonly List<Attachment> _attachments = [];
    private readonly Dictionary<uint, AbilitiesAbi.NativeArg[]> _typed = [];
    private readonly Dictionary<uint, uint> _declared = [];
    private readonly Dictionary<uint, GameNative> _game = [];
    private readonly Dictionary<uint, List<(uint Hash, uint Site)>> _programs = [];
    private readonly List<FakeImpactCallRecord> _calls = [];
    private ulong _syncCalls, _syncFaults, _deadCalls, _attachEntered, _attachSkipped, _typedRefused, _typedUnconnected;
    private int _inflight;

    /// <summary>impact_simulation's other_read and other_write lists (the dedicated server's, game build 26430253, from
    /// sigdb/programs/server): what NativeContext may read, and write, of other entities.</summary>
    public readonly HashSet<string> OtherRead = new(StringComparer.Ordinal)
    {
        "Actor", "ActorInput", "CombatFlags", "EnemySensor", "Equipment", "Faction", "ImpactHierarchy", "ImpactItemConfig",
        "InventorySetup", "LogicGraphNodeConnected", "ParriedProjectile", "PlayerInput", "PlayerSkillsUi", "PlayerState",
        "ServerAnimationGraphSlotComponent", "SnappingBoxReference", "TargetEntity", "Terraforming", "UsedItem", "Velocity",
        "WeaponReference",
    };

    /// <inheritdoc cref="OtherRead"/>
    public readonly HashSet<string> OtherWrite = new(StringComparer.Ordinal)
    {
        "AllyAggroMultiplier", "Armor", "ArmorPoints", "Block", "BodyHeat", "BodyHeatMaxMod", "BuffContainer", "Comfort",
        "Constitution", "CritChance", "CritDamageMod", "CurrentTransform", "DamageMod", "DamageModAgainst", "DamageModBackstab",
        "DamageModMerciless", "DamageModSkillShot", "DamageModSneak", "DamageSusceptibility", "Dexterity", "Durability",
        "Endurance", "EnemyTargetSelectionBonus", "Experience", "FishingEndurance", "FishingStrength", "FogResistance",
        "FreezingResistance", "Fuel", "HealingMod", "Health", "HealthRechargeMod", "Intelligence", "Inventory",
        "InventoryEmptyState", "ItemState", "Level", "LifeTime", "MagicDamageMod", "Mana", "ManaCostMod", "ManaRechargeMod",
        "MaxSkeletonSpellCount", "MaxTotemCount", "MeleeDamageMod", "Movement", "OwnerRelationship", "Oxygen", "OxygenMaxMod",
        "ParryProficiency", "RangedCombat", "RangedDamageMod", "Rested", "Revive", "SelectedTargets", "ServerCursor",
        "SlotSelection", "SpecialAbility", "Spirit", "Stamina", "StaminaDamageMod", "StaminaDepletionMod", "StaminaRechargeMod",
        "Strength", "TemporaryMining", "TerraformingLootMemory", "Threat", "ThreatBonus", "ToolDamageMod", "UnlockedSkillNodes",
        "WeaponCategoryDamageMod", "WetDurationReduction",
    };

    public void Start(KilnHarness harness) => _harness = harness;

    public void End()
    {
        lock (_lock)
        {
            _natives.Clear();
            _attachments.Clear();
            _worldIn = false;
        }
    }

    public void BeforeStep(ulong step)
    {
        lock (_lock)
        {
            _step = step;
            if (!_worldIn && step >= WorldStep) WorldSetup();
        }
        AbilitiesTable.RunListener();  // kiln.abilities' Pre: Kiln.Abilities' listener
    }

    private void Log(LogLevel level, string text) => KitBackend.Instance.Log(0, level, text);

    private static bool OnTick => FakeTables.OnTickThread && _scope is null;

    private static string Pascal(string name) => char.ToUpperInvariant(name[0]) + name[1..];

    // ---- the test's side ----

    public void AddProgram(uint program, IEnumerable<FakeEcall> calls)
    {
        lock (_lock)
        {
            var list = _programs.TryGetValue(program, out var have) ? have : _programs[program] = [];
            foreach (var c in calls)
            {
                var hash = GameNativeOf(c.Native).Hash;
                if (!list.Contains((hash, c.Site))) list.Add((hash, c.Site));
            }
            // kiln.assets and Programs.Register: the attachments are checked again in the Pre after a program went in
            if (_worldIn) CheckAttachments();
        }
    }

    public void SetGameNative(string native, FakeGameNativeHandler? handler)
    {
        lock (_lock) GameNativeOf(native).Handler = handler;
    }

    private GameNative GameNativeOf(string native)
    {
        ArgumentException.ThrowIfNullOrEmpty(native);
        if (native.Contains('.')) throw new ArgumentException($"'{native}' is a Kiln native's name, not one of the game's", nameof(native));
        var hash = native.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? Convert.ToUInt32(native[2..], 16) : AbilitiesAbi.Fnv(Pascal(native));
        if (_game.TryGetValue(hash, out var g)) return g;
        return _game[hash] = new GameNative { Hash = hash, Name = native.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? native : Pascal(native) };
    }

    public IReadOnlyList<FakeImpactCallRecord> Calls
    {
        get
        {
            lock (_lock) return [.. _calls];
        }
    }

    // ---- the world's setup ----

    private void WorldSetup()
    {
        _worldIn = true;
        foreach (var n in _natives)
        {
            if (n.State != AbilitiesAbi.StateWaiting) continue;
            if (KeysUsed() < RegistryRoom)
            {
                n.Keyed = true;
                n.State = AbilitiesAbi.StateLive;
            }
            else
            {
                n.State = AbilitiesAbi.StateRefused;
                Log(LogLevel.Warn, $"kiln.abilities: native {n.Name} (0x{n.Hash:x8}) refused at the world's setup: native registry full (the game takes 128 natives, 107 of them its own)");
            }
        }
        foreach (var a in _attachments)
            if (!_game.ContainsKey(a.Hash) && a.Active)
            {
                a.Active = false;
                Log(LogLevel.Warn, $"kiln.abilities: attachment {a.Id} stays off: 0x{a.Hash:x8} is not a native the game registered (or no free slot)");
            }
        CheckAttachments();
    }

    private int KeysUsed() => _natives.Count(n => n.Keyed);

    // -1: no such program; 0: it does not call the native (at that site); 1: it does
    private int ProgramCalls(uint program, uint hash, uint site) =>
        !_programs.TryGetValue(program, out var calls) ? -1 : calls.Exists(c => c.Hash == hash && (site == 0 || c.Site == site)) ? 1 : 0;

    private void CheckAttachments()
    {
        foreach (var a in _attachments)
        {
            if (a.Program == 0) continue;
            var r = ProgramCalls(a.Program, a.Hash, a.Site);
            var on = r == 1;
            if (!on && (!a.Checked || a.Active))
                Log(LogLevel.Warn, $"kiln.abilities: attachment {a.Id} on native 0x{a.Hash:x8} stays off: " +
                                   (r < 0 ? $"no program 0x{a.Program:x8} in the ImpactRegistry" : $"the native is not called by program 0x{a.Program:x8}{(a.Site != 0 ? " at that site" : "")}"));
            else if (on && a.Checked && !a.Active)
                Log(LogLevel.Info, $"kiln.abilities: attachment {a.Id} on native 0x{a.Hash:x8} is on: program 0x{a.Program:x8} is in now");
            a.Active = on;
            a.Checked = true;
        }
    }

    // ---- the table ----

    public int Status(AbilitiesAbi.Status* s)
    {
        lock (_lock)
        {
            var size = s->Size;
            *s = default;
            s->Size = size;
            s->Natives = (byte)(_worldIn ? 1 : 0);
            s->VanillaNatives = (uint)_game.Count;
            s->NativeCapacity = 128;
            s->KilnNatives = (uint)_natives.Count(n => n.State != AbilitiesAbi.StateDead);
            s->NativeCalls = _syncCalls + _attachEntered;
            s->Step = _step;
        }
        return FakeTables.Ok;
    }

    private List<(uint Hash, string Name, uint Argc, uint Kind)> Listed()
    {
        var list = _game.Values.OrderBy(g => g.Name, StringComparer.Ordinal)
            .Select(g => (g.Hash, g.Name, 0u, _attachments.Exists(a => a.Hash == g.Hash) ? AbilitiesAbi.KindWrapped : AbilitiesAbi.KindVanilla)).ToList();
        list.AddRange(_natives.Where(n => n.State != AbilitiesAbi.StateDead).Select(n => (n.Hash, n.Name, n.Argc, AbilitiesAbi.KindSync)));
        return list;
    }

    public uint NativeCount()
    {
        lock (_lock) return (uint)Listed().Count;
    }

    public int NativeAt(uint index, AbilitiesAbi.NativeInfo* o)
    {
        lock (_lock)
        {
            var list = Listed();
            if (index >= list.Count) return FakeTables.NotFound;
            Fill(o, list[(int)index]);
            return FakeTables.Ok;
        }
    }

    public int NativeFind(uint hash, AbilitiesAbi.NativeInfo* o)
    {
        lock (_lock)
        {
            foreach (var e in Listed())
            {
                if (e.Hash != hash) continue;
                if (o == null || o->Size < (uint)sizeof(AbilitiesAbi.NativeInfo)) return FakeTables.InvalidArgument;
                Fill(o, e);
                return FakeTables.Ok;
            }
            return FakeTables.NotFound;
        }
    }

    private static void Fill(AbilitiesAbi.NativeInfo* o, (uint Hash, string Name, uint Argc, uint Kind) e)
    {
        var size = o->Size;
        *o = default;
        o->Size = size;
        o->Hash = e.Hash;
        o->Argc = e.Argc;
        o->Kind = e.Kind;
        var bytes = Encoding.UTF8.GetBytes(e.Name);
        for (var i = 0; i < Math.Min(bytes.Length, 63); i++) o->Name[i] = bytes[i];
    }

    public int Declare(uint hash, uint argc)
    {
        if (argc > 31) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            if (_game.ContainsKey(hash)) return FakeTables.InvalidArgument;
            _declared[hash] = argc;
        }
        return FakeTables.Ok;
    }

    public int DeclareTyped(uint hash, uint argc, AbilitiesAbi.NativeArg* args)
    {
        if (argc > 31 || (argc != 0 && args == null)) return FakeTables.InvalidArgument;
        var copy = new AbilitiesAbi.NativeArg[argc];
        for (var i = 0; i < argc; i++) copy[i] = args[i];
        lock (_lock)
        {
            if (_game.ContainsKey(hash)) return FakeTables.InvalidArgument;
            _typed[hash] = copy;
            if (Find(hash) is { } n)
            {
                if (n.Argc != argc) Log(LogLevel.Warn, $"kiln.abilities: native {n.Name} registered with argc {n.Argc} is declared with {argc} arguments");
                n.Typed = copy;
            }
            else
            {
                _declared[hash] = argc;
            }
        }
        return FakeTables.Ok;
    }

    private Native? Find(uint hash) => _natives.Find(n => n.Hash == hash);

    // natives_core.h valid_native_name: "<modid>.<Name>", Name [A-Za-z0-9_], 90 bytes at most
    private static bool ValidName(string full)
    {
        var dot = full.LastIndexOf('.');
        if (dot <= 0 || dot + 1 >= full.Length || Encoding.UTF8.GetByteCount(full) > 90) return false;
        for (var i = 0; i < dot; i++)
            if (!char.IsAsciiLetterLower(full[i]) && !char.IsAsciiDigit(full[i]) && full[i] is not ('.' or '_' or '-')) return false;
        for (var i = dot + 1; i < full.Length; i++)
            if (!char.IsAsciiLetterOrDigit(full[i]) && full[i] != '_') return false;
        return true;
    }

    public int RegisterSync(ulong owner, string name, uint argc, nint fn, nint user, uint* outHash, uint* refusal)
    {
        if (!OnTick) return FakeTables.WrongThread;
        if (FakeTables.Off(AbilitiesAbi.NativesFeature)) return FakeTables.Unavailable;
        if (fn == 0 || argc > 31 || !ValidName(name) || name.StartsWith("kiln.", StringComparison.Ordinal)) return FakeTables.InvalidArgument;
        var hash = AbilitiesAbi.Fnv(name);
        if (outHash != null) *outHash = hash;
        int Refuse(uint code, string why)
        {
            if (refusal != null) *refusal = code;
            Log(LogLevel.Warn, $"kiln.abilities: sync native {name} refused: {why}");
            return FakeTables.InvalidArgument;
        }
        lock (_lock)
        {
            if (_game.ContainsKey(hash)) return Refuse(AbilitiesAbi.RefusedVanilla, "collides with a vanilla native");
            var n = Find(hash);
            if (n is not null && n.State != AbilitiesAbi.StateDead) return Refuse(AbilitiesAbi.RefusedTaken, "a Kiln native with this name is registered");
            if (n is null)
            {
                if (_natives.Count + _game.Keys.Count(h => _attachments.Exists(a => a.Hash == h)) >= KilnSlots)
                    return Refuse(AbilitiesAbi.RefusedNoSlot, "no free native slot (Kiln has 160)");
                if (_worldIn && KeysUsed() >= RegistryRoom)
                    return Refuse(AbilitiesAbi.RefusedRegistryFull, "native registry full (the game takes 128 natives, 107 of them its own)");
                n = new Native { Hash = hash, Name = name };
                _natives.Add(n);
            }
            n.Argc = argc;
            n.Owner = owner;
            n.Fn = fn;
            n.User = user;
            if (_typed.TryGetValue(hash, out var typed)) n.Typed = typed;
            _declared.Remove(hash);
            // a revived native keeps its key; a new one takes one when the world is in, else at its setup
            if (_worldIn) n.Keyed = true;
            n.State = n.Keyed ? AbilitiesAbi.StateLive : AbilitiesAbi.StateWaiting;
        }
        Log(LogLevel.Info, $"kiln.abilities: sync native {name} (0x{hash:x8}, argc {argc}) registered");
        return FakeTables.Ok;
    }

    public int Unregister(uint hash)
    {
        if (!OnTick) return FakeTables.WrongThread;
        lock (_lock)
        {
            if (Find(hash) is not { } n || n.State == AbilitiesAbi.StateDead) return FakeTables.NotFound;
            TakeOff(n);
        }
        return FakeTables.Ok;
    }

    private static void TakeOff(Native n)
    {
        n.State = AbilitiesAbi.StateDead;
        n.Fn = 0;
        n.User = 0;
        n.Owner = 0;
    }

    public int ReleaseOwner(ulong owner)
    {
        if (!OnTick) return FakeTables.WrongThread;
        if (owner == 0) return FakeTables.InvalidArgument;
        lock (_lock)
        {
            foreach (var n in _natives)
                if (n.Owner == owner && n.State != AbilitiesAbi.StateDead) TakeOff(n);
            _attachments.RemoveAll(a => a.Owner == owner);
        }
        return FakeTables.Ok;
    }

    public int State(uint hash, uint* o)
    {
        lock (_lock)
        {
            *o = Find(hash) switch
            {
                null => AbilitiesAbi.StateNone,
                var n => n.State,
            };
        }
        return FakeTables.Ok;
    }

    public int Attach(ulong owner, uint hash, AbilitiesAbi.NativeFilter* filter, int stage, int priority, nint fn, nint user, ulong* handle,
        uint* refusal)
    {
        if (!OnTick) return FakeTables.WrongThread;
        if (FakeTables.Off(AbilitiesAbi.NativesFeature)) return FakeTables.Unavailable;
        if (fn == 0 || handle == null || stage is < AbilitiesAbi.Before or > AbilitiesAbi.After ||
            (filter != null && (filter->Size < (uint)sizeof(AbilitiesAbi.NativeFilter) || filter->Reserved != 0)))
            return FakeTables.InvalidArgument;
        int Refuse(uint code)
        {
            if (refusal != null) *refusal = code;
            return FakeTables.InvalidArgument;
        }
        var program = filter != null ? filter->ProgramId : 0;
        var site = filter != null ? filter->Site : 0;
        Attachment a;
        lock (_lock)
        {
            if (program != 0 && _worldIn)
            {
                var r = ProgramCalls(program, hash, site);
                if (r < 0) return Refuse(AbilitiesAbi.RefusedProgramNotLoaded);
                if (r == 0) return Refuse(AbilitiesAbi.RefusedSiteNotFound);
            }
            if (stage == AbilitiesAbi.Instead && _attachments.Exists(x => x.Stage == AbilitiesAbi.Instead && x.Hash == hash && x.Program == program && x.Site == site))
                return Refuse(AbilitiesAbi.RefusedSecondInstead);
            if (_worldIn && !_game.ContainsKey(hash)) return Refuse(AbilitiesAbi.RefusedNotANative);
            a = new Attachment
            {
                Id = ++_attachSeq, Owner = owner, Hash = hash, Program = program, Site = site, Stage = stage, Priority = priority,
                Fn = fn, User = user, Checked = program == 0 || _worldIn,
            };
            a.Order = a.Id;
            _attachments.Add(a);
        }
        *handle = a.Id;
        var name = stage == AbilitiesAbi.Before ? "Before" : stage == AbilitiesAbi.Instead ? "Instead" : "After";
        Log(LogLevel.Info, $"kiln.abilities: attachment {a.Id}: native 0x{hash:x8}, {name}, program 0x{program:x8}, site 0x{site:x8}, priority {priority}" +
                           (!_worldIn ? program != 0 ? " (checked when the world's programs are in)" : " (waits for the world's natives)" : ""));
        return FakeTables.Ok;
    }

    public int Detach(ulong handle)
    {
        if (!OnTick) return FakeTables.WrongThread;
        lock (_lock) return _attachments.RemoveAll(a => a.Id == handle) > 0 ? FakeTables.Ok : FakeTables.NotFound;
    }

    public int AttachState(ulong handle, uint* o)
    {
        lock (_lock)
        {
            *o = _attachments.Find(a => a.Id == handle) switch
            {
                null => AbilitiesAbi.AttachGone,
                { Checked: false } => AbilitiesAbi.AttachPending,
                { Active: true } => AbilitiesAbi.AttachActive,
                _ => AbilitiesAbi.AttachOff,
            };
        }
        return FakeTables.Ok;
    }

    public int Counters(AbilitiesAbi.NativeCounters* c)
    {
        AbilitiesAbi.NativeCounters k = default;
        lock (_lock)
        {
            k.Size = (uint)sizeof(AbilitiesAbi.NativeCounters);
            k.Inflight = (uint)Volatile.Read(ref _inflight);
            k.Flags = FakeTables.Off(AbilitiesAbi.NativesFeature) ? 0 : AbilitiesAbi.CountersNatives;
            k.SyncCalls = _syncCalls;
            k.SyncFaults = _syncFaults;
            k.DeadCalls = _deadCalls;
            k.AttachEntered = _attachEntered;
            k.AttachSkipped = _attachSkipped;
            k.TypedRefused = _typedRefused;
            k.TypedUnconnected = _typedUnconnected;
            k.AttachPending = (uint)_attachments.Count(a => !a.Checked);
            k.AttachActive = (uint)_attachments.Count(a => a.Checked && a.Active);
            k.AttachOff = (uint)_attachments.Count(a => a.Checked && !a.Active);
            k.EcsState = _ecsOn ? AbilitiesAbi.EcsOn : AbilitiesAbi.EcsPending;
        }
        // as the native service: the members the caller's size reaches
        var n = Math.Min(c->Size, (uint)sizeof(AbilitiesAbi.NativeCounters));
        Buffer.MemoryCopy(&k, c, n, n);
        c->Size = n;
        return FakeTables.Ok;
    }

    // ---- the calls ----

    public FakeImpactResult Run(FakeImpactCall call)
    {
        if (!FakeTables.OnTickThread) throw new InvalidOperationException("a native call runs from the harness's tick thread");
        var args = call.Args;
        if (args.Count > 31) throw new ArgumentException("an ECALL pops at most 31 values", nameof(call));
        // the instance's variables: one per variable argument, laid out one after the other (4-byte aligned)
        var layout = new AbilitiesAbi.Variable[args.Count];
        var handles = new uint[Math.Max(args.Count, 1)];
        var size = 0;
        var vars = 0;
        for (var i = 0; i < args.Count; i++)
        {
            var a = args[i] ?? throw new ArgumentException($"argument {i} is null", nameof(call));
            if (!a.IsVariable)
            {
                handles[i] = a.RawValue;
                continue;
            }
            size = (size + 3) & ~3;
            layout[vars] = new AbilitiesAbi.Variable { Name = (uint)i, Type = a.TypeHash, Size = (ushort)a.Bytes.Length, Offset = (ushort)size };
            handles[i] = 0xFFFF0000u | (uint)vars;
            size += a.Bytes.Length;
            vars++;
        }
        var data = (byte*)NativeMemory.AllocZeroed((nuint)Math.Max(size, 1));
        var lay = (AbilitiesAbi.Variable*)NativeMemory.AllocZeroed((nuint)(Math.Max(vars, 1) * sizeof(AbilitiesAbi.Variable)));
        var argv = (uint*)NativeMemory.AllocZeroed((nuint)(handles.Length * sizeof(uint)));
        var c = (AbilitiesAbi.NativeCall*)NativeMemory.AllocZeroed((nuint)sizeof(AbilitiesAbi.NativeCall));
        try
        {
            for (var i = 0; i < vars; i++) lay[i] = layout[i];
            for (var i = 0; i < handles.Length; i++) argv[i] = handles[i];
            for (var i = 0; i < args.Count; i++)
                if (args[i].IsVariable)
                {
                    var v = layout[handles[i] & 0xFFFF];
                    args[i].Bytes.CopyTo(new Span<byte>(data + v.Offset, v.Size));
                }
            var hash = HashOf(call.Native);
            c->Size = (uint)sizeof(AbilitiesAbi.NativeCall);
            c->Hash = hash;
            c->Argc = (uint)args.Count;
            c->Args = argv;
            c->Layout = (byte*)lay;
            c->LayoutCount = (ulong)vars;
            c->Data = data;
            c->DataSize = (ulong)size;
            c->ImpactId = call.ImpactId;
            c->ProgramId = call.Program;
            c->Site = call.Site;
            c->Self = call.Self.Value;
            c->OutError = AbilitiesAbi.Undecided;

            // impact_simulation's thread: never the tick thread, which waits for the step
            var handlers = 0;
            Exception? fault = null;
            var job = new Thread(() =>
            {
                try
                {
                    handlers = Dispatch(c);
                }
                catch (Exception e)
                {
                    fault = e;
                }
            }) { IsBackground = true, Name = "kiln.abilities fake: impact_simulation" };
            job.Start();
            job.Join();
            if (fault is not null) throw new InvalidOperationException($"the fake's call of {call.Native} failed: {fault.Message}", fault);

            var after = new byte[args.Count][];
            for (var i = 0; i < args.Count; i++)
                after[i] = args[i].IsVariable ? new ReadOnlySpan<byte>(data + layout[handles[i] & 0xFFFF].Offset, args[i].Bytes.Length).ToArray() : [];
            var result = new FakeImpactResult(c->OutError, c->OutValue, c->OriginalCalled != 0, handlers, after);
            lock (_lock) _calls.Add(new FakeImpactCallRecord(_step, call.Native, hash, call.Program, call.Site, result.Error, result.Value, handlers));
            return result;
        }
        finally
        {
            NativeMemory.Free(c);
            NativeMemory.Free(argv);
            NativeMemory.Free(lay);
            NativeMemory.Free(data);
        }
    }

    private uint HashOf(string native)
    {
        if (native.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) return Convert.ToUInt32(native[2..], 16);
        return native.Contains('.') ? AbilitiesAbi.Fnv(native) : AbilitiesAbi.Fnv(Pascal(native));
    }

    // One ECALL, on the job thread (abilities.cpp dispatch_sync / dispatch_wrapped). Answers the handlers that ran.
    private int Dispatch(AbilitiesAbi.NativeCall* c)
    {
        Interlocked.Increment(ref _inflight);
        var scope = new Scope { Call = c, Model = this };
        _scope = scope;
        try
        {
            Native? kiln;
            GameNative? game;
            List<Attachment> set;
            lock (_lock)
            {
                kiln = Find(c->Hash);
                game = _game.GetValueOrDefault(c->Hash);
                set = [.. _attachments.Where(a => a.Hash == c->Hash)];
                // the self-check of the first Kiln native call of the server world
                if (_worldIn && !_ecsOn && (kiln is not null || set.Count > 0))
                {
                    _ecsOn = true;
                    Log(LogLevel.Info, "kiln.abilities: NativeContext's reads and writes of other entities are on in this world (abilities.natives.ecs)");
                }
            }
            if (kiln is not null && game is null) return Sync(c, kiln);
            if (game is not null) return Wrapped(c, game, set, scope);
            c->OutError = AbilitiesAbi.ErrUnknownNative;  // a native the registry does not hold
            c->OutValue = 0;
            return 0;
        }
        finally
        {
            _scope = null;
            Interlocked.Decrement(ref _inflight);
        }
    }

    private int Sync(AbilitiesAbi.NativeCall* c, Native n)
    {
        nint fn, user;
        AbilitiesAbi.NativeArg[]? typed;
        lock (_lock)
        {
            if (n.State != AbilitiesAbi.StateLive || n.Fn == 0)
            {
                _deadCalls++;
                c->OutError = AbilitiesAbi.ErrUnknownNative;
                c->OutValue = 0;
                return 0;
            }
            (fn, user, typed) = (n.Fn, n.User, n.Typed);
        }
        if (typed is { Length: > 0 })
        {
            var e = CheckTyped(typed, c);
            if (e != 0)
            {
                lock (_lock)
                    if (e == AbilitiesAbi.ErrNoConnection) _typedUnconnected++;
                    else _typedRefused++;
                c->OutError = e;
                c->OutValue = 0;
                return 0;
            }
        }
        lock (_lock) _syncCalls++;
        ((delegate* unmanaged[Cdecl]<AbilitiesAbi.NativeCall*, void*, void>)fn)(c, (void*)user);
        if (c->OutError == AbilitiesAbi.Undecided)
        {
            c->OutError = 0;
            c->OutValue = 0;
        }
        if (c->OutError == AbilitiesAbi.ErrInternal)
            lock (_lock) _syncFaults++;
        return 1;
    }

    // natives_core.h check_typed_args
    private static byte CheckTyped(AbilitiesAbi.NativeArg[] decl, AbilitiesAbi.NativeCall* c)
    {
        if (c->Argc < decl.Length) return AbilitiesAbi.ErrTypeMismatch;
        var layout = (AbilitiesAbi.Variable*)c->Layout;
        for (var i = 0; i < decl.Length; i++)
        {
            var h = c->Args[i];
            var d = decl[i];
            if ((h & 0xFFFF0000u) != 0xFFFF0000u)
            {
                if ((d.Flags & AbilitiesAbi.ArgRaw) != 0) continue;
                if (h == 0)
                {
                    if ((d.Flags & AbilitiesAbi.ArgOptional) != 0) continue;
                    return AbilitiesAbi.ErrNoConnection;
                }
                return AbilitiesAbi.ErrTypeMismatch;
            }
            var idx = h & 0xFFFFu;
            if (idx >= c->LayoutCount || layout == null) return AbilitiesAbi.ErrTypeMismatch;
            var v = layout[idx];
            if ((d.Type != 0 && v.Type != d.Type) || (d.Size != 0 && v.Size != d.Size)) return AbilitiesAbi.ErrTypeMismatch;
            if ((ulong)v.Offset + v.Size > c->DataSize) return AbilitiesAbi.ErrTypeMismatch;
        }
        return 0;
    }

    // natives_core.h run_stages: every matching Before (high priority first); then, if nothing decided the call, one
    // matching Instead; then, if still undecided, the game's native, once; then every matching After (low first)
    private int Wrapped(AbilitiesAbi.NativeCall* c, GameNative game, List<Attachment> all, Scope scope)
    {
        scope.Game = game;
        static int High(Attachment x, Attachment y) => x.Priority != y.Priority ? y.Priority.CompareTo(x.Priority) : x.Order.CompareTo(y.Order);
        static int Low(Attachment x, Attachment y) => x.Priority != y.Priority ? x.Priority.CompareTo(y.Priority) : x.Order.CompareTo(y.Order);
        var before = all.Where(a => a.Stage == AbilitiesAbi.Before).ToList();
        var instead = all.Where(a => a.Stage == AbilitiesAbi.Instead).ToList();
        var after = all.Where(a => a.Stage == AbilitiesAbi.After).ToList();
        before.Sort(High);
        instead.Sort(High);
        after.Sort(Low);
        ulong entered = 0, skipped = 0;
        bool Matches(Attachment a) => a.Active && (a.Program == 0 || a.Program == c->ProgramId) && (a.Site == 0 || a.Site == c->Site);
        bool Decided() => c->OriginalCalled != 0 || c->OutError != AbilitiesAbi.Undecided;
        void Enter(Attachment a, int stage)
        {
            entered++;
            scope.Stage = stage;
            scope.Handlers++;
            ((delegate* unmanaged[Cdecl]<AbilitiesAbi.NativeCall*, void*, void>)a.Fn)(c, (void*)a.User);
        }
        foreach (var a in before)
            if (Matches(a)) Enter(a, AbilitiesAbi.Before);
            else skipped++;
        if (!Decided())
            foreach (var a in instead)
            {
                if (!Matches(a))
                {
                    skipped++;
                    continue;
                }
                Enter(a, AbilitiesAbi.Instead);
                break;
            }
        if (!Decided()) RunGame(c, scope);
        foreach (var a in after)
            if (Matches(a)) Enter(a, AbilitiesAbi.After);
            else skipped++;
        lock (_lock)
        {
            _attachEntered += entered;
            _attachSkipped += skipped;
        }
        if (c->OutError == AbilitiesAbi.Undecided)
        {
            c->OutError = 0;
            c->OutValue = 0;
        }
        return scope.Handlers;
    }

    // the game's native, once per call (the latch)
    private static void RunGame(AbilitiesAbi.NativeCall* c, Scope scope)
    {
        c->OriginalCalled = 1;
        byte value = 1;
        if (scope.Game?.Handler is { } handler) value = handler(new FakeGameNativeCall(c));
        c->OutError = 0;
        c->OutValue = value;
    }

    /// <summary>call_original: from inside a handler's call only, in a Before or an Instead of a game native, once.</summary>
    public static void CallOriginal(AbilitiesAbi.NativeCall* c)
    {
        var s = _scope;
        if (s is null || c == null || s.Call != c || c->OriginalCalled != 0 || s.Game is null || s.Stage == AbilitiesAbi.After) return;
        RunGame(c, s);
    }

    // ---- the ECS bridge (native_ecs_read / native_ecs_write) on the fake world's components ----

    private bool EcsHere(AbilitiesAbi.NativeCall* call) =>
        _scope is { } s && s.Call == call && call != null && ReferenceEquals(s.Model, this) && _ecsOn && !FakeTables.Off(AbilitiesAbi.EcsRow);

    public int EcsRead(AbilitiesAbi.NativeCall* call, string component, uint entity, byte* o, uint size)
    {
        if (!EcsHere(call)) return FakeTables.Unavailable;
        if (!OtherRead.Contains(component) && !OtherWrite.Contains(component)) return FakeTables.NotFound;
        // the tick thread waits for this call: the fake world is not read or written beside it
        if (_harness?.EntitiesFake?.Block(entity, WorldAbi.Hash(component)) is not { } slot) return FakeTables.NotFound;
        if (size > slot.Size) return FakeTables.InvalidArgument;
        Buffer.MemoryCopy((void*)slot.Data, o, size, size);
        return FakeTables.Ok;
    }

    public int EcsWrite(AbilitiesAbi.NativeCall* call, string component, uint entity, byte* value, uint size, uint flags, byte* before, byte* after)
    {
        if (!EcsHere(call)) return FakeTables.Unavailable;
        if (!OtherWrite.Contains(component)) return FakeTables.NotFound;
        if (_harness?.EntitiesFake?.Block(entity, WorldAbi.Hash(component)) is not { } slot) return FakeTables.NotFound;
        if (slot.Size != size) return FakeTables.InvalidArgument;
        var p = (byte*)slot.Data;
        if ((flags & AbilitiesAbi.EcsAttribute) != 0)
        {
            if (size < 8) return FakeTables.InvalidArgument;
            if (!new ReadOnlySpan<byte>(p, 8).SequenceEqual(new ReadOnlySpan<byte>(value, 8))) return FakeTables.InvalidArgument;
        }
        if (before != null) Buffer.MemoryCopy(p, before, size, size);
        Buffer.MemoryCopy(value, p, size, size);  // in place, as the game's accessor writes (no attribute formulas offline)
        if (after != null) Buffer.MemoryCopy(p, after, size, size);
        return FakeTables.Ok;
    }
}
