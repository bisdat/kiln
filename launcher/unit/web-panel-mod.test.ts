// "Open web panel" shows for the web panel mod under either id (0.2.0-dev.31.14): Kilnwarden (`kilnwarden`) and its
// old name Kiln Web Admin (`kiln-webadmin`, the 0.1.0 test builds), Kilnwarden first (src/pages/servers/webPanelMod.ts).
import { expect, test } from "bun:test";
import type { ServerMod } from "../src/api/commands";
import { WEB_PANEL_MOD_IDS, webPanelMod } from "../src/pages/servers/webPanelMod";

const mod = (id: string, enabled = true): ServerMod => ({ id, version: "0.1.0", folder: id, enabled, source: null, clients: "none", state: enabled ? "loaded" : null, kind: "kiln" });
const others = [mod("kiln-admin"), mod("isle-icons")];

test("both ids, Kilnwarden first", () => {
  expect([...WEB_PANEL_MOD_IDS]).toEqual(["kilnwarden", "kiln-webadmin"]);
});

test("Kilnwarden on the server: the panel is Kilnwarden's", () => {
  expect(webPanelMod([...others, mod("kilnwarden")])).toBe("kilnwarden");
});

test("a Kiln Web Admin test build still has its button", () => {
  expect(webPanelMod([...others, mod("kiln-webadmin")])).toBe("kiln-webadmin");
});

test("both installed: Kilnwarden, whichever comes first in the list", () => {
  expect(webPanelMod([mod("kiln-webadmin"), ...others, mod("kilnwarden")])).toBe("kilnwarden");
  expect(webPanelMod([mod("kilnwarden"), mod("kiln-webadmin")])).toBe("kilnwarden");
});

test("only an enabled copy counts", () => {
  expect(webPanelMod([mod("kilnwarden", false), mod("kiln-webadmin")])).toBe("kiln-webadmin");
  expect(webPanelMod([mod("kilnwarden"), mod("kiln-webadmin", false)])).toBe("kilnwarden");
  expect(webPanelMod([mod("kilnwarden", false), mod("kiln-webadmin", false)])).toBeNull();
});

test("no web panel mod, or no list yet: no button", () => {
  expect(webPanelMod(others)).toBeNull();
  expect(webPanelMod([])).toBeNull();
  expect(webPanelMod(undefined)).toBeNull();
  // ids are exact: a look-alike is another mod
  expect(webPanelMod([mod("Kilnwarden"), mod("kilnwarden-extras"), mod("kiln-webadmin-old")])).toBeNull();
});
