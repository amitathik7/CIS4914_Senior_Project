"""The gateway's read model: what it has seen on the bus, answered in the console's contract (web/src/api/contract.ts).

ADR 0006 asks for an events-only read model, so everything here is built from the four topics and nothing reads engine
state. Market data, signals, orders and fills are stored as they arrive. The portfolio, the equity curve and the closed
trades are DERIVED from fills and marks with the Portfolio Manager's own rules (src/portfolio/portfolio_manager.cpp):
cash moves by price x signed quantity plus fees, fees are charged to realised P&L, average cost blends on adds and is kept
on reductions, and a flip opens the remainder at the fill price. Money is integer millionths throughout.

A Store is shared by the consumer thread (which applies events) and the HTTP threads (which read): every access holds
the store's lock, and derived figures are recomputed only when the run has changed since they were last asked for.
"""

from __future__ import annotations

import math
import threading
import time
from collections import Counter, deque
from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any, Callable

from . import wire
from .wire import Envelope, WireError

OPEN_ORDER_STATUSES = ("approved", "working", "partially_filled")
LATENCY_SAMPLES = 2048                     # per topic, for the System page
BARS_PER_SESSION = 390                     # one-minute bars, for annualising volatility (ADR 0006 section 5)
HISTOGRAM_BOUNDS_US = (100, 250, 500, 1_000, 2_500, 5_000, 10_000, 25_000, 50_000, 100_000, 250_000, 1_000_000)
STAGE_FOR_TYPE = {"market_data": "ingest", "signal": "strategy", "risk_decision": "risk", "order": "execution",
                  "fill": "portfolio"}


def utc_now() -> str:
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def parse_time(text: str) -> float | None:
    """RFC 3339 UTC -> epoch seconds (microsecond precision is plenty for latency)."""
    try:
        head, _, rest = text.rstrip("Z").partition(".")
        base = datetime.strptime(head, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=timezone.utc).timestamp()
        return base + (float(f"0.{rest}") if rest else 0.0)
    except ValueError:
        return None


@dataclass
class RunDefaults:
    """What the topics do not carry. Defaults are the engine's own (app::PipelineConfig, config::default_config())."""
    starting_cash: int = 100_000 * wire.SCALE
    risk: dict[str, Any] = field(default_factory=lambda: {
        "max_order_quantity": 1000, "max_position_notional": "25000", "max_gross_exposure": "100000",
        "max_daily_loss": "5000", "max_open_orders": 50, "allow_short": False,
    })
    simulation: dict[str, Any] = field(default_factory=lambda: {
        "slippage_bps": 1, "fees": {"per_share": "0", "per_trade": "0", "bps": 0},
        "latency_ms": {"signal_to_order": 1, "order_to_fill": 5}, "participation_cap": 1,
    })
    idle_seconds: float = 10.0             # a run with no message for this long is reported as completed


@dataclass
class _Position:
    quantity: int = 0
    average_cost: int = 0
    realized: int = 0


@dataclass
class _OpenTrade:
    strategy_id: str
    entry_time: str
    realized_at_open: int
    entry_qty: int = 0
    entry_notional: int = 0
    exit_qty: int = 0
    exit_notional: int = 0
    fees: int = 0
    peak_qty: int = 0


class Run:
    """One topic prefix."""

    def __init__(self, run_id: str, defaults: RunDefaults):
        self.run_id = run_id
        self.defaults = defaults
        self.created_at = utc_now()
        self.last_message = time.monotonic()
        self.version = 0
        self.counts: Counter[str] = Counter()
        self.decode_errors = 0
        self.duplicates = 0
        self._seen: set[tuple[str, int, str]] = set()
        self.bars: dict[str, list[dict[str, Any]]] = {}
        self.marks: list[tuple[str, str, int]] = []          # (exchange_time, symbol, price) in arrival order
        self.first_time: str | None = None
        self.last_time: str | None = None
        self.signals: list[dict[str, Any]] = []
        self.strategies: dict[str, None] = {}                # insertion-ordered set
        self.risk_decisions: list[dict[str, Any]] = []
        self.orders: dict[int, dict[str, Any]] = {}
        self.fills: dict[int, dict[str, Any]] = {}
        self.latency_us: dict[str, deque[int]] = {t: deque(maxlen=LATENCY_SAMPLES) for t in wire.EVENT_TYPES}
        self.throughput: deque[tuple[int, int]] = deque(maxlen=120)   # (wall second, messages) for the System page
        self._derived: tuple[int, dict[str, Any]] | None = None

    # ---- applying events ---------------------------------------------------------------------------------------------

    def apply(self, envelope: Envelope, received_at: float, live_since: float = 0.0) -> bool:
        key = (envelope.origin, envelope.sequence, envelope.type)
        if key in self._seen:
            self.duplicates += 1                              # at-least-once delivery: a redelivered message
            return False
        handler = {"market_data": self._market_data, "signal": self._signal,
                   "order": self._order, "fill": self._fill, "risk_decision": self._risk}[envelope.type]
        handler(envelope.payload)                             # raises WireError before anything is recorded
        self._seen.add(key)
        self.counts[envelope.type] += 1
        self.last_message = time.monotonic()
        self.version += 1
        enqueued = parse_time(envelope.enqueued_at)
        if enqueued is not None and enqueued >= live_since:     # a backlog read on start-up has no meaningful delay
            self.latency_us[envelope.type].append(max(0, round((received_at - enqueued) * 1e6)))
        second = int(received_at)
        if self.throughput and self.throughput[-1][0] == second:
            self.throughput[-1] = (second, self.throughput[-1][1] + 1)
        else:
            self.throughput.append((second, 1))
        return True

    def _market_data(self, p: dict[str, Any]) -> None:
        symbol, at = _require_str(p, "symbol"), _require_str(p, "exchange_time")
        price = p.get("price")
        if price is None and "bid" in p and "ask" in p:
            price = wire.text(wire.divide(wire.micros(p["bid"]) + wire.micros(p["ask"]), 2))
        if price is None:
            return                                            # a status event: nothing to chart or mark
        price_micros = wire.micros(price)
        if p.get("type") in ("bar", "trade"):
            self.bars.setdefault(symbol, []).append({
                "symbol": symbol, "exchange_time": at,
                "open": p.get("open", price), "high": p.get("high", price), "low": p.get("low", price),
                "price": price, "volume": int(p.get("volume", p.get("size", 0))), "sequence": int(p.get("sequence", 0)),
            })
        if price_micros > 0:
            self.marks.append((at, symbol, price_micros))
        self.first_time = at if self.first_time is None or at < self.first_time else self.first_time
        self.last_time = at if self.last_time is None or at > self.last_time else self.last_time

    def _signal(self, p: dict[str, Any]) -> None:
        signal = dict(p)
        signal.setdefault("metadata", {})
        _require_str(p, "strategy_id")
        self.signals.append(signal)
        self.strategies[p["strategy_id"]] = None

    def _risk(self, p: dict[str, Any]) -> None:
        _require_str(p, "outcome")
        decision = dict(p)
        decision.setdefault("checks", [])
        self.risk_decisions.append(decision)

    def _order(self, p: dict[str, Any]) -> None:
        order_id = int(p["id"]) if isinstance(p.get("id"), int) else _fail("order id")
        status, at = _require_str(p, "status"), _require_str(p, "updated_at")
        current = self.orders.get(order_id)
        history = list(current["history"]) if current else []
        transition: dict[str, Any] = {"status": status, "at": at}
        if status == "rejected" and p.get("reject_reason"):
            transition["note"] = p["reject_reason"]
        if not history or history[-1]["status"] != status or history[-1]["at"] != at:
            history.append(transition)
            history.sort(key=lambda h: h["at"])               # stable: equal times keep arrival order
        if current is not None and current["updated_at"] > at:
            current["history"] = history                      # a late, older update: keep its step, not its state
            return
        order = dict(p)
        order["run_id"] = self.run_id
        order["history"] = history
        self.orders[order_id] = order
        self.strategies.setdefault(p.get("strategy_id", ""), None)

    def _fill(self, p: dict[str, Any]) -> None:
        fill_id = int(p["id"]) if isinstance(p.get("id"), int) else _fail("fill id")
        quantity = p.get("filled_quantity")
        if not isinstance(quantity, int) or quantity == 0:
            _fail("filled_quantity")
        for name in ("fill_price", "fees", "slippage"):
            wire.micros(p.get(name))
        if fill_id in self.fills:
            self.duplicates += 1
            return
        fill = dict(p)
        fill["run_id"] = self.run_id
        fill["side"] = "buy" if quantity > 0 else "sell"     # the engine's Fill quantity is signed (domain/fill.hpp)
        fill["filled_quantity"] = abs(quantity)
        fill["_signed"] = quantity
        self.fills[fill_id] = fill

    # ---- reading -----------------------------------------------------------------------------------------------------

    def status(self) -> str:
        return "running" if time.monotonic() - self.last_message < self.defaults.idle_seconds else "completed"

    def symbols(self) -> list[str]:
        return sorted(set(self.bars) | {s for _, s, _ in self.marks})

    def summary(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "run_id": self.run_id, "name": self.run_id, "mode": "backtest", "status": self.status(),
            "created_at": self.created_at, "period_start": self.first_time or self.created_at,
            "symbols": self.symbols(), "strategies": [s for s in self.strategies if s],
        }
        if self.last_time:
            out["period_end"] = self.last_time
            out["clock"] = self.last_time
        if out["status"] == "completed" and self.marks:
            out["report"] = self.report()
        return out

    def report(self) -> dict[str, Any]:
        """PerformanceReport with ADR 0006 section 5's definitions (as the demo engine computes it), from the derived
        equity curve and trades. It stands in for PerformanceAnalyzer, which is still a stub."""
        d = self.derived()
        start = self.defaults.starting_cash
        points = [wire.micros(p["equity"]) for p in d["equity"]]
        returns, previous = [], start
        for value in points:
            returns.append(value / previous - 1 if previous > 0 else 0.0)
            previous = value
        closes: dict[str, int] = {}
        for p in d["equity"]:
            closes[p["time"][:10]] = wire.micros(p["equity"])
        daily, previous = [], start
        for value in closes.values():
            daily.append(value / previous - 1 if previous > 0 else 0.0)
            previous = value
        pnls = [wire.micros(t["pnl"]) for t in d["trades"]]
        gross_profit = sum(v for v in pnls if v > 0)
        gross_loss = -sum(v for v in pnls if v < 0)
        ending = points[-1] if points else start
        out: dict[str, Any] = {
            "run_id": self.run_id, "generated_at": utc_now(),
            "period_start": d["equity"][0]["time"] if points else self.created_at,
            "period_end": d["equity"][-1]["time"] if points else self.created_at,
            "total_return": ending / start - 1 if start > 0 else 0.0,
            "volatility": _sample_sd(returns) * math.sqrt(252 * BARS_PER_SESSION),
            "max_drawdown": min([p["drawdown"] for p in d["equity"]] + [0.0]),
            "trade_count": len(pnls), "ending_equity": wire.text(ending), "fees_paid": wire.text(d["fees_paid"]),
        }
        if not pnls:
            out["profit_factor_status"] = "no_trades"
        elif gross_loss == 0:
            out["profit_factor_status"] = "no_losing_trades"
        else:
            out["profit_factor"] = gross_profit / gross_loss
        if pnls:
            out["win_rate"] = sum(1 for v in pnls if v > 0) / len(pnls)
        daily_sd = _sample_sd(daily)
        if len(daily) >= 3 and daily_sd > 0:
            out["sharpe"] = (sum(daily) / len(daily)) / daily_sd * math.sqrt(252)
        return out

    def detail(self) -> dict[str, Any]:
        summary = self.summary()
        # The topics carry no strategy parameters: only what the signals show (kind from the id, order size).
        sizes = {s["strategy_id"]: s["requested_quantity"] for s in reversed(self.signals) if "requested_quantity" in s}
        strategies = [{"kind": _kind(s), "strategy_id": s, "symbols": summary["symbols"],
                       "requested_quantity": sizes.get(s, 0)} for s in summary["strategies"]]
        config: dict[str, Any] = {
            "name": self.run_id, "mode": "backtest", "period_start": summary["period_start"], "strategies": strategies,
            "risk": self.defaults.risk,
            "simulation": {"starting_cash": wire.text(self.defaults.starting_cash), **self.defaults.simulation},
        }
        if "period_end" in summary:
            config["period_end"] = summary["period_end"]
        return {**summary, "config": config, "kill_switch": {"engaged": False}}

    def bars_for(self, symbol: str) -> list[dict[str, Any]]:
        return list(self.bars.get(symbol, ()))

    def order_list(self) -> list[dict[str, Any]]:
        return sorted(self.orders.values(), key=lambda o: o["id"])

    def fill_list(self) -> list[dict[str, Any]]:
        return [{k: v for k, v in f.items() if not k.startswith("_")}
                for f in sorted(self.fills.values(), key=lambda f: (f["filled_at"], f["id"]))]

    def derived(self) -> dict[str, Any]:
        if self._derived is None or self._derived[0] != self.version:
            self._derived = (self.version, self._derive())
        return self._derived[1]

    def _derive(self) -> dict[str, Any]:
        """Walk marks and fills in time order with the Portfolio Manager's accounting."""
        timeline: list[tuple[str, int, Any]] = [(at, 0, (symbol, price)) for at, symbol, price in self.marks]
        timeline += [(f["filled_at"], 1, f) for f in self.fills.values()]
        timeline.sort(key=lambda item: (item[0], item[1]))   # marks before fills at the same instant

        cash = self.defaults.starting_cash
        positions: dict[str, _Position] = {}
        marks: dict[str, int] = {}
        open_trades: dict[str, _OpenTrade] = {}
        trades: list[dict[str, Any]] = []
        equity: list[dict[str, Any]] = []
        peak = 0
        fees_paid = 0
        group_has_mark = False

        def total_equity() -> int:
            return cash + sum(marks.get(s, p.average_cost) * p.quantity for s, p in positions.items())

        for index, (at, kind, item) in enumerate(timeline):
            if kind == 0:
                marks[item[0]] = item[1]
            else:
                fill = item
                qty, price, fees = fill["_signed"], wire.micros(fill["fill_price"]), wire.micros(fill["fees"])
                fees_paid += fees
                cash -= price * qty + fees
                pos = positions.setdefault(fill["symbol"], _Position())
                pos.realized -= fees
                old, new = pos.quantity, pos.quantity + qty
                trade = open_trades.get(fill["symbol"])
                if old == 0 or (old > 0) == (qty > 0):
                    pos.average_cost = wire.divide(pos.average_cost * abs(old) + price * abs(qty), abs(new))
                    if trade is None:
                        order = self.orders.get(fill["order_id"], {})
                        trade = open_trades[fill["symbol"]] = _OpenTrade(order.get("strategy_id", ""), at,
                                                                         pos.realized + fees)
                    trade.entry_qty += abs(qty)
                    trade.entry_notional += abs(qty) * price
                    trade.fees += fees
                else:
                    closed = min(abs(qty), abs(old))
                    pos.realized += (price - pos.average_cost) * (closed if old > 0 else -closed)
                    if trade is not None:
                        trade.exit_qty += closed
                        trade.exit_notional += closed * price
                        trade.fees += fees
                    if new == 0 or (new > 0) != (old > 0):
                        if trade is not None:
                            trades.append(_closed_trade(fill["symbol"], trade, at, pos.realized))
                            del open_trades[fill["symbol"]]
                        pos.average_cost = 0 if new == 0 else price
                        if new != 0:
                            order = self.orders.get(fill["order_id"], {})
                            trade = open_trades[fill["symbol"]] = _OpenTrade(order.get("strategy_id", ""), at,
                                                                             pos.realized)
                            trade.entry_qty, trade.entry_notional = abs(new), abs(new) * price
                pos.quantity = new
                if fill["symbol"] in open_trades:
                    open_trades[fill["symbol"]].peak_qty = max(open_trades[fill["symbol"]].peak_qty, abs(new))
                marks[fill["symbol"]] = price
            # One equity point per market-data instant, after everything at that instant.
            group_has_mark = group_has_mark or kind == 0
            following = timeline[index + 1][0] if index + 1 < len(timeline) else None
            if following != at:
                if group_has_mark:
                    value = total_equity()
                    peak = max(peak, value)
                    equity.append({"time": at, "equity": wire.text(value), "cash": wire.text(cash),
                                   "drawdown": value / peak - 1 if peak > 0 else 0.0})
                group_has_mark = False

        return {"cash": cash, "positions": positions, "marks": marks, "equity": equity, "trades": trades,
                "fees_paid": fees_paid}

    def portfolio(self) -> dict[str, Any]:
        d = self.derived()
        cash, marks = d["cash"], d["marks"]
        positions, gross, net, realized, unrealized = [], 0, 0, 0, 0
        for symbol, pos in sorted(d["positions"].items()):
            mark = marks.get(symbol, pos.average_cost)
            value = mark * pos.quantity
            upnl = (mark - pos.average_cost) * pos.quantity
            net += value
            gross += abs(value)
            realized += pos.realized
            unrealized += upnl
            positions.append({"symbol": symbol, "quantity": pos.quantity, "average_cost": wire.text(pos.average_cost),
                              "realized_pnl": wire.text(pos.realized), "unrealized_pnl": wire.text(upnl),
                              "mark_price": wire.text(mark), "market_value": wire.text(value)})
        pending, reserved = [], 0
        for order in self.order_list():
            if order.get("status") not in OPEN_ORDER_STATUSES:
                continue
            remaining = int(order.get("quantity", 0)) - int(order.get("filled_quantity", 0))
            if remaining <= 0:
                continue
            hold = 0
            if order.get("side") == "buy":
                reference = order.get("limit_price")
                hold = remaining * (wire.micros(reference) if reference else marks.get(order["symbol"], 0))
            reserved += hold
            pending.append({"order_id": order["id"], "symbol": order["symbol"], "side": order["side"],
                            "remaining_quantity": remaining, "reserved_cash": wire.text(hold)})
        equity = cash + net
        session_start = self._session_start_equity(d["equity"])
        return {
            "run_id": self.run_id, "as_of": self.last_time or utc_now(), "cash": wire.text(cash),
            "total_equity": wire.text(equity), "positions": positions, "gross_exposure": wire.text(gross),
            "net_exposure": wire.text(net), "pending_orders": pending, "reserved_cash": wire.text(reserved),
            "buying_power": wire.text(cash - reserved),
            "session_pnl": wire.text(equity - (session_start if session_start is not None else equity)),
            "realized_pnl": wire.text(realized), "unrealized_pnl": wire.text(unrealized),
        }

    def _session_start_equity(self, points: list[dict[str, Any]]) -> int | None:
        if not points:
            return None
        session = points[-1]["time"][:10]                     # UTC date; US sessions do not cross midnight UTC
        first = next(p for p in points if p["time"][:10] == session)
        return wire.micros(first["equity"])

    def equity(self) -> list[dict[str, Any]]:
        return list(self.derived()["equity"])

    def trades(self) -> list[dict[str, Any]]:
        return list(self.derived()["trades"])

    def metrics(self, queue_capacity: int) -> dict[str, Any]:
        stages, histogram, all_samples = [], Counter(), []
        for event_type, stage in STAGE_FOR_TYPE.items():
            samples = sorted(self.latency_us[event_type])
            all_samples += samples
            stages.append({"stage": stage, "events": self.counts[event_type], "errors": 0,
                           "latency_us": _latency(samples)})
        for sample in all_samples:
            histogram[next((b for b in HISTOGRAM_BOUNDS_US if sample <= b), HISTOGRAM_BOUNDS_US[-1])] += 1
        history = [{"time": datetime.fromtimestamp(second, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
                    "events_per_sec": count, "queue_depth": 0, "p99_us": _latency(sorted(all_samples))["p99"]}
                   for second, count in self.throughput]
        return {
            "run_id": self.run_id, "as_of": utc_now(), "total_events": sum(self.counts.values()),
            "total_errors": self.decode_errors, "dropped_events": 0, "queue_capacity": queue_capacity,
            "stages": stages, "history": history,
            "latency_histogram": [{"upper_us": b, "count": histogram[b]} for b in HISTOGRAM_BOUNDS_US],
        }


def _closed_trade(symbol: str, t: _OpenTrade, at: str, realized: int) -> dict[str, Any]:
    return {
        "symbol": symbol, "strategy_id": t.strategy_id, "quantity": max(t.peak_qty, t.entry_qty),
        "entry_time": t.entry_time, "exit_time": at,
        "entry_price": wire.text(wire.divide(t.entry_notional, max(1, t.entry_qty))),
        "exit_price": wire.text(wire.divide(t.exit_notional, max(1, t.exit_qty))),
        "pnl": wire.text(realized - t.realized_at_open), "fees": wire.text(t.fees),
    }


def _sample_sd(values: list[float]) -> float:
    if not values:
        return 0.0
    mean = sum(values) / len(values)
    return math.sqrt(sum((v - mean) ** 2 for v in values) / max(1, len(values) - 1))


def _latency(samples: list[int]) -> dict[str, int]:
    if not samples:
        return {"count": 0, "min": 0, "p50": 0, "p99": 0, "max": 0}
    pick = lambda q: samples[min(len(samples) - 1, int(q * len(samples)))]   # noqa: E731
    return {"count": len(samples), "min": samples[0], "p50": pick(0.5), "p99": pick(0.99), "max": samples[-1]}


def _kind(strategy_id: str) -> str:
    return "mean_reversion" if "reversion" in strategy_id else "sma_crossover"


def _require_str(p: dict[str, Any], name: str) -> str:
    value = p.get(name)
    if not isinstance(value, str) or not value:
        _fail(name)
    return value


def _fail(name: str) -> Any:
    raise WireError(f"payload member {name!r} is missing or malformed")


class Store:
    """Every run the gateway has seen, keyed by topic prefix, plus the listeners the SSE streams register."""

    def __init__(self, defaults: RunDefaults | None = None, queue_capacity: int = 0):
        self.defaults = defaults or RunDefaults()
        self.queue_capacity = queue_capacity
        self.lock = threading.RLock()
        self.runs: dict[str, Run] = {}
        self.decode_errors = 0
        self.last_error: str | None = None
        self.source_status = "starting"
        self.live_since = 0.0                  # messages enqueued before this (epoch s) are backlog: no latency sample
        self._listeners: dict[str, list[Callable[[str, dict[str, Any]], None]]] = {}

    def ingest(self, topic: str, value: bytes | str, received_at: float | None = None) -> bool:
        received_at = time.time() if received_at is None else received_at
        with self.lock:
            try:
                envelope = wire.decode(topic, value)
                run = self.runs.get(envelope.run_id)
                if run is None:
                    run = self.runs[envelope.run_id] = Run(envelope.run_id, self.defaults)
                applied = run.apply(envelope, received_at, self.live_since)
            except (WireError, KeyError, TypeError, ValueError) as error:
                self.decode_errors += 1
                self.last_error = f"{topic}: {error}"
                parts = wire.split_topic(topic)
                if parts and parts[0] in self.runs:
                    self.runs[parts[0]].decode_errors += 1
                return False
            listeners = list(self._listeners.get(envelope.run_id, ()))
        if applied:
            event = _stream_event(envelope)
            for listener in listeners:
                listener(envelope.run_id, event)
        return applied

    def get(self, run_id: str) -> Run | None:
        return self.runs.get(run_id)

    def listen(self, run_id: str, listener: Callable[[str, dict[str, Any]], None]) -> Callable[[], None]:
        with self.lock:
            self._listeners.setdefault(run_id, []).append(listener)

        def remove() -> None:
            with self.lock:
                if listener in self._listeners.get(run_id, []):
                    self._listeners[run_id].remove(listener)
        return remove


def _stream_event(envelope: Envelope) -> dict[str, Any]:
    """A RunEvent for the SSE stream. The console only uses it as 'this run changed', so it carries the raw payload."""
    name, member = {"market_data": ("bar", "bar"), "signal": ("signal", "signal"), "order": ("order", "order"),
                    "fill": ("fill", "fill"), "risk_decision": ("risk", "decision")}[envelope.type]
    return {"type": name, member: envelope.payload}
