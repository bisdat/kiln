# Personal Kiln build environment on Bazzite (Distrobox)

These scripts are personal developer infrastructure on the dedicated
`tooling/reusable-kiln-distrobox` branch of `bisdat/kiln`. They are **not** part
of Kiln's public runtime or upstream `main`. Keep them in a separate checkout;
do not merge them back into fork `main` or upstream.

The existing Fedora 44 Distrobox is named **`kiln-build`**. It holds the Fedora
packages, while Rust, Bun and their persistent toolchains live under
`~/Projects/.kiln-tools`. The container shares the host home directory; it is
**not** a security sandbox. These scripts never install files into Enshrouded,
replace a Kiln runtime, or use `rpm-ostree`.

## Directory layout

| Path | Purpose |
|---|---|
| `~/Projects/kiln` | Git management clone of `bisdat/kiln` (or `KILN_SOURCE_REPO`) |
| `~/Projects/.kiln-tools` | Persistent Rust, Cargo, Bun toolchains |
| `~/Downloads/kiln-builds/<ref>/src` | Temporary, detached Git worktree |
| `~/Downloads/kiln-builds/<ref>/build` | Temporary build products (native, .NET and Cargo) |
| `~/Downloads/kiln-builds/<ref>/artifacts` | Test reports and compiled outputs |
| `~/Downloads/kiln-builds/<ref>/logs` | Timestamped logs |
| `~/Downloads/kiln-builds/<ref>/cache` | Per-ref temporary NuGet, Bun and XDG caches |

You may override `KILN_PROJECTS_DIR`, `KILN_SOURCE_REPO`, `KILN_TEMP_DIR`,
`KILN_TOOL_ROOT`, or `KILN_DISTROBOX_NAME` in the environment. By default,
source stays in `~/Projects` and temporary work stays in `~/Downloads`.
The native Windows cross-compiler uses the existing cargo-xwin Windows SDK
(default `$HOME/.cache/cargo-xwin/xwin`, overridable with `XWIN_ROOT`).

## Separate tooling checkout

If you already have a tooling checkout, continue using it. Otherwise:

```bash
mkdir -p ~/Projects
git clone --branch tooling/reusable-kiln-distrobox \
  https://github.com/bisdat/kiln.git ~/Projects/kiln-tooling
```

This is deliberately separate from `~/Projects/kiln`, the management clone.
The branch can be updated independently without switching the management
clone away from its existing branch.

## Setup / repair the existing Distrobox

Run once and again when Fedora dependencies change:

```bash
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-setup.sh
```

Setup reuses `kiln-build` if present, installs Fedora 44 packages *inside* the
container and checks Rust stable, Bun, .NET 10 and WebKitGTK 4.1. Unlike the
original launcher-only setup, it also installs `libasan`, `libubsan` and
`libtsan` for the portable C++ tests. **The Bazzite host is unchanged.**

Native DLL cross-builds require `clang-cl`, `lld-link`, CMake, Ninja, and the
cargo-xwin Windows SDK. The setup reuses an SDK if already available and
reports if missing; it does not silently download a new SDK. To prepare a new
container, from inside `kiln-build`:

```bash
cargo install cargo-xwin
cargo xwin env
```

Follow the current Kiln native toolchain documentation if cargo-xwin changes
its SDK provisioning procedure.

## Reusable build modes

Run commands on the Bazzite **host**. The script fetches source from the
selected GitHub repository into the management clone, safely updates/creates
a detached worktree under `~/Downloads`, then runs the requested build inside
`kiln-build`.

```bash
# Historical fork PR numbering still works (launcher-only AppImage):
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh 3

# Upstream pull request #106: targeted native/managed/UI checks and two DLL builds
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh \
  upstream-pr/106 --mode ui

# A branch on bisdat/kiln: native host tests and a single Windows native target
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh \
  feature/chat-history-ctrl-r --mode native --target kiln-ui

# Managed Kiln.Ui unit tests and build (or tests only)
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh \
  feature/chat-history-ctrl-r --mode managed

# Full native UI host suite, including TSan; optionally omit DLL build
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh \
  upstream-pr/106 --mode native --native-tests full --tests-only

# Rust discovery tests only (no launcher build)
bash ~/Projects/kiln-tooling/tools/launcher/distrobox-build.sh 3 --tests-only
```

`REF` forms are `3` / `pr/3` (fork PR), `upstream-pr/106` (upstream PR), or
a branch such as `feature/my-branch` (in `bisdat/kiln`). `--mode` defaults to
`launcher`, preserving the original CLI. For native mode, `--target` selects
a CMake `kiln-*` target (default `kiln-ui`).

### What each mode runs

| Mode | Tests | Build (omitted by `--tests-only`) |
|---|---|---|
| `launcher` | `kiln-manage` Rust discovery suite | Bun typecheck + launcher-only AppImage, no runtime/self-update |
| `native` | Six portable chat/UI host checks (ASan/UBSan), or `--native-tests full` | Requested native Windows `kiln-*` target |
| `managed` | Managed `Kiln.Ui.Tests` | Managed `Kiln.Ui.dll` |
| `ui` | Native host checks + managed `Kiln.Ui.Tests` + launcher i18n | Windows `kiln-ui.dll` and managed `Kiln.Ui.dll` |

The native targeted checks are selected from the test files present in that
revision (e.g. an old branch may not have `chat_search_test.cpp`). The full
native suite uses Kiln's `native/services/ui/tests/run.sh`, including the
ThreadSanitizer tests, and may require additional host dependencies. All
failure codes propagate; neither the script nor a successful build proves an
in-game test passed.

### Outputs and safety

Check `~/Downloads/kiln-builds/<ref>/artifacts` for DLLs and test results,
and `logs/build-<UTC>.log` for failures. A Windows native DLL and managed
service DLL are **not a complete, safe-to-install runtime**. Use a separately
reviewed installation/restore procedure before modifying the game.

Existing worktrees must be clean; the script refuses to overwrite changes or
unrelated directories. No automatic cleanup or destructive reset occurs.
Only `bisdat/kiln` branches/fork PRs and `frostdev-ops/kiln` upstream PRs
are fetched. The management clone's checked-out branch is not switched.

As before, launcher-only AppImages are **test builds**. Close the normal
launcher before running one, and avoid updating the game runtime with it:

```bash
cd ~/Downloads/kiln-builds/pr-3/artifacts/launcher
APPIMAGE_EXTRACT_AND_RUN=1 ./*.AppImage
```

## References

- [Kiln native cross-builds](../building-on-linux.md)
- [Kiln launcher script](../../../tools/launcher/build-linux.sh)
- [Tauri Fedora prerequisites](https://v2.tauri.app/start/prerequisites/)
- [Distrobox documentation](https://distrobox.it/usage/distrobox-create/)
