#!/usr/bin/env python3
"""Start the whole simulated trading system with one command.

    python start.py                 # Kafka, engine, engine gateway, Strategy Lab gateway, web console
    python start.py --pace-ms 200   # slower replay, more time to press Stop or the kill switch
    python start.py --build         # rebuild trading_engine first
    python start.py --check         # only check prerequisites, start nothing
    python start.py --help          # every option

It does, in order, what docs/ENGINE_GATEWAY.md and the README describe by hand:

  1. Kafka      docker compose -f docker-compose.kafka.yml up -d (starts Docker Desktop on Windows if needed)
  2. gateway    python python/engine_gateway/serve.py --port <8080, or a free port if 8080 is taken>
  3. engine     trading_engine serve --bars tests/fixtures/pipeline/demo_bars.csv --pace-ms 50
  4. lab        python python/strategy_lab/lab_gateway.py (Strategies > Backtest)
  5. console    pnpm dev in web/, with VITE_ENGINE_API=/api and ENGINE_GATEWAY pointed at the gateway

then opens http://127.0.0.1:5173. Every process's output is shown here with a name prefix and also written to
out/logs/<name>.log. Ctrl+C stops all of them and stops (not deletes) the Kafka container, so earlier runs
are still there next time; --reset-kafka starts from an empty broker.

Standard library only; works on Windows, macOS and Linux.
"""

from __future__ import annotations

import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser
from pathlib import Path

ROOT = Path(__file__).resolve().parent
WINDOWS = os.name == "nt"
COMPOSE_FILE = ROOT / "docker-compose.kafka.yml"
KAFKA_CONTAINER = "te-kafka"
KAFKA_PORT = 9092
CONSOLE_PORT = 5173
LAB_PORT = 8765
LOG_DIR = ROOT / "out" / "logs"
DEFAULT_BARS = Path("tests") / "fixtures" / "pipeline" / "demo_bars.csv"
DOCKER_DESKTOP_EXE = Path(r"C:\Program Files\Docker\Docker\Docker Desktop.exe")

COLORS = {"kafka": "35", "gateway": "36", "engine": "33", "lab": "34", "console": "32", "start": "1"}
_print_lock = threading.Lock()
_use_color = sys.stdout.isatty() and not os.environ.get("NO_COLOR")


class LaunchError(Exception):
    """A problem the user has to fix; printed without a traceback."""


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(errors="replace")  # vite prints U+279C; a cp1252 pipe or file would kill the output thread


def say(name: str, text: str) -> None:
    label = f"{name:>7} |"
    if _use_color:
        label = f"\033[{COLORS.get(name, '0')}m{label}\033[0m"
    with _print_lock:
        print(f"{label} {text}", flush=True)


# -- small helpers ------------------------------------------------------------------------------------------------


def port_open(port: int, host: str = "127.0.0.1") -> bool:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.5)
        return s.connect_ex((host, port)) == 0


def port_free(port: int) -> bool:
    # "Nothing is listening" rather than a test bind, which fails for a minute after a server closes (TIME_WAIT).
    return not port_open(port)


def wait_for(check, timeout: float, what: str, procs: "Processes | None" = None) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        if procs is not None:
            procs.raise_if_any_exited()
        time.sleep(0.5)
    raise LaunchError(f"timed out after {timeout:.0f} s waiting for {what}")


def kafka_ready() -> bool:
    from confluent_kafka.admin import AdminClient       # installed by ensure_python_deps() before this runs
    try:
        md = AdminClient({"bootstrap.servers": f"localhost:{KAFKA_PORT}", "log_level": 0}).list_topics(timeout=3)
        return bool(md.brokers) and md.controller_id >= 0
    except Exception:
        return False


def http_ok(url: str) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=2) as r:
            return r.status < 500
    except Exception:
        return False


def run_quiet(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def run_step(name: str, cmd: list[str], cwd: Path = ROOT) -> None:
    """Run a setup command to completion, streaming its output; fail loudly."""
    say(name, "$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace")
    assert proc.stdout is not None
    for line in proc.stdout:
        say(name, line.rstrip())
    if proc.wait() != 0:
        raise LaunchError(f"`{' '.join(cmd)}` failed with exit code {proc.returncode}")


def quiet(name: str, line: str) -> bool:
    """Successful requests the console makes every second: kept in the log file, not echoed."""
    return name == "gateway" and ('" 200 -' in line or '" 304 -' in line)


def _interrupt(signum, frame):
    raise KeyboardInterrupt


# -- child processes ----------------------------------------------------------------------------------------------


class Processes:
    """The long-running children: output prefixed and logged, all stopped together."""

    def __init__(self) -> None:
        self.children: list[tuple[str, subprocess.Popen]] = []
        LOG_DIR.mkdir(parents=True, exist_ok=True)

    def start(self, name: str, cmd: list[str], cwd: Path = ROOT, env: dict | None = None) -> subprocess.Popen:
        say("start", f"{name}: {' '.join(cmd)}")
        kw: dict = {}
        if WINDOWS:
            # Own process group: Ctrl+C reaches only this script, which then stops the children itself (otherwise
            # pnpm's cmd.exe wrapper asks "Terminate batch job (Y/N)?" and the window hangs).
            kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kw["start_new_session"] = True
        full_env = dict(os.environ, PYTHONUNBUFFERED="1", FORCE_COLOR="0", **(env or {}))
        proc = subprocess.Popen(cmd, cwd=cwd, env=full_env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", **kw)
        self.children.append((name, proc))
        threading.Thread(target=self._pump, args=(name, proc), daemon=True).start()
        return proc

    @staticmethod
    def _pump(name: str, proc: subprocess.Popen) -> None:
        assert proc.stdout is not None
        with open(LOG_DIR / f"{name}.log", "w", encoding="utf-8") as log:
            for line in proc.stdout:
                log.write(line)
                log.flush()
                if not quiet(name, line):
                    say(name, line.rstrip())

    def raise_if_any_exited(self) -> None:
        for name, proc in self.children:
            code = proc.poll()
            if code is not None:
                raise LaunchError(f"{name} exited with code {code}; see its output above or out/logs/{name}.log")

    def stop_all(self) -> None:
        for name, proc in reversed(self.children):
            if proc.poll() is not None:
                continue
            say("start", f"stopping {name}")
            if WINDOWS:
                # /T takes the whole tree (pnpm -> node -> vite), which proc.terminate() would leave running.
                run_quiet(["taskkill", "/PID", str(proc.pid), "/T", "/F"])
            else:
                try:
                    os.killpg(proc.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for _, proc in self.children:
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()


# -- prerequisites ------------------------------------------------------------------------------------------------


def find_engine(explicit: str | None, config: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise LaunchError(f"--engine {explicit}: no such file")
        return path
    name = "trading_engine.exe" if WINDOWS else "trading_engine"
    configs = [config] if config else ["Release", "Debug", "RelWithDebInfo"]
    candidates = []
    for build in ("build-kafka", "build"):
        for cfg in configs:
            candidates.append(ROOT / build / "bin" / cfg / name)
        if not config:
            candidates.append(ROOT / build / "bin" / name)  # single-config generators (Ninja, Makefiles)
    found = [c for c in candidates if c.is_file()]
    if not found:
        raise LaunchError("no trading_engine build found. Build the Kafka build first (README, 'Build'), e.g.\n"
                          "  cmake --preset ... -DTRADING_ENGINE_WITH_KAFKA=ON && cmake --build build-kafka\n"
                          "or run `python start.py --build` once build-kafka is configured.")
    return max(found, key=lambda p: p.stat().st_mtime)  # the most recently built one


def docker_running() -> bool:
    return shutil.which("docker") is not None and run_quiet(["docker", "info"]).returncode == 0


def ensure_docker() -> None:
    if shutil.which("docker") is None:
        raise LaunchError("Docker is not installed (or not on PATH). Install Docker Desktop, or start your own "
                          f"Kafka on localhost:{KAFKA_PORT} and run with --no-kafka.")
    if docker_running():
        return
    if WINDOWS and DOCKER_DESKTOP_EXE.is_file():
        say("kafka", "Docker Desktop is not running; starting it (this can take a minute)")
        subprocess.Popen([str(DOCKER_DESKTOP_EXE)], creationflags=subprocess.DETACHED_PROCESS)
        wait_for(docker_running, 180, "Docker Desktop to start")
        return
    raise LaunchError("Docker is not running. Start Docker Desktop (or the docker daemon) and try again.")


def ensure_python_deps() -> None:
    try:
        import confluent_kafka  # noqa: F401
    except ImportError:
        run_step("start", [sys.executable, "-m", "pip", "install", "-r",
                           str(ROOT / "python" / "engine_gateway" / "requirements.txt")])


def find_pnpm() -> str:
    pnpm = shutil.which("pnpm")
    if pnpm is None:
        raise LaunchError("pnpm not found. Install Node.js, then `npm install -g pnpm` (or `corepack enable`).")
    return pnpm


def pick_gateway_port(requested: int | None) -> int:
    if requested is not None:
        if not port_free(requested):
            raise LaunchError(f"--gateway-port {requested} is already in use")
        return requested
    for port in (8080, 18080, 8081, 18081):
        if port_free(port):
            return port
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


# -- main ---------------------------------------------------------------------------------------------------------


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Start Kafka, the engine, both gateways and the web console together.")
    p.add_argument("--bars", action="append", help=f"CSV of bars the engine can replay (repeatable; default {DEFAULT_BARS})")
    p.add_argument("--pace-ms", type=int, default=50, help="pause after each bar time so runs can be watched (default 50)")
    p.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo"],
                   help="which trading_engine build to use (default: the most recently built)")
    p.add_argument("--engine", help="path to trading_engine, overriding the search in build-kafka/ and build/")
    p.add_argument("--build", action="store_true", help="run `cmake --build build-kafka --target trading_engine` first")
    p.add_argument("--gateway-port", type=int, help="engine gateway port (default 8080, or a free one if it is taken)")
    p.add_argument("--no-kafka", action="store_true", help=f"don't touch Docker; use a broker already on localhost:{KAFKA_PORT}")
    p.add_argument("--reset-kafka", action="store_true", help="delete the broker's topics (earlier runs) before starting")
    p.add_argument("--keep-kafka", action="store_true", help="leave the Kafka container running on exit")
    p.add_argument("--no-lab", action="store_true", help="don't start the Strategy Lab gateway")
    p.add_argument("--no-browser", action="store_true", help="don't open the console in a browser")
    p.add_argument("--check", action="store_true", help="check prerequisites and exit")
    return p.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    if WINDOWS:
        os.system("")  # turns on ANSI colours in the Windows console
    args = parse_args(argv)
    os.chdir(ROOT)
    # Ctrl+C, closing the window (Windows) and SIGTERM all mean "stop everything", even when started in the background.
    signal.signal(signal.SIGINT, _interrupt)
    signal.signal(signal.SIGTERM, _interrupt)
    if WINDOWS:
        signal.signal(signal.SIGBREAK, _interrupt)
    procs = Processes()
    started_kafka = False
    try:
        # 1. Check everything before starting anything.
        if args.build:
            cfg = args.config or "Release"
            run_step("start", ["cmake", "--build", "build-kafka", "--config", cfg, "--target", "trading_engine"])
            args.config = cfg
        engine = find_engine(args.engine, args.config)
        bars = args.bars or [str(DEFAULT_BARS)]
        for b in bars:
            if not (ROOT / b).is_file() and not Path(b).is_file():
                raise LaunchError(f"--bars {b}: no such file")
        pnpm = find_pnpm()
        if not port_free(CONSOLE_PORT):
            raise LaunchError(f"port {CONSOLE_PORT} is in use: is the console (or an earlier start.py) already running?")
        run_lab = not args.no_lab
        if run_lab and not port_free(LAB_PORT):
            say("start", f"port {LAB_PORT} is in use, so the Strategy Lab gateway seems to be running already; not starting another")
            run_lab = False
        gateway_port = pick_gateway_port(args.gateway_port)
        if gateway_port != 8080 and args.gateway_port is None:
            say("start", f"port 8080 is taken (Apache?), so the engine gateway uses {gateway_port}")
        say("start", f"engine build: {engine.relative_to(ROOT) if engine.is_relative_to(ROOT) else engine}")
        if not args.no_kafka:
            ensure_docker()
        if args.check:
            missing = []
            try:
                import confluent_kafka  # noqa: F401
            except ImportError:
                missing.append("confluent-kafka (python package)")
            if not (ROOT / "web" / "node_modules").is_dir():
                missing.append("web/node_modules")
            if missing:
                say("start", "will install on first start: " + ", ".join(missing))
            say("start", "prerequisites OK; nothing started (--check)")
            return 0
        ensure_python_deps()
        if not (ROOT / "web" / "node_modules").is_dir():
            run_step("console", [pnpm, "install"], cwd=ROOT / "web")

        # 2. Kafka.
        compose = ["docker", "compose", "-f", str(COMPOSE_FILE)]
        if not args.no_kafka:
            if args.reset_kafka:
                run_step("kafka", compose + ["down"])
            if port_open(KAFKA_PORT) and not args.reset_kafka:
                say("kafka", f"a broker is already listening on {KAFKA_PORT}; using it")
            else:
                run_step("kafka", compose + ["up", "-d"])
                started_kafka = True
        wait_for(lambda: port_open(KAFKA_PORT), 90, f"Kafka on localhost:{KAFKA_PORT}")
        # The port opens seconds before the broker serves requests; the engine's CreateTopics fails until it does.
        wait_for(kafka_ready, 90, f"the Kafka broker on localhost:{KAFKA_PORT} to answer")
        say("kafka", f"broker up on localhost:{KAFKA_PORT}")

        # 3. Gateway, engine, lab, console.
        procs.start("gateway", [sys.executable, "python/engine_gateway/serve.py", "--port", str(gateway_port)])
        engine_cmd = [str(engine), "serve", "--pace-ms", str(args.pace_ms)]
        for b in bars:
            engine_cmd += ["--bars", b]
        procs.start("engine", engine_cmd)
        if run_lab:
            procs.start("lab", [sys.executable, "python/strategy_lab/lab_gateway.py"])
        procs.start("console", [pnpm, "dev"], cwd=ROOT / "web",
                    env={"VITE_ENGINE_API": "/api", "ENGINE_GATEWAY": f"http://127.0.0.1:{gateway_port}"})

        wait_for(lambda: port_open(gateway_port), 60, "the engine gateway", procs)
        url = f"http://127.0.0.1:{CONSOLE_PORT}/"
        wait_for(lambda: http_ok(url), 90, "the web console", procs)

        say("start", "")
        say("start", f"Everything is up. Console: {url}  (Backtests > New backtest)")
        say("start", f"Engine gateway: http://127.0.0.1:{gateway_port}/v1/   Logs: out/logs/")
        say("start", "Press Ctrl+C to stop everything.")
        say("start", "")
        if not args.no_browser:
            webbrowser.open(url)

        while True:
            procs.raise_if_any_exited()
            time.sleep(1)
    except KeyboardInterrupt:
        say("start", "Ctrl+C: shutting down")
        return_code = 0
    except LaunchError as e:
        say("start", f"ERROR: {e}")
        return_code = 1
    finally:
        signal.signal(signal.SIGINT, signal.SIG_IGN)  # a second Ctrl+C must not cut the cleanup short
        procs.stop_all()
        if started_kafka and not args.keep_kafka:
            say("kafka", "stopping the Kafka container (runs are kept; --reset-kafka clears them)")
            run_quiet(["docker", "compose", "-f", str(COMPOSE_FILE), "stop"])
    return return_code


if __name__ == "__main__":
    sys.exit(main())
