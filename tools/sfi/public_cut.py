#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""public_cut.py: the stage guard of Kiln's public and private builds.

tools/package/stage.sh runs it twice:
  python3 -I tools/sfi/public_cut.py source <repo>              before anything is built
  python3 -I tools/sfi/public_cut.py stage <repo> <stage dir>   once the stage is made
A stage is private only on a branch sfi/* of <repo>'s own git checkout, and never in a release build
(KILN_RELEASE_BUILD=1). Anything else is a public cut: another branch, a detached HEAD, a folder that is not its own
git checkout, no git, a git that fails.
  source  on a public cut, a file of a private build (PRIVATE_PATHS) or a file holding a private marker
          (PRIVATE_TOKENS) stops the stage.
  stage   on every cut, a developer test build (the marker a KILN_SFI_DEV_SF18_TEST kiln-core.dll carries) stops the
          stage.
Paths match whatever their case; a link is read through, and on a public cut a link out of the tree is a finding.
For private branches (not run by stage.sh):
  python3 -I tools/sfi/public_cut.py audit <repo> <base>       the files changed since <base> that this guard would
          let through on a public cut.
Exit 0: passed (audit: nothing unmarked). 1: refused, each finding on stderr. 2: wrong usage."""
import mmap
import os
import re
import subprocess
import sys

# Files that must never reach a public cut (globs: * stays in one folder, ** crosses folders), repo-relative.
PRIVATE_PATHS = (
    "tools/sfi/oracle/**",
    "tools/sfi/wire/**",
    "tools/sfi/make_public_cut.py",
    "crates/sfi/kiln-sfi/tests/wire_peer.rs",
    "native/services/net/tests/steam_standin/wire_lease.cpp",
    "crates/sfi/kiln-sfi/src/lab/**",
    "crates/sfi/kiln-sfi/tests/lab_*",
    "crates/sfi/sfi-core/coverage/**",
    "tools/sfi/contract/**",
    "tools/sfi/apigen/**",
    "crates/sfi/sfi-conformance-tests/**",
    "crates/sfi/sfi-conformance-support/**",
    "gpl/kiln-eml/kiln-eml/tests/fixtures/m6/**obligations.json",
    "crates/sfi/sfi-core/tests/fixtures/c7f/gen_links.py",
    "tools/sfi/cut/**",
)
# This guard itself: it has to be there to run.
EXEMPT = ("tools/sfi/public_cut.py", "tools/sfi/test_public_cut.py")
# Text that only the private files hold.
PRIVATE_TOKENS = ("sfi-private", "KILN_SFI_DEV_SF18_TEST", "tools/sfi/oracle", "tools/sfi/wire", "lab-no-count-hook", "make_public_cut", "tools/sfi/contract", "tools/sfi/apigen", "sfi-core/coverage", "sfi-conformance", "sfi_conformance")
STAGE_TOKENS = ()
STAGE_NAMES = ()
# Staged folders of private build output (a public cut refuses every file in one).
STAGE_DIRS = ()
PUBLIC_CHANGES = {}
# A KILN_SFI_DEV_SF18_TEST build (native/core/src/sfi_release_policy.cpp): never staged, on any branch.
DEV_MARKER = "kiln-sfi-dev-sf18-test-build"
# Folders whose files a walk without git does not read for markers (build output and dependencies); their paths are
# still matched against PRIVATE_PATHS.
SKIP_DIRS = {"node_modules", "target", ".out", ".cache", "bin", "obj", "__pycache__"}


def glob_re(pattern):
    """A path pattern as a regex, whatever the case: * stays in one folder, ** crosses folders, and a trailing /**
    also matches the folder's own path (a link in its place)."""
    out, i = "", 0
    while i < len(pattern):
        if pattern.startswith("/**", i) and i + 3 == len(pattern):
            out, i = out + "(/.*)?", i + 3
        elif pattern.startswith("**", i):
            out, i = out + ".*", i + 2
        elif pattern[i] == "*":
            out, i = out + "[^/]*", i + 1
        else:
            out, i = out + re.escape(pattern[i]), i + 1
    return re.compile(out, re.IGNORECASE)


PRIVATE_RES = [glob_re(p) for p in PRIVATE_PATHS]
PRIVATE_RE = re.compile("|".join(f"(?:{r.pattern})" for r in PRIVATE_RES), re.IGNORECASE)


def git(repo, *args):
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}  # never another repository than <repo>
    return subprocess.run(["git", "-C", repo, *args], env=env, capture_output=True, timeout=120)


def private_branch(repo):
    """The sfi/* branch <repo> is on, or None with why this is a public cut."""
    if os.environ.get("KILN_RELEASE_BUILD") == "1":
        return None, "a release build (KILN_RELEASE_BUILD=1)"
    try:
        top = git(repo, "rev-parse", "--show-toplevel")
        if top.returncode or os.path.realpath(top.stdout.decode().strip()) != os.path.realpath(repo):
            return None, "not its own git checkout"
        head = git(repo, "symbolic-ref", "--short", "-q", "HEAD")
        if head.returncode:
            return None, "a detached HEAD"
        name = head.stdout.decode(errors="replace").strip()
    except (OSError, subprocess.SubprocessError):
        return None, "no usable git"
    if re.fullmatch(r"sfi/[A-Za-z0-9._/-]+", name):
        return name, ""
    return None, f"branch {name}"


def own_checkout(repo):
    """<repo> is the top of its own git checkout."""
    try:
        top = git(repo, "rev-parse", "--show-toplevel")
        return not top.returncode and os.path.realpath(top.stdout.decode().strip()) == os.path.realpath(repo)
    except (OSError, subprocess.SubprocessError):
        return False


def files_of(repo):
    """Repo-relative paths: git's tracked and untracked (not ignored) files, or a walk of everything but .git (links
    to folders are entries of their own, never followed)."""
    if own_checkout(repo):
        try:
            ls = git(repo, "ls-files", "-z", "--cached", "--others", "--exclude-standard")
            if not ls.returncode:
                return sorted({p.decode("utf-8", "surrogateescape") for p in ls.stdout.split(b"\0") if p})
        except (OSError, subprocess.SubprocessError):
            pass
    out = []
    for root, dirs, names in os.walk(repo):
        rel = os.path.relpath(root, repo).replace(os.sep, "/")
        prefix = "" if rel == "." else rel + "/"
        links = [d for d in dirs if os.path.islink(os.path.join(root, d))]
        dirs[:] = sorted(d for d in dirs if d != ".git" and d not in links)
        out += [prefix + n for n in names + links]
    return sorted(out)


def build_output(rel):
    """A path under a build output or dependency folder (SKIP_DIRS, native/build)."""
    parts = rel.split("/")[:-1]
    return any(p in SKIP_DIRS for p in parts) or parts[:2] == ["native", "build"]


def submodule_trees(repo):
    """A walk's submodule folders under third_party/ (.gitmodules): release.sh exports ShroudForge's pinned source into
    third_party/shroudforge, whose own words (building_input, ResourceLifeGuard, ...) are where Kiln's names come from.
    Their paths are still matched against PRIVATE_PATHS, their text is not read, as git's file list leaves a
    submodule's files out of a checkout's."""
    try:
        with open(os.path.join(repo, ".gitmodules"), encoding="utf-8", errors="replace") as f:
            paths = re.findall(r"^\s*path\s*=\s*(\S+)\s*$", f.read(), re.M)
    except OSError:
        return ()
    return tuple(p.rstrip("/") + "/" for p in paths if re.fullmatch(r"third_party/[A-Za-z0-9._-]+/?", p))


def holds(path, tokens):
    """The tokens a file holds (UTF-8 and UTF-16LE), read whole. Raises OSError when it cannot be read."""
    with open(path, "rb") as f:
        if os.fstat(f.fileno()).st_size == 0:
            return []
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as data:
            return [t for t in tokens if data.find(t.encode()) >= 0 or data.find(t.encode("utf-16-le")) >= 0]


def private_path(rel):
    return PRIVATE_RE.fullmatch(rel) is not None


def check_file(repo, rel, foreign=()):
    """Why <repo>/<rel> may not reach a public cut: findings, empty when it may. Under a `foreign` folder (a walk's
    submodule trees) only the path counts."""
    if rel in EXEMPT:
        return []
    if private_path(rel):
        return [f"private file: {rel}"]
    full = os.path.join(repo, rel)
    findings = []
    if os.path.islink(full) and not build_output(rel):
        target = os.path.realpath(full)
        inside = os.path.relpath(target, os.path.realpath(repo)).replace(os.sep, "/")
        if inside == ".." or inside.startswith("../"):
            findings.append(f"a link out of the tree: {rel} -> {target}")
        elif private_path(inside):
            return [f"private file: {rel} (a link to {inside})"]
    if not os.path.isfile(full) or build_output(rel) or rel.startswith(foreign):  # a link is read through
        return findings
    try:
        found = holds(full, PRIVATE_TOKENS)
    except OSError as e:
        return findings + [f"cannot read {rel} ({e.strerror})"]
    if found:
        findings.append(f"private change in {rel}: holds {', '.join(repr(t) for t in found)}")
    return findings


def check_source(repo):
    foreign = () if own_checkout(repo) else submodule_trees(repo)
    findings = []
    for rel in files_of(repo):
        findings += check_file(repo, rel, foreign)
    return findings


# The public lines, newest first: the audit's "public" base is the merge-base with the first one this checkout has.
PUBLIC_LINES = ("origin/dev30/integration", "origin/main")


def public_base(repo):
    """The merge-base of HEAD with the newest public line this checkout knows, or None."""
    for line in PUBLIC_LINES:
        p = subprocess.run(["git", "-C", str(repo), "merge-base", line, "HEAD"], capture_output=True, text=True)
        if p.returncode == 0 and p.stdout.strip():
            return p.stdout.strip()
    return None


def audit(repo, base):
    """The files changed since <base> (committed, staged, in the working tree, untracked) that check_file lets through
    and PUBLIC_CHANGES does not name. None when <base> is not a commit of <repo>."""
    if not own_checkout(repo) or git(repo, "rev-parse", "--verify", "-q", base + "^{commit}").returncode:
        return None
    changed = git(repo, "diff", "-z", "--name-only", "--no-renames", "--diff-filter=d", base).stdout
    untracked = git(repo, "ls-files", "-z", "--others", "--exclude-standard").stdout
    paths = sorted({p.decode("utf-8", "surrogateescape") for p in (changed + b"\0" + untracked).split(b"\0") if p})
    return [p for p in paths if p not in PUBLIC_CHANGES and p not in EXEMPT and not check_file(repo, p)]


def check_stage(stage, public):
    findings = []
    if not os.path.isdir(stage):
        return [f"no stage folder {stage}"]
    for root, dirs, names in os.walk(stage):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, stage).replace(os.sep, "/")
            if os.path.islink(full):
                findings.append(f"a link in the stage: {rel}")
                continue
            if public and n.lower().startswith(STAGE_NAMES):
                findings.append(f"a file of a private build staged: {rel}")
            if public and any(f"/{d}/" in "/" + rel.lower() for d in STAGE_DIRS):
                findings.append(f"a folder of a private build staged: {rel}")
            try:
                found = holds(full, (DEV_MARKER, *STAGE_TOKENS) if public else (DEV_MARKER,))
            except OSError as e:
                findings.append(f"cannot read {rel} ({e.strerror})")
                continue
            if DEV_MARKER in found:
                findings.append(f"a developer test build (KILN_SFI_DEV_SF18_TEST, the kiln-sfi.sf18 veto open): {rel}")
            rest = [t for t in found if t != DEV_MARKER]
            if rest:
                findings.append(f"private build output: {rel} holds {', '.join(repr(t) for t in rest)}")
    return findings


def main(argv):
    if len(argv) == 3 and argv[1] == "source":
        cmd, repo, stage = "source", argv[2], None
    elif len(argv) == 4 and argv[1] in ("stage", "audit"):
        cmd, repo, stage = argv[1], argv[2], argv[3]
    else:
        print("usage: public_cut.py source <repo> | public_cut.py stage <repo> <stage dir> | public_cut.py audit <repo> "
              "<base>|public", file=sys.stderr)
        return 2
    if not os.path.isdir(repo):
        print(f"public_cut: no repository folder {repo}", file=sys.stderr)
        return 2
    if cmd == "audit":
        if stage == "public":
            stage = public_base(repo) or stage
        missed = audit(repo, stage)
        if missed is None:
            print(f"public_cut: audit needs a git checkout and a commit of it, not {stage}", file=sys.stderr)
            return 2
        for p in missed:
            print(f"public_cut: unmarked: {p}", file=sys.stderr)
        if missed:
            print(f"public_cut: {len(missed)} file(s) changed since {stage} would pass a public cut: list a private file "
                  "in PRIVATE_PATHS, wrap private lines in 'sfi-private begin' / 'sfi-private end', or name a public "
                  "change in PUBLIC_CHANGES with why", file=sys.stderr)
        return 1 if missed else 0
    branch, why = private_branch(repo)
    if cmd == "source":
        if branch:
            print(f"public_cut: private stage (branch {branch}): a private build; never publish it", file=sys.stderr)
            return 0
        findings = check_source(repo)
    else:
        findings = check_stage(stage, public=branch is None)
    for f in findings:
        print(f"public_cut: {f}", file=sys.stderr)
    if findings:
        where = f"private stage (branch {branch})" if branch else f"public cut ({why})"
        print(f"public_cut: REFUSED: {len(findings)} finding(s) in a {where}. A private build's files never reach a "
              "public stage: stage a private build from its sfi/* branch, or leave those files out; a developer test "
              "build is never staged.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
