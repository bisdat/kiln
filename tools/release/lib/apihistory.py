#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The API history ledger: for every public API entry, the first release that shipped it and the mod API version of that
release. Kept in managed/api-history.tsv; called by tools/release/api-gate.sh (`history`, and `promote` for the append).
Readers: the agent API index and the "Since mod API 0.2.x" lines of the API reference (tools/docs-site).

The file is tab-separated with a header line and Unix newlines, one row per entry, sorted by kind, project, entry:

  kind      managed | native
  project   managed: the assembly's name (its folder's name under managed/), e.g. Kiln.Blocks
            native:  the header, relative to native/core/abi, e.g. services/kiln_stations.h
  entry     managed: the exact line of the PublicAPI.Shipped.txt / PublicAPI.Unshipped.txt record, experimental lines
                     (`[KILNX001]...`) and forwarded lines (`... (forwarded, contained in Kiln.Api)`) as they are
            native:  <header>:<Table>:<row>, a function-pointer row of a Kiln*Api table struct of native/core/abi/kiln_api.h
                     or native/core/abi/services/*.h, e.g. services/kiln_stations.h:KilnStationsApi:addSource
  release   the first release whose record has the entry, e.g. 0.2.0-dev.30.5 (a v* tag without -beta.)
  mod_api   Kiln.Mods.ApiVersion of that release, e.g. 0.2.14

`<=0.2.0-dev.26` (and `<=0.2.1`) means "that release or an earlier one": a record starts at the first release tag that has
it, and what stood in it at that point may be older. For managed entries that is the first tag with PublicAPI files
(0.2.0-dev.26 today; a project whose record begins later, with its folder already there, starts at its own first tag), for
native rows the first release tag (0.2.0-dev.10 today, which has the headers). Entries
still in PublicAPI.Unshipped.txt are not in the ledger, they get their release when `promote` moves them. The line
`const Kiln.Mods.ApiVersion = "X.Y.Z" -> string!` is not in the ledger: it carries the current API version and changes
with every bump, so it has no first release of its own.

What counts as shipped at a tag is api-gate's rule (apigate.shipped_at): the lines of Shipped.txt, plus the additions in
Unshipped.txt, minus its `*REMOVED*<line>` entries. A line that was shipped and later removed (made experimental, forwarded)
keeps its row: the ledger is the history, not the current surface.

  seed      the whole ledger from the release tags (about 5 s); rows of a release that has no tag yet (a promote
            waiting for its tag) are kept
  check     the ledger against the tags and the working tree's record: no missing, extra or wrong row, the canonical
            format, and every line of every Shipped.txt has a row
  append    one release's rows: the lines that are in a Shipped.txt but have no row, and the native rows that have none
            (promote does this with the lines it moved; running it by hand repairs a promote that did not)

Native rows have no Unshipped file: the headers of the released commit are the record, so a row not in the ledger yet
belongs to the release that is being promoted. Library module: apigate.py reads and writes the files; the trees it passes
are its GitTree / WorkTree (files(), read(path)).
"""
import re
import subprocess

LEDGER = "managed/api-history.tsv"
COLUMNS = ("kind", "project", "entry", "release", "mod_api")
SHIPPED = "PublicAPI.Shipped.txt"
UNSHIPPED = "PublicAPI.Unshipped.txt"
NULLABLE = "#nullable enable"
REMOVED = "*REMOVED*"
MODS_CS = "managed/Kiln.Api/Mods.cs"
API_RE = re.compile(r'ApiVersion = "([^"]+)"')                      # as apigate.API_RE
CARRIER_RE = re.compile(r'const Kiln\.Mods\.ApiVersion = "[^"]*" -> string!')  # as apigate.CONST_RECORD_RE
ABI_DIR = "native/core/abi"
# native.py of the docs audit: a service table is `typedef struct Kiln<Name>Api[V<n>] { ... } Kiln<Name>Api[V<n>];`
TABLE_RE = re.compile(r"typedef\s+struct\s+(Kiln\w*Api(?:V\d+)?)\s*\{(.*?)\}\s*\1\s*;", re.S)
ROW_RE = re.compile(r"KILN_CALL\s*\*\s*(\w+)\s*\)")
C_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# a release: v<X.Y.Z> or v<X.Y.Z>-dev.<N>[.<M>...]; a beta (-beta.N) and the channel tags (updates-dev) are none
TAG_RE = re.compile(r"v(\d+)\.(\d+)\.(\d+)(?:-dev\.(\d+(?:\.\d+)*))?")
RELEASE_RE = re.compile(r"(\d+)\.(\d+)\.(\d+)(?:-dev\.(\d+(?:\.\d+)*))?")
VERSION_RE = re.compile(r"\d+\.\d+\.\d+")
SHOW_MAX = 20


class HistoryError(Exception):
    """A revision, a file or the ledger that cannot be read: apigate exits 2."""


def git(repo, *args, binary=False, stdin=None):
    r = subprocess.run(["git", "-C", repo, *args], input=stdin, capture_output=True)
    if r.returncode != 0:
        raise HistoryError(f"git {' '.join(args)}: {r.stderr.decode(errors='replace').strip()}")
    return r.stdout if binary else r.stdout.decode("utf-8")


def ordinal(text):
    """Ordinal order of .NET strings (UTF-16 code units), the order of the PublicAPI files."""
    return text.encode("utf-16-be")


def sort_key(row):
    return (row[0], row[1], ordinal(row[2]))


# ---------------------------------------------------------------- releases


def release_key(release):
    """Sort key of a release name: semver order, a pre-release (-dev.N) before its stable version, dev numbers as numbers
    (0.2.0-dev.9 < 0.2.0-dev.10 < 0.2.0-dev.29.1 < 0.2.0)."""
    m = RELEASE_RE.fullmatch(release)
    if not m:
        raise HistoryError(f"not a release name (X.Y.Z or X.Y.Z-dev.N[.M]): {release}")
    dev = tuple(int(x) for x in m.group(4).split(".")) if m.group(4) else None
    return (tuple(int(x) for x in m.groups()[:3]), 1 if dev is None else 0, dev or ())


def bare(release):
    """A release or API version without its `<=` (at most that one)."""
    return release[2:] if release.startswith("<=") else release


def release_name(text):
    """`v0.2.0-dev.30.5` or `0.2.0-dev.30.5` -> `0.2.0-dev.30.5`; HistoryError when it is no release."""
    name = text[1:] if text.startswith("v") else text
    release_key(name)
    return name


def release_tags(repo, points_at=None):
    """[(tag, release)] of the release tags, oldest first (all tags, or those on the commit points_at)."""
    out = git(repo, "tag", "-l", "v*", *(["--points-at", points_at] if points_at else []))
    items = [(release_key(t[1:]), t, t[1:]) for t in out.split() if TAG_RE.fullmatch(t)]
    items.sort()
    return [(t, r) for _, t, r in items]


# ---------------------------------------------------------------- the files of a revision


class Blobs:
    """Blob contents of one repository, read in batches (git cat-file --batch) and kept."""

    def __init__(self, repo):
        self.repo = repo
        self.text = {}

    def prefetch(self, shas):
        want = sorted(set(shas) - set(self.text))
        if not want:
            return
        out = git(self.repo, "cat-file", "--batch", binary=True, stdin=("\n".join(want) + "\n").encode())
        pos = 0
        for sha in want:
            end = out.index(b"\n", pos)
            head = out[pos:end].split()
            if len(head) != 3 or head[1] != b"blob":
                raise HistoryError(f"git cat-file: unexpected answer for {sha}: {out[pos:end].decode(errors='replace')}")
            size = int(head[2])
            self.text[sha] = out[end + 1:end + 1 + size].decode("utf-8-sig")
            pos = end + 1 + size + 1

    def get(self, sha):
        if sha not in self.text:
            self.prefetch([sha])
        return self.text[sha]


class GitSnap:
    """The managed/ and native/core/abi files of a revision, as apigate's GitTree (files(), read(path)), with the blobs read
    in one batch."""

    def __init__(self, blobs, rev):
        self.blobs = blobs
        self.label = rev
        out = git(blobs.repo, "ls-tree", "-r", "-z", rev, "--", "managed", ABI_DIR, binary=True)
        self.sha = {}
        for rec in out.split(b"\0"):
            if rec:
                meta, path = rec.split(b"\t", 1)
                self.sha[path.decode("utf-8")] = meta.split()[2].decode()
        self._files = sorted(self.sha)

    def files(self):
        return self._files

    def read(self, path):
        sha = self.sha.get(path)
        return None if sha is None else self.blobs.get(sha)

    def inputs(self):
        return [self.sha[p] for p in self._files if is_input(p)]


def is_header(path):
    if path == f"{ABI_DIR}/kiln_api.h":
        return True
    rest = path[len(ABI_DIR + "/services/"):]
    return path.startswith(f"{ABI_DIR}/services/") and path.endswith(".h") and "/" not in rest


def is_record(path):
    return path.startswith("managed/") and path.rsplit("/", 1)[-1] in (SHIPPED, UNSHIPPED)


def is_input(path):
    return is_record(path) or path == MODS_CS or is_header(path)


def lines(text):
    """The entries of a PublicAPI file: no blank lines, no #nullable enable."""
    return [l.rstrip("\r") for l in (text or "").split("\n") if l.strip() and l.rstrip("\r") != NULLABLE]


def projects(t):
    """{project folder: its shipped lines} of the tree t: Shipped.txt + Unshipped.txt additions - *REMOVED* entries."""
    out = {}
    for p in t.files():
        if p.startswith("managed/") and p.rsplit("/", 1)[-1] == SHIPPED:
            proj = p.rsplit("/", 1)[0]
            have = set(lines(t.read(p)))
            for l in lines(t.read(f"{proj}/{UNSHIPPED}")):
                if l.startswith(REMOVED):
                    have.discard(l[len(REMOVED):])
                else:
                    have.add(l)
            out[proj] = {l for l in have if not CARRIER_RE.fullmatch(l)}
    return out


def project_names(folders):
    names = {}
    for f in sorted(folders):
        n = f.rsplit("/", 1)[-1]
        if n in names:
            raise HistoryError(f"two gated projects are called {n}: {names[n]} and {f}")
        names[n] = f
    return names


def native_rows(t):
    """{(header, table, row)} of the service tables of the tree t (the header relative to native/core/abi)."""
    rows = set()
    for p in t.files():
        if is_header(p):
            text = C_COMMENT_RE.sub(" ", t.read(p) or "")
            for m in TABLE_RE.finditer(text):
                for r in ROW_RE.finditer(m.group(2)):
                    rows.add((p[len(ABI_DIR) + 1:], m.group(1), r.group(1)))
    return rows


def native_entry(row):
    return f"{row[0]}:{row[1]}:{row[2]}"


def mod_api(t):
    text = t.read(MODS_CS)
    m = API_RE.search(text) if text is not None else None
    return m.group(1) if m else None


# ---------------------------------------------------------------- the ledger file


def render(rows):
    """The ledger's text: the header, the rows sorted, Unix newlines. Rows are (kind, project, entry, release, mod_api)."""
    out = ["\t".join(COLUMNS)]
    for row in sorted(rows, key=sort_key):
        for cell in row:
            if "\t" in cell or "\n" in cell or "\r" in cell:
                raise HistoryError(f"a tab or a newline in a ledger cell: {row!r}")
        out.append("\t".join(row))
    return "\n".join(out) + "\n"


def parse(text):
    """-> (rows, problems): the rows of a ledger text as (kind, project, entry, release, mod_api), and what is wrong with
    its format (the header, the cells, a repeated key, the order, the newlines)."""
    problems, rows, seen = [], [], {}
    if "\r" in text:
        problems.append("it has carriage returns: Unix newlines only")
    body = text.split("\n")
    if body and body[-1] == "":
        body.pop()
    else:
        problems.append("it does not end with a newline")
    if not body or body[0] != "\t".join(COLUMNS):
        problems.append(f"line 1 is not the header: {chr(9).join(COLUMNS)}")
    for n, l in enumerate(body[1:], 2):
        cells = l.split("\t")
        if len(cells) != len(COLUMNS) or not all(cells):
            problems.append(f"line {n}: {len(COLUMNS)} filled cells, tab-separated, expected: {l[:100]}")
            continue
        if cells[0] not in ("managed", "native"):
            problems.append(f"line {n}: kind is managed or native, not {cells[0]}")
            continue
        for what, cell, rx in (("release", cells[3], RELEASE_RE), ("mod_api", cells[4], VERSION_RE)):
            if not rx.fullmatch(bare(cell)):
                problems.append(f"line {n}: {what} is not a version (or <=version): {cell}")
        key = tuple(cells[:3])
        if key in seen:
            problems.append(f"line {n}: {cells[2][:80]} is a repeat of line {seen[key]}")
            continue
        seen[key] = n
        rows.append(tuple(cells))
    if not problems and render(rows) != text:
        for n, (a, b) in enumerate(zip(render(rows).split("\n"), text.split("\n")), 1):
            if a != b:
                problems.append(f"line {n}: the rows are not sorted by kind, project and entry (ordinal): {b[:100]}")
                break
    return rows, problems


def read_ledger(t):
    """(rows, problems) of the ledger in the tree t, or None when it has none."""
    text = t.read(LEDGER)
    return None if text is None else parse(text)


# ---------------------------------------------------------------- the history from the tags


def build(repo):
    """{(kind, project, entry): (release, mod_api)} from the release tags (see the module doc), and the release tags.
    A record begins at the first release tag that has it: a project's PublicAPI.Shipped.txt (when its folder was there at
    the tag before, what stands in it is older than that: `<=`), the native headers (those at the repository's first release
    tag are `<=`: nothing says when they began)."""
    tags = release_tags(repo)
    if not tags:
        raise HistoryError("no release tag (v0.2.0-dev.N) in this repository: fetch the tags (git fetch --tags)")
    blobs = Blobs(repo)
    snaps = [GitSnap(blobs, t) for t, _ in tags]
    managed_from = next((i for i, s in enumerate(snaps) if any(p.endswith("/" + SHIPPED) and p.startswith("managed/")
                                                              for p in s.files())), None)
    native_from = next((i for i, s in enumerate(snaps) if any(is_header(p) for p in s.files())), None)
    starts = [i for i in (managed_from, native_from) if i is not None]
    if not starts:
        return {}, tags
    blobs.prefetch(sha for s in snaps[min(starts):] for sha in s.inputs())
    rows, gated = {}, set()
    for i in range(min(starts), len(tags)):
        snap, release = snaps[i], tags[i][1]
        prev = snaps[i - 1] if i else None
        api = mod_api(snap)
        if api is None:
            raise HistoryError(f"no ApiVersion = \"...\" in {MODS_CS} at {tags[i][0]}")
        if managed_from is not None and i >= managed_from:
            shipped = projects(snap)
            for name, folder in project_names(shipped).items():
                # the record of a project begins at its first release with a Shipped.txt: what is in it then may be older
                # when the project's files were there before
                begins = folder not in gated and (prev is None or any(p.startswith(folder + "/") for p in prev.files()))
                for line in shipped[folder]:
                    rows.setdefault(("managed", name, line),
                                    ("<=" + release, "<=" + api) if begins else (release, api))
            gated.update(shipped)
        if native_from is not None and i >= native_from:
            for row in native_rows(snap):
                rows.setdefault(("native", row[0], native_entry(row)),
                                ("<=" + release, "<=" + api) if prev is None else (release, api))
    return rows, tags


def seed(repo, wt):
    """(text, counts) of a new ledger: the history of the tags plus the rows of releases that have no tag yet."""
    rows, tags = build(repo)
    names = {r for _, r in tags}
    kept = 0
    old = read_ledger(wt)
    if old:
        for row in old[0]:
            if row[3] not in names and not row[3].startswith("<=") and tuple(row[:3]) not in rows:
                rows[tuple(row[:3])] = (row[3], row[4])
                kept += 1
    text = render([(*k, *v) for k, v in rows.items()])
    return text, {"managed": sum(k[0] == "managed" for k in rows), "native": sum(k[0] == "native" for k in rows),
                  "tags": len(tags), "kept": kept}


# ---------------------------------------------------------------- check


def check(repo, wt):
    """(problems, notes, counts): the ledger of the tree wt against the tags and against wt's record."""
    problems, notes = [], []
    led = read_ledger(wt)
    if led is None:
        return [f"{LEDGER} does not exist: tools/release/api-gate.sh history --seed"], notes, {}
    ledger, bad = parse_rows(led)
    problems += [f"{LEDGER}: {b}" for b in bad]
    expected, tags = build(repo)
    names = {r for _, r in tags}
    last = release_key(tags[-1][1])

    def label(key):
        return f"{key[0]} {key[1]}: {key[2][:120]}"

    def ordered(rows):
        return sorted(rows.items(), key=lambda kv: sort_key((*kv[0], *kv[1])))

    flight = {}
    for key, (release, api) in ordered(ledger):
        if bare(release) in names:
            want = expected.get(key)
            if want is None:
                problems.append(f"extra: {label(key)}: the tag of {bare(release)} does not have it")
            elif want != (release, api):
                problems.append(f"wrong: {label(key)}: the tags say {want[0]} / {want[1]}, the ledger {release} / {api}")
        elif release.startswith("<="):
            problems.append(f"extra: {label(key)}: {release} is no release tag's name")
        else:
            flight[key] = (release, api)
    for key, (release, api) in ordered(expected):
        if key not in ledger:
            problems.append(f"missing: {label(key)} ({release} per the tags)")
    # every line of every Shipped.txt has a row (the Unshipped.txt lines are not shipped yet)
    shipped = {}
    for p in wt.files():
        if p.startswith("managed/") and p.rsplit("/", 1)[-1] == SHIPPED:
            shipped[p.rsplit("/", 1)[0]] = {l for l in lines(wt.read(p)) if not CARRIER_RE.fullmatch(l)}
    folders = project_names(shipped)
    for name, folder in sorted(folders.items()):
        for l in sorted(shipped[folder], key=ordinal):
            if ("managed", name, l) not in ledger:
                problems.append(f"unrecorded: {label(('managed', name, l))}: in {folder}/{SHIPPED}, no ledger row (promote "
                                f"appends it; or api-gate.sh history --append --release R)")
    # rows of a release without a tag: promoted, waiting for its tag; they must be in the record they came from
    headers = native_rows(wt)
    for key, (release, api) in ordered(flight):
        if key in expected:
            problems.append(f"wrong: {label(key)}: the tags say {expected[key][0]}, the ledger {release}")
        elif release_key(release) < last:
            problems.append(f"extra: {label(key)}: {release} is before the last tag and has none of its own")
        elif key[0] == "managed" and key[2] not in shipped.get(folders.get(key[1], ""), ()):
            problems.append(f"extra: {label(key)}: not in the Shipped.txt of {key[1]} ({release} has no tag yet)")
        elif key[0] == "native" and tuple(key[2].split(":")) not in headers:
            problems.append(f"extra: {label(key)}: not in the headers ({release} has no tag yet)")
    if flight:
        notes.append(f"{len(flight)} rows are for {', '.join(sorted({r for r, _ in flight.values()}))}, which has no tag "
                     f"yet (promoted, waiting for its tag)")
    counts = {"managed": sum(k[0] == "managed" for k in ledger), "native": sum(k[0] == "native" for k in ledger),
              "tags": len(tags)}
    return problems, notes, counts


def parse_rows(parsed):
    """({key: (release, mod_api)}, problems) from parse()'s result."""
    rows, problems = parsed
    return {tuple(r[:3]): (r[3], r[4]) for r in rows}, problems


# ---------------------------------------------------------------- append


def fresh(text, managed, native):
    """([(project name, line)], [(header, table, row)]): what of managed and native the ledger text has no row for."""
    have = {tuple(r[:3]) for r in parse(text)[0]}
    return ([(n, l) for n, l in sorted(set(managed), key=lambda x: (x[0], ordinal(x[1])))
             if ("managed", n, l) not in have and not CARRIER_RE.fullmatch(l)],
            sorted(r for r in set(native) if ("native", r[0], native_entry(r)) not in have))


def appended(text, release, api, managed, native):
    """(new text, n managed, n native): the ledger text with a release's rows. managed: [(project name, line)], native:
    {(header, table, row)}; an entry that has a row is left as it is."""
    rows, problems = parse(text)
    if problems:
        raise HistoryError(f"{LEDGER} is not sound ({problems[0]}): api-gate.sh history --check")
    release = release_name(release)
    if not VERSION_RE.fullmatch(api or ""):
        raise HistoryError(f"the mod API version of {release} is not X.Y.Z: {api}")
    new_m, new_n = fresh(text, managed, native)
    add = [("managed", n, l, release, api) for n, l in new_m] + \
          [("native", r[0], native_entry(r), release, api) for r in new_n]
    return (render(rows + add) if add else text), len(new_m), len(new_n)


def missing_rows(wt):
    """([(project name, line)], [native rows]) of the tree wt that the ledger has no row for: what append adds."""
    text = wt.read(LEDGER)
    if text is None:
        raise HistoryError(f"{LEDGER} does not exist: tools/release/api-gate.sh history --seed")
    record = {}
    for p in wt.files():
        if p.startswith("managed/") and p.rsplit("/", 1)[-1] == SHIPPED:
            record[p.rsplit("/", 1)[0]] = lines(wt.read(p))
    return fresh(text, [(n, l) for n, f in project_names(record).items() for l in record[f]], native_rows(wt))


def release_at(repo, rev):
    """The release whose tag points at the commit rev, or None (more than one release tag there: HistoryError)."""
    tags = release_tags(repo, points_at=rev)
    if len(tags) > 1:
        raise HistoryError(f"{len(tags)} release tags point at {rev} ({', '.join(t for t, _ in tags)}): give --release")
    return tags[0][1] if tags else None


def describe(problems, limit=SHOW_MAX * 5):
    shown = problems[:limit]
    return shown + ([f"... {len(problems) - limit} more"] if len(problems) > limit else [])
