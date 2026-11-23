#!/usr/bin/env bash
# The API gate of a release (the public API record managed/**/PublicAPI.{Shipped,Unshipped}.txt and the mod API version
# in managed/Kiln.Api/Mods.cs). release.sh calls it; a worker can run it by hand.
#   tools/release/api-gate.sh removed OLD [NEW|--worktree]       shipped API lines removed since OLD (exit 1: named)
#   tools/release/api-gate.sh pending [REV|--worktree]           Unshipped.txt files that hold lines, with counts
#   tools/release/api-gate.sh version-rule OLD [NEW|--worktree]  new API pending needs a higher API version than OLD's
#   tools/release/api-gate.sh promote [--if-same-as REV] [--release NAME]
#                                                                Unshipped.txt lines into Shipped.txt (working tree),
#                                                                their rows into managed/api-history.tsv
#   tools/release/api-gate.sh api-version [REV|--worktree]       the API version in Mods.cs
#   tools/release/api-gate.sh bump-api [VERSION] [--major]       raise it (default the next minor) in Mods.cs,
#                                                                Kiln.Sdk's Kiln.Version.props and kiln-manage's profile.rs
#   tools/release/api-gate.sh history --seed                     managed/api-history.tsv from the release tags: the first
#                                                                release and mod API of every public entry
#   tools/release/api-gate.sh history --check                    exit 1 unless it matches the tags and the API record
#   tools/release/api-gate.sh history --append --release NAME [--mod-api X.Y.Z]   rows for entries that have none (repair)
# NEW and REV default to HEAD. Details: tools/release/lib/apigate.py, lib/apihistory.py (the ledger).
# Tests: python3 -I tools/release/test_apigate.py and tools/release/test_apihistory.py
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
exec python3 -I "$here/lib/apigate.py" --repo "${KILN_API_GATE_REPO:-$(cd "$here/../.." && pwd)}" "$@"
