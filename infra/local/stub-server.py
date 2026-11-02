#!/usr/bin/env python3
"""Stand-in for the napple-online / napple-room binaries, for testing infra/local/run.sh (and the compose wiring) before the real crates exist.

It reads the same env vars as the real servers (docs/guides/deploy-servers.md#server-contract) and only proves the plumbing:
  online: HTTP on NAPPLE_ONLINE_HTTP (/healthz -> 200)
  room:   TCP on NAPPLE_ROOM_LISTEN (logs the connection, echoes bytes), HTTP on NAPPLE_ROOM_HTTP (/healthz -> 200)
Usage: stub-server.py online|room
"""
import http.server
import os
import signal
import socket
import socketserver
import sys
import threading


def addr(var, default):
    host, _, port = os.environ.get(var, default).rpartition(":")
    return (host or "0.0.0.0", int(port))


def log(msg):
    print(f"[stub {sys.argv[1]}] {msg}", flush=True)


class Health(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        ok = self.path in ("/healthz", "/")
        body = (f"ok stub {sys.argv[1]}\n" if ok else "not found\n").encode()
        self.send_response(200 if ok else 404)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        log("http " + fmt % args)


class ThreadingHTTP(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve_http(a):
    srv = ThreadingHTTP(a, Health)
    log(f"http on {a[0]}:{a[1]}")
    threading.Thread(target=srv.serve_forever, daemon=True).start()


def serve_tcp_echo(a):
    class Echo(socketserver.BaseRequestHandler):
        def handle(self):
            log(f"tcp {a[1]} connection from {self.client_address[0]}:{self.client_address[1]}")
            while True:
                data = self.request.recv(65536)
                if not data:
                    return
                self.request.sendall(data)

    class Srv(socketserver.ThreadingMixIn, socketserver.TCPServer):
        daemon_threads = True
        allow_reuse_address = True

    srv = Srv(a, Echo)
    log(f"tcp echo on {a[0]}:{a[1]}")
    threading.Thread(target=srv.serve_forever, daemon=True).start()


def main():
    if len(sys.argv) != 2 or sys.argv[1] not in ("online", "room"):
        sys.exit(__doc__)
    if sys.argv[1] == "online":
        serve_http(addr("NAPPLE_ONLINE_HTTP", "0.0.0.0:8080"))
    else:
        serve_tcp_echo(addr("NAPPLE_ROOM_LISTEN", "0.0.0.0:31415"))
        serve_http(addr("NAPPLE_ROOM_HTTP", "0.0.0.0:8081"))
    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    stop.wait()
    log("stopped")


if __name__ == "__main__":
    main()
