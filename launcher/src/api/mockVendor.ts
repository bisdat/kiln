// Mock answers for the vendor_* commands (browser mock mode; src/api/mock.ts registers them), from the launcher's real
// manifest and licence texts (src/data/vendor-components.json, src/data/vendor-licenses/). Like this build, nothing is
// pinned at first, so both rows read "Not available in this build"; `?vendor=pinned` or `__kilnVendorMock.pin(true)`
// pins every file (fake hashes) so the download flow can be seen. The preview is `?preview=upscalers` (app_info).
// Tests and the console: `__kilnVendorMock.pin(on)`, `.failNext(message, code?)`, `.pace(ms)`, `.changeLicense(id)`,
// `.damage(id)`, `.install(id, version)`, `.calls()`, `.agreed()`.
import type { KilnError } from "./commands";
import type { VendorComponent, VendorDownloadReport, VendorLicenseText, VendorRemoveReport, VendorStatus } from "./vendor";
import manifestJson from "../data/vendor-components.json";
import amdText from "../data/vendor-licenses/amd-fidelityfx-sdk-v2.4.0-mit.txt?raw";
import intelText from "../data/vendor-licenses/intel-xess-v3.0.2-license.txt?raw";

type Handler = (args: Record<string, unknown>) => unknown;
type StartTask = (kind: string, title: string, steps: string[], finish: () => unknown, stepMs?: number) => string;

interface ManifestFile { name: string; url: string; size: number; sha256: string }
interface ManifestComponent {
  id: string; vendor: string; vendorName: string; name: string; enables: string; version: string; source?: string;
  license: { name: string; file: string; sha256: string; from: string }; files: ManifestFile[];
}
const TEXTS: Record<string, string> = {
  "vendor-licenses/amd-fidelityfx-sdk-v2.4.0-mit.txt": amdText,
  "vendor-licenses/intel-xess-v3.0.2-license.txt": intelText,
};

const params = typeof location !== "undefined" ? new URLSearchParams(location.search) : new URLSearchParams();
/** `?preview=upscalers` (comma-separated names): app_info.preview. */
export const MOCK_PREVIEW: string[] = (params.get("preview") ?? "").split(",").map((s) => s.trim()).filter(Boolean);

const human = (n: number) => (n >= 1048576 ? `${(n / 1048576).toFixed(1)} MB` : `${(n / 1024).toFixed(1)} KB`);

export function vendorMockHandlers(startTask: StartTask, gameKind: (gameId: string) => string): Record<string, Handler> {
  const components: ManifestComponent[] = JSON.parse(JSON.stringify((manifestJson as { components: ManifestComponent[] }).components));
  let pinned = params.get("vendor") === "pinned";
  let failNext: KilnError | null = null;
  let stepMs = 300;
  const agreed = new Set<string>();
  /** By `${gameId}/${componentId}`. */
  const installed = new Map<string, { version: string; at: string; damaged: string[] }>();
  const calls: { cmd: string; args: Record<string, unknown> }[] = [];
  const reject = (code: KilnError["code"], message: string): never => { throw { code, message } satisfies KilnError; };
  const comp = (id: unknown) => components.find((c) => c.id === id) ?? reject("not_found", `Kiln knows no vendor component ${String(id)}.`);
  const client = (gameId: unknown) => {
    if (gameKind(String(gameId)) !== "client") reject("unsupported", "A dedicated server draws no frames: upscalers are for the game client only.");
  };
  const isPinned = (c: ManifestComponent) => pinned || c.files.every((f) => /^[0-9a-f]{64}$/.test(f.sha256));
  const status = (gameId: string, c: ManifestComponent): VendorComponent => {
    const i = installed.get(`${gameId}/${c.id}`);
    const size = c.files.reduce((n, f) => n + f.size, 0);
    const base: VendorComponent = {
      id: c.id, vendor: c.vendor, vendorName: c.vendorName, name: c.name, enables: c.enables, version: c.version, source: c.source ?? null,
      license: { name: c.license.name, sha256: c.license.sha256, from: c.license.from, agreed: agreed.has(`${c.vendor}:${c.license.sha256}`) },
      hosts: [...new Set(c.files.map((f) => new URL(f.url).host))], size, files: c.files.map((f) => ({ name: f.name, size: f.size, pinned: isPinned(c) })),
      state: "notDownloaded", installed: null, partial: 0, reason: null,
    };
    if (i) {
      const missing = i.damaged;
      const inst = { version: i.version, folder: `kiln/vendor/${c.vendor}/${i.version}`, sizeOnDisk: c.files.filter((f) => !missing.includes(f.name)).reduce((n, f) => n + f.size, 0), installed: i.at, problems: missing };
      if (i.version === c.version) {
        return missing.length
          ? { ...base, installed: inst, state: "damaged", reason: `${missing.join(", ")} of it ${missing.length === 1 ? "is" : "are"} missing or changed. Download it again.` }
          : { ...base, installed: inst, state: "downloaded" };
      }
      return { ...base, installed: inst, state: isPinned(c) ? "updateAvailable" : "downloaded" };
    }
    if (!isPinned(c)) return { ...base, state: "unavailable", reason: "This build of Kiln does not have the files' checksums yet, so it does not download them." };
    return base;
  };

  const handlers: Record<string, Handler> = {
    vendor_status: (a): VendorStatus => {
      calls.push({ cmd: "vendor_status", args: a });
      client(a.gameId);
      return { preview: MOCK_PREVIEW.includes("upscalers"), folder: "kiln/vendor", components: components.map((c) => status(String(a.gameId), c)) };
    },
    vendor_license: (a): VendorLicenseText => {
      calls.push({ cmd: "vendor_license", args: a });
      const c = comp(a.componentId);
      return { component: c.id, vendor: c.vendor, name: c.license.name, text: TEXTS[c.license.file] ?? "", sha256: c.license.sha256, from: c.license.from,
        agreed: agreed.has(`${c.vendor}:${c.license.sha256}`) };
    },
    vendor_download: (a): string => {
      calls.push({ cmd: "vendor_download", args: a });
      const c = comp(a.componentId);
      if (!MOCK_PREVIEW.includes("upscalers")) reject("refused", "Upscaler downloads are a preview that is off in this launcher. Nothing was downloaded.");
      client(a.gameId);
      if (!isPinned(c)) reject("refused", `${c.name} is not available in this build of Kiln. Nothing was downloaded.`);
      const key = `${c.vendor}:${c.license.sha256}`;
      if (!agreed.has(key)) {
        if (!a.licenseSha256) reject("consent_required", `Read ${c.vendorName}'s ${c.license.name} and agree to it first.`);
        if (a.licenseSha256 !== c.license.sha256) reject("consent_required", `The ${c.license.name} text of ${c.name} is not the one the dialog showed. Read it again before you agree.`);
        agreed.add(key);
      }
      const total = c.files.reduce((n, f) => n + f.size, 0);
      const steps: string[] = [];
      for (const f of c.files) for (const part of [0.25, 0.5, 0.75, 1]) steps.push(`Downloading ${f.name}: ${human(f.size * part)} of ${human(f.size)}`);
      const fail = failNext;
      failNext = null;
      const gameId = String(a.gameId);
      return startTask("vendor_download", `Download ${c.name}`, steps, (): VendorDownloadReport => {
        if (fail) throw fail;
        const before = installed.get(`${gameId}/${c.id}`);
        installed.set(`${gameId}/${c.id}`, { version: c.version, at: new Date().toISOString(), damaged: [] });
        const replaced = before && before.version !== c.version ? [`kiln/vendor/${c.vendor}/${before.version}`] : [];
        return { folder: `kiln/vendor/${c.vendor}/${c.version}`, fetched: total, kept: [], replaced, warnings: [] };
      }, stepMs);
    },
    vendor_remove: (a): VendorRemoveReport => {
      calls.push({ cmd: "vendor_remove", args: a });
      const c = comp(a.componentId);
      client(a.gameId);
      const i = installed.get(`${a.gameId}/${c.id}`);
      installed.delete(`${a.gameId}/${c.id}`);
      return i ? { removed: [`kiln/vendor/${c.vendor}/${i.version}`], freed: c.files.reduce((n, f) => n + f.size, 0), kept: [] } : { removed: [], freed: 0, kept: [] };
    },
  };

  if (typeof window !== "undefined") (window as unknown as { __kilnVendorMock?: unknown }).__kilnVendorMock = {
    pin: (on = true) => { pinned = on; },
    failNext: (message: string, code: KilnError["code"] = "hash_mismatch") => { failNext = { code, message }; },
    pace: (ms: number) => { stepMs = ms; },
    changeLicense: (id: string) => { const c = comp(id); c.license = { ...c.license, sha256: c.license.sha256.replace(/^./, (x) => (x === "0" ? "1" : "0")) }; },
    damage: (id: string, file?: string, gameId = "client-steam") => {
      const i = installed.get(`${gameId}/${id}`);
      if (i) i.damaged = [file ?? comp(id).files[0].name];
    },
    install: (id: string, version: string, gameId = "client-steam") => { installed.set(`${gameId}/${id}`, { version, at: new Date().toISOString(), damaged: [] }); },
    calls: () => calls.map((c) => ({ ...c })),
    agreed: () => [...agreed],
  };
  return handlers;
}
