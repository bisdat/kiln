kiln_service(skills skills.cpp tree.cpp shadow.cpp client_view.cpp)

# kiln.skills.client (client_view.cpp) against a fake client world (review 31.13). Run under Wine: tests/run.sh
add_executable(skills-client-view-native-test tests/client_view_native_test.cpp)
target_include_directories(skills-client-view-native-test PRIVATE "${CMAKE_SOURCE_DIR}/core/abi" "${CMAKE_SOURCE_DIR}/third_party")
set_target_properties(skills-client-view-native-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/skills-tests")
