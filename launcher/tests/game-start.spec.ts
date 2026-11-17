// Custom game folders (dev.29.17, docs/kiln/launcher/custom-game-folders.md) in browser-mock mode: a folder no Steam
// library knows is added, marked custom, offered the start dialog with "Start the game directly" as its default, says
// that Steam must run, and says plainly when Kiln does not know its game build (Play and Settings). Steam's own copy
// keeps "Through Steam" and says nothing about its known build. `?mock=windows-start` shows the dialog as on Windows.
import { expect, test, type Page } from "@playwright/test";

async function start(page: Page, mock: string | null = null, route = "/") {
  await page.goto(mock ? `/?mock=${mock}#${route}` : `/#${route}`);
  await page.locator(".k-splash").waitFor({ state: "detached" });
}

async function addFolder(page: Page, path: string) {
  await page.locator(".k-foot").getByRole("button", { name: "Add", exact: true }).click();
  const add = page.getByRole("dialog", { name: "Add a game folder" });
  // the path box takes its value on Enter or blur (ui PathPicker)
  await add.getByRole("textbox").fill(path);
  await add.getByRole("textbox").press("Enter");
  await add.getByRole("button", { name: "Add", exact: true }).click();
}

test("Steam's copy starts through Steam and its known build says nothing", async ({ page }) => {
  await start(page);
  const link = page.getByTestId("start-mode");
  await expect(link).toHaveText("Starts through Steam");
  await expect(page.getByTestId("unknown-build")).toHaveCount(0);
  await link.click();
  const dialog = page.getByRole("dialog", { name: "How Kiln starts this game" });
  await expect(dialog.getByRole("radio", { name: "Through Steam" })).toHaveAttribute("aria-checked", "true");
  await expect(dialog.getByTestId("steam-required")).toHaveCount(0);
  await dialog.getByRole("button", { name: "Cancel" }).click();
});

test("a folder no Steam library knows is custom, starts directly by default and says its build is unknown", async ({ page }) => {
  await start(page);
  await addFolder(page, "/games/Enshrouded-old");
  const dialog = page.getByRole("dialog", { name: "A custom game folder" });
  await expect(dialog).toBeVisible();
  await expect(dialog.getByText("No Steam library knows this folder.", { exact: false })).toBeVisible();
  await expect(dialog.getByRole("radio", { name: "Start the game directly" })).toHaveAttribute("aria-checked", "true");
  await expect(dialog.getByTestId("steam-required")).toHaveText(/Steam must be running/);
  // Linux: Proton by default (Steam's mapping, else the newest), Kiln's own prefix, the command shown as it runs
  await expect(dialog.getByLabel("Proton or Wine")).toHaveValue("");
  await expect(dialog.getByRole("option", { name: "Automatic (GE-Proton10-25)" })).toHaveCount(1);
  await expect(dialog.getByTestId("start-command")).toContainText("WINEDLLOVERRIDES=dinput8=n,b");
  await expect(dialog.getByTestId("start-command")).toContainText("proton run /games/Enshrouded-old/enshrouded.exe");
  await expect(dialog.getByTestId("start-command")).not.toContainText("SteamAppId");
  await dialog.getByRole("button", { name: "Keep the default" }).click();
  await expect(dialog).toBeHidden();

  // Play: the new folder is selected, starts directly, and its build is one Kiln does not know
  await expect(page.getByTestId("start-mode")).toHaveText("Starts directly");
  const unknown = page.getByTestId("unknown-build");
  await expect(unknown).toHaveText(/^Kiln doesn't know this game build \([0-9a-f]{12}\)\. Kiln starts with the game but stays idle: no mods load\.$/);
  await expect(page.locator("select.k-kicker-select")).toContainText("custom folder");

  // another Proton or Wine and a prefix of the player's, kept with the install
  await page.getByTestId("start-mode").click();
  const again = page.getByRole("dialog", { name: "How Kiln starts this game" });
  await again.getByLabel("Proton or Wine").selectOption({ label: "Wine (/usr/bin/wine) · Wine" });
  await again.getByRole("radio", { name: "Pick a folder" }).click();
  await again.locator("#start-prefix").fill("/games/prefixes/old");
  await again.locator("#start-prefix").press("Enter");
  await again.getByRole("button", { name: "Save" }).click();
  await expect(again).toBeHidden();
  await page.getByTestId("start-mode").click();
  const third = page.getByRole("dialog", { name: "How Kiln starts this game" });
  await expect(third.getByLabel("Proton or Wine")).toHaveValue("/usr/bin/wine");
  await expect(third.locator("#start-prefix")).toHaveValue("/games/prefixes/old");
  await expect(third.getByTestId("start-command")).toContainText("WINEPREFIX=/games/prefixes/old");
  // through Steam after all: Steam starts its own copy, said before it is saved
  await third.getByRole("radio", { name: "Through Steam" }).click();
  await expect(third.getByText("not this folder", { exact: false })).toBeVisible();
  await third.getByRole("button", { name: "Save" }).click();
  await expect(page.getByTestId("start-mode")).toHaveText("Starts through Steam");
});

test("Settings' game list marks a custom folder and its unknown build", async ({ page }) => {
  await start(page, null, "/settings");
  const folder = page.getByPlaceholder("…/common/Enshrouded");
  await folder.scrollIntoViewIfNeeded();
  await folder.fill("/games/Enshrouded 2025");
  await folder.press("Enter");
  // the Add next to the path box (the outer input group of Settings' "Add a folder")
  await folder.locator("xpath=ancestor::div[contains(@class,'k-input-group')][2]").getByRole("button", { name: "Add", exact: true }).click();
  const dialog = page.getByRole("dialog", { name: "A custom game folder" });
  await expect(dialog).toBeVisible();
  await dialog.getByRole("button", { name: "Keep the default" }).click();
  const row = page.locator("[data-testid^='game-row-client-']").filter({ hasText: "Enshrouded 2025" });
  await expect(row.getByText("custom folder")).toBeVisible();
  await expect(row.getByRole("button", { name: "Starts directly" })).toBeVisible();
  await expect(row.getByTestId("unknown-build")).toContainText("Kiln doesn't know this game build");
  // Steam's copy: neither
  const steam = page.getByTestId("game-row-client-steam");
  await expect(steam.getByRole("button", { name: "Starts through Steam" })).toBeVisible();
  await expect(steam.getByTestId("unknown-build")).toHaveCount(0);
});

test("a known build in a custom folder says nothing extra", async ({ page }) => {
  await start(page, "known-custom");
  await addFolder(page, "/games/Enshrouded-kept");
  await page.getByRole("dialog", { name: "A custom game folder" }).getByRole("button", { name: "Keep the default" }).click();
  await expect(page.getByTestId("start-mode")).toHaveText("Starts directly");
  await expect(page.getByTestId("unknown-build")).toHaveCount(0);
});

test("on Windows a direct start runs the exe in the game folder: nothing to pick", async ({ page }) => {
  await start(page, "windows-start");
  await addFolder(page, "/games/Enshrouded-old");
  const dialog = page.getByRole("dialog", { name: "A custom game folder" });
  await expect(dialog.getByRole("radio", { name: "Start the game directly" })).toHaveAttribute("aria-checked", "true");
  await expect(dialog.getByLabel("Proton or Wine")).toHaveCount(0);
  await expect(dialog.getByText("Prefix", { exact: true })).toHaveCount(0);
  await expect(dialog.getByTestId("start-command")).toContainText("enshrouded.exe");
  await expect(dialog.getByTestId("steam-required")).toBeVisible();
});

test("a direct start with Steam closed says so", async ({ page }) => {
  await start(page, "no-steam");
  await addFolder(page, "/games/Enshrouded-old");
  await page.getByRole("dialog", { name: "A custom game folder" }).getByRole("button", { name: "Keep the default" }).click();
  await expect(page.getByText("Steam isn't running")).toBeVisible();
});
