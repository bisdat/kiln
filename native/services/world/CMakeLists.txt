kiln_service(world world.cpp programs.cpp boot_programs.cpp writes.cpp entities.cpp teleport.cpp selftest.cpp seh.cpp
              game_programs.cpp wraps.cpp layout.cpp program_budget.cpp foreign.cpp ecs_reads.cpp ecs_engine.cpp ecs_prod.cpp ecs_check.cpp)
# The engine backend of the ECS reads uses kiln.internal.engine's records (Kiln-internal, same tree).
target_include_directories(kiln-world PRIVATE "${CMAKE_SOURCE_DIR}/services/engine/include")
