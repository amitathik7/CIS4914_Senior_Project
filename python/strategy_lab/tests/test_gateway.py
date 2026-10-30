"""The Engine console's gateway: request validation at the boundary, byte-for-byte pass-through of the tool's JSON, the HTTP guard
rails (host, origin, content type, size, concurrency), and a round trip through the REAL strategy_lab_replay (skipped, with a
reason, if it has not been built).

Run from python/strategy_lab:  .venv\\Scripts\\python.exe -m unittest discover -s tests   (the gateway itself is stdlib only).
"""

import base64
import http.client
import json
import threading
import unittest
from pathlib import Path
from unittest import mock

from support import FIXTURES, ROOT, real_runner

from strategy_lab_ui import bridge, datasets, gateway, scenarios
from strategy_lab_ui.errors import LabUiError

INVALID = FIXTURES.parent / "invalid"
SMA = {"kind": "sma_crossover", "params": [["short_window", "2"], ["long_window", "3"], ["symbols", "AAPL"]]}


def split_replay(response: gateway.Response) -> tuple[dict, bytes]:
    """Line 1 is the gateway's metadata; the rest is the replay tool's stdout, untouched."""
    head, _, rest = response.body.partition(b"\n")
    return json.loads(head), rest


class RealRunnerCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()
        cls.gateway = gateway.Gateway(lambda: cls.runner)

    def error_of(self, response: gateway.Response) -> dict:
        return json.loads(response.body)["error"]


class StatusCatalogDatasets(RealRunnerCase):
    def test_status_reports_the_runner_identity(self):
        body = json.loads(self.gateway.status().body)
        self.assertEqual(body["runner"]["state"], "ready")
        self.assertEqual(body["runner"]["sha256"], self.runner.sha256)
        self.assertEqual(body["runner"]["tool"], "strategy_lab_replay")
        self.assertEqual(body["runner"]["max_rows_used"], bridge.UI_MAX_ROWS)
        self.assertNotIn("\\", body["runner"]["name"])             # a file name, never a path

    def test_catalog_is_the_tools_own_document_exactly(self):
        _, expected = bridge.describe_with_output(self.runner)
        response = self.gateway.catalog()
        self.assertEqual(response.status, 200)
        self.assertEqual(response.body, expected)

    def test_datasets_list_every_fixture_with_identity_and_symbols(self):
        body = json.loads(self.gateway.datasets().body)
        self.assertEqual([d["key"] for d in body["datasets"]], [b.key for b in datasets.BUILTINS])
        crossover = next(d for d in body["datasets"] if d["key"] == "sma_crossover")
        data = (FIXTURES / "sma_crossover.csv").read_bytes()
        self.assertEqual(crossover["sha256"], datasets.sha256_hex(data))
        self.assertEqual(crossover["bytes"], len(data))
        self.assertEqual(crossover["symbols"], ["AAPL"])
        self.assertTrue(crossover["synthetic"])
        self.assertEqual(crossover["presets"]["sma_crossover"]["long_window"], "3")


class Check(RealRunnerCase):
    def check(self, kind, params):
        return self.gateway.check({"strategy": {"kind": kind, "params": params}})

    def test_an_acceptable_configuration_returns_what_the_tool_derived(self):
        response = self.check("sma_crossover", SMA["params"])
        body = json.loads(response.body)
        self.assertEqual((response.status, body["ok"]), (200, True))
        self.assertEqual(body["derived"]["earliest_signal_accepted_bar"], 4)
        self.assertEqual(body["window_size"], 3)

    def test_the_constructors_message_comes_back_verbatim_with_the_fields_it_names(self):
        response = self.check("sma_crossover", [["short_window", "5"], ["long_window", "3"], ["symbols", "AAPL"]])
        body = json.loads(response.body)
        error = body["error"]
        self.assertEqual((response.status, body["ok"]), (200, False))   # a refusal is a successful check, not a failed request
        self.assertEqual(error["code"], "runner_rejected")
        self.assertIn("long_window must be greater than short_window", error["message"])
        self.assertEqual(error["fields"], ["long_window", "short_window"])

    def test_a_parse_level_rejection_names_its_one_parameter(self):
        error = self.error_of(self.check("sma_crossover", [["short_window", "-1"], ["symbols", "AAPL"]]))
        self.assertEqual(error["field"], "short_window")
        self.assertIn("expected an unsigned whole number", error["message"])

    def test_an_unreachable_entry_threshold_is_a_warning_not_an_error(self):
        body = json.loads(self.check("mean_reversion", [["lookback", "20"], ["entry_threshold", "5"], ["symbols", "AAPL"]]).body)
        self.assertEqual([w["code"] for w in body["warnings"]], ["entry_threshold_unreachable"])
        self.assertIs(body["derived"]["entry_threshold_reachable"], False)

    def test_warnings_about_the_probe_row_are_not_reported(self):
        body = json.loads(self.check("sma_crossover", [["short_window", "5"], ["long_window", "20"], ["symbols", "ZZZ"]]).body)
        self.assertEqual(body["warnings"], [])


class Replay(RealRunnerCase):
    def replay(self, strategy=SMA, dataset=None):
        return self.gateway.replay({"strategy": strategy, "dataset": dataset or {"builtin": "sma_crossover"}})

    def test_a_builtin_replay_carries_identity_and_the_real_documents_signals(self):
        response = self.replay()
        meta, raw = split_replay(response)
        self.assertEqual(response.status, 200)
        self.assertEqual(response.content_type, "application/x-ndjson; charset=utf-8")
        self.assertEqual(meta["dataset"]["sha256"], datasets.sha256_hex((FIXTURES / "sma_crossover.csv").read_bytes()))
        self.assertEqual(meta["runner"]["sha256"], self.runner.sha256)
        self.assertEqual(meta["request"]["params"], [list(p) for p in SMA["params"]])
        document = json.loads(raw)
        self.assertEqual([(s["event_index"], s["side"]) for s in document["result"]["signals"]], [(4, "buy"), (7, "sell")])
        self.assertTrue(raw.endswith(b"}\n"))                               # the tool's own trailing newline is kept

    def test_an_upload_is_run_through_the_tools_own_parser(self):
        data = (FIXTURES / "mr_rearm.csv").read_bytes()
        upload = {"name": "mine.csv", "csv_base64": base64.b64encode(data).decode()}
        strategy = {"kind": "mean_reversion", "params": [["lookback", "4"], ["entry_threshold", "1.5"], ["symbols", "AAPL"]]}
        meta, raw = split_replay(self.replay(strategy, upload))
        self.assertEqual(meta["dataset"]["kind"], "upload")
        self.assertFalse(meta["dataset"]["synthetic"])
        self.assertEqual(meta["dataset"]["sha256"], datasets.sha256_hex(data))
        self.assertEqual(len(json.loads(raw)["result"]["signals"]), 3)

    def test_an_invalid_file_reports_line_and_column_from_the_tool(self):
        data = (INVALID / "bad_price_text.csv").read_bytes()
        response = self.replay(dataset={"name": "bad.csv", "csv_base64": base64.b64encode(data).decode()})
        error = self.error_of(response)
        self.assertEqual((response.status, error["code"]), (422, "dataset_invalid"))
        self.assertTrue(error["problems"] and error["problems"][0]["line"] and error["problems"][0]["column"])

    def test_a_rejected_configuration_is_a_422_with_the_message_verbatim(self):
        bad = {"kind": "mean_reversion", "params": [["lookback", "1"], ["symbols", "AAPL"]]}
        error = self.error_of(self.replay(bad))
        self.assertIn("lookback must be at least 2", error["message"])
        self.assertEqual(error["fields"], ["lookback"])

    def test_exact_edge_prices_pass_through_unchanged(self):
        meta, raw = split_replay(self.replay({"kind": "sma_crossover", "params": [["short_window", "1"], ["long_window", "2"],
                                                                                 ["symbols", "AAPL"]]},
                                             {"builtin": "ohlcv_example"}))
        self.assertEqual(meta["dataset"]["name"], "ohlcv_example.csv")
        self.assertIn(b'"price":', raw)


class Validation(RealRunnerCase):
    """The Lab's own scenario checks, offered to the console. They run only on an explicit POST /validate."""

    @staticmethod
    def never_called():
        raise AssertionError("the executable must not be looked for")

    def test_the_scenario_list_runs_nothing_and_needs_no_executable(self):
        body = json.loads(gateway.Gateway(self.never_called).validation_info().body)
        expected = scenarios.load_scenarios()
        self.assertGreater(len(expected.scenarios), 0)
        self.assertEqual([s["id"] for s in body["scenarios"]], [s.id for s in expected.scenarios])        # the count is the files'
        self.assertEqual(body["scenario_set"]["identity_sha256"], expected.identity)
        self.assertEqual(body["scenario_set"]["files"], list(expected.files))
        self.assertEqual(body["scenario_set"]["directory"], "tests/fixtures/strategy_lab/scenarios")
        self.assertIn("NOT the C++ CTest/GoogleTest suite", body["scope_note"])
        self.assertTrue(body["demonstration_available"])

    def test_validate_runs_every_scenario_through_the_real_executable_and_names_what_it_ran(self):
        response = self.gateway.validate({})
        self.assertEqual(response.status, 200)
        report = json.loads(response.body)
        expected = scenarios.load_scenarios()
        summary = report["summary"]
        self.assertEqual((summary["scenarios"], summary["passed"], summary["failed"], summary["error"]), (len(expected.scenarios), len(expected.scenarios), 0, 0))
        self.assertEqual(report["report"], "strategy_lab.validation_report")
        self.assertEqual(report["executable"]["sha256"], self.runner.sha256)
        self.assertEqual(report["scenario_files"]["identity_sha256"], expected.identity)
        self.assertLessEqual(report["started_at"], report["finished_at"])
        self.assertIn("NOT the C++ CTest/GoogleTest suite", report["scope_note"])
        self.assertFalse(report["demonstration"]["included"])
        for entry in report["scenarios"]:
            self.assertEqual(entry["executable_sha256"], self.runner.sha256)
            self.assertRegex(entry["dataset"]["sha256_given_to_tool"], r"^[0-9a-f]{64}$")
            self.assertTrue(entry["finished_at"])

    def test_the_opt_in_demonstration_shows_expected_against_actual_and_is_not_counted(self):
        report = json.loads(self.gateway.validate({"include_demonstration": True}).body)
        demo = [e for e in report["scenarios"] if e["demonstration"]]
        self.assertEqual(len(demo), 1)
        self.assertEqual(demo[0]["status"], "failed")                                  # it must fail: that is what it demonstrates
        self.assertTrue(demo[0]["mismatches"])
        for mismatch in demo[0]["mismatches"]:
            self.assertTrue(mismatch["where"] and mismatch["expected"] and mismatch["actual"])
        self.assertEqual(report["summary"]["failed"], 0)                                # not mixed into the totals
        self.assertTrue(report["demonstration"]["results"][0]["failed_as_designed"])

    def test_each_validation_is_numbered_since_the_gateway_started(self):
        g = gateway.Gateway(lambda: self.runner)
        numbers = [json.loads(g.validate({}).body)["validation_number_in_session"] for _ in range(2)]
        self.assertEqual(numbers, [1, 2])

    def test_a_body_that_asks_for_anything_else_is_refused_before_anything_runs(self):
        g = gateway.Gateway(self.never_called)
        for body in ({"scenario": "x"}, {"include_demonstration": "yes"}, [], "run"):
            with self.assertRaises(LabUiError) as caught:
                g.validate(body)
            self.assertEqual(caught.exception.kind, "bad_request", body)

    def test_a_missing_executable_is_a_503_with_nothing_run(self):
        def provider():
            raise LabUiError("executable_missing", "strategy_lab_replay has not been built.")

        with self.assertRaises(LabUiError) as caught:
            gateway.Gateway(provider).validate({})
        self.assertEqual(gateway.error_response(caught.exception).status, 503)


class Boundary(unittest.TestCase):
    """Validation that needs no executable: the catalog is a small stand-in."""

    def setUp(self):
        self.catalog = mock.Mock()
        spec = mock.Mock(kind="sma_crossover", available=True, unavailable_reason="")
        spec.params = [mock.Mock() for _ in range(5)]
        for param, name in zip(spec.params, ("strategy_id", "short_window", "long_window", "requested_quantity", "symbols")):
            param.name = name
        ml = mock.Mock(kind="ml", available=False, unavailable_reason="Not implemented", params=[])
        self.catalog.strategy = lambda kind: {"sma_crossover": spec, "ml": ml}.get(kind)
        self.catalog.strategies = [spec, ml]

    def rejected(self, raw):
        with self.assertRaises(LabUiError) as caught:
            gateway.parse_strategy(raw, self.catalog)
        self.assertEqual(caught.exception.kind, "bad_request")
        return caught.exception.message

    def test_unknown_kind_unknown_parameter_and_duplicates_are_refused(self):
        self.assertIn("Unknown strategy kind", self.rejected({"kind": "momentum", "params": []}))
        self.assertIn("'bogus' is not a parameter", self.rejected({"kind": "sma_crossover", "params": [["bogus", "1"]]}))
        self.assertIn("given twice", self.rejected({"kind": "sma_crossover", "params": [["short_window", "1"], ["short_window", "2"]]}))

    def test_the_unavailable_ml_strategy_cannot_be_requested(self):
        self.assertIn("Not implemented", self.rejected({"kind": "ml", "params": []}))

    def test_values_must_be_text_and_bounded(self):
        self.assertIn("pair of strings", self.rejected({"kind": "sma_crossover", "params": [["short_window", 2]]}))
        self.assertIn("longer than", self.rejected({"kind": "sma_crossover", "params": [["symbols", "A" * 5000]]}))
        self.assertIn("only", self.rejected({"kind": "sma_crossover", "params": [], "path": "x"}))

    def test_a_dataset_is_a_key_or_bytes_never_a_path(self):
        for bad in ({"path": "C:/secret.csv"}, {"builtin": "../../secret"}, {"builtin": 3}, [], {"name": "x", "csv_base64": 4}):
            with self.assertRaises(LabUiError) as caught:
                gateway.parse_dataset(bad)
            self.assertEqual(caught.exception.kind, "bad_request", bad)
        with self.assertRaises(LabUiError) as caught:
            gateway.parse_dataset({"name": "x.csv", "csv_base64": "***not base64***"})
        self.assertIn("base64", caught.exception.message)

    def test_an_empty_or_binary_upload_is_refused_before_any_process_starts(self):
        for payload, kind in ((b"", "input_empty"), (b"a,b\n\x00\x01", "input_not_text")):
            with self.assertRaises(LabUiError) as caught:
                gateway.parse_dataset({"name": "x.csv", "csv_base64": base64.b64encode(payload).decode()})
            self.assertEqual(caught.exception.kind, kind)

    def test_fields_mentioned_in_a_message_are_found_in_order(self):
        names = ["short_window", "long_window", "symbols"]
        self.assertEqual(gateway.mentioned_params("long_window must be greater than short_window", names), ["long_window", "short_window"])
        self.assertEqual(gateway.mentioned_params("the long_window_x is odd", names), [])      # whole words only

    def test_host_names_are_read_from_the_host_header(self):
        self.assertEqual([gateway.host_name(h) for h in ("127.0.0.1:5173", "localhost", "[::1]:5173", "evil.example:80")],
                         ["127.0.0.1", "localhost", "[::1]", "evil.example"])


class PassThrough(unittest.TestCase):
    """The tool's stdout reaches the browser exactly, whatever it contains."""

    def test_stdout_is_forwarded_byte_for_byte_after_one_metadata_line(self):
        stdout = ('{"schema":"strategy_lab.replay","price":9223372036854.775807,"qty":9223372036854775807,'
                  '"z":-1.7320508075688774,"tiny":1e-7,"symbol":"\u00e9\\u00e8"}\n').encode("utf-8")
        process = bridge.ProcessResult(("x",), "run\n", 0, stdout, "", False, 0.012)
        document = mock.Mock()
        outcome = bridge.RunOutcome(process, document)
        catalog = mock.Mock()
        spec = mock.Mock(kind="sma_crossover", available=True, unavailable_reason="")
        spec.params = [mock.Mock()]
        spec.params[0].name = "symbols"
        catalog.strategy = lambda kind: spec if kind == "sma_crossover" else None
        runner = bridge.Runner(("x",), "C:\\somewhere\\strategy_lab_replay.exe", 1, 2, "a" * 64)
        g = gateway.Gateway(lambda: runner)
        g._catalog = (runner.identity, catalog, b"{}")
        with mock.patch.object(bridge, "run_replay", return_value=outcome):
            response = g.replay({"strategy": {"kind": "sma_crossover", "params": [["symbols", "A"]]},
                                 "dataset": {"builtin": "sma_crossover"}})
        meta, rest = split_replay(response)
        self.assertEqual(rest, stdout)
        self.assertEqual(meta["runner"]["name"], "strategy_lab_replay.exe")             # no directory leaves the machine
        self.assertNotIn(b"somewhere", response.body)
        self.assertTrue(response.body.split(b"\n", 1)[0].isascii())                     # metadata is plain ASCII JSON


class Http(unittest.TestCase):
    """The real server on an ephemeral loopback port (the runner is real when built, otherwise only the guards are exercised)."""

    @classmethod
    def setUpClass(cls):
        try:
            runner = real_runner()
        except unittest.SkipTest:
            runner = None
        cls.runner = runner
        cls.gateway = gateway.Gateway((lambda: runner) if runner else bridge.find_runner)
        cls.server = gateway.make_server(cls.gateway, 0)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def call(self, method, path, body=None, headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            connection.request(method, path, body=body, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def test_it_listens_on_loopback_only(self):
        self.assertEqual(self.server.server_address[0], "127.0.0.1")

    def test_status_over_http_is_json_and_not_cacheable(self):
        status, headers, body = self.call("GET", "/lab/v1/status")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Cache-Control"], "no-store")
        self.assertIn("runner", json.loads(body))

    def test_a_foreign_host_header_is_refused(self):
        status, _, body = self.call("GET", "/lab/v1/status", headers={"Host": "attacker.example"})
        self.assertEqual((status, json.loads(body)["error"]["code"]), (403, "host_refused"))

    def test_the_console_through_the_vite_proxy_keeps_its_own_loopback_host_and_origin(self):
        status, _, _ = self.call("GET", "/lab/v1/status", headers={"Host": "127.0.0.1:5173", "Origin": "http://127.0.0.1:5173"})
        self.assertEqual(status, 200)

    def test_a_foreign_origin_or_cross_site_request_is_refused(self):
        for headers in ({"Origin": "https://attacker.example"}, {"Sec-Fetch-Site": "cross-site"}):
            status, _, body = self.call("GET", "/lab/v1/status", headers=headers)
            self.assertEqual((status, json.loads(body)["error"]["code"]), (403, "origin_refused"), headers)

    def test_post_needs_json_a_length_and_a_bounded_body(self):
        status, _, body = self.call("POST", "/lab/v1/check", body=b"{}", headers={"Content-Type": "text/plain"})
        self.assertEqual((status, json.loads(body)["error"]["code"]), (400, "bad_request"))
        status, _, body = self.call("POST", "/lab/v1/check", body=b"not json", headers={"Content-Type": "application/json"})
        self.assertEqual(status, 400)
        status, _, body = self.call("POST", "/lab/v1/check", headers={"Content-Type": "application/json",
                                                                       "Content-Length": str(gateway.MAX_BODY_BYTES + 1)})
        self.assertEqual((status, json.loads(body)["error"]["code"]), (413, "input_too_large"))

    def test_unknown_paths_and_wrong_methods_are_distinguished(self):
        self.assertEqual(self.call("GET", "/lab/v1/nope")[0], 404)
        self.assertEqual(self.call("GET", "/lab/v1/replay")[0], 405)
        self.assertEqual(self.call("POST", "/lab/v1/status", body=b"{}", headers={"Content-Type": "application/json"})[0], 405)

    def test_a_rejected_post_is_answered_even_when_its_body_was_never_read(self):
        # Answering and closing with unread request bytes makes some TCP stacks reset the connection, so the client never sees the status.
        body = b"x" * 300_000
        for path, expected in (("/lab/v1/status", 405), ("/lab/v1/nope", 404)):
            for attempt in range(25):
                status, _, _ = self.call("POST", path, body=body, headers={"Content-Type": "application/json"})
                self.assertEqual(status, expected, f"{path} attempt {attempt}")
        status, _, response = self.call("POST", "/lab/v1/status", body=body, headers={"Content-Type": "application/json", "Origin": "https://attacker.example"})
        self.assertEqual((status, json.loads(response)["error"]["code"]), (403, "origin_refused"))

    def test_a_replay_round_trips_over_http(self):
        if self.runner is None:
            self.skipTest("strategy_lab_replay is not built")
        payload = json.dumps({"strategy": SMA, "dataset": {"builtin": "sma_crossover"}}).encode()
        status, headers, body = self.call("POST", "/lab/v1/replay", body=payload, headers={"Content-Type": "application/json"})
        self.assertEqual(status, 200)
        self.assertTrue(headers["Content-Type"].startswith("application/x-ndjson"))
        meta, _, raw = body.partition(b"\n")
        self.assertEqual(json.loads(meta)["dataset"]["name"], "sma_crossover.csv")
        self.assertEqual(json.loads(raw)["schema"], "strategy_lab.replay")

    def test_the_scenario_list_is_served_and_validation_is_post_only(self):
        status, _, body = self.call("GET", "/lab/v1/validation")
        self.assertEqual(status, 200)
        self.assertIn("scenario_set", json.loads(body))
        self.assertEqual(self.call("GET", "/lab/v1/validate")[0], 405)
        self.assertEqual(self.call("POST", "/lab/v1/validation", body=b"{}", headers={"Content-Type": "application/json"})[0], 405)

    def test_validation_over_http_needs_no_body(self):
        if self.runner is None:
            self.skipTest("strategy_lab_replay is not built")
        status, _, body = self.call("POST", "/lab/v1/validate", headers={"Content-Type": "application/json"})
        self.assertEqual(status, 200)
        report = json.loads(body)
        self.assertEqual(report["executable"]["sha256"], self.runner.sha256)
        self.assertEqual(report["summary"]["failed"] + report["summary"]["error"], 0)

    def test_a_busy_gateway_says_so_instead_of_queueing_forever(self):
        busy = gateway.Gateway(lambda: self.runner or bridge.find_runner(), max_concurrent=1)
        if self.runner is None:
            self.skipTest("strategy_lab_replay is not built")
        busy._slots.acquire()
        with mock.patch.object(gateway, "BUSY_WAIT_S", 0.05):
            with self.assertRaises(LabUiError) as caught:
                busy.check({"strategy": SMA})
        self.assertEqual(caught.exception.kind, "busy")
        self.assertEqual(gateway.error_response(caught.exception).status, 503)


class MissingRunner(unittest.TestCase):
    def test_a_missing_executable_is_reported_with_the_build_commands_not_as_a_crash(self):
        def provider():
            raise LabUiError("executable_missing", "strategy_lab_replay has not been built.", detail="Looked for: x")

        g = gateway.Gateway(provider)
        body = json.loads(g.status().body)["runner"]
        self.assertEqual(body["state"], "missing")
        self.assertIn("cmake --build out/strategy_lab", body["setup"])
        with self.assertRaises(LabUiError) as caught:
            g.replay({"strategy": SMA, "dataset": {"builtin": "sma_crossover"}})
        response = gateway.error_response(caught.exception)
        self.assertEqual(response.status, 503)
        self.assertIn("setup", json.loads(response.body)["error"])


if __name__ == "__main__":
    unittest.main()
