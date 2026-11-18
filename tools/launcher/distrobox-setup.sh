#!/usr/bin/env bash
# One-time (and idempotent) Kiln build environment for Bazzite.
# Run on the Bazzite host: bash tools/launcher/distrobox-setup.sh
# Uses Fedora 44 only inside Distrobox; never layers packages onto the host.
set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=distrobox-env.sh
source "$here/distrobox-env.sh"
mkdir -p "$KILN_PROJECTS_DIR" "$KILN_TOOL_ROOT"

if [[ "${1:-}" == "--inside" ]]; then
  # All non-root tools, dependencies and caches live under Projects, not ~/.cargo or ~/.rustup.
  command -v dnf >/dev/null || { echo "Expected a Fedora Distrobox with dnf" >&2; exit 1; }
  sudo dnf install -y \
    bash git curl wget file unzip zip xz tar gzip \
    gcc gcc-c++ make cmake ninja-build clang lld \
    pkgconf-pkg-config openssl-devel webkit2gtk4.1-devel gtk3-devel \
    libappindicator-gtk3-devel librsvg2-devel libxdo-devel \
    patchelf python3 nodejs npm

  mkdir -p "$CARGO_HOME" "$RUSTUP_HOME" "$KILN_TOOL_ROOT/npm"
  if [[ ! -x "$CARGO_HOME/bin/rustup" ]]; then
    echo "Installing rustup into $RUSTUP_HOME (not the Bazzite host's toolchain)"
    curl --proto '=https' --tlsv1.2 -fsSL https://sh.rustup.rs \
      | sh -s -- -y --profile minimal --no-modify-path
  fi
  "$CARGO_HOME/bin/rustup" toolchain install stable --profile minimal
  "$CARGO_HOME/bin/rustup" default stable

  if [[ ! -x "$KILN_TOOL_ROOT/npm/bin/bun" ]]; then
    npm install --global --prefix "$KILN_TOOL_ROOT/npm" bun
  fi

  pkg-config --exists webkit2gtk-4.1 || {
    echo "WebKitGTK 4.1 development files were not detected" >&2
    exit 1
  }
  echo "Kiln Distrobox build tools ready:"
  printf '  Rust: '; rustc --version
  printf '  Cargo: '; cargo --version
  printf '  Bun: '; bun --version
  printf '  Node: '; node --version
  printf '  WebKitGTK: '; pkg-config --modversion webkit2gtk-4.1
  printf '  Projects: %s\n' "$KILN_PROJECTS_DIR"
  exit 0
fi

[[ $# == 0 ]] || { echo "Usage: bash tools/launcher/distrobox-setup.sh" >&2; exit 2; }
command -v distrobox >/dev/null || { echo "Install Distrobox on Bazzite first" >&2; exit 1; }
command -v podman >/dev/null || { echo "Podman is required for this Bazzite workflow" >&2; exit 1; }

if podman container exists "$KILN_DISTROBOX_NAME"; then
  echo "Reusing Distrobox: $KILN_DISTROBOX_NAME"
else
  echo "Creating Distrobox: $KILN_DISTROBOX_NAME ($KILN_DISTROBOX_IMAGE)"
  distrobox create --yes --name "$KILN_DISTROBOX_NAME" --image "$KILN_DISTROBOX_IMAGE"
fi

distrobox enter --name "$KILN_DISTROBOX_NAME" -- bash "$here/distrobox-setup.sh" --inside
