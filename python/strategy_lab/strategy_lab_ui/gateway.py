"""A local HTTP gateway between the Engine console (web/) and the real `strategy_lab_replay` executable.

A browser cannot start a native program, so this is the one piece of glue the console's Strategies section needs. It adds no
strategy logic and no JSON re-encoding of results:

* every run goes through `bridge.run_replay`, so the process handling (response file, private temp directory, time and output
  limits, exit-status contract) and the strict schema/link validation are the ones the Streamlit lab already uses;
* the replay tool's stdout is passed on BYTE FOR BYTE. The tool writes prices as exact decimal text inside JSON numbers and
  quantities as int64 integers, which `JSON.parse` would corrupt, so the browser parses that text with a lossless reader.

Endpoints (prefix /lab/v1; JSON unless noted; errors are {"error": {"code", "message", ...}} as in ADR 0006):

  GET  /status    the gateway and the runner (ready | missing | invalid), with build instructions when missing
  GET  /catalog   the tool's own `describe` document, exactly as written
  GET  /datasets  the repository's synthetic fixtures (identity, symbols, example parameters)
  POST /check     ask the real strategy constructors whether a configuration is acceptable (no replay is produced).
                  200 {"ok": true, ...derived facts} or 200 {"ok": false, "error": {...}} when they refuse it
  POST /replay    run one strategy over a built-in or uploaded dataset. The answer is TWO parts: line 1 is a compact JSON
                  object of gateway metadata, and everything after the first newline is the tool's stdout verbatim.
  GET  /validation  the checked-in scenario files (identity and a list). Runs nothing.
  POST /validate  run the checked-in scenario checks (validate.run_all) through the real executable and answer with the
                  Lab's own validation report (strategy_lab.validation_report). Only this request starts the scenarios.

Security: bound to 127.0.0.1; the Host header must name a loopback host (DNS rebinding); a browser Origin must be one of the
console's own; POST needs application/json and a bounded body; no path ever comes from the browser (datasets are named by
key, uploads travel as bytes); at most a few replays run at once.
"""

from __future__ import annotations

import argparse
import base64
import binascii
import json
import re
import sys
import threading
import traceback
from dataclasses import dataclass, field
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Sequence

from . import bridge, datasets, gateway_validation, scenarios, validate, validation_report
from .gateway_body import RequestBody
from .catalog import Catalog, StrategySpec
from .errors import LabUiError

GATEWAY_VERSION = "0.1.0"
API_VERSION = "1"
DEFAULT_PORT = 8765
PREFIX = "/lab/v1"

MAX_BODY_BYTES = 12 * 1024 * 1024       # an 8 MiB upload is 11.2 MiB as base64
BODY_READ_S = 30.0                       # a body that will be used must arrive within this long in total (on loopback it takes milliseconds)
DRAIN_S = 2.0                            # a body that will not be used is discarded for at most this long in total
MAX_PARAM_TEXT = 4096
MAX_CONCURRENT_RUNS = 2
BUSY_WAIT_S = 5.0
TEXT_CAP = 4096                          # stderr / detail shown to the browser

DEFAULT_ORIGINS = ("http://127.0.0.1:5173", "http://localhost:5173", "http://127.0.0.1:4173", "http://localhost:4173")
LOOPBACK_HOSTS = ("127.0.0.1", "localhost", "[::1]")

# One valid bar, so the real strategy constructors can be asked about a configuration without any replay worth showing.
PROBE_CSV = b"symbol,exchange_time,type,price\nPROBE,2026-01-05T14:30:00Z,bar,1\n"
# The only runner warnings that describe the configuration itself (the others are about the probe's single row).
CONFIG_WARNINGS = ("entry_threshold_unreachable",)

STATUS_FOR_KIND = {
    "bad_request": 400, "input_empty": 400, "input_not_text": 400, "input_unencodable": 400, "input_too_large": 413,
    "runner_rejected": 422, "dataset_invalid": 422, "input_missing": 500, "runner_internal_error": 500,
    "executable_missing": 503, "executable_invalid": 503, "spawn_failed": 503, "busy": 503, "timeout": 504,
    "unexpected_exit": 502, "malformed_output": 502, "unsupported_schema": 502, "invalid_structure": 502,
    "result_mismatch": 502, "output_too_large": 502,
}


@dataclass(frozen=True)
class Response:
    status: int
    body: bytes
    content_type: str = "application/json; charset=utf-8"


def dump(value: Any) -> bytes:
    return json.dumps(value, ensure_ascii=True, separators=(",", ":"), allow_nan=False).encode("ascii")


def _cap(text: str, limit: int = TEXT_CAP) -> str:
    return text if len(text) <= limit else text[:limit] + f"... [{len(text) - limit} more characters]"


def mentioned_params(message: str, names: Sequence[str]) -> list[str]:
    """Parameter names a rejection message mentions, in order of appearance (the strategies name the offending field)."""
    found = []
    for name in names:
        match = re.search(rf"(?<![A-Za-z0-9_]){re.escape(name)}(?![A-Za-z0-9_])", message)
        if match:
            found.append((match.start(), name))
    return [name for _, name in sorted(found)]


def error_payload(error: LabUiError, names: Sequence[str] = ()) -> dict[str, Any]:
    columns = [p.column for p in error.problems if p.column and p.column in names]
    fields = list(dict.fromkeys(columns + mentioned_params(error.message, names)))
    body: dict[str, Any] = {
        "code": error.kind, "title": error.title, "message": error.message,
        "problems": [{"message": p.message, "line": p.line, "column": p.column, "where": p.where} for p in error.problems],
        "total_problems": error.total_problems, "exit_status": error.exit_code, "runner_code": error.runner_code or None,
        "fields": fields, "detail": _cap(error.detail), "stderr": _cap(error.stderr or ""),
        "stdout_excerpt": _cap(error.stdout_excerpt or ""),
    }
    if len(columns) == 1:
        body["field"] = columns[0]
    if error.kind == "executable_missing":
        body["setup"] = bridge.BUILD_HELP
    return body


def error_response(error: LabUiError, names: Sequence[str] = ()) -> Response:
    return Response(STATUS_FOR_KIND.get(error.kind, 500), dump({"error": error_payload(error, names)}))


def host_name(header: str) -> str:
    """The host part of a Host header: '127.0.0.1:5173' -> '127.0.0.1', '[::1]:5173' -> '[::1]'."""
    if header.startswith("["):
        return header[:header.find("]") + 1] if "]" in header else header
    return header.rsplit(":", 1)[0] if ":" in header else header


def bad_request(message: str, field_name: str | None = None) -> LabUiError:
    error = LabUiError("bad_request", message)
    if field_name:
        error.detail = f"field: {field_name}"
    return error


# ---- request validation (the backend boundary) ------------------------------------------------------------------------

@dataclass(frozen=True)
class StrategyRequest:
    kind: str
    params: tuple[tuple[str, str], ...]


def parse_strategy(raw: Any, catalog: Catalog) -> tuple[StrategyRequest, StrategySpec]:
    """{"kind": str, "params": [[name, text], ...]}. Kind and names must be the catalog's; values are text, never converted."""
    if not isinstance(raw, dict) or set(raw) - {"kind", "params"}:
        raise bad_request("strategy must be an object with 'kind' and 'params' only.")
    kind = raw.get("kind")
    spec = catalog.strategy(kind) if isinstance(kind, str) else None
    if spec is None:
        known = ", ".join(s.kind for s in catalog.strategies)
        raise bad_request(f"Unknown strategy kind {kind!r}; the tool offers: {known}.")
    if not spec.available:
        raise bad_request(f"Strategy '{kind}' is not available: {spec.unavailable_reason}")
    pairs = raw.get("params")
    if not isinstance(pairs, list) or len(pairs) > len(spec.params):
        raise bad_request("params must be a list of [name, text] pairs, at most one per parameter.")
    allowed = {p.name for p in spec.params}
    params: list[tuple[str, str]] = []
    for pair in pairs:
        if not (isinstance(pair, list) and len(pair) == 2 and all(isinstance(x, str) for x in pair)):
            raise bad_request("each parameter must be a [name, text] pair of strings.")
        name, text = pair
        if name not in allowed:
            raise bad_request(f"'{name}' is not a parameter of {kind}; its parameters are: {', '.join(sorted(allowed))}.", name)
        if any(name == existing for existing, _ in params):
            raise bad_request(f"Parameter '{name}' is given twice.", name)
        if len(text) > MAX_PARAM_TEXT:
            raise bad_request(f"Parameter '{name}' is longer than {MAX_PARAM_TEXT} characters.", name)
        params.append((name, text))
    return StrategyRequest(kind, tuple(params)), spec


def parse_dataset(raw: Any) -> datasets.DatasetSource:
    """{"builtin": key} or {"name": str, "csv_base64": str}. The browser never names a path."""
    if not isinstance(raw, dict):
        raise bad_request("dataset must be an object.")
    if set(raw) == {"builtin"}:
        key = raw["builtin"]
        try:
            datasets.builtin(key if isinstance(key, str) else "")
        except KeyError:
            raise bad_request(f"There is no built-in dataset {key!r}.") from None
        return datasets.load_builtin(key)
    if set(raw) == {"name", "csv_base64"} and isinstance(raw["name"], str) and isinstance(raw["csv_base64"], str):
        try:
            data = base64.b64decode(raw["csv_base64"], validate=True)
        except (binascii.Error, ValueError):
            raise bad_request("csv_base64 is not valid base64.") from None
        return datasets.from_upload(raw["name"][:200], data)
    raise bad_request("dataset must be {\"builtin\": key} or {\"name\": text, \"csv_base64\": base64 text}.")


# ---- the gateway proper (no HTTP in here: it is tested directly) ------------------------------------------------------

@dataclass
class Gateway:
    runner_provider: Callable[[], bridge.Runner] = bridge.find_runner
    max_concurrent: int = MAX_CONCURRENT_RUNS
    _slots: threading.BoundedSemaphore = field(init=False, repr=False)
    _catalog: tuple[tuple, Catalog, bytes] | None = field(default=None, init=False, repr=False)
    _lock: threading.Lock = field(default_factory=threading.Lock, init=False, repr=False)
    _validations: int = field(default=0, init=False, repr=False)

    def __post_init__(self) -> None:
        self._slots = threading.BoundedSemaphore(self.max_concurrent)

    # -- the runner and its catalog (re-resolved every call, so a rebuilt executable is noticed) --------------------------
    def _runner_and_catalog(self) -> tuple[bridge.Runner, Catalog, bytes]:
        runner = self.runner_provider()
        with self._lock:
            if self._catalog is not None and self._catalog[0] == runner.identity:
                return runner, self._catalog[1], self._catalog[2]
        catalog, raw = bridge.describe_with_output(runner)
        with self._lock:
            self._catalog = (runner.identity, catalog, raw)
        return runner, catalog, raw

    def _guarded(self, work: Callable[[], Response]) -> Response:
        if not self._slots.acquire(timeout=BUSY_WAIT_S):
            raise LabUiError("busy", f"{self.max_concurrent} replays are already running; try again in a moment.")
        try:
            return work()
        finally:
            self._slots.release()

    # -- endpoints --------------------------------------------------------------------------------------------------------
    def status(self) -> Response:
        gateway = {"version": GATEWAY_VERSION, "api": API_VERSION}
        try:
            runner, catalog, _ = self._runner_and_catalog()
        except LabUiError as error:
            state = "missing" if error.kind == "executable_missing" else "invalid"
            runner_info: dict[str, Any] = {"state": state, "message": error.message, "detail": _cap(error.detail)}
            if state == "missing":
                runner_info["setup"] = bridge.BUILD_HELP
            return Response(200, dump({"gateway": gateway, "runner": runner_info}))
        return Response(200, dump({"gateway": gateway, "runner": {
            "state": "ready", "name": Path(runner.path).name, "size": runner.size, "sha256": runner.sha256,
            "tool": catalog.tool, "project_version": catalog.project_version, "schema_version": catalog.schema_version,
            "default_max_rows": catalog.default_max_rows, "hard_max_rows": catalog.hard_max_rows,
            "max_rows_used": bridge.UI_MAX_ROWS}}))

    def catalog(self) -> Response:
        _, _, raw = self._runner_and_catalog()
        return Response(200, raw)

    def datasets(self) -> Response:
        items = []
        for item in datasets.BUILTINS:
            entry: dict[str, Any] = {"key": item.key, "label": item.label, "description": item.description,
                                     "filename": item.filename, "presets": {k: dict(v) for k, v in item.presets.items()},
                                     "synthetic": True, "provenance": datasets.SYNTHETIC}
            try:
                source = datasets.load_builtin(item.key)
            except LabUiError as error:
                entry.update(available=False, message=error.message)
            else:
                rows = max(0, len(source.data.splitlines()) - 1)
                entry.update(available=True, bytes=source.size, sha256=source.sha256, rows=rows,
                             symbols=list(source.symbols))
            items.append(entry)
        return Response(200, dump({"datasets": items, "upload_limit_bytes": datasets.MAX_UPLOAD_BYTES}))

    def check(self, body: Any) -> Response:
        if not isinstance(body, dict) or set(body) != {"strategy"}:
            raise bad_request("The body must be {\"strategy\": {...}}.")
        runner, catalog, _ = self._runner_and_catalog()
        request, spec = parse_strategy(body["strategy"], catalog)
        names = [p.name for p in spec.params]

        def work() -> Response:
            try:
                outcome = bridge.run_replay(runner, dataset=PROBE_CSV, dataset_sha256=datasets.sha256_hex(PROBE_CSV),
                                            kind=request.kind, params=request.params)
            except LabUiError as error:
                if error.kind == "runner_rejected":
                    # A check that finds the configuration unacceptable has still succeeded: answer 200 so that ordinary typing
                    # does not fill the browser console with failed-request errors.
                    return Response(200, dump({"ok": False, "error": error_payload(error, names)}))
                raise
            config = outcome.document.strategies[0]
            warnings = [{"code": w.code, "message": w.message} for w in outcome.document.warnings
                        if w.code in CONFIG_WARNINGS]
            return Response(200, dump({"ok": True, "kind": config.kind, "strategy_id": config.strategy_id,
                                       "window_size": config.window_size, "derived": dict(config.derived),
                                       "parameters": dict(config.parameters), "warnings": warnings}))

        return self._guarded(work)

    def replay(self, body: Any) -> Response:
        if not isinstance(body, dict) or set(body) != {"strategy", "dataset"}:
            raise bad_request("The body must be {\"strategy\": {...}, \"dataset\": {...}}.")
        runner, catalog, _ = self._runner_and_catalog()
        request, spec = parse_strategy(body["strategy"], catalog)
        names = [p.name for p in spec.params]
        source = parse_dataset(body["dataset"])

        def work() -> Response:
            try:
                outcome = bridge.run_replay(runner, dataset=source.data, dataset_sha256=source.sha256,
                                            kind=request.kind, params=request.params)
            except LabUiError as error:
                if error.kind in ("runner_rejected", "dataset_invalid"):
                    return error_response(error, names)
                raise
            process = outcome.process
            meta = {
                "gateway": {"version": GATEWAY_VERSION, "api": API_VERSION},
                "runner": {"name": Path(runner.path).name, "size": runner.size, "sha256": runner.sha256},
                "dataset": {"kind": source.kind, "name": source.display_name, "sha256": source.sha256,
                            "bytes": source.size, "synthetic": source.synthetic, "provenance": source.provenance},
                "request": {"kind": request.kind, "params": [list(pair) for pair in request.params],
                            "max_rows": bridge.UI_MAX_ROWS},
                "process": {"exit_code": process.exit_code, "duration_ms": round(process.duration_s * 1000),
                            "args_file": process.args_file, "stderr": _cap(process.stderr),
                            "stderr_truncated": process.stderr_truncated},
            }
            # Line 1: gateway metadata (compact JSON never contains a raw newline). The rest: the tool's stdout, untouched.
            return Response(200, dump(meta) + b"\n" + process.stdout, "application/x-ndjson; charset=utf-8")

        return self._guarded(work)

    # -- scenario validation (the Lab's own checks; see validate.py and gateway_validation.py) ---------------------------------
    def validation_info(self) -> Response:
        """The scenario files that a validation would run. Nothing is run and no process is started."""
        return Response(200, dump(gateway_validation.info_document()))

    def validate(self, body: Any) -> Response:
        """Run every scenario through the real executable and answer with the Lab's validation report. Explicit requests only."""
        include = gateway_validation.include_demonstration(body)
        runner = self.runner_provider()
        scenario_set = scenarios.load_scenarios()

        def work() -> Response:
            with self._lock:
                self._validations += 1
                number = self._validations
            run = validate.run_all(runner, scenario_set, number, include_demo=include)
            return Response(200, dump(validation_report.report_dict(run, scenario_set)))

        return self._guarded(work)


# ---- HTTP -------------------------------------------------------------------------------------------------------------

def make_handler(gateway: Gateway, origins: Sequence[str]) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        server_version = f"StrategyLabGateway/{GATEWAY_VERSION}"
        protocol_version = "HTTP/1.0"
        body_in: RequestBody                                     # the unread part of this request's body (set per request in _handle)

        def log_message(self, fmt: str, *args: Any) -> None:
            sys.stderr.write(f"[gateway] {self.address_string()} {fmt % args}\n")

        def _send(self, response: Response) -> None:
            self.body_in.drain(MAX_BODY_BYTES, DRAIN_S)          # never answer with a request body still unread (see gateway_body.py)
            self.close_connection = True                         # one request per connection: nothing after this body is framed as a request
            self.send_response(response.status)
            self.send_header("Content-Type", response.content_type)
            self.send_header("Content-Length", str(len(response.body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Connection", "close")
            try:
                self.end_headers()
                if self.command != "HEAD":
                    self.wfile.write(response.body)
            except ConnectionError:
                pass                                             # the client went away (cancelled or gave up): nobody to answer

        def _fail(self, status: int, code: str, message: str) -> None:
            self._send(Response(status, dump({"error": {"code": code, "message": message}})))

        def _allowed(self) -> bool:
            if host_name(self.headers.get("Host") or "") not in LOOPBACK_HOSTS:
                self._fail(403, "host_refused", "This service answers only to loopback host names.")
                return False
            origin = self.headers.get("Origin")
            if origin is not None and origin not in origins:
                self._fail(403, "origin_refused", f"Origin {origin} is not allowed to use this service.")
                return False
            if self.headers.get("Sec-Fetch-Site") == "cross-site":
                self._fail(403, "origin_refused", "Cross-site requests are not allowed.")
                return False
            return True

        def _body(self) -> Any:
            if (self.headers.get("Content-Type") or "").split(";")[0].strip().lower() != "application/json":
                raise LabUiError("bad_request", "POST bodies must be application/json.")
            try:
                length = int(self.headers.get("Content-Length") or "")
            except ValueError:
                raise LabUiError("bad_request", "A Content-Length header is required.") from None
            if length < 0 or length > MAX_BODY_BYTES:
                raise LabUiError("input_too_large", f"The request body may be at most {MAX_BODY_BYTES // (1024 * 1024)} MiB.")
            try:
                raw = self.body_in.receive(length, BODY_READ_S)
            except OSError:
                self.body_in.abandon()
                raise LabUiError("bad_request", "The request body did not arrive completely and in time.") from None
            try:
                return json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, ValueError):
                raise LabUiError("bad_request", "The request body is not valid UTF-8 JSON.") from None

        def _body_or_empty(self) -> Any:
            """A POST whose body may be left out (Content-Length 0): validation needs no input."""
            if (self.headers.get("Content-Length") or "0").strip() == "0":
                return {}
            return self._body()

        def _declared(self) -> int:
            """The request's Content-Length as a non-negative integer (0 when absent or not a number)."""
            try:
                return max(int(self.headers.get("Content-Length") or "0"), 0)
            except ValueError:
                return 0

        def _handle(self, method: str) -> None:
            self.body_in = RequestBody(self.connection, self.rfile, self._declared())
            if not self._allowed():
                return
            path = self.path.split("?", 1)[0]
            routes: dict[tuple[str, str], Callable[[], Response]] = {
                ("GET", f"{PREFIX}/status"): gateway.status,
                ("GET", f"{PREFIX}/catalog"): gateway.catalog,
                ("GET", f"{PREFIX}/datasets"): gateway.datasets,
                ("POST", f"{PREFIX}/check"): lambda: gateway.check(self._body()),
                ("POST", f"{PREFIX}/replay"): lambda: gateway.replay(self._body()),
                ("GET", f"{PREFIX}/validation"): gateway.validation_info,
                ("POST", f"{PREFIX}/validate"): lambda: gateway.validate(self._body_or_empty()),
            }
            action = routes.get((method, path))
            if action is None:
                known = any(p == path for _, p in routes)
                self._fail(405 if known else 404, "method_not_allowed" if known else "not_found",
                           f"{method} {path} is not served." if not known else f"{method} is not allowed for {path}.")
                return
            try:
                self._send(action())
            except LabUiError as error:
                self._send(error_response(error))
            except Exception:                                    # a bug here must not take the server down
                traceback.print_exc()
                self._fail(500, "gateway_error", "The gateway failed unexpectedly; see its console for details.")

        def do_GET(self) -> None:
            self._handle("GET")

        def do_POST(self) -> None:
            self._handle("POST")

    return Handler


def make_server(gateway: Gateway, port: int = DEFAULT_PORT, origins: Sequence[str] = DEFAULT_ORIGINS) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(gateway, origins))
    server.daemon_threads = True
    return server


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Local gateway between the Engine console and strategy_lab_replay.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"loopback port (default {DEFAULT_PORT})")
    parser.add_argument("--exe", help="path to strategy_lab_replay (default: $STRATEGY_LAB_EXE, then out/strategy_lab/bin/...)")
    parser.add_argument("--allow-origin", action="append", default=[], metavar="ORIGIN",
                        help="a browser origin allowed to call the gateway, besides the Vite dev/preview servers")
    args = parser.parse_args(argv)
    gateway = Gateway(lambda: bridge.find_runner(args.exe))
    try:
        server = make_server(gateway, args.port, (*DEFAULT_ORIGINS, *args.allow_origin))
    except OSError as error:
        print(f"Cannot listen on 127.0.0.1:{args.port}: {error}. Is the gateway already running? Try --port.", file=sys.stderr)
        return 1
    try:
        runner = bridge.find_runner(args.exe)
        print(f"Runner: {runner.path}")
    except LabUiError as error:
        print(f"Runner not available yet: {error.message}\n{error.detail}", file=sys.stderr)
    print(f"Strategy Lab gateway listening on http://127.0.0.1:{args.port}{PREFIX}/ (Ctrl+C stops it)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0
