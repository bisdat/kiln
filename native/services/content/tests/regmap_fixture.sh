#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Sourced by the registry back-end suites: regmap_fixture <out dir> copies the repository's regmap/ into <out dir>/regmap-all-ok
# with every row of every per-build record set to "ok" (and its unproven reasons dropped), and prints that folder.
# The back-end suites test what a mapped row does on the build's real layout; which rows the lab has proven on a build
# (the committed record's unproven rows, kilnctl regproof) is the regmap suite's and kilnctl's to test, not theirs.
regmap_fixture() {
    local repo dst
    repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
    dst=$1/regmap-all-ok
    rm -rf "$dst" && cp -r "$repo/regmap" "$dst"
    python3 -I - "$dst" <<'EOF'
import json, pathlib, sys
for side in ("server", "client"):
    for f in sorted((pathlib.Path(sys.argv[1]) / side).glob("*.json")):
        doc = json.loads(f.read_text(encoding="utf-8"))
        doc["rows"] = {row: "ok" for row in doc.get("rows", {})}
        doc.pop("reasons", None)
        f.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
EOF
    echo "$dst"
}
