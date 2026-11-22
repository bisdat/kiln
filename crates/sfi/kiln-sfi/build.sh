#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Invoke THROUGH the project's build slot, with KILN_OUT and CARGO_TARGET_DIR.
# The separate MIT workspace owns the Rust toolchain and lockfile. SFI_MANIFEST
# permits a scratch-only copy before that workspace has integrated.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
manifest=${SFI_MANIFEST:-$here/../Cargo.toml}
: "${KILN_OUT:?set KILN_OUT to your assigned build output}"
: "${CARGO_TARGET_DIR:?set CARGO_TARGET_DIR to your assigned Cargo output}"
if [[ ! -f $manifest ]]; then
  printf '%s\n' 'sfi: MIT workspace has not integrated; set SFI_MANIFEST to a scratch workspace' >&2
  exit 2
fi
# kiln_build_stamp() (build.rs, src/lib.rs) is "<VERSION><KILN_BUILD>", the stamp native/cmake/kiln_build_stamp.cpp
# gives the native DLLs of the same build: the core makes kiln/plugins/kiln-sfi.dll the ShroudForge-dialect owner only
# when it equals its own (native/core/src/api.cpp load_one). KILN_BUILD: the build metadata that build's native build
# generated (tools/package/stage.sh passes it). Unset: the native build's generated/kiln_build.h under this KILN_OUT,
# written first by native/cmake/kiln_build_stamp.sh, with its rules (SOURCE_DATE_EPOCH/KILN_GIT, the commit, else the
# nonce file the native build of this KILN_OUT then shares), when that build has not run yet. Set but empty: the
# version only, as a native DLL built without build metadata, which no core trusts.
repo=$(cd "$here/../../.." && pwd)
if [[ ! -v KILN_BUILD ]]; then
  generated=$KILN_OUT/cmake/generated
  if [[ ! -s $generated/kiln_build.h ]]; then
    bash "$repo/native/cmake/kiln_build_stamp.sh" "$repo" "$generated"
  fi
  KILN_BUILD=$(sed -n 's/^#define KILN_BUILD "\(.*\)"$/\1/p' "$generated/kiln_build.h")
  [[ -n $KILN_BUILD ]] || { printf 'sfi: %s holds no KILN_BUILD\n' "$generated/kiln_build.h" >&2; exit 2; }
fi
[[ -z $KILN_BUILD || $KILN_BUILD =~ ^\+[0-9A-Za-z.-]+$ ]] || { printf 'sfi: bad KILN_BUILD %s\n' "$KILN_BUILD" >&2; exit 2; }
export KILN_BUILD
cd "$(dirname "$manifest")"
flags=${CARGO_ENCODED_RUSTFLAGS:-}
flags=${flags:+$flags$'\x1f'}-Ctarget-feature=+crt-static
flags+=$'\x1f'-Cpanic=unwind
# Preserve release path-remapping flags. The C shim is compiled by cc under
# cargo-xwin's clang-cl/SDK environment; nothing is loaded into a game to build.
CARGO_ENCODED_RUSTFLAGS=$flags cargo xwin build --release --locked --offline \
  --manifest-path "$manifest" -p kiln-sfi --lib --target x86_64-pc-windows-msvc
mkdir -p "$KILN_OUT/cmake/bin/plugins"
install -m 755 "$CARGO_TARGET_DIR/x86_64-pc-windows-msvc/release/kiln_sfi.dll" \
  "$KILN_OUT/cmake/bin/plugins/kiln-sfi.dll"
printf 'sfi: %s (kiln_build_stamp %s%s)\n' "$KILN_OUT/cmake/bin/plugins/kiln-sfi.dll" "$(head -1 "$repo/VERSION")" "$KILN_BUILD"
