// Equipment: the character's level and XP, the six attributes the tree gives, and what they wear: armour, the
// appearance pieces over it, and the gear (rings, shield, hook, glider, backpack), with the inspector beside them.
// The slot order is the bit order of the save's equipment mask (upstream src/Character.jsx): never reorder SLOTS.
// On the Deck (env.jsx): focus picks the slot, A goes into the inspector, B there goes back to the slot.
import React, { useRef, useState } from 'react';
import { items, itemName, ATRIBUTOS, atributosDaArvore } from '../src/gamedata.js';
import { t } from '../../i18n';
import { t as tv } from '../src/i18n.js';
import { Inspector } from './Inspector.jsx';
import { Slot, Stepper } from './parts.jsx';
import { useDeckActions, useEditorEnv } from './env.jsx';
import { readOnly } from './session.js';

// index = bit in the equipment mask: 0-4 appearance, 5-9 armour, then ring, ring, shield, hook, glider, backpack
const SLOTS = ['headVanity', 'chestVanity', 'armsVanity', 'legsVanity', 'feetVanity',
  'head', 'chest', 'arms', 'legs', 'feet', 'ring', 'ring', 'shield', 'hook', 'glider', 'backpack'];
const slotWord = (i) => (SLOTS[i] ? t(`editor.equipment.slots.${SLOTS[i]}`) : t('editor.equipment.slots.slot', { n: i }));
// the short word beside a slot: an appearance slot says "Head", as its armour slot does (the group says Appearance)
const slotShort = (i) => slotWord(SLOTS[i]?.endsWith('Vanity') ? i + 5 : i);
const GROUPS = [
  ['editor.equipment.groups.armour', [5, 6, 7, 8, 9]],
  ['editor.equipment.groups.appearance', [0, 1, 2, 3, 4]],
  ['editor.equipment.groups.accessories', [10, 11, 12, 13, 14, 15]],
];
// with 10 per attribute, four of six match the game exactly on a level-47 character; the rest come from nodes that
// follow the Flame's level and from gear, neither of which is in the character save
const BASE_ATTR = 10;

export default function Equipment({ model, setModel }) {
  const [sel, setSel] = useState(5);
  const disabled = readOnly(model);
  const [warn, setWarn] = useState(null);
  // the save's list is dense: piece k sits in the k-th lit bit of the mask
  const lit = [];
  for (let i = 0; i < 32; i++) if (model.equipMask & (1 << i)) lit.push(i);
  const bySlot = new Map(model.equipment.map((e) => [lit[e.slot] ?? e.slot, e]));
  const entry = bySlot.get(sel);
  const update = (e) => { if (!disabled) setModel({ ...model, equipment: model.equipment.map((x) => (x.slot === e.slot ? e : x)) }); };
  const remove = (e) => {
    if (disabled) return;
    // taking the worn backpack off deletes its container and everything in it
    if (items[e.itemId]?.slot === 'Backpack' && model.extra.length) {
      setWarn(t('editor.equipment.backpackFull', { item: itemName(e.itemId), count: model.extra.length }));
      return;
    }
    setWarn(null);
    setModel({ ...model, equipment: model.equipment.filter((x) => x.slot !== e.slot) });
  };
  const env = useEditorEnv();
  const insp = useRef(null);
  const [inPanel, setInPanel] = useState(false);
  const slotEl = (i) => document.querySelector(`.ke-gear [data-deck-id="worn-${i}"]`);
  useDeckActions(env.deck ? [] : null, inPanel ? { label: t('editor.page.back'), run: () => slotEl(sel)?.focus() } : null);
  const { daArvore, porChama } = atributosDaArvore(model.skills);
  const flame = Object.values(porChama).some((n) => n > 0);

  return (
    <div className="ke-split">
      <div className="ke-main">
        <section className="ke-facts">
          <label className="ke-fact"><span className="k-label">{t('editor.common.level')}</span>
            <Stepper disabled={disabled} id="char-level" value={model.level} min={1} max={50} label={t('editor.common.level')} onChange={(v) => { if (!disabled) setModel({ ...model, level: v }); }} /></label>
          <label className="ke-fact"><span className="k-label">{t('editor.equipment.xp')}</span>
            <Stepper disabled={disabled} id="char-xp" value={model.xp} min={0} max={1e9} big={1000} label={t('editor.equipment.xp')} onChange={(v) => { if (!disabled) setModel({ ...model, xp: v }); }} /></label>
          <dl className="ke-attrs" title={t('editor.equipment.attrsHint') + (flame ? `\n${t('editor.equipment.attrsFlameHint')}` : '')}>
            {ATRIBUTOS.map((a) => <div key={a}><dt className="k-label">{tv(a)}</dt><dd className="k-num">{BASE_ATTR + daArvore[a]}</dd></div>)}
          </dl>
        </section>
        <div className="ke-gear">
          {GROUPS.map(([title, idx]) => (
            <section key={title} className="ke-group">
              <header className="ke-group-head"><h2 className="k-card-title">{t(title)}</h2></header>
              <ul className="ke-gear-list">
                {idx.map((i) => {
                  const e = bySlot.get(i);
                  return (
                    <li key={i} className={'ke-gear-row' + (sel === i ? ' on' : '')} onClick={() => setSel(i)}>
                      <Slot entry={e} size={48} selected={sel === i} title={e ? itemName(e.itemId) : slotWord(i)}
                        label={`${slotWord(i)}: ${e ? itemName(e.itemId) : t('editor.common.empty')}`} deckId={`worn-${i}`} deckA={e ? t('editor.common.change') : '-'}
                        onFocus={() => { setSel(i); setInPanel(false); }}
                        onClick={() => { setSel(i); if (env.deck && e) requestAnimationFrame(() => insp.current?.querySelector('button:not(:disabled)')?.focus()); }}
                        onKeyDown={env.deck ? undefined : (ev) => { if (ev.key === 'Enter' && e) { ev.preventDefault(); setSel(i); insp.current?.querySelector('button:not(:disabled), input:not(:disabled), select:not(:disabled)')?.focus(); } }} />
                      <span className="ke-gear-text">
                        <span className="k-label">{slotShort(i)}</span>
                        <span className={'ke-gear-name' + (e ? '' : ' none')}>{e ? itemName(e.itemId) : t('editor.common.empty')}</span>
                      </span>
                    </li>
                  );
                })}
              </ul>
            </section>
          ))}
        </div>
      </div>
      <div ref={insp} className="ke-insp-wrap" onFocus={env.deck ? () => setInPanel(true) : undefined} onBlur={env.deck ? (e) => { if (!e.currentTarget.contains(e.relatedTarget)) setInPanel(false); } : undefined}>
      <Inspector disabled={disabled} entry={entry} caption={slotWord(sel)} warn={warn} onChange={update}
        onRemove={entry ? () => { remove(entry); if (env.deck) requestAnimationFrame(() => slotEl(sel)?.focus()); else slotEl(sel)?.focus(); } : null}
        empty={<div className="ke-empty"><span className="ke-empty-word">{t('editor.common.empty')}</span><span className="ke-note">{t('editor.equipment.emptyHint')}</span></div>} />
      </div>
    </div>
  );
}
