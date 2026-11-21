# SPDX-License-Identifier: MIT
# kiln.engine: exact-build engine profiles (sigdb/engine/, generated profiles.g.h) and Kiln-internal engine primitives.
kiln_service(engine engine.cpp identity.cpp facts_win.cpp resolve.cpp memory_win.cpp gate.cpp resource_guard.cpp invoke.cpp
                    invoke_win.cpp)
target_include_directories(kiln-engine PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Stand-in Windows host for headless Wine (tests/wine.sh), built explicitly; never a game target or bundle file.
add_executable(engine-host EXCLUDE_FROM_ALL tests/windows_host.cpp)
target_include_directories(engine-host PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_CURRENT_SOURCE_DIR}")
# Test-only variant for headless Wine (tests/wine.sh "gate" mode): the host names the identity and the fake engine image
# (tests/test_facts_win.cpp), so the real SEH copy, Interlocked words, tracker call and gate run against a fake engine.
# Never shipped: built explicitly into bin/tests/, outside bin/services/ (tools/package/stage.sh stages that folder).
# KILN_ENGINE_TEST_RECIPES (this target only, never kiln-engine): the host sets engine-call recipes over helper code in
# its fake image (tests/test_facts_win.cpp kiln_engine_test_set_recipes), since ported profiles carry none.
add_library(kiln-engine-test SHARED EXCLUDE_FROM_ALL engine.cpp identity.cpp tests/test_facts_win.cpp resolve.cpp
                                                     memory_win.cpp gate.cpp resource_guard.cpp invoke.cpp invoke_win.cpp
                                                     "${CMAKE_SOURCE_DIR}/cmake/kiln_build_stamp.cpp")
target_compile_definitions(kiln-engine-test PRIVATE KILN_ENGINE_TEST_RECIPES)
target_include_directories(kiln-engine-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party"
                                                    "${KILN_GENERATED_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}"
                                                    "${CMAKE_CURRENT_SOURCE_DIR}/include")
set_target_properties(kiln-engine-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
add_dependencies(kiln-engine-test kiln-build-stamp)
# Release-build check under headless Wine (tests/wine.sh): the core's real service lookup/registration and build-stamp
# blocks of native/core/src/api.cpp (between their markers, extracted here at configure time) against this build's DLLs.
set(KILN_ENGINE_API_CPP "${CMAKE_SOURCE_DIR}/core/src/api.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KILN_ENGINE_API_CPP}")
file(READ "${KILN_ENGINE_API_CPP}" KILN_ENGINE_API_TEXT)
foreach(block lookup stamp)
    string(FIND "${KILN_ENGINE_API_TEXT}" "// ---- service ${block} (begin)" begin)
    string(FIND "${KILN_ENGINE_API_TEXT}" "// ---- service ${block} (end)" end)
    if(begin EQUAL -1 OR end EQUAL -1)
        message(FATAL_ERROR "native/core/src/api.cpp has no 'service ${block}' markers")
    endif()
    math(EXPR length "${end} - ${begin}")
    string(SUBSTRING "${KILN_ENGINE_API_TEXT}" ${begin} ${length} body)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/core_blocks/service_${block}.inc" "${body}")
endforeach()
add_executable(engine-core-lookup EXCLUDE_FROM_ALL tests/core_lookup_win.cpp)
target_include_directories(engine-core-lookup PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${KILN_GENERATED_DIR}"
                                                      "${CMAKE_CURRENT_BINARY_DIR}/core_blocks" "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(engine-core-lookup kiln-build-stamp)
