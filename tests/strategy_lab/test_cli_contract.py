"""The executable's contract, checked from outside: exit status, what is on stdout and stderr,
byte-for-byte determinism, encoding, and a larger-than-fixture run. Every stdout is parsed with
a real JSON parser in strict mode (see labtest.strict_loads)."""

from __future__ import annotations

import os
import unittest
from datetime import datetime, timezone
from decimal import Decimal

import labtest
from labtest import LabTestCase, datasets, run_exe, sma_args, strict_loads


class DescribeContract(LabTestCase):
    def test_describe_is_one_valid_document_with_the_authoritative_defaults(self) -> None:
        run = run_exe(["describe"])
        self.assertEqual(run.returncode, 0)
        self.assertEqual(run.stderr, "")
        doc = run.json()
        self.assertEqual(doc["schema"], "strategy_lab.catalog")
        self.assertEqual(doc["schema_version"], "1.0")
        catalog = doc["catalog"]
        kinds = {k["kind"]: k for k in catalog["strategies"]}
        self.assertEqual(list(kinds), ["sma_crossover", "mean_reversion", "ml"])

        def defaults(kind: str) -> dict:
            return {p["name"]: p.get("default") for p in kinds[kind]["parameters"]}

        # Literals from docs/strategies/*.md section 2: the single source (the C++ config structs) must agree.
        self.assertEqual(defaults("sma_crossover"),
                         {"strategy_id": "sma_crossover", "short_window": 5, "long_window": 20, "requested_quantity": 1, "symbols": None})
        self.assertEqual(defaults("mean_reversion"),
                         {"strategy_id": "mean_reversion", "lookback": 20, "entry_threshold": 2.0, "rearm_threshold": 0.5,
                          "requested_quantity": 1, "symbols": None})
        # A quantity is a whole number of shares, an exact int64: a JSON integer and a "uint" parameter, never a double.
        for kind in ("sma_crossover", "mean_reversion"):
            quantity = [p for p in kinds[kind]["parameters"] if p["name"] == "requested_quantity"][0]
            self.assertEqual(quantity["type"], "uint", kind)
            self.assertIs(type(quantity["default"]), int, kind)
        symbols = [p for p in kinds["sma_crossover"]["parameters"] if p["name"] == "symbols"][0]
        self.assertTrue(symbols["required"])
        self.assertNotIn("default", symbols)
        self.assertFalse(kinds["ml"]["available"])
        self.assertIn("not implemented", kinds["ml"]["unavailable_reason"].lower())
        self.assertEqual([c["name"] for c in catalog["dataset_format"]["columns"] if c["required"]],
                         ["symbol", "exchange_time", "type", "price"])
        self.assertEqual(catalog["limits"]["max_strategies"], 16)

    def test_describe_is_byte_identical_between_runs(self) -> None:
        self.assertEqual(run_exe(["describe"]).stdout_bytes, run_exe(["describe"]).stdout_bytes)


class ReplayContract(LabTestCase):
    def test_a_successful_run_is_one_document_with_provenance_kept_apart_from_the_result(self) -> None:
        run = run_exe(sma_args("sma_crossover.csv"))
        self.assertEqual(run.returncode, 0)
        self.assertEqual(run.stderr, "", "a clean run says nothing on stderr")
        doc = run.json()
        self.assertEqual(list(doc), ["schema", "schema_version", "provenance", "result"])
        self.assertEqual(doc["schema"], "strategy_lab.replay")
        prov = doc["provenance"]
        generated = prov["generated_at"]
        self.assertRegex(generated, r"^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{9}Z$")
        parsed = datetime.fromisoformat(generated[:26] + "+00:00")
        self.assertLess(abs((datetime.now(timezone.utc) - parsed).total_seconds()), 300, "a real current wall-clock time")
        self.assertEqual(prov["tool"], "strategy_lab_replay")
        self.assertRegex(prov["result_sha256"], r"^[0-9a-f]{64}$")
        self.assertNotIn("generated_at", labtest.result_substring(run.stdout), "no wall clock inside the result")

    def test_the_result_hash_is_the_sha256_of_the_compact_result_bytes(self) -> None:
        run = run_exe(sma_args("sma_crossover.csv"))
        doc = run.json()
        self.assertEqual(doc["provenance"]["result_sha256"], labtest.sha256_text(labtest.result_substring(run.stdout)))

    def test_without_a_wall_clock_two_runs_are_byte_identical_and_with_one_only_provenance_differs(self) -> None:
        args = sma_args("two_symbols_interleaved.csv", symbols="AAPL,MSFT")
        a = run_exe([*args, "--no-wall-clock"])
        b = run_exe([*args, "--no-wall-clock"])
        self.assertEqual(a.stdout_bytes, b.stdout_bytes)
        self.assertNotIn("generated_at", a.stdout)

        stamped = run_exe(args)
        self.assertEqual(labtest.result_substring(stamped.stdout), labtest.result_substring(a.stdout))
        self.assertEqual(stamped.json()["provenance"]["result_sha256"], a.json()["provenance"]["result_sha256"])

    def test_pretty_and_compact_output_are_the_same_document(self) -> None:
        compact = run_exe([*sma_args("sma_crossover.csv"), "--no-wall-clock"]).json()
        pretty_run = run_exe([*sma_args("sma_crossover.csv"), "--no-wall-clock", "--pretty"])
        self.assertEqual(pretty_run.json(), compact)
        self.assertIn('\n  "schema"', pretty_run.stdout)

    def test_stdout_is_never_translated_to_crlf(self) -> None:
        # Windows text mode would turn each '\n' into '\r\n' and break a byte-exact document.
        for args in (sma_args("sma_crossover.csv"), ["describe"], [*sma_args("sma_crossover.csv"), "--pretty"]):
            self.assertNotIn(b"\r", run_exe(args).stdout_bytes, args)

    def test_the_documented_crossover_is_buy_at_bar_five_and_sell_at_bar_eight(self) -> None:
        result = run_exe(sma_args("sma_crossover.csv")).json()["result"]
        signals = result["signals"]
        self.assertEqual([(s["signal_id"], s["side"], s["event_index"], s["created_at"]) for s in signals],
                         [(1, "buy", 4, "2026-01-05T14:34:00.000000000Z"), (2, "sell", 7, "2026-01-05T14:37:00.000000000Z")])
        self.assertEqual(signals[0]["metadata"], {"long_sma": "2", "long_window": "3", "short_sma": "2.5", "short_window": "2",
                                                  "trigger": "short_crossed_above_long"})
        self.assertEqual(signals[0]["requested_quantity"], 1)
        self.assertIs(type(signals[0]["requested_quantity"]), int, "a whole number of shares, written as an integer")
        self.assertEqual(signals[0]["order_type"], "market")
        for forbidden in ("confidence", "target_exposure", "limit_price"):
            self.assertNotIn(forbidden, signals[0], "unset by the strategy, so absent, never null")
        run_id = result["run"]["run_id"]
        self.assertEqual([s["signal_ref"] for s in signals], [f"{run_id}:1", f"{run_id}:2"])
        self.assertEqual(result["engine"]["stats"]["signals_published"], 2)
        reasons = [e["results"][0]["reason"] for e in result["events"]]
        self.assertEqual(reasons, ["warming_up", "warming_up", "baseline_established", "same_side", "crossover_buy",
                                   "same_side", "same_side", "crossover_sell", "same_side"])

    def test_no_hold_signal_is_ever_manufactured(self) -> None:
        # A whole run with no signal at all: every event is explained, none becomes a TradeSignal.
        result = run_exe(sma_args("constant_price.csv", "5", "20")).json()["result"]
        self.assertEqual(result["signals"], [])
        self.assertEqual(len(result["events"]), 30)
        self.assertTrue(all(e["results"][0]["signal_ids"] == [] for e in result["events"]))
        self.assertEqual(result["summary"]["strategies"][0]["signals"], {"buy": 0, "sell": 0})

    def test_unavailable_diagnostic_values_are_listed_with_a_reason_not_written_as_zero(self) -> None:
        result = run_exe(sma_args("sma_crossover.csv")).json()["result"]
        warm = result["events"][0]["results"][0]
        self.assertEqual(warm["indicators"], {})
        self.assertEqual(warm["unavailable"], {"short_sma": "warming_up", "long_sma": "warming_up", "short_minus_long": "warming_up"})
        self.assertEqual(warm["window"], {"fill": 1, "remaining": 2, "size": 3})
        full = result["events"][2]["results"][0]
        self.assertEqual(full["indicators"], {"short_sma": 1.5, "long_sma": 2.0, "short_minus_long": -0.5})
        self.assertEqual(full["unavailable"], {})

    def test_trade_rows_show_unavailable_state_for_an_event_that_never_reached_a_symbol(self) -> None:
        events = run_exe(sma_args("bars_and_trades.csv", "1", "2")).json()["result"]["events"]
        trade = events[1]["results"][0]
        self.assertEqual((events[1]["type"], trade["reason"], trade["verdict"]), ("trade", "not_a_bar", "ignored"))
        self.assertNotIn("fill", trade["window"])
        self.assertEqual(trade["unavailable"]["short_sma"], "event_ignored")
        self.assertEqual(trade["states"], {})

    def test_optional_bar_fields_round_trip_exactly(self) -> None:
        event = run_exe(sma_args("ohlcv_example.csv")).json(exact=True)["result"]["events"][0]
        self.assertEqual((event["price"], event["open"], event["high"], event["low"], event["volume"]),
                         (Decimal("101.25"), Decimal("100.5"), Decimal("102"), Decimal("100.25"), 15000))
        self.assertIs(type(event["volume"]), int, "volume is a whole number of shares")


class FaultContract(LabTestCase):
    def test_a_refused_signal_is_a_result_with_exit_zero(self) -> None:
        run = run_exe([*sma_args("sma_crossover.csv"), "--bus-fault", "reject-signals"])
        self.assertEqual(run.returncode, 0)
        result = run.json()["result"]
        self.assertEqual(result["signals"], [])
        self.assertEqual([f["signal_id"] for f in result["publication_failures"]], [1, 2])
        self.assertEqual({f["kind"] for f in result["publication_failures"]}, {"rejected"})
        self.assertEqual(result["engine"]["stats"]["signals_rejected"], 2)
        self.assertEqual(result["run"]["context"]["bus_fault"], "reject-signals")

    def test_a_throwing_publish_is_counted_as_a_publish_error(self) -> None:
        result = run_exe([*sma_args("sma_crossover.csv"), "--bus-fault", "throw-on-signal"]).json()["result"]
        self.assertEqual(result["engine"]["stats"]["publish_errors"], 2)
        self.assertTrue(result["engine"]["strategies"][0]["last_error"].startswith("publish: "))


class ExitStatus(LabTestCase):
    def assert_error(self, args: list[str], status: int, code: str) -> dict:
        run = run_exe(args)
        self.assertEqual(run.returncode, status, run.stderr)
        self.assertNotEqual(run.stderr.strip(), "", "a failure is explained on stderr for a human")
        doc = run.json()   # stdout is still exactly one valid document
        self.assertEqual(doc["schema"], "strategy_lab.error")
        self.assertEqual(doc["error"]["code"], code)
        self.assertEqual(doc["error"]["exit_status"], status)
        return doc["error"]

    def test_every_failure_class_has_its_status_and_a_valid_error_document(self) -> None:
        good = datasets("sma_crossover.csv")
        self.assert_error([], 2, "usage_error")
        self.assert_error(["frobnicate"], 2, "usage_error")
        self.assert_error(["run", "--strategy", "sma_crossover"], 2, "usage_error")
        self.assert_error(["run", "--dataset", good], 2, "usage_error")
        self.assert_error(["run", "--dataset", good, "--strategy", "sma_crossover", "--bogus"], 2, "usage_error")
        self.assert_error(["run", "--dataset", good, "--strategy", "sma_crossover", "--max-rows", "0"], 2, "usage_error")
        self.assert_error(["run", "--dataset", good, "--strategy", "nope"], 2, "usage_error")
        self.assert_error(["run", "--dataset", good, "--strategy", "ml", "--param", "symbols=A"], 2, "unavailable_strategy")
        self.assert_error(["run", "--dataset", good, "--strategy", "sma_crossover", "--param", "bogus=1"], 2, "invalid_parameter")
        self.assert_error(["run", "--dataset", good, "--strategy", "sma_crossover", "--param", "short_window=-1"], 2, "invalid_parameter")
        self.assert_error(["run", "--dataset", good, "--strategy", "sma_crossover", "--param", "symbols=A", "--param", "short_window=0"],
                          2, "config_error")
        self.assert_error(["run", "--dataset", datasets("nope.csv"), "--strategy", "sma_crossover", "--param", "symbols=A"], 3, "dataset_error")
        self.assert_error(["run", "--dataset", good, "--max-rows", "5", "--strategy", "sma_crossover", "--param", "symbols=A"],
                          3, "limit_exceeded")

    def test_the_strategys_own_words_reach_both_streams(self) -> None:
        error = self.assert_error(["run", "--dataset", datasets("sma_crossover.csv"), "--strategy", "mean_reversion", "--param", "symbols=AAPL",
                                   "--param", "lookback=1"], 2, "config_error")
        self.assertEqual(error["message"], "configuration error: MeanReversionStrategy: lookback must be at least 2 (got 1)")
        self.assertEqual(error["problems"][0]["where"], "strategies[0]")

    def test_invalid_prices_are_refused_never_repaired(self) -> None:
        with labtest.temp_dir() as tmp:
            # not exact positive decimals: nan/inf, exponents, a sign, zero, text, nothing, more than 6 decimals,
            # more than int64 holds. Each is refused at its line and column; none is rounded or repaired.
            for bad in ("nan", "inf", "-inf", "0", "-1", "abc", "1e999", "1e3", "1.5.2", "", "0.0000001",
                        "9223372036855", "9223372036854.775808"):
                path = os.path.join(tmp, "bad.csv")
                with open(path, "w", encoding="utf-8", newline="\n") as handle:
                    handle.write(f"symbol,exchange_time,type,price\nAAPL,2026-01-05T14:30:00Z,bar,{bad}\n")
                error = self.assert_error(["run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=AAPL"], 3, "dataset_error")
                self.assertEqual((error["problems"][0]["line"], error["problems"][0]["column"]), (2, "price"), bad)

    def test_row_specific_errors_name_their_lines(self) -> None:
        with labtest.temp_dir() as tmp:
            path = os.path.join(tmp, "bad.csv")
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("symbol,exchange_time,type,price\n"
                             "AAPL,2026-01-05T14:30:00Z,bar,10\n"
                             "AAPL,2026-01-05T14:31:00Z,bar,nan\n"
                             "AAPL,2026-01-05T14:30:00Z,bar,12\n"
                             "MSFT,2026-01-05T14:29:00Z,bar,12\n")
            error = self.assert_error(["run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=AAPL"], 3, "dataset_error")
            self.assertEqual([(p["line"], p["column"]) for p in error["problems"]],
                             [(3, "price"), (4, "exchange_time"), (5, "exchange_time")])
            self.assertEqual(error["total_problems"], 3)

    def run_with_read_only_stdout(self, args: list[str]):
        # stdout is a handle to a REAL file opened read-only: every write to it fails. (Not the null device: on
        # Windows it ignores the handle's access mode and accepts writes.)
        import subprocess
        with labtest.temp_dir() as tmp:
            target = os.path.join(tmp, "read_only.txt")
            with open(target, "wb") as seed:
                seed.write(b"seed")
            with open(target, "rb") as read_only:
                return subprocess.run([labtest.EXE, *args], stdout=read_only, stderr=subprocess.PIPE, check=False)

    def test_stdout_that_cannot_be_written_is_an_internal_error_not_a_success(self) -> None:
        completed = self.run_with_read_only_stdout(sma_args("sma_crossover.csv"))
        self.assertEqual(completed.returncode, 4, completed.stderr)
        self.assertIn(b"internal_error", completed.stderr)

    def test_a_document_small_enough_to_sit_in_the_buffer_is_still_reported_as_unwritten(self) -> None:
        # An error document is a few hundred bytes: fwrite succeeds into the buffer and only the FLUSH fails.
        # The exit status must be 4 (could not write), not the 2 the usage error would otherwise give.
        completed = self.run_with_read_only_stdout([])
        self.assertEqual(completed.returncode, 4, completed.stderr)
        self.assertIn(b"could not write the result to stdout", completed.stderr)

class EncodingContract(LabTestCase):
    def test_unicode_quotes_and_backslashes_survive_an_args_file_exactly(self) -> None:
        strategy_id = 'caf\u00e9 \U0001F600 "q" back\\slash \u2028 tab\tend'
        with labtest.temp_dir() as tmp:
            args_file = os.path.join(tmp, "unicode.args")
            with open(args_file, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("\n".join(["run", "--dataset", datasets("sma_crossover.csv"), "--strategy", "sma_crossover",
                                        "--param", f"strategy_id={strategy_id}", "--param", "short_window=2",
                                        "--param", "long_window=3", "--param", "symbols=AAPL", "--no-wall-clock"]) + "\n")
            run = run_exe([f"@{args_file}"])
        self.assertEqual(run.returncode, 0, run.stderr)
        result = run.json()["result"]
        self.assertEqual(result["configuration"]["strategies"][0]["strategy_id"], strategy_id)
        self.assertEqual(result["signals"][0]["strategy_id"], strategy_id)
        self.assertEqual(result["configuration"]["strategies"][0]["parameters"]["strategy_id"], strategy_id)

    def test_a_symbol_with_a_backslash_is_escaped_and_read_back(self) -> None:
        result = run_exe(sma_args("escape_symbol.csv", "1", "2", "A\\B")).json()["result"]
        self.assertEqual(result["input"]["dataset"]["symbols"][0]["symbol"], "A\\B")
        self.assertEqual(result["signals"][0]["symbol"], "A\\B")
        self.assertEqual(result["configuration"]["strategies"][0]["parameters"]["symbols"], ["A\\B"])

    def test_an_error_echoing_hostile_text_is_still_valid_json(self) -> None:
        run = run_exe(["run", "--dataset", datasets("sma_crossover.csv"), "--strategy", "sma_crossover", "--param", 'q"uo\\te=1'])
        self.assertEqual(run.returncode, 2)
        self.assertIn('q"uo\\te', run.json()["error"]["message"])

    def test_locale_environment_variables_do_not_change_a_number(self) -> None:
        plain = run_exe([*sma_args("precision_edge.csv", "2", "3"), "--no-wall-clock"]).stdout_bytes
        for locale in ("de_DE.UTF-8", "fr_FR.UTF-8", "German_Germany.1252"):
            env = {"LC_ALL": locale, "LANG": locale, "LC_NUMERIC": locale}
            self.assertEqual(run_exe([*sma_args("precision_edge.csv", "2", "3"), "--no-wall-clock"], env=env).stdout_bytes, plain, locale)


class PrecisionContract(LabTestCase):
    def test_prices_are_read_and_written_exactly_at_the_edges_of_the_int64_range(self) -> None:
        run = run_exe(sma_args("precision_edge.csv", "2", "3"))
        events = run.json(exact=True)["result"]["events"]
        want = [Decimal("0.1"), Decimal("0.3"), Decimal("0.000001"), Decimal("123456789.123456"),
                Decimal("1.25"), Decimal("9223372036854.775807")]
        self.assertEqual([e["price"] for e in events], want)
        # The text carries exactly those digits (shortest form: trailing zeros dropped), so nothing passed through a double.
        for text in ('"price":0.1,', '"price":0.3,', '"price":0.000001,', '"price":123456789.123456,',
                     '"price":1.25,', '"price":9223372036854.775807,'):
            self.assertIn(text, run.stdout)
        # The crossover refuses the largest close (above its documented INT64_MAX / long_window^2 ceiling); the CSV
        # loader accepted it because it is exactly an int64.
        self.assertEqual(events[5]["results"][0]["reason"], "price_above_max_close")

    def test_every_number_in_a_document_is_standard_json(self) -> None:
        text = run_exe([*sma_args("precision_edge.csv", "2", "3"), "--no-wall-clock"]).stdout
        strict_loads(text)   # a real parser: a comma decimal point or a bare ".5" would not parse


class ScaleContract(LabTestCase):
    def test_a_twenty_thousand_row_two_symbol_run_is_valid_complete_and_deterministic(self) -> None:
        rows = 20000
        with labtest.temp_dir() as tmp:
            path = os.path.join(tmp, "big.csv")
            with open(path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("symbol,exchange_time,type,price\n")
                state = 12345
                cents = {"AAPL": 10000, "MSFT": 20000}   # whole cents: an integer walk, no floating point
                for i in range(rows // 2):
                    seconds = i
                    stamp = f"2026-01-05T{14 + seconds // 3600:02d}:{(seconds % 3600) // 60:02d}:{seconds % 60:02d}Z"
                    for symbol in ("AAPL", "MSFT"):
                        state = (state * 6364136223846793005 + 1442695040888963407) % (1 << 64)
                        cents[symbol] = max(100, cents[symbol] + (state >> 40) % 2001 - 1000)
                        handle.write(f"{symbol},{stamp},bar,{cents[symbol] // 100}.{cents[symbol] % 100:02d}\n")
            args = ["run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=AAPL,MSFT",
                    "--strategy", "mean_reversion", "--param", "symbols=AAPL,MSFT", "--no-wall-clock"]
            first = run_exe(args)
            self.assertEqual(first.returncode, 0, first.stderr)
            result = first.json()["result"]
            self.assertEqual(len(result["events"]), rows)
            self.assertGreater(len(result["signals"]), 100)
            self.assertEqual([s["signal_id"] for s in result["signals"]], list(range(1, len(result["signals"]) + 1)))
            self.assertEqual(result["engine"]["stats"]["events_routed"], rows)
            self.assertEqual(first.stdout_bytes, run_exe(args).stdout_bytes)
            capped = run_exe([*args, "--max-rows", "100"])
            self.assertEqual(capped.returncode, 3)


if __name__ == "__main__":
    unittest.main()
