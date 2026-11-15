import { expect, test } from "bun:test";
import { sfSelfHostedFile, sfSelfHostedText } from "../src/shell/sfSelfHosted";
test("self hosting notice names the file and explains profile switches", () => {
  expect(sfSelfHostedText("dbghelp.dll")).toBe("ShroudForge's own loader is installed in the game folder (dbghelp.dll), so ShroudForge runs its mods itself. Your profile's ShroudForge switches do not apply. Remove that file, or let Kiln manage ShroudForge, to have the profile decide.");
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
