// Inbox 85 (29.18): Runtime tells a receipt file that is gone (an antivirus took it) from one that is there with other
// bytes (src/shell/runtimeFiles.ts).
import { expect, test } from "bun:test";
import { fewPaths, fileProblems, gamePath } from "../src/shell/runtimeFiles";

test("missing files come out of the changed ones; a DLL among them is said", () => {
  const p = fileProblems({ modified: ["kiln/kiln-core.dll", "kiln/plugins/kiln-greeneye.dll"], missingFiles: ["kiln/plugins/kiln-greeneye.dll"] });
  expect(p).toEqual({ missing: ["kiln/plugins/kiln-greeneye.dll"], changed: ["kiln/kiln-core.dll"], dll: true });
  expect(fileProblems({ modified: ["kiln/sigdb/client.json"], missingFiles: ["kiln/sigdb/client.json"] }).dll).toBe(false);
  expect(fileProblems({ modified: ["DINPUT8.DLL"], missingFiles: ["DINPUT8.DLL"] }).dll).toBe(true);
});

test("an older backend without missingFiles: everything stays changed, as before", () => {
  expect(fileProblems({ modified: ["kiln/plugins/kiln-greeneye.dll"] })).toEqual({ missing: [], changed: ["kiln/plugins/kiln-greeneye.dll"], dll: false });
  expect(fileProblems(null)).toEqual({ missing: [], changed: [], dll: false });
});

test("paths are said the way the player's system writes them, three at most", () => {
  expect(gamePath("kiln/plugins/kiln-greeneye.dll", "windows")).toBe("kiln\\plugins\\kiln-greeneye.dll");
  expect(gamePath("kiln/plugins/kiln-greeneye.dll", "proton")).toBe("kiln/plugins/kiln-greeneye.dll");
  expect(fewPaths(["a/1.dll", "a/2.dll", "a/3.dll", "a/4.dll"], "windows")).toBe("a\\1.dll, a\\2.dll, a\\3.dll …");
  expect(fewPaths(["a/1.dll"], "wine")).toBe("a/1.dll");
});
