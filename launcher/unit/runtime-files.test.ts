// Inbox 85 (29.18): Runtime tells a receipt file that is gone (an antivirus took it) from one that is there with other
// bytes (src/shell/runtimeFiles.ts).
import { expect, test } from "bun:test";
import { blockedFile, fewPaths, fileProblems, gamePath } from "../src/shell/runtimeFiles";

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

// Inbox 92 (29.18): an install that Windows refused a DLL of (an antivirus) has the file in its error's detail.blocked
// (kiln_manage::blocked); every other error, and an older backend's, has none.
test("a file Windows refused is read from the error's detail; anything else is no such file", () => {
  expect(blockedFile({ detail: { blocked: { path: "kiln/plugins/kiln-greeneye.dll", osError: 5 } } })).toEqual({ path: "kiln/plugins/kiln-greeneye.dll", osError: 5 });
  expect(blockedFile({ detail: { blocked: { path: "dinput8.dll", osError: 32 }, other: 1 } })?.osError).toBe(32);
  for (const detail of [undefined, null, "blocked", ["blocked"], { pid: 4 }, { blocked: null }, { blocked: "kiln-greeneye.dll" },
    { blocked: { path: "", osError: 5 } }, { blocked: { path: "kiln/x.dll" } }, { blocked: { path: "kiln/x.dll", osError: "5" } }]) {
    expect(blockedFile({ detail })).toBeNull();
  }
  expect(blockedFile(null)).toBeNull();
  expect(blockedFile(undefined)).toBeNull();
});
