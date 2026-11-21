#!/usr/bin/env bash
# Personal Bazzite build environment. Source only from the Distrobox scripts;
# no changes to the host shell profile, game or upstream Kiln runtime.
set -euo pipefail

KILN_PROJECTS_DIR="${KILN_PROJECTS_DIR:-$HOME/Projects}"
KILN_SOURCE_REPO="${KILN_SOURCE_REPO:-$KILN_PROJECTS_DIR/kiln}"
KILN_TEMP_DIR="${KILN_TEMP_DIR:-$HOME/Downloads/kiln-builds}"
KILN_TOOL_ROOT="${KILN_TOOL_ROOT:-$KILN_PROJECTS_DIR/.kiln-tools}"
KILN_DISTROBOX_NAME="${KILN_DISTROBOX_NAME:-kiln-build}"
KILN_DISTROBOX_IMAGE="${KILN_DISTROBOX_IMAGE:-registry.fedoraproject.org/fedora-toolbox:44}"

# Reuse the SDK that cargo-xwin downloaded; do not download one on every build.
XWIN_ROOT="${XWIN_ROOT:-$HOME/.cache/cargo-xwin/xwin}"
export KILN_PROJECTS_DIR KILN_SOURCE_REPO KILN_TEMP_DIR KILN_TOOL_ROOT
export KILN_DISTROBOX_NAME KILN_DISTROBOX_IMAGE XWIN_ROOT
export RUSTUP_HOME="$KILN_TOOL_ROOT/rustup"
export CARGO_HOME="$KILN_TOOL_ROOT/cargo-home"
export PATH="$CARGO_HOME/bin:$KILN_TOOL_ROOT/npm/bin:$PATH"
