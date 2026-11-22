// shell/updateApply.ts and shell/unsaved.ts: when the launcher update restarts by itself, and which Kiln goes into the game
// by itself after the restart.
import { describe, expect, test } from "bun:test";
import type { GameStatus, InstallState } from "../src/api/commands";
import { applyWait, restartHold, runtimeToApply } from "../src/shell/updateApply";
import { unsavedKind } from "../src/shell/unsaved";

const rt = (p: Partial<InstallState>): InstallState => ({ installed: true, receipt: null, proxy: null, modified: [], upgradeAvailable: null, ...p } as InstallState);
const status = (p: Partial<GameStatus>, r: Partial<InstallState> = {}): GameStatus => ({ running: false, steamUpdating: false, runtime: rt(r), ...p } as GameStatus);

describe("the restart by itself", () => {
  test("the game, then unsaved work, then a task hold it; nothing: it restarts", () => {
    expect(restartHold({ running: true, unsaved: "studio", task: "Cook mods" })).toEqual({ kind: "game" });
    expect(restartHold({ running: false, unsaved: "editor", task: "Cook mods" })).toEqual({ kind: "unsaved", who: "editor" });
    expect(restartHold({ running: false, unsaved: null, task: "Cook mods" })).toEqual({ kind: "task", title: "Cook mods" });
    expect(restartHold({ running: false, unsaved: null, task: null })).toBeNull();
  });
  test("Pack Studio's work is named before an editor's; the save editor's store counts as an editor", () => {
    expect(unsavedKind({}, false)).toBeNull();
    expect(unsavedKind({ a: "editor", b: "studio" }, false)).toBe("studio");
    expect(unsavedKind({ a: "editor" }, false)).toBe("editor");
    expect(unsavedKind({}, true)).toBe("editor");
  });
});

describe("the Kiln the update brought", () => {
  test("only the launcher's newer Kiln, into a game that has Kiln", () => {
    expect(runtimeToApply(status({}, { upgradeAvailable: "0.2.0-dev+2" }))).toBe("0.2.0-dev+2");
    expect(runtimeToApply(status({}, { installed: false, upgradeAvailable: "0.2.0-dev+2" }))).toBeNull();
    expect(runtimeToApply(status({}, { upgradeAvailable: null, switchBack: { from: "0.2.1-dev+1", to: "0.2.0-dev+2" } }))).toBeNull();
    expect(runtimeToApply(undefined)).toBeNull();
  });
  test("it waits for the game's status, a running game and Steam's update", () => {
    expect(applyWait(undefined)).toBe("loading");
    expect(applyWait(status({ running: true }))).toBe("running");
    expect(applyWait(status({ steamUpdating: true }))).toBe("steam");
    expect(applyWait(status({}))).toBeNull();
  });
});
