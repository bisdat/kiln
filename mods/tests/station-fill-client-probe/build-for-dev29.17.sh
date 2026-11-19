#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the all-station read-only probe against the *same* Kiln SDK as the user's dev.29.17 runtime.
# This keeps the research checkout's newer SDK (currently API 0.2.10) out of the package.
set -euo pipefail

TAG="v0.2.0-dev.29.17"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
WORK="${HOME}/.cache/kiln-issue68-dev29.17"
OUT="${KILN_PROBE_OUT:-${HOME}/Downloads}"
TARGET="${WORK}/mods/tests/station-fill-client-probe"
ZIP="${OUT}/station-fill-client-probe-0.1.1.zip"

if [[ ! -e "${WORK}/.git" ]]; then
    if [[ -e "${WORK}" ]]; then
        echo "Refusing to overwrite non-checkout directory: ${WORK}" >&2
        exit 1
    fi
    mkdir -p "$(dirname -- "${WORK}")"
    git clone --depth 1 --filter=blob:none --single-branch --branch "${TAG}" \
        https://github.com/frostdev-ops/kiln.git "${WORK}"
fi

# A cached checkout must truly be the expected tagged release, not a newer SDK.
tag_commit="$(git -C "${WORK}" rev-parse "refs/tags/${TAG}^{commit}")"
head_commit="$(git -C "${WORK}" rev-parse HEAD)"
if [[ "${tag_commit}" != "${head_commit}" ]]; then
    echo "Cached checkout is not at ${TAG}: ${WORK}" >&2
    exit 1
fi
if ! grep -Fq '<KilnModApiVersion>0.2.9</KilnModApiVersion>' \
    "${WORK}/managed/Kiln.Sdk/Sdk/Kiln.Version.props"; then
    echo "Expected SDK API 0.2.9 in ${TAG}; refusing to package." >&2
    exit 1
fi

mkdir -p "${TARGET}" "${OUT}"
cp -- "${HERE}/StationFillClientProbeMod.cs" "${TARGET}/"
cp -- "${HERE}/StationFillClientProbe.csproj" "${TARGET}/"

(
    cd -- "${WORK}"
    dotnet build \
        mods/tests/station-fill-client-probe/StationFillClientProbe.csproj \
        -c Release -t:KilnPack \
        -p:KilnPackageOutputPath="${OUT}/"
)

python3 - "${ZIP}" <<'PY'
import json
import sys
import zipfile

path = sys.argv[1]
with zipfile.ZipFile(path) as package:
    name = "station-fill-client-probe/kiln.mod.json"
    manifest = json.loads(package.read(name))
    version = manifest.get("apiVersion")
    if version != "0.2.9":
        raise SystemExit(f"INCORRECT apiVersion={version!r}; expected 0.2.9")
    if manifest.get("version") != "0.1.1":
        raise SystemExit(f"INCORRECT mod version={manifest.get('version')!r}; expected 0.1.1")
    print(f"VERIFIED: {path}")
    print(f"  mod version: {manifest['version']}")
    print(f"  Kiln API: {version}")
    print(f"  Kiln build: {manifest.get('kilnBuild')}")
PY

echo "Import the VERIFIED 0.1.1 ZIP through the normal Kiln Launcher workflow."
