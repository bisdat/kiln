// Inventory: the backpack grid, swap bar (Alt) and action bar (1-8), with the inspector beside them. Drag or use
// Move (M), arrows and Enter/Space to move or swap; Escape cancels. Enter inspects an item or opens the catalog for
// an empty slot, Delete removes, Ctrl+D duplicates, I opens the catalog. On the Deck (env.jsx): focus picks the slot,
// A enters the inspector/catalog, Y moves, B cancels/returns. Slot keys and layout are upstream's (shared/inventory.mjs).
import React, { useEffect, useLayoutEffect, useRef, useState } from 'react';
import { Button } from '../../ui';
import { items, itemName } from '../src/gamedata.js';
import capacities from '../data/backpack-capacities.json';
import { inventoryLayout, entryAt, replaceSlots, keyOf } from '../shared/inventory.mjs';
import { t } from '../../i18n';
import { Inspector } from './Inspector.jsx';
import { Catalog } from './Catalog.jsx';
import { Slot } from './parts.jsx';
import { gridKey, useDeckActions, useEditorEnv } from './env.jsx';
import { readOnly } from './session.js';

/** An item no table has (another mod keeps it in the slot): shown locked, kept untouched (the save is read-only). */
const isModItem = (e) => !!e && !items[e.itemId];
const hexId = (id) => '0x' + ((Number(id) || 0) >>> 0).toString(16).padStart(8, '0');

export default function Inventory({ model, setModel }) {
  const [sel, setSel] = useState(null);
  const [catalog, setCatalog] = useState(false);
  const [dragFrom, setDragFrom] = useState(null);
  const env = useEditorEnv();
  const disabled = readOnly(model);
  const [moving, setMoving] = useState(null);
  const insp = useRef(null);
  const catalogReturn = useRef(null);
  const layout = inventoryLayout(model, items, capacities);
  const bySlot = new Map(layout.all.map((k) => [k, entryAt(model, k)]).filter(([, e]) => e));
  const entry = sel !== null ? bySlot.get(sel) : null;
  const firstFree = layout.all.find((k) => !bySlot.has(k)) ?? null;
  const slotEl = (key) => document.querySelector(`.ke-grid [data-deck-id="slot-${CSS.escape(key)}"]`);
  const focusSlot = (key) => requestAnimationFrame(() => slotEl(key)?.focus());
  const change = (edits) => { if (!disabled) setModel(replaceSlots(model, edits)); };
  const remove = () => { if (entry && !disabled) { change([[sel, null]]); focusSlot(sel); } };
  const target = sel !== null && !bySlot.has(sel) ? sel : firstFree;
  const openCatalog = () => { if (!disabled && moving === null) { catalogReturn.current = document.activeElement; setCatalog(true); } };
  const closeCatalog = () => { setCatalog(false); requestAnimationFrame(() => catalogReturn.current?.focus()); };
  const add = (e) => {
    if (target !== null && !disabled) { change([[target, e]]); setSel(target); catalogReturn.current = slotEl(target); }
  };
  const duplicate = () => {
    if (entry && firstFree !== null && !disabled) { change([[firstFree, entry]]); setSel(firstFree); if (!env.deck) focusSlot(firstFree); }
  };
  const move = (from, to) => {
    if (from === to || disabled) return;
    const a = bySlot.get(from), b = bySlot.get(to);
    if (!a) return;
    change([[from, b || null], [to, a]]);
    setSel(to);
  };
  const startMove = () => { if (entry && !disabled) { setMoving(sel); slotEl(sel)?.focus(); } };
  const cancelMove = () => { const source = moving; setMoving(null); if (source !== null) slotEl(source)?.focus(); };
  // Changing to a read-only character must not leave a pending move or catalog mutation behind.
  useEffect(() => { if (disabled) { setMoving(null); setCatalog(false); } }, [disabled]);

  const intoPanel = () => insp.current?.querySelector('button:not(:disabled), input:not(:disabled), select:not(:disabled)')?.focus();
  const [inPanel, setInPanel] = useState(false);
  useDeckActions(env.deck ? [
    moving !== null ? { button: 'y', label: t('editor.page.cancel'), run: cancelMove }
      : { button: 'y', label: t('editor.inventory.move'), disabled: !entry || disabled, run: startMove },
  ] : null, moving !== null ? { label: t('editor.page.cancel'), run: cancelMove }
    : inPanel ? { label: t('editor.page.back'), run: () => slotEl(sel)?.focus() } : null);
  const press = (key, e) => {
    setSel(key);
    if (moving !== null) { move(moving, key); setMoving(null); slotEl(key)?.focus(); return; }
    if (!env.deck) return;
    if (e) requestAnimationFrame(intoPanel); else openCatalog();
  };
  const slotKey = (ev, key, e) => {
    if (env.deck || gridKey(ev) || ev.ctrlKey || ev.altKey || ev.metaKey) return;
    if (ev.key === 'Enter' && moving === null) {
      ev.preventDefault(); setSel(key);
      if (e) requestAnimationFrame(intoPanel); else openCatalog();
    }
  };

  useEffect(() => {
    if (env.deck) return undefined;
    const k = (e) => {
      if (e.defaultPrevented || catalog || e.target.closest?.('input, select, textarea, [role="dialog"]')) return;
      if (e.key === 'Escape' && moving !== null) { e.preventDefault(); cancelMove(); return; }
      if (e.ctrlKey && (e.key === 'd' || e.key === 'D')) { e.preventDefault(); duplicate(); return; }
      if (e.ctrlKey || e.altKey || e.metaKey) return;
      if (e.key === 'Delete' && entry && moving === null) { e.preventDefault(); remove(); }
      if (e.key === 'i' || e.key === 'I') { e.preventDefault(); openCatalog(); }
      if ((e.key === 'm' || e.key === 'M') && moving === null) { e.preventDefault(); startMove(); }
    };
    window.addEventListener('keydown', k);
    return () => window.removeEventListener('keydown', k);
  });

  const cell = (key, n) => {
    const e = bySlot.get(key);
    const mod = isModItem(e);
    return (
      <div key={key} className="ke-cell"
        onDragOver={(ev) => { if (!disabled) { ev.preventDefault(); ev.dataTransfer.dropEffect = 'move'; } }}
        onDrop={(ev) => { ev.preventDefault(); const from = ev.dataTransfer.getData('text/plain') || dragFrom; if (from && bySlot.has(from)) move(from, key); setDragFrom(null); }}>
        <Slot entry={e} n={n} mod={mod} selected={sel === key || moving === key} drop={(dragFrom !== null && dragFrom !== key) || (moving !== null && moving !== key)} title={e ? itemName(e.itemId) : undefined}
          label={`${placeOf(key)}: ${mod ? t('editor.inventory.modItem') : e ? `${itemName(e.itemId)}${e.count > 1 ? ` ×${e.count}` : ''}` : t('editor.common.empty')}`}
          aria-pressed={sel === key} data-editor-cell="" onKeyDown={(ev) => slotKey(ev, key, e)}
          deckId={`slot-${key}`} deckA={moving !== null ? t('editor.inventory.putHere') : e ? t('editor.common.change') : disabled ? '-' : t('editor.common.add')}
          onFocus={() => { setSel(key); setInPanel(false); }}
          onClick={() => press(key, e)} onDoubleClick={() => { if (!e && !env.deck) { setSel(key); openCatalog(); } }}
          drag={e && !disabled ? { draggable: true, onDragStart: (ev) => { ev.dataTransfer.setData('text/plain', key); ev.dataTransfer.effectAllowed = 'move'; setDragFrom(key); }, onDragEnd: () => setDragFrom(null) } : undefined} />
      </div>
    );
  };
  const used = layout.bag.filter((k) => bySlot.has(k)).length;
  // the backpack grid scrolls inside its group: fade its foot while more rows are below
  const scroller = useRef(null);
  const [more, setMore] = useState(false);
  const look = () => { const el = scroller.current; if (el) setMore(el.scrollTop + el.clientHeight < el.scrollHeight - 4); };
  useLayoutEffect(() => {
    look();
    const ro = new ResizeObserver(look);
    if (scroller.current) ro.observe(scroller.current);
    return () => ro.disconnect();
  }, [layout.bag.length]);
  const slotNo = (key) => (layout.bag.indexOf(key) >= 0 ? layout.bag.indexOf(key) + 1 : Number(key.split(':')[1]) + 1);
  // The same container-aware names label slots, pending moves and catalog destinations.
  const placeOf = (key) => {
    const [c, s] = key.split(':'); const i = Number(s);
    if (c === 'main' && i >= 16 && i < 24) return t('editor.inventory.place.swap', { n: i - 15 });
    if (c === 'main' && i >= 24 && i < 32) return t('editor.inventory.place.action', { n: i - 23 });
    return t('editor.inventory.place.backpack', { n: slotNo(key) });
  };
  const modItems = layout.all.filter((k) => isModItem(bySlot.get(k)));

  return (
    <div className="ke-split">
      <div className="ke-main">
        {model.inventoryUnsure && !model.main.length && !model.extra.length && <p className="ke-note warn" role="alert">{t('editor.inventory.notFound')}</p>}
        {modItems.map((k) => (
          <p key={k} className="ke-note warn ke-mod-note">{t('editor.inventory.hiddenModItem', { place: placeOf(k), id: hexId(bySlot.get(k).itemId) })}</p>
        ))}
        <section className="ke-group ke-group-grow">
          <header className="ke-group-head">
            <h2 className="k-card-title">{t('editor.tabs.inventory')}</h2>
            <span className="k-card-sub">{t('editor.inventory.slotsUsed', { n: used, total: layout.bag.length, count: layout.bag.length })}</span>
            <span className="k-spacer" />
            {/* the Deck: Y moves; A on an empty slot adds (buttons above the grid sit in the D-pad's way) */}
            {!env.deck && <>
              <Button size="s" onClick={moving !== null ? cancelMove : startMove} disabled={disabled || (moving === null && !entry)} title={moving !== null ? 'Escape' : 'M'}>{moving !== null ? t('editor.page.cancel') : t('editor.inventory.move')}</Button>
              <Button size="s" onClick={openCatalog} disabled={disabled || moving !== null} title={t('editor.inventory.addItem') + ' (I)'}>{t('editor.inventory.addItem')}</Button>
            </>}
          </header>
          <div className={'ke-grid-scroll' + (more ? ' more' : '')} ref={scroller} onScroll={look}><div className="ke-grid" data-editor-grid="">{layout.bag.map((k) => cell(k))}</div></div>
        </section>
        <section className="ke-group">
          <header className="ke-group-head">
            <h2 className="k-card-title">{t('editor.inventory.swapBar')}</h2>
            <span className="k-card-sub">{t('editor.inventory.swapBarHint')}</span>
          </header>
          <div className="ke-grid" data-editor-grid="">{Array.from({ length: 8 }, (_, c) => cell(keyOf('main', 16 + c)))}</div>
        </section>
        <section className="ke-group">
          <header className="ke-group-head"><h2 className="k-card-title">{t('editor.inventory.actionBar')}</h2></header>
          <div className="ke-grid" data-editor-grid="">{Array.from({ length: 8 }, (_, c) => cell(keyOf('main', 24 + c), c + 1))}</div>
        </section>
        <p className="ke-note" role={moving !== null ? 'status' : undefined}>{moving !== null && !env.deck ? t('editor.inventory.movingHint', { place: placeOf(moving) }) : env.deck ? t('editor.inventory.deckHint') : t('editor.inventory.hint')}</p>
      </div>
      <div ref={insp} className="ke-insp-wrap" onFocus={() => setInPanel(true)} onBlur={(e) => { if (!e.currentTarget.contains(e.relatedTarget)) setInPanel(false); }}>
      <Inspector entry={entry} disabled={disabled} onChange={(e) => change([[sel, e]])}
        onRemove={remove} onDuplicate={firstFree !== null ? duplicate : null}
        empty={sel === null ? (
          <div className="ke-empty"><span className="ke-empty-word">{t('editor.inventory.noneSelected')}</span><span className="ke-note">{t('editor.inventory.noneSelectedHint')}</span></div>
        ) : (
          <div className="ke-empty">
            <span className="ke-empty-word">{t('editor.inventory.slotEmpty', { n: slotNo(sel) })}</span>
            <span className="ke-note">{t('editor.inventory.emptyHint')}</span>
            <Button onClick={openCatalog} disabled={disabled || moving !== null}>{t('editor.inventory.pickItem')}</Button>
          </div>
        )} />
      </div>
      <Catalog open={catalog} disabled={disabled} target={target === null ? null : Number(target.split(':')[1])} targetPlace={target === null ? null : placeOf(target)} onAdd={add} onClose={closeCatalog} level={model.level} />
    </div>
  );
}
