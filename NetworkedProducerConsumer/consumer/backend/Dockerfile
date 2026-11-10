FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake make pkg-config \
      libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
      libssl-dev nlohmann-json3-dev libopencv-dev \
      libavformat-dev libavcodec-dev libavutil-dev \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu's cpp-httplib 0.14 sends a malformed Content-Range for "Range: bytes=0-",
# which is what browsers ask for when loading a video, so they refuse to play it.
ADD --checksum=sha256:7d3f813d05dad23b9c66eb6d9c1ff3d66f6acfaf6e5900ce960f7990e78079ea \
    https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.20.1/httplib.h /usr/local/include/httplib.h

WORKDIR /src
COPY proto/ proto/
COPY common/ common/
COPY consumer/backend/ consumer/backend/

RUN cmake -S consumer/backend -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build -j"$(nproc)" \
 && install /build/consumer /usr/local/bin/consumer

COPY consumer/frontend/ /app/frontend/

CMD ["consumer"]
