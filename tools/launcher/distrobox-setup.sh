#!/usr/bin/env bash
# One-time (repeatable) Fedora 44 build environment for Bazzite/Kiln.
# Installs Fedora packages only inside the persistent Distrobox.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=distrobox-env.sh
source "$here/distrobox-env.sh"

if [[ "${1:-}" == "--inside" ]]; then
  [[ $# == 1 ]] || { echo "Usage: distrobox-setup.sh [--inside]" >&2; exit 2; }
  command -v dnf >/dev/null || { echo "Expected a Fedora Distrobox with dnf" >&2; exit 1; }
  # libasan/libubsan are required by the portable native UI tests. libtsan
  # supports the optional full native test suite (including thread tests).
  sudo dnf install -y \
    bash git curl wget file unzip zip xz tar gzip rsync \
    gcc gcc-c++ make cmake ninja-build clang llvm lld \
    libasan libubsan libtsan \
    pkgconf-pkg-config openssl-devel webkit2gtk4.1-devel gtk3-devel \
    libappindicator-gtk3-devel librsvg2-devel libxdo-devel \
    patchelf python3 nodejs npm dotnet-sdk-10.0

  mkdir -p "$CARGO_HOME" "$RUSTUP_HOME" "$KILN_TOOL_ROOT/npm" "$KILN_TEMP_DIR"
  if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
    echo "Installing rustup to $RUSTUP_HOME (not to the Bazzite host)"
    curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --no-modify-path
  fi
  "$CARGO_HOME/bin/rustup" toolchain install stable --profile minimal
  "$CARGO_HOME/bin/rustup" default stable
  if [[ ! -x "$KILN_TOOL_ROOT/npm/bin/bun" ]]; then
    npm install --global --prefix "$KILN_TOOL_ROOT/npm" bun
  fi
  pkg-config --exists webkit2gtk-4.1 || {
    echo "Missing WebKitGTK 4.1 development files" >&2; exit 1;
  }
  echo "Distrobox toolchains ready:"
  printf '  Rust: '; rustc --version
  printf '  Cargo: '; cargo --version
  printf '  Bun: '; bun --version
  printf '  .NET: '; dotnet --version
  printf '  WebKitGTK: '; pkg-config --modversion webkit2gtk-4.1
  if [[ -f "$XWIN_ROOT/sdk/include/um/windows.h" ]]; then
    echo "  Windows x64 SDK: $XWIN_ROOT"
  else
    echo "  Windows x64 SDK: not installed at $XWIN_ROOT"
    echo "  To cross-build native Kiln DLLs, install cargo-xwin and populate its SDK (see distrobox.md)."
  fi
  echo "  Git repositories: $KILN_PROJECTS_DIR"
  echo "  Temporary worktrees/builds: $KILN_TEMP_DIR"
  exit 0
fi

[[ $# == 0 ]] || { echo "Usage: bash distrobox-setup.sh" >&2; exit 2; }
bash -n "$here/distrobox-env.sh" "$here/distrobox-setup.sh" "$here/distrobox-build.sh"
command -v distrobox >/dev/null || { echo "Distrobox is required on Bazzite" >&2; exit 1; }
command -v podman >/dev/null || { echo "Podman is required on Bazzite" >&2; exit 1; }
mkdir -p "$KILN_PROJECTS_DIR" "$KILN_TOOL_ROOT" "$KILN_TEMP_DIR"
if podman container exists "$KILN_DISTROBOX_NAME"; then
  echo "Reusing existing Distrobox: $KILN_DISTROBOX_NAME"
else
  echo "Creating Distrobox: $KILN_DISTROBOX_NAME ($KILN_DISTROBOX_IMAGE)"
  distrobox create --yes --name "$KILN_DISTROBOX_NAME" --image "$KILN_DISTROBOX_IMAGE"
fi
distrobox enter --name "$KILN_DISTROBOX_NAME" -- bash "$here/distrobox-setup.sh" --inside
