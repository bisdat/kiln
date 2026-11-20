# SPDX-License-Identifier: MIT
# hooks-typed-test's native part: kiln-hooks-typed-test.dll, a lab-only plugin (mods/tests/hooks-typed-test/native/build.sh):
# typed links (kiln.hooks detour_typed) on RegisterNative and SetTimeScale from boot.
# Windows x64 with clang-cl + the xwin SDK, the toolchain native/build.sh uses; headers from native/core/abi only.
cmake_minimum_required(VERSION 3.20)
project(kiln-hooks-typed-test LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(KILN_NATIVE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../native")
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
# /EHa as in native/CMakeLists.txt
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/EHa>" /Zi)
add_link_options(/DEBUG /OPT:REF /IGNORE:4099)
add_library(kiln-hooks-typed-test SHARED hooks_typed_test_plugin.cpp)
target_include_directories(kiln-hooks-typed-test PRIVATE "${KILN_NATIVE}/core/abi")
set_target_properties(kiln-hooks-typed-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
