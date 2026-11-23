// Kiln's API reference (tools/docs-site/template, MIT): the navigation, "On this page", the search, code colours.
// No dependencies. The navigation comes from static/nav.<hash>.json (theme.py: one file for every page, cached), a group's
// types from api/toc/<assembly>.json (services.py) when it opens (the page's own group at once; the layout preloads both),
// the search index (search/pages.json, search/members.json: search_index.py) the first time the search box is used.
(() => {
  'use strict'
  const d = document
  const meta = n => (d.querySelector(`meta[name="${n}"]`) || {}).content || ''
  const root = new URL(meta('kiln:rel') || './', location.href)
  const url = p => new URL(p, root).href
  const norm = h => { h = h.split('#')[0].split('?')[0]; return h.endsWith('/') ? h + 'index.html' : h }
  const here = norm(location.href)
  const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])
  const json = p => fetch(url(p)).then(r => { if (!r.ok) throw new Error(`${r.status} ${p}`); return r.json() })
  const hid = s => s.replace(/\W/g, '_')
  const idle = window.requestIdleCallback || (f => setTimeout(f, 1))
  const phone = matchMedia('(max-width: 760px)')
  const TYPE = { c: 'class', s: 'struct', e: 'enum', i: 'interface', d: 'delegate' }
  const KIND = { ...TYPE, m: 'method', p: 'property', f: 'field', v: 'event', k: 'constructor', o: 'operator', g: 'guide', h: 'section', a: 'assembly', o_: 'overview' }

  // ---- Navigation -------------------------------------------------------------------------------------------------------
  const toc = d.getElementById('toc')
  const navbox = d.querySelector('[data-kd-nav]')
  const FOLD = 25   // a run of at least this many types sharing a prefix ("Ecs.Position", "Ecs.Health") folds into one entry
  const groups = new Map()

  function markCurrent(scope) {
    let found = null
    for (const a of scope.querySelectorAll('a[href]')) {
      if (norm(a.href) === here) { a.setAttribute('aria-current', 'page'); found = found || a }
    }
    return found
  }

  function typeItem(t, label) {
    return `<li><a href="${esc(url('api/' + t[1]))}"><span class="kd-t" title="${TYPE[t[2]] || ''}">${t[2].toUpperCase()}</span>${esc(label)}</a></li>`
  }

  // rows: [[label, type]], the label relative to the list (a fold's entries drop its prefix). Runs of FOLD or more labels
  // with the same first segment fold into one entry, drawn when it opens, folding again inside (Ecs.* -> Programs.*).
  function renderTypes(li, rows) {
    const count = new Map()
    for (const [label] of rows) { const i = label.indexOf('.'); if (i > 0) count.set(label.slice(0, i), (count.get(label.slice(0, i)) || 0) + 1) }
    const folded = new Map()
    const items = []
    for (const row of rows) {
      const i = row[0].indexOf('.')
      const p = i > 0 ? row[0].slice(0, i) : ''
      if (p && count.get(p) >= FOLD) {
        if (!folded.has(p)) { folded.set(p, []); items.push({ fold: p }) }
        folded.get(p).push([row[0].slice(i + 1), row[1]])
      } else items.push(row)
    }
    const owner = new Map(rows.filter(r => folded.has(r[0])).map(r => [r[0], r[1]]))
    const ul = d.createElement('ul')
    ul.innerHTML = items.map(r => r.fold
      ? `<li class="kd-asm" data-fold="${esc(r.fold)}"><button type="button" class="kd-exp" aria-expanded="false" aria-label="Types in ${esc(r.fold)}"></button>` +
        `<a href="${esc(url('api/' + (owner.get(r.fold) || folded.get(r.fold)[0][1])[1]))}">${esc(r.fold)}.*</a><span class="kd-n">${folded.get(r.fold).length}</span></li>`
      : typeItem(r[1], r[0])).join('')
    for (const f of ul.querySelectorAll(':scope > [data-fold]')) {
      const list = folded.get(f.dataset.fold)
      f.kdFill = () => renderTypes(f, list)
      f.kdHas = list.some(r => norm(url('api/' + r[1][1])) === here)
    }
    li.append(ul)
  }

  function open(li, on) {
    const btn = li.querySelector(':scope > .kd-exp')
    if (btn) btn.setAttribute('aria-expanded', on)
    let ul = li.querySelector(':scope > ul')
    if (on && !ul && li.kdFill) { li.kdFill(); li.kdFill = null; ul = li.querySelector(':scope > ul') }
    if (on && !ul && li.dataset.asm) {
      const asm = li.dataset.asm
      if (!groups.has(asm)) groups.set(asm, json(`api/toc/${encodeURIComponent(asm)}.json`))
      const wait = d.createElement('div')
      wait.className = 'kd-loading'
      wait.textContent = 'Loading…'
      const timer = setTimeout(() => li.append(wait), 120)
      return groups.get(asm).then(doc => {
        clearTimeout(timer); wait.remove()
        if (!li.querySelector(':scope > ul')) renderTypes(li, doc.types.map(t => [t[0], t]))
        if (btn && btn.getAttribute('aria-expanded') !== 'true') li.querySelector(':scope > ul').hidden = true
        return li
      }, e => { clearTimeout(timer); wait.textContent = 'Could not load the list.'; li.append(wait); groups.delete(asm); console.error(e) })
    }
    if (ul) ul.hidden = !on
    return Promise.resolve(li)
  }

  function reveal(a) {
    if (!a || !navbox || phone.matches) return
    const box = navbox.getBoundingClientRect(), r = a.getBoundingClientRect()
    if (r.top < box.top || r.bottom > box.bottom) navbox.scrollTop += r.top - box.top - box.height / 3
  }

  function drawNav(nav) {
    const link = (name, href) => `<li><a href="${esc(url(href))}">${esc(name)}</a></li>`
    let h = '<h2>API reference</h2><ul class="kd-list">' + link('Overview', 'api/index.html')
    for (const [name, href, n] of nav.groups) {
      h += `<li class="kd-asm" data-asm="${esc(name)}"><button type="button" class="kd-exp" aria-expanded="false" aria-label="Types of ${esc(name)}"></button>` +
        `<a href="${esc(url(href))}">${esc(name)}</a><span class="kd-n">${n}</span></li>`
    }
    h += '</ul><h2>Guide</h2><ul class="kd-list">' + nav.guides.map(g => link(g[0], g[1])).join('')
    if (nav.schemas.length) {
      h += `<li class="kd-asm"><button type="button" class="kd-exp" aria-expanded="false" aria-label="File formats"></button><a href="${esc(url(nav.schemas[0][1]))}">File formats</a>` +
        `<span class="kd-n">${nav.schemas.length}</span><ul hidden>${nav.schemas.map(g => link(g[0], g[1])).join('')}</ul></li>`
    }
    toc.innerHTML = h + '</ul>'
    const fallback = d.querySelector('.kd-fallback')
    if (fallback) fallback.remove()
  }

  if (toc) {
    const asm = meta('kiln:asm')
    if (asm) groups.set(asm, json(`api/toc/${encodeURIComponent(asm)}.json`))     // with the navigation, not after it
    fetch(new URL(meta('kiln:nav'), location.href)).then(r => { if (!r.ok) throw new Error(`${r.status} navigation`); return r.json() }).then(nav => {
      drawNav(nav)
      toc.addEventListener('click', e => {
        const btn = e.target.closest('.kd-exp')
        if (btn) open(btn.parentElement, btn.getAttribute('aria-expanded') !== 'true')
      })
      let current = markCurrent(toc)
      for (let li = current && current.closest('.kd-asm'); li; li = li.parentElement.closest('.kd-asm')) open(li, true)
      const group = asm && toc.querySelector(`.kd-asm[data-asm="${CSS.escape(asm)}"]`)
      if (group) {
        open(group, true).then(li => {
          // open the folds down to the page's own type (Ecs.* -> Programs.*)
          for (let list = li, fold; (fold = [...list.querySelectorAll(':scope > ul > [data-fold]')].find(f => f.kdHas)); list = fold) open(fold, true)
          current = markCurrent(li) || current
          reveal(current)
        })
      } else reveal(current)
    }).catch(e => console.error(e))
  }
  if (navbox) {
    const fit = () => { navbox.open = !phone.matches }
    fit()
    phone.addEventListener('change', fit)
  }

  // ---- The page ---------------------------------------------------------------------------------------------------------
  const article = d.querySelector('article')
  const api = d.body.dataset.yamlMime === 'ManagedReference'

  // annotate.py's lines ("Since mod API 0.2.12 (0.2.0-dev.30)", "Thread: tick thread only · Capability: kiln.build", "Side:
  // server only") as chips
  const CHIP = [[/^Since mod API\b/, 'since'], [/^Thread:/, 'thread'], [/^Side:/, 'side'], [/^Capabilit(y|ies):/, 'cap']]
  function chips() {
    for (const p of article.querySelectorAll('.summary > p, .remarks > p, .summary p, .remarks p')) {
      if (p.classList.contains('kd-meta')) continue
      const text = p.textContent.trim()
      if (!CHIP.some(([re]) => re.test(text))) continue
      const parts = p.innerHTML.split(/\s+·\s+/)
      const html = parts.map(part => {
        const plain = part.replace(/<[^>]+>/g, '').trim()
        const c = (CHIP.find(([re]) => re.test(plain)) || [, 'other'])[1]
        return `<span class="kd-chip" data-c="${c}">${part.trim()}</span>`
      }).join('')
      const prev = p.previousElementSibling
      if (prev && prev.classList.contains('kd-meta')) { prev.insertAdjacentHTML('beforeend', html); p.remove() }
      else { p.className = 'kd-meta'; p.innerHTML = html }
    }
  }

  // "On this page": the page's sections; on an API page, the members under them
  function outline() {
    const affix = d.getElementById('affix')
    if (!affix) return
    const heads = [...article.querySelectorAll(api ? 'h2[id], h3[id]' : 'h2[id]')]
    if (heads.length < 2) return
    let html = '<h2>On this page</h2><ol>', item = false, inner = false
    for (const h of heads) {
      const link = `<a href="#${esc(h.id)}">${esc(h.textContent.trim())}</a>`
      if (h.tagName === 'H3') {
        if (!item) { html += '<li>'; item = true }
        if (!inner) { html += '<ol>'; inner = true }
        html += `<li>${link}</li>`
        continue
      }
      if (inner) { html += '</ol>'; inner = false }
      html += `${item ? '</li>' : ''}<li>${link}`
      item = true
    }
    affix.innerHTML = html + (inner ? '</ol>' : '') + (item ? '</li>' : '') + '</ol>'
    if (!('IntersectionObserver' in window)) return
    const links = new Map([...affix.querySelectorAll('a')].map(a => [a.hash.slice(1), a]))
    let lit = null
    const io = new IntersectionObserver(entries => {
      for (const e of entries) {
        if (!e.isIntersecting) continue
        const a = links.get(e.target.id)
        if (!a || a === lit) continue
        if (lit) lit.removeAttribute('aria-current')
        a.setAttribute('aria-current', 'true')
        lit = a
      }
    }, { rootMargin: '0px 0px -70% 0px' })
    heads.forEach(h => io.observe(h))
  }

  // code colours: a small tokenizer per language family, run when the browser is idle
  const KW = {
    cs: 'abstract as async await base bool break byte case catch char checked class const continue decimal default delegate do double else enum event explicit extern false finally fixed float for foreach get goto if implicit in init int interface internal is lock long namespace new null object operator out override params partial private protected public readonly record ref required return sbyte scoped sealed set short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked unmanaged unsafe ushort using value var virtual void volatile when where while with yield nint nuint dynamic global file',
    c: 'auto break case char const continue default do double else enum extern float for goto if inline int long register return short signed sizeof static struct switch typedef union unsigned void volatile while bool true false nullptr uint float2 float3 float4 float4x4 half cbuffer Texture2D SamplerState return in out inout',
    json: 'true false null',
    sh: 'if then else elif fi for in do done while case esac function return export local set unset echo cd source sudo',
    py: 'and as assert break class continue def del elif else except False finally for from global if import in is lambda None nonlocal not or pass raise return True try while with yield',
  }
  const KWS = Object.fromEntries(Object.entries(KW).map(([k, v]) => [k, new Set(v.split(' '))]))
  const LANG = { csharp: 'cs', cs: 'cs', 'c#': 'cs', c: 'c', cpp: 'c', hlsl: 'c', js: 'c', javascript: 'c', ts: 'c', json: 'json', jsonc: 'json', sh: 'sh', bash: 'sh', shell: 'sh', console: 'sh', powershell: 'sh', ps1: 'sh', python: 'py', py: 'py', ini: 'ini', toml: 'ini', xml: 'xml', html: 'xml', csproj: 'xml' }
  const RE = {
    cs: /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|(\$?@"(?:[^"]|"")*"|\$?"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+')|(\b0x[\da-fA-F_]+\b|\b\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?[fFdDmMuUlL]*\b)|(\[[A-Z]\w*(?=[(\]]))|(\b[A-Za-z_]\w*\b)/g,
    c: /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])+')|(\b0x[\da-fA-F]+\b|\b\d+(?:\.\d+)?[fFuUlL]*\b)|(#\s*\w+)|(\b[A-Za-z_]\w*\b)/g,
    json: /(\/\/[^\n]*|\/\*[\s\S]*?\*\/)|("(?:\\.|[^"\\\n])*"(?=\s*:))|("(?:\\.|[^"\\\n])*")|(-?\b\d+(?:\.\d+)?(?:[eE][+-]?\d+)?\b)|(\b[a-z]+\b)/g,
    sh: /((?:^|\s)#[^\n]*)|("(?:\\.|[^"\\])*"|'[^']*')|(\$\{?\w+\}?)|(\s--?[\w-]+)|(\b[A-Za-z_][\w-]*\b)/g,
    py: /(#[^\n]*)|("""[\s\S]*?"""|'''[\s\S]*?'''|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*')|(\b\d+(?:\.\d+)?\b)|(@\w+)|(\b[A-Za-z_]\w*\b)/g,
    ini: /((?:^|\n)\s*[;#][^\n]*)|("(?:\\.|[^"\\\n])*"|'[^'\n]*')|(\b\d+(?:\.\d+)?\b)|((?:^|\n)\s*\[[^\]\n]+\])|((?:^|\n)\s*[\w.-]+(?=\s*=))/g,
    xml: /(<!--[\s\S]*?-->)|("[^"]*"|'[^']*')|(&\w+;)|(<\/?[\w:.-]+|\/?>)|(\b[\w:.-]+(?==))/g,
  }
  // the class of each capture group of RE[lang]; null: an identifier (a keyword, or in C# a capitalised name: a type or method)
  const CLS = { cs: ['c', 's', 'n', 'a', null], c: ['c', 's', 'n', 'k', null], json: ['c', 'n', 's', 'n', null], sh: ['c', 's', 'n', 't', null],
    py: ['c', 's', 'n', 'a', null], ini: ['c', 's', 'n', 'k', 't'], xml: ['c', 's', 'n', 'a', 't'] }
  function colour(code, lang) {
    const re = RE[lang], kws = KWS[lang]
    const text = code.textContent
    let out = '', last = 0, m
    re.lastIndex = 0
    while ((m = re.exec(text))) {
      if (m.index > last) out += esc(text.slice(last, m.index))
      const s = esc(m[0])
      const g = m.findIndex((x, j) => j > 0 && x !== undefined)
      let cls = CLS[lang][g - 1]
      if (cls === null) cls = kws && kws.has(m[0]) ? 'k' : lang === 'cs' && /^[A-Z]/.test(m[0]) ? 't' : ''
      out += cls ? `<span class="tk-${cls}">${s}</span>` : s
      last = re.lastIndex
      if (m[0].length === 0) re.lastIndex++
    }
    code.innerHTML = out + esc(text.slice(last))
  }
  function highlight() {
    const blocks = [...article.querySelectorAll('pre > code')]
    let i = 0
    const step = deadline => {
      while (i < blocks.length && (!deadline || deadline.timeRemaining() > 2)) {
        const code = blocks[i++]
        const m = /\blang-([\w#+-]+)/.exec(code.className)
        const lang = m && LANG[m[1].toLowerCase()]
        if (lang && code.textContent.length < 200000) colour(code, lang)
      }
      if (i < blocks.length) idle(step)
    }
    idle(step)
  }
  function copyButtons() {
    if (!navigator.clipboard) return
    article.addEventListener('click', e => {
      const b = e.target.closest('.kd-copy')
      if (!b) return
      navigator.clipboard.writeText(b.parentElement.querySelector('code').textContent).then(() => {
        b.textContent = 'Copied'
        setTimeout(() => { b.textContent = 'Copy' }, 1500)
      })
    })
    for (const pre of article.querySelectorAll('pre')) {
      if (pre.querySelector('code') && !pre.closest('.codewrapper')) pre.insertAdjacentHTML('beforeend', '<button type="button" class="kd-copy">Copy</button>')
    }
  }

  // a release's own copy (/kiln/downloads/<version>/sdk/api/) says where the newest one is
  function pinned() {
    const p = d.getElementById('kd-pinned')
    if (!p || !/\/kiln\/downloads\/[^/]+\/sdk\/api\//.test(location.pathname)) return
    const v = meta('kiln:release')
    p.innerHTML = `This is the reference of Kiln ${esc(v || 'an earlier release')}, as it shipped. The newest release's reference is at <a href="https://frostdev.io/kiln/api/">frostdev.io/kiln/api</a>.`
    p.hidden = false
  }

  if (article) {
    if (api) chips()
    outline()
    copyButtons()
    highlight()
  }
  pinned()

  // ---- Search -----------------------------------------------------------------------------------------------------------
  const input = d.getElementById('search-query')
  const box = d.getElementById('search-results')
  if (!input || !box) return
  const S = { e: [], ready: 0, error: null, loading: null }
  let shown = [], sel = 0, query = ''

  // an entry: n name, l lower-case name, f lower-case qualified name (build.blockat), k kind, c context, h href, s summary,
  // t lower-case text (context and summary), r rank bonus
  function addPages(doc) {
    S.asm = doc.asm
    S.types = doc.t
    for (const [title, href, k, page, text] of doc.g) {
      S.e.push({ n: title, l: title.toLowerCase(), f: '', k: k === 'o' ? 'o_' : k, c: page, h: href, s: text, t: (page + ' ' + text).toLowerCase(), r: k === 'a' ? 14 : k === 'g' ? 10 : k === 'o' ? 6 : 3 })
    }
    for (const [name, page, k, ai, text, gen] of doc.t) {
      const l = name.toLowerCase()
      S.e.push({ n: name, l, f: l, k, c: doc.asm[ai], h: 'api/' + page, s: text, t: (doc.asm[ai] + ' ' + text).toLowerCase(), r: gen ? -15 : 12 })
    }
  }
  function addMembers(doc) {
    const T = S.types
    for (const [ti, name, k, code, n, text] of doc.m) {
      const t = T[ti], own = hid(t[1].slice(0, -5))
      const id = code === '' ? `${own}_${hid(name)}_` : code === '=' ? `${own}_${hid(name)}` : code[0] === '#' ? code.slice(1) : own + code
      const l = name.toLowerCase()
      S.e.push({ n: name, l, f: t[0].toLowerCase() + '.' + l, k, c: `${t[0]}${n > 1 ? ` · ${n} overloads` : ''}`, h: `api/${t[1]}#${id}`, s: text,
        t: (t[0] + ' ' + text).toLowerCase(), r: t[5] ? -20 : 0 })
    }
  }
  function load() {
    if (S.loading) return S.loading
    const pages = json('search/pages.json').then(doc => { addPages(doc); S.ready |= 1; refresh() })
    S.loading = Promise.all([pages, json('search/members.json')]).then(([, doc]) => { addMembers(doc); S.ready |= 2; refresh() })
      .catch(e => { S.error = e; console.error(e); refresh() })
    return S.loading
  }

  const boundary = (name, i) => i === 0 || /[^A-Za-z0-9]/.test(name[i - 1]) || (/[A-Z]/.test(name[i]) && /[a-z0-9]/.test(name[i - 1]))
  // one query against every entry: exact > prefix > Type.Member suffix > at a word start > inside > in the text. "Kiln.X"
  // is tried as typed (the service page Kiln.Build) and without "Kiln." (Kiln.Build.BlockAt is Build.BlockAt); the better wins.
  function score(e, q) {
    const dotted = q.includes('.')
    const n = dotted && e.f ? e.f : e.l
    if (n === q) return 100
    if (n.startsWith(q)) return 80 - Math.min(20, (n.length - q.length) / 2)
    if (dotted && n.endsWith(q)) return 85
    const i = n.indexOf(q)
    if (i > 0) return (boundary(dotted && e.f ? e.f : e.n, i) ? 60 : 35) - Math.min(10, i / 3)
    return q.length > 2 && e.t.includes(q) ? 8 : -1
  }
  function find(raw) {
    const q = raw.trim().toLowerCase().replace(/\(.*$/, '').trim()
    if (!q) return []
    const short = q.replace(/^kiln\./, '')
    const words = q.split(/\s+/)
    const hits = []
    if (words.length === 1) {
      for (const e of S.e) {
        const s = short === q ? score(e, q) : Math.max(score(e, q), score(e, short))
        if (s >= 0) hits.push([s + e.r, e])
      }
    } else {
      for (const e of S.e) {
        let s = 0
        for (const w of words) {
          if (e.l.includes(w)) s += 14
          else if (e.t.includes(w)) s += 4
          else { s = -1; break }
        }
        if (s >= 0) hits.push([s + e.r, e])
      }
    }
    hits.sort((a, b) => b[0] - a[0] || a[1].n.length - b[1].n.length || (a[1].n < b[1].n ? -1 : a[1].n > b[1].n ? 1 : 0))
    return hits.slice(0, 60).map(h => h[1])
  }
  function mark(name, q) {
    const i = q ? name.toLowerCase().indexOf(q.split(/[\s.]/).pop()) : -1
    if (i < 0 || !q) return esc(name)
    const len = q.split(/[\s.]/).pop().length
    return esc(name.slice(0, i)) + '<mark>' + esc(name.slice(i, i + len)) + '</mark>' + esc(name.slice(i + len))
  }
  function show(on) {
    box.hidden = !on
    input.setAttribute('aria-expanded', on)
  }
  function render() {
    if (!query.trim()) { show(false); return }
    const q = query.trim().toLowerCase()
    let html = shown.map((e, i) => `<a class="sr-item" role="option" id="sr-${i}" aria-selected="${i === sel}" href="${esc(url(e.h))}">` +
      `<span class="sr-kind" data-k="${e.k}">${KIND[e.k] || ''}</span><span class="sr-name">${mark(e.n, q)}${e.c ? ` <span>${esc(e.c)}</span>` : ''}</span>` +
      `<span class="sr-text">${esc(e.s || '')}</span></a>`).join('')
    if (S.error) html += '<p class="sr-note">The search index did not load. Reload the page to try again.</p>'
    else if (!(S.ready & 1)) html += '<p class="sr-note">Loading the index…</p>'
    else if (!(S.ready & 2)) html += '<p class="sr-note">Loading members…</p>'
    else if (!shown.length) html += `<p class="sr-note">Nothing called “${esc(query.trim())}”. Try part of a name, or Type.Member.</p>`
    box.innerHTML = html
    input.setAttribute('aria-activedescendant', shown.length ? `sr-${sel}` : '')
    show(true)
  }
  let frame = 0
  function refresh() {
    if (frame) return
    frame = requestAnimationFrame(() => { frame = 0; shown = S.ready ? find(query) : []; sel = 0; render() })
  }
  function move(by) {
    if (!shown.length) return
    sel = (sel + by + shown.length) % shown.length
    for (const a of box.querySelectorAll('.sr-item')) a.setAttribute('aria-selected', a.id === `sr-${sel}`)
    input.setAttribute('aria-activedescendant', `sr-${sel}`)
    const a = d.getElementById(`sr-${sel}`)
    if (a) a.scrollIntoView({ block: 'nearest' })
  }

  input.addEventListener('focus', () => { load(); if (query.trim()) refresh() })
  input.addEventListener('pointerenter', load, { once: true })
  input.addEventListener('input', () => { query = input.value; load(); refresh() })
  input.addEventListener('keydown', e => {
    if (e.key === 'ArrowDown') { e.preventDefault(); move(1) }
    else if (e.key === 'ArrowUp') { e.preventDefault(); move(-1) }
    else if (e.key === 'Enter') { e.preventDefault(); const a = d.getElementById(`sr-${sel}`); if (a) location.href = a.href }
    else if (e.key === 'Escape') { if (!box.hidden) { e.preventDefault(); show(false) } else input.blur() }
  })
  d.getElementById('search').addEventListener('submit', e => e.preventDefault())
  d.addEventListener('pointerdown', e => { if (!e.target.closest('.kd-search')) show(false) })
  box.addEventListener('click', e => { if (e.target.closest('.sr-item')) show(false) })
  d.addEventListener('keydown', e => {
    const typing = /^(INPUT|TEXTAREA|SELECT)$/.test(e.target.tagName) || e.target.isContentEditable
    if ((e.key === '/' && !typing) || (e.key === 'k' && (e.ctrlKey || e.metaKey))) { e.preventDefault(); input.focus(); input.select() }
  })
  const q = new URLSearchParams(location.search).get('q')
  if (q) { input.value = query = q; load(); refresh() }
})()
