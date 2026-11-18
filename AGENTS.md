# Kiln fork: contributor and assistant instructions

This file records the `bisdat/kiln` fork owner's workflow. It applies to work done
in this fork; it is **not** an upstream Kiln policy.

## Repository boundaries

- **Upstream project:** `frostdev-ops/kiln`.
- **Development fork:** `bisdat/kiln`.
- The reusable Bazzite Distrobox environment is **fork-only development tooling**:
  `tools/launcher/distrobox-env.sh`,
  `tools/launcher/distrobox-setup.sh`,
  `tools/launcher/distrobox-build.sh`, and
  `docs/kiln/launcher/distrobox.md`.
- Keep that tooling in the fork for local builds and regression testing. **Do not
  include it, or this `AGENTS.md` file, in any upstream PR** unless the fork
  owner explicitly requests upstream submission of those files.
- The fork owner merged the Distrobox tooling in fork PR #4. Its presence on
  `bisdat/kiln:main` does **not** indicate it has been accepted upstream.

## Mandatory upstream-PR hygiene

1. Fetch the current `frostdev-ops/kiln:main`. Make a fresh feature branch
   **from that upstream base**, not from the fork's `main`, for a new upstream
   contribution. Apply or cherry-pick only the intended change(s).
2. Before opening a PR, compare its head against the **current upstream
   `main`**, inspect all changed file paths and commits, and ensure the diff
   contains no fork-only Distrobox files, unrelated local modifications, or
   this instruction file.
3. Target `frostdev-ops/kiln:main` with the PR. Never merge or push changes to
   upstream simply because they have been merged into `bisdat/kiln`.
4. If upstream wants Distrobox tooling in the future, request **explicit
   approval from the fork owner first**, then submit a separate, focused PR.
5. The usual fork workflow may keep tools on fork `main`. Do not delete these
   files just to synchronize the fork with upstream.

## Relevant history (verify live status; it may have changed)

- Fork PR #3: GE-Proton `dinput8` launcher advice fix; merged into fork.
- Fork PR #4: reusable Bazzite Distrobox tooling; merged into fork, **not
  proposed upstream**.
- Upstream issue #77: developer approved conditional Steam-setting advice.
- Upstream PR #78: focused GE-Proton advice fix on branch
  `bisdat:fix/proton-dinput8-upstream`, created directly from upstream
  `main`, with **no Distrobox tooling**. Its current merge state must be
  checked on GitHub.

## Local build convention

Use the persistent `kiln-build` Distrobox. Source, worktrees, toolchains,
caches and test AppImages are organized under `~/Projects`; see
`docs/kiln/launcher/distrobox.md`. Treat `~/kiln-issue44` only as a historical
example, never as a build path.

For future conversations: review this file, inspect the actual GitHub base/head
and compare diff, and do **not** rely solely on remembered PR history.
