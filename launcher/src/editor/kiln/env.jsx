// Where the editor's views run. The desktop page (EditorApp.jsx): mouse and keyboard, the launcher's Modal, a typed
// number, a <select>, hover. The Deck screen (../deck/DeckEditor.jsx) provides another env: a controller only, so a
// dialog is a Deck dialog (a layer: B closes it), a number is set on the number pad, a choice is a list, what hover
// shows follows focus, and a view's X/Y/trigger actions go to the screen's hint bar. The views and their rules stay one
// set of files: a fix lands in both.
import { createContext, useContext, useEffect, useRef } from 'react';

export const EditorEnv = createContext({
  deck: false,
  /** (props: {title, onClose, children, wide}) => the dialog frame; desktop: the launcher's Modal */
  Dialog: null,
  /** ({title, value, min, max, presets: [{label, value}], onDone}) => void: the Deck's number pad */
  askNumber: null,
  /** ({title, value, options: [{value, label, sub}], onDone}) => void: the Deck's list of choices */
  choose: null,
  /** ({actions, back} | null) => void: the open view's own actions on the Deck's hint bar (X/Y/triggers) and what B does
   *  first ({label, run}; null: the screen's own B) */
  setActions: null,
});

export const useEditorEnv = () => useContext(EditorEnv);

/** Desktop grid navigation, using the rendered column count so responsive layouts keep their arrow keys. */
export function gridKey(e) {
  if (e.altKey || e.ctrlKey || e.metaKey) return false;
  if (!['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown', 'Home', 'End'].includes(e.key)) return false;
  const grid = e.currentTarget.closest('[data-editor-grid]');
  const cells = [...(grid?.querySelectorAll('[data-editor-cell]') ?? [])];
  const at = cells.indexOf(e.currentTarget);
  if (at < 0) return false;
  const top = cells[0].getBoundingClientRect().top;
  const columns = cells.filter((el) => Math.abs(el.getBoundingClientRect().top - top) < 1).length || 1;
  const next = e.key === 'Home' ? 0 : e.key === 'End' ? cells.length - 1
    : at + ({ ArrowLeft: -1, ArrowRight: 1, ArrowUp: -columns, ArrowDown: columns }[e.key]);
  e.preventDefault();
  cells[Math.max(0, Math.min(cells.length - 1, next))]?.focus();
  return true;
}

/** A radio group has one Tab stop; arrows select and wrap, Home/End select the ends. Deck navigation stays spatial. */
export function radioKey(e) {
  if (e.altKey || e.ctrlKey || e.metaKey) return;
  const step = e.key === 'ArrowRight' || e.key === 'ArrowDown' ? 1 : e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? -1 : 0;
  if (!step && e.key !== 'Home' && e.key !== 'End') return;
  const radios = [...e.currentTarget.querySelectorAll('[role="radio"]:not(:disabled)')];
  const at = radios.indexOf(e.target);
  if (at < 0) return;
  e.preventDefault();
  const next = radios[e.key === 'Home' ? 0 : e.key === 'End' ? radios.length - 1 : (at + step + radios.length) % radios.length];
  next?.focus();
  next?.click();
}

/** A view's actions on the Deck's hint bar while it is shown: [{button, label, run, disabled}], and `back` ({label,
 *  run}) for B before the screen's own. The run functions read the latest render. Nothing on the desktop. */
export function useDeckActions(actions, back = null) {
  const env = useEditorEnv();
  const latest = useRef({ actions, back });
  latest.current = { actions, back };
  const sig = JSON.stringify([(actions ?? []).map((a) => [a.button, a.label, !!a.disabled]), back?.label ?? null]);
  useEffect(() => {
    if (!env.setActions) return undefined;
    env.setActions({
      actions: (actions ?? []).map((a, i) => ({ ...a, run: () => latest.current.actions?.[i]?.run() })),
      back: back ? { label: back.label, run: () => latest.current.back?.run() } : null,
    });
    return undefined;
  }, [sig, env]);
  useEffect(() => () => env.setActions?.(null), [env]);
}
