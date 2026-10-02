"""Strict JSON reading and schema validation: malformed, unsupported and inconsistent output is refused with a
useful message; ids and timestamps survive exactly."""

import copy
import json
import unittest

from support import ev, make_document, parse

from strategy_lab_ui.errors import LabUiError
from strategy_lab_ui.jsonio import strict_loads


class StrictJson(unittest.TestCase):
    def refused(self, data: bytes) -> LabUiError:
        with self.assertRaises(LabUiError) as caught:
            strict_loads(data)
        self.assertEqual(caught.exception.kind, "malformed_output")
        return caught.exception

    def test_accepts_one_document_and_a_newline(self):
        self.assertEqual(strict_loads(b'{"a":1}\n'), {"a": 1})

    def test_refuses_empty_output(self):
        self.refused(b"")

    def test_refuses_invalid_utf8(self):
        self.refused(b'{"a":"\xff"}\n')

    def test_refuses_duplicate_keys(self):
        self.assertIn("duplicate", self.refused(b'{"a":1,"a":2}\n').detail)

    def test_refuses_nan_and_infinity(self):
        for literal in (b"NaN", b"Infinity", b"-Infinity"):
            self.refused(b'{"a":' + literal + b"}\n")

    def test_refuses_trailing_data_or_a_missing_newline(self):
        self.refused(b'{"a":1}\n{"b":2}\n')
        self.refused(b'{"a":1}')
        self.refused(b'{"a":1}\n\n')

    def test_refuses_garbage(self):
        self.refused(b"not json\n")

    def test_integers_beyond_2_53_stay_exact(self):
        value = strict_loads(b'{"id":9007199254740993,"big":9223372036854775807,"huge":18446744073709551615}\n')
        self.assertEqual((value["id"], value["big"], value["huge"]), (2**53 + 1, 2**63 - 1, 2**64 - 1))
        self.assertTrue(all(isinstance(v, int) for v in value.values()))


class ReplayDocument(unittest.TestCase):
    def base(self):
        return make_document([ev("AAPL", 30), ev("AAPL", 31, signals=[(0, 1, "buy")]), ev("AAPL", 32)])

    def refused(self, document, kind: str) -> LabUiError:
        with self.assertRaises(LabUiError) as caught:
            parse(document)
        self.assertEqual(caught.exception.kind, kind, caught.exception.message)
        return caught.exception

    def test_a_good_document_parses_and_links_signals_to_events(self):
        doc = parse(self.base())
        self.assertEqual(len(doc.events), 3)
        self.assertEqual([s.signal_id for s in doc.signals], [1])
        self.assertEqual(doc.signals_by_event[1][0].event_index, 1)
        self.assertEqual(doc.events[1].results[0].signal_ids, (1,))
        self.assertEqual(doc.events[0].exchange_time, "2026-01-05T14:30:00.000000000Z")

    def test_unsupported_major_version(self):
        error = self.refused(make_document([ev("A", 30)], schema_version="2.0"), "unsupported_schema")
        self.assertIn("2.0", error.message)

    def test_a_newer_minor_version_is_accepted(self):
        self.assertEqual(parse(make_document([ev("A", 30)], schema_version="1.7")).schema_version, "1.7")

    def test_unknown_members_are_ignored(self):
        document = self.base()
        document["result"]["future_member"] = {"anything": True}
        document["result"]["events"][0]["extra"] = 5
        parse(document)

    def test_wrong_schema_name_and_unparsable_version(self):
        document = self.base()
        document["schema"] = "strategy_lab.catalog"
        self.refused(document, "malformed_output")
        document = self.base()
        document["schema_version"] = "one"
        self.refused(document, "unsupported_schema")

    def test_missing_member_names_its_path(self):
        document = self.base()
        del document["result"]["events"][1]["symbol"]
        error = self.refused(document, "invalid_structure")
        self.assertIn("$.result.events[1]", error.message)
        self.assertIn("symbol", error.message)

    def test_wrong_type_names_its_path(self):
        document = self.base()
        document["result"]["events"][0]["bus_sequence"] = "3"
        self.assertIn("bus_sequence", self.refused(document, "invalid_structure").message)

    def test_booleans_are_not_numbers(self):
        document = self.base()
        document["result"]["events"][0]["price"] = True
        self.refused(document, "invalid_structure")

    def test_event_indexes_must_be_consecutive(self):
        document = self.base()
        document["result"]["events"][2]["index"] = 5
        self.refused(document, "invalid_structure")

    def test_a_signal_must_name_a_real_event(self):
        document = self.base()
        document["result"]["signals"][0]["event_index"] = 99
        self.refused(document, "invalid_structure")

    def test_a_signal_must_match_its_events_symbol(self):
        document = self.base()
        document["result"]["signals"][0]["symbol"] = "MSFT"
        self.assertIn("symbol", self.refused(document, "invalid_structure").message)

    def test_an_event_must_list_the_signal_that_points_at_it(self):
        document = self.base()
        document["result"]["events"][1]["results"][0]["signal_ids"] = []
        self.refused(document, "invalid_structure")

    def test_events_that_swap_their_signal_ids_are_refused(self):
        # Counts agree and every listed id exists; only the per-signal link (event_index -> its event) is wrong.
        document = make_document([ev("A", 30, signals=[(0, 1, "buy")]), ev("A", 31, signals=[(0, 2, "sell")])])
        events = document["result"]["events"]
        events[0]["results"][0]["signal_ids"], events[1]["results"][0]["signal_ids"] = [2], [1]
        self.assertIn("does not list signal", self.refused(document, "invalid_structure").message)

    def test_an_event_may_not_list_a_signal_that_does_not_exist(self):
        document = self.base()
        document["result"]["events"][2]["results"][0]["signal_ids"] = [7]
        self.refused(document, "invalid_structure")

    def test_signal_ref_must_be_run_scoped(self):
        document = self.base()
        document["result"]["signals"][0]["signal_ref"] = "other:1"
        self.assertIn("signal_ref", self.refused(document, "invalid_structure").message)

    def test_summary_counts_must_agree(self):
        document = self.base()
        document["result"]["summary"]["signals"] = 4
        self.refused(document, "invalid_structure")

    def test_dataset_rows_must_equal_events(self):
        document = self.base()
        document["result"]["input"]["dataset"]["rows"] = 9
        self.refused(document, "invalid_structure")

    def test_results_must_be_one_per_strategy(self):
        document = self.base()
        document["result"]["events"][0]["results"].append(copy.deepcopy(document["result"]["events"][0]["results"][0]))
        self.refused(document, "invalid_structure")

    def test_nothing_is_displayed_from_a_non_replay_document(self):
        with self.assertRaises(LabUiError):
            parse({"schema": "strategy_lab.error", "schema_version": "1.0"})


class IdsAndTimes(unittest.TestCase):
    def test_huge_signal_ids_and_sequences_survive_the_round_trip(self):
        sid = 2**63 - 1
        document = make_document([ev("AAPL", 30), ev("AAPL", 31, signals=[(0, sid, "sell")])],
                                 run_id="feedfacefeedface")
        document["result"]["events"][1]["bus_sequence"] = 2**53 + 1
        doc = parse(document)
        signal = doc.signals[0]
        self.assertEqual((signal.signal_id, signal.signal_ref), (sid, f"feedfacefeedface:{sid}"))
        self.assertIsInstance(signal.signal_id, int)
        self.assertEqual(doc.events[1].bus_sequence, 2**53 + 1)
        self.assertEqual(doc.events[1].results[0].signal_ids, (sid,))

    def test_distinct_ids_one_apart_beyond_2_53_stay_distinct(self):
        a, b = 2**53, 2**53 + 1
        self.assertEqual(float(a), float(b))        # the trap: as floats these are the same number
        document = make_document([ev("A", 30, signals=[(0, a, "buy")]), ev("A", 31, signals=[(0, b, "sell")])])
        doc = parse(document)
        self.assertEqual([s.signal_id for s in doc.signals], [a, b])
        self.assertEqual(doc.signals_by_event[0][0].signal_id, a)
        self.assertEqual(doc.signals_by_event[1][0].signal_id, b)

    def test_nanosecond_timestamps_are_kept_as_text(self):
        precise = "2026-01-05T14:30:00.123456789Z"
        doc = parse(make_document([ev("A", 30, time=precise), ev("A", 31, time="2026-01-05T14:30:00.123456790Z")]))
        self.assertEqual([e.exchange_time for e in doc.events], [precise, "2026-01-05T14:30:00.123456790Z"])

    def test_equal_timestamps_across_symbols_keep_file_order(self):
        same = "2026-01-05T14:30:00.000000000Z"
        doc = parse(make_document([ev("MSFT", 30, time=same, signals=[(0, 1, "buy")]),
                                   ev("AAPL", 30, time=same, signals=[(0, 2, "sell")])]))
        self.assertEqual([e.symbol for e in doc.events], ["MSFT", "AAPL"])
        self.assertEqual(doc.signals_by_event[0][0].symbol, "MSFT")
        self.assertEqual(doc.signals_by_event[1][0].symbol, "AAPL")

    def test_non_finite_values_arrive_as_a_status_not_a_number(self):
        document = make_document([ev("A", 30, price=None)])
        document["result"]["events"][0]["price_status"] = "nan"
        event = parse(document).events[0]
        self.assertIsNone(event.price)
        self.assertEqual(event.price_status, "nan")


class ErrorDocument(unittest.TestCase):
    def test_error_document_parses_with_problem_locations(self):
        from strategy_lab_ui.catalog import parse_error_document
        text = json.dumps({"schema": "strategy_lab.error", "schema_version": "1.0", "error": {
            "code": "dataset_error", "exit_status": 3, "message": "bad", "total_problems": 60,
            "problems": [{"line": 3, "column": "price", "message": "no"}, {"where": "strategies[0]", "message": "x"}]}})
        error = parse_error_document(json.loads(text))
        self.assertEqual((error.exit_status, error.total_problems), (3, 60))
        self.assertEqual((error.problems[0].line, error.problems[0].column), (3, "price"))
        self.assertEqual(error.problems[1].where, "strategies[0]")


if __name__ == "__main__":
    unittest.main()
