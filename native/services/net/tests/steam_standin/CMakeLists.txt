# SPDX-License-Identifier: MIT
# test infrastructure only. The stand-in steam_api64.dll (standin.h) and its test, built only when named
# (EXCLUDE_FROM_ALL) and only into bin/tests/steam-standin/, which nothing stages or packages. Run: run.sh here.
add_library(steam-standin SHARED EXCLUDE_FROM_ALL standin.cpp)
set_target_properties(steam-standin PROPERTIES OUTPUT_NAME steam_api64
                      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests/steam-standin"
                      ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/steam-standin-lib"
                      PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests/steam-standin")
target_link_libraries(steam-standin PRIVATE ws2_32)

add_executable(steam-standin-test EXCLUDE_FROM_ALL standin_test.cpp)
set_target_properties(steam-standin-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests/steam-standin")
add_dependencies(steam-standin-test steam-standin)

# kiln.net's Steam peer-to-peer messages (../../steam_messages.cpp) over the stand-in: ../steam_lease_test.cpp, run by
# ../steam_lease_run.sh (Wine) and ../steam_lease_run.ps1 (Windows)
add_executable(steam-lease-test EXCLUDE_FROM_ALL ../steam_lease_test.cpp ../../steam_messages.cpp)
target_include_directories(steam-lease-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi")
set_target_properties(steam-lease-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests/steam-standin")
target_link_libraries(steam-lease-test PRIVATE bcrypt)  # random lease handles (steam_messages.cpp)
add_dependencies(steam-lease-test steam-standin)

