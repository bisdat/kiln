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
printf 'sfi: %s\n' "$KILN_OUT/cmake/bin/plugins/kiln-sfi.dll"
