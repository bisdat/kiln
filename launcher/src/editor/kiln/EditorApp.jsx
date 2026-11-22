// The save editor as a launcher page (Hearth): the character's name (a button: other saves), one live sentence, Save
// and Backups; tabs for Equipment, Inventory, Skills and Knowledge. The session (the saves, the open character, save,
// discard, restore) is session.js, shared with the Deck screen (../deck/DeckEditor.jsx); this file replaces upstream's
// in-game frame (src/App.jsx).
// Keys: Ctrl+S save, Z discard (asks first), B backups, Q/E previous/next tab (not while typing).
import React, { useEffect, useRef, useState } from 'react';
import { Banner, Button, Modal, Tabs, toast } from '../../ui';
import { api } from '../src/platform.js';
import { t, useLanguage } from '../../i18n';
import { dataHora, definirIdioma, assinarIdioma } from '../src/i18n.js';
import { canSave, discard, openSave, readOnly, restoreBackup, saveModel, setModel, setStatus, startSession, useSession } from './session.js';
import Equipment from './Equipment.jsx';
import Inventory from './Inventory.jsx';
import Skills from './Skills.jsx';
import Knowledge from './Knowledge.jsx';

// the editor's own words are the launcher's (launcher/src/i18n, "editor.*", in the launcher's language); item and skill
// names are the game's, English outside Portuguese (the game data ships both)
definirIdioma('en');

export const TABS = [['char', 'editor.tabs.equipment'], ['inv', 'editor.tabs.inventory'], ['skills', 'editor.tabs.skills'], ['know', 'editor.tabs.knowledge']];

/** The page's one live sentence (the Deck screen says the same). */
export function sayOf(s) {
  const { model, saves, running, dirty } = s;
  return !model
    ? (saves === null ? t('editor.page.lookingForSaves') : saves.length ? t('editor.page.pickCharacter') : t('editor.page.noSaves'))
    : model.readOnly ? t('editor.page.cannotParse')
      : model.error ? t('editor.page.readOnlyNoSave')
      : running ? t('editor.page.gameOpen')
        : dirty ? t('editor.page.unsaved')
          : t('editor.page.levelBackup', { n: model.level });
}

export default function EditorApp() {
  useLanguage();
  const s = useSession();
  const { saves, model, status, running, saving, dirty } = s;
  const [tab, setTab] = useState('inv');
  const [backups, setBackups] = useState(false);
  const [pick, setPick] = useState(false);
  // what waits for the player to agree to drop the changes: {open: path} (another character) or {discard: true}
  const [ask, setAsk] = useState(null);
  const [, redraw] = useState(0);
  useEffect(() => assinarIdioma(() => redraw((n) => n + 1)), []);
  useEffect(() => startSession(), []);

  const open = (path) => {
    setPick(false);
    if (dirty && path !== model?.path) setAsk({ open: path });
    else if (path !== model?.path || !dirty) openSave(path);
  };
  const save = async () => {
    const r = await saveModel();
    if (r === 'saved') toast.ok(t('editor.page.saved'), t('editor.page.savedHint'));
    if (r === 'skills') setTab('skills');
  };
  const ok = canSave(s);
  useEffect(() => {
    const k = (e) => {
      if (e.ctrlKey && e.key.toLowerCase() === 's') { e.preventDefault(); save(); return; }
      if (e.ctrlKey || e.altKey || e.metaKey || e.target.closest?.('input, select, textarea, [role="dialog"]')) return;
      if ((e.key === 'b' || e.key === 'B') && model) setBackups((v) => !v);
      if ((e.key === 'z' || e.key === 'Z') && dirty) setAsk({ discard: true });
      const at = TABS.findIndex((x) => x[0] === tab);
      if (e.key === 'q' || e.key === 'Q') setTab(TABS[(at + TABS.length - 1) % TABS.length][0]);
      if (e.key === 'e' || e.key === 'E') setTab(TABS[(at + 1) % TABS.length][0]);
    };
    window.addEventListener('keydown', k);
    return () => window.removeEventListener('keydown', k);
  });

  const file = model?.path?.split(/[\\/]/).pop();
  const edit = model && !model.readOnly;
  // the game wrote the save after it was read: Read again is the way on (status from session.saveModel)
  const stale = !!(status?.again && model);

  return (
    <div className="k-page k-page-fill ke">
      <header className="k-page-head">
        <div>
          <div className="k-page-name">
            <SaveSwitch model={model} saves={saves} open={pick} setOpen={setPick} onOpen={open} />
            {file && <span className="k-page-meta"><span className="k-mono k-muted">{file}</span>{dirty && <span className="k-sc" style={{ color: 'var(--k-gold-400)' }}>{t('editor.page.notSaved')}</span>}</span>}
          </div>
          <p className="k-page-sub">{sayOf(s)}</p>
        </div>
        <div className="k-page-actions">
          {dirty && <Button variant="ghost" onClick={() => setAsk({ discard: true })} title="Z">{t('editor.page.discard')}</Button>}
          <Button disabled={!model} onClick={() => setBackups(true)} title="B">{t('editor.page.backups')}</Button>
          <Button variant="primary" disabled={!ok} loading={saving} onClick={save} title={running ? t('editor.page.gameOpenShort') : 'Ctrl+S'}>{t('editor.page.save')}</Button>
        </div>
      </header>
      {status && (
        <Banner tone={status.tone} title={status.tone === 'err' ? t('editor.page.error') : t('editor.page.note')}
          actions={<>
            {stale && <button type="button" className="k-link" onClick={() => openSave(model.path)}>{t('editor.page.readAgain')}</button>}
            <button type="button" className="k-link" onClick={() => setStatus(null)}>{t('editor.page.close')}</button>
          </>}>{status.text}</Banner>
      )}
      {edit && (
        <>
          <Tabs aria-label={t('editor.page.parts')} value={tab} onChange={setTab}
            items={TABS.map(([id, label]) => ({ id, label: t(label), count: id === 'know' ? model.knowledge?.length : undefined }))} />
          <div className={'ke-body' + (readOnly(model) ? ' ke-readonly' : '')} key={model.path}>
            {tab === 'char' && <Equipment model={model} setModel={setModel} />}
            {tab === 'inv' && <Inventory model={model} setModel={setModel} />}
            {tab === 'skills' && <Skills model={model} setModel={setModel} />}
            {tab === 'know' && <Knowledge model={model} setModel={setModel} />}
          </div>
        </>
      )}
      {backups && model && <Backups model={model} dirty={dirty} onClose={() => setBackups(false)} />}
      {ask && (
        <Modal open onClose={() => setAsk(null)} title={t('editor.page.discardTitle')} width={520}
          footer={<><Button variant="ghost" onClick={() => setAsk(null)}>{t('editor.page.cancel')}</Button><Button variant="danger" onClick={() => { const a = ask; setAsk(null); if (a.open) openSave(a.open); else discard(); }}>{ask.open ? t('editor.page.discardAndOpen') : t('editor.page.discard')}</Button></>}>
          <p>{t('editor.page.discardBody', { nome: model?.name ?? '' })}</p>
        </Modal>
      )}
    </div>
  );
}

/** The character's name: a button that lists the other character saves and "Open a save file…". */
function SaveSwitch({ model, saves, open, setOpen, onOpen }) {
  const ref = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    const close = (e) => { if (!ref.current?.contains(e.target)) setOpen(false); };
    const esc = (e) => { if (e.key === 'Escape') setOpen(false); };
    document.addEventListener('mousedown', close);
    document.addEventListener('keydown', esc);
    return () => { document.removeEventListener('mousedown', close); document.removeEventListener('keydown', esc); };
  }, [open]);
  return (
    <div className="ke-who" ref={ref}>
      <h1 className="k-page-title">
        <button type="button" className="ke-who-btn" aria-haspopup="listbox" aria-expanded={open} onClick={() => setOpen((o) => !o)}>
          <span>{model ? model.name : t('editor.page.title')}</span><svg viewBox="0 0 24 24" aria-hidden><path d="M5 9l7 7 7-7" /></svg>
        </button>
      </h1>
      {open && (
        <div className="k-popover ke-who-pop" role="listbox" aria-label={t('editor.page.characters')}>
          {(saves ?? []).map((s) => (
            <button key={s.path} type="button" role="option" aria-selected={model?.path === s.path} className="k-menu-item" onClick={() => onOpen(s.path)}>
              <span style={{ fontWeight: model?.path === s.path ? 700 : 400 }}>{s.name}</span>
              <span className="k-mono k-muted">{s.file}{s.level !== undefined ? ` · ${t('editor.common.levelN', { n: s.level })}` : ''}</span>
              {s.error && <span className="ke-note warn">{s.error}</span>}
            </button>
          ))}
          {saves?.length === 0 && <span className="ke-note" style={{ padding: '6px 10px' }}>{t('editor.page.noSaves')}</span>}
          <div className="k-menu-sep" />
          <button type="button" className="k-menu-item" style={{ color: 'var(--k-ember-300)' }}
            onClick={async () => { const f = await api.browseSave(); if (f) onOpen(f); }}>{t('editor.page.openFile')}</button>
        </div>
      )}
    </div>
  );
}

/** Upstream named the kinds jogo / pasta; the launcher's commands say game / folder. */
export const BACKUP_KIND = { game: 'editor.backups.kindGame', jogo: 'editor.backups.kindGame', folder: 'editor.backups.kindKiln', pasta: 'editor.backups.kindKiln' };
/** A backup's name in a list: the game's copy number, or the time a Kiln backup was made. */
export const backupName = (b) => (b.kind === 'game' || b.kind === 'jogo' ? b.label : t('editor.backups.beforeSave'));

/** The save's backups (the game's last ten and the editor's copies): restore one, with a second click to confirm. */
function Backups({ model, dirty, onClose }) {
  const [list, setList] = useState(null);
  const [err, setErr] = useState(null);
  const [confirm, setConfirm] = useState(null);
  useEffect(() => { api.listBackups(model.path).then(setList).catch((e) => { setList([]); setErr(e.message); }); }, []);
  const restore = async (b) => {
    try {
      await restoreBackup(b);
      toast.ok(t('editor.backups.restored'), t('editor.backups.restoredBody', { arquivo: backupName(b), quando: dataHora(b.modified) }));
      setConfirm(null); onClose();
    } catch (e) { setErr(e.message); setConfirm(null); }
  };
  return (
    <Modal open onClose={onClose} title={t('editor.backups.title', { nome: model.name })} width={720}
      footer={<><Button variant="ghost" onClick={() => api.openBackups()}>{t('editor.backups.openFolder')}</Button><Button onClick={onClose}>{t('editor.page.close')}</Button></>}>
      <p className="k-hint">{t('editor.backups.hint')}{dirty ? ` ${t('editor.backups.dropsChanges')}` : ''}</p>
      {err && <Banner tone="err" title={t('editor.page.error')}>{err}</Banner>}
      <div className="ke-bk">
        {list === null && <span className="ke-note">{t('editor.backups.looking')}</span>}
        {list?.length === 0 && !err && <span className="ke-note">{t('editor.backups.none')}</span>}
        {list?.map((b) => (
          <div key={b.path} className="ke-bk-row">
            <span className="k-sc k-muted">{BACKUP_KIND[b.kind] ? t(BACKUP_KIND[b.kind]) : b.kind}</span>
            <span className="k-num">{dataHora(b.modified)}</span>
            <span className="k-mono k-ellipsis" title={b.path}>{backupName(b)}</span>
            {confirm === b.path ? (
              <span className="k-row" style={{ gap: 6, justifyContent: 'flex-end' }}>
                <Button size="s" variant="ghost" onClick={() => setConfirm(null)}>{t('editor.page.cancel')}</Button>
                <Button size="s" variant="danger" onClick={() => restore(b)}>{t('editor.backups.restore')}</Button>
              </span>
            ) : (
              <span style={{ textAlign: 'right' }}><Button size="s" onClick={() => setConfirm(b.path)} disabled={!!confirm}>{t('editor.backups.restore')}</Button></span>
            )}
          </div>
        ))}
      </div>
    </Modal>
  );
}
