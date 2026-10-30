"""Where the gateway's events come from: a Kafka cluster, or a recording of one.

Kafka. The gateway is one more reader of the bus, like every TopicEventBus instance: its consumer group is its own
(a fresh id per start), it reads every topic from the beginning, and it never commits offsets, so starting it late or
restarting it shows the whole run again. By default it subscribes by pattern to every `<prefix>.<type>` topic, so each
prefix (one per engine run, docs/EVENT_BUS.md) appears as a run as soon as its topics exist. `--topic-prefix` narrows
that to named prefixes.

Recording. `--record FILE` appends every message as one JSON line, {"topic", "value": <the message text>, "received_at": <epoch
seconds>}; the receive time keeps the System page's bus latencies meaningful on replay.
`--replay FILE` feeds such a file back with no broker, which is how the tests and a laptop demo run.
"""

from __future__ import annotations

import json
import re
import threading
import time
import uuid
from pathlib import Path
from typing import IO, Callable, Sequence

from . import wire
from .read_model import Store

Sink = Callable[[str, bytes, float], None]


def topic_pattern(prefixes: Sequence[str]) -> str:
    """The subscription regex (librdkafka treats a leading '^' as a pattern)."""
    types = "|".join(wire.EVENT_TYPES)
    if not prefixes:
        return rf"^[^_].*\.({types})$"                      # every prefix; skips Kafka's internal __* topics
    return rf"^({'|'.join(re.escape(p) for p in prefixes)})\.({types})$"


class Recorder:
    def __init__(self, path: Path):
        self._file: IO[str] = path.open("a", encoding="utf-8")
        self._lock = threading.Lock()

    def __call__(self, topic: str, value: bytes, received_at: float) -> None:
        line = json.dumps({"topic": topic, "value": value.decode("utf-8", "replace"), "received_at": received_at},
                          separators=(",", ":"))
        with self._lock:
            self._file.write(line + "\n")
            self._file.flush()

    def close(self) -> None:
        self._file.close()


def replay(path: Path, store: Store, pace_s: float = 0.0, stop: threading.Event | None = None) -> int:
    """Feed a recording to the store. pace_s > 0 sleeps between messages so the console can be watched filling."""
    count = 0
    with path.open(encoding="utf-8") as lines:
        for line in lines:
            if stop is not None and stop.is_set():
                break
            line = line.strip()
            if not line:
                continue
            record = json.loads(line)
            store.ingest(record["topic"], record["value"].encode("utf-8"), record.get("received_at"))
            count += 1
            if pace_s > 0:
                time.sleep(pace_s)
    store.source_status = "replayed"
    return count


class KafkaSource:
    """A background consumer thread. Needs the confluent-kafka package (librdkafka, the engine's Kafka client too)."""

    def __init__(self, store: Store, brokers: str, prefixes: Sequence[str] = (), record: Sink | None = None):
        try:
            from confluent_kafka import Consumer, KafkaError   # noqa: F401  (imported here: Kafka is optional)
        except ImportError:
            raise SystemExit(
                "engine gateway: reading Kafka needs the confluent-kafka package:\n"
                "    pip install -r python/engine_gateway/requirements.txt\n"
                "or run without a broker: --replay FILE") from None
        self._store = store
        self._record = record
        self._stop = threading.Event()
        self._pattern = topic_pattern(prefixes)
        self._consumer = Consumer({
            "bootstrap.servers": brokers,
            "group.id": f"te-engine-gateway-{uuid.uuid4().hex[:12]}",
            "auto.offset.reset": "earliest",
            "enable.auto.commit": False,
            "topic.metadata.refresh.interval.ms": 2000,     # notice a new run's topics within seconds
            "fetch.wait.max.ms": 100,
            "client.id": "te-engine-gateway",
        })
        self._thread = threading.Thread(target=self._run, name="kafka-consumer", daemon=True)

    def start(self) -> None:
        self._store.live_since = time.time() - 1.0          # runs already on the cluster are read as backlog
        self._consumer.subscribe([self._pattern])
        self._store.source_status = "connecting"
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._thread.join(timeout=5)

    def _run(self) -> None:
        from confluent_kafka import KafkaError
        try:
            while not self._stop.is_set():
                message = self._consumer.poll(0.5)
                if message is None:
                    if self._store.source_status == "connecting" and self._consumer.assignment():
                        self._store.source_status = "connected"
                    continue
                error = message.error()
                if error is not None:
                    if error.code() in (KafkaError._PARTITION_EOF, KafkaError.UNKNOWN_TOPIC_OR_PART):
                        continue
                    self._store.last_error = f"kafka: {error}"
                    if error.fatal():
                        self._store.source_status = "failed"
                        return
                    continue
                self._store.source_status = "connected"
                value, received_at = message.value() or b"", time.time()
                if self._record is not None:
                    self._record(message.topic(), value, received_at)
                self._store.ingest(message.topic(), value, received_at)
        finally:
            self._consumer.close()
