// SPDX-License-Identifier: MIT
// The kit's fake kiln.ui.input (Fakes/UiInput/, since 0.2.0-dev.31.13): KeyInput.Listen with KeyListenFlags.Own and PadInput
// on it. XHL's two cases (Blueprint's right-button gesture, an overlay that closes on a controller button without the game
// seeing the release), the controllers' view, a row switched off, and native/services/ui/tests/own_parity.json: the cases
// the native host test (input_own_test.cpp) runs on input_own.h, replayed here through Kiln.Ui's own code.
using System.Numerics;
using System.Reflection;
using System.Text.Json.Nodes;
using Xunit;

namespace Kiln.TestKit.Tests;

[Collection("UiInputFake")]
public sealed unsafe class UiInputFakeTests
{
    [Fact]
    public void TheTableIsTheHeadersV1()
    {
        var t = (uint*)KitBackend.Instance.Service("kiln.ui.input", 1);  // KilnServiceHeader { size, version }, five functions
        Assert.True(t != null);
        Assert.Equal((48u, 1u), (t[0], t[1]));
    }

    [Fact]
    public void BlueprintOwnsTheRightButtonGestureAndTheGameSeesNoneOfIt()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var got = new List<KeyPress>();
        var placing = false;
        using var l = kiln.Run(() => KeyInput.Listen(p =>
        {
            got.Add(p);
            if (!p.IsPointer || p.VirtualKey != 0x02) return false;
            if (p.Down && !p.Owned) placing = true;            // the press starts the gesture
            if (!p.Down && p.Owned) placing = false;           // its release (or its end) finishes it
            return true;                                       // own it
        }, KeyListenFlags.Own | KeyListenFlags.Pointer));
        Assert.True(l.Ok && l.Own && l.Pointer);
        Assert.True(kiln.Input.Down(0x02, at: new Vector2(640, 360)));
        Assert.True(placing && l.Owns(0x02) && kiln.Input.HiddenFromGame(0x02));
        Assert.True(kiln.Input.Up(0x02, at: new Vector2(700, 390)));
        Assert.False(placing);
        Assert.Empty(kiln.Input.Game);    // neither the press nor the release
        Assert.Empty(kiln.Input.Others);  // nor any other key handler
        Assert.Equal(new Vector2(640, 360), got[0].Position);
        Assert.True(got[1].Owned && !got[1].Down && got[1].Ended == KeyOwnEnd.None && got[1].Position == new Vector2(700, 390));
        // the left button is not Blueprint's: everyone gets it
        Assert.False(kiln.Input.Down(0x01));
        Assert.False(kiln.Input.Up(0x01));
        Assert.Equal(["down 1", "up 1"], kiln.Input.Game);
        // focus loss mid-gesture: Blueprint hears the end; the release that comes back later is the game's
        kiln.Input.ClearLogs();
        Assert.True(kiln.Input.Down(0x02));
        kiln.Input.FocusLost();
        Assert.Equal(KeyOwnEnd.Focus, got[^1].Ended);
        Assert.False(placing);
        Assert.False(kiln.Input.Up(0x02));
        Assert.Equal(["up 2"], kiln.Input.Game);
    }

    [Fact]
    public void AnOverlayThatClosesOnControllerBNeverLetsTheGameSeeTheRelease()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        var open = true;
        KeyListener? overlay = null;
        overlay = kiln.Run(() => KeyInput.Listen(p =>
        {
            if (!open || !p.IsPad || p.VirtualKey != 0x20D || !p.Down) return false;
            open = false;          // B closes the overlay...
            overlay!.Disown();     // ...and gives the press up as it goes: its release reaches nobody
            return true;
        }, KeyListenFlags.Own | KeyListenFlags.Pad));
        Assert.True(kiln.Input.Down(0x20D, controller: 1));
        Assert.False(open);
        Assert.False(overlay.Owns(0x20D, 1));
        Assert.True(kiln.Input.HiddenFromGame(0x20D, 1));
        Assert.True(kiln.Input.Up(0x20D, controller: 1));  // kept: the game never saw B go down
        Assert.Empty(kiln.Input.Game);
        Assert.Empty(kiln.Input.Others);
        // closed now: B is the game's again
        Assert.False(kiln.Input.Down(0x20D, controller: 1));
        Assert.False(kiln.Input.Up(0x20D, controller: 1));
        Assert.Equal(["down 20d@1", "up 20d@1"], kiln.Input.Game);
        overlay.Dispose();
    }

    [Fact]
    public void PadInputReadsTheGamesLastPollWithTheOwnedButtons()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        Assert.False(PadInput.Read(0).Connected);
        kiln.Input.SetPad(0, PadButtons.B | PadButtons.RT, new Vector2(0.5f, -0.25f), default, 0, 0.8f);
        using var l = kiln.Run(() => KeyInput.Listen(p => p.VirtualKey == 0x20D, KeyListenFlags.Own | KeyListenFlags.Pad));
        Assert.True(kiln.Input.Down(0x20D));
        var p = PadInput.Read(0);
        Assert.True(p.Connected && p.IsDown(PadButtons.B | PadButtons.RT) && p.RightTrigger == 0.8f && p.LeftStick == new Vector2(0.5f, -0.25f));
        Assert.Equal((PadButtons.B, PadButtons.B, 1ul), (p.Owned, p.Hidden, p.Polls));
        Assert.Equal((short)16384, p.RawSticks.LeftX);
        Assert.True(PadInput.Available);
        kiln.Input.Unplug(0);
        Assert.False(PadInput.Read(0).Connected);
        kiln.Capabilities.Override(UiInputFake.PadRow, false);
        Assert.False(PadInput.Available);
        kiln.Capabilities.ClearOverride(UiInputFake.PadRow);
    }

    [Fact]
    public void WithTheOwnedRowOffAListenerThatOwnsIsRefused()
    {
        using var kiln = new KilnHarness();
        kiln.Step();
        kiln.Capabilities.Override(UiInputFake.OwnedRow, false);
        var l = kiln.Run(() => KeyInput.Listen(_ => true, KeyListenFlags.Own));
        Assert.False(l.Ok);
        Assert.Contains("refused", l.Reason);
        kiln.Capabilities.ClearOverride(UiInputFake.OwnedRow);
    }

    // ---- own_parity.json ----

    private static JsonNode Parity()
    {
        var repo = typeof(UiInputFakeTests).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>().Single(a => a.Key == "KilnRepository").Value!;
        return JsonNode.Parse(File.ReadAllText(Path.Combine(repo, "native", "services", "ui", "tests", "own_parity.json")))!;
    }

    public static TheoryData<string> Cases()
    {
        var d = new TheoryData<string>();
        foreach (var c in Parity()["cases"]!.AsArray()) d.Add((string)c!["name"]!);
        return d;
    }

    private static (uint Vk, int Controller) Code(string s)
    {
        var at = s.IndexOf('@');
        var vk = Convert.ToUInt32(at < 0 ? s : s[..at], 16);
        return (vk, at < 0 ? 0 : int.Parse(s[(at + 1)..]));
    }

    private static string Text(uint vk, int controller) => controller != 0 ? $"{vk:x}@{controller}" : $"{vk:x}";

    private static readonly uint[] MouseMk = [0x01, 0x02, 0x04, 0x05, 0x06];
    private static readonly uint[] MkBits = [0x01, 0x02, 0x10, 0x20, 0x40];

    [Theory]
    [MemberData(nameof(Cases))]
    public void TheNativeParityCasesHoldThroughKeyInput(string name)
    {
        var c = Parity()["cases"]!.AsArray().Single(x => (string)x!["name"]! == name)!;
        using var kiln = new KilnHarness();
        kiln.Step();
        var owners = new List<(string Name, KeyListener Listener, List<string> Log)>();
        foreach (var o in c["owners"]!.AsArray())
        {
            var log = new List<string>();
            var takes = o!["take"]!.AsArray().Select(t => (string)t!).ToList();
            var flags = KeyListenFlags.Own | KeyListenFlags.None;
            foreach (var s in o["sources"]!.AsArray().Select(x => (string)x!))
                flags |= s switch { "mouse" => KeyListenFlags.Mouse, "pointer" => KeyListenFlags.Pointer, "pad" => KeyListenFlags.Pad, _ => 0 };
            var keys = o["sources"]!.AsArray().Any(x => (string)x! == "keys");
            var l = kiln.Run(() => KeyInput.Listen(p =>
            {
                // a KeyInput listener always has the keyboard: a case's owner without "keys" passes keys unseen
                if (!keys && !p.Owned && p.VirtualKey < 0x100 && !p.IsPointer && !p.IsMouse) return false;
                var code = Text(p.VirtualKey, p.Controller);
                if (p.Ended != KeyOwnEnd.None) log.Add($"end {code} {p.Ended.ToString().ToLowerInvariant()}");
                else if (!p.Owned) log.Add($"press {code}");
                else log.Add(p.Down ? $"repeat {code}" : $"release {code}");
                if (p.Owned || p.Ended != KeyOwnEnd.None) return false;
                return takes.Contains("*") || takes.Contains($"{p.VirtualKey:x}");
            }, flags));
            Assert.True(l.Ok, l.Reason);
            owners.Add(((string)o["name"]!, l, log));
        }
        foreach (var step in c["steps"]!.AsArray())
        {
            var s = step!.AsArray();
            var op = (string)s[0]!;
            switch (op)
            {
                case "down": { var (vk, ctl) = Code((string)s[1]!); kiln.Input.Down(vk, ctl); break; }
                case "repeat": { var (vk, _) = Code((string)s[1]!); kiln.Input.Repeat(vk); break; }
                case "up": { var (vk, ctl) = Code((string)s[1]!); kiln.Input.Up(vk, ctl); break; }
                case "focus": kiln.Input.FocusLost(); break;
                case "world": kiln.Input.WorldChange(); break;
                case "unsubscribe": owners.Single(o => o.Name == (string)s[1]!).Listener.Dispose(); break;
                case "disown": owners.Single(o => o.Name == (string)s[1]!).Listener.Disown(); break;
                case "leak": { var (vk, ctl) = Code((string)s[1]!); kiln.Input.Leak(vk, ctl); break; }
                case "mk":
                {
                    uint mk = 0;
                    for (var i = 0; i < MouseMk.Length; i++)
                        if (kiln.Input.HiddenFromGame(MouseMk[i])) mk |= MkBits[i];
                    Assert.Equal((uint)s[1]!, mk);
                    break;
                }
                case "pad": Assert.Equal((uint)s[2]!, (uint)PadInput.Read((int)s[1]!).Owned); break;
                default: Assert.Fail($"unknown step {op}"); break;
            }
        }
        Assert.Equal(c["game"]!.AsArray().Select(x => (string)x!), kiln.Input.Game);
        Assert.Equal(c["plain"]!.AsArray().Select(x => (string)x!), kiln.Input.Others);
        foreach (var (n, l, log) in owners)
        {
            Assert.Equal(c[n]!.AsArray().Select(x => (string)x!), log);
            l.Dispose();
        }
    }
}
