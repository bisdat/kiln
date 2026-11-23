kiln_service(camera camera.cpp state.cpp mode.cpp seh.cpp)
add_library(camera-fixture SHARED "${CMAKE_CURRENT_SOURCE_DIR}/../../../mods/tests/camera-test/fixture.cpp" mode.cpp state.cpp)
target_include_directories(camera-fixture PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_CURRENT_SOURCE_DIR}")
add_dependencies(kiln-camera camera-fixture)
