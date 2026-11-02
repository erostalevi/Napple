# syntax=docker/dockerfile:1.7
# Stand-in for napple-online / room / account / social while their crates are not on main: infra/local/stub-server.py (stdlib Python) answers /healthz and
# echoes UDP/TCP on the same ports and env vars as the real servers, so DNS, TLS, the SSM roll and the smoke test are proven end to end.
# The deploy workflow builds it instead of the real image when crates/<name>/Cargo.toml is missing; the first push with the crate replaces it.
#   docker build -f infra/docker/stub.Dockerfile --build-arg ROLE=online infra/local     (context = infra/local)
ARG PYTHON_IMAGE=python:3.13-alpine
FROM ${PYTHON_IMAGE}
ARG ROLE=online
LABEL org.opencontainers.image.title="napple-${ROLE} stand-in" \
      org.opencontainers.image.description="Placeholder answering /healthz until the real server crate is deployed"
RUN adduser -D -H -u 10001 -s /sbin/nologin napple \
 && install -d -o 10001 -g 10001 /var/lib/napple/online /var/lib/napple/room /var/lib/napple/account /var/lib/napple/social
COPY stub-server.py /usr/local/bin/stub-server.py
ENV NAPPLE_STUB_ROLE=${ROLE} \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1
USER 10001:10001
EXPOSE 8080/tcp 8081/tcp 8082/tcp 8083/tcp 31415/tcp
HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
  CMD python3 -c "import os,urllib.request; p = {'online': '8080', 'room': '8081', 'account': '8082', 'social': '8083'}[os.environ['NAPPLE_STUB_ROLE']]; urllib.request.urlopen('http://127.0.0.1:' + p + '/healthz', timeout=4)"
STOPSIGNAL SIGTERM
ENTRYPOINT ["/bin/sh", "-c", "exec python3 /usr/local/bin/stub-server.py \"$NAPPLE_STUB_ROLE\""]
