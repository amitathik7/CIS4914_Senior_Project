"""The write endpoints over the command channel (docs/adr/0007-engine-command-channel.md), against a fake engine that
answers commands the way `trading_engine serve` does: a report on `<run>.run` carrying the command's id."""

import json
import threading
import time
import unittest
import urllib.error
import urllib.request

from support import bar, envelope, fill, order

from engine_gateway import read_model
from engine_gateway.commands import CommandSender
from engine_gateway.read_model import Store
from engine_gateway.server import Api, make_server

CONFIG = {"name": "Probe", "mode": "backtest", "period_start": "2026-01-05T13:30:00Z",
          "period_end": "2026-01-05T20:00:00Z",
          "strategies": [{"kind": "sma_crossover", "strategy_id": "fast", "symbols": ["AAPL"], "requested_quantity": 5,
                          "short_window": 3, "long_window": 8}],
          "risk": {"max_order_quantity": 250, "max_position_notional": "9000", "max_gross_exposure": "40000",
                   "max_daily_loss": "777", "max_open_orders": 4, "allow_short": False},
          "simulation": {"starting_cash": "50000", "slippage_bps": 2, "fees": {"per_share": "0.01", "per_trade": "0",
                         "bps": 0}, "latency_ms": {"signal_to_order": 1, "order_to_fill": 5}, "participation_cap": 1}}


def report(run_id, status, command_id=None, engine_id="engine-1", kill=None, error=None, config=None, **extra):
    payload = {"run_id": run_id, "status": status, "updated_at": read_model.utc_now(), "config": config or CONFIG,
               "kill_switch": kill or {"engaged": False}, **extra}
    if engine_id:
        payload["engine_id"] = engine_id
    if command_id:
        payload["command_id"] = command_id
    if error:
        payload["error"] = error
    return envelope("run", payload, origin=engine_id or "cli")


def announcement(engine_id="engine-1", heartbeat_ms=2000):
    return envelope("engine", {"engine_id": engine_id, "version": "0.1.0", "started_at": read_model.utc_now(),
                               "sent_at": read_model.utc_now(), "heartbeat_ms": heartbeat_ms,
                               "market_data": {"symbols": ["AAPL", "MSFT"], "first_session": "2026-01-05",
                                               "last_session": "2026-01-06"}, "runs": []}, origin=engine_id)


class FakeEngine:
    """Answers each command on a thread, like the engine on the other side of Kafka."""

    def __init__(self, store: Store):
        self.store = store
        self.commands: list[dict] = []
        self.answer = True
        self.refuse: str | None = None

    def __call__(self, topic: str, key: str, value: bytes) -> None:
        doc = json.loads(value)
        assert topic == "trading_engine.control.command" and doc["type"] == "command" and doc["schema"] == 1
        command = doc["payload"]
        self.commands.append(command)
        if self.answer:
            threading.Thread(target=self._answer, args=(command,), daemon=True).start()

    def _answer(self, c: dict) -> None:
        time.sleep(0.05)
        run, cid = c["run_id"], c["command_id"]
        if c["action"] == "start_backtest":
            if self.refuse:
                self.store.ingest(f"{run}.run", report(run, "failed", cid, error=self.refuse, config=c["settings"]))
            else:
                self.store.ingest(f"{run}.run", report(run, "queued", cid, config=c["settings"]))
        elif c["action"] == "set_kill_switch":
            kill = {"engaged": c["engaged"], "changed_at": "2026-01-05T15:00:00.000000000Z",
                    **({"reason": c["reason"]} if c.get("engaged") and c.get("reason") else {})}
            self.store.ingest(f"{run}.run", report(run, "running", cid, kill=kill))
        elif c["action"] == "stop":
            self.store.ingest(f"{run}.run", report(run, "stopped", cid))


class Commands(unittest.TestCase):
    def setUp(self):
        self.store = Store()
        self.engine = FakeEngine(self.store)
        self.api = Api(self.store, version="9.9.9", commands=CommandSender(self.engine), answer_timeout=1.0)
        self.server = make_server(self.api, port=0)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def call(self, path, body=None, method="GET"):
        data = None if body is None else json.dumps(body).encode()
        request = urllib.request.Request(self.base + path, data=data, method=method,
                                         headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def request(self):
        return {k: v for k, v in CONFIG.items() if k != "mode"}

    def test_start_needs_an_engine(self):
        status, body = self.call("/v1/runs", self.request(), "POST")
        self.assertEqual((status, body["error"]["code"]), (503, "engine_offline"))
        self.assertIn("trading_engine serve", body["error"]["message"])
        self.assertEqual(self.engine.commands, [])

    def test_engine_info_comes_from_the_announcement(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        status, info = self.call("/v1/engine")
        self.assertEqual(info["market_data"], {"symbols": ["AAPL", "MSFT"], "first_session": "2026-01-05",
                                               "last_session": "2026-01-06"})
        self.assertEqual(info["gateway"]["engines"][0]["engine_id"], "engine-1")
        self.assertTrue(info["gateway"]["commands"])
        self.assertEqual(self.store.runs, {})                       # an engine is not a run

    def test_start_a_backtest(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        status, summary = self.call("/v1/runs", self.request(), "POST")
        self.assertEqual(status, 201, summary)
        self.assertEqual((summary["status"], summary["name"]), ("queued", "Probe"))
        self.assertTrue(summary["run_id"].startswith("bt-"))
        (command,) = self.engine.commands
        self.assertEqual(command["action"], "start_backtest")
        self.assertEqual(command["settings"]["risk"]["max_daily_loss"], "777")
        self.assertEqual(command["settings"]["mode"], "backtest")

        # The run's detail shows the settings the engine reported, not the defaults.
        status, detail = self.call(f"/v1/runs/{summary['run_id']}")
        self.assertEqual(detail["config"]["risk"]["max_open_orders"], 4)
        self.assertEqual(detail["config"]["simulation"]["starting_cash"], "50000")
        self.assertEqual(detail["strategies"], ["fast"])
        self.assertEqual(detail["symbols"], ["AAPL"])
        self.assertEqual(detail["kill_switch"], {"engaged": False})

    def test_a_refusal_names_the_field(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        self.engine.refuse = "strategies.0.long_window: The long window must be longer than the short one."
        status, body = self.call("/v1/runs", self.request(), "POST")
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["field"], "strategies.0.long_window")
        self.assertEqual(body["error"]["message"], "The long window must be longer than the short one.")
        self.engine.refuse = "No recorded bars match the strategies' symbols in that period."
        status, body = self.call("/v1/runs", self.request(), "POST")
        self.assertEqual((status, body["error"].get("field")), (400, None))
        self.assertIn("No recorded bars", body["error"]["message"])
        status, runs = self.call("/v1/runs")
        self.assertEqual(runs, [])                                  # refused starts never ran: not in the run list

    def test_no_answer_is_a_timeout(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        self.engine.answer = False
        status, body = self.call("/v1/runs", self.request(), "POST")
        self.assertEqual((status, body["error"]["code"]), (504, "engine_timeout"))

    def test_kill_switch_and_stop(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        self.store.ingest("bt-1.run", report("bt-1", "running", progress=0.4))
        status, detail = self.call("/v1/runs/bt-1/kill-switch", {"engaged": True, "reason": "too fast"}, "PUT")
        self.assertEqual(status, 200, detail)
        self.assertEqual(detail["kill_switch"], {"engaged": True, "changed_at": "2026-01-05T15:00:00.000000000Z",
                                                 "reason": "too fast"})
        self.assertEqual(self.engine.commands[-1]["reason"], "too fast")
        status, body = self.call("/v1/runs/bt-1/kill-switch", {"engaged": "yes"}, "PUT")
        self.assertEqual((status, body["error"]["code"]), (400, "invalid_request"))

        status, summary = self.call("/v1/runs/bt-1/stop", None, "POST")
        self.assertEqual((status, summary["status"]), (200, "stopped"))

    def test_a_command_line_run_cannot_be_controlled(self):
        self.store.ingest("trading_engine.control.engine", announcement())
        self.store.ingest("te.demo.run", report("te.demo", "running", engine_id=""))
        status, body = self.call("/v1/runs/te.demo/kill-switch", {"engaged": True}, "PUT")
        self.assertEqual((status, body["error"]["code"]), (409, "not_controllable"))
        self.assertIn("command line", body["error"]["message"])
        status, body = self.call("/v1/runs/te.demo/stop", None, "POST")
        self.assertEqual(status, 409)
        self.assertEqual(self.engine.commands, [])

    def test_a_silent_engine_fails_its_runs(self):
        self.store.ingest("trading_engine.control.engine", announcement(heartbeat_ms=100))
        self.store.ingest("bt-2.run", report("bt-2", "running"))
        self.assertEqual(self.store.get("bt-2").status(), "running")
        seen, a = self.store.engines["engine-1"]
        self.store.engines["engine-1"] = (seen - 60, a)
        summary = self.store.get("bt-2").summary()
        self.assertEqual(summary["status"], "failed")
        self.assertIn("stopped answering", summary["error"])
        status, body = self.call("/v1/runs/bt-2/kill-switch", {"engaged": True}, "PUT")
        self.assertEqual((status, body["error"]["code"]), (409, "not_controllable"))


class RunSettingsInTheReadModel(unittest.TestCase):
    def test_portfolio_uses_the_reported_starting_cash(self):
        store = Store()
        for topic, value in [("bt-3.run", report("bt-3", "completed", engine_id="")),
                             ("bt-3.market_data", bar("AAPL", 30, "100")),
                             ("bt-3.order", order(1, "AAPL", "buy", 10, "filled", 30, filled=10)),
                             ("bt-3.fill", fill(1, 1, "AAPL", 10, "100", 30))]:
            self.assertTrue(store.ingest(topic, value), store.last_error)
        run = store.get("bt-3")
        self.assertEqual(run.portfolio()["cash"], "49000")
        self.assertEqual(run.summary()["status"], "completed")
        self.assertEqual(run.summary()["report"]["ending_equity"], "50000")
        self.assertEqual(run.detail()["config"]["risk"]["max_daily_loss"], "777")

    def test_the_latest_report_wins(self):
        store = Store()
        store.ingest("bt-4.run", report("bt-4", "running", engine_id=""))
        newer = json.loads(report("bt-4", "completed", engine_id=""))
        older = json.loads(report("bt-4", "running", engine_id=""))
        older["payload"]["updated_at"] = "2000-01-01T00:00:00.000000Z"
        store.ingest("bt-4.run", json.dumps(newer))
        store.ingest("bt-4.run", json.dumps(older))
        self.assertEqual(store.get("bt-4").status(), "completed")


if __name__ == "__main__":
    unittest.main()
