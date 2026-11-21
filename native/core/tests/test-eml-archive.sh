#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through tools/slot. Header/consumer probes; no GPL provider or game is loaded.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT to the assigned scratch build directory}/eml-archive
mkdir -p "$out"
cc=${CC:-clang}
cxx=${CXX:-clang++}
inc=(-I"$here/../abi")
"$cc" -std=c11 -Wall -Wextra -Werror "${inc[@]}" "$here/abi_headers/eml_archive.c" -o "$out/c-probe"
"$out/c-probe"
"$cxx" -x c++ -std=c++20 -Wall -Wextra -Werror "${inc[@]}" "$here/abi_headers/eml_archive.c" -o "$out/cpp-probe"
"$out/cpp-probe"
python3 -I "$here/abi_headers/eml_archive_identity.py"
# Reference-provider protocol probes only, not the GPL IO provider. Include a
# pinned error copy overlapped with release/view retirement in BOTH builds.
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    "${inc[@]}" "$here/eml_archive_error_test.cpp" -o "$out/error-probe"
"$out/error-probe"
"$cxx" -std=c++20 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=thread \
    "${inc[@]}" "$here/eml_archive_error_test.cpp" -o "$out/error-tsan-probe"
TSAN_OPTIONS=halt_on_error=1:exitcode=66 "$out/error-tsan-probe"
for lang in c c++; do
    standard=c11; [[ $lang == c++ ]] && standard=c++20
    printf '#include "services/kiln_eml_archive.h"\n' | "$cc" --target=x86_64-pc-windows-msvc \
        -x "$lang" -std="$standard" -ffreestanding -Wall -Wextra -Werror "${inc[@]}" -fsyntax-only -
done
printf 'eml archive: C11/C++20 host probes and 2 Windows x64 compile-only layout checks passed\n'
