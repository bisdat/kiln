// Implementação de window.api para a versão Tauri: a lógica do save roda aqui (JS no WebView)
// e o Rust cuida de arquivos, zstd, backups e da detecção do jogo.
// Kiln: the Rust side is the launcher's editor_* commands (launcher/src-tauri/src/commands/editor.rs on
// kiln_manage::saves::editor, Linux-aware), reached through launcher/src/api/commands.ts. The Guardian server copies
// ("servidor:<steamid>", SFTP) are left out: the launcher manages servers on its own pages.
import * as kiln from '../../api/commands';
import { pickPaths } from '../../ui/DropZone';
import * as sv2c from '../shared/sv2c-web.mjs';
import { parseCharacter, buildCharacter, readCharacter, modDataIn } from '../shared/character.mjs';
import * as know from '../shared/knowledge.mjs';
import items from '../data/items.json';
import skills from '../data/skills.json';
import { t } from './i18n.js';
import { t as launcherT } from '../../i18n';
import { itemLevelsFit } from '../kiln/levels.js';
import { useGameSel } from '../../api/hooks';
import { readExactCharacter, writeExactCharacter } from '../shared/character-exact.mjs';
import { inventoryWitness } from '../shared/storage-witness.mjs';
import { planStorageReturn, storageView } from '../shared/player-storage.mjs';
import { vanillaStackLimit } from '../shared/storage-catalog.mjs';

// Kiln: commands reject with a KilnError {code, message}; the editor shows e.message, so wrap it in an Error.
const wrap = (p) => p.catch((e) => {
  throw e instanceof Error ? e : new Error(e?.message ?? (typeof e === 'string' ? e : JSON.stringify(e)));
});
const COMMANDS = {
  list_saves: () => kiln.editorListSaves(),
  file_info: (a) => kiln.editorFileInfo(a.path),
  read_file: (a) => kiln.editorReadFile(a.path),
  zstd_decompress: (a) => kiln.editorZstdDecompress(a.data),
  zstd_compress: (a) => kiln.editorZstdCompress(a.data),
  game_running: () => kiln.editorGameRunning(),
  backup_file: (a) => kiln.editorBackupFile(a.path),
  write_save: (a) => kiln.editorWriteSave(a.path, a.data),
  list_backups: (a) => kiln.editorListBackups(a.target),
  restore_backup: (a) => kiln.editorRestoreBackup(a.src, a.target),
  open_backups: () => kiln.editorOpenBackups(),
};
const invoke = (cmd, args = {}) => wrap(Promise.resolve().then(() => COMMANDS[cmd](args)));

const isItem = (id) => Object.prototype.hasOwnProperty.call(items, id);
const isSkill = (id) => Object.prototype.hasOwnProperty.call(skills.nodes, id);
// O que pode estar vestido, pelo campo slot do item: separa a lista de equipamento da mochila,
// que também é densa e começa no índice 0. As armas ficam fora desta lista.
const ESPACOS = new Set(['Armour_Head', 'Armour_UpperBody', 'Armour_Arms', 'Armour_LowerBody', 'Armour_Feet',
  'Armour_Ring', 'Shield', 'HookShot', 'Glider', 'Backpack']);
const ehEquipavel = (id) => ESPACOS.has(items[id]?.slot);
const sessions = new Map();

const readFile = async (path) => new Uint8Array(await invoke('read_file', { path }));
const decompress = async (bytes) => new Uint8Array(await invoke('zstd_decompress', { data: Array.from(bytes) }));
const compress = async (bytes) => new Uint8Array(await invoke('zstd_compress', { data: Array.from(bytes) }));

async function load(path, includeStorage = false) {
  await sv2c.init();
  const bytes = await readFile(path);
  const save = await sv2c.parse(bytes, decompress, ['character_data', 'player_character', 'player_knowledge']);
  const pc = save.blocks.find((b) => b.name === 'player_character');
  let character = null, error = null;
  // Kiln: a save that holds items nobody defines any more (a mod that is gone) is read again accepting any item id
  // (shared/character.mjs readCharacter); those stacks show as "Unknown item (0x…)". A character the editor cannot
  // write whole stays read-only, with the reason: a mod's own data in the block (Belt Lantern's lantern record), items
  // no table knows, or an inventory whose place in the save is a guess. Writing would put an empty inventory over the
  // real one, or break the mod's record (player report 2026-11-20)
  const mods = [...new Set([...modDataIn(pc.payload), ...(pc.raw ? modDataIn(pc.raw) : [])])];
  try {
    const r = readCharacter(pc.payload, isItem, isSkill, ehEquipavel);
    character = r.character;
    if (mods.length) error = t('Este save guarda dados de um mod que o editor não sabe gravar ({mods}): somente leitura', { mods: mods.join(', ') });
    else if (r.unknownItems) error = t('Itens desconhecidos neste save (mod removido?): somente leitura');
    else if (character._invUnsure) error = t('Inventário não lido com certeza neste save (mochila e barras): somente leitura');
  } catch (e) { error = e.message; }
  let knowledge = null;
  const pk = save.blocks.find((b) => b.name === 'player_knowledge');
  try { if (pk?.payload) knowledge = know.parseBlock(pk.payload); } catch (e) { knowledge = null; }
  let storage = null, storageSnapshot = null;
  if (includeStorage) {
    try {
      storageSnapshot = await wrap(kiln.editorStorageRead(path, useGameSel.getState().gameId));
      if (storageSnapshot?.text) {
        const sidecar = JSON.parse(storageSnapshot.text);
        if (sidecar.mods?.['kiln-items']?.storage) {
          const exact = readExactCharacter(pc.payload, pc.head.readUInt32LE(10));
          storage = { ...await storageView(exact, sidecar, storageSnapshot.character), local: storageSnapshot.local };
          if (mods.length) { storage.canReturn = false; storage.error = error; }
        }
      }
    } catch (e) { storage = { hasStorage: true, canReturn: false, groups: [], error: e.message }; }
  }
  return { save, character, knowledge, error, bytes, storage, storageSnapshot };
}

function publicModel(path, modified, s) {
  const c = s.character;
  return {
    path, name: sv2c.characterName(s.save), modified, error: s.error, readOnly: !c,
    level: c?.level, xp: c?.xp, skills: c?.skills ?? [], equipment: c?.equipment ?? [], main: c?.main ?? [], extra: c?.extra ?? [],
    knowledge: s.knowledge?.entries ?? null,
    equipMask: c?._equipMask ?? 0, // posiciona cada peça no espaço certo do boneco
    // Kiln: the inventory's place in the save is a guess (shared/character.mjs): read-only, and the Inventory tab says
    // so when it read empty
    inventoryUnsure: !!c?._invUnsure,
    storage: s.storage,
    blocks: s.save.blocks.map((b) => ({ name: b.name, size: b.payload ? b.payload.length : b.size })),
  };
}

function checkModel(m) {
  const ints = (v, max) => Number.isInteger(v) && v >= 0 && v <= max;
  if (!ints(m.level, 1000) || !ints(m.xp, 0xffffffff)) throw new Error(t('Nível ou XP inválido'));
  for (const s of m.skills) if (!isSkill(s.id) || !ints(s.level, 255)) throw new Error(t('Habilidade inválida'));
  for (const list of [m.equipment, m.main, m.extra]) {
    for (const e of list) {
      if (!isItem(e.itemId)) throw new Error(t('Item desconhecido: {id}', { id: e.itemId }));
      if (!ints(e.slot, 255) || !ints(e.count, 65535)) throw new Error(t('Slot ou quantidade inválidos'));
      if (!itemLevelsFit(e)) throw new Error(launcherT('editor.inspector.invalidLevels'));
    }
  }
  const slots = m.main.map((e) => e.slot);
  if (new Set(slots).size !== slots.length) throw new Error(t('Dois itens no mesmo espaço do inventário'));
  if (m.knowledge) {
    for (const k of m.knowledge) {
      if (!ints(k.id, 0xffffffff) || !ints(k.value, 0xffffffff)) throw new Error(t('Entrada de conhecimento inválida'));
    }
    const ids = m.knowledge.map((k) => k.id);
    if (new Set(ids).size !== ids.length) throw new Error(t('Conhecimento com id repetido'));
  }
}

// o conhecimento só é regravado se mudou: assim um save aberto e salvo sem mexer nele fica igual
function mudouConhecimento(s, model) {
  if (!s.knowledge || !model.knowledge) return false;
  return JSON.stringify(s.knowledge.entries) !== JSON.stringify(model.knowledge);
}

export default {
  async listSaves() {
    const files = await invoke('list_saves');
    const out = [];
    for (const f of files) {
      try {
        const s = await load(f.path);
        out.push({ ...f, name: sv2c.characterName(s.save), level: s.character?.level, error: s.error });
      } catch (e) {
        out.push({ ...f, name: '?', error: e.message });
      }
    }
    return out;
  },
  // Kiln: no Guardian server copies here (see the header)
  serverState: () => ({ configurado: false, erro: null }),
  serverStamp: async () => null,
  async openSave(path, isCurrent = () => true) {
    const s = await load(path, true);
    const { modified } = await invoke('file_info', { path });
    // A superseded read must not replace the opaque save blocks that the current model will be written over.
    if (isCurrent()) sessions.set(path, s);
    return publicModel(path, modified, s);
  },
  async browseSave() {
    // Kiln: the launcher's dialog helper (plugin-dialog; null in a plain browser)
    const r = await pickPaths({ filters: [{ name: t('Save do Enshrouded'), extensions: ['ksd'] }], multiple: false, kind: 'editorSave' });
    return r?.[0] ?? null;
  },
  async writeSave(path, model) {
    if (await invoke('game_running')) throw new Error(t('Feche o Enshrouded antes de salvar: o jogo sobrescreveria a edição.'));
    const s = sessions.get(path);
    if (!s?.character) throw new Error(t('Save não aberto para edição'));
    checkModel(model);
    const c = { ...s.character, level: model.level, xp: model.xp, skills: model.skills, equipment: model.equipment, main: model.main, extra: model.extra };
    const payload = buildCharacter(c);
    parseCharacter(payload, isItem, isSkill, ehEquipavel); // confere que o bloco novo é lido de volta
    const pk = mudouConhecimento(s, model) ? know.buildBlock({ ...s.knowledge, entries: model.knowledge }) : null;
    const blocks = s.save.blocks.map((b) => {
      if (b.name === 'player_character') return { ...b, payload, changed: true };
      if (b.name === 'player_knowledge' && pk) return { ...b, payload: pk, changed: true };
      return b;
    });
    const data = await sv2c.build({ header: s.save.header, blocks }, compress);
    await sv2c.parse(data, decompress, []); // valida hashes e estrutura
    // Kiln: write_save copies the save (and the game's own backups) to the editor's backups before it writes; a
    // backup_file call here made a second, identical copy of each
    await invoke('write_save', { path, data: Array.from(data) });
    const fresh = await load(path, true);
    sessions.set(path, fresh);
    const { modified } = await invoke('file_info', { path });
    return { model: publicModel(path, modified, fresh) };
  },
  async returnStorage(path) {
    if (await invoke('game_running')) throw new Error(launcherT('playerStorage.closeGame'));
    const s = await load(path, true); // never use a possibly stale editor session
    if (!s.storageSnapshot?.local) throw new Error(launcherT('playerStorage.localOnly'));
    if (!s.storage?.canReturn) throw new Error(s.storage?.error ?? launcherT('playerStorage.unavailable'));
    const pc = s.save.blocks.find((b) => b.name === 'player_character');
    const c = readExactCharacter(pc.payload, pc.head.readUInt32LE(10));
    const plan = await planStorageReturn(c, JSON.parse(s.storageSnapshot.text), s.storageSnapshot.character, vanillaStackLimit);
    let backup = null;
    if (plan.returned) {
      const payload = writeExactCharacter(plan.character);
      const written = readExactCharacter(payload, pc.head.readUInt32LE(10));
      const nextWitness = await inventoryWitness(written);
      if (nextWitness !== plan.sidecar.mods['kiln-items'].storage.current.witness) throw new Error('Returned inventory witness mismatch');
      const blocks = s.save.blocks.map((b) => b === pc ? { ...b, payload, changed: true } : b);
      const data = await sv2c.build({ header: s.save.header, blocks }, compress);
      await sv2c.parse(data, decompress, []);
      const result = await wrap(kiln.editorReturnStorage({ path, gameId: s.storageSnapshot.gameId, expectedSave: Array.from(s.bytes),
        expectedSidecar: s.storageSnapshot.text, data: Array.from(data), sidecar: JSON.stringify(plan.sidecar),
        oldWitness: await inventoryWitness(c), newWitness: nextWitness }));
      backup = result.backup;
    }
    const fresh = await load(path, true); sessions.set(path, fresh);
    const { modified } = await invoke('file_info', { path });
    return { model: publicModel(path, modified, fresh), returned: plan.returned, remaining: plan.remaining, modItems: plan.modItems, skipped: plan.skipped.length, backup };
  },
  gameRunning: () => invoke('game_running'),
  listBackups: (target) => invoke('list_backups', { target }),
  restoreBackup: (src, target) => invoke('restore_backup', { src, target }).then(() => { sessions.delete(target); return true; }),
  openBackups: () => invoke('open_backups'),
};
