// the warning before Play: the flow every start goes through (src/shell/sfiWarnings.ts):
// launch_play (the prepare: it starts at once without warnings), the dialog for the preflight it answers, confirm of exactly that token; Cancel confirms nothing; a changed set of
// warnings is a new dialog, never a confirm of its own; a refusal is passed on; a preflight of another game is refused;
// with no dialog host nothing is confirmed.
import { beforeEach, expect, test } from "bun:test";
import type { LaunchRequest, LaunchResult, Preflight, StartWarning } from "../src/api/commands";
import {
  askStartWarnings, isCanceledStart, launchWithPreflight, mountStartWarningHost, preflightOf, useStartWarnings, type StartIo,
} from "../src/shell/sfiWarnings";

const REQ: LaunchRequest = { mode: "profile", profileId: null };
const skipped = (id: string): StartWarning => ({
  kind: "package-skipped", code: "dependency-missing", message: `${id}: needs missing-lib`, severity: "warning",
  package: { id, version: "1.0.0", root: `mods/${id}`, saveSensitive: false, data: false, lua: true, dlls: [], reasons: [{ code: "dependency-missing", message: "needs missing-lib", severity: "warning" }] },
});
let n = 0;
const pf = (warnings: StartWarning[], gameId = "a"): Preflight => ({
  gameId, request: REQ, token: `t${++n}`, expires: "", warnings, warningsSha256: String(warnings.length), reinspectsEveryPlay: false,
});
const result = (gameId: string): LaunchResult => ({ gameId, mode: "profile", nonce: null, backup: null, uri: "", started: "" });
const pending = (p: Preflight) => ({ code: "consent_required", message: "changed", detail: { preflight: p } });

/** A fake backend: what launch_play does (starts, or answers the preflight `first` as consent_required), then what
 *  each confirm does; every call written down. */
function backend(first: Preflight | null, confirms: ((token: string) => LaunchResult)[] = []) {
  const calls: string[] = [];
  const io: StartIo = {
    play: async (g, r) => {
      calls.push(`play ${g} ${r.mode}`);
      if (!first) return result(g);
      throw pending(first);
    },
    confirm: async (g, token) => {
      calls.push(`confirm ${g} ${token}`);
      const next = confirms.shift();
      if (!next) throw new Error("unexpected confirm");
      return next(token);
    },
    cancel: async (g, token) => { calls.push(`cancel ${g} ${token}`); return true; },
  };
  return { io, calls };
}
const asker = (answers: boolean[]) => {
  const seen: [string, number][] = [];
  return { seen, ask: async (p: Preflight, round = 0) => { seen.push([p.token, round]); return answers.shift() ?? false; } };
};

beforeEach(() => { n = 0; useStartWarnings.setState({ asks: [], hosts: 0 }); });

test("no warnings: launch_play starts at once, no dialog, no confirm", async () => {
  const { io, calls } = backend(null);
  const a = asker([]);
  expect((await launchWithPreflight("a", REQ, a.ask, io)).gameId).toBe("a");
  expect(a.seen).toEqual([]);
  expect(calls).toEqual(["play a profile"]);
});

test("launch_play refused for another reason: passed on as it came, no dialog", async () => {
  const busy = { code: "game_running", message: "running" };
  const io: StartIo = { play: async () => { throw busy; }, confirm: async () => { throw new Error("no"); }, cancel: async () => true };
  const a = asker([true]);
  expect(await launchWithPreflight("a", REQ, a.ask, io).catch((x) => x)).toBe(busy);
  expect(a.seen).toEqual([]);
});

test("warnings: Cancel drops the token and confirms nothing", async () => {
  const p = pf([skipped("needy")]);
  const { io, calls } = backend(p);
  const a = asker([false]);
  const e = await launchWithPreflight("a", REQ, a.ask, io).catch((x) => x);
  expect(isCanceledStart(e)).toBe(true);
  expect(a.seen).toEqual([[p.token, 0]]);
  expect(calls).toEqual(["play a profile", `cancel a ${p.token}`]);
});

test("warnings: Continue confirms exactly the token the dialog showed, once", async () => {
  const p = pf([skipped("needy")]);
  const { io, calls } = backend(p, [(t) => { expect(t).toBe(p.token); return result("a"); }]);
  const a = asker([true]);
  await launchWithPreflight("a", REQ, a.ask, io);
  expect(calls).toEqual(["play a profile", `confirm a ${p.token}`]);
});

test("a changed set is a new dialog with the new token, never confirmed on its own; Cancel there confirms nothing more", async () => {
  const first = pf([skipped("needy")]), changed = pf([skipped("needy"), skipped("needy2")]);
  const b = backend(first, [() => { throw pending(changed); }, () => result("a")]);
  const a = asker([true, true]);
  await launchWithPreflight("a", REQ, a.ask, b.io);
  expect(a.seen).toEqual([[first.token, 0], [changed.token, 1]]);
  expect(b.calls).toEqual(["play a profile", `confirm a ${first.token}`, `confirm a ${changed.token}`]);

  // Cancel on the changed set: the newer token goes, nothing more is confirmed
  const one = pf([skipped("needy")]), late = pf([skipped("late")]);
  const c = backend(one, [() => { throw pending(late); }]);
  const cancel = asker([true, false]);
  expect(isCanceledStart(await launchWithPreflight("a", REQ, cancel.ask, c.io).catch((x) => x))).toBe(true);
  expect(cancel.seen).toEqual([[one.token, 0], [late.token, 1]]);
  expect(c.calls).toEqual(["play a profile", `confirm a ${one.token}`, `cancel a ${late.token}`]);
});

test("a changed set without warnings (never sent by the backend) is still shown, not confirmed", async () => {
  const first = pf([skipped("needy")]), empty = pf([]);
  const b = backend(first, [() => { throw pending(empty); }]);
  const a = asker([true, false]);
  expect(isCanceledStart(await launchWithPreflight("a", REQ, a.ask, b.io).catch((x) => x))).toBe(true);
  expect(a.seen.map((s) => s[1])).toEqual([0, 1]);
});

test("a refused or failed confirm is passed on as it came (its token dropped); the next Play prepares again", async () => {
  const p = pf([skipped("needy")]);
  const refused = { code: "refused", message: "press Play again", detail: { token: "expired" } };
  const b = backend(p, [() => { throw refused; }]);
  const e = await launchWithPreflight("a", REQ, asker([true]).ask, b.io).catch((x) => x);
  expect(e).toBe(refused);
  expect(isCanceledStart(e)).toBe(false);
  expect(b.calls).toEqual(["play a profile", `confirm a ${p.token}`, `cancel a ${p.token}`]);
});

test("a preflight of another game is refused: nothing confirmed, its token dropped for that game", async () => {
  const other = pf([skipped("needy")], "b");
  const b = backend(other);
  const a = asker([true]);
  const e = await launchWithPreflight("a", REQ, a.ask, b.io).catch((x) => x);
  expect(e.code).toBe("refused");
  expect(a.seen).toEqual([]);
  expect(b.calls).toEqual(["play a profile", `cancel b ${other.token}`]);
});

test("a dialog that throws counts as Cancel", async () => {
  const p = pf([skipped("needy")]);
  const b = backend(p);
  const e = await launchWithPreflight("a", REQ, async () => { throw new Error("gone"); }, b.io).catch((x) => x);
  expect(isCanceledStart(e)).toBe(true);
  expect(b.calls).toEqual(["play a profile", `cancel a ${p.token}`]);
});

test("with no dialog host the warnings are never confirmed; a host going away cancels what waits", async () => {
  const p = pf([skipped("needy")]);
  expect(await askStartWarnings(p)).toBe(false);
  const b = backend(p);
  expect(isCanceledStart(await launchWithPreflight("a", REQ, askStartWarnings, b.io).catch((x) => x))).toBe(true);
  expect(b.calls).toEqual(["play a profile", `cancel a ${p.token}`]);

  const leave = mountStartWarningHost();
  const answer = askStartWarnings(p, 0);
  expect(useStartWarnings.getState().asks.map((a) => a.preflight.token)).toEqual([p.token]);
  leave();
  expect(await answer).toBe(false);
  expect(useStartWarnings.getState().asks).toEqual([]);
});

test("one dialog at a time, oldest first; each answered on its own", async () => {
  const leave = mountStartWarningHost();
  const a = pf([skipped("x")], "a"), b = pf([skipped("y")], "b");
  const ra = askStartWarnings(a), rb = askStartWarnings(b);
  expect(useStartWarnings.getState().asks[0].preflight.gameId).toBe("a");
  useStartWarnings.getState().asks[0].answer(true);
  expect(useStartWarnings.getState().asks[0].preflight.gameId).toBe("b");
  useStartWarnings.getState().asks[0].answer(false);
  expect([await ra, await rb]).toEqual([true, false]);
  leave();
});

test("preflightOf reads only a consent_required that carries a preflight", () => {
  const p = pf([skipped("x")]);
  expect(preflightOf(pending(p))?.token).toBe(p.token);
  expect(preflightOf({ code: "refused", detail: { preflight: p } })).toBe(null);
  expect(preflightOf({ code: "consent_required", message: "x" })).toBe(null);
  expect(preflightOf({ code: "consent_required", detail: { preflight: { token: 1 } } })).toBe(null);
  expect(preflightOf(null)).toBe(null);
});
