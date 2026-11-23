// SPDX-License-Identifier: MIT
// Read-only storage mirror; returning items is an explicit, backed-up offline operation.
import React, { useState } from 'react';
import { Banner, Button, Modal } from '../../ui';
import { DeckButton } from '../../deck/ui';
import { t } from '../../i18n';
import { itemName } from '../src/gamedata.js';
import { useEditorEnv } from './env.jsx';
import { useSession, mutationsBlocked, readOnly, returnStorageModel } from './session.js';

function StorageAction({ id, children, ...props }) {
  const env = useEditorEnv();
  return env.deck ? <DeckButton id={id} {...props}>{children}</DeckButton> : <Button {...props}>{children}</Button>;
}

export function StorageRestoreWarning({ model }) {
  return model?.storage?.hasStorage ? <p className="k-hint" role="note">{t('playerStorage.restoreWarning')}</p> : null;
}

export default function Storage({ model }) {
  const env = useEditorEnv();
  const state = useSession();
  const [ask, setAsk] = useState(false), [result, setResult] = useState(null), [error, setError] = useState(null);
  const storage = model.storage;
  const busy = state.saving;
  const disabled = !storage?.canReturn || !storage?.local || state.running || state.dirty || readOnly(model) || mutationsBlocked(state);
  const close = () => { if (!busy) setAsk(false); };
  const run = async () => {
    setError(null);
    try { setResult(await returnStorageModel()); setAsk(false); }
    catch (e) { setError(e.message); setAsk(false); }
  };
  const contents = <>
    <p className="k-hint">{t('playerStorage.returnBody')}</p>
    {storage?.serverCopy && <p className="k-hint">{t('playerStorage.serverCopy')}</p>}
    <div className="k-page-actions">
      <StorageAction id="storage-cancel" disabled={busy} onClick={close}>{t('editor.page.cancel')}</StorageAction>
      <StorageAction id="storage-confirm" disabled={busy} onClick={() => void run()}>{busy ? t('playerStorage.returning') : t('playerStorage.confirm')}</StorageAction>
    </div>
  </>;
  const Dialog = env.Dialog;
  return <section className="ke-storage" role="region" aria-label={t('playerStorage.title')}>
    <h2 className="k-sub">{t('playerStorage.title')}</h2>
    <p className="k-hint">{t('playerStorage.readOnly')}</p>
    {storage?.error && <Banner tone="warn" title={t('playerStorage.unavailable')}>{storage.error}</Banner>}
    {error && <Banner tone="err" title={t('editor.page.error')}>{error}</Banner>}
    {result && <p role="status">{t('playerStorage.result', { returned: result.returned, remaining: result.remaining })}{result.backup && <> <span className="k-mono">{result.backup}</span></>}</p>}
    {result?.modItems > 0 && <p role="note">{t('playerStorage.modItems', { count: result.modItems })}</p>}
    {!storage?.hasStorage && <p className="k-hint">{t('playerStorage.empty')}</p>}
    {storage?.hasStorage && <>
      {!storage.local && <p className="k-hint">{t('playerStorage.localOnly')}</p>}
      {state.running && <p className="k-hint">{t('playerStorage.closeGame')}</p>}
      {state.dirty && <p className="k-hint">{t('playerStorage.saveFirst')}</p>}
      {storage.paired === false && <p className="k-hint">{t('playerStorage.unpaired')}</p>}
      {(storage.invalidRecords > 0 || storage.quarantined > 0) && <p className="k-hint">{t('playerStorage.unreadable')}</p>}
      <StorageAction id="storage-return" disabled={disabled} onClick={() => setAsk(true)}>{t('playerStorage.return')}</StorageAction>
      {(storage.groups ?? []).map((group) => <section key={`${group.parked}:${group.key}`} className="ke-storage-group">
        <h3 className="k-sub">{group.key}{group.parked ? ` · ${t('playerStorage.parked')}` : ''}</h3>
        <ul>{group.records.map((record, i) => <li key={`${record.slot}:${i}`}>
          <strong>{itemName(record.itemId) || `0x${Number(record.itemId).toString(16)}`}</strong> ×{record.count}
          <span className="k-mono"> · {Object.entries(record).filter(([key, value]) => !['itemId', 'count'].includes(key) && value !== undefined).map(([key, value]) => `${key}: ${value}`).join(' · ')}</span>
        </li>)}</ul>
      </section>)}
    </>}
    {ask && (Dialog ? <Dialog name="storage-return" title={t('playerStorage.returnTitle')} onClose={close}>{contents}</Dialog>
      : <Modal open locked={busy} title={t('playerStorage.returnTitle')} onClose={close}>{contents}</Modal>)}
  </section>;
}
