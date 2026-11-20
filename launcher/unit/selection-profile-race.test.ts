import { expect, test } from "bun:test";
import { QueryClient } from "@tanstack/react-query";
import { qk, showActive } from "../src/api/hooks";
import * as api from "../src/api/commands";
import { NONE, ownerOf, selectionOf, setSelection, toggle, dropSelection } from "../src/shell/selection";

test("a new profile's selection survives the late activation response and subsequent refetch", async () => {
  const qc = new QueryClient({ defaultOptions: { queries: { gcTime: 0 } } });
  const game = "client-steam";
  const a = await api.profileGet("ember-run");
  const b = await api.profileGet("vanilla-plus");
  const oa = ownerOf(a.id, a), ob = ownerOf(b.id, b);
  qc.setQueryData(qk.active(game), { profileId: a.id });
  setSelection(game, oa, toggle(NONE, "kiln/big-stacks"));
  try {
    await showActive(qc, game, b.id); // the UI publishes B before activation answers
    expect(selectionOf(game, oa).keys).toEqual([]);
    const pending = api.profileActivate(b.id, game);
    const data = await api.profileGet(b.id); // B's rows can be read while activation runs
    expect(ownerOf(b.id, data)).toBe(ob);
    expect(selectionOf(game, ob).keys).toEqual([]);
    setSelection(game, ob, toggle(NONE, "kiln/weather-plus"));
    const result = await pending; // today's unconditional profileSwitched drops the new selection
    qc.setQueryData(qk.active(game), result);
    expect(selectionOf(game, ob).keys).toEqual(["kiln/weather-plus"]);
    await showActive(qc, game, b.id); // same identity, including Apply/refetch
    await api.profileGet(b.id);
    expect(selectionOf(game, ob).keys).toEqual(["kiln/weather-plus"]);
    await showActive(qc, game, a.id);
    expect(selectionOf(game, ob).keys).toEqual([]);
  } finally { dropSelection(game); qc.clear(); }
});

test("identity rollback and No profile clear once, while repeated identity notifications keep new state", async () => {
  const qc = new QueryClient({ defaultOptions: { queries: { gcTime: 0 } } });
  const { profileSwitched } = await import("../src/api/oneActivation");
  const game = "selection-rollback";
  const oa = ownerOf("a", { id: "a", created: "first" });
  const ob = ownerOf("b", { id: "b", created: "first" });
  qc.setQueryData(qk.active(game), { profileId: "a" });
  try {
    setSelection(game, oa, toggle(NONE, "kiln/a"));
    const undo = await showActive(qc, game, "b");
    expect(selectionOf(game, oa).keys).toEqual([]);
    setSelection(game, ob, toggle(NONE, "kiln/b"));
    profileSwitched(game, "b");
    profileSwitched(game, "b");
    expect(selectionOf(game, ob).keys).toEqual(["kiln/b"]);
    undo();
    expect(selectionOf(game, ob).keys).toEqual([]);
    setSelection(game, oa, toggle(NONE, "kiln/a"));
    await showActive(qc, game, "a");
    profileSwitched(game, "a");
    expect(selectionOf(game, oa).keys).toEqual(["kiln/a"]);
    await showActive(qc, game, null);
    expect(selectionOf(game, oa).keys).toEqual([]);
    setSelection(game, "none", toggle(NONE, "kiln/library-only"));
    profileSwitched(game, null);
    expect(selectionOf(game, "none").keys).toEqual(["kiln/library-only"]);
  } finally { dropSelection(game); qc.clear(); }
});
