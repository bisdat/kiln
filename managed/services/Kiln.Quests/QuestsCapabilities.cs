// SPDX-License-Identifier: MIT
// Kiln.Quests in the capability registry (Kiln.Capabilities; docs/kiln/services/caps.md). Reported at the service's
// start (QuestsStartup, the first service tick) and at its first use, and every 25 steps while kiln.knowledge is here
// (a fault of the game's knowledge apply turns Set and Revoke off at run time). kiln.knowledge has only the core's row; the
// world's progress lists (WorldProgress.PlacesReady, BossesReady) are built on demand from kiln.assets' data and say so
// themselves: they are no capability (nothing throws for them) and get no row.
namespace Kiln;

internal static unsafe class QuestsCaps
{
    /// <summary>kiln.knowledge's table is here (the core's row).</summary>
    public const string Service = "kiln.knowledge";

    /// <summary>The sigdb features kiln.knowledge serves.</summary>
    public static readonly string[] Features =
        ["quests.knowledge", "quests.journal", "quests.dialogue", "quests.npc", "quests.changes", "quests.client_input",
         "quests.npc_pages"];

    /// <summary>The server world's roster snapshots (<see cref="NativeQuests.GetServerRosterWorld"/>,
    /// <see cref="NativeQuests.GetUnlock"/>; native row, since 0.2.0-dev.29).</summary>
    public const string RosterRead = "npcs.roster_read";

    /// <summary>The client world's roster snapshots (<see cref="NativeQuests.GetClientRosterWorld"/>,
    /// <see cref="NativeQuests.GetClientUnlock"/>; native row, a game client only).</summary>
    public const string ClientRosterRead = "npcs.client_roster_read";

    /// <summary>The roster's budget (<see cref="NativeQuests.GetRosterBudget"/>; native row).</summary>
    public const string RosterBudget = "npcs.roster_budget";

    /// <summary>Kiln's second unlock page: the roster can hold 128 entries on this side (native row: available when it
    /// is installed, or ready to install when a mod's roster needs more than 64 entries).</summary>
    public const string ExpandedRoster = "npcs.expanded_roster";

    private static readonly string[] RosterRows = [RosterRead, ClientRosterRead, RosterBudget, ExpandedRoster];

    internal static readonly ServiceCapRows Rows = new(Service, Report, everySteps: 25, refreshOnlyWithService: true);

    // kiln.knowledge's wrapper remembers a miss: forgotten when the refresh finds the table (docs/kiln/services/caps.md)
    static QuestsCaps() => ServiceCaps.WhenFound(Service, KnowledgeNative.LookAgain);

    private static void Report()
    {
        var t = KnowledgeNative.T;
        // kiln.knowledge reports the roster's rows with its table; missing or older, nobody would: say why here
        if (KnowledgeNative.Roster(t) == null)
        {
            var (rosterReason, rosterText) = t == null
                ? (CapabilityReason.ServiceMissing, "kiln.knowledge is not loaded")
                : (CapabilityReason.ServiceOlder, "this kiln.knowledge has no roster reads (update Kiln)");
            foreach (var row in RosterRows) ServiceCaps.Static(row, false, rosterReason, rosterText, Service);
        }
        if (t == null)
        {
            ServiceCaps.LowerFeatures(Service, Features);
            return;
        }
        // the table is published also when single hooks failed: each feature follows its switch in the status
        KnowledgeNative.Status s = default;
        s.Size = (uint)sizeof(KnowledgeNative.Status);
        if (t->Status == null || t->Status(&s) != KnowledgeNative.Ok) return;
        var why = KnowledgeNative.Text(s.Reason, 160);
        var text = "kiln.knowledge: " + (why.Length > 0 ? why : "this part is off on this build (see kiln.log)");
        if (s.Knowledge == 0) ServiceCaps.LowerOff("quests.knowledge", CapabilityReason.Faulted, text);
        if (s.Changes == 0) ServiceCaps.LowerOff("quests.changes", CapabilityReason.Faulted, text);
        if (s.Journal == 0) ServiceCaps.LowerOff("quests.journal", CapabilityReason.Faulted, text);
        if (s.Npc == 0) ServiceCaps.LowerOff("quests.npc", CapabilityReason.Faulted, text);
        if (s.Dialogue == 0) ServiceCaps.LowerOff("quests.dialogue", CapabilityReason.Faulted, text);
        if (s.ClientInput == 0) ServiceCaps.LowerOff("quests.client_input", CapabilityReason.Faulted, text);
    }

    /// <summary>The exception a roster read throws when its row is off.</summary>
    public static KilnUnavailableException Off(string capability, string detail)
    {
        Rows.Ensure();
        return ServiceCaps.Unavailable(capability, detail);
    }
}
