// SPDX-License-Identifier: MIT
// Kiln.Npcs: a mod's lines in the game's own speech bubble over a friendly NPC (villagers, craftspeople and their
// assistants: every NPC with dialogue lines). Requested by Marshall (Ryrin) for survival add-ons: a hungry craftsperson
// says so, and stops greeting cheerfully while hungry.
//   Npcs.Say(npc, "I'm starving", TimeSpan.FromSeconds(6));            once, on demand
//   var s = Npcs.Speech(npc); s.Suppress(); / s.Replace("No food, no work."); s.Clear();   while a condition holds
// The host decides (a dedicated server or a hosted world); every Kiln client shows it on channel kiln:npcspeech
// (required: false, so a vanilla client just keeps the game's lines). On a joined client, calls change that player's
// own screen only. The bubble is client data from end to end (docs/re/npc-speech.md): kiln.npcspeech on the client
// hooks the game's bubble, kiln.content's loca hooks answer Kiln's texts. Docs: docs/kiln/services/npc-speech.md.
namespace Kiln;

/// <summary>The answer to a speech call.</summary>
/// <param name="Ok">True when it was accepted.</param>
/// <param name="Reason">Why not (null when <paramref name="Ok"/>), or, when accepted, a note on where it cannot show
/// (null when nothing to say).</param>
public sealed record SpeechResult(bool Ok, string? Reason) : IKilnResult
{
    internal static readonly SpeechResult Yes = new(true, null);
    internal static SpeechResult No(string why) => new(false, why);
    /// <summary>"ok" (with the note), or "refused:" and the reason.</summary>
    public override string ToString() => Ok ? Reason is null ? "ok" : $"ok ({Reason})" : $"refused: {Reason}";
}

/// <summary>How a <see cref="Npcs.Say(EntityId, UiText, TimeSpan, SayOptions?)"/> goes.</summary>
/// <param name="Interrupt">True (the default): it replaces a Say that is still showing over that NPC, also another
/// mod's. False: refused while another mod's Say shows (the calling mod's own is always replaced).</param>
public sealed record SayOptions(bool Interrupt = true);

/// <summary>One mod's override of one NPC's greeting bubble (<see cref="Npcs.Speech"/>). Several mods can each have one
/// for the same NPC: the highest <see cref="Priority"/> shows, on a tie the most recent <see cref="Suppress"/> or
/// <see cref="Replace(string[])"/>; when it clears, the next one does, else the game's own lines come back. It ends by
/// <see cref="Clear"/>, when the mod unloads, or when the world ends.</summary>
public sealed class NpcSpeech
{
    internal NpcSpeech(EntityId npc, string owner)
    {
        Npc = npc;
        ModId = owner;
    }

    /// <summary>The NPC.</summary>
    public EntityId Npc { get; }

    /// <summary>The mod this override belongs to ("kiln" outside a mod).</summary>
    public string ModId { get; }

    /// <summary>What this override does now (<see cref="SpeechMode.Game"/> when it is clear). Any thread.</summary>
    public SpeechMode Mode => NpcSpeechEngine.Book.ModeOf(NpcSpeechEngine.Layer, Npc.Value, ModId);

    /// <summary>Its rank against other mods' overrides of the same NPC (higher wins). Set it before
    /// <see cref="Suppress"/> or <see cref="Replace(string[])"/>; a change counts at the next of those.</summary>
    public int Priority { get; set; }

    /// <summary>No greeting bubble while this override is on, and no greeting voice-over (a player who presses E still
    /// gets the NPC's menu). Any thread.</summary>
    [RequiresCapability(SpeechNative.BubbleCapability)]
    public SpeechResult Suppress() => NpcSpeechEngine.Set(Npc, ModId, SpeechMode.Suppress, [], Priority);

    /// <summary>The NPC's greeting bubble shows one of these lines instead of its own (a new pick each time the game
    /// picks a new line), with no voice-over. 1 to 32 lines of up to 200 characters, the same in every language. Any thread.</summary>
    /// <exception cref="ArgumentException">No line, more than 32, or an empty or too long one.</exception>
    [RequiresCapability(SpeechNative.BubbleCapability)]
    public SpeechResult Replace(params string[] lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        return Replace(lines.Select(l => new UiText(l ?? "")));
    }

    /// <summary>As <see cref="Replace(string[])"/>, with a text per language for each line
    /// (<c>UiText.Of(("en", "..."), ("de", "..."))</c>).</summary>
    /// <exception cref="ArgumentException">No line, more than 32, or an empty or too long one.</exception>
    [RequiresCapability(SpeechNative.BubbleCapability)]
    public SpeechResult Replace(IEnumerable<UiText> lines)
    {
        ArgumentNullException.ThrowIfNull(lines);
        var all = lines.ToArray();
        if (all.Length == 0 || all.Length > SpeechText.MaxLines)
            throw new ArgumentException($"Replace takes 1 to {SpeechText.MaxLines} lines, not {all.Length}", nameof(lines));
        for (var i = 0; i < all.Length; i++)
            if (SpeechText.Problem(all[i]) is { } p) throw new ArgumentException($"line {i}: {p}", nameof(lines));
        return NpcSpeechEngine.Set(Npc, ModId, SpeechMode.Replace, all, Priority);
    }

    /// <summary>Ends this override: the next mod's shows, else the game's own lines. Any thread.</summary>
    public void Clear() => NpcSpeechEngine.Set(Npc, ModId, SpeechMode.Game, [], Priority);

    /// <summary>The NPC, the mod and the mode.</summary>
    public override string ToString() => $"speech of {Npc} by {ModId}: {Mode}";
}

/// <summary>Friendly NPCs' speech bubbles: a mod's own lines in the game's bubble over a villager, craftsperson or
/// assistant. The host decides; every Kiln client shows it; vanilla clients keep the game's lines.</summary>
public static class Npcs
{
    /// <summary>The shortest and longest <see cref="Say(EntityId, UiText, TimeSpan, SayOptions?)"/>.</summary>
    public static readonly TimeSpan MinSay = TimeSpan.FromMilliseconds(500), MaxSay = TimeSpan.FromSeconds(60);

    /// <summary>This game shows Kiln's speech in its own bubbles (a game client whose kiln.npcspeech found the bubble on
    /// this build: capability <c>npcspeech.bubble</c>). A dedicated server has no bubbles of its own and answers false;
    /// it still sends what mods decide to its Kiln clients. Any thread.</summary>
    public static bool Available => SpeechNative.Loaded && Capabilities.Is(SpeechNative.BubbleCapability);

    /// <summary>Shows <paramref name="text"/> in the NPC's speech bubble for <paramref name="duration"/> (0.5 s to 60 s),
    /// with no voice-over, also when the NPC is not the player's target: every Kiln player near the NPC sees it. It shows
    /// over any <see cref="Speech"/> override; a new Say of the same NPC replaces it. The same text in every language.
    /// Any thread.</summary>
    /// <exception cref="ArgumentException"><paramref name="npc"/> is None, the duration is out of range, or the text is
    /// empty or longer than 200 characters.</exception>
    [RequiresCapability(SpeechNative.BubbleCapability)]
    public static SpeechResult Say(EntityId npc, string text, TimeSpan duration, SayOptions? options = null) =>
        Say(npc, new UiText(text ?? ""), duration, options);

    /// <summary>As <see cref="Say(EntityId, string, TimeSpan, SayOptions?)"/>, with a text per language
    /// (<c>UiText.Of(("en", "I'm starving"), ("de", "Ich verhungere"))</c>).</summary>
    /// <exception cref="ArgumentException"><paramref name="npc"/> is None, the duration is out of range, or a text is
    /// empty or longer than 200 characters.</exception>
    [RequiresCapability(SpeechNative.BubbleCapability)]
    public static SpeechResult Say(EntityId npc, UiText text, TimeSpan duration, SayOptions? options = null)
    {
        if (npc.IsNone) throw new ArgumentException("no NPC (EntityId.None)", nameof(npc));
        if (duration < MinSay || duration > MaxSay)
            throw new ArgumentException($"a Say lasts {MinSay.TotalSeconds} to {MaxSay.TotalSeconds} s, not {duration.TotalSeconds} s", nameof(duration));
        if (SpeechText.Problem(text) is { } p) throw new ArgumentException(p, nameof(text));
        return NpcSpeechEngine.Say(npc, NpcSpeechEngine.Owner(), text, duration, options ?? new SayOptions());
    }

    /// <summary>Ends the calling mod's Say over the NPC before its time (another mod's Say stays). Any thread.</summary>
    public static void StopSaying(EntityId npc) => NpcSpeechEngine.StopSay(npc, NpcSpeechEngine.Owner());

    /// <summary>The calling mod's override of the NPC's greeting bubble (a new handle each call; they all name the same
    /// override). Any thread.</summary>
    /// <exception cref="ArgumentException"><paramref name="npc"/> is None.</exception>
    public static NpcSpeech Speech(EntityId npc)
    {
        if (npc.IsNone) throw new ArgumentException("no NPC (EntityId.None)", nameof(npc));
        NpcSpeechEngine.Start();
        return new NpcSpeech(npc, NpcSpeechEngine.Owner());
    }

    /// <summary>What the NPC's greeting bubble does now, from every mod's overrides (a Say aside). Any thread.</summary>
    public static SpeechMode SpeechOf(EntityId npc) => NpcSpeechEngine.Book.Effective(npc.Value, NpcSpeechEngine.Clock()).Mode;

    /// <summary>True when a Say is showing over the NPC now. Any thread.</summary>
    public static bool IsSaying(EntityId npc) => NpcSpeechEngine.Book.Effective(npc.Value, NpcSpeechEngine.Clock()).Say is not null;

    /// <summary>The entity is a friendly NPC with dialogue lines, the kind that has a speech bubble (a villager, a
    /// craftsperson, an assistant): it has the game's <c>DynamicNpcState</c> (server) or <c>DynamicNpcDialog</c>
    /// (client). Tick thread.</summary>
    [TickThreadOnly]
    public static bool CanSpeak(EntityId npc) => !npc.IsNone && (World.Has(npc, "DynamicNpcState") || World.Has(npc, "DynamicNpcDialog"));
}
