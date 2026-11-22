# kiln.assets: mod-supplied resources and overrides. Links bcrypt (SHA-256 content hashes); embeds stb_image and
# a BCn encoder subset of bc7enc_rdo (native/third_party/{stb,bc7enc}, MIT/Unlicense).
kiln_service(assets
    assets.cpp
    catalog.cpp
    contained.cpp
    cooktrust.cpp
    scan.cpp
    hooks.cpp
    store.cpp
    image.cpp
    loaded.cpp
    loaded_prod.cpp
    util.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7enc.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/rgbcx.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7decomp.cpp)
target_link_libraries(kiln-assets PRIVATE bcrypt)
# The loaded-resource reads use kiln.internal.engine's records (Kiln-internal, same tree).
target_include_directories(kiln-assets PRIVATE "${CMAKE_SOURCE_DIR}/services/engine/include")
# The loaded-resource reads' host test (tests/test_loaded.cpp) as a Windows program, built explicitly into bin/tests/;
# never a game target or a bundle file.
add_executable(assets-loaded-test EXCLUDE_FROM_ALL tests/test_loaded.cpp loaded.cpp)
target_include_directories(assets-loaded-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/services/engine/include"
                                                      "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(assets-loaded-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/tests")
set_source_files_properties(
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7enc.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/rgbcx.cpp
    ${CMAKE_SOURCE_DIR}/third_party/bc7enc/bc7decomp.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
