# Generates the protobuf and gRPC C++ sources from proto/media_upload.proto.
# Sets PROTO_SOURCES and PROTO_INCLUDE_DIR in the caller's scope.
function(generate_media_upload_proto)
    find_program(PROTOC_EXECUTABLE protoc REQUIRED)
    find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

    get_filename_component(PROTO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../proto" ABSOLUTE)
    set(PROTO_FILE "${PROTO_DIR}/media_upload.proto")
    set(OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${OUT_DIR}")

    set(GENERATED
        "${OUT_DIR}/media_upload.pb.cc"
        "${OUT_DIR}/media_upload.pb.h"
        "${OUT_DIR}/media_upload.grpc.pb.cc"
        "${OUT_DIR}/media_upload.grpc.pb.h")

    add_custom_command(
        OUTPUT ${GENERATED}
        COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${PROTO_DIR}
                --cpp_out=${OUT_DIR}
                --grpc_out=${OUT_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
                ${PROTO_FILE}
        DEPENDS ${PROTO_FILE})

    set(PROTO_SOURCES ${GENERATED} PARENT_SCOPE)
    set(PROTO_INCLUDE_DIR "${OUT_DIR}" PARENT_SCOPE)
endfunction()
