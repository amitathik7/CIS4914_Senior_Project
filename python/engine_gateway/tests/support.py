"""Shared helpers for the engine gateway tests (stdlib unittest; run from python/engine_gateway).

`envelope` builds one wire message (docs/EVENT_BUS.md, schema 1) exactly as the C++ codec writes it, so the read model
can be tested on hand-written orders and fills before the risk manager and execution simulator publish real ones.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

REPO = ROOT.parents[1]
FIXTURES = REPO / "tests" / "fixtures" / "engine_gateway"

_sequence = 0


def t(minute: int, second: int = 0) -> str:
    return f"2026-01-05T14:{minute:02d}:{second:02d}.000000000Z"


def envelope(event_type: str, payload: dict[str, Any], origin: str = "0123456789abcdef",
             sequence: int | None = None) -> bytes:
    global _sequence
    _sequence += 1
    return json.dumps({"schema": 1, "origin": origin, "sequence": _sequence if sequence is None else sequence,
                       "type": event_type, "enqueued_at": payload.get("exchange_time") or payload.get("created_at")
                       or payload.get("updated_at") or payload.get("filled_at") or payload.get("decided_at"),
                       "payload": payload}).encode()


def bar(symbol: str, minute: int, price: str, volume: int = 1000, **extra: Any) -> bytes:
    return envelope("market_data", {"symbol": symbol, "exchange_time": t(minute), "ingest_time": t(minute),
                                    "type": "bar", "price": price, "volume": volume, "sequence": minute, **extra})


def signal(signal_id: int, symbol: str, side: str, minute: int, strategy: str = "sma_crossover") -> bytes:
    return envelope("signal", {"id": signal_id, "strategy_id": strategy, "symbol": symbol, "side": side,
                               "created_at": t(minute), "requested_quantity": 10,
                               "metadata": {"trigger": "short_crossed_above_long"}, "order_type": "market"})


def order(order_id: int, symbol: str, side: str, quantity: int, status: str, minute: int, filled: int = 0,
          **extra: Any) -> bytes:
    return envelope("order", {"id": order_id, "run_id": 1, "origin_signal": order_id, "strategy_id": "sma_crossover",
                              "symbol": symbol, "side": side, "type": "market", "quantity": quantity, "status": status,
                              "created_at": t(minute), "updated_at": t(minute, extra.pop("second", 0)),
                              "filled_quantity": filled, **extra})


def fill(fill_id: int, order_id: int, symbol: str, signed_quantity: int, price: str, minute: int,
         fees: str = "0", status: str = "filled", sequence: int = 1) -> bytes:
    return envelope("fill", {"id": fill_id, "order_id": order_id, "run_id": 1, "symbol": symbol,
                             "filled_quantity": signed_quantity, "fill_price": price, "fees": fees,
                             "slippage": "0.01", "filled_at": t(minute, 30), "status": status, "sequence": sequence})
