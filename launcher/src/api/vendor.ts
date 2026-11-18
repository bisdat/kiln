// Vendor upscaler files (AMD FSR, Intel XeSS): types and wrappers of the vendor_* commands
// (launcher/src-tauri/src/commands/vendor.rs, kiln_manage::vendor; docs/kiln/launcher/contracts.md "Vendor files").
// Settings, Upscalers (components/UpscalerSettings.tsx) is their only user, behind the preview "upscalers"
// (app_info.preview). The browser mock answers from ./mockVendor.ts.
import { invoke as tauriInvoke } from "@tauri-apps/api/core";
import { isTauri, loadMock, toKilnError, type AppInfo, type GameId, type TaskId } from "./commands";

async function call<T>(cmd: string, args: Record<string, unknown> = {}): Promise<T> {
  try {
    return isTauri() ? await tauriInvoke<T>(cmd, args) : await (await loadMock()).mockInvoke<T>(cmd, args);
  } catch (e) {
    throw toKilnError(e);
  }
}

/** The preview of the Upscalers section (settings.json `"preview": ["upscalers"]`, or KILN_LAUNCHER_PREVIEW=upscalers). */
export const UPSCALERS_PREVIEW = "upscalers";
export const upscalersPreview = (info: AppInfo | null | undefined) => !!info?.preview?.includes(UPSCALERS_PREVIEW);

/** kiln_manage::vendor::VendorState */
export type VendorState = "notDownloaded" | "downloaded" | "updateAvailable" | "unavailable" | "damaged";

export interface VendorLicenseInfo {
  /** The vendor's own name for it ("MIT licence", "Intel Simplified Software License (October 2022)"). */
  name: string;
  sha256: string;
  /** Where the text is in the vendor's repository. */
  from: string;
  /** The player agreed to this text for this vendor (<data>/state/vendor-licenses.json). */
  agreed: boolean;
  agreedAt?: string | null;
}

export interface VendorInstalled {
  version: string;
  /** Relative to the game folder: kiln/vendor/amd/2.4.0 */
  folder: string;
  sizeOnDisk: number;
  installed: string;
  /** Files missing, not plain files, or of another size or SHA-256 than the receipt's. */
  problems?: string[];
}

export interface VendorComponent {
  id: string;
  vendor: "amd" | "intel" | "nvidia" | string;
  vendorName: string;
  name: string;
  /** What it enables, one sentence. */
  enables: string;
  /** The version a download gets. */
  version: string;
  /** The vendor's release page (opened in the browser, never fetched). */
  source?: string | null;
  license: VendorLicenseInfo;
  /** Where the files come from (raw.githubusercontent.com). */
  hosts: string[];
  /** Bytes a download fetches. */
  size: number;
  files: { name: string; size: number; pinned: boolean }[];
  state: VendorState;
  installed?: VendorInstalled | null;
  /** Bytes of an interrupted download, kept to go on from. */
  partial: number;
  /** For "unavailable" and "damaged". */
  reason?: string | null;
}

export interface VendorStatus {
  /** The preview is on (downloads allowed). */
  preview: boolean;
  /** kiln/vendor */
  folder: string;
  components: VendorComponent[];
}

export interface VendorLicenseText {
  component: string;
  vendor: string;
  name: string;
  /** The vendor's words, verbatim. */
  text: string;
  sha256: string;
  from: string;
  agreed: boolean;
}

/** What a download task's result is (kiln_manage::vendor::DownloadReport). */
export interface VendorDownloadReport {
  folder: string;
  fetched: number;
  kept: string[];
  replaced: string[];
  warnings: string[];
}

export interface VendorRemoveReport {
  removed: string[];
  freed: number;
  /** What stayed, one sentence each. */
  kept: string[];
}

export const vendorStatus = (gameId: GameId) => call<VendorStatus>("vendor_status", { gameId });
export const vendorLicense = (componentId: string) => call<VendorLicenseText>("vendor_license", { componentId });
/** `licenseSha256`: the hash of the licence text the dialog showed and the player agreed to (first download of that
 *  text for its vendor), else null. */
export const vendorDownload = (gameId: GameId, componentId: string, licenseSha256: string | null) =>
  call<TaskId>("vendor_download", { gameId, componentId, licenseSha256 });
export const vendorRemove = (gameId: GameId, componentId: string) => call<VendorRemoveReport>("vendor_remove", { gameId, componentId });

export const vk = {
  status: (gameId: GameId) => ["vendor_status", gameId] as const,
  license: (componentId: string) => ["vendor_license", componentId] as const,
};
