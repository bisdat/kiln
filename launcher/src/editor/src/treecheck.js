// Validação da árvore antes de gravar. Uma árvore que o jogo considera inválida é reembolsada
// inteira ao carregar (aconteceu em 16/10/2026: 34 pontos zerados), então o editor bloqueia:
//  - nó comprado sem caminho até uma raiz (Fagulha) passando só por nós comprados;
//  - nível acima do máximo do nó;
//  - mais pontos gastos que o total informado pelo usuário (quando conhecido).
import { skills, skillName } from './gamedata.js';
import skillPoints from '../data/skill-points.json';
import { t } from './i18n.js';

const adj = new Map();
for (const [a, b] of skills.links) {
  if (!adj.has(a)) adj.set(a, new Set());
  if (!adj.has(b)) adj.set(b, new Set());
  adj.get(a).add(b); adj.get(b).add(a);
}
const roots = Object.entries(skills.nodes).filter(([, n]) => n.type === 'Root').map(([id]) => Number(id));

export function pointsUsed(list) {
  return list.reduce((t, s) => t + (skills.nodes[s.id]?.cost || 0) * s.level, 0);
}

export function totalKey(model) {
  return `ese.totalPoints.${model.path}.${model.level}`;
}
// Kiln: the game stores no point total. It works it out at each load (docs/re/progression.md section 3, SkillPointsTotal):
// 2 per level above 1, plus the amount of every skill-point knowledge (Shroud Roots +1, Elixir Wells +3) whose value is not
// 0. Null without the character's knowledge block: then only a total the player typed is known.
export function gameTotal(model) {
  if (!Array.isArray(model?.knowledge) || !Number.isInteger(model.level) || model.level < 1) return null;
  let n = skillPoints.perLevel * (model.level - 1);
  for (const k of model.knowledge) if (k.value) n += skillPoints.knowledge[k.id] || 0;
  return n;
}
export function knownTotal(model) {
  const derived = gameTotal(model);
  if (derived !== null) return derived;
  try { const v = localStorage.getItem(totalKey(model)); return v === null ? null : Number(v); } catch { return null; }
}

export function checkTree(model) {
  const problems = [];
  const bought = new Set(model.skills.filter((s) => s.level > 0).map((s) => s.id));
  for (const s of model.skills) {
    const n = skills.nodes[s.id];
    if (n && s.level > Math.max(1, n.maxLevel || 1)) {
      problems.push(t('{no}: nível {nivel} acima do máximo {max}', { no: skillName(s.id), nivel: s.level, max: n.maxLevel || 1 }));
    }
  }
  const seen = new Set(roots);
  const queue = [...roots];
  while (queue.length) {
    const u = queue.pop();
    for (const w of adj.get(u) || []) if (bought.has(w) && !seen.has(w)) { seen.add(w); queue.push(w); }
  }
  const orphans = [...bought].filter((id) => !seen.has(id)).map((id) => (skills.nodes[id] ? skillName(id) : id));
  if (orphans.length) problems.push(t('Nós sem ligação com o resto da árvore: {nos}', { nos: orphans.join(', ') }));
  const total = knownTotal(model);
  const used = pointsUsed(model.skills);
  if (total !== null && used > total) problems.push(t('Pontos em uso ({usados}) passam do total do personagem ({total})', { usados: used, total }));
  return { problems, used, total };
}
