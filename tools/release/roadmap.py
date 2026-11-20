#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Kiln's public roadmap (https://frostdev.io/kiln/roadmap) as part of the release flow.

The roadmap's one source is tools/release/roadmap/roadmap.json (its README says how to write it), in the format of the
site's src/data/kilnRoadmap.json, which this script writes byte for byte. Stdlib only; run it with python3 -I.

    roadmap.py [--source FILE] check VERSION [--notes-dir DIR]
        VERSION has an entry: the first of "upcoming" (the next minor patch, or the next major once nothing is listed
        before it), and the whole file is well-formed. Warns (never fails) when docs/kiln/release-notes/VERSION.md
        has items the entry does not seem to mention. A version already released passes with a notice.
        release.sh runs it before a final build (a missing entry stops the build; a test build only warns).
    roadmap.py [--source FILE] ship VERSION [--date YYYY-MM-DD]
        Moves the entry to "released" with its date (default: the GitHub release's publish time as a New York date),
        drops its status and note, and makes the following upcoming entry "next" (a 0.2.0-dev.N.1 slot only when the
        source lists one: nothing is invented). Shipping a released version again changes nothing. publish.sh runs it
        after the GitHub release and the frostdev.io downloads are published.
    roadmap.py [--source FILE] site CHECKOUT
        Writes CHECKOUT/src/data/kilnRoadmap.json from the source (nothing else).
    roadmap.py [--source FILE] deploy CHECKOUT [--version VERSION] [--yes] [--dry-run]
        In a frostdev.io checkout (publish.sh: KILN_SITE_DIR, default ~/Work/frostdev.io): refuses a dirty tree or
        another branch than main; git pull --ff-only origin main; stops unless the live revision on loothing-vps
        (readlink /opt/frostdev-public/current) is origin/main (deploying over a newer or different live site would
        drop someone's deploy; undeployed commits that only change the roadmap file, a failed earlier run of this
        command, are the one exception); writes the data, stops when nothing changed; npm ci, npm run check, npm run
        build; commits "Kiln roadmap: VERSION released" (Frostdev identity); pushes; ops/deploy.sh; checks that
        https://frostdev.io/kiln/roadmap shows VERSION as released. --dry-run does everything up to the commit, then
        puts the data file back. Without --yes it asks before the commit.
Exit status: 0 done (or nothing to do), 1 refused or failed (the message says why and what to run), 2 usage.
"""
import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
from datetime import date as Date, datetime
from pathlib import Path
from zoneinfo import ZoneInfo

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SOURCE = HERE / "roadmap" / "roadmap.json"
NOTES = REPO / "docs" / "kiln" / "release-notes"
README = "tools/release/roadmap/README.md"
SITE_FILE = "src/data/kilnRoadmap.json"
SITE_MARK = "src/lib/kilnRoadmap.ts"
GH_REPO = "frostdev-ops/kiln"
LIVE_HOST = "loothing-vps"
LIVE_LINK = "/opt/frostdev-public/current"
PAGE_URL = "https://frostdev.io/kiln/roadmap"
NEW_YORK = ZoneInfo("America/New_York")

# What src/lib/kilnRoadmap.ts accepts.
AREAS = ("launcher", "api", "content", "servers", "compat", "tools", "core")
KINDS = ("new", "improved", "fix")
HELDS = ("all", "hosted")
RELEASED_KEYS = ("version", "date", "title", "summary", "patch", "note", "features")
UPCOMING_KEYS = ("version", "status", "title", "summary", "patch", "note", "features")
FEATURE_KEYS = ("title", "text", "area", "kind", "gamePending", "held", "groundwork")
VERSION = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-dev\.(\d+)(?:\.(\d+))?)?$")


class RoadmapError(Exception):
    pass


# ------------------------------------------------------------------------------------------------------- the format
def parse_version(version):
    """0.2.0-dev.29 < 0.2.0-dev.29.1 < 0.2.0-dev.30 < 0.2.0."""
    m = VERSION.match(version or "")
    if not m:
        raise RoadmapError(f'"{version}" is not a Kiln version (0.2.0-dev.N, a minor patch 0.2.0-dev.N.M, or N.N.N)')
    a, b, c, dev, patch = m.groups()
    return (int(a), int(b), int(c), int(dev) if dev is not None else 10 ** 9, int(patch or 0))


def is_patch(version):
    m = VERSION.match(version)
    return bool(m and m.group(5) is not None)


def anchor(version):
    """The page's id of a release: dev-29-18."""
    m = re.match(r"^\d+\.\d+\.\d+-(dev\.\d+(?:\.\d+)?)$", version) or re.match(r"^(\d+\.\d+\.\d+)$", version)
    return m.group(1).replace(".", "-")


def real_date(value):
    if not isinstance(value, str) or not re.fullmatch(r"\d{4}-\d{2}-\d{2}", value):
        return False
    try:
        Date.fromisoformat(value)
    except ValueError:
        return False
    return True


def dump(data):
    """The bytes of the source and of the site's file."""
    return json.dumps(data, indent=2, ensure_ascii=False) + "\n"


def load(path):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise RoadmapError(f"cannot read {path}: {e}")


def site_view(data):
    return {key: data[key] for key in ("released", "upcoming", "later", "milestones")}


def validate(data):
    """Everything the page checks (an unknown area, kind or status, a release listed twice, released ones out of
    order or without a date), plus what the release flow needs: one "next", first, and upcoming in version order."""
    errors = []

    def text(value, where):
        if not isinstance(value, str) or not value.strip():
            errors.append(f"{where} needs a text")

    def feature(f, where):
        if not isinstance(f, dict):
            errors.append(f"{where} is not an object")
            return
        for key in f:
            if key not in FEATURE_KEYS:
                errors.append(f'{where}: unknown field "{key}"')
        text(f.get("title"), f"{where} title")
        text(f.get("text"), f"{where} text")
        if f.get("area") not in AREAS:
            errors.append(f'{where}: unknown area "{f.get("area")}" (one of {", ".join(AREAS)})')
        if f.get("kind", "new") not in KINDS:
            errors.append(f'{where}: unknown kind "{f.get("kind")}" (new, improved or fix)')
        for flag in ("gamePending", "groundwork"):
            if flag in f and not isinstance(f[flag], bool):
                errors.append(f"{where}: {flag} is true or false")
        if "held" in f:
            if f["held"] not in HELDS:
                errors.append(f'{where}: held "{f["held"]}" (all or hosted)')
            if f.get("gamePending") is not True:
                errors.append(f"{where}: held needs gamePending")

    def release(r, where, keys):
        if not isinstance(r, dict):
            errors.append(f"{where} is not an object")
            return None
        version = r.get("version")
        try:
            parse_version(version)
        except RoadmapError as e:
            errors.append(f"{where}: {e}")
            return None
        for key in r:
            if key not in keys:
                errors.append(f'{version}: unknown field "{key}"')
        text(r.get("title"), f"{version} title")
        text(r.get("summary"), f"{version} summary")
        if "note" in r:
            text(r["note"], f"{version} note")
        if is_patch(version) != (r.get("patch") is True):
            errors.append(f'{version}: "patch": true goes with a minor patch version (0.2.0-dev.N.M) and only there')
        features = r.get("features")
        if not isinstance(features, list) or not features:
            errors.append(f"{version} lists no features")
        else:
            for i, f in enumerate(features):
                feature(f, f"{version} feature {i + 1}")
        return version

    if not isinstance(data, dict) or sorted(data) != ["later", "milestones", "released", "upcoming"]:
        raise RoadmapError('the roadmap is an object with exactly "released", "upcoming", "later" and "milestones"')
    seen = set()
    released = data["released"] if isinstance(data["released"], list) else []
    upcoming = data["upcoming"] if isinstance(data["upcoming"], list) else []
    for r in released:
        v = release(r, "released", RELEASED_KEYS)
        if v and not real_date(r.get("date")):
            errors.append(f'{v} needs a real date (YYYY-MM-DD), not "{r.get("date")}"')
    for i in range(1, len(released)):
        a, b = released[i - 1].get("date"), released[i].get("date")
        if real_date(a) and real_date(b) and a < b:
            errors.append(f"released: {released[i - 1]['version']} is older than {released[i]['version']}; newest first")
    nexts = 0
    for i, r in enumerate(upcoming):
        v = release(r, "upcoming", UPCOMING_KEYS)
        if not v:
            continue
        status = r.get("status")
        if status not in ("next", "planned"):
            errors.append(f'{v}: status "{status}" (next or planned)')
        nexts += status == "next"
        if i == 0 and status != "next":
            errors.append(f'{v}: the first upcoming entry is "next"')
        if i and status == "next":
            errors.append(f'{v}: only the first upcoming entry is "next"')
        if "date" in r:
            errors.append(f"{v}: an upcoming entry has no date")
    for r in released + upcoming:
        v = r.get("version") if isinstance(r, dict) else None
        if v in seen:
            errors.append(f"{v} is listed twice")
        seen.add(v)
    try:
        order = [parse_version(r["version"]) for r in upcoming]
        if order != sorted(order):
            errors.append("upcoming: in version order, the next one first")
        if order and released and order[0] <= parse_version(released[0]["version"]):
            errors.append(f"upcoming: {upcoming[0]['version']} is not newer than the newest released {released[0]['version']}")
    except (RoadmapError, KeyError, TypeError):
        pass  # reported above
    for i, item in enumerate(data["later"] if isinstance(data["later"], list) else []):
        text(item.get("title"), f"later {i + 1} title")
        text(item.get("text"), f"later {i + 1} text")
        if item.get("area") not in AREAS:
            errors.append(f'later {i + 1}: unknown area "{item.get("area")}"')
    for i, m in enumerate(data["milestones"] if isinstance(data["milestones"], list) else []):
        text(m.get("title"), f"milestone {i + 1} title")
        text(m.get("text"), f"milestone {i + 1} text")
        if m.get("after") is not None and m.get("after") not in seen:
            errors.append(f'milestone {i + 1}: "after" names {m.get("after")}, which is not on the roadmap')
    if errors:
        raise RoadmapError("the roadmap is not well-formed:\n  " + "\n  ".join(errors))


def find(data, version):
    for where in ("released", "upcoming"):
        for i, r in enumerate(data[where]):
            if r["version"] == version:
                return where, i
    return None, None


def missing(version):
    kind = "a minor patch" if is_patch(version) else "a release"
    return (f'{version} has no entry in tools/release/roadmap/roadmap.json: add it to "upcoming" as the first entry '
            f'(status "next", title, summary{", \"patch\": true" if is_patch(version) else ""} and its features, '
            f'player-facing; {kind}, see {README}), commit it, then run this again')


def first_upcoming(data, version):
    """The entry of VERSION, which must be the first upcoming one."""
    where, i = find(data, version)
    if where is None:
        raise RoadmapError(missing(version))
    if i:
        before = ", ".join(r["version"] for r in data["upcoming"][:i])
        raise RoadmapError(
            f"{version} is not the next entry: {before} {'is' if i == 1 else 'are'} listed before it and not released. "
            f"Ship {'it' if i == 1 else 'them'} first, or move {'its' if i == 1 else 'their'} items into {version}'s "
            f"entry and remove {'it' if i == 1 else 'them'} ({README})")
    return data["upcoming"][0]


# ------------------------------------------------------------------------------------------------------------ check
STOP = set("""about after again also before being between both does each every from have here into just like more most
much only other over same some such than that their them then there these they this those through under until very what
when where which while will with without would your kiln kiln's game games mods your now more""".split())


def words(s):
    return {w for w in re.findall(r"[a-z0-9]+", s.lower()) if len(w) >= 4 and w not in STOP}


def note_titles(markdown):
    """The bold titles of the release notes' bullets: "- **Repair for Kiln files your antivirus removed.** ..."."""
    return [t.strip().rstrip(".:!").strip() for t in re.findall(r"^\s*[-*]\s+\*\*(.+?)\*\*", markdown, re.M)]


def unmentioned(entry, markdown):
    feats = [words(f["title"] + " " + f["text"]) for f in entry["features"]]
    out = []
    for title in note_titles(markdown):
        w = words(title)
        if w and not any(len(w & f) >= max(1, (len(w) + 1) // 2) for f in feats):
            out.append(title)
    return out


def check(data, version, notes_dir=NOTES):
    """Raises RoadmapError when VERSION cannot ship as it stands; returns warnings."""
    parse_version(version)
    validate(data)
    where, i = find(data, version)
    if where == "released":
        return [f"{version} is already released on {data['released'][i]['date']}: nothing to ship"]
    entry = first_upcoming(data, version)
    warnings = []
    notes = Path(notes_dir) / f"{version}.md"
    if notes.is_file():
        for title in unmentioned(entry, notes.read_text(encoding="utf-8")):
            warnings.append(f'release notes item not on the roadmap entry of {version}: "{title}" ({notes.name}); '
                            f"add it if players should see it on the roadmap (a warning only)")
    return warnings


# ------------------------------------------------------------------------------------------------------------- ship
def ny_date(iso):
    """GitHub's publish time (UTC) as the date in New York, the date the site's release data uses."""
    return datetime.fromisoformat(iso.replace("Z", "+00:00")).astimezone(NEW_YORK).date().isoformat()


def github_date(version, runner):
    code, out = runner.run(["gh", "release", "view", f"v{version}", "--repo", GH_REPO, "--json", "isDraft,publishedAt"],
                           check=False)
    info = {}
    if code == 0:
        try:
            info = json.loads(out)
        except ValueError:
            info = {}
    if code != 0 or info.get("isDraft") or not info.get("publishedAt"):
        raise RoadmapError(f"v{version} is not published on GitHub ({GH_REPO}) yet, so it has no release date: publish "
                           f"it first, or pass --date YYYY-MM-DD (the New York date it went out)")
    return ny_date(info["publishedAt"])


def ship(data, version, day):
    """Moves VERSION's entry to the released ones on DAY. False when it is released already (nothing changes)."""
    parse_version(version)
    validate(data)
    if find(data, version)[0] == "released":
        return False
    entry = first_upcoming(data, version)
    if not real_date(day):
        raise RoadmapError(f'{version}: the release date is a real YYYY-MM-DD, not "{day}"')
    if data["released"] and day < data["released"][0]["date"]:
        raise RoadmapError(f"{version}: {day} is older than the newest released entry ({data['released'][0]['version']}, "
                           f"{data['released'][0]['date']}); check --date")
    data["upcoming"].pop(0)
    shipped = {"version": version, "date": day, "title": entry["title"], "summary": entry["summary"]}
    if entry.get("patch"):
        shipped["patch"] = True
    shipped["features"] = entry["features"]  # an upcoming note is about its state, so it ends with the release
    data["released"].insert(0, shipped)
    if data["upcoming"]:
        data["upcoming"][0]["status"] = "next"
    validate(data)
    return True


# ------------------------------------------------------------------------------------------------------------- site
def write_site(data, checkout):
    """Writes CHECKOUT/src/data/kilnRoadmap.json; False when it already holds exactly that."""
    checkout = Path(checkout)
    if not (checkout / SITE_MARK).is_file():
        raise RoadmapError(f"{checkout} is not a frostdev.io checkout (no {SITE_MARK})")
    validate(data)
    path = checkout / SITE_FILE
    new = dump(site_view(data)).encode("utf-8")
    if path.is_file() and path.read_bytes() == new:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(new)
    return True


# ----------------------------------------------------------------------------------------------------------- deploy
class Runner:
    """Runs a command, printing it first. stream: the output goes straight to the terminal (npm, ops/deploy.sh)."""

    def run(self, cmd, cwd=None, check=True, stream=False):
        print("$ " + " ".join(shlex.quote(str(c)) for c in cmd), flush=True)
        try:
            if stream:
                p = subprocess.run(cmd, cwd=cwd)
                out = ""
            else:
                p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
                out = p.stdout if p.returncode == 0 else p.stdout + p.stderr
        except FileNotFoundError:
            p, out = subprocess.CompletedProcess(cmd, 127), f"{cmd[0]}: not found"
        if check and p.returncode != 0:
            raise RoadmapError(f"failed ({p.returncode}): {' '.join(map(str, cmd))}\n{out.strip()}".rstrip())
        return p.returncode, out


def tty_ask(question):
    if not sys.stdin.isatty():
        print(f"{question} -> no (not a terminal; pass --yes)")
        return False
    return input(f"{question} [y/N] ").strip().lower() in ("y", "yes")


def step(text):
    print(f"\n== roadmap deploy: {text}", flush=True)


def deploy(data, checkout, version, runner, yes=False, dry_run=False, ask=tty_ask, sleep=time.sleep):
    checkout = Path(checkout).resolve()
    if not (checkout / SITE_MARK).is_file():
        raise RoadmapError(f"{checkout} is not a frostdev.io checkout (no {SITE_MARK})")
    validate(data)
    version = version or data["released"][0]["version"]
    if find(data, version)[0] != "released":
        raise RoadmapError(f"{version} is not released in the roadmap source: run roadmap.py ship {version} first")

    def git(*args, check=True):
        return runner.run(["git", *args], cwd=checkout, check=check)

    step(f"site checkout {checkout} (dry run)" if dry_run else f"site checkout {checkout}")
    dirty = git("status", "--porcelain", "--untracked-files=all")[1].strip()
    if dirty:
        raise RoadmapError(f"{checkout} is not clean (commit or stash first; the deploy ships a clean, committed "
                           f"tree):\n{dirty}")
    branch = git("rev-parse", "--abbrev-ref", "HEAD")[1].strip()
    if branch != "main":
        raise RoadmapError(f"{checkout} is on {branch}, not main: switch to main first")

    step("git pull --ff-only origin main")
    git("pull", "--ff-only", "origin", "main")
    head = git("rev-parse", "HEAD")[1].strip()
    origin = git("rev-parse", "origin/main")[1].strip()
    if head != origin:
        raise RoadmapError(f"{checkout} has commits origin/main does not ({head[:8]} vs origin/main {origin[:8]}): "
                           f"push or drop them first")

    step(f"live revision ({LIVE_HOST}:{LIVE_LINK}) against origin/main")
    code, out = runner.run(["ssh", "-o", "BatchMode=yes", LIVE_HOST, "readlink", LIVE_LINK], check=False)
    live = out.strip().rsplit("/", 1)[-1]
    if code != 0 or not re.fullmatch(r"[0-9a-f]{40}", live):
        raise RoadmapError(f"cannot read the live revision (ssh {LIVE_HOST} readlink {LIVE_LINK}): {out.strip()}")
    pending = False  # undeployed roadmap-only commits (an earlier run that failed after its commit)
    if live != origin:
        if git("cat-file", "-e", f"{live}^{{commit}}", check=False)[0] != 0:
            raise RoadmapError(f"the live site is {live[:8]}, a revision origin/main does not have (deployed from an "
                               f"unpushed tree?). Deploying now would drop that deploy: find who deployed it and get "
                               f"it pushed, then run this again")
        if git("merge-base", "--is-ancestor", origin, live, check=False)[0] == 0:
            raise RoadmapError(f"the live site {live[:8]} is ahead of origin/main {origin[:8]}: someone deployed "
                               f"commits that are not pushed. Get them pushed, then run this again")
        if git("merge-base", "--is-ancestor", live, origin, check=False)[0] != 0:
            raise RoadmapError(f"the live site {live[:8]} and origin/main {origin[:8]} have gone different ways: "
                               f"sort that out by hand (docs/public-pages.md), then run this again")
        changed = git("diff", "--name-only", live, origin)[1].split()
        count = git("rev-list", "--count", f"{live}..{origin}")[1].strip()
        if changed != [SITE_FILE]:
            raise RoadmapError(f"origin/main has {count} commit(s) the live site {live[:8]} does not have yet, beyond "
                               f"the roadmap: they would go live with it. Deploy them on purpose first (ops/deploy.sh "
                               f"after a review) or ask who pushed them, then run this again")
        pending = True
        print(f"origin/main has {count} undeployed commit(s) that change only {SITE_FILE}: they go live now")
    else:
        print(f"live = origin/main = {origin[:8]}")

    step(f"write {SITE_FILE}")
    path = checkout / SITE_FILE
    before = path.read_bytes() if path.is_file() else None
    changed = write_site(data, checkout)
    if not changed and not pending:
        print(f"{SITE_FILE} already holds the roadmap: nothing to deploy")
        return 0
    print("changed" if changed else "unchanged (the undeployed commit holds it)")

    def restore():
        if changed:
            if before is None:
                path.unlink(missing_ok=True)
            else:
                path.write_bytes(before)

    try:
        if changed:
            print(git("diff", "--stat", "--", SITE_FILE)[1].rstrip())
        step("npm ci && npm run check && npm run build")
        runner.run(["npm", "ci"], cwd=checkout, stream=True)
        runner.run(["npm", "run", "check"], cwd=checkout, stream=True)
        runner.run(["npm", "run", "build"], cwd=checkout, stream=True)
        others = [line for line in git("status", "--porcelain", "--untracked-files=all")[1].splitlines()
                  if line.strip() and line[3:].strip() != SITE_FILE]
        if others:
            raise RoadmapError("the build changed other files than the roadmap: " + ", ".join(o[3:] for o in others))
        message = f"Kiln roadmap: {version} released"
        # the site checkout's own git identity (its history is the owner's), no override
        commit = ["git", "commit", "-q", "-m", message, "--", SITE_FILE]
        if dry_run:
            step("dry run: stops before the commit")
            if changed:
                print("would run: " + " ".join(shlex.quote(c) for c in commit))
            print("would run: git push origin HEAD:main")
            print("would run: bash ops/deploy.sh")
            print(f"would check: {PAGE_URL} shows {version} (#{anchor(version)}) as released")
            restore()
            print(f"{SITE_FILE} put back as it was")
            return 0
        if not yes and not ask(f"Commit {message!r}, push to origin main and deploy frostdev.io?"):
            restore()
            print(f"not deployed; {SITE_FILE} put back as it was")
            return 1
    except BaseException:
        restore()
        raise

    if changed:
        step(f"commit: {message}")
        runner.run(commit, cwd=checkout)
    step("git push origin HEAD:main")
    code, out = runner.run(["git", "push", "origin", "HEAD:main"], cwd=checkout, check=False)
    if code != 0:
        raise RoadmapError(f"the push failed (the commit is in {checkout}, not pushed): {out.strip()}\n"
                           f"Fix it, then: git -C {checkout} push origin HEAD:main && (cd {checkout} && ops/deploy.sh)")
    step("ops/deploy.sh")
    code, _ = runner.run(["bash", "ops/deploy.sh"], cwd=checkout, check=False, stream=True)
    if code != 0:
        raise RoadmapError(f"ops/deploy.sh failed (the roadmap commit is pushed). Retry: python3 -I {Path(__file__)} "
                           f"deploy {checkout} --version {version} (it deploys pushed roadmap-only commits), or "
                           f"(cd {checkout} && ops/deploy.sh)")
    step(f"check {PAGE_URL}")
    shows = re.compile(r'<details\b[^>]*\bid="' + re.escape(anchor(version)) + r'"[^>]*\bdata-status="released"')
    for attempt in range(4):
        code, page = runner.run(["curl", "-fsSL", "--max-time", "60", f"{PAGE_URL}?v={version}"], check=False)
        if code == 0 and shows.search(page):
            print(f"{PAGE_URL} shows {version} as released")
            return 0
        if attempt < 3:
            sleep(15)
    raise RoadmapError(f"deployed, but {PAGE_URL} does not show {version} (#{anchor(version)}) as released yet "
                       f"(a cache?). Look at the page; the deploy itself went through")


# -------------------------------------------------------------------------------------------------------------- cli
def main(argv=None):
    p = argparse.ArgumentParser(prog="roadmap.py", description="Kiln's roadmap on frostdev.io in the release flow")
    p.add_argument("--source", type=Path, default=SOURCE, help="the roadmap source (default %(default)s)")
    sub = p.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check", help="VERSION has its entry and the file is well-formed")
    c.add_argument("version")
    c.add_argument("--notes-dir", type=Path, default=NOTES)
    s = sub.add_parser("ship", help="move VERSION to the released ones")
    s.add_argument("version")
    s.add_argument("--date", help="YYYY-MM-DD (default: the GitHub publish time as a New York date)")
    w = sub.add_parser("site", help="write CHECKOUT/src/data/kilnRoadmap.json")
    w.add_argument("checkout", type=Path)
    d = sub.add_parser("deploy", help="update, build, commit, push and deploy the frostdev.io checkout")
    d.add_argument("checkout", type=Path)
    d.add_argument("--version", help="for the commit message and the page check (default: the newest released)")
    d.add_argument("--yes", action="store_true", help="do not ask before the commit")
    d.add_argument("--dry-run", action="store_true", help="everything up to the commit, then put the file back")
    args = p.parse_args(argv)
    try:
        data = load(args.source)
        if args.cmd == "check":
            for warning in check(data, args.version, args.notes_dir):
                print(f"WARNING: {warning}", file=sys.stderr)
            print(f"roadmap: {args.version} ok")
        elif args.cmd == "ship":
            if find(data, args.version)[0] == "released":
                validate(data)
                print(f"roadmap: {args.version} is already released on "
                      f"{data['released'][find(data, args.version)[1]]['date']}: nothing changed")
                return 0
            first_upcoming(data, args.version)  # before asking GitHub
            day = args.date or github_date(args.version, Runner())
            ship(data, args.version, day)
            args.source.write_text(dump(data), encoding="utf-8")
            nxt = data["upcoming"][0]["version"] if data["upcoming"] else "nothing"
            print(f"roadmap: {args.version} released on {day} in {args.source}; next is {nxt}")
        elif args.cmd == "site":
            changed = write_site(data, args.checkout)
            print(f"roadmap: {args.checkout / SITE_FILE} {'written' if changed else 'unchanged'}")
        elif args.cmd == "deploy":
            return deploy(data, args.checkout, args.version, Runner(), yes=args.yes, dry_run=args.dry_run)
    except RoadmapError as e:
        print(f"roadmap: {e}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
