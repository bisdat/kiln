#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Kiln-internal service tables (kiln.internal.*): the real api.cpp lookup/registration block with host stubs, ASan/UBSan,
# built with the production build identity stamped and without build metadata:
#   native/core/tests/test-service-access.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/service-access-test
mkdir -p "$out"
python3 -I - "$here/../src/api.cpp" "$out" <<'PY'
import pathlib, sys
api = pathlib.Path(sys.argv[1]).read_text()
begin, end = '// ---- service lookup (begin)', '// ---- service lookup (end)'
if begin not in api or end not in api:
    sys.exit('api.cpp has no service lookup markers')
(pathlib.Path(sys.argv[2]) / 'service_access.inc').write_text(api[api.index(begin):api.index(end)])
PY
# The production build identity (KILN_VERSION + KILN_BUILD), stamped and without build metadata.
for variant in stamped unstamped; do
  build='"+20261115.ddb9bd4"'
  extra=()
  if [[ $variant == unstamped ]]; then build='""'; extra=(-DKILN_TEST_UNSTAMPED); fi
  "${CXX:-clang++}" -std=c++20 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
    -fno-sanitize-recover=undefined -DKILN_VERSION='"0.2.0-dev"' -DKILN_BUILD="$build" "${extra[@]}" -I "$out" \
    -I "$here/../abi" "$here/service_access_test.cpp" -o "$out/service_access_test_$variant"
  printf '%s: ' "$variant"
  "$out/service_access_test_$variant"
done
