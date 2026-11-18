# One image for every backend node (auth, course, enrollment, grade, admin, dbmonitor) and the
# one-off setup and race-test containers. docker-compose.yml starts each node from it with its own
# command, so every node is its own container with its own process, port and connection pool.
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONPATH=/app:/app/generated

WORKDIR /app

COPY services/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt grpcio-tools==1.71.0

# Python code for the gRPC contracts, generated from proto/*.proto
COPY proto/ /app/proto/
RUN mkdir -p /app/generated \
 && python -m grpc_tools.protoc -I /app/proto \
        --python_out=/app/generated --grpc_python_out=/app/generated --pyi_out=/app/generated \
        /app/proto/*.proto \
 && pip uninstall -y grpcio-tools

COPY services/ /app/

RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin app
USER app
