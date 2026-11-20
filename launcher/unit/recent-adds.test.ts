// Inbox 42 (HadToRegister): an add's toast went after 8 s with the reasons in it, and nothing brought them back.
import { beforeEach, expect, test } from "bun:test";

const store = new Map<string, string>();
(globalThis as { localStorage?: unknown }).localStorage ??= {
  getItem: (k: string) => store.get(k) ?? null,
  setItem: (k: string, v: string) => void store.set(k, v),
  removeItem: (k: string) => void store.delete(k),
};

const { detailsAction, rememberAdd, staysIfDetails, useRecentAdds } = await import("../src/shell/importResult");

const left = { heading: "Nothing was added", summary: { taken: [], left: [{ file: "dinput8.dll", why: "Not needed." }], notes: [] } };
const clean = { heading: "Added and switched on", summary: { taken: ["Night Owl"], left: [], notes: [] } };

beforeEach(() => {
  useRecentAdds.setState({ adds: [] });
  localStorage.removeItem("kiln.recentAdds");
});

test("every add's toast is kept for Last adds, newest first, at most ten, across restarts", () => {
  expect(detailsAction(left)).toHaveLength(1);
  expect(detailsAction(clean)).toHaveLength(0); // nothing to read: no Details button, still kept
  const adds = useRecentAdds.getState().adds;
  expect(adds.map((a) => a.heading)).toEqual(["Added and switched on", "Nothing was added"]);
  expect(typeof adds[0].when).toBe("string");
  for (let i = 0; i < 12; i++) rememberAdd({ ...clean, heading: `add ${i}` });
  expect(useRecentAdds.getState().adds).toHaveLength(10);
  expect(useRecentAdds.getState().adds[0].heading).toBe("add 11");
  const saved = JSON.parse(localStorage.getItem("kiln.recentAdds") ?? "[]") as { heading: string }[];
  expect(saved.map((a) => a.heading)).toEqual(useRecentAdds.getState().adds.map((a) => a.heading));
});

test("a toast with reasons stays until it is closed; one without goes as before", () => {
  expect(staysIfDetails(left)).toEqual({ ms: 0 });
  expect(staysIfDetails({ ...clean, summary: { ...clean.summary, notes: ["dinput8.dll is a loader: not needed."] } })).toEqual({ ms: 0 });
  expect(staysIfDetails(clean)).toEqual({});
});
