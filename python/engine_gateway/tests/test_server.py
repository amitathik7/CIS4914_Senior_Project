"""The HTTP side: the ADR 0006 endpoints, the SSE stream, the loopback/origin checks, and --replay of a recording."""

import json
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path
from tempfile import TemporaryDirectory

from support import FIXTURES, bar, fill, order, signal

from engine_gateway import sources
from engine_gateway.read_model import Store
from engine_gateway.server import Api, make_server


class Server(unittest.TestCase):
    def setUp(self):
        self.store = Store()
        for topic, value in [("te.demo.market_data", bar("AAPL", 30, "100")), ("te.demo.signal", signal(1, "AAPL", "buy", 30)),
                             ("te.demo.order", order(1, "AAPL", "buy", 10, "filled", 30, filled=10)),
                             ("te.demo.fill", fill(1, 1, "AAPL", 10, "100", 30))]:
            self.store.ingest(topic, value)
        self.server = make_server(Api(self.store, version="9.9.9"), port=0)
        self.base = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def get(self, path, headers=None, method="GET"):
        request = urllib.request.Request(self.base + path, headers=headers or {}, method=method)
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as error:
            return error.code, json.loads(error.read())

    def test_endpoints(self):
        status, info = self.get("/v1/engine")
        self.assertEqual((status, info["data_source"], info["project_version"]), (200, "engine", "9.9.9"))
        self.assertEqual(info["market_data"]["symbols"], ["AAPL"])
        status, runs = self.get("/api/v1/runs")                                  # through the Vite proxy's path
        self.assertEqual((status, [r["run_id"] for r in runs]), (200, ["te.demo"]))
        for path, check in [("", lambda d: d["config"]["risk"]["max_open_orders"] == 50),
                            ("/portfolio", lambda d: d["cash"] == "99000"),
                            ("/equity", lambda d: len(d) == 1),
                            ("/bars?symbol=AAPL", lambda d: d[0]["price"] == "100"),
                            ("/bars?symbol=MSFT", lambda d: d == []),
                            ("/signals", lambda d: d[0]["side"] == "buy"),
                            ("/risk-decisions", lambda d: d == []),
                            ("/orders", lambda d: d[0]["history"][0]["status"] == "filled"),
                            ("/fills", lambda d: d[0]["side"] == "buy"),
                            ("/trades", lambda d: d == []),
                            ("/metrics", lambda d: d["total_events"] == 4)]:
            with self.subTest(path=path):
                status, body = self.get(f"/v1/runs/te.demo{path}")
                self.assertEqual(status, 200)
                self.assertTrue(check(body), body)

    def test_errors(self):
        status, body = self.get("/v1/runs/nope/portfolio")
        self.assertEqual((status, body["error"]["code"]), (404, "run_not_found"))
        status, body = self.get("/v1/runs", method="POST")
        self.assertEqual((status, body["error"]["code"]), (501, "not_supported"))
        self.assertIn("trading_engine run", body["error"]["message"])
        status, body = self.get("/v1/runs/te.demo/kill-switch", method="PUT")
        self.assertEqual(status, 501)
        status, body = self.get("/v1/engine", headers={"Origin": "https://evil.example"})
        self.assertEqual((status, body["error"]["code"]), (403, "origin_refused"))
        status, body = self.get("/v1/engine", headers={"Host": "evil.example:8080"})
        self.assertEqual((status, body["error"]["code"]), (403, "host_refused"))
        self.assertEqual(self.get("/v1/engine", headers={"Origin": "http://127.0.0.1:5173"})[0], 200)

    def test_stream_reports_new_events(self):
        response = urllib.request.urlopen(self.base + "/v1/runs/te.demo/stream", timeout=5)
        self.assertEqual(response.headers["Content-Type"], "text/event-stream")
        self.store.ingest("te.demo.market_data", bar("AAPL", 31, "101"))
        line = response.readline()
        while not line.startswith(b"data: "):
            line = response.readline()
        event = json.loads(line[6:])
        self.assertEqual((event["type"], event["bar"]["price"]), ("bar", "101"))
        response.close()


class Replay(unittest.TestCase):
    def test_record_then_replay(self):
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "run.jsonl"
            recorder = sources.Recorder(path)
            recorder("te.market_data", bar("AAPL", 30, "100"), 1.0)
            recorder("te.signal", signal(1, "AAPL", "buy", 30), 2.0)
            recorder.close()
            store = Store()
            self.assertEqual(sources.replay(path, store), 2)
            self.assertEqual(len(store.get("te").signals), 1)

    def test_checked_in_recording_of_a_real_engine_run(self):
        """tests/fixtures/engine_gateway/demo_run.jsonl: `trading_engine run --bus kafka` read by --record."""
        store = Store()
        sources.replay(FIXTURES / "demo_run.jsonl", store)
        self.assertEqual(store.decode_errors, 0, store.last_error)
        (run,) = store.runs.values()
        self.assertEqual(run.symbols(), ["AAPL", "MSFT"])
        self.assertEqual(run.counts["market_data"], 180)
        self.assertGreater(run.counts["signal"], 0)
        self.assertEqual(set(run.summary()["strategies"]), {s["strategy_id"] for s in run.signals})
        self.assertEqual(run.portfolio()["total_equity"], "100000")       # no fills yet: risk and execution are stubs


class TopicPattern(unittest.TestCase):
    def test_pattern(self):
        import re
        every = re.compile(sources.topic_pattern(()))
        self.assertTrue(every.match("te.demo.signal"))
        self.assertFalse(every.match("__consumer_offsets"))
        self.assertFalse(every.match("te.demo.portfolio"))
        named = re.compile(sources.topic_pattern(["te.demo"]))
        self.assertTrue(named.match("te.demo.fill"))
        self.assertFalse(named.match("te.demoX.fill"))
        self.assertFalse(named.match("other.fill"))


if __name__ == "__main__":
    unittest.main()
