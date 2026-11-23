#!/usr/bin/env bash
# Builds a complete Kiln release folder from a clean export of one commit, checks it, and stops: publishing is a
# separate, manual step (tools/release/github-draft.sh, tools/release/publish.sh; docs/kiln/launcher/releasing.md).
#   tools/release/release.sh VERSION [--beta N] [--commit REV] [--out DIR] [--work DIR] [--notes FILE] [--worktree]
#                            [--launcher-only]
# --launcher-only: a quick fix that ships only the launchers, the runtime, kilnctl and the server pack: no SDK
#   packages, kiln CLI, VS Code extension, sample mods, modding site or NuGet feed (mod authors keep the previous
#   release's SDK). The release checks need the SDK, so it implies --no-checks; the API record is not promoted.
#                                    [--skip-windows] [--no-checks] [--force] [--host-build] [--no-nexus]
# VERSION: X.Y.Z (stable) or X.Y.Z-dev.N (dev) for this tree's VERSION (tools/release/version.sh prints one). A release
#   needs its entry in the roadmap source (tools/release/roadmap/roadmap.json, checked with roadmap.py check before the
#   build; publish.sh ships it to frostdev.io/kiln/roadmap): a missing one stops here, a test build only warns.
# --beta N (or --channel beta --beta N): test build N of VERSION for the beta channel (docs/kiln/launcher/releasing.md,
#   "Test builds", "Cutting a beta"): the launcher keeps VERSION and has the channel and N baked in
#   (KILN_LAUNCHER_CHANNEL=beta, KILN_LAUNCHER_BETA=N); its files are named and signed for the build id
#   VERSION+beta.N; the folder is kiln-VERSION-beta.N with only updates/frostdev.io/beta/latest.json and
#   updates/github/latest.json (for updates-beta), never a dev or stable manifest. No Nexus Mods build, no Nexus dry
#   run, no API record promotion; github-draft.sh and publish.sh know such a folder by its manifest.
# --channel dev|stable: must fit VERSION (the channel always follows it); a release also writes the beta channel's
#   manifest (the release is the newest build a tester can have too; publish.sh moves it only when no newer test build
#   is out).
# --commit: what to build (default HEAD); exported with `git archive`, so uncommitted work never gets in.
# --worktree: test runs only: export the working tree (tracked and untracked, not ignored) instead of a commit. It needs
#   an explicit --out that is not a release folder (~/Kiln-release/kiln-*, ~/Kiln-release; e.g. ~/Kiln-release/trial/
#   kiln-VERSION), else it is refused before anything is done; its README.txt says "a test build", the Nexus dry run
#   is skipped, and github-draft.sh, publish.sh and nexus.py --publish refuse such a folder (not built from a commit).
# --out: the release folder (default ~/Kiln-release/kiln-VERSION). --work: build dir (default <repo>/.out/release-work;
#   a fixed path, so later runs reuse the compiler caches). --notes: release notes (Markdown); default: the commit
#   subjects since the previous v* tag.
# Linux files (the launcher's AppImage, deb and rpm, kilnctl, kiln) are compiled and bundled in a Docker container,
#   Ubuntu 22.04 (glibc 2.35; tools/release/linux-build/), so they run on older distros and the Steam Deck: a build on
#   this machine needs its own, newer glibc. Cargo's Linux target dir: KILN_GLIBC_DIR (default <repo>/.out/glibc)/cargo.
# --host-build: quick local tests only: build the Linux files on this machine instead (fast, no Docker). They run only
#   on distros with this machine's glibc or newer; the glibc check fails, and such a folder must never be published.
# --no-nexus: skip the Nexus Mods dry run at the end (test builds).
#
# The folder: README.txt, RELEASE-NOTES.md, SHA256SUMS, LICENSE, CREDITS.md, THIRD-PARTY-NOTICES.txt,
#   linux/    the AppImage (+ .sig, the updater signature), .deb, .rpm
#   windows/  the NSIS setup (+ .sig)
#   runtime/  kiln-runtime-VERSION.zip, kilnctl, kilnctl.exe, INSTALL-RUNTIME.txt, kiln-server-VERSION.zip (the server
#             pack: the server components as a flat zip to unzip into a server folder, lib/serverpack.py), and the
#             systemd units for kilnctl (kiln@.service, kiln-backup@.service, kiln-backup@.timer)
#   sdk/      Kiln.Sdk, Kiln.Api, Kiln.Templates nupkgs, kiln, kiln.exe (the CLI), the VS Code extension .vsix;
#             feed/: the nupkgs as a static NuGet v3 feed (lib/feed.py; its URLs: KILN_FEED_URL, default this folder on
#             frostdev.io); api/: the modding site and API reference (tools/docs-site/build.sh, when the export has it).
#             Both are built only: publish.sh uploads them with --sdk-site, github-draft.sh never
#   samples/  sample mods as zips (Library > import)
#   updates/  latest.json per source and channel: frostdev.io/<channel>/latest.json, github/latest.json
#   source/   kiln-VERSION-src.tar.gz
#   nexus/    the Nexus Mods build of the launcher: the same launcher without its self-update (Nexus Mods does not allow
#             programs that update themselves): linux/ AppImage + deb, windows/ setup, README.txt; no .sig, no
#             manifest. Only nexus.py uploads it; github-draft.sh and publish.sh leave it out.
# Signing: the updater key ~/.config/kiln-release/updater.key (+ updater.key.password); Windows Authenticode only when
#   ~/.config/kiln-release/authenticode.env exists (tools/release/authenticode.sh). Neither is ever printed.
# Checks it runs on its own output (all of them; it fails at the end when one failed): SHA256SUMS, the updater
#   signatures against the launcher's built-in key, the AppImage's --selftest on a private display, the NSIS setup
#   installed silently under Wine + --selftest (both for the Nexus build too, which must say distribution nexus), the
#   Nexus builds holding none of the self-update (feeds, updater key, plugin) and the direct ones all of it, the deb's
#   control and files, the rpm's files, `kilnctl bundle verify`,
#   kilnctl/kiln --version (Windows builds under Wine), every Linux ELF (in the AppImage, deb and rpm too) needing no
#   glibc symbol version above 2.35, the server pack (only the server components, the runtime's bytes, rebuilt
#   byte-identically) and the systemd units, kilnctl/kiln/the AppImage started on a clean Ubuntu 22.04 (a throwaway
#   container: --selftest and the launcher's window),a mod from the packed templates built against the packed SDK,
#   the NuGet feed against the nupkgs, the API reference's index.html (when built), the vsix's files, Authenticode signatures (when signed), every plugin DLL in the runtime's manifest (lib/plugins.py), and a scan of every file and archive for game data, home
#   paths, keys, passwords and tokens.
# The API gate (tools/release/api-gate.sh), before any build: no shipped public API removed since the previous v* tag,
#   new API only with a raised Kiln.Mods.ApiVersion; after a release whose checks passed, the Unshipped.txt lines move
#   into Shipped.txt in the working tree (to commit). The builds get the full version (VERSION in the export).
# The agent API index (agent/skills/kiln-modding/references/api-index/, tools/docs-site/api_index.py), before the export: regenerated
#   from the checked-out tree (its PublicAPI record, the XML docs of a Release build in $work/api-index, KILN_API_XML=DIR to use
#   docs built elsewhere, and managed/api-history.tsv). A release exports a commit, so when the index changed the release stops at
#   once and says to commit it; run again then. After the promote below it is generated once more from the same XML docs and goes
#   into the same closing commit as the API record and the ledger. Test builds (--beta, --worktree, --launcher-only) skip both.
# The doc gate: the API reference build (tools/docs-site/build.sh, in the assemble step) runs with KILN_DOCS_STRICT=1: a public name with no XML
#   summary fails the release (undocumented-baseline.txt names none). tools/check.sh holds it current in every commit (stage `docs`).
# KILN_API_BASELINE: the last release's Kiln.Api .nupkg (or it unzipped) for pack.sh's API compat gate; unset: a loud
#   warning, at the start and in the closing text, and the release goes on.
# Builds run one at a time (they share Cargo's build directory); KILN_RELEASE_NICE=1 runs them at the lowest priority.
# The sample mods build in one MSBuild run (the shared projects once, the samples side by side), zipped one by one; when
#   that run fails, each sample builds alone as before and a failing one is left out.
# KILN_BUILD_REMOTE=win1: the sample mods build on kiln-win1 (ssh kiln-win1, the owner's Windows PC; lib/remote-win1.sh)
#   while this machine builds the rest, and come back as output folders that are zipped here like a local build's. Same
#   .NET SDK version there; nothing signed or secret goes there. A failed or unreachable win1 means a local build of the
#   samples (noted in the log), never a missing sample. Off by default. With KILN_BUILD_REMOTE_NATIVE=1 as well, the native Windows DLLs build there too
#   (lib/remote-win1-native.sh: the same clang-cl/lld-link release and this machine's xwin CRT/SDK, the same path maps; the
#   DLLs equal this machine's but for the PDB id) and the runtime stage builds only the rest (KILN_NATIVE_PREBUILT).
# The Windows launcher builds (direct, Nexus) run beside the Linux ones once the notices are complete (their frontend in
#   launcher/dist-windows); KILN_RELEASE_SERIAL_LAUNCHERS=1 runs them one after the other.
# NuGet: the builds restore into a private package cache, $work/nuget-packages (KILN_RELEASE_NUGET_PACKAGES), never
#   ~/.nuget/packages: a trial run of a version leaves no kiln.* packages of it behind for later local builds.
# One run at a time per work folder and per KILN_GLIBC_DIR (lib/run-locks.sh): each run holds <work>/.release.lock and
#   KILN_GLIBC_DIR/.release.lock until it ends, and a second run on either stops at once, before it changes anything,
#   naming the lock and the run that holds it. Two releases at once need their own --work and KILN_GLIBC_DIR.
# The export's files get the time of the export (after the source tarball, which keeps the commit's): a reused cargo
#   folder then rebuilds the workspace crates every run instead of keeping another commit's build. kilnctl, kiln and
#   kiln-eml-inspect (Linux) are copied into <work>/linux-bin right after their build; the rest of the run takes them there.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
lib=$here/lib
source "$here/linux-build/paths.sh"

version="" commit=HEAD out="" work="" notes="" worktree=0 skip_windows=0 checks=1 force=0 host_build=0 nexus=1 beta="" want_channel="" launcher_only=0
while (($#)); do
  case $1 in
    --commit) commit=$2; shift 2 ;;
    --out) out=$2; shift 2 ;;
    --work) work=$2; shift 2 ;;
    --notes) notes=$(realpath "$2"); shift 2 ;;
    --worktree) worktree=1; shift ;;
    --skip-windows) skip_windows=1; shift ;;
    --no-checks) checks=0; shift ;;
    --launcher-only) launcher_only=1; checks=0; shift ;;
    --force) force=1; shift ;;
    --host-build) host_build=1; shift ;;
    --no-nexus) nexus=0; shift ;;
    --beta) beta=$2; shift 2 ;;
    --channel) want_channel=$2; shift 2 ;;
    -h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) [[ -z $version ]] || { echo "one version only" >&2; exit 2; }; version=$1; shift ;;
  esac
done
[[ -n $version ]] || { echo "usage: $0 VERSION [options] (tools/release/version.sh dev|stable prints one)" >&2; exit 2; }
# --worktree only in a run that is plainly a test: an explicit --out that is not a release folder (~/Kiln-release/kiln-*,
# the default --out, or ~/Kiln-release itself), checked before anything else is done. A real run passed it once
# (0.2.0-dev.28) and its README.txt said "a test build".
if ((worktree)); then
  releases=$(realpath -m "$HOME/Kiln-release")
  if [[ -z $out ]]; then
    echo "release: --worktree is for test runs only and needs --out DIR, not a release folder (e.g. --out $releases/trial/kiln-$version): a test build of the working tree, never published. A release builds a commit: drop --worktree" >&2
    exit 2
  fi
  # Keep the spelling too: a release folder may itself be a symlink to storage elsewhere.
  for candidate in "$(realpath -ms -- "$out")" "$(realpath -m -- "$out")"; do
    case $candidate in
      "$HOME/Kiln-release"|"$HOME/Kiln-release"/kiln-*|"$releases"|"$releases"/kiln-*) echo "release: --worktree is for test runs only: --out $out is a release folder ($releases/kiln-*); give a test folder (e.g. $releases/trial/kiln-$version), or drop --worktree for a release" >&2; exit 2 ;;
    esac
  done
fi
"$here/version.sh" check "$version"
case $version in *-*) channel=dev ;; *) channel=stable ;; esac
# a test build (--beta N): name = the folder's, the files', the tag's and the download path's; build_id = what its
# files are signed for and its manifest announces; the launcher itself keeps $version (releasing.md, "Test builds")
case $want_channel in
  ""|beta) ;;
  dev|stable) [[ $want_channel == "$channel" ]] || { echo "release: --channel $want_channel does not fit $version (a $channel version)" >&2; exit 2; } ;;
  *) echo "release: --channel is stable, dev or beta, not '$want_channel'" >&2; exit 2 ;;
esac
[[ $want_channel != beta || -n $beta ]] || { echo "release: --channel beta needs --beta N (the test build's number)" >&2; exit 2; }
[[ -z $beta || $want_channel == "" || $want_channel == beta ]] || { echo "release: --beta is for the beta channel" >&2; exit 2; }
name=$version build_id=$version
if [[ -n $beta ]]; then
  [[ $beta =~ ^[1-9][0-9]{0,8}$ ]] || { echo "release: --beta takes the test build's number (1, 2, ...), not '$beta'" >&2; exit 2; }
  channel=beta name=$version-beta.$beta build_id=$version+beta.$beta nexus=0
fi
# baked into the launcher by build.rs (empty: the channel follows the version); never inherited from the caller
export KILN_LAUNCHER_CHANNEL=${beta:+beta} KILN_LAUNCHER_BETA=$beta KILN_LAUNCHER_BASE_RELEASE=""
# the Nexus Mods build: never for a test build (test builds go to frostdev.io and GitHub only)
nexus_build=1
[[ $channel == beta ]] && nexus_build=0
out=$(realpath -m "${out:-$HOME/Kiln-release/kiln-$name}")
work=$(realpath -m "${work:-$repo/.out/release-work}")
(( $(id -u) != 0 )) || { echo "release: run as the release owner, not root" >&2; exit 1; }
safe_dir "$out"
safe_dir "$work"
disjoint_dirs "$out" "$work"
for d in "$out" "$work"; do
  [[ $repo != "$d" && $repo != "$d/"* ]] || { echo "release: directory contains the repository: $d" >&2; exit 2; }
done
if ((!host_build)); then
  glibc=$(realpath -ms "${KILN_GLIBC_DIR:-$repo/.out/glibc}")
  safe_dir "$glibc"
  disjoint_dirs "$glibc" "$work"
  disjoint_dirs "$glibc" "$out"
fi
key=$HOME/.config/kiln-release/updater.key
[[ -f $key && -f $key.password ]] || { echo "no updater key at $key (+ .password): see releasing.md" >&2; exit 1; }
# The roadmap (tools/release/roadmap.py, releasing.md "The roadmap"): a release needs its entry on frostdev.io/kiln/roadmap
# (the first upcoming one in tools/release/roadmap/roadmap.json), which publish.sh moves to the released ones. Checked
# before anything is built; a test build (--beta, --worktree) only warns.
if [[ $channel == beta ]] || ((worktree)); then
  python3 -I "$here/roadmap.py" check "$version" ||
    echo "WARNING: the roadmap has no usable entry for $version (above); a test build goes on, the release will not" >&2
else
  python3 -I "$here/roadmap.py" check "$version" ||
    { echo "release: $version cannot ship without its roadmap entry (above): add it to tools/release/roadmap/roadmap.json (tools/release/roadmap/README.md), commit, and build again" >&2; exit 1; }
fi
# One release at a time per work folder and per Linux build folder (KILN_GLIBC_DIR), taken before anything is removed
# or written (the --force removal of $out below included): two runs sharing one swap each other's files (the
# 0.2.0-dev.30 T1b trial shipped the kilnctl another run built minutes later in the same KILN_GLIBC_DIR/cargo). Held
# until this run ends (lib/run-locks.sh).
source "$lib/run-locks.sh"
lock_info="release $name, out $out, since $(date -u +%Y-%m-%dT%H:%M:%SZ)"
release_lock "$work" "another release runs in the work folder $work: wait for it, or give this one its own --work" "$lock_info" || exit 1
if ((!host_build)); then
  release_lock "$glibc" "another release builds in KILN_GLIBC_DIR=$glibc: wait for it, or give this one its own KILN_GLIBC_DIR" "$lock_info" || exit 1
fi
if [[ -e $out && -n $(ls -A "$out" 2>/dev/null) ]]; then
  ((force)) || { echo "$out is not empty (use --force to replace it)" >&2; exit 1; }
  rm -rf "$out"
fi
mkdir -p "$out" "$work"
remote_pid=""
cleanup_release() {
  rm -rf -- "$work/appimage-root"
  # the kiln-win1 sample build (KILN_BUILD_REMOTE), when the release stops before it is collected
  if [[ -n $remote_pid ]]; then kill "$remote_pid" 2>/dev/null || true; fi
  if [[ -n ${native_pid:-} ]]; then kill "$native_pid" 2>/dev/null || true; fi
  if [[ -n ${win_pid:-} ]]; then kill "$win_pid" 2>/dev/null || true; fi
  if [[ -n ${sdk_pid:-} ]]; then kill "$sdk_pid" 2>/dev/null || true; fi
}
trap cleanup_release EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
log=$work/release.log
: >"$log"
# KILN_RELEASE_NICE=1: build at the lowest CPU and IO priority (on a machine someone is using)
if [[ ${KILN_RELEASE_NICE:-} == 1 ]]; then low() { nice -n 19 ionice -c 3 "$@"; }; else low() { "$@"; }; fi
t0=$(date +%s)
step() { printf '\n== %s (%ss)\n' "$*" $(($(date +%s) - t0)) | tee -a "$log"; }
note() { echo "$*" | tee -a "$log"; }
die() { echo "release: $*" | tee -a "$log" >&2; exit 1; }
quiet() { "$@" >>"$log" 2>&1 || { tail -40 "$log" >&2; die "failed: $*"; }; }

# ---------------------------------------------------------------- 0. the agent API index
# Generated before the export so the export carries it. A release exports a commit: the index has to be committed, so a release
# whose index changed stops here (the files are regenerated in the working tree: commit them, then run again). Only for a
# release of the checked-out commit with committed inputs; a test build, a launcher-only build and --commit of another revision
# export the committed index as it is (tools/check.sh's `index` stage holds it current in every commit).
api_index=$repo/agent/skills/kiln-modding/references/api-index api_index_xml=""
if ((!launcher_only)) && ((!worktree)) && [[ $channel != beta ]] && [[ -f $repo/tools/docs-site/api_index.py ]]; then
  step "agent API index"
  if [[ $(git -C "$repo" rev-parse HEAD) != "$(git -C "$repo" rev-parse --verify "$commit^{commit}" 2>/dev/null)" ]]; then
    note "API index: $commit is not the checked-out commit: not regenerated, the committed index is exported"
  elif ! git -C "$repo" diff --quiet HEAD -- managed native/core/abi crates/sfi/kiln-abi agent/skills/kiln-modding tools/docs-site/api_index.py; then
    die "the working tree has uncommitted changes in the API inputs or the agent API index (git status): a release builds a commit, commit them first"
  else
    api_index_xml=${KILN_API_XML:-$work/api-index/artifacts/bin}
    if [[ -n ${KILN_API_XML:-} ]]; then
      quiet low python3 -I "$repo/tools/docs-site/api_index.py" --xml "$KILN_API_XML"
    else
      quiet low python3 -I "$repo/tools/docs-site/api_index.py" --build-dir "$work/api-index"
    fi
    if [[ -n $(git -C "$repo" status --porcelain -- "$api_index") ]]; then
      git -C "$repo" status --short -- "$api_index" | head -20 | tee -a "$log"
      die "the agent API index was out of date and is regenerated in $repo (above): commit it, new and removed parts included (git -C $repo add -A -- agent/skills/kiln-modding/references/api-index && git -C $repo commit -m 'skill: regenerate the API index' -- agent/skills/kiln-modding/references/api-index) and run the release again"
    fi
    note "API index: current"
  fi
fi

# ---------------------------------------------------------------- 1. export
src=$work/src
step "export $( ((worktree)) && echo "working tree" || echo "$commit") to $src"
rm -rf "$src" && mkdir -p "$src"
if ((worktree)); then
  sha=$(git -C "$repo" rev-parse --short=7 HEAD)
  export KILN_GIT=$sha
  export SOURCE_DATE_EPOCH=$(date +%s)
  built_from="the working tree at $sha (a test build, not a release)"
  (cd "$repo" && git ls-files -z -co --exclude-standard | python3 -c '
import os, sys
for p in sys.stdin.buffer.read().split(b"\0"):
    if p and os.path.isfile(p) and not os.path.islink(p): sys.stdout.buffer.write(p + b"\0")' | tar --null -T - -cf -) | tar -xf - -C "$src"
  note "working tree of $sha (with uncommitted and untracked files): a test build, never a release"
else
  full=$(git -C "$repo" rev-parse --verify "$commit^{commit}")
  export KILN_GIT=${full:0:7}
  export SOURCE_DATE_EPOCH=$(git -C "$repo" log -1 --format=%ct "$full")
  git -C "$repo" archive --format=tar "$full" | tar -xf - -C "$src"
  built_from="commit $KILN_GIT"
fi
# the source as exported, before any build writes into it
mkdir -p "$work/source"
rm -f "$work"/source/*.tar.gz
tar --sort=name --owner=0 --group=0 --numeric-owner --mode='u=rwX,go=rX' --mtime="@$SOURCE_DATE_EPOCH" -C "$work" \
  --transform "s|^src|kiln-$version|" -czf "$work/source/kiln-$version-src.tar.gz" src
# the exported files (NUL-separated): the modding site's guide stages these pages and checks its links against them,
# since the export has no .git (tools/docs-site/stage-guide.py, KILN_DOCS_TRACKED)
(cd "$src" && find . -type f -printf '%P\0') > "$work/src-files"
# KILN_RELEASE_OVERLAY=DIR (trial builds of build-script changes only): DIR's files are copied over the export after
# the source tarball, so a trial builds one commit's sources (and stamps) with newer tools/package or tools/launcher
# scripts, for a byte-by-byte comparison with a build of that commit. The folder says so; never publish it.
if [[ -n ${KILN_RELEASE_OVERLAY:-} ]]; then
  [[ -d $KILN_RELEASE_OVERLAY ]] || die "KILN_RELEASE_OVERLAY=$KILN_RELEASE_OVERLAY is no folder"
  cp -a "$KILN_RELEASE_OVERLAY"/. "$src"/
  built_from="$built_from with build scripts from $KILN_RELEASE_OVERLAY (a test build, not a release)"
  note "WARNING: KILN_RELEASE_OVERLAY: $(cd "$KILN_RELEASE_OVERLAY" && find . -type f | sed 's,^\./,,' | tr '\n' ' ')copied over the export: a test build, never publish it"
fi
[[ $(sed 's/[-+].*//' "$src/VERSION") == "${version%%-*}" ]] || die "VERSION in the export is $(cat "$src/VERSION"), not for $version"
# The API gate (tools/release/api-gate.sh), before any build: no line of a PublicAPI.Shipped.txt removed since the
# previous v* release, and new API only with a raised API version (Kiln.Mods.ApiVersion), so a mod can ask for it.
previous_release() {
  local ref=$1 channel=$2
  [[ $channel == beta ]] || ref=$ref^
  git -C "$repo" describe --tags --abbrev=0 --match 'v*' --exclude 'v*-beta.*' "$ref" 2>/dev/null || true
}
if ((worktree)); then
  gate_new=(--worktree)
  prev=$(git -C "$repo" describe --tags --abbrev=0 --match 'v*' --exclude 'v*-beta.*' HEAD 2>/dev/null || true)
else
  # GitHub makes the v* tags when a release is published: fetch them, or the gate and the notes start too early
  git -C "$repo" fetch -q --tags origin 2>/dev/null || note "release: could not fetch tags; the API gate and the notes may start too early"
  gate_new=("$full")
  # a test build's tag (v<version>-beta.<n>) is no release: the gate and the notes start at the release before
  prev=$(previous_release "$full" "$channel")
fi
if [[ $channel == beta ]]; then
  KILN_LAUNCHER_BASE_RELEASE=$(python3 "$lib/release.py" beta-floor "$prev" "$build_id") || die "cannot determine the release this beta is cut over; fetch release tags first"
  export KILN_LAUNCHER_BASE_RELEASE
fi
if [[ -z $prev ]]; then
  note "WARNING: no previous v* tag: the API gate has nothing to compare with (removed API and the API version are not checked)"
else
  step "API gate: the public API since $prev"
  "$here/api-gate.sh" removed "$prev" "${gate_new[@]}" 2>&1 | tee -a "$log" || die "shipped API was removed since $prev (above): the mod API is additive"
  "$here/api-gate.sh" version-rule "$prev" "${gate_new[@]}" 2>&1 | tee -a "$log" || die "new API needs a higher API version (above)"
fi
# The API compat gate in pack.sh (tools/release/apicompat.sh) compares the SDK's assemblies with the last release's:
# KILN_API_BASELINE, a folder (an unzipped Kiln.Api.<ver>.nupkg) or that .nupkg. A wrong one stops here, not an hour
# later in pack.sh; none is a loud warning (pack.sh's own runs under quiet, into the log only) and the release goes on.
api_compat=1
if [[ -n ${KILN_API_BASELINE:-} ]]; then
  b=$(realpath -m -- "$KILN_API_BASELINE")
  if [[ -d $b ]]; then
    [[ -n $(find "$b" -name Kiln.Api.dll -type f -print -quit) ]] || die "KILN_API_BASELINE=$b holds no Kiln.Api.dll (an unzipped Kiln.Api.<ver>.nupkg of the last release)"
  elif [[ -f $b && $b == *.nupkg ]]; then
    python3 -I -c 'import sys, zipfile; sys.exit(0 if any(n.endswith("/Kiln.Api.dll") for n in zipfile.ZipFile(sys.argv[1]).namelist()) else 1)' "$b" 2>/dev/null \
      || die "KILN_API_BASELINE=$b is not a Kiln.Api .nupkg holding Kiln.Api.dll"
  else
    die "KILN_API_BASELINE=$b does not exist or is neither a folder nor a .nupkg"
  fi
  export KILN_API_BASELINE=$b
  note "API compat gate: pack.sh compares the SDK's assemblies with $b"
else
  api_compat=0
  note "WARNING: ****************************************************************************************"
  note "WARNING: KILN_API_BASELINE is not set: pack.sh's API compat gate will NOT run."
  note "WARNING: Nothing compares this release's SDK assemblies (Kiln.Api, Kiln.Testing, the services) with the last"
  note "WARNING: release's: a removed or changed public member would ship unnoticed (the PublicAPI record gate above"
  note "WARNING: still ran). Set KILN_API_BASELINE to the last release's Kiln.Api.<ver>.nupkg, or to it unzipped."
  note "WARNING: ****************************************************************************************"
fi
# The full release version in the builds: native/CMakeLists.txt (kiln_version.h: the core's and the proxy's version),
# managed/KilnVersion.props (the assemblies' informational version and Kiln.Api's Kiln.Build stamp) and
# tools/package/stage.sh (kiln-runtime.json, so the runtime zip's name) read the export's VERSION. Written after the
# source tarball and the check above: the tarball keeps VERSION as committed.
printf '%s\n' "$version" >"$src/VERSION"
export KilnVersion=$version
# ShroudForge's runtime (submodule third_party/shroudforge, a GPL-3 component): git archive leaves submodules out, so
# export the fork at the pinned commit with its own submodules, ship that as its corresponding source, and build from it
sfpin=$(git -C "$repo" ls-tree "${full:-HEAD}" third_party/shroudforge | awk '$2 == "commit" {print $3}')
if [[ -d $src/gpl/kiln-eml && -z $sfpin ]]; then die "release has no pinned ShroudForge submodule"; fi
# A release always builds the exported pin, regardless of a developer override.
unset KILN_SHROUDFORGE_SRC
if [[ -n $sfpin ]]; then
  "$lib/sf-source.sh" "$repo/third_party/shroudforge" "$sfpin" "$work/source/shroudforge-kiln-${sfpin:0:12}-src.tar.gz" \
    "$src/third_party/shroudforge" >>"$log" 2>&1 || die "cannot export ShroudForge's source at $sfpin (git submodule update --init --recursive third_party/shroudforge)"
  note "ShroudForge runtime source: source/shroudforge-kiln-${sfpin:0:12}-src.tar.gz"
fi
date_iso=$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%d)
# git archive gives every exported file the commit's time, and Cargo counts a crate as built when no input is newer than
# its last build: a reused target folder ($work/cargo, KILN_GLIBC_DIR/cargo) that last built a later commit would keep
# that commit's code. The whole export (ShroudForge's and an overlay's files too) gets the time of now instead, newer
# than any earlier build; the source tarballs above keep the commit's (lib/run-locks.sh, fresh_mtimes).
fresh_mtimes "$src" "$(date +%s)" || die "cannot set the export's file times ($src)"

# ---------------------------------------------------------------- 2. environment
export KILN_OUT=$work/out CARGO_TARGET_DIR=$work/cargo
# the SDK packages carry the release's version (managed/Kiln.Sdk/pack.sh), and kiln (the CLI) is built knowing it
export KILN_SDK_VERSION=$version
rm -rf "$KILN_OUT" && mkdir -p "$KILN_OUT"
# A private NuGet package cache for every build of the release: pack.sh's Kiln.TestKit restores this version's Kiln.Api
# from its local feed, and NuGet keeps a package by id and version for good, so in ~/.nuget/packages a trial run would
# shadow the published packages of the same version in every later local build. Third-party packages stay for the next
# run; this version's kiln.* packages in it go first (a trial's packages of the same version are not this build's).
# KILN_RELEASE_NUGET_PACKAGES: another folder.
export NUGET_PACKAGES=${KILN_RELEASE_NUGET_PACKAGES:-$work/nuget-packages}
mkdir -p "$NUGET_PACKAGES" && rm -rf -- "$NUGET_PACKAGES"/kiln.*/"${version,,}"
note "NuGet package cache: $NUGET_PACKAGES (private: ~/.nuget/packages gets none of this release's kiln.* $version packages)"
# no build paths in the binaries: the home folder and the export become neutral prefixes; KILN_RELEASE_BUILD drops
# the "tree this was built from" defaults of kilnctl and kiln
unset RUSTFLAGS
export CARGO_ENCODED_RUSTFLAGS="--remap-path-prefix=$work=/build/work"$'\x1f'"--remap-path-prefix=$HOME=/build"$'\x1f'"--remap-path-prefix=$src=/kiln" KILN_RELEASE_BUILD=1
# Public, Nexus-approved desktop registration, shared by the direct/Nexus Linux and Windows builds.
# Launcher build.rs rejects official builds without valid values; approval/live-provider proof remains manual.
export KILN_NEXUS_ENABLED KILN_NEXUS_OAUTH_CLIENT_ID KILN_NEXUS_OAUTH_REDIRECT_URI
export ContinuousIntegrationBuild=true DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 NO_STRIP=${NO_STRIP:-true}
# Rust: one toolchain for everything this machine builds (the Windows cross-builds; with --host-build the Linux files
# too): the version the Linux build image has (linux-build/Dockerfile, RUST_VERSION). rustup's `stable` can move while
# a release builds: 0.2.0-dev.26's first build stopped with "not applicable to the 'stable' toolchain" when it did. A
# version number cannot move. KILN_RELEASE_RUST names another version for a trial build.
pin_rust() {
  local target=$1 want
  want=${KILN_RELEASE_RUST:-$(sed -n 's/^ARG RUST_VERSION=//p' "$here/linux-build/Dockerfile")}
  [[ $want =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] \
    || die "no Rust version to build with: '$want' (ARG RUST_VERSION in tools/release/linux-build/Dockerfile, or KILN_RELEASE_RUST)"
  if command -v rustup >/dev/null 2>&1; then
    rustup run "$want" rustc --version >/dev/null 2>&1 \
      || die "Rust $want is not installed here: rustup toolchain install $want --profile minimal --target $target"
    rustup target list --toolchain "$want" --installed 2>/dev/null | grep -qx "$target" \
      || die "Rust $want has no $target target here: rustup target add --toolchain $want $target"
    export RUSTUP_TOOLCHAIN=$want
  else
    [[ $(rustc --version 2>/dev/null) == "rustc $want "* ]] \
      || die "rustc here is '$(rustc --version 2>&1 | head -1)', but the release builds with Rust $want (install rustup, or that version)"
    # Without rustup nothing lists the installed targets: ask rustc where that target's library would be, and look.
    local lib
    lib=$(rustc --print target-libdir --target "$target" 2>/dev/null) || lib=
    [[ -n $lib ]] && compgen -G "$lib/libstd-*.rlib" >/dev/null \
      || die "rustc $want here has no $target library (looked in '${lib:-nowhere: rustc does not know the target}'): install that target for it, or use rustup (rustup toolchain install $want --profile minimal --target $target)"
  fi
  note "Rust $want for the files built on this machine (the Linux build image has the same)"
}
pin_rust x86_64-pc-windows-msvc
cache=${KILN_RELEASE_CACHE:-$HOME/.cache/kiln-release}
mkdir -p "$cache"
# pinned downloads (each script checks its hash): .NET for the runtime, netcoredbg for the vsix; reused when cached
seed() { # name, destination, candidates...
  local name=$1 dest=$2; shift 2
  if [[ ! -f $cache/$name ]]; then for c in "$@"; do [[ -f $c ]] && { cp "$c" "$cache/$name"; break; }; done; fi
  if [[ -f $cache/$name ]]; then mkdir -p "$(dirname "$dest")" && cp "$cache/$name" "$dest"; fi
}
seed dotnet-runtime-win-x64.zip "$src/.cache/dotnet-win-x64/runtime.zip" "$repo/.cache/dotnet-win-x64/runtime.zip"
# the Python the Windows launcher ships for the cook tools (tools/package/fetch-python.sh checks its sha256)
pyv=$(sed -n 's/^VERSION=\(.*\)$/\1/p' "$src/tools/package/fetch-python.sh")
seed "python-$pyv-embed-amd64.zip" "$src/.cache/python-win-x64/python.zip" "$repo/.cache/python-win-x64/python.zip"
nc=$(sed -n 's/^const VERSION = "\(.*\)";/\1/p' "$src/tools/vscode/scripts/fetch-netcoredbg.mjs")
seed "netcoredbg-$nc-win64.zip" "$KILN_OUT/vscode/netcoredbg/$nc/netcoredbg-win64.zip" "$repo"/.out/*/netcoredbg/"$nc"/netcoredbg-win64.zip
signing=0
if "$here/authenticode.sh" available; then signing=1; note "Authenticode: signing Windows files"; else note "Windows files unsigned (no authenticode.env)"; fi
# The Windows launcher builds (Tauri's NSIS bundler, on this machine) need makensis, and a signed setup also NSIS's own
# plugins: Tauri copies them from $NSIS_PATH, else /usr/share/nsis (tools/launcher/build-windows.sh looks beside makensis
# too). Checked here, before an hour of building, not at the bundle step.
if ((!skip_windows)); then
  makensis=$(command -v makensis) || die "the Windows setup needs makensis (NSIS) on PATH"
  if ((signing)) && [[ -z ${NSIS_PATH:-} && ! -d /usr/share/nsis/Plugins/x86-unicode ]]; then
    nsis_root=$(dirname "$(dirname "$(readlink -f "$makensis")")")
    for d in "$nsis_root/share/nsis" "$nsis_root"; do
      if [[ -d $d/Plugins/x86-unicode ]]; then export NSIS_PATH=$d; break; fi
    done
    [[ -n ${NSIS_PATH:-} ]] || die "a signed Windows setup needs NSIS's plugins (Plugins/x86-unicode): none in /usr/share/nsis or beside $makensis; set NSIS_PATH to the NSIS folder that has them"
  fi
  ((signing)) && note "NSIS plugins for the signed setup: ${NSIS_PATH:-/usr/share/nsis}"
fi
# Tauri keeps its NSIS toolset in $XDG_CACHE_HOME/tauri/NSIS (else ~/.cache) and, on Linux, removes and rebuilds it on
# every Windows build (it checks for an NSIS.exe it never puts there), so two builds sharing it race. Each run's Windows
# builds get their own cache folder in its work folder; cargo-xwin's MSVC CRT/SDK cache (large, downloaded once) stays
# the shared one.
win_env=(env XDG_CACHE_HOME="$work/xdg-cache" XWIN_CACHE_DIR="${XWIN_CACHE_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/cargo-xwin}")
sign_pe() { if ((signing)); then "$here/authenticode.sh" sign "$@" | tee -a "$log"; fi; }
# linux COMMAND...: runs a Linux build command (cargo, build-linux.sh, notices.py for the AppImage) where the Linux files
# are built: the Ubuntu 22.04 container (tools/release/linux-build/run.sh; $src, $work and tools/release mounted at
# their own paths), or this machine with --host-build. $lin_target is its cargo target dir. The updater key never goes
# into the container: the finished AppImage is signed on this machine (8. below).
glibc_max=2.35
if ((host_build)); then
  lin_target=$CARGO_TARGET_DIR
  linux() { low "$@"; }
  note "--host-build: the Linux files are built on this machine (glibc $(ldd --version | sed -n '1s/.* //p')): NOT portable, never publish this folder"
else
  export KILN_GLIBC_DIR=$glibc
  lin_target=$glibc/cargo
  # the container's crate cache shows as /build/.cargo, like the host's ~/.cargo under the /build prefix above
  lin_rustflags="$CARGO_ENCODED_RUSTFLAGS"$'\x1f'"--remap-path-prefix=$glibc/cargo-home=/build/.cargo"
  linux() {
    local nice=()
    [[ ${KILN_RELEASE_NICE:-} == 1 ]] && nice=(--nice)
    CARGO_TARGET_DIR=$lin_target CARGO_ENCODED_RUSTFLAGS=$lin_rustflags "$here/linux-build/run.sh" "${nice[@]}" \
      --mount "$src" --mount "$work" --ro "$here" \
      --env KILN_GIT --env SOURCE_DATE_EPOCH --env KILN_OUT --env CARGO_TARGET_DIR --env KILN_SDK_VERSION --env CARGO_ENCODED_RUSTFLAGS \
      --env KILN_NEXUS_ENABLED --env KILN_NEXUS_OAUTH_CLIENT_ID --env KILN_NEXUS_OAUTH_REDIRECT_URI \
      --env KILN_RELEASE_BUILD --env KILN_COOK_HELPERS --env ContinuousIntegrationBuild --env NO_STRIP \
      --env KILN_LAUNCHER_CHANNEL --env KILN_LAUNCHER_BETA --env KILN_LAUNCHER_BASE_RELEASE -- "$@"
  }
  step "Linux build image (Ubuntu 22.04, glibc $glibc_max)"
  image=$("$here/linux-build/image.sh" 2>>"$log") || { tail -40 "$log" >&2; die "the Linux build image did not build"; }
  KILN_LINUX_IMAGE=$(docker image inspect --format '{{.Id}}' "$image")
  export KILN_LINUX_IMAGE
  note "Linux files: built in $image ($KILN_LINUX_IMAGE); cargo target $lin_target"
fi

# ---------------------------------------------------------------- sample mods: projects, output folders, kiln-win1
# The sample projects zipped from their output folders (a sample on Kiln.Sdk packs itself, below) and each one's
# OutDir, in one MSBuild run (lib/sample-outdirs.proj). With KILN_BUILD_REMOTE=win1 they build there from now on.
remote=${KILN_BUILD_REMOTE:-}
[[ -z $remote || $remote == win1 ]] || die "KILN_BUILD_REMOTE=$remote: the only remote builder is win1"
samples_tsv=$work/samples-outdirs.tsv
: >"$samples_tsv"
if ((!launcher_only)); then
  : >"$work/samples-projects.txt"
  for proj in "$src"/mods/samples/*/*.csproj; do grep -q 'Sdk="Kiln.Sdk' "$proj" || printf '%s\n' "$proj" >>"$work/samples-projects.txt"; done
  if [[ -s $work/samples-projects.txt ]]; then
    dotnet msbuild "$here/lib/sample-outdirs.proj" -nologo -t:Show -p:KilnSampleList="$work/samples-projects.txt" \
      -p:ArtifactsPath="$KILN_OUT/dotnet" 2>>"$log" | awk -F'|' '/KILN-OUTDIR\|/ {print $2 "\t" $3}' >"$samples_tsv" \
      || die "cannot read the sample projects' output folders (lib/sample-outdirs.proj; log: $log)"
    [[ $(wc -l <"$samples_tsv") == $(wc -l <"$work/samples-projects.txt") ]] \
      || die "the sample projects' output folders: $(wc -l <"$samples_tsv") of $(wc -l <"$work/samples-projects.txt") (lib/sample-outdirs.proj)"
    while IFS=$'\t' read -r proj o; do
      [[ $o == "$KILN_OUT/dotnet/"* ]] || die "$proj: unexpected output folder '$o'"
    done <"$samples_tsv"
  fi
  if [[ $remote == win1 && -s $samples_tsv ]]; then
    step "sample mods: building on kiln-win1 meanwhile (lib/remote-win1.sh; log $work/remote-win1.log)"
    "$lib/remote-win1.sh" samples "$src" "$work/remote-win1" "$samples_tsv" >"$work/remote-win1.log" 2>&1 &
    remote_pid=$!
  fi
fi
# KILN_BUILD_REMOTE_NATIVE=1 (with KILN_BUILD_REMOTE=win1): the native Windows DLLs (CMake targets) on kiln-win1 too, with the same clang-cl, lld-link and xwin CRT/SDK as here
# (lib/remote-win1-native.sh): collected before the runtime stage, which then builds only what they leave out.
unset KILN_NATIVE_PREBUILT
native_pid=""
if [[ $remote == win1 && ${KILN_BUILD_REMOTE_NATIVE:-} == 1 ]]; then
  step "native DLLs: building on kiln-win1 meanwhile (lib/remote-win1-native.sh; log $work/remote-win1-native.log)"
  RELEASE_WORK=$work "$lib/remote-win1-native.sh" "$src" "$work/remote-win1-native" >"$work/remote-win1-native.log" 2>&1 &
  native_pid=$!
fi

# ---------------------------------------------------------------- 3. dependencies, command-line tools
step "dependencies (bun, npm)"
(cd "$src/launcher" && quiet low bun install --frozen-lockfile)
(cd "$src/tools/vscode" && quiet low npm ci --no-audit --no-fund)
# The cook tools ship inside kiln and the launcher (kiln_manage::cooktools: crates/kiln-manage/build.rs embeds them,
# and soundcook's native helper for the target, from KILN_COOK_HELPERS; a release build stops without it). The Linux
# helper is built where the other Linux files are (their glibc), the Windows one with clang-cl and xwin like the
# native services. The Python the Windows launcher brings is fetched and verified here, before anything is built on it.
step "the cook tools' helper (Linux, Windows) and the pinned Python"
export KILN_COOK_HELPERS=$work/cook-helpers
rm -rf "$KILN_COOK_HELPERS" && mkdir -p "$KILN_COOK_HELPERS"
(cd "$src" && quiet linux bash tools/soundcook/native/build.sh)
cp "$KILN_OUT/soundcook/bin/kiln-soundcook" "$KILN_COOK_HELPERS/kiln-soundcook"
(cd "$src" && quiet low bash tools/soundcook/native/build.sh --windows)
cp "$KILN_OUT/soundcook/win/bin/kiln-soundcook.exe" "$KILN_COOK_HELPERS/kiln-soundcook.exe"
sign_pe "$KILN_COOK_HELPERS/kiln-soundcook.exe"
if ((!skip_windows)); then quiet "$src/tools/package/fetch-python.sh"; fi
step "kilnctl and kiln (Linux, Windows)"
(cd "$src" && quiet linux cargo build --release --locked -p kilnctl -p kiln-cli)
win=x86_64-pc-windows-msvc
(cd "$src" && quiet low cargo xwin build --release --locked -p kilnctl -p kiln-cli --target $win)
# this run's own copies of the Linux CLIs, taken right after their build: the runtime bundle, the folder and the checks
# use these, never the cargo target folder (KILN_GLIBC_DIR/cargo can be shared with other builds)
linux_bin=$work/linux-bin
rm -rf "$linux_bin" && mkdir -p "$linux_bin"
for f in kilnctl kiln; do
  [[ -s $lin_target/release/$f ]] || die "no $f in $lin_target/release after the build"
  install -m 755 "$lin_target/release/$f" "$linux_bin/$f"
done
kilnctl=$linux_bin/kilnctl
# kiln-eml's archive inspector for Linux (component eml, kiln/tools/eml-inspect/linux-x86_64/) is built where the other
# Linux files are, for their glibc; the stage takes it from KILN_EML_INSPECT_LINUX (its Windows .exe builds in the stage)
if [[ -f $src/gpl/kiln-eml/kiln-eml/src/kiln_eml_inspect.rs ]]; then
  step "kiln-eml's archive inspector (Linux)"
  (cd "$src/gpl/kiln-eml" && quiet linux cargo build --release --locked -p kiln-eml --bin kiln-eml-inspect)
  [[ -s $lin_target/release/kiln-eml-inspect ]] || die "no kiln-eml-inspect in $lin_target/release"
  install -m 755 "$lin_target/release/kiln-eml-inspect" "$linux_bin/kiln-eml-inspect"
  export KILN_EML_INSPECT_LINUX=$linux_bin/kiln-eml-inspect
fi

# ---------------------------------------------------------------- 4. runtime
step "runtime: native + managed build, stage"
if [[ -n $native_pid ]]; then
  nrc=0; wait "$native_pid" || nrc=$?
  native_pid=""
  cat "$work/remote-win1-native.log" >>"$log"
  if ((nrc == 0)) && [[ -f $work/remote-win1-native/ok ]]; then
    export KILN_NATIVE_PREBUILT=$work/remote-win1-native/bin
    note "native DLLs: built on kiln-win1 ($(grep -o 'build rc=0, [0-9]* s' "$work/remote-win1-native.log" || true)); the stage builds what they leave out"
  else
    note "native DLLs: the kiln-win1 build failed (exit $nrc, $work/remote-win1-native.log): building them here"
  fi
fi
stage=$work/stage
# the native DLLs name their PDB without a path (/pdbaltpath); the PDBs themselves hold the build machine's paths, so
# they stay out of the release and go to $work/symbols (keep them for crash reports of this build)
CFLAGS="/clang:-ffile-prefix-map=\"$work=/build/work\" /clang:-ffile-prefix-map=\"$HOME=/build\" /clang:-ffile-prefix-map=\"$src=/kiln\"" \
  CXXFLAGS="/clang:-ffile-prefix-map=\"$work=/build/work\" /clang:-ffile-prefix-map=\"$HOME=/build\" /clang:-ffile-prefix-map=\"$src=/kiln\"" LDFLAGS="/pdbaltpath:%_PDB%" \
  quiet low "$src/tools/package/stage.sh" "$stage"
rm -rf "$work/symbols"
while IFS= read -r -d '' pdb; do
  dll=${pdb%.pdb}.dll
  [[ -f $dll ]] && python3 -c 'import sys; d=open(sys.argv[1],"rb").read(); sys.exit(0 if b"RSDS" in d and b"LLD" in d else 1)' "$dll" || continue
  rel=${pdb#"$stage"/}
  mkdir -p "$work/symbols/$(dirname "$rel")"
  mv "$pdb" "$work/symbols/$rel"
done < <(find "$stage" -name '*.pdb' -print0)
note "native symbols (PDBs) kept out of the release: $(find "$work/symbols" -name '*.pdb' 2>/dev/null | wc -l) in $work/symbols"
step "third-party notices"
notices=$work/notices/THIRD-PARTY-NOTICES.txt
mapfile -t assets < <(find "$KILN_OUT/dotnet/obj" -name project.assets.json -not -path '*Tests*' 2>/dev/null | sort)
nuget_args=()
for a in "${assets[@]}"; do nuget_args+=(--nuget-assets "$a"); done
# Enshwick's loader (component enshwick, MIT): its licence and crates come from the pinned checkout it was built from
ew_version=$stage/enshwick/kiln/plugins/enshwick/enshwick-version.txt
[[ -s $ew_version ]] || die "Enshwick loader has no provenance for THIRD-PARTY-NOTICES: $ew_version"
mapfile -t ew_pin < "$ew_version"
[[ ${#ew_pin[@]} == 2 && ${ew_pin[0]} =~ ^[A-Za-z0-9._-]+$ && ${ew_pin[1]} =~ ^[0-9a-f]{40}$ ]] \
  || die "invalid Enshwick loader provenance: $ew_version"
ew_src=$KILN_OUT/runtime-src/enshwick-${ew_pin[0]}
[[ $(git -C "$ew_src" rev-parse HEAD 2>/dev/null) == "${ew_pin[1]}" ]] || die "Enshwick's checkout $ew_src is not the commit the loader was built from"
ew_status=$(git -C "$ew_src" -c core.fsmonitor=false status --porcelain --untracked-files=all --ignored) \
  || die "cannot verify Enshwick's source for notices: $ew_src"
[[ -z $ew_status ]] || die "Enshwick's source changed since the loader build: $ew_src"
nuget_args+=(--enshwick-src "$ew_src")
# the Windows launcher's Python, when this release builds the Windows files
if ((!skip_windows)); then nuget_args+=(--python-root "$src/.cache/python-win-x64/root"); fi
python3 "$lib/notices.py" --src "$src" --out "$notices" --version "$version" --dotnet-root "$src/.cache/dotnet-win-x64/root" \
  "${nuget_args[@]}" | tee -a "$log"
if [[ -f $stage/eml/kiln/plugins/kiln-eml.dll ]]; then
  what=kiln-eml; [[ -d $stage/eml/kiln/tools/eml-inspect ]] && what="kiln-eml and its archive inspector (kiln/tools/eml-inspect)"
  printf '\n%s corresponding source: source/kiln-%s-src.tar.gz, directory gpl/kiln-eml/.\n' "$what" "$version" >>"$notices"
fi
if [[ -d $stage/shroudforge/kiln/plugins/shroudforge ]]; then
  # the GPL-3 component's notice; its own licences ship in kiln/plugins/shroudforge/LICENSES
  { printf '\n%s\n' "ShroudForge runtime (component \"shroudforge\", kiln/plugins/shroudforge/)"
    cat "$stage/shroudforge/kiln/plugins/shroudforge/NOTICE.txt"
    for license in "$stage/shroudforge/kiln/plugins/shroudforge/LICENSES/"*; do
      printf '\n--- %s\n' "$(basename "$license")"
      cat "$license"
    done
    printf 'Corresponding source: source/shroudforge-kiln-%s-src.tar.gz in this release, and\nhttps://github.com/frostdev-ops/shroudforge at commit %s.\n' "${sfpin:0:12}" "$sfpin"; } >>"$notices"
fi
install -m 644 "$notices" "$stage/core/kiln/THIRD-PARTY-NOTICES.txt"
install -m 644 "$src/LICENSE" "$stage/core/kiln/LICENSE.txt"
if ((signing)); then
  mapfile -t pes < <(find "$stage" -type f \( -iname '*.dll' -o -iname '*.exe' \) -not -path '*/kiln/dotnet/*' | sort)
  step "Authenticode: ${#pes[@]} runtime files"
  sign_pe "${pes[@]}"
fi
# kiln-eml's archive inspector: each version.json binds its helper's SHA-256 (the launcher and kilnctl refuse a helper
# that does not match), and signing changed the Windows one. Written again for the files as they ship, then the sfi
# component and the inspector are checked once more.
if [[ -d $stage/eml/kiln/tools/eml-inspect && -f $src/tools/sfi/sfi_stage.py ]]; then
  for d in "$stage"/eml/kiln/tools/eml-inspect/*/; do
    python3 -I "$src/tools/sfi/sfi_stage.py" version "$src" "${d%/}" "$(basename "$d")" || die "kiln-eml's archive inspector: cannot write $d/version.json"
  done
  if ! python3 -I "$src/tools/sfi/sfi_stage.py" check "$src" "$stage" 2>&1 | tee -a "$log"; then
    die "the sfi component or kiln-eml's archive inspector is incomplete after signing (see above)"
  fi
fi
step "runtime bundle"
rm -rf "$work/dist" && mkdir -p "$work/dist"
quiet "$kilnctl" bundle create "$stage" --out "$work/dist" --git "$KILN_GIT" --epoch "$SOURCE_DATE_EPOCH"
runtime_zip=$(ls "$work"/dist/kiln-runtime-*.zip)
# Kiln is the universal loader: every compatibility plugin (native/plugins/*, kiln-eml, the ShroudForge runtime,
# Enshwick's loader) must be in the runtime, in a default component for both sides; a missing one stops the release
if ! python3 -I "$lib/plugins.py" check --src "$src" "$runtime_zip" 2>&1 | tee -a "$log"; then
  die "the runtime misses plugins (see above)"
fi
runtime_version=$(basename "$runtime_zip" .zip); runtime_version=${runtime_version#kiln-runtime-}
note "runtime $runtime_version"

# ---------------------------------------------------------------- 5. launcher
resources() { python3 -c 'import json,sys; print(json.dumps({"bundle":{"resources":{sys.argv[1]:"THIRD-PARTY-NOTICES.txt",sys.argv[2]:"LICENSE.txt"}}}))' "$1" "$src/LICENSE"; }
step "launcher: Linux"
# rpm versions can't hold "-": a dev build (X.Y.Z-dev.N) has no rpm
bundles=appimage,deb
[[ $channel == stable ]] && bundles+=,rpm
rm -rf "$lin_target/release/bundle"
if ! (cd "$src" && linux tools/launcher/build-linux.sh --bundles $bundles --runtime "$runtime_zip" --version "$version" \
    --config "$(resources "$notices")" >>"$log" 2>&1); then
  [[ $bundles == *,rpm ]] || die "the Linux launcher build failed (see $log)"
  note "the build with rpm failed; building AppImage and deb only"
  bundles=appimage,deb
  (cd "$src" && quiet linux tools/launcher/build-linux.sh --bundles $bundles --runtime "$runtime_zip" --version "$version" --config "$(resources "$notices")")
fi
lin=$KILN_OUT/bundles/linux
appimage=$(ls -t "$lin"/*.AppImage | head -1)
# the AppImage carries system libraries (GTK, WebKitGTK, ...): add them to the notices, then rebundle the AppImage.
# notices.py runs where the AppImage was built: the owners and licences come from that system's package database.
step "notices for the AppImage's bundled libraries"
rm -rf "$work/appimage-root" && mkdir -p "$work/appimage-root"
unsquashfs -q -n -o "$(python3 "$lib/release.py" offset "$appimage")" -d "$work/appimage-root/r" "$appimage" >/dev/null
(cd "$src" && linux python3 "$lib/notices.py" --src "$src" --out "$notices" \
  --append-appimage --appimage-root "$work/appimage-root/r") | tee -a "$log"
rm -rf "$work/appimage-root"
# The SDK packages and the VS Code extension (dotnet, node; nothing the launchers build or read) are made beside the
# launcher builds, into their own log, and collected before the sample mods (which share the dotnet artifacts folder).
# KILN_RELEASE_SERIAL_LAUNCHERS=1: in their place below, as before.
sdk_pid=""
sdk_and_vsix() {
  local log=$work/sdk.log
  [[ ${1:-} == bg ]] && : >"$log"
  step "SDK packages"
  quiet low "$src/managed/Kiln.Sdk/pack.sh"
  step "VS Code extension"
  (cd "$src/tools/vscode" && KILN_OUT="$KILN_OUT/vscode" quiet low node scripts/package.mjs)
}
if ((!launcher_only)) && [[ ${KILN_RELEASE_SERIAL_LAUNCHERS:-} != 1 ]]; then
  sdk_and_vsix bg & sdk_pid=$!
  note "SDK packages and the VS Code extension: made beside the launcher builds (log: $work/sdk.log)"
fi
# The Windows launcher builds (direct, then Nexus) run beside the Linux ones from here on: their Cargo target and bundle
# folders are their own (the Linux files build in the container, in $lin_target), and their frontend goes to its own
# folder, launcher/dist-windows, so the two Vite runs never write the same files. They have the notices complete.
# KILN_RELEASE_SERIAL_LAUNCHERS=1: one after the other, as before. Their log: $work/launcher-windows.log, then $log.
setup="" nexus_setup="" win_pid=""
win_dist='{"build":{"beforeBuildCommand":"bun run build:web --outDir dist-windows","frontendDist":"../dist-windows"}}'
windows_launchers() {
  local log=$work/launcher-windows.log
  : >"$log"
  step "launcher: Windows (NSIS)"
  wargs=(--runtime "$runtime_zip" --version "$version" --config "$(resources "$notices")" --config "$win_dist")
  ((signing)) && wargs+=(--config "{\"bundle\":{\"windows\":{\"signCommand\":{\"cmd\":\"$here/authenticode.sh\",\"args\":[\"sign\",\"%1\"]}}}}")
  rm -rf "$CARGO_TARGET_DIR/$win/release/bundle"
  (cd "$src" && quiet low "${win_env[@]}" tools/launcher/build-windows.sh "${wargs[@]}")
  sign_pe "$CARGO_TARGET_DIR/$win/release/kilnctl.exe" "$CARGO_TARGET_DIR/$win/release/kiln.exe"
  if ((nexus_build)); then
    step "launcher: Nexus Mods build, Windows (no self-update)"
    rm -rf "$CARGO_TARGET_DIR/$win/release/bundle"
    (cd "$src" && quiet low "${win_env[@]}" tools/launcher/build-windows.sh --nexus "${wargs[@]}")
  fi
}
if ((!skip_windows)); then
  if [[ ${KILN_RELEASE_SERIAL_LAUNCHERS:-} == 1 ]]; then
    windows_launchers
    cat "$work/launcher-windows.log" >>"$log"
  else
    windows_launchers & win_pid=$!
    note "launcher: the Windows builds run beside the Linux ones (log: $work/launcher-windows.log)"
  fi
fi
(cd "$src" && quiet linux tools/launcher/build-linux.sh --bundles appimage --runtime "$runtime_zip" --version "$version" --config "$(resources "$notices")")
appimage=$(ls -t "$lin"/*.AppImage | head -1)
deb=$(ls -t "$lin"/*.deb | head -1)
rpm=$(ls -t "$lin"/*.rpm 2>/dev/null | head -1 || true)

# the Nexus Mods build: the same launcher, runtime and notices without the self-update. Nexus Mods does not allow
# programs that update themselves, so it has no self-update feature (cargo --no-default-features: no updater plugin, no
# update feeds) and no updater key in its config (launcher/src-tauri/tauri.nexus.conf.json); it is never signed for
# the updater and has no manifest. The bundle folders are emptied first: the direct build's files must not be taken.
nexus_appimage="" nexus_deb=""
if ((nexus_build)); then
step "launcher: Nexus Mods build, Linux (no self-update)"
rm -rf "$lin_target/release/bundle"
# no test build in the Nexus build (KILN_LAUNCHER_CHANNEL is empty here: test builds have no Nexus build)
(cd "$src" && quiet linux tools/launcher/build-linux.sh --nexus --bundles appimage,deb --runtime "$runtime_zip" --version "$version" \
  --config "$(resources "$notices")")
nexus_appimage=$(ls -t "$KILN_OUT"/bundles/linux-nexus/*.AppImage | head -1)
nexus_deb=$(ls -t "$KILN_OUT"/bundles/linux-nexus/*.deb | head -1)
fi
if [[ -n $win_pid ]]; then
  wrc=0; wait "$win_pid" || wrc=$?
  win_pid=""
  cat "$work/launcher-windows.log" >>"$log"
  ((wrc == 0)) || { tail -40 "$work/launcher-windows.log" >&2; die "the Windows launcher builds failed (log: $work/launcher-windows.log)"; }
  note "launcher: the Windows builds are done"
fi
if ((!skip_windows)); then
  setup=$KILN_OUT/bundles/windows/kiln-launcher-setup.exe
  ((nexus_build)) && nexus_setup=$KILN_OUT/bundles/windows-nexus/kiln-launcher-setup.exe
fi


# ---------------------------------------------------------------- 6. SDK, VS Code, samples
sdk_version="" vsix=""
skipped_samples=() not_mods=()
if ((launcher_only)); then
  note "--launcher-only: no SDK packages, VS Code extension, sample mods, modding site or feed (mod authors keep the previous release's SDK)"
else
if [[ -n $sdk_pid ]]; then
  sdk_rc=0; wait "$sdk_pid" || sdk_rc=$?
  sdk_pid=""
  cat "$work/sdk.log" >>"$log"
  ((sdk_rc == 0)) || { tail -40 "$work/sdk.log" >&2; die "the SDK packages or the VS Code extension failed (log: $work/sdk.log)"; }
else
  sdk_and_vsix
fi
sdk_version=$KILN_SDK_VERSION
vsix=$(ls -t "$KILN_OUT"/vscode/kiln-vscode-*.vsix | head -1)
step "sample mods"
rm -rf "$work/samples" && mkdir -p "$work/samples"
skipped_samples=() not_mods=()
# a sample on Kiln.Sdk packs itself (KilnPack); an older one builds its output folder (a deployable mod folder), which
# is zipped the way KilnPack does: <id>-<version>.zip holding <id>/, without the shared Kiln.* assemblies, *.deps.json
# and cooked game files. zip_mod_folder FOLDER: 2 when it holds no kiln.mod.json (a library or test project of a sample,
# SampleLib.Api), 3 when it holds test assemblies.
zip_mod_folder() {
  [ -f "$1/kiln.mod.json" ] || return 2
  python3 - "$1" "$work/samples" <<'PY'
import json, os, re, sys, zipfile
o, dest = sys.argv[1:]
text = re.sub(r"^\s*//.*$", "", open(os.path.join(o, "kiln.mod.json"), encoding="utf-8").read(), flags=re.M)
m = json.loads(re.sub(r",(\s*[}\]])", r"\1", text))
mid, ver = m["id"], m.get("version", "0.0.0")
# test frameworks never ship in a mod: a sample whose output has them is a build mistake (see the -o note above)
test_files = sorted(n for _, _, files in os.walk(o) for n in files
                    if re.match(r"(xunit|Microsoft\.TestPlatform|Microsoft\.VisualStudio\.TestPlatform|Kiln\.TestKit|testhost)", n, re.I))
if test_files:
    print(f"release: sample {mid}: test assemblies in its output: {' '.join(test_files)}", file=sys.stderr)
    sys.exit(3)
with zipfile.ZipFile(os.path.join(dest, f"{mid}-{ver}.zip"), "w", zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(o):
        for n in sorted(files):
            rel = os.path.relpath(os.path.join(d, n), o).replace(os.sep, "/")
            if (n.startswith("Kiln.") and n != "Kiln.Admin.dll") or n.endswith((".deps.json", ".runtimeconfig.json")) or rel.startswith("assets/cooked/"):
                continue
            z.write(os.path.join(d, n), f"{mid}/{rel}")
PY
}
# The project's own output folder, not `-o`: -o is a global property, so every project the build references writes
# there too (in repository mode a Blocks sample references the bake, Kiln.Sdk.Bake with Kiln.TestKit, and xunit's
# assemblies ended up in 21 sample zips of the 0.2.0-dev.30 trial). Emptied first: the work folder is reused.
declare -A sample_out=()
while IFS=$'\t' read -r proj o; do sample_out[$proj]=${o%/}; done <"$samples_tsv"
pack_folder() { # project: builds it alone, then zips its output folder
  local proj=$1 o=${sample_out[$1]:-}
  [[ -n $o ]] || return 1
  rm -rf "$o"
  low dotnet build "$proj" -c Release --artifacts-path "$KILN_OUT/dotnet" -v q -nologo >>"$log" 2>&1 || return 1
  zip_mod_folder "$o"
}
# the output folders to zip: kiln-win1's (KILN_BUILD_REMOTE=win1), else one local build of every sample
declare -A sample_built=()
if [[ -n $remote_pid ]]; then
  rrc=0; wait "$remote_pid" || rrc=$?
  remote_pid=""
  cat "$work/remote-win1.log" >>"$log"
  if ((rrc == 0)) && [[ -f $work/remote-win1/ok ]]; then
    for proj in "${!sample_out[@]}"; do sample_built[$proj]=$work/remote-win1/out/${sample_out[$proj]#"$KILN_OUT/dotnet/"}; done
    note "sample mods: built on kiln-win1 ($(grep -o 'dotnet build rc=0 in [0-9]* s' "$work/remote-win1.log" || true))"
  else
    note "sample mods: the kiln-win1 build failed (exit $rrc, $work/remote-win1.log): building them here"
  fi
fi
if ((!${#sample_built[@]})) && [[ -s $samples_tsv ]]; then
  for proj in "${!sample_out[@]}"; do rm -rf "${sample_out[$proj]}"; done
  printf '<Solution>\n' >"$work/samples.slnx"
  while IFS=$'\t' read -r proj o; do printf '  <Project Path="%s" />\n' "$proj" >>"$work/samples.slnx"; done <"$samples_tsv"
  printf '</Solution>\n' >>"$work/samples.slnx"
  if low dotnet build "$work/samples.slnx" -c Release --artifacts-path "$KILN_OUT/dotnet" -v q -nologo >>"$log" 2>&1; then
    for proj in "${!sample_out[@]}"; do sample_built[$proj]=${sample_out[$proj]}; done
  else
    note "sample mods: the one build of all samples failed: building each alone (the failing ones are left out)"
  fi
fi
# (the loop's variable is not `name`: that is the release's name, used for the files and links below)
for proj in "$src"/mods/samples/*/*.csproj; do
  sample=$(basename "$proj" .csproj)
  if grep -q 'Sdk="Kiln.Sdk' "$proj"; then
    low dotnet build "$proj" -c Release --artifacts-path "$KILN_OUT/dotnet" -v q -nologo -t:KilnPack \
      -p:KilnPackageOutputPath="$work/samples/" >>"$log" 2>&1 || skipped_samples+=("$sample")
  else
    rc=0
    if [[ -n ${sample_built[$proj]:-} ]]; then zip_mod_folder "${sample_built[$proj]}" 2>>"$log" || rc=$?
    else pack_folder "$proj" 2>>"$log" || rc=$?; fi
    ((rc == 2)) && not_mods+=("$sample")
    ((rc == 1)) && skipped_samples+=("$sample")
    ((rc == 3)) && die "sample $sample: test assemblies in its zip's files (log: $log)"
  fi
done
note "sample mods: $(ls "$work"/samples/*.zip 2>/dev/null | wc -l) zips"
((${#not_mods[@]})) && note "sample projects that are no mod (no kiln.mod.json, not zipped): ${not_mods[*]}"
((${#skipped_samples[@]})) && note "samples that did not build (left out): ${skipped_samples[*]}"
:
fi
[[ $name == "$version" || $name == "$version-beta.$beta" ]] || die "the release's name changed to '$name' during the build (a step overwrote it)"

# ---------------------------------------------------------------- 7. the folder
step "assemble $out"
mkdir -p "$out"/{linux,windows,runtime,updates,source}
((launcher_only)) || mkdir -p "$out"/{sdk,samples}
# a test build's launcher files carry its name (kiln-launcher_0.2.0-dev.29-beta.3_amd64.AppImage): a tester sees which
A=kiln-launcher_${name}_amd64.AppImage D=kiln-launcher_${name}_amd64.deb R=kiln-launcher-${name}.x86_64.rpm S=kiln-launcher_${name}_x64-setup.exe
cp "$appimage" "$out/linux/$A" && chmod 755 "$out/linux/$A"
cp "$deb" "$out/linux/$D"
[[ -n $rpm ]] && cp "$rpm" "$out/linux/$R"
[[ -n $setup ]] && cp "$setup" "$out/windows/$S"
cp "$runtime_zip" "$out/runtime/kiln-runtime-$version.zip"
# the server pack: the components kilnctl deploy --bundle installs on a server, as a flat zip a rented host's owner
# unzips into the server folder over FTP (docs/kiln/dedicated-servers.md); deterministic, like the runtime zip
python3 "$lib/serverpack.py" build "$out/runtime/kiln-runtime-$version.zip" --out "$out/runtime/kiln-server-$version.zip" \
  --readme "$here/templates/KILN-SERVER-README.txt" --version "$version" --epoch "$SOURCE_DATE_EPOCH" | tee -a "$log"
# kilnctl's systemd user units (docs/kiln/kilnctl.md "systemd"), next to kilnctl
install -m 644 "$src"/runner/systemd/*.service "$src"/runner/systemd/*.timer "$out/runtime/"
install -m 755 "$linux_bin/kilnctl" "$out/runtime/kilnctl"
if ((!skip_windows)); then
  install -m 644 "$CARGO_TARGET_DIR/$win/release/kilnctl.exe" "$out/runtime/kilnctl.exe"
fi
if ((!launcher_only)); then
install -m 755 "$linux_bin/kiln" "$out/sdk/kiln"
((skip_windows)) || install -m 644 "$CARGO_TARGET_DIR/$win/release/kiln.exe" "$out/sdk/kiln.exe"
for f in "$KILN_OUT"/nuget/*."$sdk_version".nupkg; do cp "$f" "$out/sdk/"; done
cp "$vsix" "$out/sdk/"
# the modding site (guide + API reference, DocFX) in sdk/api/, when the export has its build script
if [[ -f $src/tools/docs-site/build.sh ]]; then
  export KILN_DOCS_TRACKED=$work/src-files
  # strict: a public type or member without an XML summary (undocumented.py against undocumented-baseline.txt, which names
  # none) stops the release here; the same gate runs in tools/check.sh (stage `docs`), so it is found before the release
  export KILN_DOCS_STRICT=1
  quiet low "$src/tools/docs-site/build.sh" "$out/sdk/api"   # the modding site: guide + API reference (DocFX)
else
  note "no tools/docs-site/build.sh in the export: the API reference (sdk/api/) is left out"
fi
# the SDK's nupkgs as a static NuGet v3 feed in sdk/feed/ (lib/feed.py), for dotnet new install and a mod's
# nuget.config once the folder is served: its index names absolute URLs, by default where publish.sh would put it
# (downloads/<version>/sdk/feed/); publish.sh uploads sdk/feed/ (and sdk/api/) only with --sdk-site
feed_url=${KILN_FEED_URL:-https://frostdev.io/kiln/downloads/$name/sdk/feed/}
python3 -I "$lib/feed.py" build --out "$out/sdk/feed" --base-url "$feed_url" "$out/sdk" | tee -a "$log"
cp "$work"/samples/*.zip "$out/samples/" 2>/dev/null || true
fi
cp "$src/LICENSE" "$src/CREDITS.md" "$notices" "$out/"
cp "$work/source/"*.tar.gz "$out/source/"
# the Nexus Mods build (nexus.py uploads these; never GitHub or frostdev.io); none for a test build
if ((nexus_build)); then
  mkdir -p "$out/nexus/linux"
  cp "$nexus_appimage" "$out/nexus/linux/$A" && chmod 755 "$out/nexus/linux/$A"
  cp "$nexus_deb" "$out/nexus/linux/$D"
  [[ -n $nexus_setup ]] && mkdir -p "$out/nexus/windows" && cp "$nexus_setup" "$out/nexus/windows/$S"
fi

# release notes
if [[ -n $notes ]]; then cp "$notes" "$out/RELEASE-NOTES.md"; else
  {
    echo "# Kiln $name"
    echo
    [[ $channel == beta ]] && { echo "Test build $beta of $version, for the beta channel: for testers, not a release."; echo; }
    if ((worktree)); then echo "Test build of the working tree at $KILN_GIT."; else
      # $prev: the previous v* tag, found (after fetching the tags) for the API gate in 1.
      echo "Changes since ${prev:-the start of the project}:"
      echo
      git -C "$repo" log --no-merges -n 60 --format='- %s' ${prev:+"$prev.."}"$full"
    fi
  } >"$out/RELEASE-NOTES.md"
fi
# the manifest's notes: the first lines of RELEASE-NOTES.md after its title
short=$work/notes-short.txt
# (awk reads the whole file: with `head`, a writer still busy when head leaves dies of SIGPIPE, and pipefail ends the
# release there; the notes of 0.2.0-dev.27 were long enough for that)
sed '1{/^# /d}' "$out/RELEASE-NOTES.md" | sed '/./,$!d' | awk 'NR <= 12' >"$short"

# ---------------------------------------------------------------- 8. updater signatures, manifests
step "updater signatures"
pass_file=$key.password
updater_files=("$out/linux/$A")
[[ -n $setup ]] && updater_files+=("$out/windows/$S")
for f in "${updater_files[@]}"; do
  (set +x; cd "$src/launcher" && TAURI_SIGNING_PRIVATE_KEY_PASSWORD="$(cat "$pass_file")" \
    bun x tauri signer sign -f "$key" --app-version "$build_id" "$f" >/dev/null 2>&1) || die "signing $f failed"
  note "signed $(basename "$f")"
done
step "update manifests"
fd=https://frostdev.io/kiln/downloads/$name
gh=https://github.com/frostdev-ops/kiln/releases/download/v$name
manifest() { # base-url layout(folders|flat) out channel
  local base=$1 flat=$2 dest=$3 ch=$4 e=() b=()
  [[ $channel == beta ]] && b=(--beta "$beta" --base-release "$KILN_LAUNCHER_BASE_RELEASE")
  local la="linux/$A" ws="windows/$S"
  [[ $flat == flat ]] && la=$A ws=$S
  e+=(--entry "linux-x86_64-appimage,linux-x86_64=$base/$la|$out/linux/$A.sig|$out/linux/$A")
  [[ -n $setup ]] && e+=(--entry "windows-x86_64-nsis,windows-x86_64=$base/$ws|$out/windows/$S.sig|$out/windows/$S")
  python3 "$lib/release.py" manifest --version "$build_id" --channel "$ch" "${b[@]}" --notes "$short" --runtime "$runtime_version" \
    --date "$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)" "${e[@]}" --out "$dest"
}
# a test build writes the beta channel's manifests only: never dev or stable
manifest "$fd" folders "$out/updates/frostdev.io/$channel/latest.json" "$channel"
manifest "$gh" flat "$out/updates/github/latest.json" "$channel"
if [[ $channel == stable ]]; then
  # a release is also the newest build of the dev channel (dev builds of the next version replace it)
  manifest "$fd" folders "$out/updates/frostdev.io/dev/latest.json" dev
fi
if [[ $channel != beta ]]; then
  # ...and of the beta channel: a tester is offered the final build (publish.sh moves the beta channel to it only when
  # no newer test build is out)
  manifest "$fd" folders "$out/updates/frostdev.io/beta/latest.json" beta
fi

# ---------------------------------------------------------------- 9. README, sums
step "README.txt, INSTALL-RUNTIME.txt, SHA256SUMS"
smart="           Windows SmartScreen may warn that the publisher is unknown: the setup program is not code-signed yet.
           Choose More info > Run anyway, after checking its SHA256 below."
((signing)) && smart="           The setup program is code-signed (Authenticode)."
rpm_line=""
[[ -n $rpm ]] && rpm_line=$'\n'"           or linux/$R (Fedora, openSUSE)"
fill() { # template out: @KEY@ from the R_KEY variables
  R_VERSION=$name R_CHANNEL=$channel R_DATE=$date_iso R_SOURCE="$built_from" R_RUNTIME_VERSION=$runtime_version R_TESTBUILD=$testbuild \
  R_SDK_VERSION=$sdk_version R_APPIMAGE=$A R_DEB=$D R_SETUP=$S R_VSIX=$(basename "$vsix") R_RPM_LINE=$rpm_line \
  R_SMARTSCREEN=$smart R_RULE=$(printf '=%.0s' $(seq 1 $((5 + ${#name})))) python3 - "$1" "$2" <<'PY'
import os, sys
t = open(sys.argv[1], encoding="utf-8").read()
for k, v in os.environ.items():
    if k.startswith("R_"):
        t = t.replace("@" + k[2:] + "@", v)
open(sys.argv[2], "w", encoding="utf-8").write(t)
PY
}
# a test build says so at the top of README.txt
testbuild=""
[[ $channel == beta ]] && testbuild="
This is test build $beta of $version, for the beta channel: for testers, not a release. In the launcher, Settings >
Updates > Channel switches back to the release (also Beta $beta in the header). A world played with a test build may
not load the same way on the release; servers and players must run the same build to play together.
"
fill "$here/templates/INSTALL-RUNTIME.txt" "$out/runtime/INSTALL-RUNTIME.txt"
# the Nexus build's README (in its Nexus zips): no self-update; the files' SHA256 in it (the zips carry no SHA256SUMS)
export R_NEXUS_SUMS
if ((nexus_build)); then
  R_NEXUS_SUMS=$(cd "$out/nexus" && for f in linux/* windows/*; do [[ -f $f ]] && printf '  %s  %s\n' "$(sha256sum <"$f" | cut -d' ' -f1)" "${f#*/}"; done; true)
  fill "$here/templates/README-nexus.txt" "$out/nexus/README.txt"
fi
unset R_NEXUS_SUMS
fill "$here/templates/README.txt" "$out/README.txt.tmp"
if ((launcher_only)); then  # no sdk/ or samples/ in this release: the README says so instead of listing them
  python3 - "$out/README.txt.tmp" <<'PY'
import re, sys
p = sys.argv[1]
t = open(p, encoding="utf-8").read()
t2 = re.sub(r"Mod authors:\n(?:  sdk/.*\n|  samples/.*\n)+",
            "Mod authors:\n  This quick fix ships the launcher and the runtime only: keep the SDK of the release before it\n", t)
t2 = re.sub(r"\n  Use the SDK from a folder:.*\n.*dotnet new kiln-mod.*\n", "\n", t2)
assert t2 != t, "README: no Mod authors block to replace"
open(p, "w", encoding="utf-8").write(t2)
PY
fi
# the file list: the downloads, not nexus/ (its own README) or sdk/api/ and sdk/feed/ (built, not published by default)
python3 - "$out" <<'PY'
import os, sys
root = sys.argv[1]
def row(rel, s):
    return f"  {rel:<64} {s / 1048576:8.1f} MB" if s >= 1048576 else f"  {rel:<64} {s / 1024:8.1f} KB"
rows = []
for d, dirs, files in os.walk(root):
    dirs.sort()
    reld = os.path.relpath(d, root).replace(os.sep, "/")
    if reld == "sdk":
        dirs[:] = [n for n in dirs if n not in ("api", "feed")]
    for n in sorted(files):
        rel = os.path.relpath(os.path.join(d, n), root)
        if rel in ("README.txt.tmp", "SHA256SUMS") or rel.startswith("nexus" + os.sep):
            continue
        rows.append(row(rel, os.path.getsize(os.path.join(d, n))))
rows.append(f"  {'README.txt':<64}")
rows.append(f"  {'SHA256SUMS':<64}")
t = open(os.path.join(root, "README.txt.tmp"), encoding="utf-8").read().replace("@FILES@", "\n".join(rows))
open(os.path.join(root, "README.txt"), "w", encoding="utf-8").write(t)
os.remove(os.path.join(root, "README.txt.tmp"))
PY
python3 "$lib/release.py" sums "$out"

# ---------------------------------------------------------------- 10. checks
results=()
check() { # name command...
  local name=$1; shift
  if "$@" >>"$log" 2>&1; then results+=("PASS  $name"); else results+=("FAIL  $name (see $log)"); fi
}
if ((checks)); then
  step "checks"
  # shellcheck source=checks.sh
  source "$here/checks.sh"
  run_checks
  printf '%s\n' "${results[@]}" | tee -a "$log"
fi

# The API record: what this release added is shipped now, so its Unshipped.txt lines move into Shipped.txt in the
# repository's working tree (not committed: the closing text says so). Only after a release whose checks all ran and
# passed, never for a test build, and only while the working tree's API files are the released commit's.
api_moved=0
if ((worktree)); then
  note "API record: a test build of the working tree; the Unshipped.txt lines stay"
elif [[ $channel == beta ]]; then
  note "API record: a test build ($name) ships no API; the Unshipped.txt lines stay for the release"
elif ((!checks)) || printf '%s\n' "${results[@]}" | grep -q '^FAIL'; then
  note "API record: not promoted (the checks failed or did not run); after a good release run tools/release/api-gate.sh promote --release $version"
else
  step "API record: Unshipped.txt lines into Shipped.txt"
  if "$here/api-gate.sh" promote --if-same-as "$full" --release "$version" 2>&1 | tee -a "$log"; then
    # the index again, with the ledger's new rows (the same bytes when the API version is the one the Unshipped lines carried)
    if [[ -n $api_index_xml ]]; then
      python3 -I "$repo/tools/docs-site/api_index.py" --xml "$api_index_xml" >>"$log" 2>&1 ||
        note "API index: not regenerated after the promote (see the log); run python3 -I tools/docs-site/api_index.py --xml $api_index_xml"
    fi
    api_moved=$(git -C "$repo" status --porcelain -- ':(glob)managed/**/PublicAPI.*.txt' managed/api-history.tsv "$api_index" | wc -l)
  else
    note "API record: not promoted (above); the release itself is fine"
  fi
fi

# Nexus Mods: what would go up (GET calls only); the upload itself is publish.sh --nexus or nexus.py --publish. Never
# for a test build of the working tree (nothing of it goes up)
if ((nexus)) && ((!worktree)) && [[ -f $HOME/.config/kiln-release/nexus.env ]]; then
  step "Nexus Mods (dry run)"
  python3 -I "$here/nexus.py" "$out" --work "$work/nexus" | tee -a "$log" || note "the Nexus dry run failed (the release is fine)"
fi

# the closing text: what to do next, never a publish step for a test build of the working tree
if ((worktree)); then
  closing="Test build ${name:-$version} of the working tree is in $out (log: $log). It is never published: github-draft.sh,
publish.sh and nexus.py --publish refuse it. A release builds a commit: tools/release/release.sh $version (no --worktree)."
elif [[ ${channel:-} == beta ]]; then
  closing="Test build $beta of $version (beta channel) is in $out (log: $log). Nothing is published.
Next (by hand, see docs/kiln/launcher/releasing.md, \"Cutting a beta\"):
  tools/release/github-draft.sh $out        a DRAFT GitHub pre-release v$name (no SDK files)
  tools/release/publish.sh $out --host H    frostdev.io downloads/$name and updates/beta, the updates-beta pointer;
                                            never the dev or stable channel, never Nexus Mods"
else
  closing="Release $version is in $out (log: $log). Nothing is published.
Next (by hand, see docs/kiln/launcher/releasing.md):
  tools/release/github-draft.sh $out        a DRAFT GitHub release with the files (needs the commit pushed)
  tools/release/publish.sh $out             publish: the GitHub release, the dev and beta channel pointers, frostdev.io,
                                            then the roadmap: $version as released on frostdev.io/kiln/roadmap
  python3 -I tools/release/nexus.py $out    Nexus Mods (dry run; --publish uploads): the nexus/ build, no self-update
  python3 -I tools/docs-site/status-tables.py --server <game>/kiln/state/capabilities.json --checks <game>/kiln/state/client-checks.json \\
      -o docs/kiln/status.generated.md      the status page, from the state files of this release's lab run (a build boots no game)"
fi
step "done"
du -sh "$out" | tee -a "$log"
# sdk/api/ and sdk/feed/ (many files each) as one row each in this local build listing only
(cd "$out" && find . -type f -not -path './sdk/api/*' -not -path './sdk/feed/*' | sort | while read -r f; do printf '%10s  %s\n' "$(du -h "$f" | cut -f1)" "${f#./}"; done
  for d in sdk/api sdk/feed; do [[ -d $d ]] && printf '%10s  %s/ (%s files)\n' "$(du -sh "$d" | cut -f1)" "$d" "$(find "$d" -type f | wc -l)"; done; true) | tee -a "$log"
cat <<EOF | tee -a "$log"

$closing
EOF
if ((!api_compat)); then
  note "WARNING: the SDK was NOT compared with the last release (KILN_API_BASELINE was not set): see the warning at the start."
fi
if ((api_moved)); then
  cat <<EOF | tee -a "$log"
  The API record: $api_moved file(s) changed in $repo (this release's API moved from Unshipped.txt into Shipped.txt, its rows
  went into the API history ledger, and the agent API index is generated from them). Commit them (add -A: a new or
  removed index part is a new or deleted file, which a commit with paths alone leaves out):
  git -C "$repo" add -A -- agent/skills/kiln-modding/references/api-index &&
    git -C "$repo" commit -m "api: ship the API of $version" -- ':(glob)managed/**/PublicAPI.*.txt' managed/api-history.tsv agent/skills/kiln-modding/references/api-index
EOF
fi
if printf '%s\n' "${results[@]}" | grep -q '^FAIL'; then exit 1; fi
