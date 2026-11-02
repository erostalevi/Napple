# syntax=docker/dockerfile:1.7
# napple-account (accounts, profiles, photos, cloud-save keys and blobs, /account/v1) as a small non-root Linux image.
# Build context: the repository root (only Cargo.toml, Cargo.lock and crates/ are sent, see napple-account.Dockerfile.dockerignore).
#   docker buildx build -f infra/docker/napple-account.Dockerfile --platform linux/arm64,linux/amd64 -t napple-account .
# Each platform compiles natively (QEMU under buildx for the non-native one); the deploy workflow builds arm64 on an arm64 runner.
ARG RUST_IMAGE=rust:1.99-slim-bookworm
ARG RUNTIME_IMAGE=debian:bookworm-slim

FROM ${RUST_IMAGE} AS build
WORKDIR /src
COPY Cargo.toml Cargo.lock rustfmt.toml ./
COPY crates ./crates
RUN --mount=type=cache,target=/usr/local/cargo/registry,sharing=locked \
    --mount=type=cache,target=/src/target,sharing=locked \
    cargo build --release --locked -p napple-account --bin napple-account \
 && install -D -m 0755 target/release/napple-account /out/napple-account

FROM ${RUNTIME_IMAGE}
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 --home-dir /var/lib/napple --shell /usr/sbin/nologin napple \
 && install -d -o napple -g napple /var/lib/napple/account
COPY --from=build /out/napple-account /usr/local/bin/napple-account
USER 10001:10001
ENV NAPPLE_ACCOUNT_BIND=0.0.0.0:8082 \
    NAPPLE_ACCOUNT_DATA_DIR=/var/lib/napple/account
# 8082: HTTP (behind Caddy).
EXPOSE 8082/tcp
VOLUME ["/var/lib/napple/account"]
HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD curl -fsS http://127.0.0.1:8082/healthz >/dev/null || exit 1
STOPSIGNAL SIGTERM
ENTRYPOINT ["/usr/local/bin/napple-account"]
CMD ["serve"]
