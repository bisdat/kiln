#!/usr/bin/env bash
# Build any PR/branch from bisdat/kiln inside the reusable Fedora Distrobox.
# Host usage: bash tools/launcher/distrobox-build.sh 3 [--tests-only]
#             bash tools/launcher/distrobox-build.sh fix/another-branch
# Checkouts (git worktrees) and build artifacts: ~/Projects/kiln-worktrees and ~/Projects/kiln-builds.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=distrobox-env.sh
source "$here/distrobox-env.sh"

if [[ "${1:-}" == "--inside-build" ]]; then
  [[ $# == 4 ]] || { echo "Internal build call requires WORKTREE OUT MODE" >&2; exit 2; }
  worktree="$2"
  out="$3"
  mode="$4"
  [[ -f "$worktree/Cargo.toml" && -f "$worktree/launcher/bun.lock" ]] || {
    echo "Not a Kiln source worktree: $worktree" >&2
    exit 1
  }
  command -v cargo >/dev/null || { echo "Run distrobox-setup.sh first: cargo missing" >&2; exit 1; }
  command -v bun >/dev/null || { echo "Run distrobox-setup.sh first: bun missing" >&2; exit 1; }
  mkdir -p "$out" "$KILN_TOOL_ROOT/target"
  export KILN_OUT="$out"
  export CARGO_TARGET_DIR="$KILN_TOOL_ROOT/target"
  cd "$worktree"
  echo "Testing Kiln source: $(git rev-parse --short HEAD)"
  cargo test -p kiln-manage --test discovery
  if [[ "$mode" == "tests-only" ]]; then
    echo "Regression tests passed; no AppImage built."
    exit 0
  fi
  cd launcher
  bun install --frozen-lockfile
  bun run typecheck
  cd ..
  echo "Building launcher-only AppImage without runtime bundle or self-update"
  APPIMAGE_EXTRACT_AND_RUN=1 tools/launcher/build-linux.sh --runtime none --bundles appimage --nexus
  shopt -s nullglob
  appimages=("$out"/bundles/linux-nexus/*.AppImage)
  (("${#appimages[@]}" > 0)) || { echo "Build completed but no AppImage was found" >&2; exit 1; }
  printf '\nAppImage ready:\n'
  printf '  %s\n' "${appimages[@]}"
  exit 0
fi

usage() {
  echo "Usage: bash tools/launcher/distrobox-build.sh PR_NUMBER|BRANCH [--tests-only]" >&2
  echo "Example: bash tools/launcher/distrobox-build.sh 3" >&2
  exit 2
}

(( $# >= 1 && $# <= 2 )) || usage
ref="$1"
mode="appimage"
if [[ $# == 2 ]]; then
  [[ "$2" == "--tests-only" ]] || usage
  mode="tests-only"
fi
[[ "$ref" =~ ^[[:alnum:]_.\/-]+$ && "$ref" != *..* ]] || usage
if [[ "$ref" =~ ^[0-9]+$ ]]; then ref="pr/$ref"; fi
repo="$KILN_PROJECTS_DIR/kiln"
worktrees="$KILN_PROJECTS_DIR/kiln-worktrees"
builds="$KILN_PROJECTS_DIR/kiln-builds"
mkdir -p "$KILN_PROJECTS_DIR" "$worktrees" "$builds"

command -v git >/dev/null || { echo "Git is required on the host" >&2; exit 1; }
command -v distrobox >/dev/null || { echo "Distrobox is required on the host" >&2; exit 1; }
command -v podman >/dev/null || { echo "Podman is required on the host" >&2; exit 1; }
podman container exists "$KILN_DISTROBOX_NAME" || {
  echo "Missing $KILN_DISTROBOX_NAME: run distrobox-setup.sh first" >&2
  exit 1
}

if [[ ! -e "$repo" ]]; then
  git clone https://github.com/bisdat/kiln.git "$repo"
fi
git -C "$repo" rev-parse --is-inside-work-tree >/dev/null || {
  echo "Expected a Git repository at $repo; will not overwrite it" >&2; exit 1;
}
remote="$(git -C "$repo" remote get-url origin)"
case "$remote" in
  https://github.com/bisdat/kiln|https://github.com/bisdat/kiln.git|git@github.com:bisdat/kiln|git@github.com:bisdat/kiln.git) ;;
  *) echo "Unexpected origin: $remote; expected bisdat/kiln (not modifying this repository)" >&2; exit 1 ;;
esac

git -C "$repo" fetch --prune origin
if [[ "$ref" =~ ^pr/([0-9]+)$ ]]; then
  num="${BASH_REMATCH[1]}"
  git -C "$repo" fetch origin "refs/pull/$num/head:refs/remotes/origin/pr-$num"
  rev="refs/remotes/origin/pr-$num"
  label="pr-$num"
else
  git check-ref-format --branch "$ref" >/dev/null || usage
  rev="refs/remotes/origin/$ref"
  git -C "$repo" show-ref --verify --quiet "$rev" || { echo "No remote branch $ref in bisdat/kiln" >&2; exit 1; }
  label="${ref//\//-}"
fi
commit="$(git -C "$repo" rev-parse "$rev^{commit}")"
worktree="$worktrees/$label"
out="$builds/$label"

if [[ -e "$worktree" ]]; then
  git -C "$worktree" rev-parse --show-toplevel >/dev/null || {
    echo "$worktree exists but is not a Git worktree; will not overwrite it" >&2; exit 1;
  }
  [[ -z "$(git -C "$worktree" status --porcelain)" ]] || {
    echo "Uncommitted changes in $worktree: refusing to change checkout" >&2; exit 1;
  }
  git -C "$worktree" switch --detach "$commit"
else
  git -C "$repo" worktree add --detach "$worktree" "$commit"
fi

echo "Building $ref @ ${commit:0:12} using Distrobox $KILN_DISTROBOX_NAME"
distrobox enter --name "$KILN_DISTROBOX_NAME" -- bash "$here/distrobox-build.sh" --inside-build "$worktree" "$out" "$mode"
