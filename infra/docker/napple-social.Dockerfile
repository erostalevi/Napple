# syntax=docker/dockerfile:1.7
# napple-social (friends, presence, search, stats and the WebSocket stream, /social/v1) as a small non-root Linux image.
# Build context: the repository root (only Cargo.toml, Cargo.lock and crates/ are sent, see napple-social.Dockerfile.dockerignore).
#   docker buildx build -f infra/docker/napple-social.Dockerfile --platform linux/arm64,linux/amd64 -t napple-social .
# Each platform compiles natively (QEMU under buildx for the non-native one); the deploy workflow builds arm64 on an arm64 runner.
ARG RUST_IMAGE=rust:1.99-slim-bookworm
ARG RUNTIME_IMAGE=debian:bookworm-slim

FROM ${RUST_IMAGE} AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock rustfmt.toml ./
COPY crates ./crates
RUN --mount=type=cache,target=/usr/local/cargo/registry,sharing=locked \
    --mount=type=cache,target=/src/target,sharing=locked \
    cargo build --release --locked -p napple-social --bin napple-social \
 && install -D -m 0755 target/release/napple-social /out/napple-social

FROM ${RUNTIME_IMAGE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /var/lib/napple --shell /usr/sbin/nologin napple \
 && install -d -o napple -g napple /var/lib/napple/social
COPY --from=build /out/napple-social /usr/local/bin/napple-social
USER 10001:10001
ENV NAPPLE_SOCIAL_BIND=0.0.0.0:8083 \
    NAPPLE_SOCIAL_DATA_DIR=/var/lib/napple/social
# 8083: HTTP (behind Caddy).
EXPOSE 8083/tcp
VOLUME ["/var/lib/napple/social"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8083/social/v1/healthz >/dev/null || exit 1
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/napple-social"]
CMD ["serve"]
