import { expect, test } from "bun:test";
import { renderToStaticMarkup } from "react-dom/server";
import { MemoryRouter } from "react-router-dom";
import { EngineRow } from "../src/components/EngineStatus";
import { SfSelfHostedNotice } from "../src/components/SfSelfHostedNotice";
import type { EngineStatus, FoundReport, Profile } from "../src/api/commands";

const ownLoader = { kind: "shroudforge", engine: "shroudforge", file: "dbghelp.dll", via: "proxy", folder: "mods", delivery: "placed", enabled: 1, copied: [], notCopied: [], text: "Installed" } as const;
test("Engines shows detection before a start and labels removed-loader evidence as historical", () => {
  const row = { kind: "shroudforge", plugin: "kiln-shroudforge", file: "", state: null, ownLoader } as unknown as EngineStatus;
  expect(renderToStaticMarkup(<EngineRow s={row} />)).toContain("runs itself: dbghelp.dll");
  const removed = renderToStaticMarkup(<EngineRow s={{ ...row, ownLoader: null, state: { mode: "self-hosted", loaderFile: "dbghelp.dll" } }} />);
  expect(removed).toContain("Last start: self-hosted (loader no longer detected)");
  expect(removed).not.toContain("runs itself:");
});
test("notice exposes the existing takeover destination only for its supported proxy", () => {
  const profile = { mods: { shroudforge: [{ enabled: false }] } } as unknown as Profile;
  const found = { loaders: [{ engine: "shroudforge", file: "dinput8.dll" }] } as FoundReport;
  const render = (p: Profile | null, f = found) => renderToStaticMarkup(<MemoryRouter><SfSelfHostedNotice profile={p} found={f} /></MemoryRouter>);
  expect(render(profile)).toContain('href="/runtime"');
  expect(render(profile)).toContain('role="status"');
  expect(render(null)).toBe("");
  expect(render(profile, { ...found, loaders: [{ ...found.loaders[0], file: "winmm.dll" }] })).not.toContain("href=");
});
