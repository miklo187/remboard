# Shared C++23 core: protobuf messages, ZeroMQ/CURVE transport, pairing,
# device registry, file chunking. Consumed both by the root (Linux) build
# and by app-android's externalNativeBuild -- must be includable standalone,
# so it pulls in proto/ itself if not already present.

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
include(FetchContent)

option(REMBOARD_VENDOR_DEPS "Fetch+build zmq/sodium/protobuf instead of using system packages" ${ANDROID})

if(REMBOARD_VENDOR_DEPS)
  # libsodium ships no CMake build system; it's cross-compiled once (per
  # ABI) via its own officially-supported dist-build scripts (autotools
  # under the hood), and the result is fed to libzmq's Findsodium.cmake by
  # pre-populating the cache variables it looks for.
  FetchContent_Declare(
    libsodium_src
    GIT_REPOSITORY https://github.com/jedisct1/libsodium
    GIT_TAG 1.0.22)
  FetchContent_Populate(libsodium_src)

  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(_sodium_script "android-armv8-a.sh")
    set(_sodium_prefix_suffix "armv8-a+crypto")
  elseif(ANDROID_ABI STREQUAL "x86_64")
    set(_sodium_script "android-x86_64.sh")
    set(_sodium_prefix_suffix "westmere")
  else()
    message(FATAL_ERROR "Unsupported ANDROID_ABI for vendored libsodium: ${ANDROID_ABI}")
  endif()

  set(_sodium_prefix "${libsodium_src_SOURCE_DIR}/libsodium-android-${_sodium_prefix_suffix}")

  if(NOT EXISTS "${_sodium_prefix}/lib/libsodium.a")
    message(STATUS "Building libsodium for Android ${ANDROID_ABI} (cached afterward)...")
    # CMake's own NDK toolchain setup leaves CC/CXX/CFLAGS etc. pointing at
    # the plain (untargeted) clang in this process's environment;
    # execute_process would otherwise leak that in, silently defeating
    # android-build.sh's own `${CC:-<target-specific default>}` logic.
    execute_process(
      COMMAND ${CMAKE_COMMAND} -E env
              --unset=CC --unset=CXX --unset=CPP
              --unset=CFLAGS --unset=CXXFLAGS --unset=CPPFLAGS --unset=LDFLAGS
              "ANDROID_NDK_HOME=${ANDROID_NDK}"
              "NDK_PLATFORM=${ANDROID_PLATFORM}"
              "LIBSODIUM_FULL_BUILD=1"
              sh "${libsodium_src_SOURCE_DIR}/dist-build/${_sodium_script}"
      WORKING_DIRECTORY "${libsodium_src_SOURCE_DIR}"
      RESULT_VARIABLE _sodium_build_result
      OUTPUT_VARIABLE _sodium_build_output
      ERROR_VARIABLE _sodium_build_error)
    if(NOT _sodium_build_result EQUAL 0)
      message(FATAL_ERROR
        "libsodium Android build failed (${_sodium_build_result}):\n"
        "${_sodium_build_output}\n${_sodium_build_error}")
    endif()
  endif()

  set(SODIUM_INCLUDE_DIRS "${_sodium_prefix}/include" CACHE PATH "" FORCE)
  set(SODIUM_LIBRARIES "${_sodium_prefix}/lib/libsodium.a" CACHE FILEPATH "" FORCE)
  set(SODIUM_FOUND TRUE CACHE BOOL "" FORCE)

  add_library(sodium_vendored STATIC IMPORTED GLOBAL)
  set_target_properties(sodium_vendored PROPERTIES
    IMPORTED_LOCATION "${SODIUM_LIBRARIES}"
    INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIRS}")

  # libzmq has a native CMake build; point it at the sodium build above
  # (its Findsodium.cmake sees SODIUM_* already cached and skips its own
  # search) instead of vendoring yet another crypto backend.
  set(WITH_LIBSODIUM ON CACHE BOOL "" FORCE)
  set(WITH_LIBSODIUM_STATIC ON CACHE BOOL "" FORCE)
  set(ENABLE_CURVE ON CACHE BOOL "" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED OFF CACHE BOOL "" FORCE)
  set(BUILD_STATIC ON CACHE BOOL "" FORCE)
  set(WITH_PERF_TOOL OFF CACHE BOOL "" FORCE)
  # pkg-config isn't cross-compile-aware here, so WITH_LIBBSD's default ON
  # incorrectly picks up the *host's* libbsd via pkg_check_modules; Android's
  # bionic libc already provides strlcpy, so this isn't needed at all.
  set(WITH_LIBBSD OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    libzmq
    GIT_REPOSITORY https://github.com/zeromq/libzmq
    GIT_TAG v4.3.5)
  FetchContent_MakeAvailable(libzmq)

  # protobuf's own CMake build (protoc itself is never invoked here --
  # proto/CMakeLists.txt always locates and runs the *host* protoc; this is
  # just the runtime library the generated .pb.cc files link against, so it
  # must match the target ABI). Transitively FetchContents abseil-cpp. Must
  # run before add_subdirectory(proto) below so protobuf::libprotobuf
  # already exists when proto/CMakeLists.txt checks for it.
  set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_PROTOC_BINARIES OFF CACHE BOOL "" FORCE)
  set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
  set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)

  # Must match the host protoc's version exactly (protobuf enforces an
  # exact generator/runtime match for C++ at compile time, not just
  # ABI-compatible-or-newer) -- `protoc --version` on this machine reports
  # "libprotoc 36.0".
  FetchContent_Declare(
    protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf
    GIT_TAG v36.0)
  FetchContent_MakeAvailable(protobuf)
else()
  pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq)
  pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium)
endif()

if(NOT TARGET remboard_proto)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../proto ${CMAKE_CURRENT_BINARY_DIR}/proto_build)
endif()

if(NOT ANDROID)
  pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
endif()

FetchContent_Declare(
  cppzmq
  GIT_REPOSITORY https://github.com/zeromq/cppzmq
  GIT_TAG v4.10.0)
set(CPPZMQ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cppzmq)

FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json
  GIT_TAG v3.11.3)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(nlohmann_json)

add_library(remboard_core STATIC
  src/core.cpp
  src/types.cpp
  src/identity/identity.cpp
  src/store/device_store.cpp
  src/transfer/chunker.cpp
  src/transport/zmq_transport.cpp
  src/transport/zap_handler.cpp
)

target_include_directories(remboard_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(remboard_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(remboard_core
  PUBLIC
    remboard_proto
    nlohmann_json::nlohmann_json
  PRIVATE
    Threads::Threads
)

if(REMBOARD_VENDOR_DEPS)
  # cppzmq's plain "cppzmq" target links against a "libzmq" (shared)
  # target; we only build libzmq-static (BUILD_SHARED=OFF above), so that
  # target never exists and CMake silently passes "libzmq" through to the
  # linker as a bare -llibzmq flag. cppzmq's "cppzmq-static" target links
  # against libzmq-static instead, matching what we actually built.
  target_link_libraries(remboard_core PRIVATE cppzmq-static libzmq-static sodium_vendored)
else()
  target_link_libraries(remboard_core PRIVATE cppzmq PkgConfig::ZMQ PkgConfig::SODIUM)
endif()

if(NOT ANDROID)
  target_sources(remboard_core PRIVATE src/discovery/discovery_avahi.cpp)
  target_link_libraries(remboard_core PRIVATE PkgConfig::AVAHI)
else()
  target_sources(remboard_core PRIVATE src/discovery/discovery_jni_shim.cpp)
endif()

target_compile_features(remboard_core PUBLIC cxx_std_23)

if(NOT ANDROID)
  add_subdirectory(test)
endif()
