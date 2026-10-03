"""The subprocess bridge: bounded and clean, exit statuses interpreted per the runner contract, malformed output refused.

Part 1 drives a small fake runner (a Python script) through every failure path. Part 2 runs the REAL
strategy_lab_replay (skipped, with a reason, if it has not been built).
"""

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

from support import dumps, ev, make_document, real_runner

from strategy_lab_ui import bridge, datasets
from strategy_lab_ui.errors import LabUiError

FAKE_SCRIPT = Path(__file__).with_name("fake_runner.py")


def fake_runner(directory: Path | None = None) -> bridge.Runner:
    return bridge.Runner((sys.executable, str(FAKE_SCRIPT)), str(FAKE_SCRIPT), 0, 0, "f" * 64)


class FakeRunnerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls._dir = tempfile.TemporaryDirectory()
        cls.runner = fake_runner(Path(cls._dir.name))

    @classmethod
    def tearDownClass(cls):
        cls._dir.cleanup()

    def kind_of(self, mode, **kw):
        result = bridge.run_process(self.runner, [mode], **kw)
        try:
            bridge._raise_for_status(result)
        except LabUiError as error:
            return error
        self.fail(f"{mode}: no error raised")

    # -- bounds ----------------------------------------------------------------------------------
    def test_a_run_that_exceeds_the_time_limit_is_stopped_and_reported(self):
        started = time.monotonic()
        with self.assertRaises(LabUiError) as caught:
            bridge.run_process(self.runner, ["sleep"], timeout=1.0)
        self.assertEqual(caught.exception.kind, "timeout")
        self.assertLess(time.monotonic() - started, 20)

    def test_output_beyond_the_limit_kills_the_process_and_is_reported(self):
        started = time.monotonic()
        with self.assertRaises(LabUiError) as caught:
            bridge.run_process(self.runner, ["flood"], max_stdout=1024 * 1024, timeout=30)
        self.assertEqual(caught.exception.kind, "output_too_large")
        self.assertLess(time.monotonic() - started, 20)

    def test_stderr_is_bounded_but_never_blocks_the_process(self):
        result = bridge.run_process(self.runner, ["stderr"])
        self.assertEqual(result.exit_code, 0)
        self.assertTrue(result.stderr_truncated)
        self.assertEqual(len(result.stderr.encode()), bridge.MAX_STDERR_BYTES)
        self.assertEqual(result.stdout, b"{}\n")

    # -- isolation and cleanup ---------------------------------------------------------------------
    def test_the_process_runs_in_a_private_directory_that_is_removed_afterwards(self):
        result = bridge.run_process(self.runner, ["echo"], files={"dataset.csv": b"a,b\n"})
        info = json.loads(result.stdout)
        self.assertEqual(sorted(info["files"]), ["args.txt", "dataset.csv"])
        self.assertFalse(Path(info["cwd"]).exists(), "the temporary directory was not cleaned up")
        self.assertEqual(result.argv[-1], "@args.txt")
        self.assertEqual(info["args"][0], "echo")

    def test_the_directory_is_removed_even_when_the_run_fails(self):
        with self.assertRaises(LabUiError):
            bridge.run_process(self.runner, ["sleep"], timeout=0.5)
        leftovers = [p for p in Path(tempfile.gettempdir()).glob("strategy_lab_ui_*") if p.is_dir()]
        self.assertEqual(leftovers, [])

    def test_the_command_is_an_argument_list_with_no_shell(self):
        hostile = "A & calc.exe | echo pwned > out.txt"
        result = bridge.run_process(self.runner, ["echo", hostile])
        self.assertEqual(json.loads(result.stdout)["args"][1], hostile)       # arrives as plain data

    def test_non_ascii_arguments_survive_through_the_response_file(self):
        values = ["\u00c4PPL, \u65e5\u672c", "caf\u00e9", "\U0001f600"]
        result = bridge.run_process(self.runner, ["echo", *values])
        self.assertEqual(json.loads(result.stdout)["args"][1:4], values)
        self.assertEqual(result.args_file, "\n".join(["echo", *values]) + "\n")

    def test_a_non_ascii_temporary_directory_works_because_the_response_file_is_relative(self):
        with tempfile.TemporaryDirectory(prefix="t\u00e9mp_\u65e5\u672c_") as base:
            saved = tempfile.tempdir
            tempfile.tempdir = base
            try:
                result = bridge.run_process(self.runner, ["echo", "x"], files={"dataset.csv": b"1"})
            finally:
                tempfile.tempdir = saved
        info = json.loads(result.stdout)
        self.assertIn("\u65e5\u672c", info["cwd"])
        self.assertEqual(info["files"], ["args.txt", "dataset.csv"])

    def test_non_ascii_stderr_is_decoded(self):
        result = bridge.run_process(self.runner, ["stderr_text"])
        self.assertIn("h\u00e9llo \u65e5\u672c", result.stderr)

    # -- interpreting exit status --------------------------------------------------------------------
    def test_exit_2_is_a_rejected_configuration_with_the_tools_own_words(self):
        error = self.kind_of("err2")
        self.assertEqual((error.kind, error.exit_code), ("runner_rejected", 2))
        self.assertIn("long_window must be greater than short_window", error.message)

    def test_exit_3_is_a_rejected_dataset_with_located_problems(self):
        error = self.kind_of("err3")
        self.assertEqual((error.kind, error.exit_code), ("dataset_invalid", 3))
        self.assertEqual((error.problems[0].line, error.problems[0].column), (3, "price"))

    def test_exit_4_is_an_internal_error(self):
        self.assertEqual(self.kind_of("err4").kind, "runner_internal_error")

    def test_an_exit_status_the_tool_never_uses_is_unexpected(self):
        error = self.kind_of("exit7")
        self.assertEqual((error.kind, error.exit_code), ("unexpected_exit", 7))

    def test_an_error_document_that_disagrees_with_the_exit_status_is_refused(self):
        self.assertEqual(self.kind_of("err_mismatch").kind, "malformed_output")

    def test_a_failure_status_without_an_error_document_is_malformed_output(self):
        error = self.kind_of("exit3_text")
        self.assertEqual(error.kind, "malformed_output")
        self.assertIn("plain text", error.stdout_excerpt)

    def test_exit_0_with_an_error_document_is_not_a_result(self):
        result = bridge.run_process(self.runner, ["err_but_zero"])
        bridge._raise_for_status(result)                             # status 0 passes this stage...
        from strategy_lab_ui.jsonio import strict_loads
        from strategy_lab_ui.schema import parse_replay
        with self.assertRaises(LabUiError) as caught:                 # ...but the document is not a replay
            parse_replay(strict_loads(result.stdout))
        self.assertEqual(caught.exception.kind, "malformed_output")

    # -- run_replay end to end through the fake ---------------------------------------------------------
    def run_replay_with(self, document_or_bytes, **kw):
        directory = Path(self._dir.name)
        payload = document_or_bytes if isinstance(document_or_bytes, bytes) else dumps(document_or_bytes)
        (directory / "canned.json").write_bytes(payload)
        runner = self.runner
        original = bridge.run_process

        def patched(r, args, **kwargs):
            return original(r, ["replay", str(directory / "canned.json")], **kwargs)

        bridge.run_process = patched
        try:
            return bridge.run_replay(runner, dataset=b"d", dataset_sha256=kw.pop("sha", "ab" * 32), kind=kw.pop("kind", "sma_crossover"),
                                     params=[("short_window", "1")], **kw)
        finally:
            bridge.run_process = original

    def test_a_good_document_is_accepted(self):
        outcome = self.run_replay_with(make_document([ev("A", 30), ev("A", 31, signals=[(0, 1, "buy")])]))
        self.assertEqual(len(outcome.document.events), 2)

    def test_malformed_output_is_refused_with_a_useful_message_and_an_excerpt(self):
        with self.assertRaises(LabUiError) as caught:
            self.run_replay_with(b"this is not json\n")
        self.assertEqual(caught.exception.kind, "malformed_output")
        self.assertIn("this is not json", caught.exception.stdout_excerpt)

    def test_unsupported_schema_is_refused(self):
        with self.assertRaises(LabUiError) as caught:
            self.run_replay_with(make_document([ev("A", 30)], schema_version="2.0"))
        self.assertEqual(caught.exception.kind, "unsupported_schema")

    def test_inconsistent_output_is_refused(self):
        document = make_document([ev("A", 30, signals=[(0, 1, "buy")])])
        document["result"]["events"][0]["results"][0]["signal_ids"] = []
        with self.assertRaises(LabUiError) as caught:
            self.run_replay_with(document)
        self.assertEqual(caught.exception.kind, "invalid_structure")

    def test_a_result_for_different_input_is_never_accepted(self):
        with self.assertRaises(LabUiError) as caught:
            self.run_replay_with(make_document([ev("A", 30)], dataset_sha="cd" * 32))
        self.assertEqual(caught.exception.kind, "result_mismatch")
        self.assertIn("SHA-256", caught.exception.message)

    def test_a_result_for_a_different_strategy_is_never_accepted(self):
        with self.assertRaises(LabUiError) as caught:
            self.run_replay_with(make_document([ev("A", 30)], kind="mean_reversion"))
        self.assertEqual(caught.exception.kind, "result_mismatch")


class ArgumentFile(unittest.TestCase):
    def test_what_the_response_file_cannot_carry_is_refused_not_mangled(self):
        for bad in ("", "a\nb", "a\rb", "#comment", "@nested"):
            with self.assertRaises(LabUiError, msg=repr(bad)) as caught:
                bridge.encode_args(["run", bad])
            self.assertEqual(caught.exception.kind, "input_unencodable")

    def test_lone_surrogates_cannot_be_encoded(self):
        with self.assertRaises(LabUiError) as caught:
            bridge.encode_args(["run", "bad\ud800"])
        self.assertEqual(caught.exception.kind, "input_unencodable")

    def test_utf8_without_a_bom_one_argument_per_line(self):
        self.assertEqual(bridge.encode_args(["run", "x=\u00e9"]), b"run\nx=\xc3\xa9\n")


class Locating(unittest.TestCase):
    def test_nothing_built_gives_an_actionable_message(self):
        with tempfile.TemporaryDirectory() as empty:
            with self.assertRaises(LabUiError) as caught:
                bridge.find_runner(environ={}, repo_root=Path(empty))
        self.assertEqual(caught.exception.kind, "executable_missing")
        self.assertIn("cmake --build out/strategy_lab", caught.exception.detail)
        self.assertIn("STRATEGY_LAB_EXE", caught.exception.message)

    def test_an_explicit_path_that_does_not_exist(self):
        with self.assertRaises(LabUiError) as caught:
            bridge.find_runner("C:/definitely/not/here/strategy_lab_replay.exe", environ={})
        self.assertEqual(caught.exception.kind, "executable_missing")

    def test_a_directory_is_not_an_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(LabUiError) as caught:
                bridge.find_runner(directory, environ={})
        self.assertEqual(caught.exception.kind, "executable_invalid")

    @unittest.skipUnless(os.name == "nt", "the .exe rule is Windows-only")
    def test_a_non_exe_file_is_refused_on_windows(self):
        with tempfile.NamedTemporaryFile(suffix=".txt", delete=False) as handle:
            path = handle.name
        try:
            with self.assertRaises(LabUiError) as caught:
                bridge.identify(Path(path))
            self.assertEqual(caught.exception.kind, "executable_invalid")
        finally:
            os.unlink(path)

    def test_the_environment_variable_is_used_when_no_path_is_given(self):
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / "tool.exe"
            fake.write_bytes(b"not really")
            runner = bridge.find_runner(environ={bridge.EXE_ENV: str(fake)})
        self.assertEqual(Path(runner.path).name, "tool.exe")
        self.assertEqual(len(runner.sha256), 64)

    def test_the_identity_changes_when_the_file_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            fake = Path(directory) / "tool.exe"
            fake.write_bytes(b"one")
            first = bridge.identify(fake)
            fake.write_bytes(b"two!")
            self.assertNotEqual(first.identity, bridge.identify(fake).identity)
            self.assertNotEqual(first.sha256, bridge.identify(fake).sha256)

    def test_a_program_that_is_not_the_replay_tool_is_rejected_by_describe(self):
        with tempfile.TemporaryDirectory() as directory:
            runner = fake_runner(Path(directory))
            with self.assertRaises(LabUiError) as caught:
                bridge.describe(runner, timeout=20)           # the fake answers 'describe' with nothing useful
        self.assertEqual(caught.exception.kind, "executable_invalid")

    def test_a_program_that_cannot_start_is_reported(self):
        runner = bridge.Runner(("C:/no/such/program.exe",), "C:/no/such/program.exe", 0, 0, "0" * 64)
        with self.assertRaises(LabUiError) as caught:
            bridge.run_process(runner, ["describe"])
        self.assertEqual(caught.exception.kind, "spawn_failed")


# ---- the real executable ----------------------------------------------------------------------------

class RealExecutable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()

    def replay(self, fixture, kind, params, **kw):
        source = datasets.load_builtin(fixture) if isinstance(fixture, str) else fixture
        return bridge.run_replay(self.runner, dataset=source.data, dataset_sha256=source.sha256, kind=kind,
                                 params=params, **kw)

    def test_describe_returns_the_catalog_with_defaults_from_the_cpp_config(self):
        catalog = bridge.describe(self.runner)
        self.assertEqual([s.kind for s in catalog.strategies], ["sma_crossover", "mean_reversion", "ml"])
        self.assertFalse(catalog.strategy("ml").available)
        sma = catalog.strategy("sma_crossover")
        self.assertEqual((sma.param("short_window").default, sma.param("long_window").default), (5, 20))
        self.assertTrue(sma.param("symbols").required and not sma.param("symbols").has_default)
        self.assertEqual(catalog.strategy("mean_reversion").param("entry_threshold").default, 2)
        # A whole-share quantity is a "uint" parameter and an int; a rate default (0.5) is a plain float, never a
        # Decimal: only prices keep their exact decimal type.
        quantity = sma.param("requested_quantity")
        self.assertEqual((quantity.type, quantity.default), ("uint", 1))
        self.assertIs(type(quantity.default), int)
        rearm = catalog.strategy("mean_reversion").param("rearm_threshold").default
        self.assertEqual(rearm, 0.5)
        self.assertIs(type(rearm), float)
        self.assertIn("crossover_buy", [r.code for r in sma.reasons])

    def test_sma_fixture_signals_are_where_the_hand_derivation_puts_them(self):
        outcome = self.replay("sma_crossover", "sma_crossover",
                              [("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL")])
        doc = outcome.document
        self.assertEqual([(s.signal_id, s.side, s.event_index) for s in doc.signals], [(1, "buy", 4), (2, "sell", 7)])
        self.assertEqual([doc.events[4].results[0].reason, doc.events[7].results[0].reason], ["crossover_buy", "crossover_sell"])
        self.assertEqual(outcome.process.exit_code, 0)
        self.assertEqual(doc.events[4].results[0].indicators, {"short_sma": 2.5, "long_sma": 2, "short_minus_long": 0.5})

    def test_mean_reversion_fixture(self):
        outcome = self.replay("mr_rearm", "mean_reversion", [("lookback", "4"), ("entry_threshold", "1.5"),
                                                              ("rearm_threshold", "0.5"), ("symbols", "AAPL")])
        self.assertEqual([(s.side, s.event_index) for s in outcome.document.signals], [("buy", 3), ("buy", 5), ("sell", 6)])
        self.assertAlmostEqual(outcome.document.events[3].results[0].indicators["z_score"], -1.7320508075688772)

    def test_a_valid_run_with_no_signals(self):
        outcome = self.replay("constant_price", "sma_crossover", [("symbols", "AAPL")])
        self.assertEqual(outcome.document.signals, ())
        self.assertEqual(len(outcome.document.events), 30)

    def test_two_symbols_with_equal_timestamps_keep_file_order_and_ids(self):
        first = self.replay("tie_aapl_first", "sma_crossover", [("short_window", "1"), ("long_window", "2"), ("symbols", "AAPL,MSFT")])
        second = self.replay("tie_msft_first", "sma_crossover", [("short_window", "1"), ("long_window", "2"), ("symbols", "AAPL,MSFT")])
        sells = lambda out: [(s.signal_id, s.symbol) for s in out.document.signals if s.side == "sell"]
        self.assertEqual(sells(first)[0], (1, "AAPL"))
        self.assertEqual(sells(second)[0], (1, "MSFT"))
        times = [e.exchange_time for e in first.document.events]
        self.assertEqual(times[0], times[1])                                # equal timestamps, distinct events

    def test_unknown_parameter_value_reaches_the_runner_and_its_words_come_back(self):
        with self.assertRaises(LabUiError) as caught:
            self.replay("sma_crossover", "sma_crossover", [("short_window", "abc"), ("symbols", "AAPL")])
        self.assertEqual(caught.exception.kind, "runner_rejected")
        self.assertIn("short_window", caught.exception.message)

    def test_a_configuration_the_strategy_rejects_is_shown_verbatim(self):
        with self.assertRaises(LabUiError) as caught:
            self.replay("sma_crossover", "sma_crossover", [("short_window", "5"), ("long_window", "3"), ("symbols", "AAPL")])
        self.assertIn("long_window must be greater than short_window", caught.exception.message)

    def test_an_invalid_dataset_reports_line_and_column(self):
        bad = datasets.from_upload("bad.csv", b"symbol,exchange_time,type,price\nXYZ,2026-03-02T14:30:00Z,bar,50\n"
                                              b"XYZ,2026-03-02T14:31:00Z,bar,abc\n")
        with self.assertRaises(LabUiError) as caught:
            self.replay(bad, "sma_crossover", [("symbols", "XYZ")])
        error = caught.exception
        self.assertEqual((error.kind, error.exit_code), ("dataset_invalid", 3))
        self.assertEqual((error.problems[0].line, error.problems[0].column), (3, "price"))

    def test_out_of_order_times_are_refused_not_repaired(self):
        bad = datasets.from_upload("order.csv", b"symbol,exchange_time,type,price\nX,2026-03-02T14:31:00Z,bar,1\n"
                                                b"X,2026-03-02T14:30:00Z,bar,2\n")
        with self.assertRaises(LabUiError) as caught:
            self.replay(bad, "sma_crossover", [("symbols", "X")])
        self.assertEqual(caught.exception.kind, "dataset_invalid")

    def test_a_csv_with_a_header_only_is_rejected(self):
        with self.assertRaises(LabUiError) as caught:
            self.replay(datasets.from_upload("h.csv", b"symbol,exchange_time,type,price\n"), "sma_crossover", [("symbols", "X")])
        self.assertEqual(caught.exception.kind, "dataset_invalid")

    def test_non_ascii_allowlist_symbols_round_trip_through_the_response_file(self):
        outcome = self.replay("sma_crossover", "sma_crossover",
                              [("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL,\u00c4PPL,\u65e5\u672c")])
        self.assertEqual(outcome.process.exit_code, 0)
        listed = outcome.document.strategies[0].parameters["symbols"]
        self.assertEqual(listed, ["AAPL", "\u00c4PPL", "\u65e5\u672c"])
        text = " ".join(w.message for w in outcome.document.warnings)
        self.assertIn("\u00c4PPL", text)
        self.assertIn("\u65e5\u672c", text)

    def test_a_non_ascii_temporary_directory_does_not_break_the_real_tool(self):
        with tempfile.TemporaryDirectory(prefix="t\u00e9mp_\u65e5\u672c_") as base:
            saved = tempfile.tempdir
            tempfile.tempdir = base
            try:
                outcome = self.replay("sma_crossover", "sma_crossover",
                                      [("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL")])
            finally:
                tempfile.tempdir = saved
        self.assertEqual(len(outcome.document.signals), 2)

    def test_the_row_limit_is_enforced_by_the_tool(self):
        lines = ["symbol,exchange_time,type,price"]
        for i in range(20_001):
            lines.append(f"X,2026-01-01T{i // 3600 % 24:02d}:{i // 60 % 60:02d}:{i % 60:02d}.{i // 86400:03d}Z,bar,10")
        big = datasets.from_upload("big.csv", ("\n".join(lines) + "\n").encode())
        with self.assertRaises(LabUiError) as caught:
            self.replay(big, "sma_crossover", [("symbols", "X")], max_rows=20_000)
        self.assertEqual(caught.exception.kind, "dataset_invalid")
        self.assertIn("20000", caught.exception.message + " ".join(p.message for p in caught.exception.problems))

    def test_the_run_identity_is_deterministic(self):
        params = [("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL")]
        a = self.replay("sma_crossover", "sma_crossover", params).document
        b = self.replay("sma_crossover", "sma_crossover", params).document
        self.assertEqual(a.run_id, b.run_id)
        self.assertEqual(a.provenance["result_sha256"], b.provenance["result_sha256"])
        c = self.replay("sma_crossover", "sma_crossover", [("short_window", "1"), ("long_window", "3"), ("symbols", "AAPL")]).document
        self.assertNotEqual(a.run_id, c.run_id)

    def test_the_raw_output_is_kept_verbatim_for_the_json_export(self):
        outcome = self.replay("sma_crossover", "sma_crossover", [("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL")])
        self.assertTrue(outcome.process.stdout.endswith(b"}\n"))
        self.assertEqual(json.loads(outcome.process.stdout)["result"]["run"]["run_id"], outcome.document.run_id)


if __name__ == "__main__":
    unittest.main()
