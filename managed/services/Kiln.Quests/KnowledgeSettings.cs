// SPDX-License-Identifier: MIT
// kiln/config/knowledge.json: kiln.knowledge's opt-in security checks. The native service reads the file itself (at
// boot and when it changes), so the checks work without any managed mod; Kiln.Quests registers it as Kiln's config
// "knowledge" so the launcher and the dashboard can edit it with comments.
namespace Kiln;

/// <summary>The file's schema (Kiln's config "knowledge"). Docs: docs/kiln/services/knowledge.md "Security".</summary>
[Comment("kiln.knowledge security checks. Both are OFF by default; read docs/kiln/services/knowledge.md \"Security\" before turning them on.")]
public sealed class KnowledgeSettings
{
    /// <summary>Hold every knowledge operation a client sends (<c>TriggerKnowledgeAction</c>) and drop it unless its id is in
    /// <see cref="AllowClientKnowledge"/> or a <see cref="KnowledgeRequestEvent"/> handler allows it. Off by default: the vanilla
    /// client sends some knowledge itself (tutorial progress), which the veto would drop.</summary>
    [Comment("Hold every knowledge operation a client sends (TriggerKnowledgeAction) and drop it unless its id is in " +
             "allowClientKnowledge or a KnowledgeRequestEvent handler allows it. Stops a modified client from unlocking " +
             "quests, NPCs or recipes by itself. Risk: the vanilla client sends some knowledge on its own (tutorial " +
             "progress); those ids must be allowed or the tutorial stalls. Watch the server log: every distinct id a " +
             "client sends is logged once (logClientKnowledge).")]
    public bool VetoClientKnowledge { get; set; }

    /// <summary>Knowledge ids or names that pass the client veto, such as <c>0x1234abcd</c>, <c>npc:NPC_Workshop_Bard01.unlock</c>
    /// or <c>quest:0x8755c335</c>.</summary>
    [Comment("Knowledge ids or names that pass the client veto: \"0x1234abcd\", \"npc:NPC_Workshop_Bard01.unlock\", \"quest:0x8755c335\".")]
    public List<string> AllowClientKnowledge { get; set; } = [];

    /// <summary>Hold the knowledge of a dialogue line a client picked and run it only if the line's requirement holds for that
    /// player and the player stands within <see cref="DialogueRange"/> metres of the NPC. Off by default.</summary>
    [Comment("Hold the knowledge of a dialogue line a client picked and run it only if the line's requirement holds for " +
             "that player and the player stands within dialogueRange metres of the NPC. Stops clients from picking " +
             "options the NPC does not offer them. Risk: a line whose requirement the server evaluates differently " +
             "from the client (e.g. personal progression edge cases) loses its knowledge; delays the knowledge by one step.")]
    public bool CheckDialogue { get; set; }

    /// <summary>Metres between player and NPC for <see cref="CheckDialogue"/> (0: no distance check; default 12).</summary>
    [Comment("Metres between player and NPC for checkDialogue (0: no distance check)."), Range(0, 1000)]
    public double DialogueRange { get; set; } = 12.0;

    /// <summary>Log each distinct knowledge id a client sends, once per player and session (default on).</summary>
    [Comment("Log each distinct knowledge id a client sends (once per player and session).")]
    public bool LogClientKnowledge { get; set; } = true;
}

internal static class KnowledgeSettingsFile
{
    private static ModConfig<KnowledgeSettings>? _file;
    private static bool _failed;

    /// <summary>Registers Kiln's config "knowledge" (tick thread; once). Null when that is not possible now.</summary>
    internal static ModConfig<KnowledgeSettings>? Ensure()
    {
        if (_file is not null || _failed || !Game.IsTickThread) return _file;
        var prev = ModContext.CurrentMod;
        ModContext.CurrentMod = null;  // Kiln's own config, not the calling mod's
        try
        {
            _file = Config.Find("knowledge") as ModConfig<KnowledgeSettings> ??
                    Config.Register<KnowledgeSettings>(ConfigKind.Common, "knowledge");
        }
        catch (Exception e)
        {
            _failed = true;
            Backend.Current.Log(0, LogLevel.Warn, $"knowledge: kiln/config/knowledge.json not registered ({e.Message}); the native service still reads it");
        }
        finally
        {
            ModContext.CurrentMod = prev;
        }
        return _file;
    }

    internal static void Save(KnowledgeSecurity s)
    {
        var f = Ensure() ?? throw new InvalidOperationException("Knowledge.Security: the config file is not available (call it on the tick thread)");
        f.Value.VetoClientKnowledge = s.VetoClientKnowledge;
        f.Value.CheckDialogue = s.CheckDialogue;
        f.Value.DialogueRange = s.DialogueRange;
        f.Value.LogClientKnowledge = s.LogClientKnowledge;
        f.Save();
    }
}
