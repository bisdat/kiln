// bun test unit/ (from launcher/): what a start says in words (shell/startProgress.ts) and the mock's scripted start
// (src/api/mockProgress.ts: the owner's start of 13 Nov, the port of kiln_manage::start_progress::estimate).
import { expect, test } from "bun:test";
import type { StartStep, StartTimeline } from "../src/api/commands";
import { aboutText, preStartLine, quietText, startWords, stepLine, tookText } from "../src/shell/startProgress";
import { clearStartHistory, endAt, estimate, mockBegin, mockProgress, ownerScript, setStartScript, type KeptStart, type StepNow } from "../src/api/mockProgress";
import shared from "../../crates/kiln-manage/tests/start-progress/estimate-cases.json";

test("the time left is rounded as a person says it", () => {
  expect(aboutText(3_900)).toBe("a few seconds");
  expect(aboutText(12_400)).toBe("about 12 s");
  expect(aboutText(38_000)).toBe("about 40 s");
  expect(aboutText(41_000)).toBe("about 40 s");
  expect(aboutText(83_000)).toBe("about 1 min 25 s");
  expect(aboutText(120_000)).toBe("about 2 min");
  expect(tookText(605)).toBe("0.6 s");
  expect(tookText(18_750)).toBe("19 s");
  expect(tookText(79_000)).toBe("1 min 19 s");
});

const step = (o: Partial<StartStep>): StartStep => ({ id: "services", label: "Starting Kiln's services", state: "running", launcher: false, ...o });

test("the running step's line names the step, what it works on and the count", () => {
  expect(stepLine(step({ detail: "kiln-vfx: building 85 materials", done: 14, total: 31 }))).toBe("Starting Kiln's services: kiln-vfx, building 85 materials (14 of 31)");
  expect(stepLine(step({ id: "cook", label: "Cooking Kiln Equipment (1 of 3)", done: 0, total: 3, detail: "kiln: cooking assets/cook.json (modelcook)" })))
    .toBe("Cooking Kiln Equipment (1 of 3): kiln, cooking assets/cook.json (modelcook)");
  // kiln-eml's own progress is the detail of the early plugins
  const prep = { message: "Preparing the game data for EML mods: 12.3 GiB of 48.6 GiB." } as never;
  expect(stepLine(step({ id: "early-plugins", label: "Starting ShroudForge" }), prep)).toBe("Preparing the game data for EML mods: 12.3 GiB of 48.6 GiB");
});

test("a kiln-mode start's startup pass is said by its step, in the launcher's words", () => {
  const prep = (o: object) => ({ stage: "startup", label: "", doneBytes: 0, totalBytes: 0, filesDone: 0, filesTotal: 0, quietMs: 0, message: "(kiln-manage's English)", ...o }) as never;
  expect(preStartLine(prep({ step: "verify", stepNumber: 3, steps: 5 }))).toBe("Preparing ShroudForge mods for Kiln's interpreter: checking the prepared game data (3 of 5).");
  expect(preStartLine(prep({ step: "checking", stepNumber: 1, steps: 5 }))).toBe("Preparing ShroudForge mods for Kiln's interpreter: checking the game files (1 of 5).");
  expect(preStartLine(prep({ step: "save" }))).toBe("Preparing ShroudForge mods for Kiln's interpreter: saving the result.");
  // a step this launcher does not know (a newer kiln-eml): the sentence without it
  expect(preStartLine(prep({ step: "later", stepNumber: 6, steps: 6 }))).toBe("Preparing ShroudForge mods for Kiln's interpreter.");
  // the EML stages keep kiln-manage's sentence
  expect(preStartLine(prep({ stage: "checking" }))).toBe("(kiln-manage's English)");
  // as the early plugins' detail
  expect(stepLine(step({ id: "early-plugins", label: "Starting ShroudForge" }), prep({ step: "write", stepNumber: 4, steps: 5 })))
    .toBe("Preparing ShroudForge mods for Kiln's interpreter: writing the game data (4 of 5)");
});

function play(at: number): StartTimeline {
  return mockProgress("g", Date.now() + 0 * at)!;
}

test("the scripted start: the first start is timed, the second counts down from its history", () => {
  const s = ownerScript({ scale: 1 });
  setStartScript(s);
  clearStartHistory("g");
  const real = Date.now;
  let clock = 1_000_000;
  Date.now = () => clock;
  try {
    mockBegin("g", "profile");
    const firsts: StartTimeline[] = [];
    for (let t = 0; t <= endAt(s); t += 1000) { clock = 1_000_000 + t; firsts.push(play(t)); }
    clock = 1_000_000 + endAt(s);
    expect(play(0).phase).toBe("ready");
    for (const tl of firsts.slice(0, -1)) expect(tl.estimate.leftMs).toBeNull();
    // the bar never moves back, also where the count of steps turns into time (the launcher's kept maximum)
    firsts.forEach((tl, i) => { if (i) expect(tl.estimate.fraction).toBeGreaterThanOrEqual(firsts[i - 1].estimate.fraction); });
    const w = startWords(firsts[30]);
    expect(w.left).toMatch(/^First start with these mods on this PC: Kiln is timing it\. Step 1 of 14\.$/);
    expect(w.line).toBe("Cooking BetterDummies (2 of 3): kiln, cooking assets/cook.json (modelcook)");
    // the second identical start: a number within 3 s of the truth at every second
    mockBegin("g", "profile");
    let worst = 0;
    for (let t = 0; t < endAt(s); t += 1000) {
      clock = 1_000_000 + endAt(s) + 1 + t;
      const tl = play(t);
      worst = Math.max(worst, Math.abs((tl.estimate.leftMs ?? 1e9) - (endAt(s) - t)));
    }
    expect(worst).toBeLessThanOrEqual(3_000);
  } finally {
    Date.now = real;
    setStartScript(null);
  }
});

test("a slow step says it takes longer than usual", () => {
  const tl: StartTimeline = {
    phase: "booting", started: "2026-11-13T17:26:52.000Z", atMs: 125_000, launcher: true,
    steps: [step({ state: "running", usualMs: 18_750 })], running: 0,
    estimate: { leftMs: 32_600, starts: 1, same: true, slow: { id: "services", usualMs: 18_750 }, fraction: 0.79, byTime: true, stepsDone: 7, stepsPlanned: 14, usual: [] },
  };
  const w = startWords(tl);
  expect(w.left).toBe("Starting Kiln's services: taking longer than usual (usually 19 s). Then about 35 s.");
  expect(w.basis).toBe("From your last start with these mods");
  expect(w.valueText).toBe("79 percent, longer than usual: Starting Kiln's services");
  expect(startWords({ ...tl, phase: "ready" }).pct).toBe(100);
});

test("the mock's estimate gives what kiln_manage's gives in every shared case (no drift)", () => {
  const all = shared as unknown as { histories: Record<string, KeptStart[]>; cases: { name: string; plan: string[]; history: string | null; kiln: string; fingerprint: string;
    now: number; ready: boolean; steps: (Partial<StepNow> & { id: string; ms: number })[]; expect: Record<string, unknown> }[] };
  expect(all.cases.length).toBeGreaterThanOrEqual(9);
  for (const c of all.cases) {
    const steps: StepNow[] = c.steps.map((s) => ({ tookMs: null, keys: [], parts: [], partMs: s.ms, ...s }));
    const e = estimate(c.plan, steps, c.now, c.history ? all.histories[c.history] : [], c.kiln, c.fingerprint, c.ready);
    expect({ name: c.name, leftMs: e.leftMs, slow: e.slow, starts: e.starts, same: e.same, stepsDone: e.stepsDone, usual: e.usual })
      .toEqual({ name: c.name, ...c.expect });
  }
});

test("a silent start names its step; at the last step it says the game is loading or at its menu", () => {
  const quiet = (s: StartStep): StartTimeline => ({
    phase: "quiet", started: "2026-11-14T18:09:00.000Z", atMs: 5_200_000, quietMs: 5_183_000, launcher: true, steps: [s], running: 0,
    estimate: { leftMs: null, starts: 0, same: false, slow: null, fraction: 0.9, byTime: false, stepsDone: 12, stepsPlanned: 15, usual: [] },
  });
  // a step that never ends is said with its name
  expect(quietText(quiet(step({ id: "content", label: "Applying the mods' content", done: 3, total: 198 }))))
    .toBe("Kiln has said nothing for 5183 s (last: Applying the mods' content). A step can be slow on a busy PC; if the game does not move, close it and save a support report on the Logs page.");
  // Kiln's part is over and the menu's mark has not come (a Nexus report, 2026-11-14): no "close it", and when it ends
  const game = quietText(quiet(step({ id: "game", label: "The game is loading" })));
  expect(game).toBe("Kiln has loaded the mods. The game is still loading, or it is at its menu and Kiln did not see the menu appear; this goes away once you load a world.");
  // the mods that load at the first world, as before
  expect(quietText(quiet(step({ id: "mods", label: "Waiting for the game to load the mods" })))).toMatch(/^The game is waiting at its menu/);
});
