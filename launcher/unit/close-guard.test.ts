import { describe, expect, test } from "bun:test";
import { closeLoss, closeQuestion } from "../src/shell/closeGuard";

describe("closing the window while work would be lost", () => {
  test("nothing to lose: no question", () => {
    expect(closeLoss({}, { dirty: false })).toBeNull();
    expect(closeLoss({}, { dirty: false, busy: false })).toBeNull();
  });
  test("a save being written comes first, then Pack Studio, then the editors", () => {
    expect(closeLoss({ a: "studio" }, { dirty: true, busy: true })).toBe("busy");
    expect(closeLoss({ a: "editor", b: "studio" }, { dirty: true })).toBe("studio");
    expect(closeLoss({ a: "editor" }, { dirty: false })).toBe("editor");
    expect(closeLoss({}, { dirty: true })).toBe("editor");
  });
  test("the save editor's changes name the character", () => {
    expect(closeQuestion("editor", { dirty: true, name: "Mira" }, {})).toContain("Mira");
    expect(closeQuestion("editor", { dirty: true, name: null }, {})).toContain("save editor");
    // another editor's changes too: the general line
    expect(closeQuestion("editor", { dirty: true, name: "Mira" }, { a: "editor" })).not.toContain("Mira");
    expect(closeQuestion("busy", { dirty: false, name: null }, {})).toContain("writing a save");
    expect(closeQuestion("studio", { dirty: false, name: null }, { a: "studio" })).toContain("Pack Studio");
  });
});
