#!/usr/bin/env bash
# Publishes a release that release.sh built and github-draft.sh drafted. This is the step that reaches users: run it
# by hand. It asks before each remote change (--yes skips the questions).
#   tools/release/publish.sh RELEASE_DIR --host SSH_HOST [--repo OWNER/NAME] [--no-dev-channel] [--no-beta-channel]
#                            [--sdk-site] [--yes] [--dry-run]
# --host: the frostdev.io web server as ssh knows it (or KILN_RELEASE_HOST); the files go to $KILN_RELEASE_DIR there,
#   default /opt/frostdev-downloads/kiln (the folder the site's nginx serves as /kiln/updates/ and /kiln/downloads/).
# --nexus: afterwards upload it to the Kiln page on Nexus Mods (tools/release/nexus.py --publish; it asks first).
# --sdk-site: also upload sdk/feed/ (the SDK as a static NuGet feed) and sdk/api/ (the modding site and API reference).
#   Without it both stay out of the upload and of the SHA256SUMS that goes up: publishing them is the owner's decision.
# --no-dev-channel: a stable release normally also becomes the dev channel's newest build; skip that for a hotfix
#   release when a newer dev build is out.
# --no-beta-channel: a release normally also becomes the beta channel's newest build (testers are offered the final
#   build), unless the beta channel already names a newer test build (read back from frostdev.io first); skip it.
# --dry-run: checks the folder and prints every remote command instead of running it (no ssh, rsync, gh or curl).
# A test build (release.sh --beta N): only the beta channel moves: downloads/<version>-beta.<n>/, updates/beta/ on
#   frostdev.io, the draft v<version>-beta.<n> published as a pre-release (never "latest"), the updates-beta pointer
#   (created when missing). Never the dev or stable manifests, updates-dev, Nexus Mods (--nexus is refused) or sdk/.
# 1. Checks the folder: built from a commit (README.txt says so; a test build of the working tree is refused before
#    anything else), SHA256SUMS, the updater signatures against the launcher's built-in key in tauri.conf.json.
# 2. frostdev.io: uploads the folder to <host>:/opt/frostdev-downloads/kiln/downloads/<version>/, then the manifests
#    (updates/frostdev.io/<channel>/latest.json) to /opt/frostdev-downloads/kiln/updates/<channel>/, each through a
#    temporary name, so a manifest never points at files that are not there yet. Then reads them back over https.
#    Never nexus/ (the Nexus Mods build, no self-update: only nexus.py uploads it), sdk/feed/ and sdk/api/ only with
#    --sdk-site; the SHA256SUMS that goes up lists what goes up.
# 3. GitHub: publishes the draft v<version> (a pre-release for dev builds; "latest" for a stable one) and points the
#    dev channel at it: the `updates-dev` pre-release holds the dev channel's latest.json (created when missing).
# The nginx locations for /kiln/updates/ and /kiln/downloads/ must be live on the host first (releasing.md).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
repo_dir=$(cd "$here/../.." && pwd)
dir="" gh_repo=frostdev-ops/kiln host=${KILN_RELEASE_HOST:-} yes=0 dev_channel=1 beta_channel=1 nexus=0 sdk_site=0 dry=0
while (($#)); do
  case $1 in
    --repo) gh_repo=$2; shift 2 ;;
    --host) host=$2; shift 2 ;;
    --yes) yes=1; shift ;;
    --no-dev-channel) dev_channel=0; shift ;;
    --no-beta-channel) beta_channel=0; shift ;;
    --dry-run) dry=1; yes=1; shift ;;
    --nexus) nexus=1; shift ;;
    --sdk-site) sdk_site=1; shift ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) dir=$(realpath "$1"); shift ;;
  esac
done
[[ -d $dir && -n $host ]] || { echo "usage: $0 RELEASE_DIR --host SSH_HOST [--repo OWNER/NAME] [--no-dev-channel] [--sdk-site] [--yes]" >&2; exit 2; }
# a test build of a working tree (release.sh --worktree) never goes up: README.txt names the commit, as github-draft.sh
# reads it, or nothing is done
grep -Eq 'from commit [0-9a-f]{7,40}' "$dir/README.txt" 2>/dev/null ||
  { echo "$dir was not built from a commit (a test build of a working tree?): never published" >&2; exit 1; }
ask() { ((yes)) && return 0; read -r -p "$1 [y/N] " a; [[ $a == y || $a == Y ]]; }
# every remote command goes through r: printed instead of run in a dry run
r() { if ((dry)); then printf 'would run:'; printf ' %q' "$@"; echo; else "$@"; fi; }
# what stays out of downloads/<version>/: sums_skip (a grep -E alternative of SHA256SUMS paths) and rsync_skip
publish_filter() {
  sums_skip='nexus|updates' rsync_skip=(--exclude updates/ --exclude /nexus/ --exclude /SHA256SUMS)
  if [[ $channel == beta ]]; then
    sums_skip+='|sdk' rsync_skip+=(--exclude /sdk/)
    echo "a test build: sdk/ stays out (its packages carry the release's version)"
  elif ((sdk_site)); then
    echo "sdk/feed/ and sdk/api/ go up too (--sdk-site): the NuGet feed and the API reference become public"
  else
    sums_skip+='|sdk/feed|sdk/api' rsync_skip+=(--exclude /sdk/feed/ --exclude /sdk/api/)
    echo "sdk/feed/ (the NuGet feed) and sdk/api/ (the API reference) stay out; --sdk-site publishes them"
  fi
}
version=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' "$dir/updates/github/latest.json")
beta=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("kiln", {}).get("beta") or "")' "$dir/updates/github/latest.json")
case $version in *-*) channel=dev ;; *) channel=stable ;; esac
# the id the files are signed for (a test build's: X.Y.Z-dev.N+beta.B), and the name of its tag and download folder
build_id=$version name=$version
if [[ -n $beta ]]; then
  [[ $version == *"+beta.$beta" ]] || { echo "the manifest's version $version does not name beta $beta" >&2; exit 1; }
  channel=beta version=${version%%+*} name=${version%%+*}-beta.$beta
  ((nexus)) && { echo "a test build never goes to Nexus Mods: drop --nexus" >&2; exit 2; }
  # a test build's folder holds the beta channel's manifests only
  for f in "$dir"/updates/frostdev.io/*/latest.json; do
    [[ $f == "$dir/updates/frostdev.io/beta/latest.json" ]] || { echo "a test build's folder must not hold $f" >&2; exit 1; }
  done
fi
tag=v$name
remote=${KILN_RELEASE_DIR:-/opt/frostdev-downloads/kiln}

echo "== checking $dir ($name, $channel)"
python3 "$here/lib/release.py" sums "$dir" --check
pub=$(mktemp) sums=$(mktemp)
trap 'rm -f "$pub" "$sums"' EXIT
python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["plugins"]["updater"]["pubkey"])' \
  "$repo_dir/launcher/src-tauri/tauri.conf.json" >"$pub"
mapfile -t signed < <(find "$dir/linux" "$dir/windows" -name '*.sig' | sed 's/\.sig$//')
python3 "$here/lib/release.py" verify --pubkey "$pub" --version "$build_id" "${signed[@]}"
if ((dry)); then
  echo "would check: gh release view $tag --repo $gh_repo (a draft)"
else
  gh release view "$tag" --repo "$gh_repo" --json isDraft -q .isDraft | grep -qx true \
    || { echo "no draft $tag on $gh_repo (tools/release/github-draft.sh first)" >&2; exit 1; }
fi

# the channels this folder moves: a test build only beta; a release its own, dev for a stable one (unless
# --no-dev-channel), and beta unless --no-beta-channel or the beta channel names a newer test build already
beta_newer() { # is the live beta manifest's build newer than this release? (unreadable: no)
  ((dry)) && { echo "would read: https://frostdev.io/kiln/updates/beta/latest.json" >&2; return 1; }
  local live
  live=$(curl -fsS "https://frostdev.io/kiln/updates/beta/latest.json" 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["version"])' 2>/dev/null) || return 1
  [[ $(python3 "$here/lib/release.py" order "$live" "$build_id") == 1 ]] && { echo "the beta channel names $live, newer than $build_id: it stays" >&2; return 0; }
  return 1
}
channels=()
for f in "$dir"/updates/frostdev.io/*/latest.json; do
  ch=$(basename "$(dirname "$f")")
  case $ch in
    beta) [[ $channel == beta ]] || { ((beta_channel)) && ! beta_newer; } || continue ;;
    dev) [[ $channel == stable && $dev_channel == 0 ]] && continue ;;
  esac
  channels+=("$ch")
done
echo "channels to move: ${channels[*]}"

echo "== frostdev.io ($host:$remote)"
if ask "Upload $name to $host:$remote/downloads/$name and switch the manifests of ${channels[*]}?"; then
  r ssh "$host" mkdir -p "$remote/downloads" "${channels[@]/#/$remote/updates/}"
  # The download folder excludes Nexus builds and update manifests (published separately), and without --sdk-site the
  # NuGet feed and the API reference (publish_filter: the same paths out of the rsync and out of the SHA256SUMS); a
  # test build's sdk/ never goes up (its packages carry the release's version)
  publish_filter
  grep -Ev "^[0-9a-f]+  ($sums_skip)/" "$dir/SHA256SUMS" >"$sums"
  r rsync -a --chmod=D755,F644 "${rsync_skip[@]}" "$dir/" "$host:$remote/downloads/$name/"
  r rsync -a --chmod=F644 "$sums" "$host:$remote/downloads/$name/SHA256SUMS"
  for ch in "${channels[@]}"; do
    f=$dir/updates/frostdev.io/$ch/latest.json
    r rsync -a --chmod=F644 "$f" "$host:$remote/updates/$ch/.latest.json.new"
    r ssh "$host" mv -f "$remote/updates/$ch/.latest.json.new" "$remote/updates/$ch/latest.json"
    ((dry)) && continue
    got=$(curl -fsS "https://frostdev.io/kiln/updates/$ch/latest.json" | sha256sum | cut -d' ' -f1)
    [[ $got == "$(sha256sum <"$f" | cut -d' ' -f1)" ]] && echo "https://frostdev.io/kiln/updates/$ch/latest.json is $build_id" \
      || echo "WARNING: https://frostdev.io/kiln/updates/$ch/latest.json does not match yet (cache?)" >&2
  done
  ((dry)) || { curl -fsSI "https://frostdev.io/kiln/downloads/$name/SHA256SUMS" >/dev/null && echo "downloads reachable"; }
fi

# the fixed pre-release that holds a channel's latest.json on GitHub (created when missing)
pointer() { # release-name title-word
  if ((dry)) || ! gh release view "$1" --repo "$gh_repo" >/dev/null 2>&1; then
    r gh release create "$1" --repo "$gh_repo" --prerelease --target "$( ((dry)) && echo HEAD || git -C "$repo_dir" rev-parse HEAD)" \
      --title "Kiln Launcher $2 channel" \
      --notes "The $2 channel's update manifest (latest.json), for the launcher's self-update. Not a release: the builds are in the v* releases."
  fi
  r gh release upload "$1" --repo "$gh_repo" --clobber "$dir/updates/github/latest.json"
}
echo "== GitHub ($gh_repo)"
if ask "Publish the draft $tag and point the GitHub ${channels[*]} channels at it?"; then
  if [[ $channel == stable ]]; then r gh release edit "$tag" --repo "$gh_repo" --draft=false --latest
  else r gh release edit "$tag" --repo "$gh_repo" --draft=false --prerelease; fi
  # the dev channel's fixed pre-release (GitHub's "latest" skips pre-releases); a stable release also serves dev
  if [[ " ${channels[*]} " == *" dev "* ]]; then
    pointer updates-dev dev
    echo "published $tag; dev channel -> $build_id"
  elif [[ $channel != beta ]]; then
    echo "published $tag; the dev channel stays where it was"
  fi
  # the beta channel's: a test build, or a release a tester is offered (the same manifest, GitHub's file URLs)
  if [[ " ${channels[*]} " == *" beta "* ]]; then
    pointer updates-beta beta
    echo "published $tag; beta channel -> $build_id"
  else
    echo "the beta channel stays where it was"
  fi
fi

if ((nexus)); then
  echo "== Nexus Mods"
  python3 -I "$here/nexus.py" "$dir"
  if ask "Upload these files to Nexus Mods and post the changelog?"; then
    python3 -I "$here/nexus.py" "$dir" --publish
  fi
fi
