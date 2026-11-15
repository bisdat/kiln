import { expect, test } from "bun:test";
import { sfSelfHostedFile, sfSelfHostedText } from "../src/shell/sfSelfHosted";
test("self hosting notice names the file and explains profile switches", () => {
  const text = sfSelfHostedText("dbghelp.dll");
  expect(text).toContain("own loader is installed (dbghelp.dll).");
  expect(text).toContain("If it loads");
  expect(text).toContain("Profile switches still update copies Kiln manages.");
  expect(text).toContain("Folders you installed yourself keep their own switches.");
  expect(text).not.toContain("switches do not apply");
  expect(text).not.toMatch(/[—–;]/);
  expect(sfSelfHostedText("winmm.dll")).toContain("(winmm.dll)");
});

import type { Profile, FoundReport } from "../src/api/commands";
test("only the loaded profile with SF entries and current disk detection gets a notice", () => {
  const profile = { mods: { shroudforge: [{ enabled: false }] } } as unknown as Profile;
  const disk = { loaders: [{ engine: "shroudforge", file: "dbghelp.dll" }] } as FoundReport;
  expect(sfSelfHostedFile(profile, disk)).toBe("dbghelp.dll");
  expect(sfSelfHostedFile(null, disk)).toBeNull();
  expect(sfSelfHostedFile({ mods: {} } as Profile, disk)).toBeNull();
  expect(sfSelfHostedFile(profile, { loaders: [] } as unknown as FoundReport)).toBeNull();
  expect(sfSelfHostedFile(profile, undefined)).toBeNull();
});

import { readFileSync } from "node:fs";
test("observed native self hosting describes the adapter boundary and keeps managed switches", () => {
  const native = readFileSync(new URL("../../native/plugins/shroudforge/src/plugin.cpp", import.meta.url), "utf8");
  expect(native).not.toContain("Kiln's profile does not decide ShroudForge mods this start");
  expect(native.match(/Managed package switches still apply\./g)).toHaveLength(2);
});
