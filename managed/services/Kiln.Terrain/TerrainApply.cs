// SPDX-License-Identifier: MIT
// The overlay engine: one patch, batch by batch, with the ledger's rules (docs/kiln/services/terrain.md, "The ledger").
// It reaches the world only through ITerrainBackend. Tick thread.
namespace Kiln;

internal sealed class TerrainEngine(ITerrainBackend backend, TerrainSession session, Action<TerrainChangedEvent>? raise)
{
    private static readonly TimeSpan WorldWait = TimeSpan.FromMinutes(5);

    private enum BlockPlan { Write, Done, PlayerEdit, GivenUp }

    public async Task<TerrainResult> Apply(TerrainPlan plan, TerrainApplyOptions options, int edge)
    {
        if (backend.BudgetStep is not null) edge = Math.Min(edge, 8);
        var patch = plan.Patch;
        if (!Enum.IsDefined(options.InstallChoice)) throw new ArgumentException("unknown InstallChoice", nameof(options));
        if (patch.Persistence != TerrainPersistence.Kept && options.InstallChoice != TerrainInstallChoice.Automatic)
            throw new ArgumentException("InstallChoice applies only to kept overlays", nameof(options));
        TerrainResult Result(TerrainState state, string? reason, long cells = 0, int batches = 0, IReadOnlyList<TerrainConflict>? conflicts = null) =>
            new(state is TerrainState.Completed or TerrainState.AlreadyApplied or TerrainState.Adopted, reason, state, patch.Id, patch.Version, cells, batches, conflicts ?? []);

        if (!await WaitForWorld(options.Cancel)) return Result(TerrainState.Refused, "the world did not start running");
        CheckGrid(plan);
        var world = backend.WorldId ?? "";
        var kept = patch.Persistence == TerrainPersistence.Kept;
        if (kept && !session.Begin(world, backend.Mod, patch.Id))
            return Result(TerrainState.Refused, $"plan '{patch.Id}' is being applied already");
        try
        {
            return await Run(plan, options, edge, world, kept);
        }
        finally
        {
            if (kept) session.End(world, backend.Mod, patch.Id);
        }
    }

    private async Task<TerrainResult> Run(TerrainPlan plan, TerrainApplyOptions options, int edge, string world, bool kept)
    {
        if (backend.BudgetStep is not null) edge = Math.Min(edge, 8);
        var patch = plan.Patch;
        if (!Enum.IsDefined(options.InstallChoice)) throw new ArgumentException("unknown InstallChoice", nameof(options));
        if (patch.Persistence != TerrainPersistence.Kept && options.InstallChoice != TerrainInstallChoice.Automatic)
            throw new ArgumentException("InstallChoice applies only to kept overlays", nameof(options));
        double? presenceShare = null;
        bool installedWithoutProof = false, adoptFromPresence = false;
        string? presenceReason = null;
        TerrainResult Result(TerrainState state, string? reason, long cells = 0, int batches = 0, IReadOnlyList<TerrainConflict>? conflicts = null) =>
            new(state is TerrainState.Completed or TerrainState.AlreadyApplied or TerrainState.Adopted, DecisionReason(reason, presenceReason), state, patch.Id, patch.Version, cells, batches, conflicts ?? [])
            { PresenceShare = presenceShare, InstalledWithoutProof = installedWithoutProof };
        TerrainBook? book = null;
        TerrainRecord? rec = null;
        if (kept)
        {
            if (backend.PersistenceFailure is { } unavailable) return Result(TerrainState.Refused, unavailable);
            if (!TryBook(out book, out var error)) return Result(TerrainState.Refused, error);
            session.Learn(world, backend.Mod, book!);
            TerrainLoss.Witness(backend, book!);   // the witness knows every plan the ledger has (TerrainLoss)
            book!.Plans.TryGetValue(patch.Id, out rec);
            if (rec is not null)
            {
                presenceShare = rec.PresenceShare;
                installedWithoutProof = rec.InstalledWithoutProof;
                if (patch.Version < rec.Version)
                    return Result(TerrainState.Refused, $"this world has plan '{patch.Id}' at version {rec.Version}; version {patch.Version} is older and a downgrade is never applied");
                if (patch.Version == rec.Version && rec.Content != plan.ContentHash)
                    return Result(TerrainState.Refused, $"plan '{patch.Id}' version {patch.Version} was applied with other content: content changed without a new version");
                if (patch.Version == rec.Version && rec.Complete)
                {
                    var left = GivenUpOf(rec);
                    return left.Count == 0
                        ? Result(TerrainState.AlreadyApplied, null)
                        : Result(TerrainState.Conflict, $"{left.Count} block(s) were left alone when this version was applied", conflicts: left);
                }
            }
            else if (book.Plans.Count >= TerrainPlan.MaxPlans)
            {
                return Result(TerrainState.Refused, $"this mod already has {TerrainPlan.MaxPlans} kept plans in this world; Terrain.Forget one first");
            }
            // another mod's plan on a block of ours: nothing is written
            var foreign = new List<TerrainConflict>();
            foreach (var b in plan.Blocks())
                if (session.OwnerOf(world, backend.Mod, TerrainPlan.BlockKey(b)) is { } owner)
                    foreign.Add(new TerrainConflict(TerrainConflictKind.OtherPlan, TerrainPlan.WholeBlock(b), owner));
            if (foreign.Count > 0)
                return Result(TerrainState.Refused, $"{foreign.Count} block(s) belong to {foreign[0].Owner}", conflicts: foreign);
            // no record, but the witness says this world had one: its ledger was lost (TerrainLoss, "When the ledger is gone")
            if (rec is null && options.InstallChoice == TerrainInstallChoice.Automatic && !book.WasForgotten(patch.Id) && TerrainLoss.Check(backend, plan, book) is { } lost)
            {
                if (lost.State != TerrainState.NeedsAdoption) return Result(lost.State, lost.Reason);
                var sample = await SamplePresence(plan, options, edge, world);
                if (sample.Error is not null) return Result(TerrainState.Partial, sample.Error);
                presenceShare = sample.Share;
                presenceReason = PresenceReason(plan, sample);
                session.WarnOnce(world, backend.Mod, patch.Id, presenceReason);
                if (sample.Equal != sample.Cells && sample.Share >= PresenceBound)
                    return Result(TerrainState.NeedsAdoption, presenceReason);
                adoptFromPresence = sample.Equal == sample.Cells;
                installedWithoutProof = !adoptFromPresence;
                // Sampling may have yielded while another mod claimed these blocks. Recheck before publishing ours.
                foreach (var b in plan.Blocks())
                    if (session.OwnerOf(world, backend.Mod, TerrainPlan.BlockKey(b)) is { } owner)
                        return Result(TerrainState.Refused, $"a block belongs to {owner}", conflicts: [new(TerrainConflictKind.OtherPlan, TerrainPlan.WholeBlock(b), owner)]);
            }
            book.Unforget(patch.Id);
            if (backend.NewWorldSession) book.EstablishedInNewWorld = true;

            if (rec is null)
            {
                rec = new TerrainRecord();
                book.Plans[patch.Id] = rec;
            }
            else if (patch.Version > rec.Version)
            {
                // an upgrade: what the earlier version owns stays known until each block was visited. An earlier upgrade
                // that was interrupted leaves both of its maps as "earlier": its newer entries win.
                var earlier = rec.EarlierBlocks ?? new Dictionary<string, TerrainBlock>();
                var earlierRegion = rec.EarlierRegion ?? rec.Region;
                if (rec.EarlierBlocks is not null && !SameBox(rec.Region, earlierRegion))
                {
                    // two regions cannot both be "the earlier one": blocks of the interrupted version are checked
                    // against its region, the rest are given up (never written again) rather than guessed at
                    earlier = earlier.Where(p => !TerrainPlan.Intersect(KeyBox(p.Key), rec.Box).IsEmpty).ToDictionary(p => p.Key, p => p.Value);
                    foreach (var b in earlier.Values) b.GivenUp = true;
                    earlierRegion = rec.Region;
                }
                foreach (var (k, b) in rec.Blocks) earlier[k] = b;
                rec.EarlierBlocks = earlier;
                rec.EarlierRegion = earlierRegion;
                rec.Blocks = new();
            }
            rec.Version = patch.Version;
            rec.Region = TerrainRecord.FromBox(plan.Region);
            rec.Content = plan.ContentHash;
            rec.Complete = false;
            rec.PresenceShare = presenceShare;
            rec.InstalledWithoutProof = installedWithoutProof;
            if (options.InstallChoice == TerrainInstallChoice.Adopt || adoptFromPresence) { rec.Adopted = true; rec.Adopting = true; }
            if (options.InstallChoice == TerrainInstallChoice.InstallAnyway) { rec.Adopted = false; rec.Adopting = false; }
            rec.KilnVersion = backend.KilnVersion;
            backend.Save(book);
            if (!TerrainLoss.Witness(backend, book))
                return Result(TerrainState.Refused, "the terrain ownership witness could not be written; nothing was changed");
            session.Claim(world, backend.Mod, patch.Id, plan.Blocks().Select(TerrainPlan.BlockKey));
        }

        if (!kept)
        {
            var foreign = plan.Blocks().Select(b => (Box: b, Owner: session.OwnerOf(world, backend.Mod, TerrainPlan.BlockKey(b))))
                .Where(b => b.Owner is not null).Select(b => new TerrainConflict(TerrainConflictKind.OtherPlan, TerrainPlan.WholeBlock(b.Box), b.Owner)).ToList();
            if (foreign.Count > 0) return Result(TerrainState.Refused, $"blocks belong to {foreign[0].Owner}", conflicts: foreign);
        }
        if (kept && rec!.Adopting)
        {
            // Read first, then publish all receipts together. No writes or Changed events on adoption.
            var adopted = new Dictionary<string, TerrainBlock>();
            foreach (var box in plan.Batches(edge))
            {
                if (options.Cancel.IsCancellationRequested || !SameWorld(world)) return Result(TerrainState.Partial, "adoption stopped");
                if (!await backend.Load(box, options.LoadTimeout) || !SameWorld(world))
                    return Result(TerrainState.Partial, "adoption could not read the current terrain");
                var read = await Read(box, world);
                if (!read.Ok)
                    return Result(TerrainState.Partial, "adoption could not read the current terrain");
                foreach (var part in TerrainPlan.Cut(box, 8))
                {
                    if (!plan.AnyAddressed(part)) continue;
                    var hash = TerrainPlan.Fingerprint(box, read.Cells, part);
                    adopted[TerrainPlan.BlockKey(part)] = new TerrainBlock { Before = hash, After = hash };
                }
            }
            rec!.Blocks = adopted;
            rec.EarlierBlocks = null; rec.EarlierRegion = null;
            rec.Adopted = true; rec.Adopting = false; rec.Complete = true; rec.AppliedAt = DateTimeOffset.UtcNow;
            backend.Save(book!);
            session.Learn(world, backend.Mod, book!);
            return Result(TerrainState.Adopted, presenceReason ?? "Current terrain adopted without writing. Original terrain cannot be restored; Forget only releases ownership.");
        }
        var batches = plan.Batches(edge);
        var conflicts = new List<TerrainConflict>();
        long changed = 0;
        var written = 0;
        string? stop = null;
        // Pacing. Without the host budget (test adapters) one batch a tick, as 0.2.0-dev.28. With it a batch is one unit
        // of at most 8 x 8 x 8 cells and the budget's admission decides when the step is full (Acquire waits for the next
        // tick): the loop itself yields only where the mod's own CellsPerTick is used up for this tick. Lab, dev29-terrain2:
        // a tick after every unit made one 32,768-cell patch take 64 ticks (3.6 s on an idle server; 38 ms in dev.28).
        long ceiling = Math.Max(1, (long)options.CellsPerTick), sinceYield = 0;
        var paced = backend.BudgetStep;
        for (var i = 0; i < batches.Count && stop is null; i++)
        {
            if (options.Cancel.IsCancellationRequested) { stop = "canceled"; break; }
            if (!SameWorld(world)) { stop = "the world stopped or changed"; break; }
            var box = batches[i];
            bool loaded;
            try { loaded = await backend.Load(box, options.LoadTimeout); }
            catch (BuildException e) { stop = e.Message; break; }
            if (!SameWorld(world)) { stop = "the world stopped or changed"; break; }
            var read = loaded ? await Read(box, world) : (false, Array.Empty<Voxel>());
            var now = read.Item2;
            if (!read.Item1)
            {
                stop = $"the voxel tiles under {box} did not load";
                break;
            }
            if (!await session.Budget.Acquire(backend, 0, 0) || !SameWorld(world)) { stop = "the world stopped or changed"; break; }
            // The batch's reads and writes charge their own time (Read, Write). Charging the whole batch here as well
            // counted them twice, and counted a wait for the next tick inside them as work, which closed that tick.
            var (stopped, cells, wrote) = await Batch(plan, box, now, rec, options, conflicts, world);
            if (stopped is not null) { stop = stopped; }
            if (wrote)
            {
                written++;
                changed += cells;
                raise?.Invoke(new TerrainChangedEvent(backend.Mod, patch.Id, patch.Version, box, cells, i + 1, batches.Count, i == batches.Count - 1));
                options.Progress?.Report(new TerrainProgress(i + 1, batches.Count, changed));
            }
            if (backend.BudgetStep != paced) { paced = backend.BudgetStep; sinceYield = 0; }  // a tick passed inside the batch
            sinceYield += box.Count;
            if (stop is null && i < batches.Count - 1 && (backend.BudgetStep is null || sinceYield + batches[i + 1].Count > ceiling))
            {
                await backend.NextTick();
                paced = backend.BudgetStep;
                sinceYield = 0;
            }
        }

        if (stop is null && !SameWorld(world)) stop = "the world stopped or changed";
        if (stop is not null)
            return Result(TerrainState.Partial, stop, changed, written, conflicts);
        if (kept)
        {
            // blocks the earlier version owned and this one does not address: no longer the plan's
            rec!.EarlierBlocks = null;
            rec.EarlierRegion = null;
            rec.Complete = true;
            rec.AppliedAt = DateTimeOffset.UtcNow;
            backend.Save(book!);
            session.Learn(world, backend.Mod, book!);
        }
        return conflicts.Count == 0
            ? Result(TerrainState.Completed, null, changed, written)
            : Result(TerrainState.Conflict, $"{conflicts.Count} block(s) were left alone", changed, written, conflicts);
    }

    /// <summary>One batch: decide each block, write what may be written, record it. Returns a reason when the patch must
    /// stop, the cells changed, and whether anything was written.</summary>
    private async Task<(string? Stop, long Cells, bool Wrote)> Batch(TerrainPlan plan, CellBox box, Voxel[] now, TerrainRecord? rec,
        TerrainApplyOptions options, List<TerrainConflict> conflicts, string world)
    {
        var region = plan.Region;
        var n = now.Length;
        var cells = (Voxel[])now.Clone();
        var mask = new bool[n];
        var blocks = new List<(CellBox Part, string Key, string Before)>();
        foreach (var part in TerrainPlan.Cut(box, 8))
        {
            if (!plan.AnyAddressed(part)) continue;
            var key = TerrainPlan.BlockKey(part);
            var before = TerrainPlan.Fingerprint(box, now, part);
            var what = rec is null ? BlockPlan.Write : await Decide(rec, key, part, before, world);
            switch (what)
            {
                case BlockPlan.Done:
                    continue;
                case BlockPlan.GivenUp:
                case BlockPlan.PlayerEdit:
                    var why = what == BlockPlan.GivenUp ? WhyOf(rec!, key) : TerrainConflictKind.PlayerEdit;
                    conflicts.Add(new TerrainConflict(why, TerrainPlan.WholeBlock(part)));
                    rec!.Blocks[key] = new TerrainBlock { Before = before, After = before, GivenUp = true, Why = (int)why };
                    rec.EarlierBlocks?.Remove(key);
                    continue;
            }
            for (var z = part.Z; z < part.Z + part.SizeZ; z++)
            for (var y = part.Y; y < part.Y + part.SizeY; y++)
            for (var x = part.X; x < part.X + part.SizeX; x++)
            {
                var ri = region.Index(x - region.X, y - region.Y, z - region.Z);
                if (!plan.Addressed[ri]) continue;
                var bi = box.Index(x - box.X, y - box.Y, z - box.Z);
                var want = plan.Target[ri];
                if (plan.OnlyAdd && now[bi].Fill >= want.Fill) continue;
                cells[bi] = want;
                mask[bi] = true;
            }
            blocks.Add((part, key, before));
        }
        if (blocks.Count == 0) return (null, 0, false);

        long changed = 0;
        var wrote = false;
        var label = $"terrain {plan.Patch.Id} v{plan.Patch.Version}";
        var transient = plan.Patch.Persistence == TerrainPersistence.Transient;
        // Publish expected fingerprints BEFORE calling Build: its events may save, edit cells or end the world.
        // Never adopt arbitrary readback as our "after", and never rewrite a receipt that no longer matches it.
        void Record(IEnumerable<(CellBox Part, string Key, string Before)> parts)
        {
            if (rec is null) return;
            foreach (var (part, key, before) in parts)
                rec.Blocks[key] = new TerrainBlock { Before = before, After = TerrainPlan.Fingerprint(box, cells, part) };
        }
        void Committed(IEnumerable<(CellBox Part, string Key, string Before)> parts)
        {
            if (rec is null) return;
            foreach (var (_, key, _) in parts) rec.EarlierBlocks?.Remove(key);
        }
        // An explicit refusal committed nothing: retain the older ownership until a later retry.
        var previous = rec is null ? null : new Dictionary<string, TerrainBlock>(rec.Blocks);
        void Undo(IEnumerable<(CellBox Part, string Key, string Before)> parts)
        {
            if (rec is null) return;
            foreach (var (_, key, _) in parts)
                if (previous!.TryGetValue(key, out var prior)) rec.Blocks[key] = prior;
                else rec.Blocks.Remove(key);
        }
        if (mask.AsSpan().Contains(true))
        {
            Record(blocks);
            BuildResult r;
            // guarded: kiln.build commits only if the masked cells still hold what this batch read, compared after its
            // BuildEditingEvent handlers and right before the commit, so an edit made in between is never written over
            try { r = await Write(box, cells, mask, options.InBases, transient, label, options.LoadTimeout, expected: now); }
            catch (BuildException e) { return (e.Message, changed, wrote); }
            if (!SameWorld(world)) return ("the world stopped or changed", r.CellsChanged, !r.Refused);
            if (r.Refusal is BuildRefusal.Base or BuildRefusal.Changed)
            {
                Undo(blocks);
                // the batch touches a base, or someone changed its cells: block by block, preserving receipts for each
                // successful write
                foreach (var block in blocks)
                {
                    var (part, key, before) = block;
                    var (pc, pm) = Sub(box, cells, mask, part);
                    if (!pm.AsSpan().Contains(true)) { Record([block]); Committed([block]); continue; }
                    if (!SameWorld(world)) return ("the world stopped or changed", changed, wrote);
                    Record([block]);
                    BuildResult pr;
                    try { pr = await Write(part, pc, pm, options.InBases, transient, label, options.LoadTimeout, expected: Sub(box, now, mask, part).Cells); }
                    catch (BuildException e) { return (e.Message, changed, wrote); }
                    if (pr.Refusal is BuildRefusal.Base or BuildRefusal.Changed)
                    {
                        // a base, or cells changed since this batch read them: nothing of the block is written, the plan
                        // gives it up
                        var why = pr.Refusal == BuildRefusal.Base ? TerrainConflictKind.BaseZone : TerrainConflictKind.PlayerEdit;
                        conflicts.Add(new TerrainConflict(why, TerrainPlan.WholeBlock(part)));
                        if (rec is not null) rec.Blocks[key] = new TerrainBlock { Before = before, After = before, GivenUp = true, Why = (int)why };
                        Committed([block]);
                    }
                    else if (pr.Refused)
                    {
                        Undo([block]);
                        return ($"a write of {part} was refused: {pr.Reason}", changed, wrote);
                    }
                    else
                    {
                        Committed([block]);
                        changed += pr.CellsChanged;
                        wrote = true;
                    }
                }
            }
            else if (r.Refused)
            {
                Undo(blocks);
                return ($"a write of {box} was refused: {r.Reason}", 0, false);
            }
            else
            {
                Committed(blocks);
                changed = r.CellsChanged;
                wrote = true;
            }
        }
        else { Record(blocks); Committed(blocks); }
        if (!SameWorld(world)) return ("the world stopped or changed", changed, wrote);
        if (rec is not null && !(await Read(box, world)).Ok)
            return ($"the voxel tiles under {box} went away after the write", changed, wrote);
        return (null, changed, wrote);
    }

    /// <summary>What to do with a block the record may know. <paramref name="now"/> is the fingerprint of the block's cells
    /// inside the new region.</summary>
    private async Task<BlockPlan> Decide(TerrainRecord rec, string key, CellBox part, string now, string world)
    {
        if (rec.Blocks.TryGetValue(key, out var mine))
        {
            // this version already met the block (a resumed apply)
            if (mine.GivenUp) return BlockPlan.GivenUp;
            if (now == mine.After) return BlockPlan.Done;
            // Equal to "before" is ambiguous: a player may have restored precisely the original ground.
            return BlockPlan.PlayerEdit;
        }
        if (rec.EarlierBlocks is not null && rec.EarlierBlocks.TryGetValue(key, out var old))
        {
            if (old.GivenUp) return BlockPlan.GivenUp;
            // the earlier version's fingerprint is over its own region's part of the block
            var oldPart = TerrainPlan.Intersect(TerrainPlan.WholeBlock(part), TerrainRecord.ToBox(rec.EarlierRegion ?? []));
            if (oldPart.IsEmpty) return BlockPlan.PlayerEdit;
            var read = await Read(oldPart, world);
            if (!read.Ok) return BlockPlan.PlayerEdit;
            return TerrainPlan.Fingerprint(oldPart, read.Cells, oldPart) == old.After ? BlockPlan.Write : BlockPlan.PlayerEdit;
        }
        return BlockPlan.Write;
    }

    public async Task<TerrainPlanReport> Inspect(TerrainPlan plan, TerrainApplyOptions options, int edge)
    {
        if (backend.BudgetStep is not null) edge = Math.Min(edge, 8);
        var patch = plan.Patch;
        var batches = plan.Batches(edge);
        var blockList = plan.Blocks().ToList();
        double? presenceShare = null;
        string? presenceReason = null;
        TerrainPlanReport Report(bool ok, string? reason, TerrainState would, IReadOnlyList<TerrainConflict>? conflicts = null) =>
            new(ok, DecisionReason(reason, presenceReason), plan.Region, plan.AddressedCells, blockList.Count, batches.Count, (long)blockList.Count * TerrainPlan.BytesPerBlock,
                would, conflicts ?? []) { PresenceShare = presenceShare };

        if (!backend.WorldRunning) return Report(false, "the world is not running", TerrainState.Refused);
        CheckGrid(plan);
        var world = backend.WorldId ?? "";
        TerrainRecord? rec = null;
        if (patch.Persistence == TerrainPersistence.Kept)
        {
            if (!TryBook(out var book, out var error)) return Report(false, error, TerrainState.Refused);
            session.Learn(world, backend.Mod, book!);
            book!.Plans.TryGetValue(patch.Id, out rec);
            if (rec is not null)
            {
                if (patch.Version < rec.Version) return Report(true, null, TerrainState.Refused);
                if (patch.Version == rec.Version && rec.Content != plan.ContentHash) return Report(true, null, TerrainState.Refused);
                if (patch.Version == rec.Version && rec.Complete)
                {
                    var left = GivenUpOf(rec);
                    return Report(true, null, left.Count == 0 ? TerrainState.AlreadyApplied : TerrainState.Conflict, left);
                }
            }
            else if (book.Plans.Count >= TerrainPlan.MaxPlans)
            {
                return Report(true, null, TerrainState.Refused);
            }
            var foreign = new List<TerrainConflict>();
            foreach (var b in blockList)
                if (session.OwnerOf(world, backend.Mod, TerrainPlan.BlockKey(b)) is { } owner)
                    foreign.Add(new TerrainConflict(TerrainConflictKind.OtherPlan, TerrainPlan.WholeBlock(b), owner));
            if (foreign.Count > 0) return Report(true, null, TerrainState.Refused, foreign);
            if (rec is null && options.InstallChoice == TerrainInstallChoice.Automatic && !book.WasForgotten(patch.Id) && TerrainLoss.Check(backend, plan, book) is { } lost)
            {
                if (lost.State != TerrainState.NeedsAdoption) return Report(true, lost.Reason, lost.State);
                var sample = await SamplePresence(plan, options, edge, world);
                if (sample.Error is not null) return Report(false, sample.Error, TerrainState.Partial);
                presenceShare = sample.Share;
                presenceReason = PresenceReason(plan, sample);
                if (sample.Equal == sample.Cells) return Report(true, presenceReason, TerrainState.Adopted);
                if (sample.Share >= PresenceBound) return Report(true, presenceReason, TerrainState.NeedsAdoption);
            }
        }
        if (session.Running(world, backend.Mod, patch.Id)) return Report(true, "the plan is being applied already", TerrainState.Refused);
        if (patch.Persistence == TerrainPersistence.Transient)
        {
            var foreign = blockList.Select(b => (Box: b, Owner: session.OwnerOf(world, backend.Mod, TerrainPlan.BlockKey(b))))
                .Where(b => b.Owner is not null).Select(b => new TerrainConflict(TerrainConflictKind.OtherPlan, TerrainPlan.WholeBlock(b.Box), b.Owner)).ToList();
            if (foreign.Count > 0) return Report(true, $"blocks belong to {foreign[0].Owner}", TerrainState.Refused, foreign);
        }
        bool loaded;
        try { loaded = await backend.Load(plan.Region, options.LoadTimeout); }
        catch (BuildException e) { return Report(false, e.Message, TerrainState.Partial); }
        if (!SameWorld(world)) return Report(false, "the world stopped or changed", TerrainState.Partial);
        if (!loaded)
            return Report(false, $"the voxel tiles under {plan.Region} did not load", TerrainState.Partial);
        var now = new Voxel[plan.Region.Count];
        foreach (var chunk in plan.Batches(edge))
        {
            var read = await Read(chunk, world);
            if (!read.Ok) return Report(false, "the world stopped or tiles went away", TerrainState.Partial);
            for (var z = 0; z < chunk.SizeZ; z++)
            for (var y = 0; y < chunk.SizeY; y++)
                read.Cells.AsSpan(chunk.Index(0,y,z),chunk.SizeX).CopyTo(now.AsSpan(plan.Region.Index(chunk.X-plan.Region.X,chunk.Y+y-plan.Region.Y,chunk.Z+z-plan.Region.Z),chunk.SizeX));
        }
        if (!SameWorld(world))
            return Report(false, $"the voxel tiles under {plan.Region} did not load", TerrainState.Partial);

        var conflicts = new List<TerrainConflict>();
        // what an upgrade or a resumed apply would meet; a view of the record as the apply would see it
        TerrainRecord? view = rec is null ? null : ViewFor(rec, patch.Version);
        foreach (var part in blockList)
        {
            if (!await session.Budget.Acquire(backend, 0, 0) || !SameWorld(world)) return Report(false, "the world stopped or changed", TerrainState.Partial);
            var started = backend.BudgetMilliseconds;
            try
            {
            var whole = TerrainPlan.WholeBlock(part);
            var key = TerrainPlan.BlockKey(part);
            var what = view is null ? BlockPlan.Write : await Decide(view, key, part, TerrainPlan.Fingerprint(plan.Region, now, part), world);
            if (what is BlockPlan.PlayerEdit or BlockPlan.GivenUp)
            {
                conflicts.Add(new TerrainConflict(what == BlockPlan.GivenUp ? WhyOf(view!, key) : TerrainConflictKind.PlayerEdit, whole));
                continue;
            }
            if (what == BlockPlan.Done) continue;
            if (options.InBases) continue;
            // a dry-run write of the block: kiln.build says whether a base is in the way
            var (pc, pm) = Sub(plan.Region, plan.Target, plan.Addressed, part);
            BuildResult probe;
            try { probe = await Write(part, pc, pm, false, true, "terrain inspect", options.LoadTimeout, dryRun: true); }
            catch (BuildException e) { return Report(false, e.Message, TerrainState.Partial); }
            if (!SameWorld(world)) return Report(false, "the world stopped or changed", TerrainState.Partial);
            if (probe.Refusal == BuildRefusal.Base) conflicts.Add(new TerrainConflict(TerrainConflictKind.BaseZone, whole));
            else if (probe.Refused) return Report(false, probe.Reason, TerrainState.Partial);
            }
            finally { session.Budget.Charge(backend, started); }
        }
        return Report(true, null, conflicts.Count == 0 ? TerrainState.Completed : TerrainState.Conflict, conflicts);
    }

    /// <summary>The record as an apply of <paramref name="version"/> would start from, without changing the stored one.</summary>
    private static TerrainRecord ViewFor(TerrainRecord rec, int version)
    {
        if (version == rec.Version) return rec;
        var earlier = new Dictionary<string, TerrainBlock>(rec.EarlierBlocks ?? []);
        var region = rec.EarlierRegion ?? rec.Region;
        if (rec.EarlierBlocks is not null && !SameBox(rec.Region, region))
        {
            earlier = earlier.Where(p => !TerrainPlan.Intersect(KeyBox(p.Key), rec.Box).IsEmpty).ToDictionary(p => p.Key, p => p.Value);
            foreach (var k in earlier.Keys.ToList()) earlier[k] = new TerrainBlock { GivenUp = true };
            region = rec.Region;
        }
        foreach (var (k, b) in rec.Blocks) earlier[k] = b;
        return new TerrainRecord { Version = version, Region = rec.Region, EarlierBlocks = earlier, EarlierRegion = region };
    }

    public TerrainResult Forget(string id, bool restoreTerrain)
    {
        if (restoreTerrain)
            return new(false, "Original terrain cannot be restored: overlays store fingerprints, not original cells; adopted terrain has no original image. Use Forget(id) to release ownership only.", TerrainState.Refused, id, 0, 0, 0, []);
        var ok = Forget(id);
        return new(ok, ok ? null : "no removable plan in this world", ok ? TerrainState.Completed : TerrainState.Refused, id, 0, 0, 0, []);
    }

    public bool Forget(string id)
    {
        if (session.Running(backend.WorldId ?? "", backend.Mod, id)) return false;
        if (!TryBook(out var book, out _)) return false;
        if (!book!.Forget(id)) return false;
        backend.Save(book);
        TerrainLoss.Unwitness(backend, id);
        session.Learn(backend.WorldId ?? "", backend.Mod, book);
        return true;
    }

    public IReadOnlyList<TerrainPlanInfo> Applied()
    {
        if (!TryBook(out var book, out _)) return [];
        session.Learn(backend.WorldId ?? "", backend.Mod, book!);
        return book.Plans.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => new TerrainPlanInfo(p.Key, p.Value.Version, p.Value.Box,
            p.Value.Complete, p.Value.Blocks.Values.Count(b => !b.GivenUp), p.Value.Blocks.Values.Count(b => b.GivenUp), p.Value.AppliedAt) { Adopted = p.Value.Adopted, PresenceShare = p.Value.PresenceShare, InstalledWithoutProof = p.Value.InstalledWithoutProof }).ToList();
    }

    private static string? DecisionReason(string? reason, string? presence) =>
        reason is null || presence is null || reason == presence ? reason ?? presence : $"{reason} {presence}";

    private const double PresenceBound = 0.25;
    private readonly record struct Presence(long Equal, long Cells, string? Error = null)
    {
        public double Share => Cells == 0 ? 0 : (double)Equal / Cells;
    }

    private async Task<Presence> SamplePresence(TerrainPlan plan, TerrainApplyOptions options, int edge, string world)
    {
        long equal = 0, cells = 0;
        foreach (var box in plan.Batches(edge))
        {
            if (options.Cancel.IsCancellationRequested || !SameWorld(world)) return new(0, 0, "presence sampling stopped");
            try
            {
                if (!await backend.Load(box, options.LoadTimeout) || !SameWorld(world)) return new(0, 0, "presence sampling could not load terrain");
                var read = await Read(box, world);
                if (!read.Ok || options.Cancel.IsCancellationRequested || !SameWorld(world)) return new(0, 0, "presence sampling stopped or could not read terrain");
                var started = backend.BudgetMilliseconds;
                try
                {
                    for (var z = 0; z < box.SizeZ; z++)
                    for (var y = 0; y < box.SizeY; y++)
                    for (var x = 0; x < box.SizeX; x++)
                    {
                        var ri = plan.Region.Index(box.X + x - plan.Region.X, box.Y + y - plan.Region.Y, box.Z + z - plan.Region.Z);
                        if (!plan.Addressed[ri]) continue;
                        cells++;
                        if (read.Cells[box.Index(x,y,z)].Raw == plan.Target[ri].Raw) equal++;
                    }
                }
                finally { session.Budget.Charge(backend, started); }
            }
            catch (BuildException e) { return new(0, 0, $"presence sampling failed: {e.Message}"); }
        }
        return new(equal, cells);
    }

    private string PresenceReason(TerrainPlan plan, Presence sample)
    {
        var share = sample.Share.ToString("0.##%", System.Globalization.CultureInfo.InvariantCulture);
        var action = sample.Equal == sample.Cells
            ? "already present; adopting without writing. Original terrain cannot be restored"
            : sample.Share >= PresenceBound ? "needs adoption; nothing written; choose Adopt or InstallAnyway"
            : "below the 25% presence bound; installing without proof";
        return $"terrain: {backend.Mod}/{plan.Patch.Id}, world {backend.SaveIdentity.Id ?? backend.WorldId}: {sample.Equal}/{sample.Cells} addressed cells exactly match ({share}); {action}. " +
            "Presence bound: 25%. Removing more than 75% of the measured overlay can make it look absent and allow refill; naturally matching cells can also trigger adoption.";
    }

    private async Task<(bool Ok, Voxel[] Cells)> Read(CellBox box, string world)
    {
        if (!await session.Budget.Acquire(backend, box.Count + (backend.BudgetStep is null ? 0 : 4), 0) || !SameWorld(world)) return (false, []);
        var start = backend.BudgetMilliseconds;
        try { var ok = backend.TryRead(box, out var cells); return (ok, cells); }
        finally { session.Budget.Charge(backend, start); }
    }

    private async Task<BuildResult> Write(CellBox box, Voxel[] cells, bool[] mask, bool inBases, bool transient,
        string label, TimeSpan timeout, bool dryRun = false, Voxel[]? expected = null)
    {
        var world = backend.WorldId ?? "";
        if (!await backend.Load(box, timeout) || !SameWorld(world)) return new BuildResult(0, 0, false, false, box, 0, BuildRefusal.NotLoaded);
        // Native widening, compare, before/readback, and streaming probes. Even a refused write pays this allowance.
        var widened = TerrainPlan.WholeBlock(box).Count; // production units never cross an 8-cell boundary
        var reads = backend.BudgetStep is null ? 0 : widened * 4;
        if (!await session.Budget.Acquire(backend, reads, dryRun ? 0 : widened) || !SameWorld(world)) return new BuildResult(0, 0, false, false, box, 0, BuildRefusal.NotLoaded);
        var start = backend.BudgetMilliseconds;
        try { return await backend.Write(box, cells, mask, inBases, transient, label, timeout, dryRun, expected); }
        finally { session.Budget.Charge(backend, start); }
    }

    private bool SameWorld(string world) => backend.WorldRunning && (backend.WorldId ?? "") == world;

    private bool TryBook([System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out TerrainBook? book, out string? error)
    {
        if (!backend.WorldRunning)
        {
            book = null;
            error = "the world is not running";
            return false;
        }
        if (backend.PersistenceFailure is { } failure)
        {
            book = null;
            error = failure;
            return false;
        }
        try
        {
            book = backend.Book(); // opens this world's sidecar before its match is inspected
            if (backend.LoadedData == DataMatch.Older)
                throw new InvalidDataException("terrain ledger is older than this world save; writes are refused until matching world data is restored");
            if (book is null) throw new InvalidDataException("terrain ledger is null; stored data was kept");
            book.Validate(backend.GridCells);
            error = null;
            return true;
        }
        catch (InvalidDataException e)
        {
            book = null;
            error = e.Message;
            Kiln.Backend.Current.Log(0, LogLevel.Warn, $"terrain: {error}");
            return false;
        }
    }

    private async Task<bool> WaitForWorld(CancellationToken cancel)
    {
        var until = DateTime.UtcNow + WorldWait;
        while (!backend.WorldRunning)
        {
            if (!backend.CanWaitForWorld || cancel.IsCancellationRequested || DateTime.UtcNow > until) return false;
            await backend.NextTick();
        }
        return true;
    }

    private void CheckGrid(TerrainPlan plan)
    {
        if (backend.GridCells is not { } g) return;
        var r = plan.Region;
        if ((long)r.X + r.SizeX > g.X || (long)r.Z + r.SizeZ > g.Z)
            throw new ArgumentException($"plan '{plan.Patch.Id}': the region {r} is outside the voxel grid ({g.X} x {g.Z} cells)", "patch");
    }

    private static List<TerrainConflict> GivenUpOf(TerrainRecord rec)
    {
        var list = new List<TerrainConflict>();
        foreach (var (key, b) in rec.Blocks.OrderBy(b => b.Key, StringComparer.Ordinal))
        {
            if (!b.GivenUp) continue;
            var p = key.Split(',');
            list.Add(new TerrainConflict(KindOf(b), new CellBox(int.Parse(p[0]), int.Parse(p[1]), int.Parse(p[2]), 8, 8, 8)));
        }
        return list;
    }

    private static TerrainConflictKind KindOf(TerrainBlock b) =>
        b.Why == (int)TerrainConflictKind.BaseZone ? TerrainConflictKind.BaseZone : TerrainConflictKind.PlayerEdit;

    private static TerrainConflictKind WhyOf(TerrainRecord rec, string key) =>
        rec.Blocks.TryGetValue(key, out var b) || (rec.EarlierBlocks is not null && rec.EarlierBlocks.TryGetValue(key, out b))
            ? KindOf(b) : TerrainConflictKind.PlayerEdit;

    private static CellBox KeyBox(string key)
    {
        var p = key.Split(','); // the book was validated before use
        return new CellBox(int.Parse(p[0]), int.Parse(p[1]), int.Parse(p[2]), 8, 8, 8);
    }

    private static bool SameBox(int[] a, int[] b) => a.AsSpan().SequenceEqual(b);

    /// <summary>The cells and mask of <paramref name="part"/> cut out of those of <paramref name="box"/>.</summary>
    private static (Voxel[] Cells, bool[] Mask) Sub(CellBox box, Voxel[] cells, bool[] mask, CellBox part)
    {
        var pc = new Voxel[part.Count];
        var pm = new bool[part.Count];
        for (var z = 0; z < part.SizeZ; z++)
        for (var y = 0; y < part.SizeY; y++)
        {
            var from = box.Index(part.X - box.X, part.Y - box.Y + y, part.Z - box.Z + z);
            var to = part.Index(0, y, z);
            cells.AsSpan(from, part.SizeX).CopyTo(pc.AsSpan(to));
            mask.AsSpan(from, part.SizeX).CopyTo(pm.AsSpan(to));
        }
        return (pc, pm);
    }
}
