kiln_service(world world.cpp programs.cpp boot_programs.cpp writes.cpp entities.cpp teleport.cpp selftest.cpp seh.cpp
              game_programs.cpp wraps.cpp layout.cpp program_budget.cpp foreign.cpp ecs_reads.cpp ecs_engine.cpp ecs_lookup.cpp ecs_writes.cpp ecs_write_hold.cpp
              ecs_write_policy.cpp ecs_prod.cpp ecs_check.cpp ecs_write_check.cpp named_services.cpp
              named_service_hold.cpp named_prod.cpp region_win.cpp)
# The engine backend of the ECS reads uses kiln.internal.engine's records (Kiln-internal, same tree).
target_include_directories(kiln-world PRIVATE "${CMAKE_SOURCE_DIR}/services/engine/include")
# Windows test of the reads' fault paths under headless Wine (tests/wine.sh), built explicitly into bin/tests/; never a
# game target or a bundle file. The engine backend and logic over kiln.engine's real Windows primitives (the SEH copy,
# the views) and a fake engine in VirtualAlloc'd memory.
set(KILN_ENGINE_DIR "${CMAKE_SOURCE_DIR}/services/engine")
add_executable(world-ecs-fault EXCLUDE_FROM_ALL tests/ecs_fault_win.cpp ecs_engine.cpp ecs_lookup.cpp ecs_reads.cpp
                                                ecs_writes.cpp ecs_write_hold.cpp ecs_write_policy.cpp
                                                "${KILN_ENGINE_DIR}/resolve.cpp" "${KILN_ENGINE_DIR}/gate.cpp"
                                                "${KILN_ENGINE_DIR}/memory_win.cpp")
target_compile_definitions(world-ecs-fault PRIVATE KILN_WORLD_HOLD_TEST_POINTS)
target_include_directories(world-ecs-fault PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${KILN_ENGINE_DIR}" "${KILN_ENGINE_DIR}/include"
                                                   "${KILN_ENGINE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(world-ecs-fault PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
# Windows test of the named services over the same real primitives and the real VirtualQuery range check (natively on a
# Windows runner, or under headless Wine), built explicitly into bin/tests/; never a game target or a bundle file.
add_executable(world-named-win EXCLUDE_FROM_ALL tests/named_services_win.cpp named_services.cpp named_service_hold.cpp
                                                region_win.cpp "${KILN_ENGINE_DIR}/resolve.cpp" "${KILN_ENGINE_DIR}/gate.cpp"
                                                "${KILN_ENGINE_DIR}/memory_win.cpp")
target_compile_definitions(world-named-win PRIVATE KILN_WORLD_HOLD_TEST_POINTS)
target_include_directories(world-named-win PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${KILN_ENGINE_DIR}" "${KILN_ENGINE_DIR}/include"
                                                   "${KILN_ENGINE_DIR}/tests" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(world-named-win PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
