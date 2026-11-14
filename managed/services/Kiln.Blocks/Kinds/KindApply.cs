// SPDX-License-Identifier: MIT
// Applying a kind's plan (chapter 16d section 4): the frozen products are lowered by Kiln's own compilers into a ledger of
// the plan's own (KindTransaction), under a native owner of the plan's own, and nothing of it is published until every
// registration was taken. A refusal part way revokes that owner and the plan leaves nothing behind. A plan is bound to the
// mod instance that compiled it; the provider's code never runs here.
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;

namespace Kiln;

internal sealed class KindApplication
{
    public required BlockPlan Plan;
    public required ModBlocks Target;
    public required BlockHandle Handle;      // provisional until Applied; then what Blocks.Find returns
    public required KindStage Stage;
    public BlockApplyState State = BlockApplyState.PendingContent;
    public string Reason = "PendingContent";
    public ulong Generation, ContentGeneration, Owner;
    /// <summary>Registered under <see cref="Owner"/> and waiting for its parts at content ready (still PendingContent, nothing published).</summary>
    public bool Settling;
    /// <summary>Refused, and its owner could not be revoked: what it registered stays until the mod unloads (said in the reason).</summary>
    public bool LeftOver;
    /// <summary>Applied, and being replayed for this later content generation (lowered again, its parts at content ready run again):
    /// its new generation is published when that is done. Null while no replay waits.</summary>
    public ulong? Replay;
    /// <summary>The programs it put in the consumer's ledger when it was published.</summary>
    public Guid[] Published = [];

    public BlockApplyResult Result => new()
    {
        Ok = State == BlockApplyState.Applied, Reason = State == BlockApplyState.Applied ? "" : Reason, State = State, RequestId = Plan.RequestId,
        Generation = Generation, ContentGeneration = ContentGeneration, Guid = Handle.Guid, Id = Handle.Id, GameName = Handle.GameName,
    };
}

/// <summary>A plan lowered and not committed: the ledger, the blocks in commit order and the primary's handle.</summary>
internal sealed record KindStage(ModBlocks Ledger, IReadOnlyList<KindTransaction.StagedBlock> Blocks, BlockHandle Primary, ulong? ContentGeneration, long Bytes);

internal static class KindApply
{
    internal const long MaxPlanBytes = 16L << 20, MaxConsumerBytes = 64L << 20;

    private static readonly object Gate = new();
    private static readonly Dictionary<(object Consumer, string Name), KindApplication> Apps = new();
    private static readonly ConditionalWeakTable<BlockHandle, KindApplication> ByHandle = new();
    private static readonly HashSet<object> Watched = new(ReferenceEqualityComparer.Instance);
    private static ulong _generation;
    private static IDisposable? _tick;

    // ---- what the game answers; tests replace these ------------------------------------------------------------

    /// <summary>The content generation now, or null while content is not ready.</summary>
    internal static Func<ulong?> ContentGeneration = () => Content.Status() is { Ready: true } s ? s.Generation : null;
    /// <summary>A native owner for one plan.</summary>
    internal static Func<string, ulong> CreateOwner = BlocksBridge.ServiceOwner;
    /// <summary>Revokes a plan's owner: every layer it registered comes off. Null: this build cannot (then nothing is applied).</summary>
    internal static Action<ulong>? RevokeOwner
    {
        get => _revokeSet ? _revoke : BackendRevokes() ? BackendRevoke : null;
        set { _revoke = value; _revokeSet = true; }
    }
    private static Action<ulong>? _revoke;
    private static bool _revokeSet;
    private static readonly Action<ulong> BackendRevoke = owner => Backend.Current.OwnerRevoke(owner);
    private static bool BackendRevokes()
    {
        try { return Backend.Current.CanRevokeOwners; }
        catch (Exception e) when (e is InvalidOperationException or NotSupportedException) { return false; }
    }
    internal static Func<string, bool> CapabilityIs = Capabilities.Is;
    internal static Func<bool> OnTickThread = () => Game.IsTickThread;

    private static string Staged(BlockPlan plan, string key) => KindCompiler.StagedName(plan.Name, key);

    /// <summary>Private products occupy names too, until their consumer or pending request goes away.</summary>
    internal static bool NameReserved(ModBlocks target, string name, string? exceptPlan = null)
    {
        lock (Gate) return Apps.Values.Any(a => ReferenceEquals(a.Target, target) && a.Plan.Name != exceptPlan
            && a.State is BlockApplyState.PendingContent or BlockApplyState.Applied && a.Stage.Blocks.Any(b => b.Name == name));
    }

    private static IBlock Parse(BlockProduct p) => p.Kind switch
    {
        BuiltInBlockProductKind.Item => BlockJson.Parse<ItemBlock>(p.CanonicalJson),
        BuiltInBlockProductKind.Weapon => BlockJson.Parse<WeaponBlock>(p.CanonicalJson),
        BuiltInBlockProductKind.Spell => BlockJson.Parse<SpellBlock>(p.CanonicalJson),
        BuiltInBlockProductKind.Status => BlockJson.Parse<StatusBlock>(p.CanonicalJson),
        _ => BlockJson.Parse<EffectBlock>(p.CanonicalJson),
    };

    private static BlockHandle Define(ModBlocks target, string name, IBlock block) => block switch
    {
        ItemBlock b => Compiler.Define(target, name, b),
        WeaponBlock b => Compiler.Define(target, name, b),
        SpellBlock b => Compiler.Define(target, name, b),
        StatusBlock b => Compiler.Define(target, name, b),
        _ => Compiler.Define(target, name, (EffectBlock)block),
    };

    /// <summary>Lowers the plan's products into a ledger of their own. Registers nothing. What is wrong with a product is a
    /// <see cref="BlockError"/> that names the plan's block.</summary>
    internal static KindStage Stage(ModBlocks target, BlockPlan plan)
    {
        KindCompiler.RefuseInsideCompile("Blocks.Apply");
        using var tx = KindTransaction.Begin(target);
        var label = $"{plan.KindId} {plan.Name}";
        // what the products expand to is counted while Kiln's compilers emit it (R3): a refusal comes before the work, not after
        using var budget = KindBudget.Open(label);
        try
        {
            // each product waits under its staged name: one that names another (ProductName) stages it first
            foreach (var p in plan.Products)
            {
                var (name, product) = (Staged(plan, p.Key), p);
                if (target.Handles.ContainsKey(name) || target.Pending.ContainsKey(name) || NameReserved(target, name, plan.Name))
                    throw new BlockError(label, p.Key, $"mod {target.Id} has a block \"{name}\" already: a kind's plan never replaces one");
                BlockHandle? done = null;
                tx.Stage.Pending[name] = () => done ??= Define(target, name, Parse(product));
            }
            foreach (var p in plan.Products)
            {
                var name = Staged(plan, p.Key);
                if (tx.Stage.Handles.ContainsKey(name)) continue;
                if (!tx.Stage.Compiling.Add(name)) throw new BlockError(label, p.Key, "the product refers back to itself");
                try { tx.Stage.Pending[name](); }
                finally { tx.Stage.Compiling.Remove(name); }
            }
        }
        catch (BlockError e) when (!e.Block.StartsWith(plan.KindId, StringComparison.Ordinal))
        {
            throw new BlockError(label, e.Field, $"{e.Block}: {e.Problem}");
        }
        catch (System.Text.Json.JsonException e)
        {
            throw new BlockError(label, null, $"a frozen product does not read back as its block: {e.Message}");
        }
        tx.Stage.Pending.Clear();
        var bytes = tx.Staged.Sum(s => s.Low.GeneratedBytes);
        if (bytes > MaxPlanBytes) throw new BlockError(label, null, $"the plan makes {bytes >> 20} MiB of programs and definitions; at most {MaxPlanBytes >> 20} MiB");
        var primary = tx.Stage.Handles[Staged(plan, plan.PrimaryKey)];
        return new KindStage(tx.Stage, [.. tx.Staged], primary, ContentGeneration(), bytes);
    }

    /// <summary>The identities of live plans that are compiled and not refused: a later compile may name them.</summary>
    internal static List<(string, string, KindCompiler.Identity)> Provisional(KindOwner consumer)
    {
        lock (Gate)
            return [.. Apps.Where(a => a.Value.Plan.Consumer.Alive() && a.Value.State is BlockApplyState.PendingContent or BlockApplyState.Applied)
                .Select(a => (a.Value.Plan.ConsumerModId, a.Key.Name, new KindCompiler.Identity(a.Value.Handle.Guid, a.Value.Handle.GameName, a.Value.Handle.BlockType, a.Value.Plan.KindId)
                { Application = () => a.Value.Plan.Consumer.Alive() ? a.Value.State : BlockApplyState.Removed }))];
    }

    private static void CheckCaller(BlockPlan plan, KindOwner caller, string api)
    {
        if (!plan.Consumer.Same(caller))
            throw new ArgumentException($"{api}: the plan of \"{plan.Name}\" was compiled by {(plan.ConsumerModId == caller.ModId ? "an earlier load of this mod" : $"mod {plan.ConsumerModId}")}; a plan is applied by the mod instance that compiled it");
        if (!caller.Alive()) throw new ArgumentException($"{api}: mod {caller.ModId} is unloading");
    }

    /// <summary>Takes a plan for its consumer: staged at once (so the identity is known and what is wrong throws here), applied
    /// at once when it can be, else when content is ready. <paramref name="handle"/> makes the block's handle from the primary's identity.</summary>
    internal static KindApplication Submit(ModBlocks target, KindOwner caller, BlockPlan plan, Func<KindStage, BlockHandle> handle)
    {
        CheckCaller(plan, caller, "Blocks.Apply");
        var key = (plan.Consumer.Token, plan.Name);
        KindApplication? before;
        lock (Gate) Apps.TryGetValue(key, out before);
        if (before != null)
        {
            if (before.State == BlockApplyState.Applied)
            {
                if (before.Plan.InputHash == plan.InputHash) return before;   // the same plan again: the same answer, no new layer, no event
                return new KindApplication { Plan = plan, Target = target, Handle = before.Handle, Stage = before.Stage, State = BlockApplyState.Refused, Reason = "NameAlreadyApplied: reload the mod to change a block that is applied" };
            }
            if (before.State == BlockApplyState.PendingContent && before.Plan.InputHash == plan.InputHash) return Pump(before);
            if (plan.RequestId < before.Plan.RequestId)
                return new KindApplication { Plan = plan, Target = target, Handle = before.Handle, Stage = before.Stage, State = BlockApplyState.Refused, Reason = "StaleRequest: a later compile of this block was submitted" };
        }
        if (target.Handles.ContainsKey(plan.Name) || NameReserved(target, plan.Name)) throw new BlockError($"{plan.KindId} {plan.Name}", null, $"mod {target.Id} has a block \"{plan.Name}\" already");
        var stage = Stage(target, plan);
        long others;
        lock (Gate) others = Apps.Where(a => ReferenceEquals(a.Key.Consumer, plan.Consumer.Token) && a.Key.Name != plan.Name && a.Value.State != BlockApplyState.Refused).Sum(a => a.Value.Stage.Bytes);
        if (others + stage.Bytes > MaxConsumerBytes) throw new BlockError($"{plan.KindId} {plan.Name}", null, $"mod {target.Id}'s plans make more than {MaxConsumerBytes >> 20} MiB together");
        var app = new KindApplication { Plan = plan, Target = target, Stage = stage, Handle = handle(stage) };
        if (before != null)
        {
            TakeBack(before);   // a request that was registered and waiting for its parts at content ready
            Set(before, BlockApplyState.Removed, "Removed: a later compile replaced the pending or refused request");
        }
        lock (Gate)
        {
            Apps[key] = app;
            ByHandle.AddOrUpdate(app.Handle, app);
            if (Watched.Add(plan.Consumer.Token) && plan.Consumer.Token is ModContext mod)
            {
                var token = plan.Consumer.Token;
                mod.Retiring += () => RetireConsumer(token);
            }
            try { _tick ??= BlocksBridge.AddTick("kiln blocks kinds", _ => PumpAll()); }
            catch (Exception e) when (e is InvalidOperationException or NotSupportedException) { }
        }
        if (before != null) Changed(app, before.Result);
        return Pump(app);
    }

    /// <summary>What <see cref="Blocks.Application(BlockPlan)"/> answers: the state of the plan's block for its consumer.</summary>
    internal static BlockApplyResult Of(BlockPlan plan)
    {
        lock (Gate)
        {
            if (Apps.TryGetValue((plan.Consumer.Token, plan.Name), out var app) && app.Plan.InputHash == plan.InputHash) return app.Result;
        }
        return new BlockApplyResult { Ok = false, State = BlockApplyState.Removed, Reason = "NotSubmitted: the plan was not applied, or its mod was unloaded", RequestId = plan.RequestId };
    }

    internal static BlockApplyResult? Of(BlockHandle handle) => ByHandle.TryGetValue(handle, out var app) ? app.Result : null;

    /// <summary>A byte-identical replay can have a different generic view of the same record. Its handle still tracks the one application.</summary>
    internal static BlockHandle<T> HandleAs<T>(BlockHandle handle) where T : class, IBlock
    {
        if (handle is BlockHandle<T> typed) return typed;
        var app = ByHandle.TryGetValue(handle, out var found) ? found : throw new ArgumentException("the handle has no block kind application", nameof(handle));
        var view = new BlockHandle<T>(handle.Mod, handle.Name, handle.Guid, handle.GameName, (T)app.Plan.Projection!);
        ByHandle.AddOrUpdate(view, app);
        return view;
    }

    private static KindApplication Pump(KindApplication app)
    {
        if (app.State == BlockApplyState.PendingContent && !app.Settling && OnTickThread()) TryCommit(app);
        return app;
    }

    /// <summary>The tick: plans that wait for content are tried again, and applied plans are replayed in a later content generation.</summary>
    internal static void PumpAll()
    {
        KindApplication[] waiting, stale = [];
        var now = ContentGeneration();
        lock (Gate)
        {
            waiting = [.. Apps.Values.Where(a => a.State == BlockApplyState.PendingContent && !a.Settling)];
            if (now is { } g) stale = [.. Apps.Values.Where(a => a.State == BlockApplyState.Applied && a.ContentGeneration != g && a.Replay != g)];
        }
        foreach (var app in waiting) TryCommit(app);
        if (now is { } generation)
            foreach (var app in stale) Relower(app, generation);
    }

    private static void Set(KindApplication app, BlockApplyState state, string reason)
    {
        var before = app.Result;
        app.State = state;
        app.Reason = reason;
        Changed(app, before);
    }

    private static void Changed(KindApplication app, BlockApplyResult before)
    {
        var after = app.Result;
        if (before.State == after.State && before.Generation == after.Generation && before.RequestId == after.RequestId) return;
        KindEvents.Queue(new BlockApplicationChangedEvent(app.Plan.ConsumerModId, app.Plan.Name, before, after));
    }

    private static void TryCommit(KindApplication app)
    {
        var plan = app.Plan;
        var label = $"{plan.KindId} {plan.Name}";
        if (!plan.Consumer.Alive()) return;   // its retirement takes it away
        if (plan.Kind.Retired) { Set(app, BlockApplyState.Refused, $"ProviderRetired: kind {plan.KindId} was taken away before the plan was applied"); return; }
        var dependencies = plan.Dependencies.Select(d => d.Application?.Invoke() ?? BlockApplyState.Applied).ToArray();
        if (dependencies.Any(s => s is BlockApplyState.Refused or BlockApplyState.Removed))
        { Set(app, BlockApplyState.Refused, "DependencyRefused: a block resolved during Compile was refused, replaced or unloaded"); return; }
        if (dependencies.Any(s => s == BlockApplyState.PendingContent)) return;
        if (ContentGeneration() is not { } generation) return;   // PendingContent: tried again on the tick
        foreach (var need in plan.Requires)
            if (!CapabilityIs(need)) { Set(app, BlockApplyState.Refused, $"RequiresOff: kind {plan.KindId} needs {need}, which is off here"); return; }
        var have = (app.Target.ManifestClients ??= BlocksBridge.ManifestClients(app.Target.Id)) switch { "none" => ClientsLevel.None, "mod" => ClientsLevel.Mod, _ => ClientsLevel.Kiln };
        if (have < plan.Clients) { Set(app, BlockApplyState.Refused, $"ClientsTooLow: kind {plan.KindId} asks \"clients\": \"{plan.Clients.ToString().ToLowerInvariant()}\" of mod {app.Target.Id}"); return; }
        if (RevokeOwner is not { } revoke) { Set(app, BlockApplyState.Refused, "blocks.apply is off: this build cannot take a refused plan's registrations back"); return; }
        if (app.Target.Handles.ContainsKey(plan.Name)) { Set(app, BlockApplyState.Refused, $"NameTaken: mod {app.Target.Id} defined a block \"{plan.Name}\" in the meantime"); return; }
        // the plan was lowered against another content generation: lowered again, against this one
        if (app.Stage.ContentGeneration != generation)
        {
            try { app.Stage = Stage(app.Target, plan); }
            catch (KindDependencyPending) { return; }
            catch (BlockError e) { Set(app, BlockApplyState.Refused, $"DependencyRefused: {e.Message}"); return; }
        }
        if (app.Stage.Blocks.Any(b => app.Target.Handles.ContainsKey(b.Name) || NameReserved(app.Target, b.Name, plan.Name)))
        { Set(app, BlockApplyState.Refused, "NameTaken: a generated block name was defined while the plan waited"); return; }
        ulong owner = 0;
        var ledger = app.Stage.Ledger;
        var tx = KindTransaction.Resume(app.Target, ledger);
        try
        {
            owner = CreateOwner($"blocks:{app.Target.Id}:kind:{plan.Name}");
            if (owner == 0) throw new InvalidOperationException("no owner could be made for the plan");
            ledger.Owner = owner;
            tx.BeginCommit();
            foreach (var staged in app.Stage.Blocks) staged.Low.CommitRegistrations();
            if (ContentGeneration() != generation) throw new KindStale();
            foreach (var staged in app.Stage.Blocks) staged.Low.CommitAfter();
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            tx.Dispose();
            ledger.Revoked = true;
            if (owner != 0)
            {
                try { revoke(owner); }
                catch (Exception x) when (x is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {label}: its owner could not be revoked: {x.Message}"); }
            }
            if (e is KindStale)
            {
                // content changed under the attempt: nothing of it stays, and the plan is tried again against the new content
                app.Stage = app.Stage with { ContentGeneration = null };
                BlocksBridge.Log(LogLevel.Info, $"blocks: {app.Target.Id}: block {label}: StaleContent, the attempt was taken back and is tried again");
                return;
            }
            BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {label} was refused, and nothing of it stays: {e.Message}");
            Set(app, BlockApplyState.Refused, $"Refused: {e.Message}");
            return;
        }
        tx.Dispose();
        // every registration was taken
        app.Owner = owner;
        app.ContentGeneration = generation;
        if (ledger.HookWanted)
        {
            // Parts of the plan run at content ready: its rules, programs that name the game's values, checks and icons. The plan stays
            // PendingContent, with nothing published, until its first pass and the check of its definitions are done: then it is applied, or
            // refused and taken back (Settled). The ledger is told before it is promoted, as a pass may run at once.
            app.Settling = true;
            app.Reason = "PendingContent: registered; waiting for its parts at content ready (rules, programs that name the game's values, checks)";
            ledger.PlanDefinitions = [.. app.Stage.Blocks.SelectMany(b => b.Low.DefinitionNames)];
            ledger.Settle = problems => Settled(app, ledger, problems);
            BlockRuntime.Promote(ledger);
            return;
        }
        BlockRuntime.Promote(ledger);
        Publish(app, generation);
    }

    /// <summary>The plan's parts at content ready are done (<paramref name="problems"/>: what went wrong, none when nothing did). Applied, or
    /// refused with the first problem and taken back as any refused plan: its owner is revoked. When that fails, the reason says what stays.</summary>
    private static void Settled(KindApplication app, ModBlocks ledger, IReadOnlyList<string> problems)
    {
        lock (Gate)
        {
            if (!app.Settling) return;   // replaced or retired while it waited: taken back there
            app.Settling = false;
        }
        var plan = app.Plan;
        var label = $"{plan.KindId} {plan.Name}";
        if (!plan.Consumer.Alive() || app.State != BlockApplyState.PendingContent) return;   // its retirement takes it away
        var cause = "ContentReadyFailed";
        if (problems.Count == 0)
        {
            if (!app.Target.Handles.TryGetValue(plan.Name, out var other) || ReferenceEquals(other, app.Handle))
            {
                Publish(app, app.ContentGeneration);
                return;
            }
            cause = "NameTaken";
            problems = [$"mod {app.Target.Id} defined a block \"{plan.Name}\" while the plan waited for content ready"];
        }
        ledger.Revoked = true;   // nothing of it runs again
        var left = "";
        if (RevokeOwner is not { } revoke) left = "; this build cannot take its registrations back: they stay until the mod unloads";
        else
            try { revoke(app.Owner); }
            catch (Exception x) when (x is not OutOfMemoryException) { left = $"; its registrations could not be taken back ({x.Message}): they stay until the mod unloads"; }
        app.LeftOver = left.Length > 0;
        var more = problems.Count > 1 ? $" (and {problems.Count - 1} more: kiln.log, or kiln blocks)" : "";
        BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {label} was refused at content ready{(left.Length > 0 ? "" : ", and nothing of it stays")}: {problems[0]}{more}{left}");
        Set(app, BlockApplyState.Refused, $"{cause}: {problems[0]}{more}{left}");
    }

    /// <summary>A plan that is registered and waiting for its parts at content ready is taken back (replaced, or its mod unloads).</summary>
    private static void TakeBack(KindApplication app)
    {
        lock (Gate)
        {
            if (!app.Settling) return;
            app.Settling = false;
        }
        app.Stage.Ledger.Revoked = true;
        app.Stage.Ledger.Settle = null;
        try { RevokeOwner?.Invoke(app.Owner); }
        catch (Exception x) when (x is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {app.Plan.Name}: its owner could not be revoked: {x.Message}"); }
    }

    /// <summary>Every registration was taken and every part is done: the block exists from here on, in a new application generation
    /// (the first, or one for a later content generation after its replay).</summary>
    private static void Publish(KindApplication app, ulong contentGeneration)
    {
        var plan = app.Plan;
        var label = $"{plan.KindId} {plan.Name}";
        var ledger = app.Stage.Ledger;
        var replay = app.State == BlockApplyState.Applied;
        var json = new JsonObject { ["kind"] = plan.KindId, ["primary"] = app.Stage.Primary.Name, ["inputHash"] = plan.InputHash };
        var products = new JsonObject();
        foreach (var staged in app.Stage.Blocks) products[staged.Name] = staged.Low.Json.DeepClone();
        json["products"] = products;
        foreach (var guid in app.Published.Where(g => !ledger.Programs.ContainsKey(g))) app.Target.Programs.TryRemove(guid, out _);
        foreach (var (guid, program) in ledger.Programs) app.Target.Programs[guid] = program;
        app.Published = [.. ledger.Programs.Keys];
        app.Target.Handles[plan.Name] = app.Handle;
        app.Target.Lowered[plan.Name] = json;
        app.Target.ReportDirty = true;
        var before = app.Result;
        lock (Gate) app.Generation = ++_generation;
        app.ContentGeneration = contentGeneration;
        app.State = BlockApplyState.Applied;
        app.Reason = "";
        BlocksBridge.Log(LogLevel.Info, replay
            ? $"blocks: {app.Target.Id}: {label} replayed for content generation {contentGeneration} (application generation {app.Generation})"
            : $"blocks: {app.Target.Id}: {label} -> {app.Stage.Blocks.Count} block(s) of Kiln's own ({string.Join(", ", app.Stage.Blocks.Select(b => b.Label))})");
        Changed(app, before);
    }

    // ---- a later content generation (16d review R2) ---------------------------------------------------------------------

    /// <summary>An applied plan in a later content generation (the game reloaded its content, or a new world): its frozen products are
    /// lowered again by Kiln's compilers against this content, never by the provider. A lowering that registers the same keeps its
    /// registrations and replays its parts at content ready; another one is registered again under a new owner. The plan's new
    /// generation is published only when that is done; a failure refuses it and revokes its owner (<see cref="Withdraw"/>).</summary>
    private static void Relower(KindApplication app, ulong generation)
    {
        var plan = app.Plan;
        if (!plan.Consumer.Alive() || app.State != BlockApplyState.Applied) return;   // its retirement takes it away
        var dependencies = plan.Dependencies.Select(d => d.Application?.Invoke() ?? BlockApplyState.Applied).ToArray();
        if (dependencies.Any(s => s is BlockApplyState.Refused or BlockApplyState.Removed))
        {
            Withdraw(app, generation, "DependencyRefused", "a block resolved during Compile was refused, replaced or unloaded");
            return;
        }
        if (dependencies.Any(s => s == BlockApplyState.PendingContent)) return;   // tried again on the tick
        KindStage stage;
        try { stage = Stage(app.Target, plan); }
        catch (KindDependencyPending) { return; }
        catch (BlockError e)
        {
            Withdraw(app, generation, "RelowerFailed", e.Message);
            return;
        }
        if (stage.Primary.Guid != app.Handle.Guid || stage.Primary.GameName != app.Handle.GameName)
        {
            Withdraw(app, generation, "RelowerFailed", $"its primary lowers to {stage.Primary.GameName} ({stage.Primary.Guid}), not {app.Handle.GameName} ({app.Handle.Guid})");
            return;
        }
        // programs that name the game's values, finished against this content (the copy is the new lowering's own)
        var late = new Dictionary<Guid, byte[]>();
        foreach (var (guid, builder) in stage.Ledger.LateBuilders)
        {
            if (builder.Finish(guid, BlockRuntime.Host.ResolveLate, out var failed) is not { } blob)
            {
                Withdraw(app, generation, "ContentReadyFailed", $"block {failed!.Block}, {failed.Path}: the game has no {failed.Kind} \"{failed.Value}\"");
                return;
            }
            late[guid] = blob;
        }
        var had = app.Stage.Ledger;
        var hadLate = had.LateBuilders.Keys.ToDictionary(g => g, g => had.Programs.TryGetValue(g, out var p) ? p.Blob : []);
        if (Digest(stage, late) == Digest(app.Stage, hadLate))
        {
            Arm(app, had, generation);
            return;
        }
        Reapply(app, stage, generation);
    }

    /// <summary>What a lowering registers and runs, as one hash: two lowerings with the same digest register the same.</summary>
    private static string Digest(KindStage stage, IReadOnlyDictionary<Guid, byte[]> late)
    {
        using var h = System.Security.Cryptography.IncrementalHash.CreateHash(System.Security.Cryptography.HashAlgorithmName.SHA256);
        void Text(string s)
        {
            h.AppendData(System.Text.Encoding.UTF8.GetBytes(s));
            h.AppendData([0]);
        }
        foreach (var b in stage.Blocks)
        {
            Text(b.Name);
            Text(KindJson.Canonical(b.Low.Json));
            b.Low.Digest(h);
        }
        foreach (var (guid, blob) in late.OrderBy(x => x.Key))
        {
            Text(guid.ToString());
            h.AppendData(blob);
            h.AppendData([0]);
        }
        return Convert.ToHexString(h.GetHashAndReset());
    }

    /// <summary>The same registrations stay: the plan's parts at content ready run again for <paramref name="generation"/>, and only a pass
    /// that starts from now on is that replay (one the game began before is not). A newer generation arms again, and this one's
    /// callback is discarded.</summary>
    private static void Arm(KindApplication app, ModBlocks ledger, ulong generation)
    {
        lock (Gate) app.Replay = generation;
        if (!ledger.HookWanted)
        {
            Replayed(app, ledger, generation, []);   // nothing of it runs at content ready
            return;
        }
        ledger.SettleFrom = ledger.Passes + 1;
        ledger.Settle = problems => Replayed(app, ledger, generation, problems);
        ledger.RulesDirty = true;   // the pass runs at the next ready tick, whether the game's own replay of this content ran already or not
    }

    /// <summary>The plan lowers differently against this content: its registrations come off (kiln.content adopts the same GUIDs registered
    /// again), and the new lowering registers under a new owner of its own, then replays as at its first content ready.</summary>
    private static void Reapply(KindApplication app, KindStage stage, ulong generation)
    {
        var plan = app.Plan;
        var label = $"{plan.KindId} {plan.Name}";
        var had = app.Stage.Ledger;
        had.Revoked = true;
        had.Settle = null;
        if (RevokeOwner is not { } revoke)
        {
            Withdraw(app, generation, "RelowerFailed", "it lowers differently against this content, and this build cannot take its registrations back");
            return;
        }
        try { revoke(app.Owner); }
        catch (Exception x) when (x is not OutOfMemoryException)
        {
            Withdraw(app, generation, "RelowerFailed", $"it lowers differently against this content, and its registrations could not be taken back ({x.Message})");
            return;
        }
        BlocksBridge.Log(LogLevel.Info, $"blocks: {app.Target.Id}: block {label} lowers differently against content generation {generation}: registered again");
        ulong owner = 0;
        var ledger = stage.Ledger;
        var tx = KindTransaction.Resume(app.Target, ledger);
        try
        {
            owner = CreateOwner($"blocks:{app.Target.Id}:kind:{plan.Name}");
            if (owner == 0) throw new InvalidOperationException("no owner could be made for the plan");
            ledger.Owner = owner;
            tx.BeginCommit();
            foreach (var staged in stage.Blocks) staged.Low.CommitRegistrations();
            foreach (var staged in stage.Blocks) staged.Low.CommitAfter();
        }
        catch (Exception e) when (e is not OutOfMemoryException)
        {
            tx.Dispose();
            ledger.Revoked = true;
            if (owner != 0)
            {
                try { revoke(owner); }
                catch (Exception x) when (x is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {label}: its owner could not be revoked: {x.Message}"); }
            }
            Withdraw(app, generation, "Refused", e.Message, revokeOwner: false);   // the old owner is revoked already
            return;
        }
        tx.Dispose();
        app.Stage = stage;
        app.Owner = owner;
        lock (Gate) app.Replay = generation;
        if (ledger.HookWanted)
        {
            ledger.PlanDefinitions = [.. stage.Blocks.SelectMany(b => b.Low.DefinitionNames)];
            ledger.Settle = problems => Replayed(app, ledger, generation, problems);
            BlockRuntime.Promote(ledger);
            return;
        }
        BlockRuntime.Promote(ledger);
        Replayed(app, ledger, generation, []);
    }

    /// <summary>The replay for <paramref name="generation"/> is done. A callback of an overtaken generation, or of a ledger that is no longer
    /// the plan's, is discarded.</summary>
    private static void Replayed(KindApplication app, ModBlocks ledger, ulong generation, IReadOnlyList<string> problems)
    {
        lock (Gate)
        {
            if (app.Replay != generation || !ReferenceEquals(app.Stage.Ledger, ledger)) return;
            app.Replay = null;
        }
        if (!app.Plan.Consumer.Alive() || app.State != BlockApplyState.Applied) return;
        problems = [.. problems.Distinct(StringComparer.Ordinal)];   // the game's own replay and this one may both have found it
        if (problems.Count == 0)
        {
            Publish(app, generation);
            return;
        }
        var more = problems.Count > 1 ? $" (and {problems.Count - 1} more: kiln.log, or kiln blocks)" : "";
        Withdraw(app, generation, "ContentReadyFailed", $"{problems[0]}{more}");
    }

    /// <summary>An applied plan failed in a later content generation: refused with the cause, its owner revoked as for any refused plan,
    /// and its block no longer published (<see cref="Blocks.Find"/> does not give it, the export and reports leave it out). When the
    /// revoke fails, the reason says what stays.</summary>
    private static void Withdraw(KindApplication app, ulong generation, string cause, string problem, bool revokeOwner = true)
    {
        var plan = app.Plan;
        var label = $"{plan.KindId} {plan.Name}";
        var ledger = app.Stage.Ledger;
        lock (Gate) app.Replay = null;
        ledger.Revoked = true;   // nothing of it runs again
        ledger.Settle = null;
        var left = "";
        if (revokeOwner)
        {
            if (RevokeOwner is not { } revoke) left = "; this build cannot take its registrations back: they stay until the mod unloads";
            else
                try { revoke(app.Owner); }
                catch (Exception x) when (x is not OutOfMemoryException) { left = $"; its registrations could not be taken back ({x.Message}): they stay until the mod unloads"; }
        }
        app.LeftOver = left.Length > 0;
        if (app.Target.Handles.TryGetValue(plan.Name, out var h) && ReferenceEquals(h, app.Handle)) app.Target.Handles.TryRemove(plan.Name, out _);
        app.Target.Lowered.TryRemove(plan.Name, out _);
        foreach (var guid in app.Published) app.Target.Programs.TryRemove(guid, out _);
        app.Published = [];
        app.Target.ReportDirty = true;
        BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {label} was refused in content generation {generation}{(left.Length > 0 ? "" : ", and nothing of it stays")}: {problem}{left}");
        Set(app, BlockApplyState.Refused, $"{cause}: {problem}{left}");
    }

    private sealed class KindStale : Exception;

    /// <summary>The consumer mod instance is gone: its plans' owners are revoked and their blocks are no more.</summary>
    internal static void RetireConsumer(object consumer)
    {
        KindApplication[] gone;
        lock (Gate)
        {
            gone = [.. Apps.Where(a => ReferenceEquals(a.Key.Consumer, consumer)).Select(a => a.Value)];
            foreach (var app in gone) Apps.Remove((consumer, app.Plan.Name));
            Watched.Remove(consumer);
        }
        foreach (var app in gone)
        {
            TakeBack(app);   // registered and waiting for its parts at content ready
            lock (Gate) app.Replay = null;   // a replay that waits is discarded
            app.Stage.Ledger.Revoked = true;
            if (app.State == BlockApplyState.Applied || app.LeftOver)
            {
                if (app.Target.Handles.TryGetValue(app.Plan.Name, out var h) && ReferenceEquals(h, app.Handle)) app.Target.Handles.TryRemove(app.Plan.Name, out _);
                try { RevokeOwner?.Invoke(app.Owner); }
                catch (Exception x) when (x is not OutOfMemoryException) { BlocksBridge.Log(LogLevel.Error, $"blocks: {app.Target.Id}: block {app.Plan.Name}: its owner could not be revoked: {x.Message}"); }
            }
            if (app.State is BlockApplyState.Applied or BlockApplyState.PendingContent) Set(app, BlockApplyState.Removed, "Removed: the mod was unloaded");
        }
    }

    /// <summary>For tests: forget every plan, with no revoke and no event.</summary>
    internal static void Reset()
    {
        lock (Gate)
        {
            Apps.Clear();
            Watched.Clear();
            _generation = 0;
        }
    }
}
