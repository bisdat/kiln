#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# The stop's facts (src/stop_facts.h) under the thread sanitizer: native/core/tests/test-stop-facts.sh
# (test-stop-facts.sh old: the round-2 pattern, which the sanitizer must flag).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:-$here/../../build}/core-tests
mkdir -p "$out"
"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread -I"$here/../src" "$here/stop_facts_test.cpp" -o "$out/stop_facts_test"
TSAN_OPTIONS="halt_on_error=1 exitcode=66" "$out/stop_facts_test" "$@"
