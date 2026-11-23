// Settings' lines of help: a kiln.json comment's one line (ui/SchemaForm.tsx commentHead) is the help, not the subject
// before its colon ('With the DLSS render size perMode', "Kiln's frame cap", "Chat history"), and More holds only what the
// line does not say (pages/settings/model.ts moreBeyond).
import { expect, test } from "bun:test";
import type { SchemaSnapshot } from "../src/api/commands";
import { graphicsLabels } from "../src/components/GraphicsSettings";
import snapshot from "../src/data/kiln-json.snapshot.json";
import { moreBeyond } from "../src/pages/settings/model";
import { kilnLabels } from "../src/pages/settings/searchIndex";
import { commentHead } from "../src/ui/SchemaForm";

const schema = snapshot as unknown as SchemaSnapshot;
const labels: Record<string, string> = { ...kilnLabels(), ...graphicsLabels() };
const head = (path: string) => {
  const e = schema.entries.find((x) => x.path === path)!;
  return commentHead(e.comment!, labels[path]);
};

test("a comment that opens with when it applies shows the clause after the colon", () => {
  expect(head("graphics.dlssQualityScale")).toBe("The render scale of DLSS Quality (the default is DLSS's own)");
  expect(head("graphics.dlssUltraPerformanceScale")).toBe("The render scale of DLSS Ultra Performance (the default is DLSS's own)");
  expect(head("graphics.dlssRenderScale")).toBe("Render width and height as a share of the output (0.67 = 67 %)");
  expect(head("ui.theme")).toBe("The theme (<mod id>:<name>; Kiln's in-game Look setting lists the ones your mods offer)");
});

test("a comment that opens with the row's name, or a few words, shows the help after it, cut at its first clause", () => {
  expect(head("graphics.dlssScale")).toBe("Game, perMode or custom.");
  expect(head("graphics.fpsCap")).toBe("The game renders at most this many frames a second.");
  expect(head("ui.chatHistory")).toBe("Up/Down recall sent lines; Ctrl+R searches them; / commands persist across restarts");
  expect(head("ui.chatCompletion")).toBe("Tab suggests the Kiln commands you may use in the HUD and Social chat");
  expect(head("ui.overlay")).toBe("Auto or off. Applies from the next game start");
});

test("a comment whose part before the colon says what the entry is keeps it", () => {
  expect(head("ui.look")).toBe("Look of Kiln's in-game menus and windows");
  expect(head("graphics.readout")).toBe("A small readout in a corner of the game");
  expect(head("ui.outlines")).toBe("Mods' outlines on things in the world against the game's own highlight");
  // short comments show whole
  expect(head("ui.scale")).toBeNull();
  expect(head("mods.fault_limit")).toBeNull();
});

test("no line of help is only the row's name or a subject before a colon", () => {
  for (const e of schema.entries) {
    if (!e.comment) continue;
    const h = commentHead(e.comment, labels[e.path]);
    if (h === null) continue;
    const words = (s: string) => s.toLowerCase().replace(/[^a-z0-9]+/g, " ").trim().replace(/^the /, "");
    expect(words(h), e.path).not.toBe(words(labels[e.path] ?? ""));
    expect(h.startsWith("With "), e.path).toBe(false);
    expect(h.length, e.path).toBeLessThanOrEqual(92);
  }
});

test("More holds only what the line does not say", () => {
  // a More that opens with the line: the rest
  expect(moreBeyond("The launcher's own texts. Applies at once.",
    "The launcher's own texts. Applies at once; a text a translation lacks shows in English. Messages from Kiln itself stay in English for now."))
    .toBe("A text a translation lacks shows in English. Messages from Kiln itself stay in English for now.");
  expect(moreBeyond("Choose FSR 3 as the upscaler in the game's graphics settings: Kiln runs your choice in its place.",
    "Choose FSR 3 as the upscaler in the game's graphics settings: Kiln runs your choice in its place. A change applies at once."))
    .toBe("A change applies at once.");
  // nothing more: no More
  expect(moreBeyond("Off: https only.", "Off: https only.")).toBeUndefined();
  // a More that says something else stays as it is; none stays none
  expect(moreBeyond("For a window that stays black.", "Applies at the next start.")).toBe("Applies at the next start.");
  expect(moreBeyond(undefined, "Text.")).toBe("Text.");
  expect(moreBeyond("Line.", undefined)).toBeUndefined();
});
