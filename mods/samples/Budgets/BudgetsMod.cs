// SPDX-License-Identifier: MIT
using Kiln;

namespace Budgets;

/// <summary>
/// Reads Kiln's two quotas when the world's data is ready and defines one buff clone only when there is room for it:
/// <see cref="GameDataRegistry.GetBudget"/> (buff clone records, process-wide) and <see cref="World.GetProgramBudget"/>
/// (Kiln's program slots and the world's own program table). A budget can be not ready (no world yet): then it says why
/// and promises no room. A definition that answers is never an exception for a full room or a taken id.
/// </summary>
public sealed class BudgetsMod : Mod
{
    private static readonly Guid Ember = Guid.Parse("3c1b7e52-9a40-4d6f-8e21-7b5d0c4a9f13");

    public override void OnLoad()
    {
        Content.OnReady(_ =>
        {
            var buffs = Content.Buffs.GetBudget();
            Log.Info(buffs.Ok
                ? $"budgets: buff clones: {buffs.Budget.Available} of {buffs.Budget.Capacity} left ({buffs.Budget.KilnUsed} in use, {buffs.Budget.Reserved} kept)"
                : $"budgets: buff clones: not readable now ({buffs.Reason})");
            if (buffs.Budget.Available is > 0)
            {
                var r = Content.Buffs.Define(new GameDataClone { Guid = Ember, Name = "Budgets_Ember", Base = "0x5f425faf", DisplayName = "Ember" },
                    new DefinitionRegistrationOptions());
                Log.Info(r.Ok ? $"budgets: Budgets_Ember is {r.State} (receipt {r.RegistrationId})" : $"budgets: Budgets_Ember refused: {r.Code}: {r.Reason}");
            }
            var programs = World.GetProgramBudget();
            Log.Info(programs.Ok
                ? $"budgets: world programs: Kiln's slots {programs.KilnSlots.Available} of {programs.KilnSlots.Capacity} left, the world's table " +
                  $"{programs.EngineSlots.Available} of {programs.EngineSlots.Capacity} left; a declaration file holds {programs.PerFileMax}"
                : $"budgets: world programs: not readable now ({programs.Reason})");
        });
    }
}
