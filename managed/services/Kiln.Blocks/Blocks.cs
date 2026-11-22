// SPDX-License-Identifier: MIT
// The face of Kiln.Blocks: Blocks.Define in C#, blocks/*.json in data, both the same objects. Compiling is in Compiler.cs.
using System.Collections.Concurrent;
using System.Text.Json.Nodes;

namespace Kiln;

/// <summary>A block is wrong: which block, which field and why. Thrown by <see cref="Blocks.Define{T}"/>; a data file logs it.</summary>
public sealed class BlockError(string block, string? field, string problem)
    : ArgumentException($"block {block}{(string.IsNullOrEmpty(field) ? "" : $", {field}")}: {problem}")
{
    public string Block { get; } = block;
    public string? Field { get; } = field;
    public string Problem { get; } = problem;
}

/// <summary>
/// Building blocks: say what you mean, Kiln makes the game's resources. Call in <c>OnLoad</c>:
/// <code>
/// var chilled = Blocks.Define("chilled", new StatusBlock { Name = "Chilled", Seconds = 8 });
/// var burst = Blocks.Define("frost-burst", new EffectBlock { Damage = Damage.Ice(40), Applies = chilled });
/// Blocks.Define("frostbolt", new SpellBlock
/// {
///     Name = "Frostbolt",
///     Cast = new CastBlock { ManaCost = 30 },
///     Delivery = new ProjectileBlock { OnImpact = [new EffectBlock { Area = new AreaBlock { Radius = 3, Effects = [burst] } }] },
///     Recipe = new RecipeBlock { Makes = 10, Needs = { [GameItems.Material_T1_Twigs] = 2 } },
/// });
/// Blocks.Define("frost-edge", new WeaponBlock { Name = "Frost Edge", Class = WeaponClass.Sword, OnHit = [burst] });
/// </code>
/// The ids come from your mod id and the block's name: never write a GUID, and keep the names (a rename says <c>Was</c>).
/// docs/kiln/modding/blocks.md.
/// </summary>
public static partial class Blocks
{
    private static readonly ConcurrentDictionary<string, ModBlocks> Mods_ = new();

    /// <summary>The services blocks compile to are there on this build.</summary>
    public static bool Available => NewContent.Available && Content.GameDataAvailable && Assets.Available;

    /// <summary>Defines a block as the calling mod's. Compiling happens now: what is wrong throws <see cref="BlockError"/> here,
    /// and the mod does not load with a half-defined block.</summary>
    public static BlockHandle<T> Define<T>(string name, T block) where T : class, IBlock
    {
        ArgumentNullException.ThrowIfNull(block);
        // Mod.OnBoot stages with BootDefinitions: a live definition there would register before the generation is checked
        if (BootFrame.Current is { } boot)
            throw new InvalidOperationException($"PhaseClosed: Blocks.Define is refused inside {boot.ModId}'s OnBoot; stage the block with BootDefinitions.For(boot).Define");
        var mod = CallerMod();
        // a name of the mod's committed boot definitions (chapter 08 §4.2): the same block is a reference to it, another one is refused
        if (BootHandles.SameAsCommitted(mod.Id, name, block) is { } committed)
            return mod.Handles.TryGetValue(name, out var active) && active is BlockHandle<T> live ? live : (BlockHandle<T>)committed;
        // a block of a kind a mod registered (by its own type, not by T): compiled by its provider, applied by Kiln
        if (!KindRegistry.IsBuiltIn(block.GetType())) return KindDefine.Define(mod, name, block);
        return Compiler.Define(mod, name, block);
    }

    /// <summary>Defines a VFX as the calling mod's. The handle goes in a <c>Vfx</c> field, and to <see cref="Vfx"/>'s calls. The game lists VFX once at start, so one defined here is
    /// there from the next start; see <see cref="VfxBlockHandle.Status"/>. A block in a data file's "vfx" section is there at once.</summary>
    public static VfxBlockHandle Define(string name, VfxBlock block) => (VfxBlockHandle)Define<VfxBlock>(name, block);

    /// <summary>Defines a material as the calling mod's. Its <see cref="BlockHandle.Name"/> goes in a recipe's <c>material</c> and in <see cref="ParticleBlock.Material"/>. The game lists every
    /// material's textures once at start, so one defined here is there from the next start; see <see cref="MaterialBlockHandle.Status"/>. A block in a data file's "materials" section is there at once.</summary>
    public static MaterialBlockHandle Define(string name, MaterialBlock block) => (MaterialBlockHandle)Define<MaterialBlock>(name, block);

    /// <summary>Changes a game material in place, as the calling mod's. The game reads its material tables once at start, so a tweak new to this game is there from the next start;
    /// see <see cref="TableBlockHandle{T}.Status"/>. One in a data file's "materialTweaks" section is there at once.</summary>
    public static TableBlockHandle<MaterialTweakBlock> Define(string name, MaterialTweakBlock block) => (TableBlockHandle<MaterialTweakBlock>)Define<MaterialTweakBlock>(name, block);

    /// <summary>Defines a new building material as the calling mod's. The game reads its material tables once at start, so one new to this game is there from the next start;
    /// see <see cref="TableBlockHandle{T}.Status"/>. One in a data file's "buildingMaterials" section is there at once.</summary>
    public static TableBlockHandle<BuildingMaterialBlock> Define(string name, BuildingMaterialBlock block) => (TableBlockHandle<BuildingMaterialBlock>)Define<BuildingMaterialBlock>(name, block);

    /// <summary>Returns whether a pool is in the served boot table, durably accepted for the next start, or refused.
    /// RequireRegistered never writes pending storage. AcceptedNextStart consumes zero current entries and is rechecked at boot.</summary>
    [TickThreadOnly,RequiresCapability("worldtables.material_pools")]
#pragma warning disable KILNX028
    public static DefinitionRegistrationResult Define(string name,BuildingMaterialPoolBlock block,DefinitionRegistrationOptions options)
    {
        ArgumentNullException.ThrowIfNull(block);ArgumentNullException.ThrowIfNull(options);
        if(BootFrame.Current is {} boot)throw new InvalidOperationException($"PhaseClosed: Blocks.Define is refused inside {boot.ModId}'s OnBoot; stage the block instead");
        Game.AssertTickThread();
        return Compiler.DefinePoolResult(CallerMod(),name,block,options);
    }
#pragma warning restore KILNX028

    /// <summary>Defines a new terrain material as the calling mod's. The game reads its material tables once at start, so one new to this game is there from the next start;
    /// see <see cref="TableBlockHandle{T}.Status"/>. One in a data file's "terrainMaterials" section is there at once.</summary>
    public static TableBlockHandle<TerrainMaterialBlock> Define(string name, TerrainMaterialBlock block) => (TableBlockHandle<TerrainMaterialBlock>)Define<TerrainMaterialBlock>(name, block);

    /// <summary>Defines a loot table as the calling mod's. The handle goes in a creature's <c>Drops</c>, and rolls, drops and gives by itself: <see cref="LootTableHandle.Roll"/>. It asks nothing of players' games.</summary>
    public static LootTableHandle Define(string name, LootTableBlock block) => (LootTableHandle)Define<LootTableBlock>(name, block);
    /// <summary>Defines a quest as the calling mod's: <c>Quests.Define</c> gets what it lowers to. The handle's <see cref="QuestHandle.FullId"/> is what <c>Quests.Start</c> takes. The game builds
    /// its journal at start, so a quest defined here is in it from the next start. A quest in a data file's "quests" section
    /// is compiled during boot and is there on the first start; it does not need a generated quest file.</summary>
    public static QuestHandle Define(string name, QuestBlock block) => (QuestHandle)Define<QuestBlock>(name, block);

    /// <summary>Defines a dialogue as the calling mod's. The handle goes where a <see cref="DialogueDefinition"/> does (<c>new NpcBlock { Talk = handle }</c>); a block names it by its name (<c>TalkName</c>).</summary>
    public static DialogueHandle Define(string name, DialogueBlock block) => (DialogueHandle)Define<DialogueBlock>(name, block);

    /// <summary>Defines an affix (a kind of rolled modifier on one item) as the calling mod's: the same registration as
    /// <see cref="Affixes.Define"/> and a data file's "affixes" section. The handle's <see cref="AffixHandle.Id"/>,
    /// <c>"mod:name"</c>, is what rolled items keep. It asks nothing of players' games.</summary>
    public static AffixHandle Define(string name, AffixBlock block) => (AffixHandle)Define<AffixBlock>(name, block);

    /// <summary>Defines an affix pool (which items roll which affixes) as the calling mod's: the same as
    /// <see cref="Affixes.DefinePool"/> and a data file's "affixPools" section. Its entries name affixes of this mod's code or
    /// data files, or another loaded mod's. The handle rolls: <see cref="AffixPoolHandle.Roll"/>.</summary>
    public static AffixPoolHandle Define(string name, AffixPoolBlock block) => (AffixPoolHandle)Define<AffixPoolBlock>(name, block);

    /// <summary>Defines a rarity roll (which item types roll which rarity when they are crafted or dropped as loot) as the
    /// calling mod's: the same as a data file's "rarityRolls" section. It acts on the server, from the next item the game makes
    /// once the item types exist; a later definition wins an item type an earlier one also names. It asks nothing of players' games.</summary>
    public static BlockHandle<RarityRollBlock> Define(string name, RarityRollBlock block) => Define<RarityRollBlock>(name, block);

    /// <summary>Defines a rarity upgrade (which items it takes, up to which rarity, and what each step costs) as the calling
    /// mod's: the same as a data file's "rarityUpgrades" section. <c>Items.UpgradeRarity(player, item, "name")</c> runs it (or
    /// the handle's <c>ToString()</c>, <c>"mod:name"</c>, from another mod). It acts on the server and asks nothing of
    /// players' games.</summary>
    public static BlockHandle<RarityUpgradeBlock> Define(string name, RarityUpgradeBlock block) => Define<RarityUpgradeBlock>(name, block);

    /// <summary>A block a mod defined (its id, or null: the calling mod), or null. A block the mod staged in <see cref="Mod.OnBoot"/>
    /// and its boot generation committed is found from the commit on, also in <c>OnLoad</c> before its runtime activation and after the
    /// mod's runtime unload: the boot content stays in the game until the process ends.</summary>
    public static BlockHandle? Find(string name, string? mod = null)
    {
        var id = mod ?? BlocksBridge.CallerModId;
        if (id == null) return null;
        return Mods_.TryGetValue(id, out var m) && m.Handles.TryGetValue(name, out var h) ? h : BootHandles.Find(id, name);
    }

    /// <summary>What a block became, as JSON: the items, recipes, templates, buffs and programs, with the values given to them.
    /// The same document whichever way the block was written.</summary>
    public static string? Lowered(string name, string? mod = null)
    {
        var id = mod ?? BlocksBridge.CallerModId;
        return id != null && Mods_.TryGetValue(id, out var m) && m.Lowered.TryGetValue(name, out var j) ? j.ToJsonString(new() { WriteIndented = true }) : null;
    }

    /// <summary>The mods with blocks, each with its block names and the errors of its data files.</summary>
    public static IReadOnlyDictionary<string, (IReadOnlyList<string> Blocks, IReadOnlyList<string> Errors)> Report() =>
        Mods_.ToDictionary(m => m.Key, m => ((IReadOnlyList<string>)m.Value.Handles.Keys.OrderBy(x => x, StringComparer.Ordinal).ToList(), (IReadOnlyList<string>)m.Value.Errors.ToList()));

    /// <summary>What is not wrong but may not be what was meant: a block renamed without <c>Was</c>.</summary>
    public static IReadOnlyList<string> Warnings(string? mod = null)
    {
        var id = mod ?? BlocksBridge.CallerModId;
        if (id == null || !Mods_.TryGetValue(id, out var m)) return [];
        lock (m.Warnings) return m.Warnings.ToList();
    }

    /// <summary>Writes every program blocks made to <paramref name="directory"/> (<c>&lt;guid&gt;.bin</c> + <c>.txt</c>, for
    /// <c>tools/impact/impact.py dis</c> and <c>lint</c>). Returns how many.</summary>
    public static int DumpPrograms(string directory)
    {
        Directory.CreateDirectory(directory);
        var n = 0;
        foreach (var m in Mods_.Values)
            foreach (var (guid, (key, blob, listing)) in m.Programs)
            {
                File.WriteAllBytes(Path.Combine(directory, $"{guid}.bin"), blob);
                File.WriteAllText(Path.Combine(directory, $"{guid}.txt"), $"{m.Id} {key}\n{listing}");
                n++;
            }
        return n;
    }

    internal static IEnumerable<ModBlocks> AllMods => Mods_.Values;

    internal static ModBlocks ModFor(string id, Func<ulong> owner)
    {
        var m = Mods_.GetOrAdd(id, i => new ModBlocks(i, owner()));
        return m;
    }

    private static ModBlocks CallerMod()
    {
        var id = BlocksBridge.CallerModId ?? throw new InvalidOperationException("Blocks.Define must be called from mod code");
        BlocksStartup.Touch();
        var m = ModFor(id, BlocksBridge.CallerOwner);
        var owner = BlocksBridge.CallerOwner();
        // A reload has a new owner: what the old one had went with it. The mod's OnLoad and its data files make all of it again.
        if (m.Owner != owner)
        {
            if (m.OwnedByMod) m.Reset();
            m.Owner = owner;
        }
        m.OwnedByMod = true;
        BlocksStartup.ReadData(m);
        return m;
    }

    // ---- a VFX defined with an answer (since 0.2.0-dev.29; package 17a S2, chapter 17.1 §5 and I1) ----------------------------

    /// <summary>
    /// Client: defines a VFX as the calling mod's and says what became of it: registered in this process, accepted for the next
    /// start, or refused with a code and a reason (the two-argument <see cref="Define(string, VfxBlock)"/> gives a handle whose
    /// <see cref="VfxBlockHandle.Status"/> says less). The game lists its VFX once, before any mod loads, so a VFX registers in this
    /// process only when this start built that block with this content (from the mod's blocks files, its baked file, or a
    /// definition stored at an earlier start): <see cref="DefinitionRegistrationState.Registered"/>, 1 slot, once kiln.vfx has read
    /// the game's VFX table (at the first update of a client world) and found it there with the bytes Kiln served.
    /// <para><see cref="DefinitionTiming.RequireRegistered"/>: registered now, or refused with nothing stored: <c>RegistryClosed</c>
    /// (a new VFX, or a changed one), <c>NotReady</c> (built at this start, the table not read yet: ask
    /// <see cref="GetVfxRegistration"/> later), <c>GameRefused</c> (the game's table does not have it, or holds other data under
    /// its GUID), <c>CapacityExceeded</c>, <c>OwnerInactive</c>.</para>
    /// <para><see cref="DefinitionTiming.AllowNextStart"/>: registered when it is; otherwise, when only the moment stands in the way,
    /// the block is stored for the next start (<see cref="DefinitionRegistrationState.AcceptedNextStart"/>, no slot taken now) and
    /// that start builds it before the game lists its VFX, checking it again. A new VFX that would not fit (what the game's table
    /// holds, the new VFX accepted in this process, and this one, over the capacity <see cref="Vfx.ResourceBudget"/> reads) is
    /// refused <c>CapacityExceeded</c>, never stored. <c>StorageFailure</c> when it cannot be stored.</para>
    /// <para>The same definition again answers the same; its <see cref="DefinitionRegistrationResult.RegistrationId"/> reads the
    /// answer later. A change is posted as <see cref="DefinitionRegistrationChangedEvent"/> (and <see cref="CapacityChangedEvent"/>
    /// for the budget) at a later step while something listens. When the answer is Registered or AcceptedNextStart the block is
    /// the mod's as with the two-argument overload (other blocks and <see cref="Find"/> name it); a refusal leaves nothing.
    /// Client only: a dedicated server has no VFX (the two-argument overload and data files work on both sides). Not run in a
    /// game client yet (guided checks <c>17-vfx-budget.before</c> / <c>17-vfx-budget.after</c>).</para>
    /// </summary>
    /// <param name="name">The block's name: letters, digits, '-', '_' and '.'.</param>
    /// <param name="block">The VFX.</param>
    /// <param name="options">What the call may settle for.</param>
    /// <exception cref="ArgumentNullException"><paramref name="block"/> or <paramref name="options"/> is null.</exception>
    /// <exception cref="ArgumentException">An unknown timing.</exception>
    /// <exception cref="BlockError">The block is wrong (the builder's rules, as for a blocks file): nothing was stored.</exception>
    /// <exception cref="KilnUnavailableException"><c>blocks.vfx_registration</c>: no kiln.vfx here (a dedicated server), one older
    /// than 0.2.0-dev.29, or one that cannot check a registration on this game build or session.</exception>
    [ClientOnly]
    [TickThreadOnly]
    [RequiresCapability(VfxCaps.RegistrationRow)]
    public static DefinitionRegistrationResult Define(string name, VfxBlock block, DefinitionRegistrationOptions options)
    {
        ArgumentNullException.ThrowIfNull(block);
        var timing = DefinitionRegistration.Timing(options, nameof(options));
        Services.RequireTickThread("Blocks.Define(string, VfxBlock, DefinitionRegistrationOptions)");
        if (BootFrame.Current is { } boot)
            throw new InvalidOperationException($"PhaseClosed: Blocks.Define is refused inside {boot.ModId}'s OnBoot");
        if (!VfxDefinitions.CanDefine) throw VfxDefinitions.Off("Blocks.Define(name, VfxBlock, options)");
        return Compiler.DefineVfxWithAnswer(CallerMod(), name, block, timing);
    }

    /// <summary>Client: what became of a VFX definition since its <see cref="Define(string, VfxBlock, DefinitionRegistrationOptions)"/>
    /// call, by its <see cref="DefinitionRegistrationResult.RegistrationId"/>: registered once kiln.vfx read the game's table, kept
    /// for a mod that is no longer loaded (<see cref="DefinitionRegistrationState.Retained"/>, <c>OwnerInactive</c>, its slot still
    /// taken), superseded by a later definition of the block, stored for the next start. A definition stored at an earlier start
    /// is known again at this one, and says what this start made of it. <c>NotFound</c> for an id this process does not know. Any
    /// thread; it copies service state without waiting for the game or reading game memory. The defining overload can
    /// synchronously write a pending file when accepting for the next start; that I/O holds no snapshot lock.</summary>
    /// <exception cref="ArgumentException">The empty GUID.</exception>
    /// <exception cref="KilnUnavailableException"><c>blocks.vfx_registration</c>, as for the define.</exception>
    [ClientOnly]
    [AnyThread]
    [RequiresCapability(VfxCaps.RegistrationRow)]
    public static DefinitionRegistrationResult GetVfxRegistration(Guid registrationId)
    {
        DefinitionRegistration.CheckId(registrationId, nameof(registrationId));
        var a = VfxDefinitions.Receipt(registrationId);
        if (a.Rc == VfxNative.InvalidArgument) throw new ArgumentException(a.Reason ?? "not a registration id", nameof(registrationId));
        if (a.Rc == VfxNative.Unavailable) throw VfxCaps.Off(VfxCaps.RegistrationRow, a.Reason ?? "kiln.vfx's result-bearing definitions are off");
        if (a.Rc != VfxNative.Ok) throw new InvalidOperationException($"Blocks.GetVfxRegistration: kiln.vfx answered {a.Rc}");
        return DefinitionRegistration.ToResult(a.Reply, a.Report);
    }
}

/// <summary>What one mod defined.</summary>
internal sealed class ModBlocks(string id, ulong owner)
{
    public string Id { get; } = id;
    public ulong Owner { get; set; } = owner;
    public readonly ConcurrentDictionary<string, BlockHandle> Handles = new();
    public readonly ConcurrentDictionary<string, JsonObject> Lowered = new();
    public readonly ConcurrentDictionary<Guid, (string Key, byte[] Blob, string Listing)> Programs = new();
    public readonly List<string> Errors = new();
    public readonly List<string> Warnings = new();
    /// <summary>The text of VFX blocks read from data files, by name, until each is compiled: what the boot build read.</summary>
    public readonly Dictionary<string, string> VfxRaw = new();
    /// <summary>Templates carrying a VFX, defined once for the mod however many blocks play it.</summary>
    public readonly HashSet<Guid> Carriers = new();
    /// <summary>Game VFX named by a block, filled in when content is ready.</summary>
    public readonly List<VfxLate> VfxLates = new();
    public bool VfxPreloaded;
    /// <summary>Blocks of this mod's data files not compiled yet: a reference by name compiles them first.</summary>
    public readonly Dictionary<string, Func<BlockHandle>> Pending = new();
    /// <summary>The data files, read and not compiled yet; <see cref="DataLoaded"/>: compiled.</summary>
    public BlockJson.DataFiles? Data;
    public bool DataLoaded;
    public readonly HashSet<string> Compiling = new();
    public string? ManifestClients;
    /// <summary>The mod's folder while its OnBoot definitions lower (BootLowering.LowerOwner): no loaded mod lists the mod yet, so the
    /// files its blocks name are read against the folder the boot phase gives. Null on every other ledger.</summary>
    public string? BootDirectory;
    /// <summary>The folder a block's file of mod <paramref name="mod"/> is read against: the boot phase's for this ledger's own mod while its
    /// OnBoot definitions lower, else the loaded mod's (empty when no loaded mod is <paramref name="mod"/>).</summary>
    public string DirectoryOf(string mod) => BootDirectory is { Length: > 0 } dir && mod == Id ? dir : BlocksBridge.ModDirectory(mod);
    /// <summary>The owner is the mod's own, not one of Kiln's for a mod with only data files.</summary>
    public bool OwnedByMod;
    /// <summary>Set on the isolated ledger of a kind's plan (Kinds/KindTransaction.cs): the consumer's real ledger, whose blocks
    /// and data files a staged block may name. Null on every ledger a mod's own blocks compile into.</summary>
    public ModBlocks? StagesFor;
    /// <summary>A plan's ledger whose plan is being committed: only then do its blocks register.</summary>
    public volatile bool CommitOpen;
    /// <summary>A plan's ledger that was applied: it is the live ledger of the plan's blocks from then on (its rules and late
    /// programs run as a mod's do).</summary>
    public volatile bool Promoted;
    /// <summary>A plan's ledger whose owner was revoked: nothing of it runs again.</summary>
    public volatile bool Revoked;
    /// <summary>The runtime wanted its content-ready pass while the ledger was only staged.</summary>
    public bool HookWanted;
    /// <summary>A plan's ledger whose plan waits for its parts at content ready (KindApply): told once, after the first whole pass
    /// and its check of the plan's definitions, with every error the ledger holds then (none: the plan is applied).</summary>
    public Action<IReadOnlyList<string>>? Settle;
    /// <summary>Passes at content ready that started on this ledger.</summary>
    public int Passes;
    /// <summary>The first pass whose check may call <see cref="Settle"/>: a plan's replay in a later content generation waits for a pass that
    /// starts after it was armed (KindApply), not one the game began before.</summary>
    public int SettleFrom;
    /// <summary>The names of the definitions a plan's blocks registered: the only ones its pass checks (null on a mod's own ledger).</summary>
    public HashSet<string>? PlanDefinitions;
    /// <summary>The ledger whose data files and cycle check a lookup uses: this one, or the one it stages for.</summary>
    public ModBlocks Authored => StagesFor ?? this;

    /// <summary>A block by name: this ledger's, or (when staging) the consumer's.</summary>
    public bool TryHandle(string name, out BlockHandle handle) =>
        Handles.TryGetValue(name, out handle!) || (StagesFor != null && StagesFor.Handles.TryGetValue(name, out handle!));
    /// <summary>A rule was added since they were last applied: they are applied again though content did not change.</summary>
    public volatile bool RulesDirty;

    /// <summary>Forgets what the mod made: it is reloading, and makes it again.</summary>
    public void Reset()
    {
        QuestCompiler.Forget(this);
        Handles.Clear();
        Lowered.Clear();
        Programs.Clear();
        LateBuilders.Clear();
        lock (Errors) Errors.Clear();
        lock (Warnings) Warnings.Clear();
        ReadyRules.Clear();
        LatePending.Clear();
        LateFailed.Clear();
        LateRules.Clear();
        Icons.Clear();
        OwnIcons.Clear();
        ReadyChecks.Clear();
        PlayerTriggers.Forget(Id);
        LootRuntime.Forget(Id);
        AffixRuntime.Forget(Id);
        RoutineRegistry.Forget(Id);
        RarityRollRuntime.Forget(Id);
        ItemUpgrades.Forget(Id);
        MoveApplications.Forget(Id);
        ToolWearRuntime.Forget(this);
        UiRuntime.Forget(Id);
        ContainerRuntime.Forget(Id);
        InteractRuntime.Forget(Id);
        VfxRaw.Clear();
        VfxLates.Clear();
        Carriers.Clear();
        VfxPreloaded = false;
        Data?.Dispose();
        Data = null;
        DataLoaded = false;
        Pending.Clear();
        ReportDirty = true;
    }

    /// <summary>Something changed since the report for <c>kiln blocks</c> was written.</summary>
    public volatile bool ReportDirty;
    /// <summary>Programs that wait for values only the running game knows, by GUID.</summary>
    public readonly ConcurrentDictionary<Guid, ProgramBuilder> LateBuilders = new();
    public readonly List<Guid> LatePending = new();
    public readonly HashSet<Guid> LateFailed = new();
    public readonly List<(string Section, string Entry, string Path, Guid Program)> LateRules = new();
    public readonly List<(string Section, string Entry, GameDataRegistry Registry, string Source, string Block)> Icons = new();
    /// <summary>PNG icons of the mod's own statuses and perks.</summary>
    public readonly List<OwnIcon> OwnIcons = new();
    public bool ReadyHooked;
    public readonly List<Action<Action<string>>> ReadyChecks = new();
    public readonly List<(string Section, string Entry, JsonObject Rules)> ReadyRules = new();

    /// <summary>Declares that this mod's UI blocks show another mod's stat as <c>stats.&lt;mod&gt;/&lt;id&gt;</c> (since 0.2.0-dev.31;
    /// in a block file: <c>"statImports": ["other:id"]</c>). Call it before the blocks are defined. It is a declaration, not
    /// a permission: the stat's own read policy decides whether this game sees its value (until then it shows as unavailable).</summary>
    /// <exception cref="InvalidOperationException">Called outside a mod's code.</exception>
    [AnyThread]
    public static UiStatImportRegistration ImportStat(ModValueKey key)
    {
        if (string.IsNullOrEmpty(key.Mod) || string.IsNullOrEmpty(key.Id)) throw new ArgumentException("a stat key needs its mod and id", nameof(key));
        var mod = BlocksBridge.CallerModId ?? throw new InvalidOperationException("Blocks.ImportStat is called from a mod's code");
        return new UiStatImportRegistration(true, null, key, UiStats.Import(mod, key));
    }
}
