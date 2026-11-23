kiln_service(build build.cpp voxel.cpp shapes.cpp props.cpp policy.cpp regions.cpp transient.cpp transient_journal.cpp debug.cpp seh.cpp
              voxel_tasks.cpp voxel_tasks_hold.cpp block_ops.cpp block_runtime.cpp block_engine_win.cpp)
# The task-thread voxel members use kiln.internal.engine's write gate (Kiln-internal, same tree).
target_include_directories(kiln-build PRIVATE "${CMAKE_SOURCE_DIR}/services/engine/include")
# Windows test of the task-thread voxel grid's fault paths (native on a Windows runner, or headless Wine), built explicitly
# into bin/tests/; never a game target or a bundle file: the grid over kiln.engine's real Windows primitives (the SEH
# copy, the views, the write gate) and a fake engine and voxel world in VirtualAlloc'd memory.
set(KILN_ENGINE_DIR "${CMAKE_SOURCE_DIR}/services/engine")
add_executable(build-voxel-fault EXCLUDE_FROM_ALL tests/voxel_tasks_fault_win.cpp voxel_tasks.cpp voxel_tasks_hold.cpp
                                                  "${KILN_ENGINE_DIR}/resolve.cpp" "${KILN_ENGINE_DIR}/gate.cpp"
                                                  "${KILN_ENGINE_DIR}/memory_win.cpp")
target_compile_definitions(build-voxel-fault PRIVATE KILN_BUILD_HOLD_TEST_POINTS)
target_include_directories(build-voxel-fault PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${KILN_ENGINE_DIR}" "${KILN_ENGINE_DIR}/include"
                                                     "${KILN_ENGINE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(build-voxel-fault PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
# Offline block policy and Windows engine-boundary tests. No game or world boot.
add_executable(build-block-ops EXCLUDE_FROM_ALL tests/block_ops_test.cpp block_ops.cpp)
add_executable(build-block-engine EXCLUDE_FROM_ALL tests/block_engine_win_test.cpp block_engine_win.cpp)
add_executable(build-block-policy EXCLUDE_FROM_ALL tests/block_policy_test.cpp policy.cpp)
add_executable(build-block-writer EXCLUDE_FROM_ALL tests/block_writer_win_test.cpp voxel.cpp shapes.cpp policy.cpp
                                                 block_runtime.cpp block_ops.cpp block_engine_win.cpp)
target_include_directories(build-block-writer PRIVATE "${KILN_ENGINE_DIR}/include")
foreach(test build-block-ops build-block-engine build-block-policy build-block-writer)
    target_include_directories(${test} PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
    set_target_properties(${test} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
endforeach()
# Native Windows test of the save-region hook's guarded paths: regions.cpp compiled alone over VirtualAlloc'd lists that
# fault. No game or world boot.
add_executable(build-regions-win EXCLUDE_FROM_ALL tests/regions_win_test.cpp)
set_target_properties(build-regions-win PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
