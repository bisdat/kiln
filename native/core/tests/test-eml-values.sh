#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Run through tools/slot. Header/consumer probes; no GPL provider or game is loaded.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
out=${KILN_OUT:?set KILN_OUT to the assigned scratch build directory}/eml-values
mkdir -p "$out"
cc=${CC:-clang}
cxx=${CXX:-clang++}
inc=(-I"$here/../abi")
"$cc" -std=c11 -Wall -Wextra -Werror "${inc[@]}" "$here/abi_headers/eml_values.c" -o "$out/c-probe"
"$out/c-probe"
"$cxx" -x c++ -std=c++20 -Wall -Wextra -Werror "${inc[@]}" "$here/abi_headers/eml_values.c" -o "$out/cpp-probe"
"$out/cpp-probe"
# The truncated-reader probe reads only within each declared extent: ASan/UBSan prove it.
"$cc" -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
    "${inc[@]}" "$here/abi_headers/eml_values.c" -o "$out/asan-probe"
"$out/asan-probe"
python3 -I "$here/abi_headers/eml_values_identity.py"
python3 -I "$here/abi_headers/eml_archive_identity.py"
# Windows x64 layouts, both include orders: values alone pulls the canonical archive header,
# and archive-first proves the values header redeclares nothing. The probe's offset, size
# and enum asserts also compile for the Windows x64 target (layout part only, no C library).
for lang in c c++; do
    standard=c11; [[ $lang == c++ ]] && standard=c++20
    "$cc" --target=x86_64-pc-windows-msvc -x "$lang" -std="$standard" -ffreestanding -Wall -Wextra -Werror \
        -DPROBE_LAYOUT_ONLY "${inc[@]}" -fsyntax-only "$here/abi_headers/eml_values.c"
    printf '#include "services/kiln_eml_values.h"\n' | "$cc" --target=x86_64-pc-windows-msvc \
        -x "$lang" -std="$standard" -ffreestanding -Wall -Wextra -Werror "${inc[@]}" -fsyntax-only -
    printf '#include "services/kiln_eml_archive.h"\n#include "services/kiln_eml_values.h"\n' | \
        "$cc" --target=x86_64-pc-windows-msvc -x "$lang" -std="$standard" -ffreestanding \
        -Wall -Wextra -Werror "${inc[@]}" -fsyntax-only -
done
printf 'eml values: C11/C++20 host probes, ASan/UBSan probe and 6 Windows x64 compile-only layout checks passed\n'
