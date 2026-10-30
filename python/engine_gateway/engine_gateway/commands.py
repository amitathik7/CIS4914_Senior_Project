"""The gateway's side of the command channel (docs/adr/0007-engine-command-channel.md): it writes commands to
`<control>.command` in the event bus envelope, and an engine started with `trading_engine serve` answers each one on the
run's `<prefix>.run` topic with a report carrying the command's id.

The Kafka producer is optional like the consumer: a gateway replaying a recording has nowhere to send commands.
"""

from __future__ import annotations

import json
import secrets
import threading
from datetime import datetime, timezone
from typing import Any, Callable

from . import wire
from .read_model import utc_now

Produce = Callable[[str, str, bytes], None]       # (topic, key, value); raises when the message cannot be sent


class CommandError(Exception):
    """The command could not be handed to the broker."""


def new_run_id() -> str:
    """A fresh topic prefix for a console backtest: bt-20261030-101500-3f2a."""
    return f"bt-{datetime.now(timezone.utc):%Y%m%d-%H%M%S}-{secrets.token_hex(2)}"


class CommandSender:
    def __init__(self, produce: Produce, control_prefix: str = wire.CONTROL_PREFIX):
        self._produce = produce
        self.control_prefix = control_prefix
        self.origin = f"gateway-{secrets.token_hex(4)}"
        self._sequence = 0
        self._lock = threading.Lock()

    @property
    def topic(self) -> str:
        return f"{self.control_prefix}.{wire.COMMAND_TYPE}"

    def send(self, action: str, run_id: str, **fields: Any) -> str:
        """Writes one command and returns its id, which the engine's answer carries."""
        command_id = f"{self.origin}-{secrets.token_hex(6)}"
        now = utc_now()
        with self._lock:
            self._sequence += 1
            envelope = {"schema": wire.SCHEMA, "origin": self.origin, "sequence": self._sequence,
                        "type": wire.COMMAND_TYPE, "enqueued_at": now,
                        "payload": {"command_id": command_id, "action": action, "run_id": run_id, "issued_at": now,
                                    **fields}}
        self._produce(self.topic, run_id, json.dumps(envelope, separators=(",", ":")).encode("utf-8"))
        return command_id


class KafkaProduce:
    """`Produce` over confluent-kafka: one producer, each command flushed before the HTTP request is answered."""

    def __init__(self, brokers: str):
        from confluent_kafka import Producer             # optional dependency, as in sources.KafkaSource
        self._producer = Producer({"bootstrap.servers": brokers, "client.id": "te-engine-gateway",
                                   "acks": "all", "enable.idempotence": True, "linger.ms": 0})
        self._lock = threading.Lock()

    def __call__(self, topic: str, key: str, value: bytes) -> None:
        failure: list[str] = []

        def delivered(error: Any, _message: Any) -> None:
            if error is not None:
                failure.append(str(error))

        with self._lock:
            try:
                self._producer.produce(topic, key=key, value=value, on_delivery=delivered)
            except BufferError as error:
                raise CommandError(f"the producer queue is full: {error}") from None
            except Exception as error:                   # KafkaException: bad topic, closed producer, ...
                raise CommandError(str(error)) from None
            left = self._producer.flush(5.0)
        if left or failure:
            raise CommandError(failure[0] if failure else "Kafka did not acknowledge the command within 5 s")
