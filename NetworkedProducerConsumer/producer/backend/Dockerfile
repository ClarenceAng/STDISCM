FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      g++ cmake make pkg-config \
      libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc \
      libssl-dev \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY proto/ proto/
COPY common/ common/
COPY producer/backend/ producer/backend/

RUN cmake -S producer/backend -B /build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build /build -j"$(nproc)" \
 && install /build/producer /usr/local/bin/producer

CMD ["producer"]
