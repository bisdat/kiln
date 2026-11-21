#!/usr/bin/env bash
# Personal Bazzite helper for Kiln branches and fork/upstream PR builds.
# Git management clone and persistent tools: ~/Projects
# Temporary checkout, logs, build outputs and artifacts: ~/Downloads/kiln-builds
# Never installs anything into Enshrouded and never modifies kiln/main.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=distrobox-env.sh
source "$here/distrobox-env.sh"

usage() {
  cat <<'USAGE'
Usage: bash tools/launcher/distrobox-build.sh REF [--mode launcher|ui|native|managed]
             [--tests-only] [--native-tests targeted|full] [--target kiln-ui]

REF:
  3                 fork PR #3 (alias for pr/3; preserves old behaviour)
  pr/3              PR #3 in bisdat/kiln
  upstream-pr/106   PR #106 in frostdev-ops/kiln
  feature/example   branch in bisdat/kiln

Modes (default: launcher):
  launcher   Rust discovery tests, then launcher typecheck and AppImage
  ui         native UI host checks, managed UI tests, i18n, Windows + managed DLLs
  native     native UI host checks, then cross-build --target (default kiln-ui)
  managed    managed Kiln.Ui build and test suite

--tests-only omits the target build or AppImage, but runs that mode's tests.
--native-tests full uses native/services/ui/tests/run.sh (incl. TSan);
                     targeted is the six portable chat/UI checks (default).

No game installation or runtime deployment. Artifacts are NOT a full Kiln runtime.
All ephemeral files: ~/Downloads/kiln-builds/<ref>/
USAGE
}

if [[ "${1:-}" == "--inside-build" ]]; then
  # Called only from the host phase; do not consume the public CLI flags here.
  [[ $# == 7 ]] || { echo "Internal build invocation expects 6 arguments" >&2; exit 2; }
  worktree="$2"; out="$3"; mode="$4"; tests_only="$5"; native_tests="$6"; target="$7"
  [[ -f "$worktree/Cargo.toml" && -f "$worktree/launcher/bun.lock" ]] || {
    echo "Not a Kiln checkout: $worktree" >&2; exit 1;
  }
  git -C "$worktree" rev-parse --is-inside-work-tree >/dev/null || {
    echo "Not a Git worktree: $worktree" >&2; exit 1;
  }
  mkdir -p "$out"/{logs,artifacts,build,cache,tmp}
  log="$out/logs/build-$(date -u +%Y%m%d-%H%M%S).log"
  exec > >(tee -a "$log") 2>&1
  trap 'status=$?; if ((status != 0)); then echo "BUILD FAILED (exit $status). Log: $log" >&2; fi' EXIT
  export KILN_OUT="$out/build" CARGO_TARGET_DIR="$out/build/cargo"
  export NUGET_PACKAGES="$out/cache/nuget" DOTNET_CLI_HOME="$out/cache/dotnet"
  export BUN_INSTALL_CACHE_DIR="$out/cache/bun" XDG_CACHE_HOME="$out/cache/xdg"
  export TMPDIR="$out/tmp"
  cd "$worktree"
  echo "Kiln Distrobox $KILN_DISTROBOX_NAME | mode=$mode | tests-only=$tests_only"
  echo "Source commit: $(git rev-parse HEAD)"
  echo "Worktree: $worktree"
  echo "Log: $log"

  native_host_tests() {
    local dir="$worktree/native/services/ui/tests" d="$out/build/native-host-tests"
    mkdir -p "$d"
    command -v g++ >/dev/null || { echo "Missing g++; rerun distrobox-setup.sh" >&2; return 1; }
    if [[ "$native_tests" == full ]]; then
      echo '=== Full native UI host suite (including ThreadSanitizer) ==='
      KILN_OUT="$KILN_OUT" bash "$dir/run.sh"
      echo 'PASS: full native UI host suite'
      return
    fi
    echo '=== Six targeted native chat/UI checks (ASan + UBSan) ==='
    local name file count=0
    for name in chat_keys chat_search chat_history chat_completion_text chat_completion_abi; do
      file="$dir/${name}_test.cpp"
      if [[ ! -f "$file" ]]; then
        echo "SKIP: $name (test not present in this revision)"
        continue
      fi
      echo "Test: $name"
      g++ -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=address,undefined \
        -I"$worktree/native/core/abi" "$file" -o "$d/$name"
      "$d/$name"
      ((count+=1))
    done
    if [[ -f "$dir/run-window-constraints.sh" ]]; then
      echo 'Test: window constraints and ABI'
      KILN_OUT="$KILN_OUT" bash "$dir/run-window-constraints.sh"
      ((count+=1))
    fi
    ((count > 0)) || { echo "No native UI tests found" >&2; return 1; }
    echo "PASS: $count targeted native checks"
  }

  native_build() {
    local bin="$KILN_OUT/cmake/bin"
    [[ -s "$XWIN_ROOT/sdk/include/um/windows.h" ]] || {
      echo "Windows SDK not found: $XWIN_ROOT/sdk/include/um/windows.h" >&2
      echo "Install/populate cargo-xwin inside kiln-build, or set XWIN_ROOT to the SDK." >&2
      return 1
    }
    command -v clang-cl >/dev/null || { echo "Missing clang-cl; rerun distrobox-setup.sh" >&2; return 1; }
    echo "=== Windows native target: $target ==="
    bash "$worktree/native/build.sh" "$target"
    [[ -d "$bin" ]] || { echo "Native build returned without bin/ output" >&2; return 1; }
    mkdir -p "$out/artifacts/native"
    (cd "$bin" && find . -type f -name '*.dll' -exec cp --parents -f {} "$out/artifacts/native" \;)
    echo 'Native DLL artifacts:'
    find "$out/artifacts/native" -type f -name '*.dll' -print | sort
  }

  managed_build_and_test() {
    command -v dotnet >/dev/null || { echo "Missing dotnet; rerun distrobox-setup.sh" >&2; return 1; }
    local csproj="$worktree/managed/services/Kiln.Ui/Kiln.Ui.csproj"
    local tests="$worktree/managed/tests/Kiln.Ui.Tests/Kiln.Ui.Tests.csproj"
    if [[ "$tests_only" != 1 ]]; then
      echo '=== Managed Kiln.Ui build ==='
      dotnet build "$csproj" -c Release --artifacts-path "$KILN_OUT/dotnet" -nologo
      local dll="$KILN_OUT/dotnet/bin/Kiln.Ui/release/Kiln.Ui.dll"
      [[ -s "$dll" ]] || { echo "Missing managed build artifact: $dll" >&2; return 1; }
      mkdir -p "$out/artifacts/managed"
      cp -f "$dll" "$out/artifacts/managed/Kiln.Ui.dll"
      echo "Managed DLL: $out/artifacts/managed/Kiln.Ui.dll"
    fi
    echo '=== Managed Kiln.Ui tests ==='
    mkdir -p "$out/artifacts/test-results"
    dotnet test "$tests" -c Release --artifacts-path "$KILN_OUT/dotnet" -nologo \
      --logger 'trx;LogFileName=kiln-ui-tests.trx' \
      --results-directory "$out/artifacts/test-results"
  }

  launcher_build() {
    command -v cargo >/dev/null || { echo "Missing cargo; rerun distrobox-setup.sh" >&2; return 1; }
    command -v bun >/dev/null || { echo "Missing bun; rerun distrobox-setup.sh" >&2; return 1; }
    echo '=== kiln-manage discovery tests ==='
    cargo test -p kiln-manage --test discovery
    if [[ "$tests_only" == 1 ]]; then
      echo 'PASS: launcher tests-only mode'
      return
    fi
    echo '=== Launcher dependencies and typecheck ==='
    (cd launcher && bun install --frozen-lockfile && bun run typecheck)
    echo '=== Launcher-only AppImage (no runtime or self-update) ==='
    APPIMAGE_EXTRACT_AND_RUN=1 tools/launcher/build-linux.sh --runtime none --bundles appimage --nexus
    shopt -s nullglob
    local images=("$KILN_OUT"/bundles/linux-nexus/*.AppImage)
    (( ${#images[@]} > 0 )) || { echo "No AppImage found under $KILN_OUT" >&2; return 1; }
    mkdir -p "$out/artifacts/launcher"
    cp -f "${images[@]}" "$out/artifacts/launcher/"
    printf 'Launcher test AppImages: %s\n' "$out/artifacts/launcher/"
    find "$out/artifacts/launcher" -maxdepth 1 -name '*.AppImage' -print
  }

  case "$mode" in
    launcher) launcher_build ;;
    managed) managed_build_and_test ;;
    native)
      native_host_tests
      [[ "$tests_only" == 1 ]] || native_build
      ;;
    ui)
      native_host_tests
      managed_build_and_test
      echo '=== Launcher translation check ==='
      (cd launcher && bun install --frozen-lockfile && bun run check:i18n)
      [[ "$tests_only" == 1 ]] || native_build
      ;;
    *) echo "Unknown mode: $mode" >&2; exit 2 ;;
  esac
  echo "SUCCESS: $mode @ $(git rev-parse --short=12 HEAD)"
  echo "Artifacts: $out/artifacts"
  echo "Full log: $log"
  exit 0
fi

if [[ $# == 0 ]]; then usage >&2; exit 2; fi
ref='' mode=launcher tests_only=0 native_tests=targeted target=kiln-ui
while (($#)); do
  case "$1" in
    -h|--help) usage; exit 0 ;;
    --mode)
      (($# >= 2)) || { echo 'Missing value for --mode' >&2; exit 2; }
      mode="$2"; shift 2 ;;
    --tests-only) tests_only=1; shift ;;
    --native-tests)
      (($# >= 2)) || { echo 'Missing value for --native-tests' >&2; exit 2; }
      native_tests="$2"; shift 2 ;;
    --target)
      (($# >= 2)) || { echo 'Missing value for --target' >&2; exit 2; }
      target="$2"; shift 2 ;;
    --*) echo "Unknown flag: $1" >&2; usage >&2; exit 2 ;;
    *) [[ -z "$ref" ]] || { echo "Only one REF is allowed" >&2; exit 2; }
       ref="$1"; shift ;;
  esac
done
[[ -n "$ref" ]] || { usage >&2; exit 2; }
case "$mode" in launcher|ui|native|managed) ;; *) echo "Unknown --mode: $mode" >&2; exit 2 ;; esac
case "$native_tests" in targeted|full) ;; *) echo "Unknown --native-tests: $native_tests" >&2; exit 2 ;; esac
[[ "$target" =~ ^kiln-[a-zA-Z0-9_-]+$ ]] || { echo "Invalid native target: $target" >&2; exit 2; }
[[ "$mode" == native || "$target" == kiln-ui ]] || { echo "--target is supported only with --mode native" >&2; exit 2; }
[[ "$mode" == native || "$mode" == ui || "$native_tests" == targeted ]] || {
  echo "--native-tests is supported only with --mode native or ui" >&2; exit 2;
}

command -v git >/dev/null || { echo 'Missing git on Bazzite' >&2; exit 1; }
command -v distrobox >/dev/null || { echo 'Missing distrobox on Bazzite' >&2; exit 1; }
command -v podman >/dev/null || { echo 'Missing podman on Bazzite' >&2; exit 1; }
podman container exists "$KILN_DISTROBOX_NAME" || {
  echo "Missing $KILN_DISTROBOX_NAME: run distrobox-setup.sh first" >&2; exit 1;
}
mkdir -p "$KILN_PROJECTS_DIR" "$KILN_TEMP_DIR"
repo="$KILN_SOURCE_REPO"
if [[ ! -e "$repo" ]]; then git clone https://github.com/bisdat/kiln.git "$repo"; fi
git -C "$repo" rev-parse --is-inside-work-tree >/dev/null || {
  echo "$repo is not a Git repository; refusing to overwrite it" >&2; exit 1;
}
remote="$(git -C "$repo" remote get-url origin)"
case "$remote" in
  https://github.com/bisdat/kiln|https://github.com/bisdat/kiln.git|git@github.com:bisdat/kiln|git@github.com:bisdat/kiln.git) ;;
  *) echo "Unexpected origin $remote (expected bisdat/kiln)" >&2; exit 1 ;;
esac

git -C "$repo" fetch --no-tags --prune origin
if [[ "$ref" =~ ^upstream-pr/([1-9][0-9]*)$ ]]; then
  num="${BASH_REMATCH[1]}"
  git -C "$repo" fetch --no-tags https://github.com/frostdev-ops/kiln.git \
    "refs/pull/$num/head:refs/remotes/upstream/pr-$num"
  rev="refs/remotes/upstream/pr-$num"; label="upstream-pr-$num"
elif [[ "$ref" =~ ^pr/([1-9][0-9]*)$ || "$ref" =~ ^([1-9][0-9]*)$ ]]; then
  num="${BASH_REMATCH[1]}"
  git -C "$repo" fetch --no-tags origin \
    "refs/pull/$num/head:refs/remotes/origin/pr-$num"
  rev="refs/remotes/origin/pr-$num"; label="pr-$num"
else
  git check-ref-format --branch "$ref" >/dev/null || { usage >&2; exit 2; }
  [[ "$ref" != *..* ]] || { echo 'Invalid branch ref' >&2; exit 2; }
  rev="refs/remotes/origin/$ref"; label="branch-${ref//\//-}"
  git -C "$repo" show-ref --verify --quiet "$rev" || {
    echo "No branch $ref in bisdat/kiln" >&2; exit 1;
  }
fi
commit="$(git -C "$repo" rev-parse --verify "$rev^{commit}")"
out="$KILN_TEMP_DIR/$label"
worktree="$out/src"
mkdir -p "$out"
if [[ -e "$worktree" ]]; then
  git -C "$repo" worktree list --porcelain | grep -Fxq "worktree $worktree" || {
    echo "$worktree exists but is not a registered worktree of $repo; refusing to touch it" >&2; exit 1;
  }
  [[ -z "$(git -C "$worktree" status --porcelain --untracked-files=all)" ]] || {
    echo "Worktree $worktree has uncommitted changes; refusing to overwrite them" >&2; exit 1;
  }
  git -C "$worktree" switch --detach "$commit"
else
  git -C "$repo" worktree add --detach "$worktree" "$commit"
fi
if [[ "$mode" == ui || "$mode" == native ]]; then
  git -C "$worktree" submodule update --init --recursive
fi
echo "Building $ref @ ${commit:0:12} inside Distrobox $KILN_DISTROBOX_NAME"
echo "Git management clone: $repo"
echo "Temporary worktree and output: $out"
distrobox enter --name "$KILN_DISTROBOX_NAME" -- \
  bash "$here/distrobox-build.sh" --inside-build "$worktree" "$out" "$mode" "$tests_only" "$native_tests" "$target"
