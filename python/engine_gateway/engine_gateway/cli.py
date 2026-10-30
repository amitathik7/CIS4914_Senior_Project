"""Command line: `python python/engine_gateway/serve.py --brokers localhost:9092` (docs/ENGINE_GATEWAY.md)."""

from __future__ import annotations

import argparse
import sys
import threading
from pathlib import Path
from typing import Sequence

from . import sources, wire
from .commands import CommandSender, KafkaProduce
from .read_model import RunDefaults, Store
from .server import DEFAULT_ORIGINS, DEFAULT_PORT, Api, make_server


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Serve the Engine console's API from the engine's Kafka topics.")
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--brokers", default="localhost:9092", help="Kafka bootstrap servers (default localhost:9092)")
    source.add_argument("--replay", type=Path, metavar="FILE", help="read a --record file instead of Kafka")
    parser.add_argument("--topic-prefix", action="append", default=[], metavar="NAME",
                        help="only this run's topics (repeatable; default: every <prefix>.<type> topic on the cluster)")
    parser.add_argument("--control-prefix", default=wire.CONTROL_PREFIX, metavar="NAME",
                        help=f"the engines' command and heartbeat topics (default {wire.CONTROL_PREFIX})")
    parser.add_argument("--read-only", action="store_true",
                        help="never send commands: starting a backtest, stop and the kill switch are refused")
    parser.add_argument("--record", type=Path, metavar="FILE", help="also append every Kafka message to FILE (JSON lines)")
    parser.add_argument("--pace", type=float, default=0.0, metavar="SECONDS", help="with --replay: wait between messages")
    parser.add_argument("--starting-cash", default="100000", help="the run's starting cash (default the engine's, 100000)")
    parser.add_argument("--idle-seconds", type=float, default=10.0,
                        help="a run with no message for this long shows as completed (default 10)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"loopback port (default {DEFAULT_PORT})")
    parser.add_argument("--allow-origin", action="append", default=[], metavar="ORIGIN",
                        help="a browser origin allowed to call the gateway, besides the Vite dev/preview servers")
    args = parser.parse_args(argv)

    try:
        defaults = RunDefaults(starting_cash=wire.micros(args.starting_cash), idle_seconds=args.idle_seconds)
    except wire.WireError:
        parser.error("--starting-cash must be decimal text, e.g. 100000 or 2500.50")
    store = Store(defaults)
    commands = None
    if not args.replay and not args.read_only:
        try:
            commands = CommandSender(KafkaProduce(args.brokers), args.control_prefix)
        except ImportError:
            pass                                             # KafkaSource below explains how to install it
    try:
        server = make_server(Api(store, commands=commands), args.port, (*DEFAULT_ORIGINS, *args.allow_origin))
    except OSError as error:
        print(f"Cannot listen on 127.0.0.1:{args.port}: {error}. Is the gateway already running? Try --port.",
              file=sys.stderr)
        return 1

    recorder = sources.Recorder(args.record) if args.record else None
    consumer = None
    stop = threading.Event()
    if args.replay:
        if not args.replay.is_file():
            print(f"No such recording: {args.replay}", file=sys.stderr)
            return 1
        threading.Thread(target=sources.replay, args=(args.replay, store, args.pace, stop), daemon=True).start()
        where = f"recording {args.replay}"
    else:
        consumer = sources.KafkaSource(store, args.brokers, args.topic_prefix, recorder, args.control_prefix)
        consumer.start()
        where = f"Kafka at {args.brokers}, topics {sources.topic_pattern(args.topic_prefix, args.control_prefix)}"
        if commands is not None:
            where += f"; commands to {commands.topic}"

    print(f"Engine gateway reading {where}\n"
          f"Listening on http://127.0.0.1:{args.port}/v1/ (console: VITE_ENGINE_API=/api pnpm dev). Ctrl+C stops it.",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        if consumer is not None:
            consumer.stop()
        if recorder is not None:
            recorder.close()
        server.server_close()
    return 0
