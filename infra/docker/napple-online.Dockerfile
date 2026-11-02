# syntax=docker/dockerfile:1.7
# napple-online (accounts/tokens HTTP API + NEX/PRUDP game server) as a small non-root Linux image.
# Build context: the repository root (only Cargo.toml, Cargo.lock and crates/ are sent, see napple-online.Dockerfile.dockerignore).
#   docker buildx build -f infra/docker/napple-online.Dockerfile --platform linux/arm64,linux/amd64 -t napple-online .
# Each platform compiles natively (QEMU under buildx for the non-native one); the deploy workflow builds arm64 on an arm64 runner.
ARG RUST_IMAGE=rust:1.99-slim-bookworm
ARG RUNTIME_IMAGE=debian:bookworm-slim

FROM ${RUST_IMAGE} AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock rustfmt.toml ./
COPY crates ./crates
RUN --mount=type=cache,target=/usr/local/cargo/registry,sharing=locked \
    --mount=type=cache,target=/src/target,sharing=locked \
    cargo build --release --locked -p napple-online --bin napple-online \
 && install -D -m 0755 target/release/napple-online /out/napple-online

FROM ${RUNTIME_IMAGE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /var/lib/napple --shell /usr/sbin/nologin napple \
 && install -d -o napple -g napple /var/lib/napple/online
COPY --from=build /out/napple-online /usr/local/bin/napple-online
USER 10001:10001
ENV NAPPLE_ONLINE_HTTP=0.0.0.0:8080 \
    NAPPLE_ONLINE_NEX_AUTH=0.0.0.0:60000 \
    NAPPLE_ONLINE_NEX_SECURE=0.0.0.0:60001 \
    NAPPLE_ONLINE_DATA=/var/lib/napple/online
EXPOSE 8080/tcp 60000/udp 60001/udp
VOLUME ["/var/lib/napple/online"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8080/healthz >/dev/null || exit 1
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/napple-online"]
