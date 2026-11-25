// shell/updateApply.ts and shell/unsaved.ts: when the launcher update restarts by itself, and which Kiln goes into the game
// by itself after the restart.
import { describe, expect, test } from "bun:test";
import type { GameStatus, InstallState } from "../src/api/commands";
import { applyKey, applyWait, gameBehind, loadDecided, restartHold, runtimeToApply, saveDecided } from "../src/shell/updateApply";
import { unsavedKind } from "../src/shell/unsaved";

const rt = (p: Partial<InstallState>): InstallState => ({ installed: true, receipt: null, proxy: null, modified: [], upgradeAvailable: null, ...p } as InstallState);
const status = (p: Partial<GameStatus>, r: Partial<InstallState> = {}): GameStatus => ({ running: false, steamUpdating: false, runtime: rt(r), ...p } as GameStatus);

describe("the restart by itself", () => {
  test("the game, then a start under way, then unsaved work, then a task hold it; nothing: it restarts", () => {
    expect(restartHold({ running: true, launching: true, unsaved: "studio", task: "Cook mods" })).toEqual({ kind: "game" });
    expect(restartHold({ running: false, launching: true, unsaved: "studio", task: "Cook mods" })).toEqual({ kind: "launch" });
    expect(restartHold({ running: false, unsaved: "editor", task: "Cook mods" })).toEqual({ kind: "unsaved", who: "editor" });
    expect(restartHold({ running: false, unsaved: null, task: "Cook mods" })).toEqual({ kind: "task", title: "Cook mods" });
    expect(restartHold({ running: false, unsaved: null, task: null })).toBeNull();
  });
  test("the save editor writing a save (a restore has no unsaved changes) is waited for, after the game and its start", () => {
    expect(restartHold({ running: false, writing: true, unsaved: null, task: null })).toEqual({ kind: "writing" });
    expect(restartHold({ running: false, writing: true, unsaved: "editor", task: "Cook mods" })).toEqual({ kind: "writing" });
    expect(restartHold({ running: false, launching: true, writing: true, unsaved: null, task: null })).toEqual({ kind: "launch" });
    expect(restartHold({ running: true, writing: true, unsaved: null, task: null })).toEqual({ kind: "game" });
  });
  test("Pack Studio's work is named before an editor's; the save editor's store counts as an editor", () => {
    expect(unsavedKind({}, false)).toBeNull();
    expect(unsavedKind({ a: "editor", b: "studio" }, false)).toBe("studio");
    expect(unsavedKind({ a: "editor" }, false)).toBe("editor");
    expect(unsavedKind({}, true)).toBe("editor");
  });
});

describe("the Kiln the update brought", () => {
  test("only the launcher's own (shipped) Kiln when it is newer, into a game that has Kiln", () => {
    const shipped = "0.2.0-dev+2";
    expect(runtimeToApply(status({}, { upgradeAvailable: "0.2.0-dev+2" }), shipped)).toBe("0.2.0-dev+2");
    expect(runtimeToApply(status({}, { installed: false, upgradeAvailable: "0.2.0-dev+2" }), shipped)).toBeNull();
    expect(runtimeToApply(status({}, { upgradeAvailable: null, switchBack: { from: "0.2.1-dev+1", to: "0.2.0-dev+2" } }), shipped)).toBeNull();
    expect(runtimeToApply(undefined, shipped)).toBeNull();
    // a newer bundle in the Library (a test build's), or a shipped runtime not known yet: a manual offer only
    expect(runtimeToApply(status({}, { upgradeAvailable: "0.2.1-dev+9" }), shipped)).toBeNull();
    expect(runtimeToApply(status({}, { upgradeAvailable: "0.2.0-dev+2" }), null)).toBeNull();
  });
  test("one automatic install per update and game, whatever runtime the next offer names", () => {
    expect(applyKey("0.2.0-dev.30.5", "client-a")).toBe(applyKey("0.2.0-dev.30.5", "client-a"));
    expect(applyKey("0.2.0-dev.30.5", "client-a")).not.toBe(applyKey("0.2.0-dev.30.5", "client-b"));
  });
  test("it waits for the game's status, a running game and Steam's update", () => {
    expect(applyWait(undefined)).toBe("loading");
    expect(applyWait(status({ running: true }))).toBe("running");
    expect(applyWait(status({ steamUpdating: true }))).toBe("steam");
    expect(applyWait(status({}))).toBeNull();
  });
});

describe("Play's Update game banner (0.2.0-dev.31.12)", () => {
  const has = "0.2.0-dev+1", next = "0.2.0-dev+2";
  const receipt = { runtime: { version: has } } as InstallState["receipt"];
  const behind = (p: Partial<GameStatus>, r: Partial<InstallState>, o: Parameters<typeof gameBehind>[1] = { channel: false }) =>
    gameBehind(status(p, { receipt, ...r }), o);
  test("the game's Kiln older than the launcher's: both versions, nothing holds it", () => {
    expect(behind({}, { upgradeAvailable: next })).toEqual({ has, next, hold: null, failed: false });
  });
  test("none when equal or newer (no offer), without Kiln, after a switch back, under the channel notice, before the status", () => {
    expect(behind({}, { upgradeAvailable: null })).toBeNull();
    expect(behind({}, { upgradeAvailable: has })).toBeNull();
    expect(behind({}, { installed: false, upgradeAvailable: next })).toBeNull();
    expect(behind({}, { upgradeAvailable: next, switchBack: { from: "0.2.1-dev+1", to: next } })).toBeNull();
    expect(behind({}, { upgradeAvailable: next }, { channel: true })).toBeNull();
    expect(gameBehind(undefined, { channel: false })).toBeNull();
  });
  test("held while a change runs, then the game runs, then it starts, then Steam updates it", () => {
    const running = { state: "running", version: next };
    expect(behind({ running: true, steamUpdating: true }, { upgradeAvailable: next }, { channel: false, starting: true, change: running })?.hold).toBe("applying");
    expect(behind({ running: true, steamUpdating: true }, { upgradeAvailable: next }, { channel: false, starting: true })?.hold).toBe("running");
    expect(behind({ steamUpdating: true }, { upgradeAvailable: next }, { channel: false, starting: true })?.hold).toBe("starting");
    expect(behind({ steamUpdating: true }, { upgradeAvailable: next })?.hold).toBe("steam");
  });
  test("a failed install of this very version is said on it; another change's failure is not", () => {
    expect(behind({}, { upgradeAvailable: next }, { channel: false, change: { state: "failed", version: next } })?.failed).toBe(true);
    expect(behind({}, { upgradeAvailable: next }, { channel: false, change: { state: "failed", version: "0.1.9+1" } })?.failed).toBe(false);
    expect(behind({}, { upgradeAvailable: next }, { channel: false, change: { state: "done", version: next } })?.failed).toBe(false);
  });
});

// (review, 0.2.0-dev.31.13) Later was React state only: a page loaded again after its web process ended
// (src-tauri/src/webproc.rs) counted down and restarted into the update the player had put off
describe("the player's Later lasts for this run of the launcher", () => {
  const store = () => { const m = new Map<string, string>(); return { getItem: (k: string) => m.get(k) ?? null, setItem: (k: string, v: string) => { m.set(k, v); } }; };
  test("a page loaded again reads what the page before it decided", () => {
    const s = store();
    expect(loadDecided(s)).toEqual({});
    saveDecided(s, { "0.2.0-dev.31.14": "stopped", "0.2.0-dev.31.15": "held" });
    expect(loadDecided(s)).toEqual({ "0.2.0-dev.31.14": "stopped", "0.2.0-dev.31.15": "held" });
  });
  test("nothing stored, no storage, or anything unreadable is no decision", () => {
    expect(loadDecided(null)).toEqual({});
    saveDecided(null, { a: "stopped" });
    for (const bad of ["not json", "[1]", "null", "{\"a\":\"go\",\"b\":\"stopped\"}"]) {
      const s = store(); s.setItem("kiln.update.decided", bad);
      expect(loadDecided(s)).toEqual(bad.includes("stopped") ? { b: "stopped" } : {});
    }
    const throwing = { getItem: () => { throw new Error("denied"); }, setItem: () => { throw new Error("denied"); } };
    expect(loadDecided(throwing)).toEqual({});
    expect(() => saveDecided(throwing, { a: "held" })).not.toThrow();
  });
});
