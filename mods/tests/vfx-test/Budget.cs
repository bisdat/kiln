// SPDX-License-Identifier: MIT
// The fixture of the guided check 17-vfx-budget (mods/tests/client-pass/Dev2917aVfxBudget.cs; package 17a S2). The check's own mod
// (client-pass) says "clients": "kiln", and a new VFX needs "clients": "mod", so the definitions are this mod's. In a game client (or
// a simulated game), OnLoad defines three VFX through the result-bearing Blocks.Define(name, VfxBlock, options) with AllowNextStart and
// one more with RequireRegistered, logs each answer and writes them to <data>/vfx-test/budget.json for the check. The game lists its VFX
// once per start: the first start stores the three for the next one; at the next start they were built before the game listed its VFX,
// and their receipts say Registered once kiln.vfx read the game's table (in a world).
using System.Text.Json;
using System.Text.Json.Nodes;
using Kiln;

namespace VfxTest;

internal static class BudgetFixture
{
    public const string File = "budget.json";

    /// <summary>The three effects: a burst of sprites in one colour that fly up and fade over about a second.</summary>
    public static readonly (string Name, string Colour)[] Effects = [("budget_red", "#ff3030"), ("budget_green", "#30ff50"), ("budget_blue", "#4070ff")];

    private static VfxBlock Block(string colour) => new()
    {
        Particles =
        [
            new ParticleBlock
            {
                Burst = 40, Lifetime = (1.0f, 1.4f), Speed = (1.5f, 3f), Gravity = 0, Size = 0.25f, Color = colour,
                Alpha = Curve.Linear(1, 0), Blend = Kiln.Blend.Additive,
            },
        ],
    };

    /// <summary>In OnLoad (the tick thread): defines the three, and one under RequireRegistered that is never stored.</summary>
    public static void Define(string dataDirectory, Action<string> info)
    {
        const string Row = "blocks.vfx_registration";
        var doc = new JsonObject { ["format"] = 1, ["fixture"] = ClientPass.VfxBudgetProof.Fixture, ["process"] = ClientPass.VfxBudgetProof.ProcessKey, ["at"] = DateTime.UtcNow.ToString("o") };
        if (!Capabilities.Is(Row))
        {
            info($"budget: {Capabilities.Why(Row)}");
            doc["off"] = Capabilities.Why(Row);
            Write(dataDirectory, doc);
            return;
        }
        doc["budgetAtLoad"] = Budget();
        var effects = new JsonArray();
        foreach (var (name, colour) in Effects)
        {
            var r = Blocks.Define(name, Block(colour), new DefinitionRegistrationOptions(DefinitionTiming.AllowNextStart));
            info($"budget: {name}: {r.State} ({r.Code}), {r.SlotsConsumed} slot(s), receipt {r.RegistrationId}{(r.Reason is { } why ? ": " + why : "")}");
            effects.Add(Answer(name, r));
        }
        doc["effects"] = effects;
        // a new VFX that is never stored: RequireRegistered refuses it (RegistryClosed; CapacityExceeded when the table is full)
        var closed = Blocks.Define("budget_never_stored", Block("#ffffff"), new DefinitionRegistrationOptions(DefinitionTiming.RequireRegistered));
        info($"budget: budget_never_stored (RequireRegistered): {closed.State} ({closed.Code}){(closed.Reason is { } w ? ": " + w : "")}");
        doc["requireRegistered"] = Answer("budget_never_stored", closed);
        doc["budgetAfterDefines"] = Budget();
        Write(dataDirectory, doc);
    }

    private static JsonObject Answer(string name, DefinitionRegistrationResult r) => new()
    {
        ["name"] = name, ["registrationId"] = r.RegistrationId.ToString(), ["definition"] = r.Definition.ToString(), ["ok"] = r.Ok,
        ["state"] = r.State.ToString(), ["code"] = r.Code.ToString(), ["slots"] = r.SlotsConsumed, ["reason"] = r.Reason,
    };

    private static JsonNode Budget()
    {
        try
        {
            var b = Vfx.ResourceBudget;
            return new JsonObject
            {
                ["ok"] = b.Ok, ["reason"] = b.Reason, ["capacity"] = b.Budget.Capacity, ["game"] = b.Budget.GameUsed, ["kiln"] = b.Budget.KilnUsed,
                ["reserved"] = b.Budget.Reserved, ["revision"] = b.Budget.Revision,
            };
        }
        catch (KilnUnavailableException e)
        {
            return new JsonObject { ["ok"] = false, ["reason"] = e.Message };
        }
    }

    private static void Write(string dataDirectory, JsonObject doc)
    {
        Directory.CreateDirectory(dataDirectory);
        System.IO.File.WriteAllText(Path.Combine(dataDirectory, File), doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }
}
