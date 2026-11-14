// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// NoClip after GitHub issue #25 (the dev.28 fix round): the points of the issue a real game still has to show. Orders
// 152-156, next to rules.noclip (150) and rules.noclip-teleport (151) in Session1.cs, whose helpers these use.
//
// - rules.noclip-descent: sneak sinks every time it is held, also in the game's own sneak mode, and letting go stops
//   the sink (the mover's crouched rows said "do not steer"; kiln.rules flight.cpp now steers with the default row).
// - rules.noclip-death: a death inside the ground ends NoClip, and the respawned character stands with collision.
// - rules.noclip-world: closing the world with NoClip on inside the ground, and loading it again.
// - rules.noclip-disconnect: the same for a joined player who leaves a server and joins again.
public sealed partial class ClientPassMod
{
    // PlayerInput: the digital input's held bits (rules/input.mask + 8 on 26430253); Sneak 55
    private const int NoClipHeldBits = 0x3b0;
    private const int NoClipSneakBit = 55;
    private const string InSneakModeNote = ", in the game's sneak mode";

    [ClientCheck("rules.noclip-descent", "NoClip: sneak sinks every time, also in the game's sneak mode", Area = "Creative mode",
        Since = Dev28, Needs = ClientCheckNeeds.Any, Order = 152)]
    public async Task RulesNoClipDescent(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedNoClip(c)) return;
        c.OnEnd(() => NoClip.Set(c.Player, false));
        var on = NoClip.Set(c.Player, true);
        if (!c.Expect(on.Ok, "NoClip turned on for you", on.Message))
        {
            c.Instruct($"NoClip could not be turned on: {on.Message} Press fail, with that line as the note.");
            await c.Verdict();
            return;
        }
        c.Instruct("NoClip is on and you lift off; stay in open air for this check. Do these in any order, for about two " +
                   "minutes, each at least twice: (1) hold sneak while standing still in the air, then let go; (2) fly forward " +
                   "with sprint held and hold sneak meanwhile, then let go of everything; (3) press sneak ONCE and let go (the " +
                   "game's own sneak mode switches with it when the sneak key is set to toggle), then repeat (1) and (2); " +
                   "press sneak once more to leave the game's sneak mode. Each time you hold sneak you should sink, and each " +
                   "time you let go you should stop and hover, not drift on. The check counts the times you held sneak.");
        var windows = new List<(int Steps, double Drop, bool SneakMode)>();
        int held = 0, sneakModeSteps = 0;
        double from = 0;
        bool inMode = false;
        var until = DateTime.UtcNow.AddMinutes(3);
        while (DateTime.UtcNow < until && windows.Count < 12)
        {
            await c.Ticks();
            if (World.Position(c.Player) is not { } p) continue;
            var sneak = World.ReadBytes(c.Player, "PlayerInput") is { Length: > NoClipHeldBits + 8 } b &&
                        (BitConverter.ToUInt64(b, NoClipHeldBits) >> NoClipSneakBit & 1) != 0;
            var mode = PlayerStates.Is(c.Player, PlayerStateId.InSneakMode);
            if (mode && NoClip.Of(c.Player).Active) sneakModeSteps++;
            if (sneak && held == 0) { from = p.Y; inMode = false; }
            if (sneak) { held++; inMode |= mode; continue; }
            if (held >= 15) windows.Add((held, from - p.Y, inMode));
            held = 0;
        }
        c.Expect(windows.Count >= 4, $"you held sneak at least four times for a quarter second or more ({windows.Count} times)");
        var sank = windows.Count(w => w.Drop > 0.5);
        c.Expect(windows.Count > 0 && sank == windows.Count, $"every hold of sneak sank the server's character ({sank} of {windows.Count})",
            string.Join("; ", windows.Select(w => $"{w.Steps} steps: {w.Drop:0.0} m down" + (w.SneakMode ? InSneakModeNote : ""))));
        // Whether the game's sneak mode turns on in Kiln's flight at all: the cause the fix answers (the crouched rows).
        c.Expect(true, sneakModeSteps > 0
            ? $"the game's sneak mode was on during NoClip's flight for {sneakModeSteps} steps (the case of issue #25)"
            : "the game's sneak mode never turned on during NoClip's flight (say in the note whether your sneak key is set to toggle)");
        c.Instruct("Was it so: every time you held sneak you sank, also after pressing sneak once (the game's sneak mode) and " +
                   "while flying forward fast; every time you let go you stopped and hovered? In the note, say whether your " +
                   "sneak key is set to toggle or hold (Settings, Controls).");
        await c.Verdict();
    }

    [ClientCheck("rules.noclip-death", "NoClip: a death inside the ground ends it", Area = "Creative mode", Since = Dev28,
        Needs = ClientCheckNeeds.Any, Order = 153)]
    public async Task RulesNoClipDeath(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedNoClip(c)) return;
        var ended = new List<PlayerNoClipEndedEvent>();
        var sub = Events.On<PlayerNoClipEndedEvent>(e => { if (e.Player == c.Player) ended.Add(e); });
        c.OnEnd(() =>
        {
            sub.Dispose();
            NoClip.Set(c.Player, false);
        });
        var on = NoClip.Set(c.Player, true);
        if (!c.Expect(on.Ok, "NoClip turned on for you", on.Message))
        {
            c.Instruct($"NoClip could not be turned on: {on.Message} Press fail, with that line as the note.");
            await c.Verdict();
            return;
        }
        c.Instruct("Use a disposable character or world: this check ends your character's life. NoClip is on. Sink well into " +
                   "the ground (hold sneak) and stay there. The check ends your life when you are inside the ground.");
        var inside = await c.Until(() => World.Position(c.Player) is { } p && Build.TryRead(Build.BoxAround(p with { Y = p.Y + 1.0 }, 0.01, 0.01, 0.01), out var v) &&
                                         v.Length > 0 && !v[0].IsAir, TimeSpan.FromMinutes(3), "you are inside the ground");
        c.Expect(inside, "the server's character is inside the ground");
        Combat.SetInvulnerable(c.Player, false);
        Combat.SetHealth(c.Player, 0);
        var died = await c.Until(() => ended.Count > 0, TimeSpan.FromSeconds(30), "NoClip ends with the death");
        c.Expect(died && ended[0].Reason == NoClipEnd.Death, "NoClip ended because you died", died ? ended[0].ToString() : "no event");
        c.Expect(!Rules.Of(c.Player).NoClip, "your rules no longer carry NoClip", Rules.Of(c.Player).ToString());
        c.Instruct("Respawn as usual.");
        var feet = await c.Until(() => OnItsFeet(c.Player), TimeSpan.FromMinutes(2), "your character respawned and stands on the ground");
        c.Expect(feet && !NoClip.Of(c.Player).Active, "the respawned character is out of NoClip's flight, with collision", Locomotion(c.Player));
        c.Instruct("Was it so: the chat said NoClip ended because you died; after the respawn you stand on the ground, walk, " +
                   "run and jump with the usual animations, and cannot walk through a wall?");
        await c.Verdict();
    }

    [ClientCheck("rules.noclip-world", "NoClip: closing the world inside the ground", Area = "Creative mode", Since = Dev28,
        Needs = ClientCheckNeeds.Hosted, Order = 154)]
    public async Task RulesNoClipWorld(CheckContext c)
    {
        if (!await NeedPlayer(c) || !await NeedNoClip(c)) return;
        var on = NoClip.Set(c.Player, true);
        if (!c.Expect(on.Ok, "NoClip turned on for you", on.Message))
        {
            c.Instruct($"NoClip could not be turned on: {on.Message} Press fail, with that line as the note.");
            await c.Verdict();
            return;
        }
        c.Instruct("Use a disposable world. NoClip is on. Sink well into the ground (hold sneak) and stay there. Then, while " +
                   "you are inside the ground, quit to the main menu and load the same world again. NoClip does not come back " +
                   "on its own (Kiln's switch ends with the world), and Kiln cannot move you out on the way out: the game " +
                   "places a character that was saved inside the ground. Once you are in the world again, type /noclip " +
                   "status in the chat.");
        c.Instruct("Was it so: after loading you are not inside the ground and not falling under the world (standing on the " +
                   "ground, or put somewhere safe by the game), /noclip status says 'on for nobody', and you cannot walk " +
                   "through a wall? If you are stuck inside the ground, say so in the note and free yourself with the game's " +
                   "Unstuck (in the menu).");
        await c.Verdict();
    }

    [ClientCheck("rules.noclip-disconnect", "NoClip: leaving a server inside the ground", Area = "Creative mode", Since = Dev28,
        Needs = ClientCheckNeeds.Joined, Order = 155)]
    public async Task RulesNoClipDisconnect(CheckContext c)
    {
        c.Instruct("Join a server that runs Kiln with \"allowNoClip\": true in its kiln/config/rules.json, as a player with " +
                   "admin rights (or ask its admin to type /noclip on <you>). Type /noclip to turn NoClip on. Sink well into " +
                   "the ground (hold sneak) and stay there. Then leave the server (quit to the main menu) while you are inside " +
                   "the ground, and join it again.");
        c.Instruct("Was it so: when you came back you were not inside the ground and not falling under the world, NoClip was " +
                   "off (you cannot walk through a wall, /noclip status on the server says 'on for nobody'), and while NoClip " +
                   "was on, sinking and flying looked smooth on your screen, not pulled back? If you were stuck, say where in " +
                   "the note and use the game's Unstuck.");
        await c.Verdict();
    }
}
