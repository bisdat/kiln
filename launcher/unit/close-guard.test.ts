import { describe, expect, test } from "bun:test";
import { closeLoss, closeQuestion, onCloseRequest, type CloseDeps } from "../src/shell/closeGuard";

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

// (0.2.0-dev.31.13) the backend closes the window by itself when the page does not say it took the request
// (src-tauri/src/webproc.rs): the answer goes out first, before any question
describe("the page answers the backend's close request first", () => {
  function deps(over: Partial<CloseDeps> = {}) {
    const said: string[] = [];
    const d: CloseDeps = {
      by: {}, edits: { dirty: false, busy: false, name: null },
      seen: async (asking) => { said.push(asking ? "seen:asking" : "seen:closing"); },
      ask: async () => { said.push("ask"); return true; },
      destroy: async () => { said.push("destroy"); },
      ...over,
    };
    let prevented = false;
    return { d, said, e: { preventDefault: () => { prevented = true; } }, prevented: () => prevented };
  }
  test("nothing to lose: says it closes, the window closes as Tauri does it", async () => {
    const x = deps();
    await onCloseRequest(x.e, x.d);
    expect(x.said).toEqual(["seen:closing"]);
    expect(x.prevented()).toBe(false);
  });
  test("work to lose: says it asks before the question; Close destroys, Stay keeps", async () => {
    const x = deps({ by: { a: "editor" } });
    await onCloseRequest(x.e, x.d);
    expect(x.said).toEqual(["seen:asking", "ask", "destroy"]);
    expect(x.prevented()).toBe(true);
    const y = deps({ by: { a: "studio" } });
    y.d.ask = async () => { y.said.push("ask"); return false; };
    await onCloseRequest(y.e, y.d);
    expect(y.said).toEqual(["seen:asking", "ask"]);
  });
  test("a backend that cannot hear it, or a question that cannot open, never traps the window", async () => {
    const x = deps({ by: { a: "editor" }, seen: () => Promise.reject(new Error("no ipc")), ask: () => Promise.reject(new Error("no dialog")) });
    await onCloseRequest(x.e, x.d);
    expect(x.said).toEqual(["destroy"]);
  });
});
