"""The event bus wire format (docs/EVENT_BUS.md, "Wire format (schema 1)") as the gateway reads it.

One Kafka message is one envelope:

    {"schema":1,"origin":"9f2c...","sequence":42,"type":"signal","enqueued_at":"...Z","payload":{...}}

Money and prices are exact decimal text with at most six places. The gateway keeps them as integer millionths
("micros"), the representation of `common::Decimal`, so its arithmetic is the engine's: no float ever touches a price.
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any

SCHEMA = 1
# The four topics of the C++ bus, plus a risk decision topic the console's Risk page can use once the Risk Manager
# publishes one (payload shaped like RiskDecision in web/src/api/contract.ts).
EVENT_TYPES = ("market_data", "signal", "order", "fill", "risk_decision")
SCALE = 1_000_000

_DECIMAL = re.compile(r"-?(\d+)(?:\.(\d{1,6}))?")


class WireError(ValueError):
    """A message the gateway cannot use. It is counted and skipped, never fatal."""


@dataclass(frozen=True)
class Envelope:
    run_id: str          # the topic prefix: one prefix is one run (docs/EVENT_BUS.md)
    type: str            # one of EVENT_TYPES
    origin: str
    sequence: int
    enqueued_at: str
    payload: dict[str, Any]


def split_topic(topic: str) -> tuple[str, str] | None:
    """'te.demo.signal' -> ('te.demo', 'signal'); None when the topic is not one of the bus's."""
    prefix, _, suffix = topic.rpartition(".")
    if not prefix or suffix not in EVENT_TYPES:
        return None
    return prefix, suffix


def decode(topic: str, value: bytes | str) -> Envelope:
    parts = split_topic(topic)
    if parts is None:
        raise WireError(f"topic {topic!r} is not an event bus topic")
    run_id, topic_type = parts
    try:
        doc = json.loads(value)
    except (ValueError, UnicodeDecodeError) as error:
        raise WireError(f"not JSON: {error}") from None
    if not isinstance(doc, dict):
        raise WireError("message is not a JSON object")
    if doc.get("schema") != SCHEMA:
        raise WireError(f"schema {doc.get('schema')!r} is not {SCHEMA}")
    if doc.get("type") != topic_type:
        raise WireError(f"type {doc.get('type')!r} on topic {topic!r}")
    payload = doc.get("payload")
    origin = doc.get("origin")
    sequence = doc.get("sequence")
    enqueued_at = doc.get("enqueued_at")
    if not isinstance(payload, dict) or not isinstance(origin, str) or not isinstance(sequence, int) \
            or not isinstance(enqueued_at, str):
        raise WireError("envelope is missing origin, sequence, enqueued_at or payload")
    return Envelope(run_id, topic_type, origin, sequence, enqueued_at, payload)


# ---- exact decimals --------------------------------------------------------------------------------------------------

def micros(text: Any) -> int:
    """Exact decimal text -> integer millionths. Raises WireError on anything else (a JSON number included)."""
    if not isinstance(text, str):
        raise WireError(f"price {text!r} is not decimal text")
    match = _DECIMAL.fullmatch(text)
    if match is None:
        raise WireError(f"price {text!r} is not decimal text")
    whole, fraction = match.group(1), (match.group(2) or "")
    value = int(whole) * SCALE + int(fraction.ljust(6, "0"))
    return -value if text.startswith("-") else value


def text(value: int) -> str:
    """Integer millionths -> the engine's decimal text (`common::format_decimal`): no trailing zeros, no exponent."""
    sign = "-" if value < 0 else ""
    whole, fraction = divmod(abs(value), SCALE)
    fraction_text = f"{fraction:06d}".rstrip("0")
    return f"{sign}{whole}.{fraction_text}" if fraction_text else f"{sign}{whole}"


def divide(value: int, n: int) -> int:
    """`Decimal / int64` in the engine: rounds half away from zero."""
    q, r = divmod(abs(value), abs(n))
    if r * 2 >= abs(n):
        q += 1
    return q if (value < 0) == (n < 0) else -q
