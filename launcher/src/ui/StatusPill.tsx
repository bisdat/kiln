// State words (glaze colours, always with a word: no pills, no chips), the banner strip, a keyboard hint.
// StatusPill and Badge keep their names and the tone prop; both render a word in its tone.
import { useEffect, useState, type ReactNode } from "react";
import { useT } from "../i18n";

export type Tone = "ok" | "warn" | "err" | "info" | "idle" | "ember";

export interface StatusPillProps {
  tone?: Tone;
  /** Kept for old callers (no animation: nothing glows unless it is hot). */
  pulse?: boolean;
  title?: string;
  children: ReactNode;
}

/** A state word: Connected (ok), Vanilla (info), Updating (warn), Refused (err), Stopped (idle), Running (ember). */
export function StatusPill({ tone = "idle", title, children }: StatusPillProps) {
  return <span className={`k-pill k-tone-${tone}`} title={title}>{cap(children)}</span>;
}

/** A smaller state word (a kind, a tag). */
export function Badge({ tone = "idle", title, children }: { tone?: Tone; title?: string; children: ReactNode }) {
  return <span className={`k-badge k-tone-${tone}`} title={title}>{cap(children)}</span>;
}

/** Words read as words: "running" shows as "Running". */
function cap(c: ReactNode): ReactNode {
  return typeof c === "string" && c.length ? c.charAt(0).toUpperCase() + c.slice(1) : c;
}

export function Kbd({ children }: { children: ReactNode }) {
  return <kbd className="k-kbd">{children}</kbd>;
}

export interface BannerProps {
  tone?: Tone;
  /** The state word or short lead, in its glaze colour. */
  title?: ReactNode;
  /** One action on the right. */
  actions?: ReactNode;
  /** One sentence. */
  children?: ReactNode;
  /** The player may fold the strip to its title line (Hide / Show); remembered under this id across starts. Give an id
   *  that changes when the notice says something new (a file name, a version), so a new notice opens again. */
  fold?: string;
}

const FOLD_KEY = "kiln.banner.folded.";

function readFolded(id: string | undefined): boolean {
  if (!id) return false;
  try { return localStorage.getItem(FOLD_KEY + id) === "1"; } catch { return false; }
}

/** A flat Soot 3 strip at the top of the sheet: refusals, warnings, Steam updates. */
export function Banner({ tone = "info", title, actions, children, fold }: BannerProps) {
  const t = useT();
  const [folded, setFolded] = useState(() => readFolded(fold));
  // a notice that says something new (a new fold id on a mounted strip) opens again
  useEffect(() => setFolded(readFolded(fold)), [fold]);
  const toggle = () => {
    const next = !folded;
    setFolded(next);
    try { if (next) localStorage.setItem(FOLD_KEY + fold, "1"); else localStorage.removeItem(FOLD_KEY + fold); } catch { /* private mode: this session only */ }
  };
  return (
    <div className={"k-banner" + (folded ? " folded" : "")} role={tone === "err" ? "alert" : "status"}>
      <div className="k-banner-body">
        {title && <div className={`k-banner-title k-tone-${tone}`}>{title}</div>}
        {children && !folded && <div className="k-banner-text">{children}</div>}
      </div>
      {(actions || fold) && (
        <div className="k-banner-actions">
          {actions}
          {fold && <button type="button" className="k-link k-banner-fold" aria-expanded={!folded} onClick={toggle}>{folded ? t("ui.banner.show") : t("ui.banner.hide")}</button>}
        </div>
      )}
    </div>
  );
}
