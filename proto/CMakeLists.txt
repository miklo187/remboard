# Generates C++ sources from remboard.proto and exposes them as the
# remboard_proto target.
#
# protoc must always run on the *host*, even when this file is included from
# an Android (NDK) cross-build via core/CMakeLists.txt -- it is a codegen
# tool, not something deployed to the device. We therefore locate the host
# protoc explicitly via find_program() rather than relying on a
# possibly-cross-compiled protobuf::protoc target.
find_program(Protobuf_PROTOC_EXECUTABLE protoc REQUIRED)

# The runtime library (libprotobuf, linked into the generated .pb.cc) is the
# only part that needs to match the target architecture. On the host build
# this comes from the system package; on Android, core/CMakeLists.txt
# arranges for a cross-compiled protobuf::libprotobuf target to exist before
# this file is included.
if(NOT TARGET protobuf::libprotobuf)
  find_package(Protobuf CONFIG REQUIRED)
endif()

set(REMBOARD_PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${REMBOARD_PROTO_GEN_DIR})

set(REMBOARD_PROTO_SRC ${REMBOARD_PROTO_GEN_DIR}/remboard.pb.cc)
set(REMBOARD_PROTO_HDR ${REMBOARD_PROTO_GEN_DIR}/remboard.pb.h)

add_custom_command(
  OUTPUT ${REMBOARD_PROTO_SRC} ${REMBOARD_PROTO_HDR}
  COMMAND ${Protobuf_PROTOC_EXECUTABLE}
          --cpp_out=${REMBOARD_PROTO_GEN_DIR}
          -I ${CMAKE_CURRENT_SOURCE_DIR}
          ${CMAKE_CURRENT_SOURCE_DIR}/remboard.proto
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/remboard.proto
  COMMENT "Generating remboard protobuf sources")

add_library(remboard_proto STATIC ${REMBOARD_PROTO_SRC})
target_include_directories(remboard_proto PUBLIC ${REMBOARD_PROTO_GEN_DIR})
target_link_libraries(remboard_proto PUBLIC protobuf::libprotobuf)
