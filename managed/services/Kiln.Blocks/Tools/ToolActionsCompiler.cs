// SPDX-License-Identifier: MIT
// Declarative terrain actions use Kiln's existing ImpactProgram writer, content registration and sequence cloning.
// Vanilla building-mode primary/secondary bindings and repeat windows remain the game's; no key bindings or timers.
using System.Text.Json.Nodes;
namespace Kiln;

internal sealed record ToolActionPlan(Guid PrimaryProgram, Guid SecondaryProgram, Guid PrimarySequence, Guid SecondarySequence,
    TerrainToolMode PrimaryMode, TerrainToolMode SecondaryMode);

internal static partial class Compiler
{
    // Independently decoded from vanilla ItemInfo and ActorSequenceResource (build 26430625).
    // SpawnImpact#0 applies the building-state buff; #1 performs the terrain stroke at 216666671 ns.
    private const string ToolPrimarySequence = "Building_Overgrowth_Add", ToolSecondarySequence = "Building_Overgrowth_Remove";

    internal static ToolActionPlan LowerToolActions(BlockContext ctx, Lowered low, string key, string name,
        TerrainToolAction primary, TerrainToolAction secondary)
    {
        ValidateToolAction(ctx, primary, "primary");
        ValidateToolAction(ctx, secondary, "secondary");
        ctx.Need(ClientsLevel.Mod, "primary", "building tools add item, impact program and sequence definitions");
        var pp = ToolProgram(ctx, low, key + ".primary");
        var sp = ToolProgram(ctx, low, key + ".secondary");
        var ps = BlockIds.Derive(ctx.Mod.Id, "sequence", key + ".primary");
        var ss = BlockIds.Derive(ctx.Mod.Id, "sequence", key + ".secondary");
        low.Clone("sequences", ps, $"{ctx.Mod.Id}.{name}.primary", ToolPrimarySequence,
            new JsonObject { ["events[SpawnImpact#1].impact"] = pp.ToString() });
        low.Clone("sequences", ss, $"{ctx.Mod.Id}.{name}.secondary", ToolSecondarySequence,
            new JsonObject { ["events[SpawnImpact#1].impact"] = sp.ToString() });
        return new ToolActionPlan(pp, sp, ps, ss, primary.Mode, secondary.Mode);
    }

    internal static ItemDefinition ApplyToolActions(BlockContext ctx, Lowered low, ItemDefinition brush, ToolActionPlan plan)
    {
        var fields = brush.Fields is null ? new Dictionary<string, object>() : new(brush.Fields);
        fields["equipment.repeatActionWhenPressed"] = true;
        // The game's build cursor checks the selected building material on an input whose item asks for it, and the
        // terraform impact refuses the stroke while the check fails (CursorFlag MissingMaterials; rules/CursorFlags'
        // material check, docs/re/rules.md 2.1). Building_Overgrowth_Shape_Small asks on primary, its Add. A Remove takes
        // no material, and a removal-only tool has none to select: it asks on neither input (kiln#133).
        if (plan.PrimaryMode == TerrainToolMode.Remove) fields["equipment.cursorConfig.checkRequiredMaterialsOnPrimaryAction"] = false;
        if (plan.SecondaryMode == TerrainToolMode.Remove) fields["equipment.cursorConfig.checkRequiredMaterialsOnSecondaryAction"] = false;
        ApplyToolActionRules(ctx, low, brush.Name, plan);
        return brush with { Fields = fields };
    }

    internal static void ApplyToolActionRules(BlockContext ctx, Lowered low, string brushGameName, ToolActionPlan plan)
    {
        // ItemDefinition.Fields/Refs use the scalar content schema, which deliberately does not flatten BlobArray
        // elements. Indexed sequence paths must use itemdata after the clone owns its lists, as Draft.DataRules does.
        // The two-slot brush starter has Secondary at 0 and Primary at 1; the root tool has no sequence slots.
        var rules = new JsonObject
        {
            ["sequences[0].sequence"] = plan.SecondarySequence.ToString(),
            ["sequences[0].sequenceId"] = BlockIds.Fnv(plan.SecondarySequence),
            ["sequences[1].sequence"] = plan.PrimarySequence.ToString(),
            ["sequences[1].sequenceId"] = BlockIds.Fnv(plan.PrimarySequence),
        };
        low.AfterCommit(() => BlockRuntime.AddRules(ctx.Mod, "itemdata", brushGameName, rules));
        var targets = (low.Json["toolActionRules"] ??= new JsonObject()).AsObject();
        targets[brushGameName] = rules.DeepClone();
    }

    private static void ValidateToolAction(BlockContext ctx, TerrainToolAction? action, string path)
    {
        if (action is null || !Enum.IsDefined(action.Mode))
            throw new BlockError(ctx.Block, path + ".mode", "a terrain tool action is Remove or Add");
        if (!action.TerrainOnly)
            throw new BlockError(ctx.Block, path + ".terrainOnly", "building-tool actions target terrain only, never buildings or entities");
        if (action.RemoveMaterials is null || action.RemoveMaterials.Count > 127 ||
            action.RemoveMaterials.Any(cell => cell is 0 or >= 128) ||
            action.RemoveMaterials.Distinct().Count() != action.RemoveMaterials.Count)
            throw new BlockError(ctx.Block, path + ".removeMaterials", "list distinct terrain cells 1..127 only; empty permits no removal");
    }

    private static Guid ToolProgram(BlockContext ctx, Lowered low, string key)
    {
        var b = new ProgramBuilder(key) { Stable = true };
        var root = b.V("Actor", ImpactType.Entity);
        var transform = b.V("Cursor", ImpactType.Transform);
        // 255 is deliberately not a vanilla TerraformingType. The program is inert if the native binding is absent,
        // disabled or removed. Only the qualified native hook substitutes Remove/Add in invocation-local data.
        var mode = b.VByte("ToolMode", new ImpactType(0x4835c9e9, 1), 255);
        var damage = b.V("ToolDamage", new ImpactType(0xc4b5300b, 20));
        var material = b.V("FeedbackMaterial", ImpactType.Material);
        var radius = b.VFloat("BrushRadius", 0.5f, config: 0x5258131e);
        _ = b.V("UsedItem", ImpactType.ItemId, config: 0x220e6b10);
        b.BeginCode();
        b.Call("getRoot", "actor", root); b.Pop();
        b.CallNative("getCursorTransform", 0x79fb429b, "cursor", transform, root); b.Pop();
        b.CallNative("terraforming", 0x9aabd181, "stroke", root, transform, mode, damage, material, radius, radius, null, null);
        b.Pop();
        b.DestroySelf(); b.Halt();
        b.BeginShutdown(); b.Halt();
        var (guid, blob) = ctx.Build(b);
        low.Program(guid, key, blob, b.Listing.ToString());
        return guid;
    }
}
