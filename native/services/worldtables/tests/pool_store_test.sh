#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
: "${KILN_OUT:?}"
out=$KILN_OUT/worldtables-host-test
mkdir -p "$out"
g++ -std=c++20 -O1 -g -Wall -Wextra -fsanitize=address,undefined -I"$here/.." -I"$here/../../surfaces/cook" -I"$here/../../../third_party" "$here/test_pool_store.cpp" "$here/../merge.cpp" "$here/../terrain.cpp" "$here/../../surfaces/cook/surfacecheck.cpp" -o "$out/test_pool_store"
"$out/test_pool_store"
