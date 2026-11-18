# Reusable Kiln test builds in Distrobox (Bazzite)

Kiln's launcher is a Tauri 2 / WebKitGTK 4.1 application. Keep the Fedora build
dependencies and toolchains inside a persistent Distrobox instead of layering
packages onto the Bazzite host. Source, non-root tools, worktrees, caches and
AppImages stay in `~/Projects`. No files in `~/kiln-issue44` are consulted.

The default container is **kiln-build**, from
`registry.fedoraproject.org/fedora-toolbox:44`. Distrobox itself uses Podman's
normal container storage. The container shares your home directory, so do not
treat it as a security sandbox. The scripts never use `rpm-ostree`.

## First setup

On Bazzite (host terminal):

```bash
mkdir -p ~/Projects
git clone --branch tooling/reusable-kiln-distrobox \
  https://github.com/bisdat/kiln.git ~/Projects/kiln
bash ~/Projects/kiln/tools/launcher/distrobox-setup.sh
```

If `~/Projects/kiln` already exists, **do not clone over it**. In that Git
repository, fetch and switch to the tooling branch only after saving any
uncommitted changes. After this tooling PR is merged, use `main` instead of
the temporary tooling branch.

Setup is repeatable. The installer adds Fedora packages **inside Distrobox**
and .NET 10 inside the container, plus Rust stable (rustup) and Bun (npm) into
`~/Projects/.kiln-tools`. It checks
for WebKitGTK 4.1. Setup needs network access to download packages/toolchains.

## Test PR #3

```bash
bash ~/Projects/kiln/tools/launcher/distrobox-build.sh 3
```

This command:

1. Fetches PR #3 from GitHub into the management clone at `~/Projects/kiln`.
2. Creates or refreshes a **detached** checkout at `~/Projects/kiln-worktrees/pr-3` (refuses to overwrite local changes).
3. Runs the full `kiln-manage` discovery regression suite in `kiln-build`.
4. Installs the launcher frontend's pinned Bun dependencies and typechecks it.
5. Builds a **launcher-only** AppImage, with no bundled game runtime or self-update.

Output: `~/Projects/kiln-builds/pr-3/bundles/linux-nexus/*.AppImage`.

Close the normal Kiln launcher before running the AppImage. It uses the usual
Kiln data folder unless you deliberately set `KILN_DATA_DIR`. It is a test
launcher, not an official signed updater build. Do not install or update the
game runtime: this PR changes only launcher-side advice. To run:

```bash
cd ~/Projects/kiln-builds/pr-3/bundles/linux-nexus
APPIMAGE_EXTRACT_AND_RUN=1 ./*.AppImage
```

With Enshrouded set to GE-Proton 11-12 and Steam launch options empty, **One
Steam Setting** should not appear; the game and all installed mods should
continue working. Older GE-Proton versions retain the advice. The fix checks
Steam's *selected app-specific* compatibility-tool mapping; merely having
GE-Proton 11-12 installed does not count.

## Future builds

Use the same persistent container and tools:

```bash
# A different PR in bisdat/kiln
bash ~/Projects/kiln/tools/launcher/distrobox-build.sh 7

# A branch in bisdat/kiln (including branches with slashes)
bash ~/Projects/kiln/tools/launcher/distrobox-build.sh fix/example

# Test the Rust discovery suite without packaging
bash ~/Projects/kiln/tools/launcher/distrobox-build.sh 3 --tests-only

# Recheck/update Fedora and toolchain dependencies
bash ~/Projects/kiln/tools/launcher/distrobox-setup.sh
```

All builds share `~/Projects/.kiln-tools/target` (Cargo's compile cache) and
the rustup/Bun installations. Each PR or branch gets its own source worktree
and output folder. Changing a revision when its worktree has uncommitted
changes is refused, never automatically reset. A build of an unknown PR is
also refused; only `bisdat/kiln` is fetched.

If you want an interactive shell to investigate build errors:

```bash
distrobox enter --name kiln-build
source ~/Projects/kiln/tools/launcher/distrobox-env.sh
cd ~/Projects/kiln-worktrees/pr-3
```

`--nexus` is deliberately used **only as a way to disable launcher
self-update**, not to modify any mods. `--runtime none` omits the game runtime
bundle, so these launcher-only test AppImages do not ship replacement Kiln
services. The normal launcher and game remain installed and unchanged by
building.

## References

- [Tauri 2 Fedora build prerequisites](https://v2.tauri.app/start/prerequisites/)
- [Distrobox creation](https://distrobox.it/usage/distrobox-create/)
- [Kiln launcher build script](../../../tools/launcher/build-linux.sh)
- [Kiln launcher development instructions](app.md#running-it)
