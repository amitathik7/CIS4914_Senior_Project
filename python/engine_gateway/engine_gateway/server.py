"""The engine API of ADR 0006 (REST for state, Server-Sent Events for change), served from the read model.

Paths are accepted with or without the `/api` prefix, so the console reaches it both through the Vite proxy
(`VITE_ENGINE_API=/api`, which forwards /api/... unchanged) and directly.

Read-only: the gateway never sits on the trading path (ADR 0006 section 1). Starting a backtest, stopping a run and
the kill switch are answered with 501 and a message the console shows, because the engine has no command topic yet.

Security, as the Strategy Lab gateway: bound to 127.0.0.1, the Host header must be a loopback name (DNS rebinding),
a browser Origin must be one of the console's own.
"""

from __future__ import annotations

import json
import queue
import re
import sys
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Sequence
from urllib.parse import parse_qs, unquote, urlsplit

from .read_model import Run, Store

GATEWAY_VERSION = "0.1.0"
DEFAULT_PORT = 8080
LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "[::1]")
DEFAULT_ORIGINS = ("http://127.0.0.1:5173", "http://localhost:5173", "http://127.0.0.1:4173", "http://localhost:4173")
HEARTBEAT_S = 3.0
STREAM_QUEUE = 1000                      # events buffered per browser; beyond that the stream only says "changed"

RUN_PATH = re.compile(r"/v1/runs/([^/]+)(/[a-z-]+)?")


def project_version() -> str:
    cmake = Path(__file__).resolve().parents[3] / "CMakeLists.txt"
    try:
        match = re.search(r"project\([^)]*VERSION\s+([0-9.]+)", cmake.read_text(encoding="utf-8"), re.S)
    except OSError:
        match = None
    return match.group(1) if match else "unknown"


def dump(value: Any) -> bytes:
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str):
        super().__init__(message)
        self.status, self.code, self.message = status, code, message


NOT_SUPPORTED = ("This console is reading the engine's Kafka topics, which are read-only. Start a run with "
                 "`trading_engine run --bus kafka --topic-prefix NAME` (docs/ENGINE_GATEWAY.md).")


class Api:
    def __init__(self, store: Store, version: str | None = None):
        self.store = store
        self.version = version or project_version()

    def _run(self, run_id: str) -> Run:
        run = self.store.get(run_id)
        if run is None:
            raise ApiError(404, "run_not_found", f"No run {run_id!r} has been seen on the bus.")
        return run

    def engine(self) -> dict[str, Any]:
        symbols: set[str] = set()
        first, last = None, None
        for run in self.store.runs.values():
            symbols.update(run.symbols())
            if run.first_time and (first is None or run.first_time < first):
                first = run.first_time
            if run.last_time and (last is None or run.last_time > last):
                last = run.last_time
        return {
            "project_version": self.version, "data_source": "engine", "api_version": "1",
            "market_data": {"symbols": sorted(symbols), "first_session": (first or "")[:10],
                            "last_session": (last or "")[:10]},
            "gateway": {"version": GATEWAY_VERSION, "source": self.store.source_status,
                        "decode_errors": self.store.decode_errors, **({"last_error": self.store.last_error}
                                                                     if self.store.last_error else {})},
        }

    def runs(self) -> list[dict[str, Any]]:
        summaries = sorted((run.summary() for run in self.store.runs.values()), key=lambda s: s["created_at"],
                           reverse=True)
        return sorted(summaries, key=lambda s: s["status"] != "running")      # running first, then newest first

    def run_resource(self, run_id: str, resource: str, query: dict[str, list[str]]) -> Any:
        run = self._run(run_id)
        if resource == "":
            return run.detail()
        if resource == "/bars":
            return run.bars_for((query.get("symbol") or [""])[0])
        readers: dict[str, Callable[[], Any]] = {
            "/portfolio": run.portfolio, "/equity": run.equity, "/signals": lambda: list(run.signals),
            "/risk-decisions": lambda: list(run.risk_decisions), "/orders": run.order_list, "/fills": run.fill_list,
            "/trades": run.trades, "/metrics": lambda: run.metrics(self.store.queue_capacity),
        }
        reader = readers.get(resource)
        if reader is None:
            raise ApiError(404, "not_found", f"/runs/{{id}}{resource} is not served.")
        return reader()


def make_handler(api: Api, origins: Sequence[str]) -> type[BaseHTTPRequestHandler]:
    store = api.store

    class Handler(BaseHTTPRequestHandler):
        server_version = f"EngineGateway/{GATEWAY_VERSION}"
        protocol_version = "HTTP/1.0"

        def log_message(self, fmt: str, *args: Any) -> None:
            if "/stream" not in self.path:
                sys.stderr.write(f"[engine-gateway] {fmt % args}\n")

        def _send(self, status: int, body: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            try:
                self.wfile.write(body)
            except ConnectionError:
                pass

        def _fail(self, status: int, code: str, message: str) -> None:
            self._send(status, dump({"error": {"code": code, "message": message}}))

        def _allowed(self) -> bool:
            host = self.headers.get("Host") or ""
            host = host[:host.find("]") + 1] if host.startswith("[") else host.rsplit(":", 1)[0]
            if host not in LOOPBACK_HOSTS:
                self._fail(403, "host_refused", "This service answers only to loopback host names.")
                return False
            origin = self.headers.get("Origin")
            if origin is not None and origin not in origins:
                self._fail(403, "origin_refused", f"Origin {origin} is not allowed to use this service.")
                return False
            return True

        def _route(self, method: str) -> None:
            if not self._allowed():
                return
            url = urlsplit(self.path)
            path = url.path[4:] if url.path.startswith("/api/") else url.path
            try:
                if method != "GET":
                    if path == "/v1/runs" or RUN_PATH.fullmatch(path):
                        raise ApiError(501, "not_supported", NOT_SUPPORTED)
                    raise ApiError(404, "not_found", f"{method} {path} is not served.")
                if path == "/v1/engine":
                    with store.lock:
                        return self._send(200, dump(api.engine()))
                if path == "/v1/runs":
                    with store.lock:
                        return self._send(200, dump(api.runs()))
                match = RUN_PATH.fullmatch(path)
                if match is None:
                    raise ApiError(404, "not_found", f"{path} is not served.")
                run_id, resource = unquote(match.group(1)), match.group(2) or ""
                if resource == "/stream":
                    return self._stream(run_id)
                with store.lock:
                    body = dump(api.run_resource(run_id, resource, parse_qs(url.query)))
                self._send(200, body)
            except ApiError as error:
                self._fail(error.status, error.code, error.message)
            except Exception:                                    # a bug here must not take the server down
                traceback.print_exc()
                self._fail(500, "gateway_error", "The gateway failed unexpectedly; see its console for details.")

        def _stream(self, run_id: str) -> None:
            events: queue.Queue[dict[str, Any]] = queue.Queue(STREAM_QUEUE)

            def listener(_: str, event: dict[str, Any]) -> None:
                try:
                    events.put_nowait(event)
                except queue.Full:
                    pass                                         # the console re-reads on any event, so dropping is safe

            remove = store.listen(run_id, listener)
            try:
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.end_headers()
                while True:
                    try:
                        event = events.get(timeout=HEARTBEAT_S)
                    except queue.Empty:
                        with store.lock:
                            run = store.get(run_id)
                            event = {"type": "run", "run": run.summary()} if run else None
                        if event is None:
                            self.wfile.write(b": waiting for the run\n\n")
                            self.wfile.flush()
                            continue
                    self.wfile.write(b"data: " + dump(event) + b"\n\n")
                    self.wfile.flush()
            except (ConnectionError, OSError):
                pass                                             # the browser closed the stream
            finally:
                remove()

        def do_GET(self) -> None:
            self._route("GET")

        def do_POST(self) -> None:
            self._route("POST")

        def do_PUT(self) -> None:
            self._route("PUT")

    return Handler


def make_server(api: Api, port: int = DEFAULT_PORT, origins: Sequence[str] = DEFAULT_ORIGINS) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(api, origins))
    server.daemon_threads = True
    return server
