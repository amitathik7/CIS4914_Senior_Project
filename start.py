#!/usr/bin/env python3
"""Start the whole simulated trading system with one command.

    python3 start.py                 # Kafka, engine, engine gateway, Strategy Lab gateway, web console
    python3 start.py --pace-ms 200   # slower replay, more time to press Stop or the kill switch
    python3 start.py --year 2025     # one calendar year of real minute bars instead of the last 365 days
    python3 start.py --demo          # the 90-minute demo bars, for a quick look
    python3 start.py --set-keys      # enter (or replace) your Alpaca keys for live runs
    python3 start.py --check         # only check prerequisites, start nothing
    python3 start.py --help          # every option

First it sets itself up, so a fresh clone needs nothing else run by hand:

  * .venv      a virtualenv with the gateway's Python packages (kept in step with requirements.txt); the
               script then re-runs itself inside it
  * engine     configures build-kafka (Kafka on) if there is no Kafka build, and builds it incrementally on
               every start, so a pull never runs a stale engine. librdkafka missing: offers to install it
               (Homebrew on macOS, vcpkg on Windows)
  * keys       ALPACA_API_KEY / ALPACA_API_SECRET from the environment, else the ones saved earlier (macOS
               Keychain, or a file in your user folder elsewhere; never in the project), else it asks once.
               Enter skips: backtests then replay the demo bars, and live runs are off
  * data       with keys, downloads the last 365 days of AAPL, MSFT and SPY minute bars into data/ once
               (gitignored); if that fails, this start replays the demo bars and the next one tries again
  * web        pnpm install when web/node_modules is missing or older than the lockfile

Then it does, in order, what docs/ENGINE_GATEWAY.md and the README describe by hand:

  1. Kafka      docker compose -f docker-compose.kafka.yml up -d (starts Docker Desktop if needed; on 9092, or a
                free port if something other than a broker holds 9092)
  2. gateway    python python/engine_gateway/serve.py --port <8080, or a free port if 8080 is taken>
  3. engine     trading_engine serve --bars data/alpaca_year.csv --pace-ms 50
  4. lab        python python/strategy_lab/lab_gateway.py (Strategies > Backtest)
  5. console    pnpm dev in web/, with VITE_ENGINE_API=/api and ENGINE_GATEWAY pointed at the gateway

then opens http://127.0.0.1:5173. A default port (5173, 8080, 8765, 9092) that is already taken moves that
service to a free port, every other piece is pointed at it, and the ports used are printed at the end. Every
process's output is shown here with a name prefix and also written to out/logs/<name>.log. Ctrl+C stops all of
them and stops (not deletes) the Kafka container, so earlier runs are still there next time; --reset-kafka starts
from an empty broker.

Standard library only; works on Windows, macOS and Linux.
"""

from __future__ import annotations

import argparse
import datetime
import getpass
import hashlib
import json
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
MAC = sys.platform == "darwin"
VENV = ROOT / ".venv"
REQUIREMENTS = ROOT / "python" / "engine_gateway" / "requirements.txt"
KAFKA_BUILD = ROOT / "build-kafka"
KEY_VARS = ("ALPACA_API_KEY", "ALPACA_API_SECRET")
KEY_SERVICES = ("alpaca-api-key", "alpaca-api-secret")   # macOS Keychain item names
LAUNCH = "py start.py" if WINDOWS else "python3 start.py"
LAB_OPTION = "-DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON"
LAB_TARGETS = ("strategy_lab_replay", "strategy_lab_backtest")
COMPOSE_FILE = ROOT / "docker-compose.kafka.yml"
KAFKA_CONTAINER = "te-kafka"
KAFKA_PORT = 9092
GATEWAY_PORT = 8080
CONSOLE_PORT = 5173
LAB_PORT = 8765
LOG_DIR = ROOT / "out" / "logs"
DEMO_BARS = Path("tests") / "fixtures" / "pipeline" / "demo_bars.csv"
YEAR_BARS = Path("data") / "alpaca_year.csv"           # the default: fetched once with the user's own keys, never committed
YEAR_SYMBOLS = "AAPL,MSFT,SPY"
LAST_BARS = Path("out") / "start-bars.json"            # the bars the last start replayed, used again by default
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


def port_bindable(port: int) -> bool:
    # For fallback ports only: also rules out ports Windows reserves (Hyper-V/Docker) that nothing listens on.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        try:
            s.bind(("127.0.0.1", port))
            return True
        except OSError:
            return False


def pick_port(default: int, requested: int | None = None, flag: str = "") -> int:
    """The default port if nothing is listening on it, else the next free one of the 9 after it, else any free port."""
    if requested is not None:
        if not port_free(requested):
            raise LaunchError(f"{flag} {requested} is already in use")
        return requested
    if port_free(default):
        return default
    for port in range(default + 1, default + 10):
        if port_free(port) and port_bindable(port):
            return port
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_for(check, timeout: float, what: str, procs: "Processes | None" = None) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if check():
            return
        if procs is not None:
            procs.raise_if_any_exited()
        time.sleep(0.5)
    raise LaunchError(f"timed out after {timeout:.0f} s waiting for {what}")


def kafka_ready(port: int = KAFKA_PORT) -> bool:
    from confluent_kafka.admin import AdminClient       # installed by ensure_python_deps() before this runs
    try:
        md = AdminClient({"bootstrap.servers": f"localhost:{port}", "log_level": 0}).list_topics(timeout=3)
        return bool(md.brokers) and md.controller_id >= 0
    except Exception:
        return False


def http_ok(url: str) -> bool:
    try:
        with urllib.request.urlopen(url, timeout=2) as r:
            return r.status < 500
    except Exception:
        return False


def engine_announced(gateway_port: int, engine_id: str) -> bool:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{gateway_port}/v1/engine", timeout=2) as r:
            info = json.load(r)
    except Exception:
        return False
    return any(e.get("engine_id") == engine_id for e in info.get("gateway", {}).get("engines", []))


def run_quiet(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)


def run_step(name: str, cmd: list[str], cwd: Path = ROOT, env: dict | None = None) -> None:
    """Run a setup command to completion, streaming its output; fail loudly."""
    say(name, "$ " + " ".join(cmd))
    proc = subprocess.Popen(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                            encoding="utf-8", errors="replace", env=None if env is None else {**os.environ, **env})
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


def cache_flag(build_dir: Path, option: str) -> bool | None:
    """A CMake option as the build directory was configured (False if unset), or None if there is no build."""
    try:
        text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        if line.startswith(option + ":"):
            return line.split("=", 1)[1].strip().upper() in ("ON", "TRUE", "1", "YES", "Y")
    return False


def kafka_setting(build_dir: Path) -> bool | None:
    return cache_flag(build_dir, "TRADING_ENGINE_WITH_KAFKA")


def build_dir_of(exe: Path) -> Path:
    """build-x/bin/Debug/trading_engine.exe -> build-x (also build-x/bin/trading_engine)."""
    for parent in exe.parents:
        if (parent / "CMakeCache.txt").is_file():
            return parent
    return exe.parent


def configure_hint() -> str:
    if WINDOWS:
        return ("  cmake -S . -B build-kafka -G \"Visual Studio 17 2022\" -A x64 `\n"
                "    -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DTRADING_ENGINE_WITH_KAFKA=ON\n"
                "  py start.py")
    return ("  cmake -S . -B build-kafka -DCMAKE_BUILD_TYPE=Release -DTRADING_ENGINE_WITH_KAFKA=ON   # needs librdkafka\n"
            "  python3 start.py")


def ask_yes(question: str) -> bool:
    if not sys.stdin.isatty():
        return False
    try:
        return input(f"{question} [Y/n] ").strip().lower() in ("", "y", "yes")
    except EOFError:
        return False


def brew() -> str | None:
    return shutil.which("brew") if MAC else None


def brew_env() -> dict:
    """Lets pip and CMake find Homebrew's librdkafka when they build against it."""
    prefix = run_quiet([brew(), "--prefix"]).stdout.strip() if brew() else ""
    return {"C_INCLUDE_PATH": f"{prefix}/include", "LIBRARY_PATH": f"{prefix}/lib"} if prefix else {}


def vcpkg() -> Path | None:
    for root in (os.environ.get("VCPKG_ROOT"), r"C:\vcpkg"):
        if root and (Path(root) / "vcpkg.exe").is_file():
            return Path(root)
    return None


def librdkafka_found() -> bool:
    if WINDOWS:
        v = vcpkg()
        return v is not None and "librdkafka:x64-windows" in run_quiet([str(v / "vcpkg.exe"), "list", "librdkafka"]).stdout
    return shutil.which("pkg-config") is not None and run_quiet(["pkg-config", "--exists", "rdkafka"]).returncode == 0


def ensure_librdkafka() -> None:
    """The Kafka C library the engine (and, without a prebuilt wheel, confluent-kafka) builds against."""
    if librdkafka_found():
        return
    if MAC and brew():
        cmd = [brew(), "install", "librdkafka", "pkgconf"]
    elif WINDOWS and vcpkg():
        cmd = [str(vcpkg() / "vcpkg.exe"), "install", "librdkafka:x64-windows", "curl:x64-windows"]
    else:
        hint = ("install vcpkg (https://vcpkg.io), then: vcpkg install librdkafka:x64-windows curl:x64-windows"
                if WINDOWS else "sudo apt install librdkafka-dev libcurl4-openssl-dev pkg-config"
                if not MAC else "install Homebrew (https://brew.sh), then: brew install librdkafka pkgconf")
        raise LaunchError(f"librdkafka (the Kafka C library) is not installed. To install it: {hint}")
    if not ask_yes(f"start   | librdkafka (the Kafka C library) is missing. Install it now with `{' '.join(cmd[1:])}`?"):
        raise LaunchError("librdkafka is needed for the engine. Install it with: " + " ".join(Path(cmd[0]).name if i == 0
                          else c for i, c in enumerate(cmd)))
    run_step("start", cmd)


def libcurl_missing(build_dir: Path) -> bool:
    """True if the build was configured without libcurl (no Alpaca downloads or live feed in that engine)."""
    try:
        text = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    except OSError:
        return False
    return "CURL_INCLUDE_DIR:PATH=CURL_INCLUDE_DIR-NOTFOUND" in text


def libcurl_installed() -> bool:
    if WINDOWS:
        v = vcpkg()
        return v is not None and "curl:x64-windows" in run_quiet([str(v / "vcpkg.exe"), "list", "curl"]).stdout
    if MAC:
        return True     # part of the macOS SDK
    return shutil.which("pkg-config") is not None and run_quiet(["pkg-config", "--exists", "libcurl"]).returncode == 0


def ensure_libcurl() -> None:
    """A build-kafka configured before libcurl was installed (e.g. librdkafka came first) keeps the miss cached, and
    its engine can't download Alpaca bars. Installs libcurl if needed and configures the build again."""
    if not libcurl_missing(KAFKA_BUILD):
        return
    hint = ("vcpkg install curl:x64-windows" if WINDOWS else "sudo apt install libcurl4-openssl-dev pkg-config")
    if not libcurl_installed():
        cmd = [str(vcpkg() / "vcpkg.exe"), "install", "curl:x64-windows"] if WINDOWS and vcpkg() else None
        if cmd is None or not ask_yes("start   | libcurl (needed to download Alpaca bars) is missing. "
                                      "Install it now with `vcpkg install curl:x64-windows`?"):
            say("build", f"WARNING: this engine has no libcurl, so Alpaca downloads and live runs fail. "
                         f"Install it ({hint}) and start again.")
            return
        run_step("start", cmd)
    say("build", "build-kafka was configured without libcurl; configuring it again so Alpaca downloads work")
    run_step("build", ["cmake", "-S", str(ROOT), "-B", str(KAFKA_BUILD), "-U", "CURL_*"], env=brew_env() or None)


def configure_kafka_build() -> None:
    if shutil.which("cmake") is None:
        raise LaunchError("cmake is not installed" + (" (brew install cmake)" if MAC else "") + ".")
    ensure_librdkafka()
    if WINDOWS:
        toolchain = vcpkg() / "scripts" / "buildsystems" / "vcpkg.cmake"   # ensure_librdkafka() found vcpkg
        cmd = ["cmake", "-S", str(ROOT), "-B", str(KAFKA_BUILD), "-G", "Visual Studio 17 2022", "-A", "x64",
               f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}", "-DTRADING_ENGINE_WITH_KAFKA=ON", LAB_OPTION]
    else:
        generator = ["-G", "Ninja"] if shutil.which("ninja") else []
        cmd = ["cmake", "-S", str(ROOT), "-B", str(KAFKA_BUILD), *generator, "-DCMAKE_BUILD_TYPE=Release",
               "-DTRADING_ENGINE_WITH_KAFKA=ON", LAB_OPTION]
    say("build", "no Kafka build of trading_engine yet; configuring build-kafka (once)")
    run_step("build", cmd, env=brew_env() or None)


def prepare_engine(explicit: str | None, config: str | None, build: bool) -> Path:
    """Configures build-kafka if no Kafka build exists, brings it up to date, and returns the engine to run."""
    if explicit:
        return find_engine(explicit, config)
    if kafka_setting(KAFKA_BUILD) is None:
        try:
            return find_engine(None, config)      # a Kafka build somewhere else (e.g. out/ from the IDE)
        except LaunchError:
            configure_kafka_build()
    if build and kafka_setting(KAFKA_BUILD) is True:
        ensure_libcurl()
        if not cache_flag(KAFKA_BUILD, "TRADING_ENGINE_BUILD_STRATEGY_LAB"):
            run_step("build", ["cmake", "-S", str(ROOT), "-B", str(KAFKA_BUILD), LAB_OPTION], env=brew_env() or None)
        run_step("build", ["cmake", "--build", str(KAFKA_BUILD), "--config", config or "Release",
                           "--target", "trading_engine", *LAB_TARGETS])
    return find_engine(None, config)


def lab_exe(config: str | None) -> Path | None:
    """strategy_lab_replay from build-kafka, for the Strategy Lab gateway (Strategies page)."""
    name = "strategy_lab_replay.exe" if WINDOWS else "strategy_lab_replay"
    for path in [KAFKA_BUILD / "bin" / c / name for c in ([config] if config else ["Release", "Debug"])] + \
                [KAFKA_BUILD / "bin" / name]:
        if path.is_file():
            return path
    return None


def find_engine(explicit: str | None, config: str | None) -> Path:
    """The newest trading_engine built with Kafka. `serve` exits at once ("serve needs Kafka") in any other build,
    so a newer build without Kafka (an ordinary `build/` or a Debug build from the IDE) is passed over."""
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            raise LaunchError(f"--engine {explicit}: no such file")
        if kafka_setting(build_dir_of(path.resolve())) is False:
            raise LaunchError(f"--engine {explicit} was built without Kafka (TRADING_ENGINE_WITH_KAFKA=OFF), and "
                              "`trading_engine serve` needs it. Use a Kafka build, e.g.\n" + configure_hint())
        return path
    name = "trading_engine.exe" if WINDOWS else "trading_engine"
    configs = [config] if config else ["Release", "Debug", "RelWithDebInfo", "MinSizeRel"]
    candidates = []
    for build in sorted(p for p in ROOT.iterdir() if p.is_dir() and p.name.startswith("build")) + [ROOT / "out"]:
        for cfg in configs:
            candidates.append(build / "bin" / cfg / name)
        if not config:
            candidates.append(build / "bin" / name)  # single-config generators (Ninja, Makefiles)
    found = [c for c in candidates if c.is_file()]
    with_kafka = [c for c in found if kafka_setting(build_dir_of(c)) is not False]
    if not with_kafka:
        if found:
            listed = "\n".join(f"  {c.relative_to(ROOT)}" for c in found)
            raise LaunchError("found trading_engine, but no build made with Kafka:\n" + listed + "\n"
                              "`trading_engine serve` (what the console talks to) needs -DTRADING_ENGINE_WITH_KAFKA=ON. "
                              "Configure a separate Kafka build once, then build it:\n" + configure_hint())
        raise LaunchError("no trading_engine build found. Configure the Kafka build once, then build it:\n"
                          + configure_hint())
    return max(with_kafka, key=lambda p: p.stat().st_mtime)  # the most recently built one


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
    if MAC and Path("/Applications/Docker.app").is_dir():
        say("kafka", "Docker Desktop is not running; starting it (this can take a minute)")
        run_quiet(["open", "-a", "Docker"])
        wait_for(docker_running, 180, "Docker Desktop to start")
        return
    raise LaunchError("Docker is not running. Start Docker Desktop (or the docker daemon) and try again.")


def venv_python() -> Path:
    return VENV / ("Scripts/python.exe" if WINDOWS else "bin/python")


def in_venv() -> bool:
    return Path(sys.prefix).resolve() == VENV.resolve()


def ensure_venv(argv: list[str]) -> int | None:
    """Sets up .venv with the gateway's packages and re-runs this script in it. None: already running there.

    A virtualenv rather than the system Python: Homebrew's (and Debian's) refuse `pip install` outright."""
    stamp = VENV / ".start-requirements"
    wanted = hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()
    current = stamp.is_file() and stamp.read_text().strip() == wanted
    if in_venv() and current:
        return None
    if not venv_python().is_file():
        say("start", "creating .venv for the gateways' Python packages (once)")
        run_step("start", [sys.executable, "-m", "venv", str(VENV)])
    if not current:
        if not WINDOWS:
            ensure_librdkafka()   # confluent-kafka builds against it where pip has no prebuilt wheel
        run_step("start", [str(venv_python()), "-m", "pip", "install", "--disable-pip-version-check", "-q",
                           "-r", str(REQUIREMENTS)], env=brew_env() or None)
        stamp.write_text(wanted)
    if in_venv():
        return None
    cmd = [str(venv_python()), str(Path(__file__).resolve()), *argv]
    if WINDOWS:   # no exec on Windows: run it and pass its exit code on; it handles Ctrl+C itself
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        return subprocess.call(cmd)
    os.execv(cmd[0], cmd)
    return None  # not reached


def key_dir() -> Path:
    base = Path(os.environ["APPDATA"]) if WINDOWS and os.environ.get("APPDATA") else Path.home() / ".config"
    return base / "trading_engine"


def key_store() -> str:
    return "the macOS Keychain" if MAC else str(key_dir() / "alpaca.json")


def stored_keys() -> tuple[str, str] | None:
    if MAC:
        found = [subprocess.run(["security", "find-generic-password", "-s", s, "-w"], capture_output=True, text=True)
                 for s in KEY_SERVICES]
        if all(r.returncode == 0 and r.stdout.strip() for r in found):
            return found[0].stdout.strip(), found[1].stdout.strip()
        return None
    try:
        saved = json.loads((key_dir() / "alpaca.json").read_text(encoding="utf-8"))
        return (saved["key"], saved["secret"]) if saved.get("key") and saved.get("secret") else None
    except (OSError, ValueError, KeyError):
        return None


def save_keys(key: str, secret: str) -> None:
    if MAC:
        # Through stdin, so the secret never appears in a process list.
        user = getpass.getuser()
        script = "".join(f"add-generic-password -U -a {user} -s {s} -w {v}\n" for s, v in zip(KEY_SERVICES, (key, secret)))
        if subprocess.run(["security", "-i"], input=script, capture_output=True, text=True).returncode != 0:
            raise LaunchError("could not save the keys in the macOS Keychain")
        return
    key_dir().mkdir(parents=True, exist_ok=True)
    path = key_dir() / "alpaca.json"
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)   # readable by you only
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"key": key, "secret": secret}, f)


def forget_keys() -> None:
    if MAC:
        for s in KEY_SERVICES:
            run_quiet(["security", "delete-generic-password", "-s", s])
    (key_dir() / "alpaca.json").unlink(missing_ok=True)
    (key_dir() / "alpaca.skip").unlink(missing_ok=True)


def ask_keys() -> tuple[str, str] | None:
    say("start", "Your Alpaca keys download a year of real market data for backtests and run live trading (free paper")
    say("start", f"account: alpaca.markets, then API Keys). They are saved once, in {key_store()}, never in the project.")
    say("start", "Enter skips: backtests then replay 90 minutes of demo bars.")
    try:
        key = input("        Alpaca key ID: ").strip()
        secret = getpass.getpass("        Alpaca secret (hidden): ").strip() if key else ""
    except EOFError:
        return None
    return (key, secret) if key and secret else None


def ensure_alpaca_keys(set_keys: bool) -> None:
    """Puts the Alpaca keys in this process's environment, which the engine inherits."""
    skip = key_dir() / "alpaca.skip"
    if not set_keys and all(os.environ.get(v) for v in KEY_VARS):
        say("start", "Alpaca keys: from the environment")
        return
    keys = None if set_keys else stored_keys()
    if keys is not None:
        say("start", f"Alpaca keys: from {key_store()}")
    else:
        if not set_keys and (skip.exists() or not sys.stdin.isatty()):
            say("start", f"Alpaca keys: none saved, so no real data or live runs ({LAUNCH} --set-keys adds them)")
            return
        keys = ask_keys()
        if keys is None:
            key_dir().mkdir(parents=True, exist_ok=True)
            skip.touch()
            say("start", f"skipped: backtests replay the demo bars, live runs are off. {LAUNCH} --set-keys adds the keys later")
            return
        save_keys(*keys)
        skip.unlink(missing_ok=True)
        say("start", f"Alpaca keys saved in {key_store()}; you won't be asked again")
    os.environ[KEY_VARS[0]], os.environ[KEY_VARS[1]] = keys


def web_deps_stale() -> bool:
    marker = ROOT / "web" / "node_modules" / ".modules.yaml"
    lock = ROOT / "web" / "pnpm-lock.yaml"
    return not marker.is_file() or (lock.is_file() and lock.stat().st_mtime > marker.stat().st_mtime)


def find_pnpm() -> str:
    pnpm = shutil.which("pnpm")
    if pnpm is None:
        raise LaunchError("pnpm not found. Install Node.js, then `npm install -g pnpm` (or `corepack enable`).")
    return pnpm


# -- main ---------------------------------------------------------------------------------------------------------


def last_bars() -> list[str]:
    """The bars the previous start replayed, if they are all still there."""
    try:
        bars = json.loads((ROOT / LAST_BARS).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return []
    if not isinstance(bars, list) or not bars or not all(isinstance(b, str) and (ROOT / b).is_file() for b in bars):
        return []
    if bars == [str(DEMO_BARS)]:
        return []           # written by older versions after a plain start; the year of real bars replaces it now
    if bars != [str(YEAR_BARS)]:
        say("start", f"replaying {', '.join(bars)} as last time (--demo for the demo bars, --bars to choose)")
    return bars


def remember_bars(bars: list[str]) -> None:
    if bars == [str(DEMO_BARS)]:
        return              # --demo is for one start; the next goes back to the real bars
    try:
        (ROOT / LAST_BARS).parent.mkdir(exist_ok=True)
        (ROOT / LAST_BARS).write_text(json.dumps(bars), encoding="utf-8")
    except OSError:
        pass


def year_file(year: int | None) -> Path:
    return YEAR_BARS if year is None else Path("data") / f"alpaca_{year}.csv"


def year_range(year: int | None, today: datetime.date) -> tuple[datetime.date, datetime.date]:
    """The 365 days up to yesterday, or one calendar year (the current one up to yesterday)."""
    end = today - datetime.timedelta(days=1)
    if year is None:
        return end - datetime.timedelta(days=365), end
    if year > end.year:
        raise LaunchError(f"--year {year}: no trading day of {year} has finished yet")
    return datetime.date(year, 1, 1), min(datetime.date(year, 12, 31), end)


def fetch_year(engine: Path, year: int | None, required: bool) -> bool:
    """Downloads a year of minute bars once. False, after saying why, when the automatic download can't happen."""
    path = year_file(year)
    target = ROOT / path
    if target.is_file():
        say("data", f"replaying {path} (delete it to download it again)")
        return True
    if not all(os.environ.get(v) for v in KEY_VARS):
        if required:
            raise LaunchError(f"--year downloads the bars with your Alpaca keys, and none are saved. Add them with "
                              f"{LAUNCH} --set-keys, then try again.")
        say("data", f"replaying the demo bars; once you add Alpaca keys ({LAUNCH} --set-keys) a year of real "
                    "bars is downloaded instead")
        return False
    start, end = year_range(year, datetime.date.today())
    partial = target.with_name(target.name + ".partial")
    target.parent.mkdir(exist_ok=True)
    say("data", f"downloading {start} to {end} of {YEAR_SYMBOLS} minute bars from Alpaca into {path} (once, about a minute)")
    try:
        run_step("data", [str(engine), "fetch-alpaca", "--symbols", YEAR_SYMBOLS, "--start", start.isoformat(),
                          "--end", end.isoformat(), "--timeframe", "1Min", "--out", str(partial)])
    except LaunchError:
        partial.unlink(missing_ok=True)
        if required:
            raise
        say("data", "WARNING: the download failed (the reason is above), so this start replays the demo bars; "
                    "the next start tries again")
        return False
    os.replace(partial, target)
    return True


def year_arg(text: str) -> int:
    if not (text.isascii() and text.isdigit() and len(text) == 4):
        raise argparse.ArgumentTypeError(f"expected a year such as 2025, not {text!r}")
    return int(text)


def parse_args(argv: list[str] | None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Start Kafka, the engine, both gateways and the web console together.")
    p.add_argument("--bars", action="append",
                   help=f"CSV of bars the engine can replay (repeatable; default: the last year of {YEAR_SYMBOLS} minute "
                        f"bars, downloaded once into {YEAR_BARS} with your Alpaca keys)")
    p.add_argument("--demo", action="store_true", help=f"replay the 90-minute demo bars ({DEMO_BARS}) this time")
    p.add_argument("--year", nargs="?", const=0, type=year_arg, metavar="YYYY",
                   help=f"replay one calendar year of {YEAR_SYMBOLS} minute bars instead (downloaded once into "
                        "data/alpaca_YYYY.csv); without YYYY, the last 365 days, as by default")
    p.add_argument("--pace-ms", type=int, default=50, help="pause after each bar time so runs can be watched (default 50)")
    p.add_argument("--config", choices=["Debug", "Release", "RelWithDebInfo"],
                   help="which trading_engine build to use (default: the most recently built)")
    p.add_argument("--engine", help="path to trading_engine, overriding the search in build-kafka/ and build/")
    p.add_argument("--no-build", action="store_true", help="don't bring build-kafka up to date before starting")
    p.add_argument("--build", action="store_true", help=argparse.SUPPRESS)   # the default now; kept for old habits
    p.add_argument("--set-keys", action="store_true", help="enter (or replace) your Alpaca keys for live runs")
    p.add_argument("--forget-keys", action="store_true", help="delete the saved Alpaca keys and exit")
    p.add_argument("--gateway-port", type=int, help="engine gateway port (default 8080, or a free one if it is taken)")
    p.add_argument("--no-kafka", action="store_true", help=f"don't touch Docker; use a broker already on localhost:{KAFKA_PORT}")
    p.add_argument("--reset-kafka", action="store_true", help="delete the broker's topics (earlier runs) before starting")
    p.add_argument("--keep-kafka", action="store_true", help="leave the Kafka container running on exit")
    p.add_argument("--no-lab", action="store_true", help="don't start the Strategy Lab gateway")
    p.add_argument("--no-browser", action="store_true", help="don't open the console in a browser")
    p.add_argument("--check", action="store_true", help="check prerequisites and exit")
    return p.parse_args(argv)


def choose_bars(args: argparse.Namespace) -> tuple[list[str], bool, int | None]:
    """The bars to replay, whether they still have to be downloaded, and which year (None: the last 365 days)."""
    if sum((args.year is not None, bool(args.bars), args.demo)) > 1:
        raise LaunchError("--year, --bars and --demo each choose the bars to replay; use one")
    year = args.year or None
    if args.year is not None:
        return [str(year_file(year))], True, year
    if args.demo:
        return [str(DEMO_BARS)], False, None
    if args.bars:
        for b in args.bars:
            if not (ROOT / b).is_file() and not Path(b).is_file():
                raise LaunchError(f"--bars {b}: no such file")
        return args.bars, False, None
    bars = last_bars() or [str(YEAR_BARS)]
    return bars, bars == [str(YEAR_BARS)], None


def main(argv: list[str] | None = None) -> int:
    if WINDOWS:
        os.system("")  # turns on ANSI colours in the Windows console
    raw = sys.argv[1:] if argv is None else argv
    args = parse_args(raw)
    os.chdir(ROOT)
    if args.forget_keys:
        forget_keys()
        say("start", f"saved Alpaca keys deleted from {key_store()}")
        return 0
    if not args.check:
        try:
            code = ensure_venv(raw)
        except LaunchError as e:
            say("start", f"ERROR: {e}")
            return 1
        if code is not None:
            return code
    # Ctrl+C, closing the window (Windows) and SIGTERM all mean "stop everything", even when started in the background.
    signal.signal(signal.SIGINT, _interrupt)
    signal.signal(signal.SIGTERM, _interrupt)
    if WINDOWS:
        signal.signal(signal.SIGBREAK, _interrupt)
    procs = Processes()
    started_kafka = False
    try:
        # 1. Check everything before starting anything.
        engine = (find_engine(args.engine, args.config) if args.check
                  else prepare_engine(args.engine, args.config, build=not args.no_build))
        bars, download, year = choose_bars(args)
        pnpm = find_pnpm()
        # A taken default port moves that service to a free one; everything that connects to it is told the new one.
        run_lab = not args.no_lab
        console_port = pick_port(CONSOLE_PORT)
        gateway_port = pick_port(GATEWAY_PORT, args.gateway_port, "--gateway-port")
        lab_port = pick_port(LAB_PORT) if run_lab else LAB_PORT
        for name, default, port in (("web console", CONSOLE_PORT, console_port),
                                    ("engine gateway", GATEWAY_PORT, gateway_port),
                                    ("Strategy Lab gateway", LAB_PORT, lab_port)):
            if port != default and not (name == "engine gateway" and args.gateway_port is not None):
                say("start", f"port {default} is in use, so the {name} uses {port}")
        # The gateways only answer the console's own origins; tell them about a moved console.
        origins = []
        if console_port != CONSOLE_PORT:
            for host in ("127.0.0.1", "localhost"):
                origins += ["--allow-origin", f"http://{host}:{console_port}"]
        say("start", f"engine build: {engine.relative_to(ROOT) if engine.is_relative_to(ROOT) else engine}")
        if not args.no_kafka:
            ensure_docker()
        if args.check:
            missing = []
            try:
                import confluent_kafka  # noqa: F401
            except ImportError:
                missing.append("confluent-kafka (python package)")
            if web_deps_stale():
                missing.append("web/node_modules")
            if not all(os.environ.get(v) for v in KEY_VARS) and stored_keys() is None:
                missing.append("Alpaca keys (asked once; live runs and the year of real bars need them)")
            if missing:
                say("start", "will install on first start: " + ", ".join(missing))
            say("start", "prerequisites OK; nothing started (--check)")
            return 0
        ensure_alpaca_keys(args.set_keys)
        if download and not fetch_year(engine, year, required=args.year is not None):
            bars = [str(DEMO_BARS)]
        remember_bars(bars)
        if web_deps_stale():
            run_step("console", [pnpm, "install"], cwd=ROOT / "web")

        # 2. Kafka.
        compose = ["docker", "compose", "-f", str(COMPOSE_FILE)]
        kafka_port = KAFKA_PORT
        if not args.no_kafka:
            if args.reset_kafka:
                run_step("kafka", compose + ["down"])
            if (not args.reset_kafka and port_open(KAFKA_PORT)
                    and any(kafka_ready(KAFKA_PORT) for _ in range(3))):
                say("kafka", f"a broker is already listening on {KAFKA_PORT}; using it")
            else:
                kafka_port = pick_port(KAFKA_PORT)
                if kafka_port != KAFKA_PORT:
                    say("kafka", f"port {KAFKA_PORT} is in use by something that is not a Kafka broker, "
                                 f"so Kafka uses {kafka_port}")
                # docker-compose.kafka.yml publishes (and advertises) the broker on TE_KAFKA_PORT.
                run_step("kafka", compose + ["up", "-d"], env={"TE_KAFKA_PORT": str(kafka_port)})
                started_kafka = True
        brokers = f"localhost:{kafka_port}"
        wait_for(lambda: port_open(kafka_port), 90, f"Kafka on {brokers}")
        # The port opens seconds before the broker serves requests; the engine's CreateTopics fails until it does.
        wait_for(lambda: kafka_ready(kafka_port), 90, f"the Kafka broker on {brokers} to answer")
        say("kafka", f"broker up on {brokers}")

        # 3. Gateway, engine, lab, console.
        procs.start("gateway", [sys.executable, "python/engine_gateway/serve.py", "--port", str(gateway_port),
                                "--brokers", brokers, *origins])
        engine_id = f"start-{os.getpid()}"
        engine_cmd = [str(engine), "serve", "--pace-ms", str(args.pace_ms), "--engine-id", engine_id,
                      "--brokers", brokers]
        for b in bars:
            engine_cmd += ["--bars", b]
        procs.start("engine", engine_cmd)
        console_env = {"VITE_ENGINE_API": "/api", "ENGINE_GATEWAY": f"http://127.0.0.1:{gateway_port}"}
        if run_lab:
            lab = lab_exe(args.config) if not os.environ.get("STRATEGY_LAB_EXE") else None
            procs.start("lab", [sys.executable, "python/strategy_lab/lab_gateway.py", "--port", str(lab_port), *origins],
                        env={"STRATEGY_LAB_EXE": str(lab)} if lab else None)
            console_env["LAB_GATEWAY"] = f"http://127.0.0.1:{lab_port}"
        procs.start("console", [pnpm, "dev", "--port", str(console_port), "--strictPort"], cwd=ROOT / "web",
                    env=console_env)

        wait_for(lambda: port_open(gateway_port), 60, "the engine gateway", procs)
        # Up means this engine's heartbeat reached the gateway: before that, New backtest answers "no engine".
        wait_for(lambda: engine_announced(gateway_port, engine_id), 60, "the engine's first heartbeat", procs)
        url = f"http://127.0.0.1:{console_port}/"
        wait_for(lambda: http_ok(url), 90, "the web console", procs)

        say("start", "")
        say("start", f"Everything is up. Console: {url}  (Backtests > New backtest, or New live run)")
        say("start", f"Ports: console {console_port}, engine gateway {gateway_port}, "
                     + (f"Strategy Lab {lab_port}, " if run_lab else "") + f"Kafka {kafka_port}")
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
