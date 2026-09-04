add_executable(cli_harness cli_harness.cpp)
target_link_libraries(cli_harness PRIVATE remboard_core)
target_include_directories(cli_harness PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)

include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest
  GIT_TAG v1.15.2)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(remboard_core_tests
  chunker_test.cpp
  device_store_test.cpp
  identity_test.cpp
  zap_handler_test.cpp
)
target_link_libraries(remboard_core_tests PRIVATE remboard_core cppzmq nlohmann_json::nlohmann_json GTest::gtest_main)
target_include_directories(remboard_core_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_compile_features(remboard_core_tests PRIVATE cxx_std_23)

include(GoogleTest)
gtest_discover_tests(remboard_core_tests)
