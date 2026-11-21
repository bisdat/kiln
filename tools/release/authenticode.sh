#!/usr/bin/env bash
# Optional Windows Authenticode signing with Azure Artifact Signing (formerly Trusted Signing) through jsign.
#   tools/release/authenticode.sh available        exit 0 when the credentials file exists
#   tools/release/authenticode.sh sign FILE...     sign and timestamp each PE file (the Tauri signCommand calls this)
#   tools/release/authenticode.sh verify FILE...   check each file's signature (osslsigncode verify)
# Credentials: $KILN_AUTHENTICODE_ENV, default ~/.config/kiln-release/authenticode.env (keys: releasing.md,
# "Authenticode"). Nothing here prints a secret: the access token reaches jsign through the environment.
# Needs: jsign (the `jsign` command, or JSIGN=<path to jsign.jar> in the env file, with java) and, to verify,
# osslsigncode. Timestamps: http://timestamp.acs.microsoft.com (RFC 3161).
set -euo pipefail
env_file=${KILN_AUTHENTICODE_ENV:-$HOME/.config/kiln-release/authenticode.env}
cmd=${1:-}
shift || true

case $cmd in
  available) [[ -f $env_file ]]; exit ;;
  sign|verify) ;;
  *) echo "usage: $0 available | sign FILE... | verify FILE..." >&2; exit 2 ;;
esac

# files at once: verify and sign run this many at the same time (each signing job is one jsign run for its share of the
# files, so one JVM and one connection per job instead of one per file)
par=${KILN_SIGN_JOBS:-4}
[[ $par =~ ^[1-9][0-9]*$ ]] || par=4

if [[ $cmd == verify ]]; then
  command -v osslsigncode >/dev/null || { echo "authenticode: osslsigncode is not installed (needed to verify)" >&2; exit 1; }
  verify_one() {
    if osslsigncode verify -in "$1" >/dev/null 2>&1; then echo "authenticode OK: $(basename "$1")"
    else echo "authenticode FAILED: $1" >&2; return 1; fi
  }
  fail=0; pids=()
  for f in "$@"; do
    while (( $(jobs -rp | wc -l) >= par * 2 )); do wait -n || fail=1; done
    verify_one "$f" & pids+=($!)
  done
  for p in "${pids[@]}"; do wait "$p" || fail=1; done
  exit $fail
fi

[[ -f $env_file ]] || { echo "authenticode: no $env_file" >&2; exit 1; }
set -a
# shellcheck disable=SC1090
source "$env_file"
set +a
for k in AZURE_TENANT_ID AZURE_CLIENT_ID AZURE_CLIENT_SECRET AZURE_SIGNING_ENDPOINT AZURE_SIGNING_ACCOUNT AZURE_SIGNING_PROFILE; do
  [[ -n ${!k:-} ]] || { echo "authenticode: $k is not set in $env_file" >&2; exit 1; }
done
if [[ -n ${JSIGN:-} ]]; then jsign=(java -jar "$JSIGN"); else
  command -v jsign >/dev/null || { echo "authenticode: jsign is not installed (or set JSIGN=<jsign.jar> in $env_file)" >&2; exit 1; }
  jsign=(jsign)
fi

# an access token for the signing service (Entra client credentials), reused for an hour across calls of one release
cache=${XDG_RUNTIME_DIR:-/tmp}/kiln-authenticode-$(id -u).token
if [[ ! -s $cache || $(($(date +%s) - $(stat -c %Y "$cache"))) -gt 3000 ]]; then
  umask 077
  printf '%s' "$AZURE_CLIENT_SECRET" | curl -fsS "https://login.microsoftonline.com/$AZURE_TENANT_ID/oauth2/v2.0/token" \
    --data-urlencode "client_id=$AZURE_CLIENT_ID" --data-urlencode "client_secret@-" \
    --data-urlencode "scope=https://codesigning.azure.net/.default" --data-urlencode "grant_type=client_credentials" \
    | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])' >"$cache.tmp"
  mv "$cache.tmp" "$cache"
fi
KILN_SIGNING_TOKEN=$(cat "$cache")
export KILN_SIGNING_TOKEN
endpoint=${AZURE_SIGNING_ENDPOINT#https://}
endpoint=${endpoint%/}
sign_batch() {
  "${jsign[@]}" --storetype TRUSTEDSIGNING --keystore "$endpoint" --storepass env:KILN_SIGNING_TOKEN \
    --alias "$AZURE_SIGNING_ACCOUNT/$AZURE_SIGNING_PROFILE" --alg SHA-256 \
    --tsaurl http://timestamp.acs.microsoft.com --tsmode RFC3161 --replace "$@" >/dev/null
  local f; for f in "$@"; do echo "authenticode: signed $(basename "$f")"; done
}
files=("$@")
n=${#files[@]}
((n)) || exit 0
((par > n)) && par=$n
if ((par == 1)); then sign_batch "${files[@]}"; exit; fi
# deal the files out round-robin, one jsign run per share, all at the same time; any failed share fails the call
pids=()
for ((j = 0; j < par; j++)); do
  share=()
  for ((x = j; x < n; x += par)); do share+=("${files[x]}"); done
  sign_batch "${share[@]}" & pids+=($!)
done
fail=0
for p in "${pids[@]}"; do wait "$p" || fail=1; done
((fail == 0)) || { echo "authenticode: signing failed (see above)" >&2; exit 1; }
