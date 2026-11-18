import { expect, test } from "bun:test";
import type { Task } from "../src/api/commands";
import { upscalersPreview, type VendorComponent } from "../src/api/vendor";
import { upscalerRow } from "../src/components/UpscalerSettings";
import manifest from "../src/data/vendor-components.json";

const amd = (patch: Partial<VendorComponent> = {}): VendorComponent => ({
  id: "amd-fsr", vendor: "amd", vendorName: "AMD", name: "AMD FSR", enables: "AMD's FSR upscalers.", version: "2.4.0", source: null,
  license: { name: "MIT licence", sha256: "ab".repeat(32), from: "https://github.com/x", agreed: false },
  hosts: ["raw.githubusercontent.com"], size: 72_981_808, files: [{ name: "a.dll", size: 72_981_808, pinned: true }],
  state: "notDownloaded", installed: null, partial: 0, reason: null, ...patch,
});
const installed = { version: "2.4.0", folder: "kiln/vendor/amd/2.4.0", sizeOnDisk: 72_981_808, installed: "2026-11-18T12:00:00Z", problems: [] };
const task = (patch: Partial<Task>): Task => ({ id: "t1", kind: "vendor_download", title: "Download AMD FSR", state: "running", progress: 0.4,
  message: "Downloading a.dll: 29.2 MB of 73.0 MB", started: "2026-11-18T12:00:00Z", finished: null, ...patch });

test("not downloaded: the size and the host, one Download", () => {
  const r = upscalerRow(amd());
  expect(r.state).toBe("notDownloaded");
  expect(r.detail).toBe("70 MB from raw.githubusercontent.com.");
  expect(r.actions).toEqual(["download"]);
  // an interrupted download goes on
  const p = upscalerRow(amd({ partial: 10_000_000 }));
  expect(p.actions).toEqual(["continue"]);
  expect(p.detail).toContain("9.5 MB of it arrived already");
});

test("downloading: the task's progress and message, Cancel only", () => {
  const r = upscalerRow(amd(), task({}));
  expect(r).toEqual({ state: "downloading", detail: "Downloading a.dll: 29.2 MB of 73.0 MB", progress: 0.4, actions: ["cancel"] });
  expect(upscalerRow(amd(), task({ state: "queued", progress: null, message: null })).progress).toBeNull();
});

test("downloaded: the version and the size on disk, Remove", () => {
  const r = upscalerRow(amd({ state: "downloaded", installed }), task({ state: "done", progress: 1 }));
  expect(r.state).toBe("downloaded");
  expect(r.detail).toBe("Version 2.4.0, 70 MB on disk.");
  expect(r.actions).toEqual(["remove"]);
});

test("update available: what is there and what is out, Update and Remove", () => {
  const r = upscalerRow(amd({ state: "updateAvailable", installed: { ...installed, version: "2.3.0", folder: "kiln/vendor/amd/2.3.0" } }));
  expect(r.state).toBe("updateAvailable");
  expect(r.detail).toBe("Version 2.3.0 is downloaded. Version 2.4.0 is out: 70 MB from raw.githubusercontent.com.");
  expect(r.actions).toEqual(["update", "remove"]);
});

test("not available in this build: the reason, no button, whatever a task did", () => {
  const reason = "This build of Kiln does not have the files' checksums yet, so it does not download them.";
  const r = upscalerRow(amd({ state: "unavailable", reason }), task({ state: "failed", error: { code: "refused", message: "x" } }));
  expect(r).toEqual({ state: "unavailable", detail: reason, actions: [] });
});

test("failed: the task's reason and Try again; Remove too when an earlier version is there", () => {
  const error = { code: "hash_mismatch" as const, message: "a.dll from AMD (raw.githubusercontent.com) is not the file Kiln expects." };
  const r = upscalerRow(amd(), task({ state: "failed", error }));
  expect(r.state).toBe("failed");
  expect(r.reason).toBe(error.message);
  expect(r.actions).toEqual(["tryAgain"]);
  const u = upscalerRow(amd({ state: "updateAvailable", installed: { ...installed, version: "2.3.0" } }), task({ state: "failed", error }));
  expect(u.actions).toEqual(["tryAgain", "remove"]);
  // a canceled download is no failure: the row is what the folder holds
  expect(upscalerRow(amd({ partial: 5 }), task({ state: "canceled" })).state).toBe("notDownloaded");
  // a component the folder holds complete is downloaded whatever an earlier task said
  expect(upscalerRow(amd({ state: "downloaded", installed }), task({ state: "failed", error })).state).toBe("downloaded");
});

test("missing or changed files read as failed with Download again", () => {
  const r = upscalerRow(amd({ state: "damaged", installed: { ...installed, problems: ["a.dll"] }, reason: "a.dll of it is missing or changed. Download it again." }));
  expect(r.state).toBe("failed");
  expect(r.reason).toBe("a.dll of it is missing or changed. Download it again.");
  expect(r.actions).toEqual(["downloadAgain", "remove"]);
});

test("the section is a preview: only app_info's 'upscalers' shows it", () => {
  expect(upscalersPreview(null)).toBe(false);
  expect(upscalersPreview({ preview: [] } as never)).toBe(false);
  expect(upscalersPreview({ preview: ["other"] } as never)).toBe(false);
  expect(upscalersPreview({ preview: ["upscalers"] } as never)).toBe(true);
});

test("the shipped manifest's components never say DLSS and pin every file", () => {
  for (const c of manifest.components) {
    expect(c.name).not.toMatch(/dlss/i);
    expect(c.enables).not.toMatch(/dlss/i);
    expect(c.files.every((f) => /^[0-9a-f]{64}$/.test(f.sha256))).toBe(true);
  }
  expect(manifest.components.map((c) => c.license.name)).toEqual(["MIT licence", "Intel Simplified Software License (October 2022)"]);
});
