"""Exact numeric row order for the sortable tables (table_order), and the types of catalog / configuration parameters.

Why it exists: `st.dataframe` sorts a text column as text ('100' before '20' before '3') and turns a numeric column into a
floating-point number in the browser, so two prices that differ only in the last digit (9223372036854.775806 and ...807) display
as the same number and integers above 2**53 are mis-ordered. Prices, quantities and signal ids are therefore shown as exact text
and ordered by an explicit control that sorts in Python on the exact int / Decimal values. These tests pin that order, never
through a float, and pin which catalog parameters may be floats (ratios) and which must stay exact integers (quantities).
"""

import re
import unittest
from decimal import Decimal

from support import APP, dumps, ev, make_document, outcome_of, parse, real_runner

from streamlit.testing.v1 import AppTest

from strategy_lab_ui import compare_tables, table_order, tables
from strategy_lab_ui.catalog import parse_catalog
from strategy_lab_ui.compare import CompareModel, MemberConfig, MemberRun
from strategy_lab_ui.errors import LabUiError
from strategy_lab_ui.jsonio import strict_loads
from strategy_lab_ui.replay import ReplayModel
from strategy_lab_ui.schema import parse_replay
from strategy_lab_ui.table_order import RECORDED, RowOrder, choices, exact_order

ASC = "{}, low to high (exact)"
DESC = "{}, high to low (exact)"
BIG = 2**62                                  # BIG and BIG + 1 are different integers but the same float
HIGH, NEXT = "9223372036854.775807", "9223372036854.775806"     # INT64_MAX and one less, in millionths: the same float


class ExactOrder(unittest.TestCase):
    def test_numbers_sort_by_value_not_as_text(self):
        # As text 100 < 20 < 3; by value 3 < 20 < 100.
        values = [100, 3, 20]
        self.assertEqual(sorted(str(v) for v in values), ["100", "20", "3"])
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder()), [100, 3, 20],
                         "the recorded order is untouched")
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", False)), [3, 20, 100])
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", True)), [100, 20, 3])

    def test_ints_and_decimals_are_compared_exactly_with_each_other(self):
        values = [Decimal("20"), 3, Decimal("0.000001"), 100, Decimal("3.000001"), Decimal("99.999999")]
        want = [Decimal("0.000001"), 3, Decimal("3.000001"), Decimal("20"), Decimal("99.999999"), 100]
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", False)), want)
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", True)), want[::-1])

    def test_integers_above_2_53_that_a_float_cannot_tell_apart_stay_in_exact_order(self):
        self.assertEqual(float(BIG), float(BIG + 1), "the trap: as floats these are the same number")
        values = [BIG + 1, 7, BIG, 2**53 + 1, 2**53]
        ascending = exact_order(values, [{"x": v} for v in values], RowOrder("x", False))
        self.assertEqual(ascending, [7, 2**53, 2**53 + 1, BIG, BIG + 1])
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", True)), ascending[::-1])

    def test_decimals_that_differ_only_in_the_last_millionth_stay_in_exact_order(self):
        high, below = Decimal(HIGH), Decimal(NEXT)
        self.assertEqual(float(high), float(below), "the trap: as floats these are the same number")
        values = [high, Decimal("3"), below]
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", False)), [Decimal("3"), below, high])
        self.assertEqual(exact_order(values, [{"x": v} for v in values], RowOrder("x", True)), [high, below, Decimal("3")])

    def test_ties_keep_their_recorded_order_in_both_directions_and_missing_values_are_last(self):
        rows = ["a", "b", "c", "d", "e"]
        keys = [{"x": 5}, {"x": None}, {"x": 5}, {"x": 1}, {"x": None}]
        self.assertEqual(exact_order(rows, keys, RowOrder("x", False)), ["d", "a", "c", "b", "e"])
        self.assertEqual(exact_order(rows, keys, RowOrder("x", True)), ["a", "c", "d", "b", "e"])

    def test_the_choices_are_the_recorded_order_then_each_column_both_ways(self):
        labels = list(choices(("Signal", "Quantity")))
        self.assertEqual(labels, [RECORDED, ASC.format("Signal"), DESC.format("Signal"),
                                  ASC.format("Quantity"), DESC.format("Quantity")])
        self.assertEqual(choices(("Quantity",))[DESC.format("Quantity")], RowOrder("Quantity", True))
        self.assertEqual(choices(())[RECORDED], RowOrder())


def explore_model(*, ids, quantities, prices=None) -> ReplayModel:
    """One bar per signal: signal `ids[i]` with `quantities[i]` shares on event i. `prices` (text) replaces each close."""
    specs = [ev("AAPL", 30 + i, 5000 + i, signals=[(0, sid, "buy")]) for i, sid in enumerate(ids)]
    document = make_document(specs)
    for signal, quantity in zip(document["result"]["signals"], quantities):
        signal["requested_quantity"] = quantity
    raw = dumps(document).decode("utf-8")
    for i, text in enumerate(prices or ()):
        raw = re.sub(rf'"price":{5000 + i}(?=[,}}])', f'"price":{text}', raw, count=1)
    return ReplayModel(parse_replay(strict_loads(raw.encode("utf-8"))))


class ExploreTables(unittest.TestCase):
    IDS = [10, 9, 100, BIG + 1, BIG]
    QUANTITIES = [20, 3, 100, BIG, BIG + 1]

    def setUp(self):
        self.model = explore_model(ids=self.IDS, quantities=self.QUANTITIES)

    def column(self, column, order):
        return list(tables.signals_table(self.model, self.model.total, None, order)[column])

    def test_the_default_is_the_recorded_order_and_the_text_is_exact(self):
        self.assertEqual(self.column("Signal", RowOrder()), [str(i) for i in self.IDS])
        self.assertEqual(self.column("Quantity", RowOrder()), [str(q) for q in self.QUANTITIES])

    def test_signal_ids_sort_numerically_so_9_10_100_not_10_100_9(self):
        as_text = sorted(str(i) for i in self.IDS)
        self.assertEqual(as_text[:2], ["10", "100"], "a text sort would put 10 and 100 before 9")
        want = [9, 10, 100, BIG, BIG + 1]
        self.assertEqual(self.column("Signal", RowOrder("Signal", False)), [str(i) for i in want])
        self.assertEqual(self.column("Signal", RowOrder("Signal", True)), [str(i) for i in want[::-1]])

    def test_quantities_sort_numerically_including_two_that_differ_by_one_above_2_53(self):
        want = [3, 20, 100, BIG, BIG + 1]
        self.assertEqual(self.column("Quantity", RowOrder("Quantity", False)), [str(q) for q in want])
        self.assertEqual(self.column("Quantity", RowOrder("Quantity", True)), [str(q) for q in want[::-1]])

    def test_ordering_moves_whole_rows_so_columns_stay_aligned(self):
        frame = tables.signals_table(self.model, self.model.total, None, RowOrder("Quantity", False))
        pairs = list(zip(frame["Signal"], frame["Quantity"]))
        self.assertEqual(pairs, [(str(sid), str(q)) for sid, q in sorted(zip(self.IDS, self.QUANTITIES), key=lambda p: p[1])])

    def test_the_prefix_still_limits_the_rows_whatever_the_order(self):
        frame = tables.signals_table(self.model, 3, None, RowOrder("Signal", True))
        self.assertEqual(list(frame["Signal"]), ["100", "10", "9"])


class DiagnosticsOrder(unittest.TestCase):
    PRICES = ["20", "3", "100", HIGH, NEXT, "0.000001"]

    def setUp(self):
        specs = [ev("AAPL", 30 + i, 5000 + i) for i in range(len(self.PRICES))] + [ev("AAPL", 40, None)]
        document = make_document(specs)
        raw = dumps(document).decode("utf-8")
        for i, text in enumerate(self.PRICES):
            raw = re.sub(rf'"price":{5000 + i}(?=[,}}])', f'"price":{text}', raw, count=1)
        self.model = ReplayModel(parse_replay(strict_loads(raw.encode("utf-8"))))

    def closes(self, order):
        return list(tables.diagnostics_table(self.model, self.model.total, None, (), order)["Close"])

    def test_closes_are_exact_text_in_recorded_order_by_default(self):
        self.assertEqual(self.closes(RowOrder()), [*self.PRICES, ""])

    def test_prices_sort_numerically_and_the_two_that_a_float_merges_stay_apart(self):
        self.assertEqual(float(Decimal(HIGH)), float(Decimal(NEXT)))
        ascending = ["0.000001", "3", "20", "100", NEXT, HIGH]
        self.assertEqual(self.closes(RowOrder("Close", False)), [*ascending, ""], "no price is last")
        self.assertEqual(self.closes(RowOrder("Close", True)), [*ascending[::-1], ""], "no price is last either way")


def compare_run(member, ids, quantities, run_id):
    specs = [ev("AAPL", 30 + i, 10 + i, signals=[(0, sid, "buy")]) for i, sid in enumerate(ids)]
    document = make_document(specs, run_id=run_id)
    for signal, quantity in zip(document["result"]["signals"], quantities):
        signal["requested_quantity"] = quantity
    outcome = outcome_of(document)
    config = MemberConfig(member, "sma_crossover", (("short_window", "2"), ("long_window", "3"), ("symbols", "AAPL")),
                          f"{member}: sma", f"{member}: sma")
    return MemberRun(config, outcome, ReplayModel(outcome.document))


class CompareOrder(unittest.TestCase):
    def setUp(self):
        a = compare_run("A", [10, 9, BIG + 1], [20, 3, BIG + 1], "0123456789abcdef")
        b = compare_run("B", [100, BIG, 2], [BIG, 100, 7], "fedcba9876543210")
        self.model = CompareModel([a, b])
        self.labels = {"A": "A", "B": "B"}

    def column(self, column, order):
        return list(compare_tables.signals_table(self.model, self.labels, self.model.total, None, order)[column])

    def test_raw_signal_ids_of_both_runs_sort_together_numerically(self):
        want = [2, 9, 10, 100, BIG, BIG + 1]
        self.assertEqual(self.column("Signal id (raw)", RowOrder("Signal id (raw)", False)), [str(i) for i in want])
        self.assertEqual(self.column("Signal id (raw)", RowOrder("Signal id (raw)", True)), [str(i) for i in want[::-1]])

    def test_quantities_of_both_runs_sort_together_numerically(self):
        want = [3, 7, 20, 100, BIG, BIG + 1]
        self.assertEqual(self.column("Quantity", RowOrder("Quantity", False)), [str(q) for q in want])

    def test_the_diagnostics_close_sorts_numerically(self):
        frame = compare_tables.diagnostics_table(self.model, self.model.total, None, RowOrder("Close", True))
        self.assertEqual(list(frame["Close"]), ["12", "11", "10"])


class Catalog(unittest.TestCase):
    """A catalog default keeps the type the catalog declares; only a "double" may be a float."""

    @staticmethod
    def document(params):
        return {"schema": "strategy_lab.catalog", "schema_version": "1.0", "catalog": {
            "tool": "strategy_lab_replay", "project_version": "0.0.1",
            "limits": {"default_max_rows": 50000, "hard_max_rows": 1000000, "max_strategies": 16},
            "dataset_format": {"id": "strategy_lab_bars_csv/1", "row_types": ["bar", "trade"], "columns": []},
            "bus_faults": [{"name": "none", "description": ""}],
            "strategies": [{"kind": "k", "title": "K", "summary": "s", "docs": "d", "available": True,
                            "parameters": params, "signal_metadata_keys": [], "decision_reasons": [],
                            "indicators": [], "states": []}]}}

    @staticmethod
    def entry(name, kind, default=None, **extra):
        out = {"name": name, "type": kind, "required": default is None, "constraint": "c"}
        if default is not None:
            out["default"] = default
        out.update(extra)
        return out

    def parse(self, params):
        # through the runner's strict reader, so a decimal point really arrives as a Decimal
        text = dumps(self.document(params)).decode("utf-8")
        return parse_catalog(strict_loads(text.encode("utf-8"))).strategies[0]

    def test_a_quantity_and_a_window_default_stay_exact_integers_and_a_ratio_becomes_a_float(self):
        spec = self.parse([self.entry("requested_quantity", "uint", 25), self.entry("long_window", "uint", 20),
                           self.entry("rearm_threshold", "double", 0.5), self.entry("entry_threshold", "double", 2),
                           self.entry("strategy_id", "string", "x"), self.entry("symbols", "string_list")])
        self.assertEqual(spec.param("requested_quantity").default, 25)
        self.assertIs(type(spec.param("requested_quantity").default), int)
        self.assertIs(type(spec.param("long_window").default), int)
        rearm = spec.param("rearm_threshold").default
        self.assertEqual(rearm, 0.5)
        self.assertIs(type(rearm), float, "a ratio is a float, not a Decimal")
        self.assertEqual(spec.param("entry_threshold").default, 2)       # a whole-number double stays what the tool wrote
        self.assertEqual(spec.param("strategy_id").default, "x")
        self.assertFalse(spec.param("symbols").has_default)

    def test_an_integer_above_2_53_survives_as_a_quantity_default(self):
        spec = self.parse([self.entry("requested_quantity", "uint", BIG + 1)])
        self.assertEqual(spec.param("requested_quantity").default, BIG + 1)

    def test_a_quantity_or_window_default_that_is_not_an_integer_is_refused_never_converted(self):
        for params in ([self.entry("requested_quantity", "uint", 1.5)], [self.entry("long_window", "uint", 20.0)]):
            text = dumps(self.document(params)).decode("utf-8")
            with self.assertRaises(LabUiError) as caught:
                parse_catalog(strict_loads(text.encode("utf-8")))
            self.assertEqual(caught.exception.kind, "invalid_structure")
            self.assertIn("integer", str(caught.exception))

    def test_a_default_of_the_wrong_type_is_refused(self):
        for entry in (self.entry("a", "double", "0.5"), self.entry("b", "string", 3), self.entry("c", "string_list", "AAPL")):
            text = dumps(self.document([entry])).decode("utf-8")
            with self.assertRaises(LabUiError):
                parse_catalog(strict_loads(text.encode("utf-8")))


class ConfigurationParameters(unittest.TestCase):
    """The parameters a result echoes: a quantity stays an exact integer, ratios are floats."""

    def parse(self, parameters):
        document = make_document([ev("A", 30, 5)], parameters=parameters)
        return parse_replay(strict_loads(dumps(document)))

    def test_ratios_become_floats_and_integers_stay_integers(self):
        doc = self.parse({"short_window": 2, "requested_quantity": BIG + 1, "entry_threshold": 1.5, "rearm_threshold": 0.5})
        params = doc.strategies[0].parameters
        self.assertEqual(params["requested_quantity"], BIG + 1)
        self.assertIs(type(params["requested_quantity"]), int)
        self.assertIs(type(params["short_window"]), int)
        self.assertIs(type(params["entry_threshold"]), float)
        self.assertEqual((params["entry_threshold"], params["rearm_threshold"]), (1.5, 0.5))

    def test_a_requested_quantity_that_is_not_an_integer_is_refused(self):
        for bad in (1.5, 2.0, 0):
            with self.assertRaises(LabUiError, msg=str(bad)):
                self.parse({"requested_quantity": bad})


class ExploreControl(unittest.TestCase):
    """The control, in the real app against the real executable: choosing an order reorders the table's rows."""

    @classmethod
    def setUpClass(cls):
        cls.runner = real_runner()

    def setUp(self):
        self.at = AppTest.from_file(APP, default_timeout=90).run()
        self.at.button(key="ui_run").click().run()
        while not self.at.button(key="nav_signal").disabled:           # reveal every signal of the demo run (ids 1 and 2)
            self.at.button(key="nav_signal").click().run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])

    def frame(self, column):
        return next(element.value for element in self.at.dataframe if column in element.value.columns)

    def test_signals_follow_the_chosen_order_and_default_to_the_recorded_one(self):
        self.assertEqual(self.at.selectbox(key="ui_order_signals").value, RECORDED)
        self.assertEqual(list(self.frame("Signal")["Signal"]), ["1", "2"])
        self.at.selectbox(key="ui_order_signals").set_value(DESC.format("Signal")).run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])
        self.assertEqual(list(self.frame("Signal")["Signal"]), ["2", "1"])
        self.at.selectbox(key="ui_order_signals").set_value(ASC.format("Quantity")).run()
        self.assertEqual(list(self.frame("Signal")["Signal"]), ["1", "2"], "equal quantities keep their recorded order")

    def test_the_diagnostics_close_follows_the_chosen_order(self):
        recorded = [t for t in self.frame("Close")["Close"]]
        self.assertEqual(recorded[:3], ["3", "2", "1"])               # the fixture's closes 3 2 1 2 3 4 3 2 1
        self.at.selectbox(key="ui_order_diagnostics").set_value(DESC.format("Close")).run()
        self.assertFalse(self.at.exception, [e.value for e in self.at.exception])
        shown = list(self.frame("Close")["Close"])
        self.assertEqual(shown, sorted(recorded, key=Decimal, reverse=True))
        self.assertEqual(shown[0], "4")

    def test_the_control_says_why_a_header_click_is_not_the_exact_order(self):
        control = self.at.selectbox(key="ui_order_signals")
        self.assertIn("TEXT", control.proto.help)
        self.assertIn("2^53", control.proto.help)
        self.assertEqual(table_order.HELP, control.proto.help)


if __name__ == "__main__":
    unittest.main()
