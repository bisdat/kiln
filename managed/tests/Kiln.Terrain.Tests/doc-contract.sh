#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/../../.."
python3 - <<'PY'
from pathlib import Path
service=Path('docs/kiln/services/terrain.md').read_text()
limits=next(x for x in Path('docs/kiln/modding/limits.md').read_text().splitlines() if x.startswith('| Terrain changes by mods (new)'))
caps=next(x for x in Path('docs/kiln/services/caps.md').read_text().splitlines() if x.startswith('| `terrain.overlays`'))
checks=[('legacy versus new ledger marker documented', all(x in service for x in ('absent provenance field', '`EstablishedInNewWorld: false`', 'legacy ledger'))),
('presence rule and limit documented',all(x in service for x in ('25%', '75%', 'leave padding', 'naturally matching')) and '25%' in limits and '25%' in caps),
('measurement results documented',all(x in service for x in ('TerrainResult.PresenceShare', 'TerrainPlanInfo.PresenceShare', 'InstalledWithoutProof'))),
('adoption decisions documented', all(x in service for x in ('NeedsAdoption','InstallAnyway','TerrainInstallChoice.Adopt'))),
('shared budget documented',all(x in service for x in ('4,096','32,768','2 ms')) and 'No shared write budget' not in service),
('lifecycle dependency documented', 'world.lifecycle' in service and 'world.lifecycle' in caps),
('limits match adoption and budget', 'NeedsAdoption' in limits and '4,096' in limits and 'no shared write budget' not in limits.lower()),
('old cloud auto-write rule removed','Moving a world without Kiln data applies overlays again' not in service and 'holds `terrain.overlays` on clients' not in service)]
for name,ok in checks: print(('PASS ' if ok else 'FAIL ')+name)
print(f'doc contract: {sum(ok for _,ok in checks)} passed, {sum(not ok for _,ok in checks)} failed')
raise SystemExit(0 if all(ok for _,ok in checks) else 1)
PY
