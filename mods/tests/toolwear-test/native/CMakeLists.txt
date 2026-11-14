# SPDX-License-Identifier: MIT
# toolwear-test's native part: kiln-toolwear-test.dll, a lab-only plugin (mods/tests/toolwear-test/native/build.sh): a
# native rule owner of kiln.toolwear. Windows x64 with clang-cl + the xwin SDK, the toolchain native/build.sh uses;
# headers from native/core/abi only.
cmake_minimum_required(VERSION 3.20)
project(kiln-toolwear-test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(KILN_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../native")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
add_compile_options(/Zi)
add_link_options(/DEBUG /OPT:REF /IGNORE:4099)
add_library(kiln-toolwear-test SHARED toolwear_native.cpp)
target_include_directories(kiln-toolwear-test PRIVATE "${KILN_NATIVE}/core/abi")
set_target_properties(kiln-toolwear-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
