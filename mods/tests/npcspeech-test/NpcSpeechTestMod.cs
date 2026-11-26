// SPDX-License-Identifier: MIT
// Lab driver for Kiln.Npcs: one console command per call, each replying with the result, so tools/nettest/npcspeechtest.py
// can line up what it sends a Kiln peer on kiln:npcspeech. Entity ids need not be real NPCs: the host checks nothing about
// the entity (a client's bubble does), which lets a fresh lab world without villagers test the server half.
using Kiln;

namespace NpcSpeechTest;

public sealed class NpcSpeechTestMod : Mod
{
    private IDisposable? _command;

    public override void OnLoad() =>
        _command = Commands.Register("npcspeech-test", Run, CommandPermission.Admin, usage: "say|replace|suppress|clear|stop|status <entity>",
            description: "Kiln.Npcs lab driver");

    public override void OnUnload() => _command?.Dispose();

    private static void Run(CommandContext c)
    {
        var verb = c.Count > 0 ? c.Args[0] : "status";
        var npc = c.Count > 1 && uint.TryParse(c.Args[1], out var id) ? new EntityId(id) : new EntityId(900001);
        var speech = Npcs.Speech(npc);
        SpeechResult? r = verb switch
        {
            "say" => Npcs.Say(npc, UiText.Of(("en", "I'm starving..."), ("de", "Ich verhungere...")), TimeSpan.FromSeconds(30)),
            "replace" => speech.Replace("No food, no work.", "My stomach...", "Is there nothing to eat?"),
            "suppress" => speech.Suppress(),
            "clear" => Do(speech.Clear),
            "stop" => Do(() => Npcs.StopSaying(npc)),
            _ => null,
        };
        var villagers = Villagers.List();
        c.Reply($"npcspeech-test {verb} {npc.Value}: {(r is null ? "-" : r.ToString())}; mode {Npcs.SpeechOf(npc)}, saying {Npcs.IsSaying(npc)}, " +
                $"available {Npcs.Available}, villagers {villagers.Count} (can speak {villagers.Count(v => Npcs.CanSpeak(v.Entity))}), " +
                $"kiln.npcspeech {Capabilities.Get("kiln.npcspeech").State}");
    }

    private static SpeechResult Do(Action a)
    {
        a();
        return new SpeechResult(true, null);
    }
}
