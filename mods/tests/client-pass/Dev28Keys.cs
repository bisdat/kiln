// SPDX-License-Identifier: MIT
using Kiln;
using Kiln.Testing;
using static Kiln.Testing.ClientCheckTags;

namespace ClientPass;

// 0.2.0-dev.28, the fix round's 00-integration.keys (GitHub issue 33): on a UK layout the key left of 1 sends VK_OEM_8,
// so a binding on "Backquote" (VK_OEM_3) did not fire there and fired on the ' key instead, and the Keys page saved that
// key as "Key223", which the next start could not read. Now a punctuation name is the key at that place on any layout
// (its scan code) and every key's name reads back. What the code can see is asserted (the presses each binding counted,
// the name the Keys page saved); the player switches the layout and presses the keys. The offline half is
// managed/tests/Kiln.Ui.Tests/KeyNameTests.cs. Order 6910 (00-integration's block).
public sealed partial class ClientPassMod
{
    [ClientCheck("00-keys.physical-backquote", "Backquote is the key left of 1 on a UK keyboard layout",
        Area = "Menus and hooks", Since = Dev28, Tags = [Visual], Needs = ClientCheckNeeds.Hosted, Order = 6910)]
    public async Task KeysPhysicalBackquote(CheckContext c)
    {
        if (!await NeedPlayer(c)) return;
        if (!c.Expect(KeyChord.TryParse("Key223", out _), "this Kiln reads key numbers (0.2.0-dev.28 or later)"))
        {
            c.Instruct("This Kiln cannot read \"Key223\" yet (0.2.0-dev.28 fixes it). Press fail.");
            await c.Verdict();
            return;
        }
        KeyBinding Bind(string id, string key)
        {
            var b = KeyBindings.Register("client-pass." + id, key, null, $"guided check 00-keys.physical-backquote: {key}", "Client pass");
            if (b.Key != b.Default) KeyBindings.Reset(b.Id);  // rebound in the Keys page by an earlier run
            return b;
        }
        var console = Bind("keys-backquote", "Backquote");
        var quote = Bind("keys-quote", "Quote");
        c.OnEnd(() =>
        {
            if (console.Key != console.Default) KeyBindings.Reset(console.Id);  // step 2 rebinds it
            console.Dispose();
            quote.Dispose();
        });
        int left = 0, apostrophe = 0;
        void Count()
        {
            left += console.TakePresses();
            apostrophe += quote.TakePresses();
        }
        string Counts() => $"Kiln has counted: Backquote {left}, Quote {apostrophe}.";

        // 1) the UK layout: the key left of 1 is Backquote, the ' key (right of ;) is Quote and not Backquote
        const string step1 = "Step 1 of 2. This check binds two keys of its own while it runs: Backquote and Quote. Switch " +
                             "Windows to the English (United Kingdom) keyboard layout (Settings > Time & language > " +
                             "Language & region: add it, then Win+Space switches; on Linux under Proton, the gb layout of " +
                             "your desktop). Close this list (Ctrl+Shift+F5) and, in play, press the key left of 1 (under " +
                             "Esc) exactly 3 times, then the ' key (right of ;, left of #) exactly 2 times. ";
        var shown = "";
        var done = await c.Until(() =>
        {
            Count();
            var text = step1 + Counts();
            if (text != shown) c.Instruct(shown = text);
            return left >= 3 && apostrophe >= 2;
        }, TimeSpan.FromMinutes(5), "you pressed the key left of 1 three times and the ' key twice");
        await c.Delay(TimeSpan.FromSeconds(1));
        Count();
        if (done)
            c.Expect(left == 3 && apostrophe == 2, "the key left of 1 counted as Backquote 3 times, the ' key as Quote 2 times",
                $"Backquote {left}, Quote {apostrophe} (Backquote above 3: the ' key fired it, which is the bug)");

        // 2) the Keys page saves the key left of 1 as Backquote (dev.27 saved Key223, lost at the next start)
        var parked = KeyChord.Parse("Ctrl+Shift+F11");
        KeyBindings.Rebind(console.Id, parked);  // so that the page's new key shows as a change
        c.Instruct("Step 2 of 2. Still on the UK layout: open Kiln's menu (Insert), go to Keys and find the group \"Client " +
                   "pass\": its Backquote line now shows Ctrl+Shift+F11. Click that key, then press the key left of 1 " +
                   "(no modifier). The line must show Backquote, not Key223 or Oem8. Close the menu and switch back to " +
                   "your own layout. Pass when step 1 counted 3 and 2 and the line showed Backquote; else fail and write " +
                   "what the line showed. " + Counts());
        await c.Until(() => console.Key != parked, TimeSpan.FromMinutes(3), "you gave the Backquote line a new key");
        c.Log.Info($"client-pass: 00-keys.physical-backquote counted Backquote {left}, Quote {apostrophe}; the line holds {console.Key}");
        c.Expect(console.Key == KeyChord.Parse("Backquote"), "the Keys page saved the key left of 1 as Backquote",
            $"it saved {console.Key}");
        await c.Verdict();
    }
}
