FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF && \
    cmake --build /build --parallel

FROM ubuntu:24.04
RUN useradd --system --no-create-home --shell /usr/sbin/nologin gateway
COPY --from=build /build/gateway /usr/local/bin/gateway
COPY config/gateway.docker.yaml /etc/gateway/gateway.yaml
USER gateway
EXPOSE 8080 9090
ENTRYPOINT ["/usr/local/bin/gateway", "/etc/gateway/gateway.yaml"]
