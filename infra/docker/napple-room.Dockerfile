# syntax=docker/dockerfile:1.7
# napple-room (LDN / LAN Play relay rooms) as a small non-root Linux image.
# Build context: the repository root (only Cargo.toml, Cargo.lock and crates/ are sent, see napple-room.Dockerfile.dockerignore).
#   docker buildx build -f infra/docker/napple-room.Dockerfile --platform linux/arm64,linux/amd64 -t napple-room .
# Each platform compiles natively (QEMU under buildx for the non-native one); the deploy workflow builds arm64 on an arm64 runner.
ARG RUST_IMAGE=rust:1.99-slim-bookworm
ARG RUNTIME_IMAGE=debian:bookworm-slim

FROM ${RUST_IMAGE} AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock rustfmt.toml ./
COPY crates ./crates
RUN --mount=type=cache,target=/usr/local/cargo/registry,sharing=locked \
    --mount=type=cache,target=/src/target,sharing=locked \
    cargo build --release --locked -p napple-room --bin napple-room \
 && install -D -m 0755 target/release/napple-room /out/napple-room

FROM ${RUNTIME_IMAGE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /var/lib/napple --shell /usr/sbin/nologin napple \
 && install -d -o napple -g napple /var/lib/napple/room
COPY --from=build /out/napple-room /usr/local/bin/napple-room
USER 10001:10001
ENV NAPPLE_ROOM_LISTEN=0.0.0.0:31415 \
    NAPPLE_ROOM_HTTP=0.0.0.0:8081 \
    NAPPLE_ROOM_DATA=/var/lib/napple/room
EXPOSE 31415/tcp 31415/udp 8081/tcp
VOLUME ["/var/lib/napple/room"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8081/healthz >/dev/null || exit 1
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/napple-room"]
