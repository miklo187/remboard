cmake_minimum_required(VERSION 3.19)
project(remboard CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

add_subdirectory(proto)
add_subdirectory(core)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/app-linux/CMakeLists.txt)
  add_subdirectory(app-linux)
endif()
