# SPDX-License-Identifier: MIT
# kiln-dllmod.dll: runs mods made for AndyPimms' Enshrouded ModLoader (packages of a DLL and a <file>.dll.mod manifest,
# ABI 1) on Kiln, without that loader's dinput8.dll. Kiln's own code (no code of that loader is used). Deployed to
# <game>/kiln/plugins/. Docs: docs/kiln/engines/dllmod.md.
kiln_plugin(dllmod src/plugin.cpp src/session.cpp)

# Host tests (tests/run_host.sh, under plain Wine): the host with test packages built from tests/test_mod.cpp, all in
# bin/dllmod-tests/. The Linux part (src/package.h) is tests/run.sh.
set(DLLMOD_TESTS "${CMAKE_BINARY_DIR}/bin/dllmod-tests")
function(dllmod_test_mod name)
    add_library(dllmod-test-${name} SHARED tests/test_mod.cpp)
    target_compile_definitions(dllmod-test-${name} PRIVATE ${ARGN})
    set_target_properties(dllmod-test-${name} PROPERTIES OUTPUT_NAME dllmod_test_${name}
                          RUNTIME_OUTPUT_DIRECTORY "${DLLMOD_TESTS}" ARCHIVE_OUTPUT_DIRECTORY "${DLLMOD_TESTS}/lib")
endfunction()
dllmod_test_mod(settings)
dllmod_test_mod(plain TM_PLAIN)
dllmod_test_mod(fail TM_FAIL)
dllmod_test_mod(fault TM_FAULT)
dllmod_test_mod(lib TM_LIB)
dllmod_test_mod(uses_lib TM_USES_LIB)
target_link_libraries(dllmod-test-uses_lib PRIVATE dllmod-test-lib)

add_executable(dllmod-host-test tests/host_test.cpp src/session.cpp)
target_include_directories(dllmod-host-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(dllmod-host-test PROPERTIES OUTPUT_NAME dllmod_host_test RUNTIME_OUTPUT_DIRECTORY "${DLLMOD_TESTS}")
add_dependencies(dllmod-host-test dllmod-test-settings dllmod-test-plain dllmod-test-fail dllmod-test-fault dllmod-test-lib
                 dllmod-test-uses_lib)
