// kiln-sfi's row in the Engines view.
// The ShroudForge row of the Engines view while the game's ShroudForge mods go to Kiln's interpreter: kiln-sfi's
// kiln/state/sfi.json as kiln_manage::sfi_state reads it. The phase word, its reason, the start before it when that one
// died, and one line per package, each with its tone. A record that is not this start's (sfi_state `staleCode`) and a
// file that cannot be read are "unknown" with why, never running or done.
import type { EngineStatus, Json } from "../api/commands";
import { KIND_NAME, KIND_SHORT, KIND_TONE } from "../api/kinds";
import { t, useT } from "../i18n";
import { Badge, type Tone } from "../ui";

type Obj = Record<string, Json>;
const isObj = (v: unknown): v is Obj => !!v && typeof v === "object" && !Array.isArray(v);
const str = (v: Json | undefined): string | null => (typeof v === "string" && v ? v : typeof v === "number" ? String(v) : null);

/** The row of the interpreter: kiln_manage::sfi_state writes plugin "kiln-sfi" on the ShroudForge row. */
export const isSfiRow = (s: EngineStatus) => s.kind === "shroudforge" && s.plugin === "kiln-sfi";

const PHASES: Record<string, Tone> = { starting: "info", running: "ok", ended: "idle", aside: "idle", refused: "warn", unknown: "warn" };
const PACKAGES: Record<string, Tone> = { running: "ok", "data applied": "ok", off: "idle", skipped: "warn" };
const PACKAGE_KEY: Record<string, string> = { running: "running", "data applied": "dataApplied", off: "off", skipped: "skipped" };
const STALE = ["older-start", "not-running", "other-process"];

export interface SfiLine { id: string; version: string | null; word: string; tone: Tone; reason: string | null }
export interface SfiView {
  /** The phase in the current language (a phase this launcher does not know: as written). */
  word: string;
  tone: Tone;
  reason: string | null;
  /** The start before the record's own, when it did not end cleanly: never this start's fact. */
  previous: string | null;
  lines: SfiLine[];
}

const phaseWord = (phase: string) => (phase in PHASES ? t(`sfBackend.engineRow.phase.${phase}`) : phase);

export function sfiView(s: EngineStatus): SfiView {
  if (s.error || !isObj(s.state)) {
    return { word: t("sfBackend.engineRow.phase.unknown"), tone: "warn", previous: null, lines: [],
      reason: s.error ? t("sfBackend.engineRow.unreadable", { error: s.error }) : null };
  }
  const st = s.state;
  const phase = str(st.status) ?? "unknown";
  const known = phase in PHASES;
  const stale = str(st.staleCode);
  const reason = stale && STALE.includes(stale)
    ? t(`sfBackend.engineRow.stale.${stale}`, { phase: phaseWord(str(st.recordPhase) ?? "unknown") })
    : str(st.reason);
  const prev = str(st.previous);
  const lines = (Array.isArray(st.mods) ? st.mods : []).filter(isObj).map((m): SfiLine => {
    const state = str(m.state) ?? "unknown";
    return {
      id: str(m.id) ?? "?",
      version: str(m.version),
      word: state in PACKAGE_KEY ? t(`sfBackend.engineRow.package.${PACKAGE_KEY[state]}`) : state,
      tone: PACKAGES[state] ?? "info",
      reason: str(m.reason),
    };
  });
  return {
    word: phaseWord(phase),
    tone: known ? PHASES[phase] : "info",
    reason,
    previous: prev === "incomplete" || prev === "suspect" ? t(`sfBackend.engineRow.previous.${prev}`) : null,
    lines,
  };
}

export function SfiEngineRow({ s }: { s: EngineStatus }) {
  const t = useT();
  const v = sfiView(s);
  const phase = isObj(s.state) ? str(s.state.status) ?? "unknown" : "unknown";
  return (
    <div className="k-list-item" data-engine={s.kind} data-sfi-phase={phase} style={{ flexWrap: "wrap", alignItems: "baseline", rowGap: 2 }} title={s.file}>
      <Badge tone={KIND_TONE[s.kind]} title={KIND_NAME[s.kind]}>{KIND_SHORT[s.kind]}</Badge>
      <span className="k-mono k-muted">{t("sfBackend.engineRow.plugin")}</span>
      <span className="k-spacer" />
      <span className={`k-word-sm k-tone-${v.tone}`}>{v.word}</span>
      {v.lines.length > 0 && <span className="k-hint">{t("engineStatus.mods", { count: v.lines.length })}</span>}
      {v.reason && <span className={`k-hint${v.tone === "warn" || v.tone === "err" ? " k-tone-warn" : ""}`} data-sfi-reason="" style={{ flexBasis: "100%" }}>{v.reason}</span>}
      {v.previous && <span className="k-hint k-tone-warn" data-sfi-previous="" style={{ flexBasis: "100%" }}>{v.previous}</span>}
      {v.lines.length > 0 && (
        <div className="k-hint" style={{ flexBasis: "100%" }} aria-label={t("sfBackend.engineRow.packages")}>
          {v.lines.map((l, i) => (
            <div key={l.id + i} data-sfi-package={l.id}>
              <span className="k-mono">{l.id}</span>{l.version ? <span className="k-muted"> {l.version}</span> : null}{" "}
              <span className={`k-tone-${l.tone}`}>{l.word}</span>
              {l.reason && <span>: {l.reason}</span>}
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
