# SPDX-License-Identifier: MIT
# hooks_v2_test.exe: the kiln.hooks v2 registry (core/src/services/hooks*.cpp) built with a few host stubs and run under
# Wine, no game needed: native/core/tests/test-hooks-v2.sh. Output: bin/hooksv2/hooks_v2_test.exe.
add_executable(hooks_v2_test
    ../../core/tests/hooks_v2_test.cpp
    ../../core/src/services/hooks.cpp
    ../../core/src/services/hooks_protect.cpp
    ../../core/src/services/hooks_chain.cpp
    ../../core/src/services/hooks_chain_asm.cpp
    ../../core/src/services/hooks_fingerprint.cpp
    ../../core/src/services/hooks_seh.cpp)
target_include_directories(hooks_v2_test PRIVATE ../../core/abi ../../core/src ../../third_party "${KILN_GENERATED_DIR}")
target_link_libraries(hooks_v2_test PRIVATE kiln-safetyhook)
# the test sees a thread reach kiln.hooks' code lock (services/hooks_code.h); kiln-core never defines it
target_compile_definitions(hooks_v2_test PRIVATE KILN_HOOKS_TEST_LOCK_PROBE)
set_target_properties(hooks_v2_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/hooksv2")

# hooks-v2-module.dll: "another module" (a mod's or a loader's DLL) that hooks_v2_test opens and hooks inside
# (open_module, test_foreign_module). Not deployed.
add_library(hooks_v2_module SHARED ../../core/tests/hooks_module_dll.cpp)
set_target_properties(hooks_v2_module PROPERTIES OUTPUT_NAME hooks-v2-module RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/hooksv2")
add_dependencies(hooks_v2_test hooks_v2_module)

# foreign-stub.dll: a stand-in for another engine's hooks in lab tests (see foreign_stub.cpp). Not deployed.
add_library(foreign_stub SHARED foreign_stub.cpp)
target_link_libraries(foreign_stub PRIVATE kiln-safetyhook)
set_target_properties(foreign_stub PROPERTIES OUTPUT_NAME foreign-stub RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/hooksv2")

# hooks_typed_test.exe: the typed link kind of kiln.hooks (dev.28), the same way: native/core/tests/test-hooks-typed.sh.
add_executable(hooks_typed_test
    ../../core/tests/hooks_typed_test.cpp
    ../../core/src/services/hooks.cpp
    ../../core/src/services/hooks_chain.cpp
    ../../core/src/services/hooks_chain_asm.cpp
    ../../core/src/services/hooks_fingerprint.cpp
    ../../core/src/services/hooks_seh.cpp)
target_include_directories(hooks_typed_test PRIVATE ../../core/abi ../../core/src ../../third_party "${KILN_GENERATED_DIR}")
target_link_libraries(hooks_typed_test PRIVATE kiln-safetyhook)
set_target_properties(hooks_typed_test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/hooksv2")
