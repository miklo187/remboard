cmake_minimum_required(VERSION 3.22)
project(remboard_jni)

if(NOT DEFINED REMBOARD_ROOT)
  message(FATAL_ERROR "REMBOARD_ROOT not set (should come from app/build.gradle.kts)")
endif()

add_subdirectory(${REMBOARD_ROOT}/core ${CMAKE_CURRENT_BINARY_DIR}/core_build)

add_library(remboard_jni SHARED jni_bridge.cpp)
target_link_libraries(remboard_jni PRIVATE remboard_core log)
