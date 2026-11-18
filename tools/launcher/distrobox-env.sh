#!/usr/bin/env bash
# Shared build tool locations. The source, toolchains, targets and artifacts all live under ~/Projects.
# Source this file from Kiln's Distrobox scripts, never from a user's shell profile.
set -euo pipefail

KILN_PROJECTS_DIR="${KILN_PROJECTS_DIR:-$HOME/Projects}"
KILN_TOOL_ROOT="$KILN_PROJECTS_DIR/.kiln-tools"
KILN_DISTROBOX_NAME="${KILN_DISTROBOX_NAME:-kiln-build}"
KILN_DISTROBOX_IMAGE="${KILN_DISTROBOX_IMAGE:-registry.fedoraproject.org/fedora-toolbox:44}"

export KILN_PROJECTS_DIR KILN_TOOL_ROOT KILN_DISTROBOX_NAME KILN_DISTROBOX_IMAGE
export RUSTUP_HOME="$KILN_TOOL_ROOT/rustup"
export CARGO_HOME="$KILN_TOOL_ROOT/cargo-home"
export PATH="$CARGO_HOME/bin:$KILN_TOOL_ROOT/npm/bin:$PATH"
