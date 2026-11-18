# The view node: renders the web pages and calls the other nodes over gRPC for everything else.
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONPATH=/app:/app/generated

WORKDIR /app

COPY view/requirements.txt /tmp/requirements.txt
RUN pip install --no-cache-dir -r /tmp/requirements.txt grpcio-tools==1.71.0

COPY proto/ /app/proto/
RUN mkdir -p /app/generated \
 && python -m grpc_tools.protoc -I /app/proto \
        --python_out=/app/generated --grpc_python_out=/app/generated --pyi_out=/app/generated \
        /app/proto/*.proto \
 && pip uninstall -y grpcio-tools

COPY view/ /app/

RUN useradd --uid 10001 --no-create-home --shell /usr/sbin/nologin app
USER app

EXPOSE 8080
# One process, so the random key that signs flash messages is the same for every request
# (see app.py); threads serve requests side by side.
CMD ["gunicorn", "--workers", "1", "--threads", "16", "--bind", "0.0.0.0:8080", \
     "--access-logfile", "-", "--timeout", "60", "app:app"]
