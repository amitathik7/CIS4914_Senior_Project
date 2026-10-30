"""The engine API of ADR 0006 (REST for state, Server-Sent Events for change), served from the read model.

Paths are accepted with or without the `/api` prefix, so the console reaches it both through the Vite proxy
(`VITE_ENGINE_API=/api`, which forwards /api/... unchanged) and directly.

The gateway never sits on the trading path (ADR 0006 section 1): it reads the topics and, for the three write
endpoints (start a backtest, stop a run, the kill switch), writes a command to `<control>.command` and waits for the
engine's answer on the run's `<prefix>.run` topic (docs/adr/0007-engine-command-channel.md). With no command sender
(--replay) or no engine announcing itself, those endpoints answer with an error the console shows.

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

from .commands import CommandError, CommandSender, new_run_id
from .read_model import Run, Store

GATEWAY_VERSION = "0.1.0"
DEFAULT_PORT = 8080
LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "[::1]")
DEFAULT_ORIGINS = ("http://127.0.0.1:5173", "http://localhost:5173", "http://127.0.0.1:4173", "http://localhost:4173")
HEARTBEAT_S = 3.0
STREAM_QUEUE = 1000                      # events buffered per browser; beyond that the stream only says "changed"
ANSWER_TIMEOUT_S = 10.0                  # how long a write endpoint waits for the engine's answer
MAX_BODY = 1 << 20
FIELD_ERROR = re.compile(r"([a-z_]+(?:\.[a-z0-9_]+)*): (.+)", re.S)

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
    def __init__(self, status: int, code: str, message: str, field: str | None = None):
        super().__init__(message)
        self.status, self.code, self.message, self.field = status, code, message, field


NOT_SUPPORTED = ("This gateway is replaying a recording, so there is no engine to send commands to. Start it on "
                 "Kafka and run `trading_engine serve --bars FILE` (docs/ENGINE_GATEWAY.md).")
NO_ENGINE = ("No engine is listening for commands. Start one with `trading_engine serve --bars FILE --bus kafka` "
             "(docs/ENGINE_GATEWAY.md), then try again.")
NOT_CONTROLLABLE = ("This run was started from the command line, so nothing can send it commands. Start a backtest "
                    "from the console to stop it or use its kill switch.")


class Api:
    def __init__(self, store: Store, version: str | None = None, commands: CommandSender | None = None,
                 answer_timeout: float = ANSWER_TIMEOUT_S):
        self.store = store
        self.version = version or project_version()
        self.commands = commands
        self.answer_timeout = answer_timeout

    def _run(self, run_id: str) -> Run:
        run = self.store.get(run_id)
        if run is None:
            raise ApiError(404, "run_not_found", f"No run {run_id!r} has been seen on the bus.")
        return run

    def engine(self) -> dict[str, Any]:
        symbols: set[str] = set()
        first, last = None, None
        engines = self.store.live_engines()
        if engines:                                          # what a backtest can replay: the engines' recorded data
            for a in engines:
                coverage = a["market_data"]
                symbols.update(coverage.get("symbols", []))
                if coverage.get("first_session") and (first is None or coverage["first_session"] < first):
                    first = coverage["first_session"]
                if coverage.get("last_session") and (last is None or coverage["last_session"] > last):
                    last = coverage["last_session"]
        else:                                                # what the runs on the bus covered
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
                        "decode_errors": self.store.decode_errors, "commands": self.commands is not None,
                        "engines": [{"engine_id": a["engine_id"], "version": a.get("version", ""),
                                     "runs": a.get("runs", [])} for a in engines],
                        **({"last_error": self.store.last_error} if self.store.last_error else {})},
        }

    def runs(self) -> list[dict[str, Any]]:
        summaries = sorted((run.summary() for run in self.store.runs.values() if not run.refused()),
                           key=lambda s: s["created_at"], reverse=True)
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

    # ---- commands (ADR 0007) -----------------------------------------------------------------------------------------

    def _need_commands(self) -> CommandSender:
        if self.commands is None:
            raise ApiError(501, "not_supported", NOT_SUPPORTED)
        return self.commands

    def _send(self, action: str, run_id: str, **fields: Any) -> dict[str, Any] | None:
        """Writes a command and waits for the engine's answer (None if it does not come in time)."""
        try:
            command_id = self._need_commands().send(action, run_id, **fields)
        except CommandError as error:
            raise ApiError(503, "kafka_unavailable", f"The command could not be sent to Kafka: {error}") from None
        return self.store.wait_for_answer(command_id, self.answer_timeout)

    def _controllable(self, run_id: str) -> None:
        self._need_commands()
        with self.store.lock:
            run = self._run(run_id)
            if run.controlled_by() is None:
                raise ApiError(409, "not_controllable", NOT_CONTROLLABLE if run.status_report is None
                               or not run.status_report.get("engine_id") else NO_ENGINE)

    def create_backtest(self, body: Any) -> dict[str, Any]:
        self._need_commands()
        if not isinstance(body, dict):
            raise ApiError(400, "invalid_request", "The request must be a JSON object (a BacktestRequest).")
        with self.store.lock:
            if not self.store.live_engines():
                raise ApiError(503, "engine_offline", NO_ENGINE)
        run_id = new_run_id()
        settings = {k: v for k, v in body.items() if k in ("name", "period_start", "period_end", "strategies", "risk",
                                                            "simulation")}
        answer = self._send("start_backtest", run_id, settings={**settings, "mode": "backtest"})
        if answer is None:
            raise ApiError(504, "engine_timeout", "The engine did not answer within "
                           f"{self.answer_timeout:g} seconds. Is `trading_engine serve` still running?")
        if answer.get("error"):
            raise _refusal(answer["error"])
        with self.store.lock:
            return self._run(run_id).summary()

    def stop_run(self, run_id: str) -> dict[str, Any]:
        self._controllable(run_id)
        answer = self._send("stop", run_id)
        if answer is not None and answer.get("error"):
            raise ApiError(409, "not_running", answer["error"])
        with self.store.lock:
            return self._run(run_id).summary()                # still "running" until the replay ends, if it is slow

    def set_kill_switch(self, run_id: str, body: Any) -> dict[str, Any]:
        self._need_commands()
        if not isinstance(body, dict) or not isinstance(body.get("engaged"), bool):
            raise ApiError(400, "invalid_request", "Send {\"engaged\": true|false, \"reason\"?: text}.")
        reason = body.get("reason")
        if reason is not None and not isinstance(reason, str):
            raise ApiError(400, "invalid_request", "reason must be text.")
        self._controllable(run_id)
        answer = self._send("set_kill_switch", run_id, engaged=body["engaged"],
                            **({"reason": reason} if reason else {}))
        if answer is None:
            raise ApiError(504, "engine_timeout", "The engine did not confirm the kill switch within "
                           f"{self.answer_timeout:g} seconds; it may still change.")
        if answer.get("error"):
            raise ApiError(409, "not_running", answer["error"])
        with self.store.lock:
            return self._run(run_id).detail()


def _refusal(error: str) -> ApiError:
    """The engine's 'strategies.0.long_window: message' as the console's {field, message}."""
    match = FIELD_ERROR.fullmatch(error)
    if match is None:
        return ApiError(400, "invalid_backtest", error)
    field = match.group(1)
    if field in ("period_start", "period_end"):
        field = "period"
    return ApiError(400, "invalid_backtest", match.group(2), field)


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

        def _fail(self, status: int, code: str, message: str, field: str | None = None) -> None:
            self._send(status, dump({"error": {"code": code, "message": message, **({"field": field} if field else {})}}))

        def _body(self) -> Any:
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                length = -1
            if length < 0 or length > MAX_BODY:
                raise ApiError(400, "invalid_request", "The request body is missing a valid Content-Length.")
            raw = self.rfile.read(length) if length else b""
            try:
                return json.loads(raw or b"null")
            except ValueError:
                raise ApiError(400, "invalid_json", "The request body is not JSON.") from None

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
                    return self._write(method, path)
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
                self._fail(error.status, error.code, error.message, error.field)
            except Exception:                                    # a bug here must not take the server down
                traceback.print_exc()
                self._fail(500, "gateway_error", "The gateway failed unexpectedly; see its console for details.")

        def _write(self, method: str, path: str) -> None:
            match = RUN_PATH.fullmatch(path)
            if method == "POST" and path == "/v1/runs":
                return self._send(201, dump(api.create_backtest(self._body())))
            if match is not None and method == "POST" and match.group(2) == "/stop":
                return self._send(200, dump(api.stop_run(unquote(match.group(1)))))
            if match is not None and method == "PUT" and match.group(2) == "/kill-switch":
                return self._send(200, dump(api.set_kill_switch(unquote(match.group(1)), self._body())))
            raise ApiError(404, "not_found", f"{method} {path} is not served.")

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
