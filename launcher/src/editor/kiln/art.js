// The game art and the items of Kiln mods, for both shells (KilnEditor.jsx on the desktop, deck/DeckEditor.jsx): the
// mod overlay (<art>/modded/items.json) is merged into the editor's tables before a save is read and again when it
// changes (the open character is read again then, unless it has changes); a missing icon becomes a neutral square;
// `hint` is the one line while the launcher makes the art from the game ("Preparing game art…") or cannot.
import { useEffect, useRef, useState } from 'react';
import { assetsPresent, IS_TAURI, ASSET_BASE } from '../src/platform.js';
import { mergeModded } from '../src/gamedata.js';
import { useAssetsStatus, useExtractTask } from '../../components/GameArt';
import { reopenIfClean } from './session.js';
import { PLACEHOLDER } from './parts.jsx';
import { t } from '../../i18n';
import { setGameNames } from './game-names.js';

/** The dev fallback that copies upstream's art (the browser dev server only). */
export const FETCH_SCRIPT = 'tools/launcher/fetch-editor-assets.sh';

async function loadOverlay(version) {
  if (!version) return null;
  try {
    const r = await fetch(`${ASSET_BASE}modded/items.json?v=${encodeURIComponent(version)}`);
    return r.ok ? await r.json() : null;
  } catch {
    return null;
  }
}

async function loadNames(version) {
  try {
    const r = await fetch(`${ASSET_BASE}names/names.json?v=${encodeURIComponent(version)}`);
    return r.ok ? await r.json() : null;
  } catch { return null; }
}

/** {ready, gen, assets, hint}: mount the views once `ready`; key them by `gen` (new art: icons that fell back to the
 *  placeholder load again). `root`: the element whose broken images get the placeholder. */
export function useEditorArt(root) {
  const [assets, setAssets] = useState(null);
  const [gen, setGen] = useState(0);
  const [ready, setReady] = useState(false);
  const st = useAssetsStatus();
  const task = useExtractTask(st.data);
  const e = st.data?.editor;
  const known = !IS_TAURI || st.data !== undefined || st.isError;
  const artKey = e?.extracted ?? '';
  const modVersion = e?.modded?.version ?? '';
  useEffect(() => { assetsPresent().then(setAssets); }, [artKey]);
  // Load both tables before mounting either shell. A fresh art generation also refreshes the local names.
  const merged = useRef(false);
  useEffect(() => {
    if (!known) return undefined;
    let live = true;
    Promise.all([loadOverlay(modVersion), loadNames(artKey)]).then(([ov, names]) => {
      if (!live) return;
      setGameNames(names);
      mergeModded(ov, modVersion);
      if (merged.current) { setGen((g) => g + 1); reopenIfClean(); }
      merged.current = true;
      setReady(true);
    });
    return () => { live = false; };
  }, [modVersion, artKey, known]);
  // on the document: the catalog is a dialog drawn outside the editor's element (the desktop's Modal, a Deck dialog)
  useEffect(() => {
    const onError = (ev) => {
      const img = ev.target;
      if (img instanceof HTMLImageElement && img.src !== PLACEHOLDER && (root.current?.contains(img) || img.closest('.ke-cat, .ke-insp, .ke-slot'))) img.src = PLACEHOLDER;
    };
    document.addEventListener('error', onError, true);
    return () => document.removeEventListener('error', onError, true);
  }, [root]);
  const preparing = IS_TAURI && (task || (assets === false && e?.canExtract !== false));
  // hintKey: the launcher key of the line (KilnEditor.jsx sets the dev script's name in code); hint: its text
  const hintKey = preparing ? 'editor.art.preparing'
    : assets === false ? (IS_TAURI ? 'editor.art.needsClient' : 'editor.art.notFound')
      : null;
  const hint = hintKey ? t(hintKey, { script: FETCH_SCRIPT }) : null;
  return { ready, gen, assets, hint, hintKey, preparing: !!preparing };
}
