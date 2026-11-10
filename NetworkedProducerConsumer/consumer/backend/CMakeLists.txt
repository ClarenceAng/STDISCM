cmake_minimum_required(VERSION 3.16)
project(consumer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs videoio)
find_package(nlohmann_json REQUIRED)
pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
pkg_check_modules(PROTOBUF REQUIRED IMPORTED_TARGET protobuf)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil)

include(../../common/GrpcProto.cmake)
generate_media_upload_proto()

add_executable(consumer
    src/main.cpp
    src/av_mux.cpp
    src/upload_service.cpp
    src/video_library.cpp
    src/video_worker.cpp
    src/web_server.cpp
    ${PROTO_SOURCES})

target_include_directories(consumer PRIVATE ../../common ${PROTO_INCLUDE_DIR} ${OpenCV_INCLUDE_DIRS})
target_compile_options(consumer PRIVATE -Wall -Wextra)
target_link_libraries(consumer PRIVATE
    PkgConfig::GRPCPP PkgConfig::PROTOBUF PkgConfig::FFMPEG
    ${OpenCV_LIBS} nlohmann_json::nlohmann_json OpenSSL::Crypto Threads::Threads)
