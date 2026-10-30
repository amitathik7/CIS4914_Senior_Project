"""The read model: stored events, and the portfolio, equity and trades derived from fills with the engine's rules."""

import unittest

from support import bar, fill, order, signal

from engine_gateway import wire
from engine_gateway.read_model import RunDefaults, Store


def store_with(*messages: tuple[str, bytes]) -> Store:
    store = Store(RunDefaults(starting_cash=wire.micros("10000")))
    for topic, value in messages:
        store.ingest(topic, value)
    return store


class MarketDataAndSignals(unittest.TestCase):
    def test_bars_marks_and_run_summary(self):
        store = store_with(("te.market_data", bar("AAPL", 30, "192.50")), ("te.market_data", bar("MSFT", 30, "412.5")),
                           ("te.market_data", bar("AAPL", 31, "190.83", open="192", high="193", low="190")),
                           ("te.signal", signal(1, "AAPL", "buy", 31)))
        run = store.get("te")
        aapl = run.bars_for("AAPL")
        self.assertEqual([b["price"] for b in aapl], ["192.50", "190.83"])
        self.assertEqual(aapl[0]["open"], "192.50")                      # a close-only bar charts as a flat candle
        self.assertEqual((aapl[1]["open"], aapl[1]["high"], aapl[1]["low"]), ("192", "193", "190"))
        summary = run.summary()
        self.assertEqual(summary["symbols"], ["AAPL", "MSFT"])
        self.assertEqual(summary["strategies"], ["sma_crossover"])
        self.assertEqual(summary["status"], "running")
        self.assertEqual((summary["period_start"], summary["clock"]), (aapl[0]["exchange_time"], aapl[1]["exchange_time"]))
        self.assertEqual(run.signals[0]["metadata"]["trigger"], "short_crossed_above_long")
        self.assertEqual(run.detail()["config"]["simulation"]["starting_cash"], "10000")

    def test_one_prefix_is_one_run(self):
        store = store_with(("a.market_data", bar("AAPL", 30, "1")), ("b.market_data", bar("AAPL", 30, "2")))
        self.assertEqual(sorted(store.runs), ["a", "b"])

    def test_redelivery_and_bad_messages_are_counted_not_applied(self):
        message = bar("AAPL", 30, "1")
        store = store_with(("te.market_data", message), ("te.market_data", message),
                           ("te.market_data", b"not json"), ("te.market_data", bar("AAPL", 31, "1.1234567")))
        run = store.get("te")
        self.assertEqual(len(run.bars_for("AAPL")), 1)
        self.assertEqual(run.duplicates, 1)
        self.assertEqual(store.decode_errors, 2)
        self.assertIn("te.market_data", store.last_error)

    def test_no_market_data_no_equity(self):
        store = store_with(("te.signal", signal(1, "AAPL", "buy", 31)))
        run = store.get("te")
        self.assertEqual(run.equity(), [])
        self.assertEqual(run.portfolio()["total_equity"], "10000")
        self.assertEqual(run.portfolio()["session_pnl"], "0")


class Orders(unittest.TestCase):
    def test_history_from_successive_order_events(self):
        store = store_with(("te.order", order(7, "AAPL", "buy", 10, "approved", 31)),
                           ("te.order", order(7, "AAPL", "buy", 10, "working", 31, second=1)),
                           ("te.order", order(7, "AAPL", "buy", 10, "filled", 31, filled=10, second=30,
                                              average_fill_price="190.84")))
        (o,) = store.get("te").order_list()
        self.assertEqual(o["status"], "filled")
        self.assertEqual(o["run_id"], "te")
        self.assertEqual([h["status"] for h in o["history"]], ["approved", "working", "filled"])

    def test_a_late_older_update_adds_history_but_not_state(self):
        store = store_with(("te.order", order(7, "AAPL", "buy", 10, "filled", 31, filled=10, second=30)),
                           ("te.order", order(7, "AAPL", "buy", 10, "working", 31, second=1)))
        (o,) = store.get("te").order_list()
        self.assertEqual(o["status"], "filled")
        self.assertEqual([h["status"] for h in o["history"]], ["working", "filled"])

    def test_rejection_note(self):
        store = store_with(("te.order", order(8, "AAPL", "buy", 10, "rejected", 31, reject_reason="max_order_quantity")))
        self.assertEqual(store.get("te").order_list()[0]["history"][0]["note"], "max_order_quantity")


class RiskDecisions(unittest.TestCase):
    def test_risk_decision_topic(self):
        from support import envelope
        decision = {"id": 1, "signal_id": 1, "decided_at": "2026-01-05T14:31:00.000000000Z", "symbol": "AAPL",
                    "side": "buy", "strategy_id": "sma_crossover", "outcome": "rejected", "requested_quantity": 10,
                    "approved_quantity": 0, "reason": "max_order_quantity"}
        store = store_with(("te.risk_decision", envelope("risk_decision", decision)))
        (d,) = store.get("te").risk_decisions
        self.assertEqual((d["outcome"], d["checks"]), ("rejected", []))


class Accounting(unittest.TestCase):
    """Figures checked by hand against PortfolioManager::apply (src/portfolio/portfolio_manager.cpp)."""

    def run_of(self):
        return store_with(
            ('te.signal', signal(9, 'AAPL', 'buy', 30)),
            ("te.market_data", bar("AAPL", 30, "100")),
            ("te.order", order(1, "AAPL", "buy", 10, "filled", 30, filled=10)),
            ("te.fill", fill(1, 1, "AAPL", 10, "100", 30, fees="1")),          # buy 10 @ 100, fee 1
            ("te.market_data", bar("AAPL", 31, "103")),
            ("te.order", order(2, "AAPL", "buy", 5, "filled", 31, filled=5)),
            ("te.fill", fill(2, 2, "AAPL", 5, "103", 31, fees="1")),           # add 5 @ 103: avg (1000+515)/15 = 101
            ("te.market_data", bar("AAPL", 32, "104")),
            ("te.order", order(3, "AAPL", "sell", 15, "filled", 32, filled=15)),
            ("te.fill", fill(3, 3, "AAPL", -15, "104", 32, fees="1")),         # close: (104-101)*15 = 45
            ("te.market_data", bar("AAPL", 33, "105")),
        ).get("te")

    def test_round_trip(self):
        run = self.run_of()
        p = run.portfolio()
        self.assertEqual(p["cash"], "10042")                               # 10000 - 1001 - 516 + 1559
        self.assertEqual(p["realized_pnl"], "42")                          # 45 - 3 fees
        self.assertEqual((p["unrealized_pnl"], p["net_exposure"], p["total_equity"]), ("0", "0", "10042"))
        (position,) = p["positions"]
        self.assertEqual((position["quantity"], position["average_cost"]), (0, "0"))
        (trade,) = run.trades()
        self.assertEqual(trade, {"symbol": "AAPL", "strategy_id": "sma_crossover", "quantity": 15,
                                 "entry_time": run.fill_list()[0]["filled_at"], "exit_time": run.fill_list()[2]["filled_at"],
                                 "entry_price": "101", "exit_price": "104", "pnl": "42", "fees": "3"})
        fills = run.fill_list()
        self.assertEqual([(f["side"], f["filled_quantity"]) for f in fills], [("buy", 10), ("buy", 5), ("sell", 15)])
        self.assertNotIn("_signed", fills[0])

    def test_report_once_the_run_goes_quiet(self):
        run = self.run_of()
        self.assertNotIn("report", run.summary())                        # still running
        run.defaults.idle_seconds = 0
        r = run.summary()["report"]
        self.assertEqual((r["trade_count"], r["win_rate"], r["profit_factor_status"]), (1, 1.0, "no_losing_trades"))
        self.assertEqual((r["ending_equity"], r["fees_paid"]), ("10042", "3"))
        self.assertAlmostEqual(r["total_return"], 0.0042)
        self.assertLess(r["max_drawdown"], 0)                           # 10043 -> 10042
        self.assertNotIn("sharpe", r)                                   # one session

    def test_equity_curve_one_point_per_bar(self):
        equity = self.run_of().equity()
        self.assertEqual([e["equity"] for e in equity], ["10000", "10029", "10043", "10042"])
        # Each point is taken at the bar, before that minute's fill at :30. 14:31: 8999 cash + 10 x 103;
        # 14:32: 8483 cash + 15 x 104; 14:33: flat.
        self.assertEqual(equity[1]["cash"], "8999")
        self.assertTrue(all(e["drawdown"] <= 0 for e in equity))

    def test_open_position_and_drawdown(self):
        run = store_with(("te.market_data", bar("AAPL", 30, "100")),
                         ("te.fill", fill(1, 1, "AAPL", 10, "100", 30)),
                         ("te.market_data", bar("AAPL", 31, "90")),
                         ("te.order", order(2, "AAPL", "buy", 4, "working", 31, filled=1)),
                         ("te.order", order(3, "AAPL", "sell", 2, "working", 31))).get("te")
        p = run.portfolio()
        self.assertEqual((p["unrealized_pnl"], p["total_equity"], p["gross_exposure"]), ("-100", "9900", "900"))
        self.assertEqual(p["pending_orders"], [
            {"order_id": 2, "symbol": "AAPL", "side": "buy", "remaining_quantity": 3, "reserved_cash": "270"},
            {"order_id": 3, "symbol": "AAPL", "side": "sell", "remaining_quantity": 2, "reserved_cash": "0"}])
        self.assertEqual((p["reserved_cash"], p["buying_power"]), ("270", "8730"))
        self.assertAlmostEqual(run.equity()[-1]["drawdown"], 9900 / 10000 - 1)
        self.assertEqual(run.trades(), [])

    def test_flip_opens_the_remainder_at_the_fill_price(self):
        run = store_with(("te.fill", fill(1, 1, "AAPL", 10, "100", 30)),
                         ("te.fill", fill(2, 2, "AAPL", -15, "110", 31))).get("te")
        (position,) = run.portfolio()["positions"]
        self.assertEqual((position["quantity"], position["average_cost"], position["realized_pnl"]), (-5, "110", "100"))
        (trade,) = run.trades()
        self.assertEqual((trade["quantity"], trade["pnl"], trade["exit_price"]), (10, "100", "110"))

    def test_duplicate_fill_id_applies_once(self):
        store = store_with(("te.fill", fill(1, 1, "AAPL", 10, "100", 30)), ("te.fill", fill(1, 1, "AAPL", 10, "100", 30)))
        self.assertEqual(store.get("te").portfolio()["cash"], "9000")


class Metrics(unittest.TestCase):
    def test_counts_by_stage(self):
        store = store_with(("te.market_data", bar("AAPL", 30, "1")), ("te.market_data", bar("AAPL", 31, "1")),
                           ("te.signal", signal(1, "AAPL", "buy", 31)))
        m = store.get("te").metrics(0)
        self.assertEqual({s["stage"]: s["events"] for s in m["stages"]},
                         {"ingest": 2, "strategy": 1, "risk": 0, "execution": 0, "portfolio": 0})
        self.assertEqual(m["total_events"], 3)
        self.assertEqual(sum(b["count"] for b in m["latency_histogram"]), 3)

    def test_backlog_has_no_latency_samples(self):
        store = Store()
        store.live_since = 10**10                                       # everything was enqueued before the gateway started
        store.ingest("te.market_data", bar("AAPL", 30, "1"))
        self.assertEqual(store.get("te").metrics(0)["stages"][0]["latency_us"]["count"], 0)


if __name__ == "__main__":
    unittest.main()
