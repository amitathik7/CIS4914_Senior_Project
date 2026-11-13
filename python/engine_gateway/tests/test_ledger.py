"""The incrementally kept ledger gives exactly what a walk of the whole timeline gives, however events arrive and
whenever it is asked; and the thinned equity and bar series keep what a chart needs."""

import random
import unittest
from unittest import mock

from support import envelope

from engine_gateway import read_model, wire
from engine_gateway.read_model import RunDefaults, Store, _SeenSequences

SYMBOLS = ("AAPL", "MSFT", "SPY")


def at(day: int, minute: int, second: int = 0) -> str:
    return f"2026-03-{day:02d}T{14 + minute // 60:02d}:{minute % 60:02d}:{second:02d}.000000000Z"


def bar(symbol: str, when: str, price: str) -> tuple[str, bytes]:
    return "te.market_data", envelope("market_data", {"symbol": symbol, "exchange_time": when, "ingest_time": when,
                                                      "type": "bar", "price": price, "volume": 500, "sequence": 1})


def fill(fill_id: int, symbol: str, quantity: int, price: str, when: str) -> tuple[str, bytes]:
    return "te.fill", envelope("fill", {"id": fill_id, "order_id": fill_id, "run_id": 1, "symbol": symbol,
                                        "filled_quantity": quantity, "fill_price": price, "fees": "0.35",
                                        "slippage": "0.01", "filled_at": when, "status": "filled", "sequence": 1})


def order(order_id: int, symbol: str, when: str) -> tuple[str, bytes]:
    return "te.order", envelope("order", {"id": order_id, "run_id": 1, "origin_signal": order_id,
                                          "strategy_id": f"s{order_id % 3}", "symbol": symbol, "side": "buy",
                                          "type": "market", "quantity": 10, "status": "filled", "created_at": when,
                                          "updated_at": when, "filled_quantity": 10})


def scenario(rng: random.Random) -> list[tuple[str, bytes]]:
    """Three days of bars for three symbols, fills (some at a bar's own instant), and their orders."""
    messages, fill_id = [], 0
    for day in (2, 3, 4):
        for minute in range(0, 90):
            for symbol in SYMBOLS:
                if rng.random() < 0.9:
                    messages.append(bar(symbol, at(day, minute), f"{100 + rng.randint(-500, 500) / 100:.2f}"))
            if rng.random() < 0.25:
                fill_id += 1
                symbol = rng.choice(SYMBOLS)
                when = at(day, minute, rng.choice((0, 30)))       # second 0: the same instant as that minute's bars
                messages.append(fill(fill_id, symbol, rng.choice((-30, -10, 10, 20, 40)),
                                     f"{100 + rng.randint(-500, 500) / 100:.2f}", when))
                messages.append(order(fill_id, symbol, when))
    return messages


def reference(run: read_model.Run) -> dict:
    """The derivation the gateway did before it kept a ledger: sort everything, walk it from the start."""
    timeline = sorted(run.timeline, key=lambda e: (e[0], e[1], e[2]))
    cash, positions, marks, open_trades, trades, equity, peak = run.starting_cash(), {}, {}, {}, [], [], 0
    for index, (when, kind, _, item) in enumerate(timeline):
        if kind == 0:
            marks[item[0]] = item[1]
        else:
            qty, price, fees = item["_signed"], wire.micros(item["fill_price"]), wire.micros(item["fees"])
            cash -= price * qty + fees
            pos = positions.setdefault(item["symbol"], read_model._Position())
            pos.realized -= fees
            old, new = pos.quantity, pos.quantity + qty
            trade = open_trades.get(item["symbol"])
            if old == 0 or (old > 0) == (qty > 0):
                pos.average_cost = wire.divide(pos.average_cost * abs(old) + price * abs(qty), abs(new))
                if trade is None:
                    trade = open_trades[item["symbol"]] = read_model._OpenTrade(item["order_id"], when, pos.realized + fees)
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
                        trades.append(read_model._closed_trade(item["symbol"], trade, when, pos.realized))
                        del open_trades[item["symbol"]]
                    pos.average_cost = 0 if new == 0 else price
                    if new != 0:
                        trade = open_trades[item["symbol"]] = read_model._OpenTrade(item["order_id"], when, pos.realized)
                        trade.entry_qty, trade.entry_notional = abs(new), abs(new) * price
            pos.quantity = new
            if item["symbol"] in open_trades:
                open_trades[item["symbol"]].peak_qty = max(open_trades[item["symbol"]].peak_qty, abs(new))
            marks[item["symbol"]] = price
        following = timeline[index + 1][0] if index + 1 < len(timeline) else None
        if following != when and any(e[1] == 0 for e in timeline if e[0] == when):
            value = cash + sum(marks.get(s, p.average_cost) * p.quantity for s, p in positions.items())
            peak = max(peak, value)
            equity.append({"time": when, "equity": wire.text(value), "cash": wire.text(cash),
                           "drawdown": value / peak - 1 if peak > 0 else 0.0})
    return {"cash": cash, "equity": equity, "trades": [t["pnl"] for t in trades],
            "positions": {s: (p.quantity, p.average_cost, p.realized) for s, p in positions.items()}}


def ledger_view(run: read_model.Run) -> dict:
    ledger = run.ledger()
    return {"cash": ledger.cash, "equity": run.equity(), "trades": [t["pnl"] for t in run.trades()],
            "positions": {s: (p.quantity, p.average_cost, p.realized) for s, p in ledger.positions.items()}}


class LedgerMatchesAFullWalk(unittest.TestCase):
    def check(self, seed: int, deliver) -> None:
        rng = random.Random(seed)
        messages = deliver(scenario(rng), rng)
        store = Store(RunDefaults(starting_cash=wire.micros("100000")))
        for i, (topic, value) in enumerate(messages):
            store.ingest(topic, value)
            if rng.random() < 0.05:
                store.get("te").ledger()                       # read mid-stream, as the console does
        run = store.get("te")
        self.assertEqual(ledger_view(run), reference(run))
        self.assertTrue(all(t["strategy_id"] for t in run.trades()))   # orders that came after their fill still count

    def test_in_time_order(self):
        with mock.patch.object(read_model, "CHECKPOINT_EVERY", 7):
            for seed in range(5):
                self.check(seed, lambda m, rng: m)

    def test_topics_delivered_in_batches(self):
        """Kafka hands over each topic's backlog in chunks, so fills can land long after the bars around them."""
        def batches(messages, rng):
            by_topic: dict[str, list] = {}
            for m in messages:
                by_topic.setdefault(m[0], []).append(m)
            out = []
            while any(by_topic.values()):
                topic = rng.choice([t for t, q in by_topic.items() if q])
                take = rng.randint(1, 40)
                out += by_topic[topic][:take]
                del by_topic[topic][:take]
            return out
        with mock.patch.object(read_model, "CHECKPOINT_EVERY", 7):
            for seed in range(5):
                self.check(seed, batches)

    def test_fully_shuffled(self):
        with mock.patch.object(read_model, "CHECKPOINT_EVERY", 11):
            for seed in range(3):
                self.check(seed, lambda m, rng: rng.sample(m, len(m)))


class Thinning(unittest.TestCase):
    def store(self) -> read_model.Run:
        store = Store(RunDefaults(starting_cash=wire.micros("100000")))
        messages = []
        for day in (2, 3, 4):
            for minute in range(90):
                messages.append(bar("AAPL", at(day, minute), f"{100 + (minute % 7) - (3 if minute == 40 else 0)}"))
        messages.append(fill(1, "AAPL", 100, "100", at(2, 0, 30)))
        for topic, value in messages:
            store.ingest(topic, value)
        return store.get("te")

    def test_equity_keeps_the_last_session_and_every_low(self):
        run = self.store()
        full = run.equity()
        thin = run.equity(points=20)
        last_day = [p for p in full if p["time"].startswith("2026-03-04")]
        self.assertEqual(thin[-len(last_day):], last_day)                    # the last session as it was
        older = thin[:-len(last_day)]
        self.assertLessEqual(len(older), 20)
        self.assertEqual(min(wire.micros(p["equity"]) for p in older),
                         min(wire.micros(p["equity"]) for p in full if not p["time"].startswith("2026-03-04")))
        self.assertEqual(older, sorted(older, key=lambda p: p["time"]))
        self.assertEqual(run.equity(points=10_000), full)

    def test_bars_merge_older_sessions_into_candles(self):
        run = self.store()
        full = run.bars_for("AAPL")
        thin = run.bars_for("AAPL", points=30)
        last_day = [b for b in full if b["exchange_time"].startswith("2026-03-04")]
        self.assertEqual(thin[-len(last_day):], last_day)
        older, full_older = thin[:-len(last_day)], full[:-len(last_day)]
        self.assertLessEqual(len(older), 30)
        self.assertEqual(sum(b["volume"] for b in older), sum(b["volume"] for b in full_older))
        self.assertEqual(min(wire.micros(b["low"]) for b in older), min(wire.micros(b["low"]) for b in full_older))
        self.assertEqual((older[0]["open"], older[-1]["price"]), (full_older[0]["open"], full_older[-1]["price"]))


class SeenSequences(unittest.TestCase):
    def test_out_of_order_and_redelivered(self):
        seen = _SeenSequences()
        for sequence in (500, 3, 900, 4, 501):
            self.assertTrue(seen.add(sequence))
        for sequence in (500, 3, 900, 4, 501):
            self.assertTrue(seen.has(sequence))
            self.assertFalse(seen.add(sequence))
        self.assertFalse(seen.has(2))
        self.assertFalse(seen.has(901))


if __name__ == "__main__":
    unittest.main()
