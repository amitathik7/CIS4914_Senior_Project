"""The JSON serializers on documents real runs cannot produce, parsed by a real JSON parser.

The probe (tests/strategy_lab/json_probe.cpp) builds hostile content: NaN and infinite prices and
diagnostics, unavailable values, quotes, backslashes, control characters, line separators, emoji, the
extremes of the double range, and counters beyond 2^53. The checks below read every value back.
"""

from __future__ import annotations

import math
import unittest

import labtest
from labtest import LabTestCase, run_exe, strict_loads


def probe(*args: str, env: dict[str, str] | None = None) -> labtest.Run:
    return run_exe(list(args), env=env, exe=labtest.PROBE)


class HostileDocument(LabTestCase):
    needs_probe = True

    def setUp(self) -> None:
        run = probe("document")
        self.assertEqual(run.returncode, 0, run.stderr)
        self.run_ = run
        self.doc = run.json()          # strict: duplicate keys, NaN, Infinity, trailing text all fail here
        self.result = self.doc["result"]

    def test_it_is_one_valid_document_with_no_non_finite_literal_anywhere(self) -> None:
        self.assertEqual(self.doc["schema"], "strategy_lab.replay")
        for token in ("NaN", "Infinity", "nan,", ":inf", "inf,"):
            self.assertNotIn(token, self.run_.stdout.replace('"nan"', "").replace('"inf"', "").replace('"-inf"', ""), token)

        def walk(value: object) -> None:
            if isinstance(value, float):
                self.assertTrue(math.isfinite(value), value)
            elif isinstance(value, dict):
                for inner in value.values():
                    walk(inner)
            elif isinstance(value, list):
                for inner in value:
                    walk(inner)

        walk(self.doc)

    def test_hostile_strings_come_back_exactly(self) -> None:
        self.assertEqual(self.result["input"]["dataset"]["name"], 'n"a\\me\t\x01  \U0001F600.csv')
        self.assertEqual(self.result["input"]["dataset"]["symbols"][0]["symbol"], "A\\B")
        self.assertEqual(self.result["warnings"][0], {"code": 'w"code', "message": 'message with \\ and "quotes" and \n newline',
                                                       "strategy_id": 's"1'})
        signal = self.result["signals"][0]
        self.assertEqual(signal["metadata"], {'k"ey': "v\\al\nue \U0001F600", "ünï": "\x7f"})
        self.assertEqual(signal["strategy_id"], 's"1')
        self.assertEqual(self.result["engine"]["strategies"][0]["last_error"], 'error with "quotes" \x01')
        self.assertEqual(self.result["publication_failures"][0]["message"], 'refused "on purpose"')
        params = self.result["configuration"]["strategies"][0]["parameters"]
        self.assertEqual(params["symbols"], ["A\\B", "café"])
        self.assertEqual(params["strategy_id"], 's"1')

    def test_raw_control_characters_and_line_separators_never_appear_unescaped(self) -> None:
        body = self.run_.stdout[:-1]   # the one trailing newline is the document's terminator
        for char in ("\x00", "\x01", "\t", "\n", "\r", "\x7f", " ", " "):
            self.assertNotIn(char, body, repr(char))

    def test_numbers_at_the_edges_read_back_to_the_same_double(self) -> None:
        params = dict(self.result["configuration"]["strategies"][0]["parameters"])
        self.assertEqual(params["requested_quantity"], 1.7976931348623157e308)
        self.assertEqual(params["tiny"], 5e-324)
        self.assertEqual(params["third"], 1.0 / 3.0)
        derived = self.result["configuration"]["strategies"][0]["derived"]
        self.assertEqual(derived["ratio"], 0.1 + 0.2)
        self.assertIs(derived["flag"], True)
        first = self.result["events"][0]
        self.assertEqual(first["price"], 0.1)
        self.assertEqual(first["volume"], 1e-320)
        self.assertEqual(self.result["signals"][0]["requested_quantity"], 1.0 / 3.0)
        self.assertEqual(self.result["signals"][0]["confidence"], 0.1 + 0.2)

    def test_counters_beyond_two_to_the_fifty_third_are_exact_integers(self) -> None:
        stats = self.result["engine"]["stats"]
        self.assertEqual(stats["events_routed"], 18446744073709551615)
        self.assertEqual(stats["signals_published"], 9007199254740993)
        self.assertEqual(self.result["configuration"]["strategies"][0]["derived"]["count"], 18446744073709551615)
        self.assertEqual(self.result["input"]["dataset"]["bytes"], 123456789012)

    def test_non_finite_event_and_signal_fields_are_omitted_with_a_status(self) -> None:
        first = self.result["events"][0]
        for name, status in (("open", "nan"), ("high", "inf"), ("low", "-inf")):
            self.assertNotIn(name, first)
            self.assertEqual(first[f"{name}_status"], status)
        second = self.result["events"][1]
        self.assertNotIn("price", second)
        self.assertEqual(second["price_status"], "nan")
        third = self.result["events"][2]
        self.assertNotIn("price", third)
        self.assertNotIn("price_status", third, "an absent price is simply absent")
        signal = self.result["signals"][0]
        self.assertNotIn("target_exposure", signal)
        self.assertEqual(signal["target_exposure_status"], "nan")

    def test_unavailable_and_non_finite_diagnostic_values_are_listed_not_written(self) -> None:
        diag = self.result["events"][0]["results"][0]
        self.assertTrue(diag["diagnostics_available"])
        self.assertEqual(diag["indicators"], {"finite": 0.30000000000000004, "neg\"zero": 0})
        # Negative zero is written as "-0" (valid JSON; JavaScript reads it as -0). Python's parser turns that
        # into the integer 0, so the sign is checked in the raw text, where it is the tool's doing.
        self.assertIn('"neg\\"zero":-0', self.run_.stdout)
        self.assertEqual(diag["unavailable"], {"nan": "non_finite", "inf": "non_finite", "absent": "r\"x"})
        self.assertEqual(diag["states"], {'st"ate': "va\\lue"})

        ignored = self.result["events"][1]["results"][0]
        self.assertEqual(ignored["unavailable"], {"short_sma": "event_ignored"})
        self.assertEqual(ignored["indicators"], {})

    def test_an_event_with_no_diagnostic_says_so_instead_of_inventing_one(self) -> None:
        silent = self.result["events"][2]["results"][0]
        self.assertFalse(silent["diagnostics_available"])
        self.assertIn("diagnostics_unavailable_reason", silent)
        self.assertNotIn("verdict", silent)
        self.assertNotIn("indicators", silent)

    def test_the_result_hash_covers_the_result_bytes(self) -> None:
        self.assertEqual(self.doc["provenance"]["result_sha256"], labtest.sha256_text(labtest.result_substring(self.run_.stdout)))
        self.assertNotIn("generated_at", self.doc["provenance"], "the probe asks for no wall clock")

    def test_pretty_output_is_the_same_document(self) -> None:
        pretty = probe("document", "--pretty")
        self.assertEqual(pretty.returncode, 0)
        self.assertEqual(pretty.json(), self.doc)
        self.assertNotIn("\r", pretty.stdout)


class HostileLocale(LabTestCase):
    needs_probe = True

    def test_a_comma_decimal_global_locale_changes_not_one_byte(self) -> None:
        plain = probe("document")
        hostile = probe("locale")
        self.assertEqual(hostile.returncode, 0, hostile.stderr)
        self.assertEqual(hostile.stdout_bytes, plain.stdout_bytes, "number text must not depend on the process locale")
        strict_loads(hostile.stdout)
        # And with the environment hostile too (it is the C library, not the environment, that formats).
        env = {"LC_ALL": "de_DE.UTF-8", "LANG": "de_DE.UTF-8"}
        self.assertEqual(probe("locale", env=env).stdout_bytes, plain.stdout_bytes)


class ErrorDocument(LabTestCase):
    needs_probe = True

    def test_an_error_with_invalid_utf8_and_control_text_is_still_valid_json(self) -> None:
        run = probe("error")
        self.assertEqual(run.returncode, 0)
        error = run.json()["error"]
        self.assertEqual(error["code"], "dataset_error")
        self.assertEqual(error["exit_status"], 3)
        self.assertEqual(error["message"], 'bad byte ? and a tab\t and a quote " and é', "invalid bytes become '?', valid text is kept")
        self.assertEqual(error["total_problems"], 99)
        problem = error["problems"][0]
        self.assertEqual((problem["line"], problem["column"], problem["where"]), (7, "pri?ce", "strategies[0]"))
        self.assertEqual(problem["message"], "line\nbreak \x01 and ??")


if __name__ == "__main__":
    unittest.main()
