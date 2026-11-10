cmake_minimum_required(VERSION 3.16)
project(producer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)

include(../../common/GrpcProto.cmake)
generate_media_upload_proto()

add_executable(producer src/main.cpp ${PROTO_SOURCES})
target_include_directories(producer PRIVATE ../../common ${PROTO_INCLUDE_DIR})
target_compile_options(producer PRIVATE -Wall -Wextra)
target_link_libraries(producer PRIVATE
    PkgConfig::GRPCPP PkgConfig::PROTOBUF OpenSSL::Crypto Threads::Threads)
